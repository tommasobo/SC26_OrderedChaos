#include "uec_nic.h"

#include "uec_sink.h"
#include "uec_src.h"

////////////////////////////////////////////////////////////////
//  UEC NIC
////////////////////////////////////////////////////////////////

UecNIC::UecNIC(id_t src_num, EventList& eventList, linkspeed_bps linkspeed, uint32_t ports)
    : EventSource(eventList, "uecNIC"), NIC(src_num) {
    _nodename        = "uecNIC" + to_string(src_num);
    _control_size    = 0;
    _linkspeed       = linkspeed;
    _num_queued_srcs = 0;
    _no_of_ports     = ports;
    _ports.resize(_no_of_ports);
    for (uint32_t p = 0; p < _no_of_ports; p++) {
        _ports[p].send_end_time = 0;
        _ports[p].last_pktsize  = 0;
        _ports[p].busy          = false;
    }
    _busy_ports    = 0;
    _rr_port       = rand() % _no_of_ports;  // start on a random port
    _ratio_data    = 1;
    _ratio_control = 10;
    _crt           = 0;
}

// srcs call request_sending to see if they can send now.  If the
// answer is no, they'll be called back when it's time to send.
const Route* UecNIC::requestSending(UecSrc& src) {
    if (UecSrc::_debug) {
        cout << src.nodename() << " requestSending at "
             << timeAsUs(EventList::getTheEventList().now()) << endl;
    }
    if (_busy_ports == _no_of_ports) {
        // we're already sending on all ports
        /*
        if (_num_queued_srcs == 0 && _control.empty()) {
            // need to schedule the callback
            eventlist().sourceIsPending(*this, _send_end_time);
        }
        */

        if (UecSrc::_log_reaction_events && !_active_srcs.empty() &&
            _active_srcs.back() == &src && !src.rtx_queue_empty()) {
            cout << timeAsUs(eventlist().now()) << " flow " << src.flow()->flow_id()
                << " [DEBUG A] RTX request is being QUEUED by the NIC." << endl;
        }

        _num_queued_srcs += 1;
        _active_srcs.push_back(&src);
        return NULL;
    }
    assert(/*_num_queued_srcs == 0 &&*/ _control.empty());
    uint32_t portnum = findFreePort();
    return src.getPortRoute(portnum);
}

uint32_t UecNIC::findFreePort() {
    assert(_busy_ports < _no_of_ports);
    do {
        _rr_port = (_rr_port + 1) % _no_of_ports;

    } while (_ports[_rr_port].busy);
    return _rr_port;
}

uint32_t UecNIC::sendOnFreePortNow(simtime_picosec endtime, const Route* rt) {
    if (rt) {
        assert(_ports[_rr_port].busy == false);
    } else {
        _rr_port = findFreePort();
    }
    _ports[_rr_port].send_end_time = endtime;
    _ports[_rr_port].busy          = true;
    _busy_ports++;
    eventlist().sourceIsPending(*this, endtime);
    return _rr_port;
}

// srcs call startSending when they are allowed to actually send
void UecNIC::startSending(UecSrc& src, mem_b pkt_size, const Route* rt) {
    if (UecSrc::_debug) {
        cout << src.nodename() << " startSending at "
             << timeAsUs(EventList::getTheEventList().now()) << endl;
    }

    if (_num_queued_srcs > 0) {
        UecSrc* queued_src = _active_srcs.front();
        _active_srcs.pop_front();
        _num_queued_srcs--;
        assert(_num_queued_srcs >= 0);
        if (queued_src != &src) {
            throw std::runtime_error("NIC: queued_src != src in startSending");
        }
    }

    simtime_picosec endtime = eventlist().now() + (pkt_size * 8 * timeFromSec(1.0)) / _linkspeed;
    sendOnFreePortNow(endtime, rt);
}

// srcs call cantSend when they previously requested to send, and now its their turn, they can't for
// some reason.
void UecNIC::cantSend(UecSrc& src) {
    if (UecSrc::_debug) {
        cout << src.nodename() << " cantSend at " << timeAsUs(EventList::getTheEventList().now())
             << endl;
    }

    if (_num_queued_srcs == 0 && _control.empty()) {
        // it was an immediate send, so nothing to do if we can't send after all
        return;
    }
    if (_num_queued_srcs > 0) {
        _num_queued_srcs--;

        UecSrc* queued_src = _active_srcs.front();
        _active_srcs.pop_front();

        assert(queued_src == &src);
        assert(_busy_ports < _no_of_ports);

        if (_num_queued_srcs > 0) {
            // give the next src a chance.
            queued_src         = _active_srcs.front();
            const Route* route = queued_src->getPortRoute(findFreePort());
            queued_src->timeToSend(*route);
            return;
        }
    }
    if (!_control.empty()) {
        // need to send a control packet, since we didn't manage to send a data packet.
        sendControlPktNow();
    }
}

void UecNIC::sendControlPacket(UecBasePacket* pkt, UecSrc* src, UecSink* sink) {
    assert((src || sink) && !(src && sink));

    _control_size += pkt->size();
    CtrlPacket cp = {pkt, src, sink};
    _control.push_back(cp);

    if (UecSrc::_debug) {
        cout << "NIC " << this << " request to send control packet of type " << pkt->str()
             << " control queue size " << _control_size << " " << _control.size() << endl;
    }

    if (_busy_ports == _no_of_ports) {
        // all ports are busy
        if (UecSrc::_debug) {
            cout << "NIC sendControlPacket " << this << " already sending on all ports\n";
        }
    } else {
        // send now!
        sendControlPktNow();
    }
}

// actually do the send of a queued control packet
void UecNIC::sendControlPktNow() {
    assert(!_control.empty());
    assert(_busy_ports != _no_of_ports);

    CtrlPacket cp = _control.front();
    _control.pop_front();
    UecBasePacket* p = cp.pkt;

    simtime_picosec endtime = eventlist().now() + (p->size() * 8 * timeFromSec(1.0)) / _linkspeed;
    uint32_t        port_to_use = sendOnFreePortNow(endtime, NULL);
    if (UecSrc::_debug)
        cout << "NIC " << this << " send control of size " << p->size() << " at "
             << timeAsUs(eventlist().now()) << endl;

    _control_size -= p->size();
    assert(p->route() == NULL);
    const Route* route;
    if (cp.src)
        route = cp.src->getPortRoute(port_to_use);
    else
        route = cp.sink->getPortRoute(port_to_use);
    p->set_route(*route);
    p->sendOn();
}

void UecNIC::doNextEvent() {
    // doNextEvent should be called every time a packet will have finished being sent
    uint32_t last_port = _no_of_ports;
    for (uint32_t p = 0; p < _no_of_ports; p++) {
        if (_ports[p].busy && _ports[p].send_end_time == eventlist().now()) {
            last_port = p;
            break;
        }
    }
    assert(last_port != _no_of_ports);
    _busy_ports--;
    _ports[last_port].busy = false;

    if (UecSrc::_debug)
        cout << "NIC " << this << " doNextEvent at " << timeAsUs(eventlist().now()) << endl;

    if (_num_queued_srcs > 0 && !_control.empty()) {
        _crt++;

        if (_crt >= (_ratio_control + _ratio_data))
            _crt = 0;

        if (UecSrc::_debug) {
            cout << "NIC " << this << " round robin time between srcs " << _num_queued_srcs
                 << " and control " << _control.size() << " " << _crt;
        }

        if (_crt < _ratio_data) {
            // it's time for the next source to send
            UecSrc*      queued_src = _active_srcs.front();
            const Route* route      = queued_src->getPortRoute(findFreePort());
            queued_src->timeToSend(*route);

            if (UecSrc::_debug)
                cout << " send data " << endl;

            return;
        } else {
            sendControlPktNow();
            return;
        }
    }

    if (_num_queued_srcs > 0) {
        UecSrc*      queued_src = _active_srcs.front();
        const Route* route      = queued_src->getPortRoute(findFreePort());
        queued_src->timeToSend(*route);

        if (UecSrc::_debug)
            cout << "NIC " << this << " send data ONLY " << endl;
    } else if (!_control.empty()) {
        sendControlPktNow();
    }
}
