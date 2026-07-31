// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef CALLBACKPIPE_H
#define CALLBACKPIPE_H

#include "pipe.h"

/**
 * @brief A CallbackPipe is a variant of Pipe that allows specifying a callback destination
 * for packets.
 *
 * When a packet reaches the end of the pipe after the delay, instead of calling
 * Packet::sendOn(), it will be sent to the specified callback sink's receivePacket()
 * method. If no callback is specified, the packet will be sent back to its current hop
 * (be careful of loops!).
 */
class CallbackPipe : public Pipe {
public:
    /// @brief Constructor for a CallbackPipe.
    /// @param delay The delay of the pipe.
    /// @param eventlist The eventlist to use.
    /// @param callback The callback sink to use.
    CallbackPipe(simtime_picosec delay, EventList& eventlist, PacketSink* callback);

    /// Instead of calling Packet::sendOn(), the packet is sent to the callback sink's
    /// receivePacket() method.
    void handOffPacket(Packet* pkt) override;

private:
    /// The callback sink to use. If it is NULL, the packet will be sent back to its current hop.
    PacketSink* _callback;
};

#endif
