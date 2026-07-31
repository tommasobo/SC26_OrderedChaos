// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "loggers.h"

#include <iomanip>
#include <iostream>

// LoggedManager is a way to keep track of all the Logged instances
// that have been created so we can dump a map of IDs to Names to help
// interpret the IDs in the logfiles.

LoggedManager::LoggedManager(){};

void LoggedManager::add_logged(Logged* logged) {
    _idmap.push_back(logged);
}

void LoggedManager::dump_idmap() {
    std::ofstream fout("idmap.txt");
    for (size_t i = 0; i < _idmap.size(); i++) {
        fout << _idmap[i]->get_id() << " " << _idmap[i]->_name << endl;
    }
    fout.close();
}

LoggedManager Logged::_logged_manager;

string Logger::event_to_str(RawLogEvent& event) {
    return event.str();
}

QueueLoggerFactory::QueueLoggerFactory(Logfile* lg, QueueLoggerType logtype, EventList& eventlist)
    : _logfile(lg), _logger_type(logtype), _eventlist(eventlist){};

QueueLogger* QueueLoggerFactory::createQueueLogger() {
    QueueLogger* queue_logger = 0;
    switch (_logger_type) {
        case LOGGER_SIMPLE:
            queue_logger = new QueueLoggerSimple();
            _logfile->addLogger(*queue_logger);
            break;
        case LOGGER_SAMPLING:
            queue_logger = new QueueLoggerSampling(_sample_period, _eventlist);
            _logfile->addLogger(*queue_logger);
            break;
        case MULTIQUEUE_SAMPLING:
            abort();  // we can't do this - don't know the ID
            break;
        case LOGGER_EMPTY:
            queue_logger = new QueueLoggerEmpty(_sample_period, _eventlist);
            _logfile->addLogger(*queue_logger);
            break;
    }
    assert(queue_logger);
    _loggers.push_back(queue_logger);
    return queue_logger;
}

void QueueLoggerSimple::logQueue(BaseQueue& queue, QueueLogger::QueueEvent ev, Packet& pkt) {
    _logfile->writeRecord(Logger::QUEUE_EVENT,
                          queue.get_id(),
                          ev,
                          (double)queue.queuesize(),
                          pkt.flow().get_id(),
                          pkt.id());
}

string QueueLoggerSimple::event_to_str(RawLogEvent& event) {
    stringstream ss;
    ss << fixed << setprecision(9) << event._time;
    assert(event._type == Logger::QUEUE_EVENT);
    ss << " ID " << event._id;
    switch (event._ev) {
        case QueueLogger::PKT_ENQUEUE:
            ss << " Ev ENQUEUE";
            break;
        case QueueLogger::PKT_DROP:
            ss << " Ev DROP";
            break;
        case QueueLogger::PKT_SERVICE:
            ss << " Ev SERVICE";
            break;
        case QueueLogger::PKT_TRIM:
            ss << " Ev TRIM";
            break;
        case QueueLogger::PKT_BOUNCE:
            ss << " Ev BOUNCE";
            break;
    }
    ss << " Qsize " << (uint64_t)event._val1 << " FlowID " << (uint64_t)event._val2 << " PktID "
       << (uint64_t)event._val3;
    return ss.str();
}

QueueLoggerEmpty::QueueLoggerEmpty(simtime_picosec period, EventList& eventlist)
    : EventSource(eventlist, "QueuelogEmpty"),
      _last_transition(0),
      _total_busy(0),
      _period(period),
      _last_dump(0),
      _busy(false),
      _queue(0),
      _pkt_arrivals(0),
      _pkt_trims(0) {
    eventlist.sourceIsPendingRel(*this, 0);
};

// log the fraction of time the link is busy/empty
void QueueLoggerEmpty::logQueue(BaseQueue& queue, QueueLogger::QueueEvent ev, Packet& pkt) {
    if (!_queue) {
        _queue = &queue;
    }
    switch (ev) {
        case PKT_ARRIVE:
            // it arrived, don't know its outcome yet
            _pkt_arrivals++;
            break;
        case PKT_ENQUEUE:
            if (_busy == false) {
                // queue transitioned from empty to non-empty
                _last_transition = eventlist().now();
                _busy            = true;
            }
            break;
        case PKT_DROP:
            break;
        case PKT_TRIM:
            _pkt_trims++;
            break;
        case PKT_BOUNCE:
            break;
        case PKT_UNQUEUE:
        case PKT_SERVICE:
            if (_queue->queuesize() == 0) {
                // queue transitioned from non-empty to empty
                assert(_busy);
                _total_busy += eventlist().now() - _last_transition;
                _busy            = false;
                _last_transition = eventlist().now();
            }
            break;
    }
}

void QueueLoggerEmpty::doNextEvent() {
    eventlist().sourceIsPendingRel(*this, _period);
    if (_busy) {
        _total_busy += eventlist().now() - _last_transition;
    }
    if (_queue) {
        double trim_frac = 0;
        if (_pkt_arrivals > 0) {
            trim_frac = ((double)_pkt_trims) / _pkt_arrivals;
        }
        cout << eventlist().now() << " " << _queue->nodename() << " " << _total_busy << " "
             << eventlist().now() - _last_dump << " "
             << ((double)_total_busy) / (eventlist().now() - _last_dump) << " " << trim_frac
             << endl;
    }
    reset_count();
    _last_dump = eventlist().now();
}

void QueueLoggerEmpty::reset_count() {
    _last_transition = eventlist().now();
    _total_busy      = 0;
    _pkt_trims       = 0;
    _pkt_arrivals    = 0;
    if (_queue) {
        _busy = (_queue->queuesize() > 0);
    } else {
        // we've not seen any packets yet, so don't know the queue,
        // but it must be empty
        _busy = false;
    }
}

string QueueLoggerEmpty::event_to_str(RawLogEvent& event) {
    stringstream ss;
    ss << "QueueLoggerEmpty::event_to_str TBD\n";
    return ss.str();
}

QueueLoggerSampling::QueueLoggerSampling(simtime_picosec period, EventList& eventlist)
    : EventSource(eventlist, "QueuelogSampling"),
      _queue(NULL),
      _lastlook(0),
      _period(period),
      _lastq(0),
      _seenQueueInD(false),
      _cumidle(0),
      _cumarr(0),
      _cumdrop(0) {
    eventlist.sourceIsPendingRel(*this, 0);
}

void QueueLoggerSampling::doNextEvent() {
    eventlist().sourceIsPendingRel(*this, _period);
    if (_queue == NULL)
        return;
    mem_b queuebuff = _queue->maxsize();
    if (!_seenQueueInD) {  // queue size hasn't changed in the past D time units
        _logfile->writeRecord(QUEUE_APPROX,
                              _queue->get_id(),
                              QUEUE_RANGE,
                              (double)_lastq,
                              (double)_lastq,
                              (double)_lastq);
        _logfile->writeRecord(
            QUEUE_APPROX, _queue->get_id(), QUEUE_OVERFLOW, 0, 0, (double)queuebuff);
    } else {  // queue size has changed
        _logfile->writeRecord(QUEUE_APPROX,
                              _queue->get_id(),
                              QUEUE_RANGE,
                              (double)_lastq,
                              (double)_minQueueInD,
                              (double)_maxQueueInD);
        _logfile->writeRecord(QUEUE_APPROX,
                              _queue->get_id(),
                              QUEUE_OVERFLOW,
                              -(double)_lastIdledInD,
                              (double)_lastDroppedInD,
                              (double)queuebuff);
    }
    _seenQueueInD         = false;
    simtime_picosec now   = eventlist().now();
    simtime_picosec dt_ps = now - _lastlook;
    _lastlook             = now;
    // if the queue is empty, we've just been idling
    if ((_queue != NULL) && (_queue->queuesize() == 0))
        _cumidle += timeAsSec(dt_ps);
    _logfile->writeRecord(QUEUE_RECORD, _queue->get_id(), CUM_TRAFFIC, _cumarr, _cumidle, _cumdrop);
}

void QueueLoggerSampling::logQueue(BaseQueue& queue, QueueEvent ev, Packet& pkt) {
    if (_queue == NULL)
        _queue = &queue;
    assert(&queue == _queue);
    _lastq = queue.queuesize();

    if (!_seenQueueInD) {
        _seenQueueInD   = true;
        _minQueueInD    = queue.queuesize();
        _maxQueueInD    = _minQueueInD;
        _lastDroppedInD = 0;
        _lastIdledInD   = 0;
        _numIdledInD    = 0;
        _numDropsInD    = 0;
    } else {
        _minQueueInD = min(_minQueueInD, queue.queuesize());
        _maxQueueInD = max(_maxQueueInD, queue.queuesize());
    }
    simtime_picosec now   = eventlist().now();
    simtime_picosec dt_ps = now - _lastlook;
    double          dt    = timeAsSec(dt_ps);
    _lastlook             = now;
    switch (ev) {
        case PKT_SERVICE:  // we've just been working
            break;
        case PKT_ENQUEUE:
            _cumarr += timeAsSec(queue.drainTime(&pkt));
            if (queue.queuesize() > pkt.size())  // we've just been working
            {
            } else {  // we've just been idling
                mem_b idledwork = queue.serviceCapacity(dt_ps);
                _cumidle += dt;
                _lastIdledInD = idledwork;
                _numIdledInD++;
            }
            break;
        case PKT_DROP:  // assume we've just been working
        {
            assert(queue.queuesize() >= pkt.size());  // it is possible to
            // drop when queue is
            // idling, but this
            // logger can't make
            // sense of it
            double localdroptime = timeAsSec(queue.drainTime(&pkt));
            _cumarr += localdroptime;
            _cumdrop += localdroptime;
            _lastDroppedInD = pkt.size();
            _numDropsInD++;
            break;
        }
        case PKT_TRIM:
        case PKT_BOUNCE:
        case PKT_UNQUEUE:
        case PKT_ARRIVE:
            /* we don't currently do anything with this */
            break;
    }
}

string QueueLoggerSampling::event_to_str(RawLogEvent& event) {
    stringstream ss;
    ss << fixed << setprecision(9) << event._time;
    switch (event._type) {
        case Logger::QUEUE_APPROX:
            ss << " Type QUEUE_APPROX";
            ss << " ID " << event._id;
            switch (event._ev) {
                case QUEUE_RANGE:
                    ss << " Ev RANGE LastQ " << (int)event._val1 << " MinQ " << (int)event._val2
                       << " MaxQ " << (int)event._val3;
                    if (event._name != "")
                        ss << " Name " << event._name;
                    break;
                case QUEUE_OVERFLOW:
                    ss << " Ev OVERLOW LastIdled " << (int)event._val1 << " LastDropped "
                       << (int)event._val2 << " QueueBuf " << (int)event._val3;
                    if (event._name != "")
                        ss << " Name " << event._name;
                    break;
                default:
                    ss << " Unknown Event " << event._ev;
            }
            break;
        case Logger::QUEUE_RECORD:
            ss << " Type QUEUE_APPROX";
            ss << " ID " << event._id;
            assert(event._ev == QueueLogger::CUM_TRAFFIC);
            ss << " Ev CUM_TRAFFIC CumArr " << (int)event._val1 << " CumIdle " << (int)event._val2
               << " CumDrop " << (int)event._val3;
            if (event._name != "")
                ss << " Name " << event._name;
            break;
        default:
            ss << "Unknown record type: " << event._type;
    }
    return ss.str();
}

MultiQueueLoggerSampling::MultiQueueLoggerSampling(id_t            id,
                                                   simtime_picosec period,
                                                   EventList&      eventlist)
    : EventSource(eventlist, "MultiQueuelogSampling"),
      _id(id),
      _period(period),
      _seenQueueInD(false),
      _currentQueueSizeBytes(0),
      _currentQueueSizePkts(0) {
    eventlist.sourceIsPendingRel(*this, 0);
}

void MultiQueueLoggerSampling::doNextEvent() {
    eventlist().sourceIsPendingRel(*this, _period);
    if (!_seenQueueInD) {  // queue size hasn't changed in the past D time units
        _logfile->writeRecord(QUEUE_APPROX,
                              _id,
                              QUEUE_RANGE,
                              (double)_currentQueueSizeBytes,
                              (double)_currentQueueSizeBytes,
                              (double)_currentQueueSizeBytes);
    } else {  // queue size has changed
        _logfile->writeRecord(QUEUE_APPROX,
                              _id,
                              QUEUE_RANGE,
                              (double)_currentQueueSizeBytes,
                              (double)_minQueueInD,
                              (double)_maxQueueInD);
    }
    _seenQueueInD = false;
}

void MultiQueueLoggerSampling::logQueue(BaseQueue& queue, QueueEvent ev, Packet& pkt) {
    switch (ev) {
        case PKT_ENQUEUE:
            _currentQueueSizeBytes += pkt.size();
            _currentQueueSizePkts++;
            // cout << get_id() << " EN size " << pkt.size() << " Queue " << queue.nodename() << "
            // total " << _currentQueueSizeBytes << " qs " << queue.queuesize() << " id " <<
            // queue.get_id() << endl;

            assert(queue.queuesize() <= _currentQueueSizeBytes);
            break;
        case PKT_TRIM:
            //_currentQueueSizeBytes += pkt.si
            //_currentQueueSizePkts ++;
            // cout << get_id() << " TR size " << pkt.size() << endl;
            break;
        case PKT_SERVICE:
            _currentQueueSizeBytes -= pkt.size();
            _currentQueueSizePkts--;
            // cout << get_id() << " SE size " << -pkt.size() << endl;
            // cout << get_id() << " SE size " << pkt.size() << " Queue " << queue.nodename() << "
            // total " << _currentQueueSizeBytes << " qs " << queue.queuesize() << " id " <<
            // queue.get_id() << endl;

            break;
        case PKT_UNQUEUE:
            _currentQueueSizeBytes -= pkt.size();
            _currentQueueSizePkts--;
            // cout << get_id() << " UN size " << -pkt.size() << endl;
            break;
        case PKT_DROP:
        case PKT_BOUNCE:
        case PKT_ARRIVE:
            // doesn't change queue size
            break;
    }
    if (!_seenQueueInD) {
        _seenQueueInD = true;
        _minQueueInD  = _currentQueueSizeBytes;
        _maxQueueInD  = _minQueueInD;
    } else {
        _minQueueInD = min(_minQueueInD, _currentQueueSizeBytes);
        _maxQueueInD = max(_maxQueueInD, _currentQueueSizeBytes);
    }
    assert(_currentQueueSizePkts >= 0);
    assert(_currentQueueSizeBytes >= 0);
}

string MultiQueueLoggerSampling::event_to_str(RawLogEvent& event) {
    stringstream ss;
    ss << fixed << setprecision(9) << event._time;
    switch (event._type) {
        case Logger::QUEUE_APPROX:
            ss << " Type QUEUE_APPROX";
            ss << " ID " << event._id;
            switch (event._ev) {
                case QUEUE_RANGE:
                    ss << " Ev RANGE LastQ " << (int)event._val1 << " MinQ " << (int)event._val2
                       << " MaxQ " << (int)event._val3;

                    if (event._name != "")
                        ss << " Name " << event._name;
                    break;
                default:
                    ss << " Unknown Event " << event._ev;
            }
            break;
        default:
            ss << "Unknown record type: " << event._type;
    }
    return ss.str();
}

void FlowEventLoggerSimple::logEvent(
    PacketFlow& flow, Logged& location, FlowEvent ev, mem_b bytes, uint64_t pkts) {
    _logfile->writeRecord(Logger::FLOW_EVENT, location.get_id(), ev, flow.get_id(), bytes, pkts);
}

string FlowEventLoggerSimple::event_to_str(RawLogEvent& event) {
    stringstream ss;
    ss << fixed << setprecision(9) << event._time;
    assert(event._type == Logger::FLOW_EVENT);
    ss << " Type FLOW_EVENT SrcID " << event._id;
    switch ((FlowEventLogger::FlowEvent)event._ev) {
        case START:
            ss << " Ev START ";
            ss << " FlowID " << (uint64_t)event._val1;
            ss << " Flowsize " << (uint64_t)event._val2;
            break;
        case FINISH:
            ss << " Ev FINISH";
            ss << " FlowID " << (uint64_t)event._val1;
            ss << " Bytes " << (uint64_t)event._val2;
            ss << " Pkts " << (uint64_t)event._val3;
            break;
    }
    return ss.str();
}

void TrafficLoggerSimple::logTraffic(Packet& pkt, Logged& location, TrafficEvent ev) {
    _logfile->writeRecord(
        Logger::TRAFFIC_EVENT, location.get_id(), ev, pkt.flow().get_id(), pkt.id(), 0);
}

string TrafficLoggerSimple::event_to_str(RawLogEvent& event) {
    stringstream ss;
    ss << fixed << setprecision(9) << event._time;
    assert(event._type == Logger::TRAFFIC_EVENT);
    ss << " Type TRAFFIC ID " << event._id;
    switch ((TrafficLogger::TrafficEvent)event._ev) {
        case PKT_ARRIVE:
            ss << " Ev ARRIVE ";
            break;
        case PKT_DEPART:
            ss << " Ev DEPART ";
            break;
        case PKT_CREATESEND:
            ss << " Ev CREATESEND ";
            break;
        case PKT_CREATE:
            ss << " Ev CREATE ";
            break;
        case PKT_SEND:
            ss << " Ev SEND ";
            break;
        case PKT_DROP:
            ss << " Ev DROP ";
            break;
        case PKT_RCVDESTROY:
            ss << " Ev RCV ";
            break;
        case PKT_TRIM:
            ss << " Ev TRIM ";
            break;
        case PKT_BOUNCE:
            ss << " Ev BOUNCE ";
            break;
    }
    ss << " FlowID " << (uint64_t)event._val1 << " PktID " << (uint64_t)event._val2;
    return ss.str();
}

SinkLoggerSampling::SinkLoggerSampling(simtime_picosec   period,
                                       EventList&        eventlist,
                                       Logger::EventType sink_type,
                                       int               event_type)
    : EventSource(eventlist, "SinkSampling"),
      _last_time(0),
      _period(period),
      _sink_type(sink_type),
      _event_type(event_type) {
    eventlist.sourceIsPendingRel(*this, 0);
}

void SinkLoggerSampling::monitorSink(DataReceiver* sink) {
    _sinks.push_back(sink);
    _last_seq.push_back(sink->cumulative_ack());
    _last_rate.push_back(0);
}

void SinkLoggerSampling::doNextEvent() {
    eventlist().sourceIsPendingRel(*this, _period);
    simtime_picosec now   = eventlist().now();
    simtime_picosec delta = now - _last_time;
    _last_time            = now;
    uint64_t deltaB;
    uint32_t deltaSnd = 0;
    double   rate;

    for (uint64_t i = 0; i < _sinks.size(); i++) {
        if (_last_seq[i] <= _sinks[i]->cumulative_ack()) {
            // this deals with resets for periodic sources
            deltaB = _sinks[i]->cumulative_ack() - _last_seq[i];
            if (delta > 0)
                rate = deltaB * 1000000000000.0 / delta;  // Bps
            else
                rate = 0;
            _logfile->writeRecord(_sink_type,
                                  _sinks[i]->get_id(),
                                  _event_type,
                                  _sinks[i]->cumulative_ack(),
                                  deltaB > 0 ? (deltaSnd * 100000 / deltaB) : 0,
                                  rate);

            _last_rate[i] = rate;
        }
        _last_seq[i] = _sinks[i]->cumulative_ack();
    }
}
