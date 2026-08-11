// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "pipe.h"

#include <spdlog/fmt/fmt.h>

Pipe::Pipe(simtime_picosec delay, EventList& eventlist)
    : EventSource(eventlist, "pipe"), _delay(delay), _next_sink(nullptr) {
    _nodename = fmt::format("pipe({}us)", delay / 1000000);
}

void Pipe::receivePacket(Packet& pkt) {
    if (_inflight.empty()) {
        // Only notify the eventlist if there were no packets in flight
        eventlist().sourceIsPendingRel(*this, _delay);
    }

    InFlightPacket inflight_pkt;
    inflight_pkt.time = eventlist().now() + _delay;
    inflight_pkt.pkt  = &pkt;
    _inflight.push(inflight_pkt);
}

Packet* Pipe::dequeuePacketAndScheduleNextEvent() {
    if (_inflight.empty()) {
        throw std::runtime_error("No packets in flight in pipe");
    }

    Packet* pkt = _inflight.next_to_pop().pkt;
    _inflight.pop();
    if (!_inflight.empty()) {
        // Notify the eventlist we've another event pending
        simtime_picosec nexteventtime = _inflight.next_to_pop().time;
        _eventlist.sourceIsPending(*this, nexteventtime);
    }
    return pkt;
}

void Pipe::handOffPacket(Packet* pkt) {
    // Tell the packet to move itself on to the next hop
    pkt->flow().logTraffic(*pkt, *this, TrafficLogger::PKT_DEPART);
    pkt->sendOn();
}

void Pipe::doNextEvent() {
    Packet* pkt = dequeuePacketAndScheduleNextEvent();
    handOffPacket(pkt);
}
