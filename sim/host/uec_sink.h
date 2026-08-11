#pragma once

#include "uec_config.h"
#include "uec_nic.h"
#include "uec_packet.h"
#include "uec_src.h"

class UecPullPacer;
class UecSinkPort;

class UecSink : public DataReceiver {
public:
    struct Stats {
        uint64_t received;
        uint64_t bytes_received;
        uint64_t duplicates;
        uint64_t out_of_order;
        uint64_t trimmed;
        uint64_t pulls;
        uint64_t rts;
        uint64_t ecn_received;
        uint64_t ecn_bytes_received;
    };

    UecSink(TrafficLogger* trafficLogger,
            UecPullPacer*  pullPacer,
            UecNIC&        nic,
            uint32_t       no_of_ports);
    UecSink(TrafficLogger* trafficLogger,
            linkspeed_bps  linkSpeed,
            double         rate_modifier,
            uint16_t       mtu,
            EventList&     eventList,
            UecNIC&        nic,
            uint32_t       no_of_ports);
    void receivePacket(Packet& pkt, uint32_t port_num);

    void processData(UecDataPacket& pkt);
    void processRts(const UecRtsPacket& pkt);
    void processTrimmed(const UecDataPacket& pkt);

    void handlePullTarget(UecBasePacket::seq_t pt);

    virtual const string& nodename() { return _nodename; }

    virtual uint64_t cumulative_ack() { return _expected_epsn; }

    virtual uint32_t drops() { return 0; }

    inline flowid_t flowId() const { return _flow.flow_id(); }

    UecPullPacket* pull(UecBasePacket::pull_quanta& extra_credit);

    bool     shouldSack();
    uint16_t unackedPackets();
    void     setEndTrigger(Trigger& trigger);

    UecBasePacket::seq_t sackBitmapBase(UecBasePacket::seq_t epsn);
    UecBasePacket::seq_t sackBitmapBaseIdeal();
    uint64_t             buildSackBitmap(UecBasePacket::seq_t ref_epsn);
    UecAckPacket*        sack(uint16_t             path_id,
                              UecBasePacket::seq_t seqno,
                              UecBasePacket::seq_t acked_psn,
                              bool                 ce,
                              bool                 rtx);

    UecNackPacket* nack(uint16_t path_id, UecBasePacket::seq_t seqno);

    UecBasePacket::pull_quanta backlog() {
        if (_highest_pull_target > _latest_pull)
            return _highest_pull_target - _latest_pull;
        else
            return 0;
    }

    UecBasePacket::pull_quanta slowCredit() {
        if (_highest_pull_target >= _latest_pull)
            return 0;
        else
            return _latest_pull - _highest_pull_target;
    }

    UecBasePacket::pull_quanta rtx_backlog() { return _retx_backlog; }

    const Stats& stats() const { return _stats; }

    void registerMetrics();
    void logMetricSink();
    void connectPort(uint32_t port_num, UecSrc& src, const Route& routeback);

    const Route* getPortRoute(uint32_t port_num) const;

    UecSinkPort* getPort(uint32_t port_num);

    void setSrc(uint32_t s) { _srcaddr = s; }

    void resetLBToECMP();
    bool backgroundECMPFlow = false;

    inline void setFlowId(flowid_t flow_id) { _flow.set_flowid(flow_id); }

    inline bool inPullQueue() const { return _in_pull; }

    inline bool inSlowPullQueue() const { return _in_slow_pull; }

    inline void addToPullQueue() { _in_pull = true; }

    inline void removeFromPullQueue() { _in_pull = false; }

    inline void addToSlowPullQueue() {
        _in_pull      = false;
        _in_slow_pull = true;
    }

    inline void removeFromSlowPullQueue() {
        _in_pull      = false;
        _in_slow_pull = false;
    }

    AtlahsHtsimApi *_atlahs_api = nullptr;
    graph_node_properties *lgs_node; // used for logging purposes
    uint64_t send_size = 0;
    uint32_t from_sink = -1;
    uint32_t to_sink = -1;
    uint32_t tag_sink;
    simtime_picosec last_data_sent_time;

    uint64_t lgs_time;
    uint64_t lgs_starttime;         // only used for MSGs to identify start times
    uint64_t lgs_syncstart;

    uint64_t lgs_ts; /* this is a timestamp that determines the (original) insertion order of 
                  elements in the queue, it is increased for every new element, not for 
                  re-insertions! Needed for correctness. */
    uint64_t lgs_size;						// number of bytes to send, recv, or time to spend in loclop
    uint32_t lgs_target;					// partner for send/recv
    uint32_t lgs_host;            // owning host 
    uint32_t lgs_offset;          // for Parser (to identify schedule element)
    uint32_t lgs_tag;							// tag for send/recv
    uint32_t lgs_handle;          // handle for network layer :-/
    uint8_t lgs_proc;							// processing element for this operation
    uint8_t lgs_nic;							// network interface for this operation
    char lgs_type;							  // see below
    void set_src(uint32_t s) { _srcaddr = s; }

    inline UecNIC* getNIC() const { return &_nic; }

    inline void setPCIeModel(PCIeModel* c) {
        assert(_model_pcie);
        _pcie = c;
    }

    inline void setOversubscribedCC(OversubscribedCC* c) { _receiver_cc = c; }

    uint16_t nextEntropy();

    UecSrc* getSrc() { return _src; }

    uint32_t getMaxCwnd() { return _src->maxWnd(); };

    PCIeModel* pcieModel() const { return _pcie; }

    static mem_b                      _bytes_unacked_threshold;
    static uint16_t                   _mtus_per_pull;
    static UecBasePacket::pull_quanta _credit_per_pull;
    static int                        TGT_EV_SIZE;

    static bool _receiver_oversubscribed_cc;

    // for sink logger
    inline mem_b total_received() const { return _stats.bytes_received; }

    uint32_t reorder_buffer_size();  // count is in packets

    inline UecPullPacer* pullPacer() const { return _pullPacer; }

private:
    uint32_t             _no_of_ports;
    vector<UecSinkPort*> _ports;
    uint32_t             _srcaddr;
    UecNIC&              _nic;
    UecSrc*              _src;
    PacketFlow           _flow;
    UecPullPacer*        _pullPacer;
    UecBasePacket::seq_t _expected_epsn;
    UecBasePacket::seq_t _high_epsn;
    UecBasePacket::seq_t
        _ref_epsn;  // used for SACK bitmap calculation in spec, unused here for NOW.
    UecBasePacket::pull_quanta _retx_backlog;
    UecBasePacket::pull_quanta _latest_pull;
    UecBasePacket::pull_quanta _highest_pull_target;

    bool _in_pull;       // this tunnel is in the pull queue.
    bool _in_slow_pull;  // this tunnel is in the slow pull queue.

    // received payload bytes, used to decide when flow has finished.
    mem_b    _received_bytes;
    uint16_t _accepted_bytes;

    // used to help the sender slide his window.
    uint64_t _recvd_bytes;
    // used for flow control in sender CC mode.
    // decides whether to reduce cwnd at sender; will change dynamically based on receiver resource
    // availability.
    uint8_t _rcv_cwnd_pen;

    std::unordered_map<uint32_t, uint32_t> dropped_map; // Key: flow ID, Value: true if PSN dropped


    Trigger* _end_trigger;
    ModularVector<uint8_t, uecMaxInFlightPkts>
        _epsn_rx_bitmap;  // list of packets above a hole, that we've received

    vector<UecBasePacket::seq_t> _rss_last_subflow_acked_seq_no;
    vector<int32_t>              _rss_last_subflow_acked_entropy;

public:
    // loss detection
    void           processProbe(UecDataPacket& pkt);
    void           pflrSendNack(UecBasePacket::seq_t seq_no, uint16_t ev);
    static int32_t _pflr_scheme_id;
    // Host-only trim/probe validation and coalescing. A trim that is the
    // first loss evidence initializes and marks the existing NACK bitmap, so
    // the later ordinary proactive probe cannot emit a duplicate NACK. A
    // probe promoted into the high-priority header queue is ignored because
    // it no longer proves FIFO ordering behind the named packet.
    static bool _pflr_receiver_trim_probe_coalescing;
    // Ablation-only escape hatch: retain receiver NACK coalescing while
    // processing probes that a congested switch promoted into its header
    // queue. Disabled by default because promotion breaks FIFO evidence.
    static bool _pflr_accept_header_promoted_probes;
    // static bool _pflr_print_debug_msg;
    // static bool _pflr_disable_probe;
    // static bool _pflr_disable_nack;
    // pflr 1 states
    vector<UecDataPacket::seq_t> _pflr1_slots_expect_psn = {};
    // pflr 2 states
    vector<vector<UecDataPacket::seq_t>> _pflr2_slots_start_psn  = {};
    vector<vector<UecDataPacket::seq_t>> _pflr2_slots_expect_psn = {};
    vector<vector<uint16_t>>             _pflr2_slots_generation = {};
    // pflr 3 states
    UecDataPacket::seq_t _pflr3_bitmap_start_psn = 0;
    vector<uint8_t>      _pflr3_receive_bitmap   = {};
    vector<uint8_t>      _pflr3_nack_bitmap      = {};
    // pflr 4 states
    // vector<vector<uint8_t>> _slots_bitmap = {}; // (slot, bitmap)
    // vector<vector<uint16_t>> _slots_bitmap_sections_ev = {}; // ev (bPSN) of each bitmap section
    // enum SectionStatus {EMPTY = 0, ACTIVE = 1};
    // vector<vector<SectionStatus>> _section_status = {};
    // vector<vector<UecDataPacket::seq_t>> _slots_bitmap_sections_start_psn = {}; // start psn of
    // each bitmap section vector<vector<UecDataPacket::seq_t>> _slots_bitmap_sections_end_psn = {};
    // // end psn of each bitmap section vector<vector<UecDataPacket::seq_t>> _slots_bitmap_zpsn =
    // {}; // first zero position = checking window start vector<vector<uint8_t>>
    // _slots_bitmap_is_nack_sent = {}; // if nack is sent on this position vector<vector<uint32_t>>
    // _slots_counter_map = {}; // if nack is sent on this position
private:
    uint32_t _out_of_order_count;
    bool     _ack_request;

    uint16_t _entropy;

    // variables for PCIe model
    PCIeModel*        _pcie;
    OversubscribedCC* _receiver_cc;

    Stats      _stats;
    string     _nodename;
    CsvMetric* _sink_stats;

public:
    static bool _oversubscribed_cc;
    static bool _model_pcie;
};
