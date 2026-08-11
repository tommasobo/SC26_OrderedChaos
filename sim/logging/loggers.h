// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef LOGGERS_H
#define LOGGERS_H

#include <list>
#include <map>

#include "data_receiver.h"
#include "event_source.h"
#include "eventlist.h"
#include "logfile.h"
#include "logger_types.h"
#include "packet.h"
#include "packet_flow.h"
#include "queue.h"

class FlowEventLoggerSimple : public FlowEventLogger {
public:
    void logEvent(PacketFlow& flow, Logged& location, FlowEvent ev, mem_b bytes, uint64_t pkts);
    static string event_to_str(RawLogEvent& event);
};

class TrafficLoggerSimple : public TrafficLogger {
public:
    void          logTraffic(Packet& pkt, Logged& location, TrafficEvent ev);
    static string event_to_str(RawLogEvent& event);
};

// a queue logger manager will create the relevant QueueLogger when
// requested.  This is useful so we don't need to tell every topology
// what type of logging we want right now - just configure the
// QueueLoggerManager, and it will create QueueLoggers when requested
// according to its config.
class QueueLoggerFactory {
public:
    enum QueueLoggerType { LOGGER_SIMPLE, LOGGER_SAMPLING, MULTIQUEUE_SAMPLING, LOGGER_EMPTY };

    QueueLoggerFactory(Logfile* lg, QueueLoggerType logtype, EventList& eventlist);
    QueueLogger* createQueueLogger();

    void set_sample_period(simtime_picosec sample_period) { _sample_period = sample_period; }

private:
    Logfile*             _logfile;
    QueueLoggerType      _logger_type;
    simtime_picosec      _sample_period;
    EventList&           _eventlist;
    vector<QueueLogger*> _loggers;
};

class QueueLoggerSimple : public QueueLogger {
public:
    virtual void  logQueue(BaseQueue& queue, QueueEvent ev, Packet& pkt);
    static string event_to_str(RawLogEvent& event);
};

// QueueLoggerEmpty simply keeps track of the amount of time a queue was busy
class QueueLoggerEmpty : public QueueLogger, public EventSource {
public:
    QueueLoggerEmpty(simtime_picosec period, EventList& eventlist);
    virtual void    logQueue(BaseQueue& queue, QueueEvent ev, Packet& pkt);
    void            doNextEvent();
    static string   event_to_str(RawLogEvent& event);
    void            reset_count();
    simtime_picosec _last_transition;
    simtime_picosec _total_busy;

private:
    simtime_picosec _period;
    simtime_picosec _last_dump;
    bool            _busy;
    BaseQueue*      _queue;
    uint32_t        _pkt_arrivals;
    uint32_t        _pkt_trims;
};

class QueueLoggerSampling : public QueueLogger, public EventSource {
public:
    QueueLoggerSampling(simtime_picosec period, EventList& eventlist);
    void          logQueue(BaseQueue& queue, QueueEvent ev, Packet& pkt);
    void          doNextEvent();
    static string event_to_str(RawLogEvent& event);

private:
    BaseQueue*      _queue;
    simtime_picosec _lastlook;
    simtime_picosec _period;
    mem_b           _lastq;
    bool            _seenQueueInD;
    mem_b           _minQueueInD;
    mem_b           _maxQueueInD;
    mem_b           _lastDroppedInD;
    mem_b           _lastIdledInD;
    int             _numIdledInD;
    int             _numDropsInD;
    double          _cumidle;
    double          _cumarr;
    double          _cumdrop;
};

class MultiQueueLoggerSampling : public QueueLogger, public EventSource {
public:
    MultiQueueLoggerSampling(id_t id, simtime_picosec period, EventList& eventlist);
    void          logQueue(BaseQueue& queue, QueueEvent ev, Packet& pkt);
    void          doNextEvent();
    static string event_to_str(RawLogEvent& event);

private:
    int             _id;
    simtime_picosec _period;
    bool            _seenQueueInD;
    mem_b           _minQueueInD;
    mem_b           _maxQueueInD;
    mem_b           _currentQueueSizeBytes;
    int             _currentQueueSizePkts;
};

class SinkLoggerSampling : public Logger, public EventSource {
public:
    SinkLoggerSampling(simtime_picosec   period,
                       EventList&        eventlist,
                       Logger::EventType sink_type,
                       int               _event_type);
    virtual void doNextEvent();
    void         monitorSink(DataReceiver* sink);

protected:
    vector<DataReceiver*> _sinks;

    vector<uint64_t> _last_seq;
    vector<uint32_t> _last_sndbuf;
    vector<double>   _last_rate;

    simtime_picosec   _last_time;
    simtime_picosec   _period;
    Logger::EventType _sink_type;
    int               _event_type;
};

#endif
