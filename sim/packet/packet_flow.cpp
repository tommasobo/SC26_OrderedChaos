// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "packet_flow.h"

#include "packet.h"

// flow ids above this are dynamically allocated; ones less than this can be manually allocated
#define FLOW_ID_DYNAMIC_BASE 1000000000
flowid_t PacketFlow::_max_flow_id = FLOW_ID_DYNAMIC_BASE;

PacketFlow::PacketFlow(TrafficLogger* logger) : Logged("PacketFlow"), _logger(logger) {
    _flow_id = _max_flow_id++;
}

void PacketFlow::set_flowid(flowid_t id) {
    if (id >= FLOW_ID_DYNAMIC_BASE) {
        cerr << "Illegal flow ID - manually allocation must be less than dynamic base\n";
        assert(0);
    }
    _flow_id = id;
}

void PacketFlow::set_logger(TrafficLogger* logger) {
    _logger = logger;
}

void PacketFlow::logTraffic(Packet& pkt, Logged& location, TrafficLogger::TrafficEvent ev) {
    if (_logger)
        _logger->logTraffic(pkt, location, ev);
}

Logged::id_t Logged::LASTIDNUM = 1;
