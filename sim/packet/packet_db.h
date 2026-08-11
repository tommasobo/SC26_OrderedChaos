#pragma once

#include <vector>

// For speed, it may be useful to keep a database of all packets that
// have been allocated -- that way we don't need a malloc for every
// new packet, we can just reuse old packets. Care, though -- the set()
// method will need to be invoked properly for each new/reused packet
template <class P>
class PacketDB {
public:
    PacketDB() : _alloc_count(0) {}

    ~PacketDB() {}

    P* allocPacket() {
        if (_freelist.empty()) {
            P* p = new P();
            p->inc_ref_count();
            _alloc_count++;
            return p;
        } else {
            P* p = _freelist.back();
            _freelist.pop_back();
            p->inc_ref_count();
            return p;
        }
    };

    void freePacket(P* pkt) {
        assert(pkt->ref_count() >= 1);
        pkt->dec_ref_count();

        if (!pkt->ref_count())
            _freelist.push_back(pkt);
    };

protected:
    std::vector<P*> _freelist;
    int             _alloc_count;
};
