#include "callback_pipe.h"

#include <iostream>

CallbackPipe::CallbackPipe(simtime_picosec delay, EventList& eventlist, PacketSink* c)
    : Pipe(delay, eventlist) {
    _nodename = "callbackpipe(" + std::to_string(delay / 1000000) + "us)";
    // if callback is NULL, this is send the packet back to its current hop - careful with loops!
    _callback = c;
}

void CallbackPipe::handOffPacket(Packet* pkt) {
    // Tell the packet to move itself to the callback sink if one is specified,
    // otherwise send it back to its current hop.
    if (_callback)
        _callback->receivePacket(*pkt);
    else
        pkt->currentHop()->receivePacket(*pkt);
}
