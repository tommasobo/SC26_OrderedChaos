#pragma once

#include <cstdint>
#include <string>

#include "packet.h"
#include "packet_sink.h"
#include "route.h"
#include "uec_sink.h"

// Packets are received on ports, but then passed to the Sink for handling
class UecSinkPort : public PacketSink {
public:
    UecSinkPort(UecSink& sink, uint32_t portnum);
    void setRoute(const Route& route);

    inline const Route* route() const { return _route; }

    virtual void               receivePacket(Packet& pkt);
    virtual const std::string& nodename();

private:
    UecSink&     _sink;
    uint8_t      _port_num;
    const Route* _route;
};
