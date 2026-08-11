#pragma once

#include "eventlist.h"
#include "uec_sink.h"
#include "uec_src.h"

class UecPullPacer : public EventSource {
public:
    enum reason { PCIE = 0, OVERSUBSCRIBED_CC = 1 };

    UecPullPacer(linkspeed_bps linkSpeed,
                 double        pull_rate_modifier,
                 uint16_t      bytes_credit_per_pull,
                 EventList&    eventList,
                 uint32_t      no_of_ports);
    void doNextEvent();
    void requestPull(UecSink* sink);

    bool isActive(UecSink* sink);
    bool isIdle(UecSink* sink);

    inline linkspeed_bps linkspeed() const { return _linkspeed; }

    void updatePullRate(reason r, double relative_rate);

private:
    list<UecSink*> _active_senders;  // TODO priorities?
    list<UecSink*> _idle_senders;    // TODO priorities?

    const simtime_picosec _time_per_quanta;
    simtime_picosec       _actual_time_per_quanta;

    bool _active;

    double _rates[2];

    linkspeed_bps _linkspeed;
    uint16_t      _bytes_credit_per_pull;
};
