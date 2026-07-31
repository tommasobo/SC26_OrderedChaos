// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef UEC_H
#define UEC_H

#include <list>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "circular_buffer.h"
#include "data_receiver.h"
#include "event_source.h"
#include "eventlist.h"
#include "metric.h"
#include "modular_vector.h"
#include "oversubscribed_cc.h"
#include "packet_sink.h"
#include "pcie_model.h"
#include "trigger.h"
#include "uec_nic.h"
#include "uec_packet.h"
#include "rack_tlp.h"
#include "../atlahs_event.h"
#include "../atlahs_htsim_api.h"

#define timeInf 0
// min RTO bound in us
//  *** don't change this default - override it by calling UecSrc::setMinRTO()
#define DEFAULT_UEC_RTO_MIN 100

class UecPullPacer;
class UecSink;
class UecLogger;
class UecSrcPort;

class UecSrc : public EventSource, public TriggerTarget {
public:
    struct Stats {
        /* all must be non-negative, but we'll make them signed so we
           can do maths with them without concern about underflow */
        int32_t new_pkts_sent;
        int32_t rtx_pkts_sent;
        int32_t rts_pkts_sent;
        int32_t rto_events;
        int32_t acks_received;
        int32_t nacks_received;
        int32_t pulls_received;
        int32_t bounces_received;
        int32_t rts_nacks;
        int32_t _sleek_counter;
    };

    struct FlowBenderStats {
        uint16_t _current_consecutive_congested_rtt;  // Number of consecutive congested rounds (to
                                                      // be compared with
                                                      // _flowbender_params.consecutive_rtt_number)
        uint16_t _current_rtt_ecn_packet_count;  // Count of ECN marked packets within one round
        uint16_t _number_of_packets_in_current_round;  // Counter to know when a rouund (or one RTT
                                                       // finishes)
        simtime_picosec last_update;                   // Timestamp of last update
        uint16_t        _entropy;
    };

    UecSrc(TrafficLogger* trafficLogger,
           EventList&     eventList,
           UecNIC&        nic,
           uint32_t       no_of_ports,
           bool           rts = false);
    void delFromSendTimes(simtime_picosec time, UecDataPacket::seq_t seq_no);

    // Registers the metrics for the flow using the DataCollector singleton.
    enum CCEventType {
        QUICK_ADAPT,
        FAST_INCREASE,
        FAIR_DECREASE,
        PROPORTIONAL_DECREASE,
        FAIR_INCREASE,
        PROPORTIONAL_INCREASE,
        NACK_DECREASE,
        TIMEOUT_DECREASE,
        NO_CHANGE,
        ADDITIVE_INCREASE,
        MULTIPLICATIVE_DECREASE,
    };


    // RSS no-ACK reroute state
    bool is_ecmp_bg = false;
    simtime_picosec _rss_noack_timeout = timeFromUs(100u); // tune: e.g., = _rto or k * _base_rtt
    std::vector<simtime_picosec> _rss_last_feedback_time;
    std::vector<simtime_picosec> _rss_noack_deadline;
    std::vector<simtime_picosec> _rss_last_reroute_time;
    simtime_picosec _rss_reroute_cooldown = timeFromUs(100u); // prevent thrashing

    int rssSubflowFromEv(uint16_t path_id) const;
    void rssBumpEntropyForSubflow(int sid, const char* reason);

    // Register all the metrics that are gonna be collected for this object.
    void registerMetrics();
    /*
    Register per flow metrics (flow size, flow start time, flow end time, flow completion time, base
    rtt, target rtt, rto, bdp and max cwnd)
    */
    void logMetricFlow();
    // For each receiving packet at the sender, logs: rtt, ackedBytes, isNack and if the packet has
    // ECN.
    void logMetricAck(simtime_picosec rtt, int ackedBytes, bool isNack, bool hasECN);
    // For each change in the cwnd, log its value and the event that triggered it.
    void logMetricCCEvent(CCEventType cc_action, uint64_t cwnd);
    // For each change in the cwnd, log its value.
    void logMetricCwnd(uint64_t cwnd, simtime_picosec raw_rtt);
    // If using RSS, for every period, record all RSS metrics before processing the update
    void logMetricRssSubflow(
        vector<simtime_picosec> path_feedback_rtt,
        vector<simtime_picosec> path_feedback_worse_rtt,
        vector<float>           path_feedback_ecn,
        vector<uint16_t>        entropy,
        vector<uint16_t>        packet_count);  // ECN feedback is the fraction of packets marked
    // If using FlowBender, record metrics once per RTT
    void logMetricFlowBender(UecSrc::FlowBenderStats);

    void logMetricUssSubflow(vector<simtime_picosec> path_feedback_mean_rtt,
                             vector<simtime_picosec> path_feedback_worse_rtt,
                             vector<float>           path_feedback_ecn,
                             vector<uint16_t>        entropy,
                             vector<uint16_t>        packet_count);

    static void disableFairDecrease();
    /**
     * Initialize global NSCC parameters.
     */
    static void initNsccParams(simtime_picosec network_rtt,
                               linkspeed_bps   linkspeed,
                               simtime_picosec target_Qdelay);
    /**
     * Initialize per-connection NSCC parameters.
     */
    void initNscc(mem_b cwnd, simtime_picosec peer_rtt = UecSrc::_network_rtt);
    /**
     * Initialize per-connection RCCC parameters.
     */
    void initRccc(mem_b cwnd, simtime_picosec peer_rtt = UecSrc::_network_rtt);

    void logFlowEvents(FlowEventLogger& flow_logger) { _flow_logger = &flow_logger; }

    virtual void connectPort(
        uint32_t portnum, Route& routeout, Route& routeback, UecSink& sink, simtime_picosec start);

    const Route* getPortRoute(uint32_t port_num) const;

    UecSrcPort* getPort(uint32_t port_num) { return _ports[port_num]; }

    void timeToSend(const Route& route);
    void receivePacket(Packet& pkt, uint32_t portnum);
    void doNextEvent();

    void setSrc(uint32_t src) { _srcaddr = src; }

    void setDst(uint32_t dst) { _dstaddr = dst; }

    void resetLBToECMP();
    bool backgroundECMPFlow = false;


    AtlahsHtsimApi *_atlahs_api = nullptr;
    std::unique_ptr<graph_node_properties> lgs_node; // used for logging purposes
    uint64_t send_size = 0;
    uint32_t from = -1;
    uint32_t to = -1;
    uint32_t tag;
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
    void setFlowSize(uint64_t flow_size) { _flow_size = flow_size; }
    void set_dst(uint32_t dst) { _dstaddr = dst; }
    simtime_picosec last_data_sent_time;

    std::string getSrcDstFlowid() { return _src_dst_flowid; }

    static void setMinRTO(uint32_t min_rto_in_us) {
        _min_rto = timeFromUs((uint32_t)min_rto_in_us);
    }

    static void set_use_exp_avg_ecn(bool value) { use_exp_avg_ecn = value; }

    static bool get_use_exp_avg_ecn() { return use_exp_avg_ecn; }

    static void set_fast_increase_scaling_factor(double value) {
        fast_increase_scaling_factor = value;
    }

    static void set_prop_increase_scaling_factor(double value) {
        prop_increase_scaling_factor = value;
    }

    static void set_fair_increase_scaling_factor(double value) {
        fair_increase_scaling_factor = value;
    }

    static void set_fair_decrease_scaling_factor(double value) {
        fair_decrease_scaling_factor = value;
    }

    static void set_mult_decrease_scaling_factor(double value) {
        mult_decrease_scaling_factor = value;
    }

    static double get_fast_increase_scaling_factor() { return fast_increase_scaling_factor; }

    static double get_prop_increase_scaling_factor() { return prop_increase_scaling_factor; }

    static double get_fair_increase_scaling_factor() { return fair_increase_scaling_factor; }

    static double get_fair_decrease_scaling_factor() { return fair_decrease_scaling_factor; }

    static double get_mult_decrease_scaling_factor() { return mult_decrease_scaling_factor; }

    void setCwnd(mem_b cwnd) {
        //_maxwnd = cwnd;
        _cwnd = cwnd;
    }

    void setMaxWnd(mem_b maxwnd) {
        //_maxwnd = cwnd;
        _maxwnd = maxwnd;
    }

    void boundBaseRTT(simtime_picosec network_rtt) {
        _base_rtt = network_rtt;
        _bdp      = timeAsUs(_base_rtt) * _nic.linkspeed() / 8000000;
        _maxwnd   = 1.5 * _bdp;

        if (!_shown) {
            cout << "Bound base RTT: _bdp " << _bdp << " _maxwnd " << _maxwnd << " _base_rtt "
                 << timeAsUs(_base_rtt) << endl;
            _shown = true;
        }
    }

    mem_b maxWnd() const { return _maxwnd; }

    const Stats& stats() const { return _stats; }

    void setEndTrigger(Trigger& trigger);
    // called from a trigger to start the flow.
    virtual void           activate();
    static uint32_t        _path_entropy_size;  // now many paths do we include in our path set
    static int             _global_node_count;
    static simtime_picosec _min_rto;
    static uint16_t        _hdr_size;
    static uint16_t        _mss;  // does not include header
    static uint16_t        _mtu;  // does include header

    static bool _sender_based_cc;
    static bool _receiver_based_cc;

    enum Sender_CC {
        DCTCP,
        NSCC,
        CONSTANT,
        SMARTT,
        SMARTT_ECN_AIMD,
        SMARTT_ECN_AIFD,
        SMARTT_ECN_FIMD,
        SMARTT_ECN_FIFD,
        SMARTT_RTT
    };

    enum LoadBalancing_Algo { BITMAP, FLOWLET, REPS, OBLIVIOUS, MIXED, RSS, ECMP, FLOWBENDER, USS };

    enum RSSWorseEntropyMetric { MEAN_RTT, WORSE_RTT, ECN };

    enum PathFeedbackBit { PATH_GOOD, PATH_ECN, PATH_NACK, PATH_TIMEOUT };

    struct PathFeedback {
        PathFeedbackBit feedback_bit;
        simtime_picosec rtt_estimate;
    };

    enum EvState { STATE_GOOD, STATE_SKIP, STATE_ASSUMED_BAD };

    static Sender_CC          _sender_cc_algo;
    static LoadBalancing_Algo _load_balancing_algo;

    static bool _enable_qa_gate;

    static bool _enable_sleek;

    // RACK-TLP (RFC 8985)
    static RackTlpMode _rack_tlp_mode;
    static std::string _rack_tlp_log_dir;
    static bool _tlp_confirmed_loss_cwnd;

    virtual const string& nodename() { return _nodename; }

    inline void setFlowId(flowid_t flow_id) { _flow.set_flowid(flow_id); }

    void setFlowsize(uint64_t flow_size_in_bytes);

    mem_b flowsize() { return _flow_size; }

    inline PacketFlow* flow() { return &_flow; }

    inline flowid_t flowId() const { return _flow.flow_id(); }

    static bool _debug;
    static bool _trace_rtx;
    // Emit the legacy human-readable Drop:/RTX: event stream used by
    // reaction-time analyses.  Disabled by default because it can produce
    // multi-gigabyte logs for collective workloads.
    static bool _log_reaction_events;
    static bool _shown;
    bool        _debug_src;

    bool debug() const { return _debug_src; }

private:
    UecNIC&             _nic;
    uint32_t            _no_of_ports;
    vector<UecSrcPort*> _ports;

    struct sendRecord {
        // need a constructor to be able to put this in a map
        sendRecord(mem_b psize, simtime_picosec stime) : pkt_size(psize), send_time(stime){};
        mem_b           pkt_size;
        simtime_picosec send_time;
    };

    UecLogger*       _logger;
    TrafficLogger*   _pktlogger;
    FlowEventLogger* _flow_logger;
    Trigger*         _end_trigger;

    // TODO in-flight packet storage - acks and sacks clear it
    // list<UecDataPacket*> _activePackets;

    // we need to access the in_flight packet list quickly by sequence number, or by send time.
    map<UecDataPacket::seq_t, sendRecord>           _tx_bitmap;
    multimap<simtime_picosec, UecDataPacket::seq_t> _send_times;
    map<UecDataPacket::seq_t, int>                  _rtx_times;

    map<UecDataPacket::seq_t, mem_b> _rtx_queue;

    void  startFlow();
    bool  isSpeculative();
    void  sendIfPermitted();
    mem_b sendPacket(const Route& route);
    mem_b sendNewPacket(const Route& route);
    mem_b sendRtxPacket(const Route& route);
    void  sendRTS();
    void  sendProbe();
    void  createSendRecord(UecDataPacket::seq_t seqno, mem_b pkt_size);
    void  queueForRtx(UecBasePacket::seq_t seqno, mem_b pkt_size);
    void  recalculateRTO();
    void  startRTO(simtime_picosec send_time);
    void  clearRTO();   // timer just expired, clear the state
    void  cancelRTO();  // cancel running timer and clear state
    void  smartt_update_fast_increase_state(bool skip, simtime_picosec delay);
    bool  smartt_should_fast_increase();
    void  smartt_fast_increase();
    bool  smartt_check_and_do_quick_adapt(bool skip, bool trimmed);
    void  smartt_quick_adapt(bool trimmed);
    void  smartt_update_wtd_state(bool skip);
    bool  smartt_wtd_can_decrease();
    void  clamp_cwnd();

    void (UecSrc::*smartt_main_loop)(bool skip, simtime_picosec delay);
    void smartt_vanilla_main_loop(bool skip, simtime_picosec delay);
    void smartt_ecn_aimd_main_loop(bool skip, simtime_picosec delay);
    void smartt_ecn_aifd_main_loop(bool skip, simtime_picosec delay);
    void smartt_ecn_fimd_main_loop(bool skip, simtime_picosec delay);
    void smartt_ecn_fifd_main_loop(bool skip, simtime_picosec delay);
    void smartt_rtt_main_loop(bool skip, simtime_picosec delay);

    // not used, except for debugging timer issues
    void checkRTO() {
        if (_rtx_timeout_pending)
            assert(_rto_timer_handle != eventlist().nullHandle());
        else
            assert(_rto_timer_handle == eventlist().nullHandle());
    }

    void                       rtxTimerExpired();

    // RACK-TLP methods
    void rackOnAckUpdate(UecDataPacket::seq_t acked_psn, simtime_picosec send_time);
    void rackDetectLosses();
    void rackArmReorderTimer();
    int  rackSubflowForSeqno(UecDataPacket::seq_t seqno) const;
    void tlpComputeAndArmPTO();
    void tlpComputeAndArmPTO(int subflow_id);   // per-subflow variant
    void tlpArmEarliestPTO();                    // pick earliest subflow PTO and set timer
    void tlpCancelPTO();
    void tlpSendProbe();
    void tlpSendProbeForSubflow(int subflow_id); // probe a specific subflow
    void scheduleRtxPacedSend();  // pace NACK-triggered retransmissions

    UecBasePacket::pull_quanta computePullTarget();
    void                       handlePull(UecBasePacket::pull_quanta pullno);
    mem_b                      handleAckno(UecDataPacket::seq_t ackno);
    mem_b                      handleCumulativeAck(UecDataPacket::seq_t cum_ack);
    void                       processProbeAck(const UecAckPacket& pkt);
    void                       processAck(const UecAckPacket& pkt);
    void                       processNack(const UecNackPacket& pkt);
    void                       processPull(const UecPullPacket& pkt);
    void                       runSleek(uint32_t ooo, UecBasePacket::seq_t cum_ack);

    // added for NSCC
    bool  can_send_NSCC(mem_b pkt_size);
    void  set_cwnd_bounds();
    mem_b getNextPacketSize();
    void  quick_adapt(bool trimmed);
    void  updateCwndOnAck_NSCC(bool skip, simtime_picosec delay, mem_b newly_acked_bytes);
    void  updateCwndOnNack_NSCC(bool skip, mem_b nacked_bytes);

    void updateCwndOnAck_SMARTT(bool skip, simtime_picosec delay, mem_b newly_acked_bytes);
    void updateCwndOnNack_SMARTT(bool skip, mem_b nacked_bytes);

    void updateCwndOnAck_DCTCP(bool skip, simtime_picosec delay, mem_b newly_acked_bytes);
    void updateCwndOnNack_DCTCP(bool skip, mem_b nacked_bytes);

    void dontUpdateCwndOnAck(bool skip, simtime_picosec delay, mem_b newly_acked_bytes);
    void dontUpdateCwndOnNack(bool skip, mem_b nacked_bytes);

    void (UecSrc::*updateCwndOnAck)(bool skip, simtime_picosec delay, mem_b newly_acked_bytes);
    void (UecSrc::*updateCwndOnNack)(bool skip, mem_b nacked_bytes);

    uint16_t nextEntropy_bitmap(UecBasePacket::seq_t seq_no);
    uint16_t nextEntropy_REPS(UecBasePacket::seq_t seq_no);
    uint16_t nextEntropy_oblivious(UecBasePacket::seq_t seq_no);
    uint16_t nextEntropy_mixed(UecBasePacket::seq_t seq_no);
    uint16_t nextEntropy_rss(UecBasePacket::seq_t seq_no);
    uint16_t nextEntropy_ecmp(UecBasePacket::seq_t seq_no);
    uint16_t nextEntropy_flowbender(UecBasePacket::seq_t seq_no);
    uint16_t nextEntropy_uss(UecBasePacket::seq_t seq_no);
    uint16_t nextEntropy_flowlet(UecBasePacket::seq_t seq_no);

    void processEv_bitmap(uint16_t path_id, PathFeedback path_feedback);
    void processEv_REPS(uint16_t path_id, PathFeedback path_feedback);
    void processEv_oblivious(uint16_t path_id, PathFeedback path_feedback);
    void processEv_mixed(uint16_t path_id, PathFeedback path_feedback);
    void processEv_rss(uint16_t path_id, PathFeedback path_feedback);
    void processEv_ecmp(uint16_t path_id, PathFeedback path_feedback);
    void processEv_flowbender(uint16_t path_id, PathFeedback path_feedback);
    void processEv_uss(uint16_t path_id, PathFeedback path_feedback);
    void processEv_flowlet(uint16_t path_id, PathFeedback feedback);

    inline EvState ev_state(uint16_t path) const {
        if (_ev_skip_bitmap[path] == 0)
            return STATE_GOOD;
        else if (_ev_skip_bitmap[path] == _max_penalty)
            return STATE_ASSUMED_BAD;
        else
            return STATE_SKIP;
    }

    uint16_t (UecSrc::*nextEntropy)(UecBasePacket::seq_t sequence_number);
    void (UecSrc::*processEv)(uint16_t path_id, PathFeedback path_feedback);

    bool checkFinished(UecDataPacket::seq_t cum_ack);

    Stats    _stats;
    UecSink* _sink;


    int flowlet_entropy = 0;
    simtime_picosec last_pkt_flowlet = 0;
    simtime_picosec flowlet_timeout = 0;
    simtime_picosec flowlet_timeout_wait = 0;

    // unlike in the NDP simulator, we maintain all the main quantities in bytes
    mem_b                      _flow_size;
    bool                       _done_sending;  // make sure we only trigger once
    mem_b                      _backlog;  // how much we need to send, not including retransmissions
    mem_b                      _rtx_backlog;
    mem_b                      _cwnd;
    mem_b                      _maxwnd;
    UecBasePacket::pull_quanta _pull_target;
    UecBasePacket::pull_quanta _pull;
    mem_b                _credit;  // receive request credit in pull_quanta, but consume it in bytes
    inline mem_b         credit() const;
    void                 stopSpeculating();
    void                 spendCredit(mem_b pktsize);
    UecDataPacket::seq_t _highest_sent;
    UecDataPacket::seq_t _highest_rtx_sent;
    mem_b                _in_flight;
    mem_b                _bdp;
    bool                 _send_blocked_on_nic;
    bool                 _speculating;

    // Original SMaRTT extra parameters
    bool          need_quick_adapt  = false;
    double        exp_avg_ecn_value = 0.3;
    double        exp_avg_alpha     = 0.05;
    double        exp_avg_ecn       = 0;
    uint32_t      target_window;
    static bool   use_exp_avg_ecn;
    uint32_t      counter_consecutive_good_bytes = 0;
    bool          increasing                     = false;
    static double fast_increase_scaling_factor;
    static double prop_increase_scaling_factor;
    static double fair_increase_scaling_factor;
    static double fair_decrease_scaling_factor;
    static double mult_decrease_scaling_factor;
    static double target_rtt_scaling_factor;
    static bool   use_fast_increase;
    uint64_t      previous_window_end = 0;
    uint32_t      saved_acked_bytes   = 0;

public:
    static linkspeed_bps   _reference_network_linkspeed;
    static simtime_picosec _reference_network_rtt;
    static mem_b           _reference_network_bdp;
    static linkspeed_bps   _network_linkspeed;
    static simtime_picosec _network_rtt;
    static mem_b           _network_bdp;
    // Smarttrack parameters
    static mem_b           _min_cwnd;
    static uint32_t        _qa_scaling;
    static simtime_picosec _target_Qdelay;
    static double          _gamma;
    static double          _alpha;
    // static double _scaling_c;
    // static double _fd;
    static double _fi;
    static double _fi_scale;
    simtime_picosec last_ack_sent_time;

    struct RSSParams {
        uint16_t              _rss_number_of_subflows;
        simtime_picosec       _rss_update_interval;
        RSSWorseEntropyMetric _rss_worse_entropy_metric;
        int _rss_number_of_subflow_bits;  // the number of bits to keep in the entropy for the
                                          // subflow ID (should always be equal to 32 -
                                          // __builtin_clz(_rss_number_of_subflows - 1))
        double threshold;  // For frozen_rss, threshold of either rtt or ecn% underwhich the update
                           // is skipped. 0 for normal RSS
        uint32_t max_number_of_rounds_to_skip;  // For random_skip, how many rounds to skip max. 0
                                                // for normal RSS
        uint32_t period_jitter;
    };

    struct USSParams {
        uint16_t _number_of_subflows;
        int _number_of_subflow_bits;  // the number of bits to keep in the entropy for the subflow
                                      // ID (should always be equal to 32 -
                                      // __builtin_clz(_rss_number_of_subflows - 1))
    };

    struct FlowBenderParams {
        double   _ecn_threshold;
        uint16_t consecutive_rtt_number;
    };
    static struct RSSParams _rss_params;
    static struct USSParams _uss_params;
    static uint16_t
        ecmp_background_traffic_nodes;  // ID of the first src node to use the load balancing algo
                                        // specifiec by _load_balancing_algo. All those before use
                                        // ECMP
    static struct FlowBenderParams _flowbender_params;
    static int                     USS_LOG_FREQUENCY;
    static double                  _scaling_factor_a;
    static double                  _scaling_factor_b;
    static double                  _eta;
    static double                  _qa_threshold;
    static double                  _delay_alpha;
    // static double _ecn_thresh;
    static uint32_t        _adjust_bytes_threshold;
    static simtime_picosec _adjust_period_threshold;
    // debug
    static flowid_t _debug_flowid;

private:
    bool quick_adapt(bool is_loss, simtime_picosec avgqdelay);
    void fair_increase(uint32_t newly_acked_bytes);
    void proportional_increase(uint32_t newly_acked_bytes, simtime_picosec delay);
    void fast_increase(uint32_t newly_acked_bytes, simtime_picosec delay);
    // void fair_decrease(bool can_decrease, uint32_t newly_acked_bytes);
    void            multiplicative_decrease(uint32_t newly_acked_bytes);
    void            fulfill_adjustment();
    void            mark_packet_for_retransmission(UecBasePacket::seq_t psn, uint16_t pktsize);
    void            resolve_tlp_ack(TlpState& tlp,
                                    UecDataPacket::seq_t acked_psn,
                                    bool rtx_echo);
    void            update_delay(simtime_picosec delay, bool update_avg, bool skip);
    void            update_base_rtt(simtime_picosec raw_rtt, uint16_t packet_size);
    simtime_picosec get_avg_delay();
    uint16_t        get_avg_pktsize();

    // entropy value calculation
    uint16_t _no_of_paths;       // must be a power of 2
    uint16_t _path_random;       // random upper bits of EV, set at startup and never changed
    uint16_t _path_xor;          // random value set each time we wrap the entropy values - XOR with
                                 // _current_ev_index
    uint16_t _current_ev_index;  // count through _no_of_paths and then wrap.  XOR with _path_xor to
                                 // get EV
    vector<uint8_t>         _ev_skip_bitmap;  // paths scores for load balancing
    simtime_picosec         _rss_next_update_time;
    simtime_picosec         _rss_jitter_range;
    vector<simtime_picosec> _rss_state_mean_rtt;  // measured sum of RTTs for visited paths (one per
                                                  // subflow); used for mean RTT computation
    vector<simtime_picosec>
                     _rss_state_worse_rtt;  // measured RTTs for visited paths (one per subflow)
    vector<int>      _rss_state_ecn;        // measured RTTs for visited paths (one per subflow)
    vector<uint16_t> _rss_state_entropies;  // past entropy values (one per subflow)
    vector<uint16_t> _rss_state_number_of_ev_pkts;   // number of packets since last entropy change
    uint32_t         _rss_number_of_rounds_to_skip;  // For random_skip
    FlowBenderStats  _flowbender_stats;
    uint8_t          _max_penalty;  // max value we allow in _path_penalties (typically 1 or 2).
    uint16_t         _ev_skip_count;
    uint16_t         _ev_bad_count;

public:
    // loss detection
    static bool     _enable_precise_fast_loss_recovery;
    static int32_t  _pflr_scheme_id;
    static bool     _pflr_print_debug_msg;
    static bool     _pflr_disable_probe;
    static bool     _pflr_disable_nack;
    static bool     _pflr_proactive_probe;
    static int      _pflr_proactive_probe_pkt_count;
    // proactive probing
    vector<simtime_picosec>   _pflr_slots_scheduled_probe_send_time;
    vector<EventList::Handle> _pflr_slots_probe_send_handle;
    vector<uint8_t>           _pflr_slots_probe_is_pending;
    uint8_t                   _pflr_probe_is_bootstrapped;
    static bool               _pflr_proactive_rtx_probe;
    static bool               _pflr_pace_rtx;  // pace retransmissions based on cwnd/RTT
    static double             _pflr_rtx_jitter_ratio;  // random initial delay = U[0, ratio*SRTT] before first RTX
    static uint32_t           _pflr4_no_packet_per_slot;
    static bool               _pflr4_use_ev_recovery;
    static uint32_t           _pflr5_counter_map_bit_count;
    // pflr 0 stuff
    unordered_map<uint16_t, vector<UecBasePacket::seq_t>> pflr0_sender_record;
    void pflr0RegisterSend(UecBasePacket::seq_t seq_no, uint16_t ev);
    void pflr0SenderReceiveAck(UecBasePacket::seq_t seq_no, uint16_t ev, bool is_probe_ack);
    // pflr 4 stuff
    UecDataPacket::seq_t            _pflr4_init_highest_sent      = 0;
    vector<vector<uint16_t>>        _pflr4_slots_ev_queue         = {};
    vector<vector<uint16_t>>        _pflr4_slots_ev_map_old_ev    = {};
    vector<vector<uint16_t>>        _pflr4_slots_ev_map_new_ev    = {};
    vector<vector<simtime_picosec>> _pflr4_slots_ev_map_timeout   = {};
    vector<vector<int32_t>>         _pflr4_slots_ev_map_countdown = {};
    void                            pflr4PrintEvMap(uint16_t slot_id);
    uint32_t                        pflr4GetStageId();
    void                            pflr4CleanUpTimeout(uint16_t slot_id);
    uint16_t                        pflr4LookupEv(uint16_t slot_id, uint16_t old_ev);
    void                            pflr4SenderReceiveEv(UecBasePacket::seq_t seq_no, uint16_t ev);
    void pflr4RegisterTranslation(uint16_t slot_id, uint16_t old_ev, uint16_t new_ev);
    void pflr4RegisterRto(UecBasePacket::seq_t seq_no);
    bool pflr4IsRtoRtx(UecBasePacket::seq_t seq_no);
    unordered_set<UecBasePacket::seq_t> _pflr4_rto_rtx_table;
    vector<uint16_t>                    _current_evs  = {};  // current sending EVs of all slots
    vector<uint16_t>                    _previous_evs = {};  // previous sending EVs of all slots

    enum EvStatus { UN_INITIALIZED = 0, INITIALIZED = 1, IS_NEW = 2 };

    vector<EvStatus>                             _ev_status                       = {};
    vector<pair<UecBasePacket::seq_t, uint16_t>> _probe_queue                     = {};  // psn, ev
    vector<UecDataPacket::PflrProbeType>         _probe_type_queue                = {};
    vector<simtime_picosec>                      _slots_last_proactive_probe_time = {};
    vector<UecBasePacket::seq_t>                 _slots_last_proactive_probe_psn  = {};
    vector<pair<UecBasePacket::seq_t, uint16_t>> _slots_last_data_packet_info     = {};
    void                                         enqueueProbe(UecBasePacket::seq_t         seq_no,
                                                              uint16_t                     ev,
                                                              UecDataPacket::PflrProbeType probe_type);
    void                                         enqueueDynamicProactiveProbe();
    mem_b                                        sendProbePacket(const Route& route);
    static uint16_t                              getNoSlots();
    static bool                                  usePflr();

    bool rtx_queue_empty() {
        return _rtx_queue.empty();
    }
    simtime_picosec _last_send_time;

    // RACK-TLP per-flow state
    RackState     _rack;                         // single-state (non-RSS or fallback)
    std::vector<RackState> _rack_per_subflow;    // per-subflow state for RSS (Falcon-style)
    TlpState      _tlp;                          // single TLP state (non-RSS fallback)
    std::vector<TlpState> _tlp_per_subflow;      // per-subflow TLP state for RSS
    RackTlpStats  _rack_stats;
    RackTlpLogger _rack_logger;
    bool          _tlp_pto_pending = false;
    simtime_picosec _tlp_pto_timeout = 0;
    EventList::Handle _tlp_pto_handle;
    int           _tlp_pto_subflow = -1;         // which subflow the current PTO timer is for

    // PFLD RTX pacing state
    bool              _rtx_pace_pending = false;
    bool              _proactive_tail_check_scheduled = false;
    int               _proactive_tail_rounds = 0;
    simtime_picosec   _rtx_pace_timer   = 0;     // when the pacing timer is set to fire
    simtime_picosec   _rtx_pace_next    = 0;     // next pacing slot
    EventList::Handle _rtx_pace_handle;


private:
    // RTT estimate data for RTO and sender based CC.
    simtime_picosec   _rtt, _mdev, _rto, _raw_rtt;
    bool              _rtx_timeout_pending;  // is the RTO running?
    simtime_picosec   _rto_send_time;  // when we sent the oldest packet that the RTO is waiting on.
    simtime_picosec   _rtx_timeout;    // when the RTO is currently set to expire
    simtime_picosec   _last_rts;       // time when we last sent an RTS (or zero if never sent)
    EventList::Handle _rto_timer_handle;

    // used to drive ACK clock
    uint64_t _recvd_bytes;

    // Smarttrack sender based CC variables.
    simtime_picosec _base_rtt;
    mem_b           _base_bdp;
    mem_b           _achieved_bytes = 0;
    // used to trigger SmartTrack fulfill
    mem_b           _received_bytes  = 0;
    uint32_t        _fi_count        = 0;
    bool            _trigger_qa      = false;
    simtime_picosec _qa_endtime      = 0;
    uint32_t        _bytes_to_ignore = 0;
    uint32_t        _bytes_ignored   = 0;
    uint32_t        _inc_bytes       = 0;
    simtime_picosec _avg_delay       = 0;

    simtime_picosec _last_eta_time    = 0;
    simtime_picosec _last_adjust_time = 0;
    bool            _increase         = false;
    simtime_picosec _last_dec_time    = 0;
    uint32_t        _highest_recv_seqno;
    bool            _loss_recovery_mode = false;
    uint32_t        _recovery_seqno     = 0;
    uint32_t        _loss_counter       = 0;

    simtime_picosec   _probe_timer_when = 0;
    simtime_picosec   _probe_seqno      = 0;
    simtime_picosec   _probe_send_time  = 0;
    EventList::Handle _probe_timer_handle;

    uint16_t       _crt_path;
    list<uint16_t> _next_pathid;
    list<uint16_t> _knowngood_pathid;

    // Connectivity
    PacketFlow      _flow;
    simtime_picosec _flow_start_time;
    int _lost_packets = 0;
    string          _nodename;
    int             _node_num;
    uint32_t        _srcaddr;
    uint32_t        _dstaddr;

    // Data collection for the flow.
    std::string               _src_dst_flowid;       // Identifier for the flow.
    CsvMetric*                _flow_metric;          // Logs the flow metrics of a given flow.
    TimeSeriesMetric*         _ack_metric;           // Logs the metrics when receiving an ACK/NACK.
    TimeSeriesMetric*         _cc_event_metric;      // Logs when a certain CC event happens
    TimeSeriesMetric*         _cwnd_metric;          // Logs when cwnd changes
    vector<TimeSeriesMetric*> _rss_subflow_metrics;  // Logs on updates
    vector<TimeSeriesMetric*> _uss_subflow_metrics;  // Log every some amount of packets
    TimeSeriesMetric*         _flowbender_metrics;   // Log every RTT
};
#endif  // UEC_H
