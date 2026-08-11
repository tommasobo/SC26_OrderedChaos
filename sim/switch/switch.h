#pragma once
/*
 * A switch to group together multiple ports (currently used in the
 * PAUSE implementation), and in generic_topology
 *
 * At the moment we don't normally build topologies where the switch
 * receives a packet and makes a forwarding decision - the route
 * already carries the forwarding path.  But we might revisit this to
 * simulate switches that make dynamic decisions.
 */

#include <functional>
#include <list>
#include <memory>
#include <string>

#include "event_source.h"
#include "eventlist.h"
#include "logger_types.h"
#include "packet.h"
#include "packet_flow.h"
#include "packet_sink.h"
#include "queue.h"
#include "route_table.h"
#include "switch_buffer.h"

class BaseQueue;
class LosslessQueue;
class LosslessInputQueue;
class RouteTable;

class Switch : public EventSource, public PacketSink {
public:
    Switch(EventList&                    eventlist,
           std::string                   s             = "none",
           std::unique_ptr<SwitchBuffer> switch_buffer = nullptr)
        : EventSource(eventlist, s), _switch_buffer(std::move(switch_buffer)) {
        _name = s;
        _id   = id++;
    };

    void doNextEvent() override { abort(); }

    const string& nodename() override { return _name; }

    // inherited from PacketSink - only use when route strategy implies use of ECMP_FIB, i.e. the
    // packet does not carry a full route.
    void receivePacket(Packet& pkt) override { abort(); }

    void receivePacket(Packet& pkt, VirtualQueue* prev) override { abort(); }

    virtual int addPort(BaseQueue* q);

    virtual void addHostPort(int addr, int flowid, PacketSink* transport) { abort(); };
    virtual void removeHostPort(int addr, int flowid) {}

    uint32_t getID() { return _id; };

    virtual uint32_t getType() { return 0; }

    // Used when route strategy is ECMP_FIB and variants.
    virtual Route* getNextHop(Packet& pkt) { return getNextHop(pkt, NULL); }

    virtual Route* getNextHop(Packet& pkt, BaseQueue* ingress_port) { abort(); };

    BaseQueue* getPort(int id) {
        assert(id >= 0);
        if ((unsigned int)id < _ports.size())
            return _ports.at(id);
        else
            return NULL;
    }

    unsigned int portCount() { return _ports.size(); }

    void sendPause(LosslessQueue* problem, unsigned int wait);
    void configureLossless();

    void add_logger(Logfile& log, simtime_picosec sample_period);

    inline SwitchBuffer* getSwitchBuffer() { return _switch_buffer.get(); }

protected:
    ///  Gives the switch instance a unique ID.
    static uint32_t id;

    vector<BaseQueue*> _ports;
    uint32_t           _id;
    string             _name;

    RouteTable* _fib;

    /// Added to simulate shared buffering.
    std::unique_ptr<SwitchBuffer> _switch_buffer = nullptr;
};
