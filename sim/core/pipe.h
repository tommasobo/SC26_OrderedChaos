// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef PIPE_H
#define PIPE_H

/*
 * A pipe is a dumb device which simply delays all incoming packets
 */

#include <list>
#include <utility>

#include "circular_buffer.h"
#include "event_source.h"
#include "eventlist.h"
#include "helpers.h"
#include "logger_types.h"
#include "packet.h"
#include "packet_flow.h"
#include "packet_sink.h"
#include "types.h"

/**
 * @brief A pipe is a simple FIFO queue which delays all incoming packets by a fixed amount of time
 * and then hands them off to the packet's next hop.
 */
class Pipe : public EventSource, public PacketSink {
public:
    /// Constructor.
    Pipe(simtime_picosec delay, EventList& eventlist);

    /// Inherited from PacketSink.
    void receivePacket(Packet& pkt) override;

    /// Inherited from EventSource.
    void doNextEvent() override;

    // @name Setters.
    //@{
    void forceName(string name) { _nodename = name; }

    void setNext(PacketSink* next_sink) { _next_sink = next_sink; }

    //@}

    // @name Getters.
    //@{
    const string& nodename() override { return _nodename; }

    PacketSink* next() const { return _next_sink; }

    simtime_picosec delay() { return _delay; }

    //@}

protected:
    struct InFlightPacket {
        simtime_picosec time;
        Packet*         pkt;
    };

    /// Dequeue the next packet from the pipe, and if there are more packets in flight,
    /// schedule the next event.
    Packet* dequeuePacketAndScheduleNextEvent();

    /// Hand off the packet to the next hop.
    virtual void handOffPacket(Packet* pkt);

    /// The name of the pipe.
    string _nodename;
    /// The packets in flight because of the pipe delay.
    CircularBuffer<InFlightPacket> _inflight;

private:
    /// The delay of the pipe.
    simtime_picosec _delay;
    // TODO(aghalayini): Is this really needed? It is not used in the current implementation. Can we
    // remove it?
    PacketSink* _next_sink;
};

#endif
