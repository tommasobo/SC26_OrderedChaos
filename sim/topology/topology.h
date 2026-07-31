// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef TOPOLOGY
#define TOPOLOGY

#include "loggers.h"
#include "packet.h"
#include "packet_flow.h"

#define HOST_NIC 100000  // host nic speed in Mbps
#define CORE_TO_HOST 4

#define NI 3  // Number of intermediate switches
#define NA 6  // Number of aggregation switches
#define NT 9  // Number of ToR switches (180 hosts)

#define NS 20  // Number of servers per ToR switch
#define TOR_AGG2(tor) (10 * NA - tor - 1) % NA

#define SWITCH_BUFFER 97
#define RANDOM_BUFFER 3
#define FEEDER_BUFFER 1000

class Topology {
public:
    virtual vector<const Route*>* get_paths(uint32_t src, uint32_t dest) {
        return get_bidir_paths(src, dest, true);
    }

    virtual vector<const Route*>* get_bidir_paths(uint32_t src, uint32_t dest, bool reverse) = 0;
    virtual vector<uint32_t>*     get_neighbours(uint32_t src)                               = 0;

    virtual uint32_t no_of_nodes() const { abort(); }

    // add loggers to record total queue size at switches
    virtual void add_switch_loggers(Logfile& log, simtime_picosec sample_period) { abort(); }
};

#endif
