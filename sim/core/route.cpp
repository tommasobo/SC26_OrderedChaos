// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "route.h"

#include <climits>

#include "packet.h"
#include "packet_flow.h"
#include "pipe.h"
#include "queue.h"

void Route::push_back(PacketSink* sink) {
    if (sink == nullptr) {
        throw std::runtime_error("Null sink cannot be added to route");
    }
    _sinklist.push_back(sink);
    update_hopcount(sink);
}

void Route::push_at(PacketSink* sink, int id) {
    if (sink == nullptr) {
        throw std::runtime_error("Null sink cannot be added to route");
    }
    _sinklist.insert(_sinklist.begin() + id, sink);
    update_hopcount(sink);
}

void Route::push_front(PacketSink* sink) {
    if (sink == nullptr) {
        throw std::runtime_error("Null sink cannot be added to route");
    }
    _sinklist.insert(_sinklist.begin(), sink);
    update_hopcount(sink);
}

void Route::update_hopcount(PacketSink* sink) {
    if (dynamic_cast<Pipe*>(sink) != nullptr) {
        _hop_count++;
    }
}

void Route::check_non_null() {
    bool null_found = false;
    for (auto sink : _sinklist)
        if (sink == nullptr) {
            null_found = true;
            break;
        }
    if (null_found) {
        throw std::runtime_error("Null node found in route");
    }
}
