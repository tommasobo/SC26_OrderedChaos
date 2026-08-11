#include "uec_pull_pacer.h"

#include "eventlist.h"
#include "uec_sink.h"
#include "uec_src.h"

////////////////////////////////////////////////////////////////
//  UEC PACER
////////////////////////////////////////////////////////////////

// pull rate modifier should generally be something like 0.99 so we pull at just less than line rate
UecPullPacer::UecPullPacer(linkspeed_bps linkSpeed,
                           double        pull_rate_modifier,
                           uint16_t      bytes_credit_per_pull,
                           EventList&    eventList,
                           uint32_t      no_of_ports)
    : EventSource(eventList, "uecPull"),
      _time_per_quanta((8 * UEC_PULL_QUANTUM * 1e12 / (linkSpeed * no_of_ports)) /
                       pull_rate_modifier) {
    _active                   = false;
    _actual_time_per_quanta   = _time_per_quanta;
    _bytes_credit_per_pull    = bytes_credit_per_pull;
    _linkspeed                = linkSpeed;
    _rates[PCIE]              = 1;
    _rates[OVERSUBSCRIBED_CC] = 1;
}

void UecPullPacer::doNextEvent() {
    if (_active_senders.empty() && _idle_senders.empty()) {
        _active = false;
        return;
    }

    UecSink*                   sink = NULL;
    UecPullPacket*             pullPkt;
    UecBasePacket::pull_quanta extra_credit = 0;

    if (!_active_senders.empty()) {
        sink = _active_senders.front();

        assert(sink->inPullQueue());

        _active_senders.pop_front();
        pullPkt = sink->pull(extra_credit);

        // TODO if more pulls are needed, enqueue again
        if (UecSrc::_debug)
            cout << "PullPacer: Active: " << sink->getSrc()->flow()->str() << " backlog "
                 << sink->backlog() << " at " << timeAsUs(eventlist().now()) << endl;
        if (sink->backlog() > 0)
            _active_senders.push_back(sink);
        else {  // this sink has had its demand satisfied, move it to idle senders list.
            _idle_senders.push_back(sink);
            sink->removeFromPullQueue();
            sink->addToSlowPullQueue();
        }
    } else {  // no active senders, we must have at least one idle sender
        sink = _idle_senders.front();
        _idle_senders.pop_front();

        if (!sink->inSlowPullQueue())
            sink->addToSlowPullQueue();

        if (UecSrc::_debug)
            cout << "PullPacer: Idle: " << sink->getSrc()->flow()->str() << " at "
                 << timeAsUs(eventlist().now()) << " backlog " << sink->backlog() << " "
                 << sink->slowCredit() << " max "
                 << UecBasePacket::quantize_floor(sink->getMaxCwnd()) << endl;
        extra_credit = UecSink::_credit_per_pull;
        pullPkt      = sink->pull(extra_credit);
        pullPkt->set_slow_pull(true);

        if (sink->backlog() == 0 &&
            sink->slowCredit() < UecBasePacket::quantize_floor(sink->getMaxCwnd())) {
            // only send upto 1BDP worth of speculative credit.
            // backlog will be negative once this source starts receiving speculative credit.
            _idle_senders.push_back(sink);
        } else {
            sink->removeFromSlowPullQueue();
        }
    }

    pullPkt->flow().logTraffic(*pullPkt, *this, TrafficLogger::PKT_SEND);

    // pullPkt->sendOn();
    sink->getNIC()->sendControlPacket(pullPkt, NULL, sink);
    _active = true;

    if (extra_credit == 0) {
        // we need some time between pulls, even if we're not sending more credit;
        extra_credit = 1024 >> UEC_PULL_SHIFT;
    }
    simtime_picosec pkt_time = _actual_time_per_quanta * extra_credit;
    assert(pkt_time > 0);
    eventlist().sourceIsPendingRel(*this, pkt_time);
}

void UecPullPacer::updatePullRate(reason r, double relative_rate) {
    _rates[r] = relative_rate;

    _actual_time_per_quanta = _time_per_quanta / min(_rates[PCIE], _rates[OVERSUBSCRIBED_CC]);

    if (UecSrc::_debug)
        cout << "Interpacket delay "
             << timeAsUs(_actual_time_per_quanta * UecSink::_credit_per_pull) << endl;
}

bool UecPullPacer::isActive(UecSink* sink) {
    for (auto i = _active_senders.begin(); i != _active_senders.end(); i++) {
        if (*i == sink)
            return true;
    }
    return false;
}

bool UecPullPacer::isIdle(UecSink* sink) {
    for (auto i = _idle_senders.begin(); i != _idle_senders.end(); i++) {
        if (*i == sink)
            return true;
    }
    return false;
}

void UecPullPacer::requestPull(UecSink* sink) {
    if (isActive(sink)) {
        abort();
    }
    assert(sink->inPullQueue());

    _active_senders.push_back(sink);
    // TODO ack timer

    if (!_active) {
        eventlist().sourceIsPendingRel(*this, 0);
        _active = true;
    }
}
