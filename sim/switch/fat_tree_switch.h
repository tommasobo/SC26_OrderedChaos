// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef _FATTREESWITCH_H
#define _FATTREESWITCH_H

#include <unordered_map>

#include "callback_pipe.h"
#include "helpers.h"
#include "switch.h"

class FatTreeTopology;

class FlowletInfo {
public:
    uint32_t        _egress;
    simtime_picosec _last;

    FlowletInfo(uint32_t egress, simtime_picosec lasttime) {
        _egress = egress;
        _last   = lasttime;
    };
};

class FatTreeSwitch : public Switch {
public:
    enum switch_type { NONE = 0, TOR = 1, AGG = 2, CORE = 3 };

    enum routing_strategy {
        NIX              = 0,
        ECMP             = 1,
        ADAPTIVE_ROUTING = 2,
        ECMP_ADAPTIVE    = 3,
        RR               = 4,
        RR_ECMP          = 5
    };

    enum sticky_choices { PER_PACKET = 0, PER_FLOWLET = 1 };

    FatTreeSwitch(EventList&       eventlist,
                  string           s,
                  switch_type      t,
                  uint32_t         id,
                  simtime_picosec  switch_delay,
                  FatTreeTopology* ft);

    virtual void   receivePacket(Packet& pkt);
    virtual Route* getNextHop(Packet& pkt, BaseQueue* ingress_port);

    virtual uint32_t getType() { return _type; }

    uint32_t adaptive_route(vector<FibEntry*>* ecmp_set, int8_t (*cmp)(FibEntry*, FibEntry*));
    uint32_t replace_worst_choice(vector<FibEntry*>* ecmp_set,
                                  int8_t (*cmp)(FibEntry*, FibEntry*),
                                  uint32_t my_choice);
    uint32_t adaptive_route_p2c(vector<FibEntry*>* ecmp_set, int8_t (*cmp)(FibEntry*, FibEntry*));

    static int8_t compare_flow_count(FibEntry* l, FibEntry* r);
    static int8_t compare_pause(FibEntry* l, FibEntry* r);
    static int8_t compare_bandwidth(FibEntry* l, FibEntry* r);
    static int8_t compare_queuesize(FibEntry* l, FibEntry* r);
    static int8_t compare_pqb(FibEntry* l, FibEntry* r);  // compare pause,queue, bw.
    static int8_t compare_pq(FibEntry* l, FibEntry* r);   // compare pause, queue
    static int8_t compare_pb(FibEntry* l, FibEntry* r);   // compare pause, bandwidth
    static int8_t compare_qb(FibEntry* l, FibEntry* r);   // compare pause, bandwidth

    static int8_t (*fn)(FibEntry*, FibEntry*);

    std::vector<std::vector<int>> usage;
    simtime_picosec last_check = 0;

    virtual void addHostPort(int addr, int flowid, PacketSink* transport_port);
    virtual void removeHostPort(int addr, int flowid);

    virtual void permute_paths(vector<FibEntry*>* uproutes);

    static void set_strategy(routing_strategy s) {
        assert(_strategy == NIX);
        _strategy = s;
    }

    static void set_ar_fraction(uint16_t f) {
        assert(f >= 1);
        _ar_fraction = f;
    }

    static routing_strategy _strategy;
    static uint16_t         _ar_fraction;
    static uint16_t         _ar_sticky;
    static simtime_picosec  _sticky_delta;
    static double           _ecn_threshold_fraction;
    static double           _speculative_threshold_fraction;
    static uint16_t         _trim_size;
    static bool             _disable_trim;
    static bool             _low_priority_trim;
    static bool             _no_droping_low_header;
    static double           _switch_random_drop_prob;
    static bool             _log_subflow_routes;

private:
    switch_type      _type;
    Pipe*            _pipe;
    FatTreeTopology* _ft;

    // CAREFUL: can't always have a single FIB for all up destinations when there are failures!
    vector<FibEntry*>* _uproutes;

    unordered_map<uint32_t, FlowletInfo*> _flowlet_maps;

    static unordered_map<BaseQueue*, uint32_t> _port_flow_counts;

    uint32_t        _crt_route;
    uint32_t        _hash_salt;
    simtime_picosec _last_choice;

    unordered_map<Packet*, bool> _packets;
};

#endif
