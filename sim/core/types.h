#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <sstream>
#include <string>
#include <vector>

class Route;

#define ECN_CE 1
#define ECN_CWR 2
#define ECN_ECHO 4

// Specify units for simulation time, link speed, buffer capacity
typedef long long sint64_t;
typedef uint64_t  simtime_picosec;
typedef uint64_t  linkspeed_bps;
typedef sint64_t  mem_b;     // memory in bytes (prefer over int for anything measured in bytes)
typedef int       mem_pkts;  // memory in packets (prefer over int for anything that counts packets)
typedef uint32_t  addr_t;
typedef uint16_t  port_t;
typedef uint32_t  packetid_t;
typedef uint32_t  flowid_t;

typedef Route                 route_t;
typedef std::vector<route_t*> routes_t;

// Gumph
// TODO(aghalayini):We should not have this explicit namespace in a header file.
#if defined(__cplusplus) && !defined(__STL_NO_NAMESPACES)
using namespace std;
#endif
