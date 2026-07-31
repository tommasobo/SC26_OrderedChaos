#pragma once

#include "event_source.h"
#include "eventlist.h"
#include "nic.h"
#include "uec_packet.h"

class UecSrc;
class UecSink;

// UecNIC aggregates UecSrcs that are on the same NIC.  It round
// robins between active srcs when we're limited by the sending
// linkspeed due to outcast (or just at startup) - this avoids
// building an output queue like the old NDP simulator did, and so
// better models what happens in a h/w NIC.
class UecNIC : public EventSource, public NIC {
    struct PortData {
        simtime_picosec send_end_time;
        bool            busy;
        mem_b           last_pktsize;
    };

    struct CtrlPacket {
        UecBasePacket* pkt;
        UecSrc*        src;
        UecSink*       sink;
    };

public:
    UecNIC(id_t src_num, EventList& eventList, linkspeed_bps linkspeed, uint32_t ports);

    // handle traffic sources.
    const Route* requestSending(UecSrc& src);
    void         startSending(UecSrc& src, mem_b pkt_size, const Route* rt);
    void         cantSend(UecSrc& src);

    // handle control traffic from receivers.
    // only one of src or sink must be set
    void     sendControlPacket(UecBasePacket* pkt, UecSrc* src, UecSink* sink);
    uint32_t findFreePort();
    void     doNextEvent();

    linkspeed_bps linkspeed() const { return _linkspeed; }

    int activeSources() const { return _active_srcs.size(); }

    virtual const string& nodename() const { return _nodename; }

    list<UecSrc*> _active_srcs;

private:
    void                    sendControlPktNow();
    uint32_t                sendOnFreePortNow(simtime_picosec endtime, const Route* rt);
    list<struct CtrlPacket> _control;
    mem_b                   _control_size;

    linkspeed_bps _linkspeed;
    int           _num_queued_srcs;

    // data related to the NIC ports
    vector<struct PortData> _ports;
    uint32_t                _rr_port;  // round robin last port we sent on
    uint32_t                _no_of_ports;
    uint32_t                _busy_ports;

    int _ratio_data, _ratio_control, _crt;

    string _nodename;
};
