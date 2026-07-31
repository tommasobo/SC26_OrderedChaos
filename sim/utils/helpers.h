#pragma once

#include "types.h"

/*
 * The following hash function is adapted from "Hash Functions" by Bob Jenkins
 * ("Algorithm Alley", Dr. Dobbs Journal, September 1997).
 *
 * http://www.burtleburtle.net/bob/hash/spooky.html
 */
#define MIX(a, b, c)    \
    do {                \
        a -= b;         \
        a -= c;         \
        a ^= (c >> 13); \
        b -= c;         \
        b -= a;         \
        b ^= (a << 8);  \
        c -= a;         \
        c -= b;         \
        c ^= (b >> 13); \
        a -= b;         \
        a -= c;         \
        a ^= (c >> 12); \
        b -= c;         \
        b -= a;         \
        b ^= (a << 16); \
        c -= a;         \
        c -= b;         \
        c ^= (b >> 5);  \
        a -= b;         \
        a -= c;         \
        a ^= (c >> 3);  \
        b -= c;         \
        b -= a;         \
        b ^= (a << 10); \
        c -= a;         \
        c -= b;         \
        c ^= (b >> 15); \
    } while (/*CONSTCOND*/ 0)

static inline uint32_t freeBSDHash(uint32_t target1, uint32_t target2 = 0, uint32_t target3 = 0) {
    uint32_t a = 0x9e3779b9, b = 0x9e3779b9, c = 0;  // hask key

    b += target3;
    c += target2;
    a += target1;
    MIX(a, b, c);
    return c;
}

#undef MIX

double drand();
int    pareto(int xm, int mean);
double exponential(double lambda);

simtime_picosec timeFromSec(double secs);
simtime_picosec timeFromMs(double msecs);
simtime_picosec timeFromMs(int msecs);
simtime_picosec timeFromUs(double usecs);
simtime_picosec timeFromUs(uint32_t usecs);
simtime_picosec timeFromNs(double nsecs);
double          timeAsMs(simtime_picosec ps);
double          timeAsUs(simtime_picosec ps);
double          timeAsNs(simtime_picosec ps);
double          timeAsSec(simtime_picosec ps);

mem_b memFromPkt(double pkts);

linkspeed_bps speedFromGbps(double Gbitps);
linkspeed_bps speedFromMbps(uint64_t Mbitps);
linkspeed_bps speedFromMbps(double Mbitps);
linkspeed_bps speedFromKbps(uint64_t Kbitps);
linkspeed_bps speedFromPktps(double packetsPerSec);
double        speedAsPktps(linkspeed_bps bps);
double        speedAsGbps(linkspeed_bps bps);
