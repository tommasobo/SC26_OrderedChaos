// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef _RANDOM_QUEUE_H
#define _RANDOM_QUEUE_H
#include "queue.h"
/*
 * A simple FIFO queue that drops randomly when it gets full
 */

#include <list>

#include "event_source.h"
#include "eventlist.h"
#include "helpers.h"
#include "logger_types.h"
#include "packet.h"
#include "packet_flow.h"
#include "types.h"

class RandomQueue : public Queue {
public:
    RandomQueue(linkspeed_bps bitrate,
                mem_b         maxsize,
                EventList&    eventlist,
                QueueLogger*  logger,
                mem_b         drop);
    void receivePacket(Packet& pkt);
    void set_packet_loss_rate(double v);
    // should really be private, but loggers want to see
private:
    mem_b  _drop_th, _drop;
    int    _buffer_drops;
    double _plr;
};

#endif
