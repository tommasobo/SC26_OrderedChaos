#pragma once

#include <cstdint>
#include <string>

#include "packet.h"
#include "packet_sink.h"
#include "route.h"
#include "uec_src.h"

// Packets are received on ports, but then passed to the Src for handling
class UecSrcPort : public PacketSink {
public:
    UecSrcPort(UecSrc& src, uint32_t portnum);
    void setRoute(const Route& route);

    inline const Route* route() const { return _route; }

    virtual void               receivePacket(Packet& pkt);
    virtual const std::string& nodename();

private:
    UecSrc&      _src;
    uint8_t      _port_num;
    const Route* _route;  // we're only going to support ECMP_HOST for now.
};
