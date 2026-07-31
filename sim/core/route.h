// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef ROUTE_H
#define ROUTE_H

/*
 * A Route, carried by packets, to allow routing
 */

#include <list>
#include <vector>

#include "types.h"

class PacketSink;

/**
 * @brief A Route, carried by packets, to allow routing across the network over multiple packet
 * sinks.
 *
 * A route is a list of packet sinks that a packet will traverse to reach its destination.
 */
class Route {
public:
    Route() : _hop_count(0), _reverse(nullptr) {}

    /// @name Push methods for adding sinks to the route and accessors for the route.
    /// @{
    void push_back(PacketSink* sink);
    void push_at(PacketSink* sink, int id);
    void push_front(PacketSink* sink);

    inline size_t size() const { return _sinklist.size(); }

    inline std::vector<PacketSink*>::const_iterator begin() const { return _sinklist.begin(); }

    inline std::vector<PacketSink*>::const_iterator end() const { return _sinklist.end(); }

    inline PacketSink* at(size_t n) const { return _sinklist.at(n); }

    /// @}

    /// Checks that the route is not null and that all the sinks in the route are not null.
    /// @throws std::runtime_error if a null sink is found in the route.
    void check_non_null();

    /// @name Getters
    /// @{
    inline int path_id() const { return _path_id; }

    inline int no_of_paths() const { return _no_of_paths; }

    inline uint32_t hop_count() const { return _hop_count; }

    inline const Route* reverse() const { return _reverse; }

    /// @}

    /// @name Setters
    /// @{
    inline void set_reverse(Route* reverse) { _reverse = reverse; }

    /// @}

private:
    /// Increments the hop count only if the sink is a pipe, meaning it is a hop.
    void update_hopcount(PacketSink* sink);

    /// The list of sinks in the route.
    std::vector<PacketSink*> _sinklist;
    /// The number of hops in the route.
    uint32_t _hop_count;
    /// The reverse route.
    Route* _reverse;
    /// The path identifier for this path.
    int _path_id;
    /// The total number of paths sender is using.
    int _no_of_paths;
};

#endif
