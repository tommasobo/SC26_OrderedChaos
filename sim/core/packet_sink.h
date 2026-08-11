#pragma once

#include <string>

class Packet;
class VirtualQueue;

class PacketSink {
public:
    PacketSink() { _remoteEndpoint = nullptr; }

    virtual ~PacketSink() {}

    virtual void receivePacket(Packet& pkt) = 0;

    virtual void receivePacket(Packet& pkt, VirtualQueue* previousHop) { receivePacket(pkt); };

    virtual void setRemoteEndpoint(PacketSink* q) { _remoteEndpoint = q; };

    virtual void setRemoteEndpoint2(PacketSink* q) {
        _remoteEndpoint = q;
        q->setRemoteEndpoint(this);
    };

    PacketSink* getRemoteEndpoint() { return _remoteEndpoint; }

    virtual const std::string& nodename() = 0;

    PacketSink* _remoteEndpoint;
};
