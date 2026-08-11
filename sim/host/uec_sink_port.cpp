#include "uec_sink_port.h"

#include <cstdint>
#include <string>

#include "packet.h"
#include "packet_sink.h"
#include "route.h"
#include "uec_sink.h"

////////////////////////////////////////////////////////////////
//  UEC SINK PORT
////////////////////////////////////////////////////////////////
UecSinkPort::UecSinkPort(UecSink& sink, uint32_t port_num) : _sink(sink), _port_num(port_num) {}

void UecSinkPort::setRoute(const Route& route) {
    _route = &route;
}

void UecSinkPort::receivePacket(Packet& pkt) {
    _sink.receivePacket(pkt, _port_num);
}

const string& UecSinkPort::nodename() {
    return _sink.nodename();
}
