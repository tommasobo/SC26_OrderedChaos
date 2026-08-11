#pragma once

#include "packet_flow.h"

class VirtualQueue;
class LosslessInputQueue;

typedef enum {
    IP,
    TCP,
    TCPACK,
    TCPNACK,
    SWIFT,
    SWIFTACK,
    STRACK,
    STRACKACK,
    NDP,
    NDPACK,
    NDPNACK,
    NDPPULL,
    NDPRTS,
    NDPLITE,
    NDPLITEACK,
    NDPLITEPULL,
    NDPLITERTS,
    ETH_PAUSE,
    TOFINO_TRIM,
    ROCE,
    ROCEACK,
    ROCENACK,
    HPCC,
    HPCCACK,
    HPCCNACK,
    CNP,
    EQDSDATA,
    EQDSPULL,
    EQDSACK,
    EQDSNACK,
    EQDSRTS,
    UECDATA,
    UECPULL,
    UECACK,
    UECNACK,
    UECRTS
} packet_type;

typedef enum { NONE, UP, DOWN } packet_direction;

// See tcppacket.h to illustrate how Packet is typically used.
class Packet {
    friend class PacketFlow;

public:
    // use PRIO_NONE if the packet is never expected to encounter a priority queue, otherwise
    // default to PRIO_LO
    typedef enum { PRIO_LO, PRIO_MID, PRIO_HI, PRIO_NONE } PktPriority;

    constexpr static int DEFAULT_PACKET_SIZE = 1500;

    /* empty constructor; Packet::set must always be called as
       well. It's a separate method, for convenient reuse */
    Packet() {
        _is_header    = false;
        _bounced      = false;
        _type         = IP;
        _flags        = 0;
        _refcount     = 0;
        _dst          = UINT32_MAX;
        _pathid       = UINT32_MAX;
        _direction    = NONE;
        _ingressqueue = NULL;
    }

    /* say "this packet is no longer wanted". (doesn't necessarily
       destroy it, so it can be reused) */
    virtual void free();

    static void set_packet_size(int packet_size) {
        // Use Packet::set_packet_size() to change the default packet
        // size for TCP or NDP data packets.  You MUST call this
        // before the value has been used to initialize anything else.
        // If someone has already read the value of packet size, no
        // longer allow it to be changed, or all hell will break
        // loose.
        assert(_packet_size_fixed == false);
        _data_packet_size = packet_size;
    }

    static int data_packet_size() {
        _packet_size_fixed = true;
        return _data_packet_size;
    }

    bool is_ack = false;  // is this packet an ack packet?
    bool is_rtx = false; // is this packet a retransmission?
    string flow_name;

    virtual PacketSink* sendOn();  // "go on to the next hop along your route"
                                   // returns what that hop is

    virtual PacketSink* previousHop() {
        if (_nexthop >= 2)
            return _route->at(_nexthop - 2);
        else
            return NULL;
    }

    virtual PacketSink* currentHop() {
        if (_nexthop >= 1)
            return _route->at(_nexthop - 1);
        else
            return NULL;
    }

    virtual PacketSink* sendOn2(VirtualQueue* crtSink);

    uint16_t size() const { return _size; }

    void set_size(int i) { _size = i; }

    packet_type type() const { return _type; };

    bool header_only() const { return _is_header; }

    bool header_low_only() const { return _is_header_low; }

    bool bounced() const { return _bounced; }

    PacketFlow& flow() const { return *_flow; }

    virtual ~Packet(){};

    inline const packetid_t id() const { return _id; }

    inline uint32_t flow_id() const { return _flow->flow_id(); }

    inline uint32_t dst() const { return _dst; }

    inline void set_dst(uint32_t dst) { _dst = dst; }

    inline uint32_t pathid() {
        assert(_pathid != UINT32_MAX);
        return _pathid;
    }

    inline void set_pathid(uint32_t p) { _pathid = p; }

    const Route* route() const { return _route; }

    const Route* reverse_route() const { return _route->reverse(); }

    inline void set_next_hop(PacketSink* snk) { _next_routed_hop = snk; }

    virtual void strip_payload(uint16_t trim_size) {
        assert(trim_size >= 64);
        assert(!_is_header);
        _is_header = true;
        _size      = trim_size;
    }

    virtual void strip_payload_low(uint16_t trim_size) {
        assert(trim_size >= 64);
        assert(!_is_header_low);
        _is_header_low = true;
        _size          = trim_size;
    }

    virtual void bounce();
    virtual void unbounce(uint16_t pktsize);

    inline uint32_t path_len() const { return _path_len; }

    virtual void go_up() {
        if (_direction == NONE)
            _direction = UP;
        else if (_direction == DOWN)
            abort();
    }

    virtual void go_down() {
        if (_direction == UP)
            _direction = DOWN;
        else if (_direction == NONE)
            abort();
    }

    virtual void set_direction(packet_direction d) {
        if (d == _direction)
            return;
        if ((_direction == NONE) || (_direction == UP && d == DOWN))
            _direction = d;
        else {
            cout << "Current direction is " << _direction << " trying to change it to " << d
                 << endl;
            abort();
        }
    }

    virtual PktPriority priority() const = 0;

    virtual packet_direction get_direction() { return _direction; }

    void inc_ref_count() { _refcount++; };

    void dec_ref_count() { _refcount--; };

    int ref_count() { return _refcount; };

    inline uint32_t flags() const { return _flags; }

    inline void set_flags(uint32_t f) { _flags = f; }

    uint32_t nexthop() const { return _nexthop; }  // only intended to be used for debugging

    virtual void set_route(const Route& route);
    virtual void set_route(const Route* route = nullptr);
    virtual void set_route(PacketFlow& flow, const Route& route, int pkt_size, packetid_t id);

    void set_ingress_queue(LosslessInputQueue* t) {
        assert(!_ingressqueue);
        _ingressqueue = t;
    }

    LosslessInputQueue* get_ingress_queue() {
        assert(_ingressqueue);
        return _ingressqueue;
    }

    void clear_ingress_queue() {
        assert(_ingressqueue);
        _ingressqueue = NULL;
    }

    string str() const;

    bool drop_this = false;

protected:
    void set_attrs(PacketFlow& flow, int pkt_size, packetid_t id);

    static int _data_packet_size;    // default size of a TCP or NDP data packet,
                                     // measured in bytes
    static bool _packet_size_fixed;  // prevent foot-shooting

    packet_type _type;

    uint16_t _size, _oldsize;

    bool     _is_header;
    bool     _is_header_low;
    bool     _bounced;  // packet has hit a full queue, and is being bounced back to the sender
    uint32_t _flags;    // used for ECN & friends

    uint32_t         _dst;        // used for packets that do not have a route in switched networks.
    uint32_t         _pathid;     // used for ECMP hashing.
    packet_direction _direction;  // used to avoid loop in FatTrees.

    // A packet can contain a route or a routegraph, but not both.
    // Eventually switch over entirely to RouteGraph?
    const Route* _route;

    // PacketSink* _detour;
    uint32_t _nexthop, _oldnexthop;

    // used for tunneling purposes when one packet can be referenced by multiple classes
    uint8_t _refcount;

    // used when using routing tables in switches, i.e. the packet has no route.
    PacketSink* _next_routed_hop;

    packetid_t          _id;
    PacketFlow*         _flow{nullptr};
    static PacketFlow   _defaultFlow;
    LosslessInputQueue* _ingressqueue;
    uint32_t _path_len;  // length of the path in hops - used in BCube priority routing with NDP
};