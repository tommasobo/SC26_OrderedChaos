// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef PACKET_FLOW_H
#define PACKET_FLOW_H

#include <iostream>
#include <vector>

#include "helpers.h"
#include "logger_types.h"
#include "route.h"
#include "types.h"

class Packet;

class PacketFlow : public Logged {
    friend class Packet;

public:
    PacketFlow(TrafficLogger* logger);
    virtual ~PacketFlow(){};
    void set_logger(TrafficLogger* logger);
    void logTraffic(Packet& pkt, Logged& location, TrafficLogger::TrafficEvent ev);
    void set_flowid(flowid_t id);

    inline flowid_t flow_id() const { return _flow_id; }

    bool log_me() const { return _logger != NULL; }

protected:
    static packetid_t _max_flow_id;
    flowid_t          _flow_id;
    TrafficLogger*    _logger;
};

#endif
