#include "uec_src_port.h"

#include <cstdint>
#include <string>

#include "packet.h"
#include "packet_sink.h"
#include "route.h"
#include "uec_src.h"

////////////////////////////////////////////////////////////////
//  UEC SRC PORT
////////////////////////////////////////////////////////////////
UecSrcPort::UecSrcPort(UecSrc& src, uint32_t port_num) : _src(src), _port_num(port_num) {}

void UecSrcPort::setRoute(const Route& route) {
    _route = &route;
}

void UecSrcPort::receivePacket(Packet& pkt) {
    _src.receivePacket(pkt, _port_num);
}

const string& UecSrcPort::nodename() {
    return _src.nodename();
}
