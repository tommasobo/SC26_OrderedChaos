// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef _ECN_QUEUE_H
#define _ECN_QUEUE_H
#include "queue.h"
/*
 * A simple ECN queue that marks on dequeue as soon as the packet occupancy exceeds the set
 * threshold.
 */

#include <list>

#include "event_source.h"
#include "eventlist.h"
#include "helpers.h"
#include "logger_types.h"
#include "packet.h"
#include "packet_flow.h"
#include "types.h"

class ECNQueue : public Queue {
public:
    ECNQueue(linkspeed_bps bitrate,
             mem_b         maxsize,
             EventList&    eventlist,
             QueueLogger*  logger,
             mem_b         drop);
    void receivePacket(Packet& pkt);
    void completeService();

private:
    mem_b _K;
    int   _state_send;
};

#endif
