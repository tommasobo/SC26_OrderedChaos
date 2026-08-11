// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef _LOSSLESS_OUTPUT_QUEUE_H
#define _LOSSLESS_OUTPUT_QUEUE_H
/*
 * A FIFO queue that supports PAUSE frames and lossless operation
 */

#include <list>

#include "eth_pause_packet.h"
#include "event_source.h"
#include "eventlist.h"
#include "helpers.h"
#include "logger_types.h"
#include "packet.h"
#include "packet_flow.h"
#include "queue.h"
#include "types.h"

class LosslessOutputQueue : public Queue {
public:
    LosslessOutputQueue(linkspeed_bps bitrate,
                        mem_b         maxsize,
                        EventList&    eventlist,
                        QueueLogger*  logger);

    void receivePacket(Packet& pkt);
    void receivePacket(Packet& pkt, VirtualQueue* q);

    void beginService();
    void completeService();

    bool is_paused() { return _state_send == PAUSED || _state_send == PAUSE_RECEIVED; }

    enum queue_state { PAUSED, READY, PAUSE_RECEIVED };

private:
    list<VirtualQueue*> _vq;

    int      _state_send;
    int      _sending;
    uint64_t _txbytes;

public:
    static int _ecn_enabled;
    static int _K;
};

#endif
