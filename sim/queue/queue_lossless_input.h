// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef _LOSSLESS_INPUT_QUEUE_H
#define _LOSSLESS_INPUT_QUEUE_H
#include "queue.h"
/*
 * A FIFO queue that supports PAUSE frames and lossless operation
 */

#include <list>

#include "callback_pipe.h"
#include "eth_pause_packet.h"
#include "event_source.h"
#include "eventlist.h"
#include "helpers.h"
#include "logger_types.h"
#include "packet.h"
#include "packet_flow.h"
#include "switch.h"
#include "types.h"

class Switch;

class LosslessInputQueue : public Queue, public VirtualQueue {
public:
    LosslessInputQueue(EventList& eventlist);
    LosslessInputQueue(EventList&      eventlist,
                       BaseQueue*      peer,
                       Switch*         sw,
                       simtime_picosec wire_latency);
    LosslessInputQueue(EventList& eventlist, BaseQueue* peer);

    virtual void receivePacket(Packet& pkt);

    void         sendPause(unsigned int wait);
    virtual void completedService(Packet& pkt);

    virtual void setName(const string& name) {
        Logged::setName(name);
        _nodename += name;
    }

    virtual string& nodename() { return _nodename; }

    enum { PAUSED, READY, PAUSE_RECEIVED };

    static uint64_t _low_threshold;
    static uint64_t _high_threshold;

private:
    int           _state_recv;
    CallbackPipe* _wire;
};

#endif
