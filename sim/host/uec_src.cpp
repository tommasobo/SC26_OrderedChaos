// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "uec_src.h"

#include <math.h>

#include <cstdint>

#include "circular_buffer.h"
#include "data_collector.h"
#include "pcie_model.h"
#include "uec_config.h"
#include "uec_logger.h"
#include "uec_pull_pacer.h"
#include "uec_sink.h"
#include "uec_src_port.h"

using namespace std;

// Static stuff
flowid_t UecSrc::_debug_flowid = UINT32_MAX;
// _path_entropy_size is the number of paths we spray across.  If you don't set it, it will default
// to all paths.
uint32_t UecSrc::_path_entropy_size = 256;
int      UecSrc::_global_node_count = 0;
bool     UecSrc::_shown             = false;

/* _min_rto can be tuned using setMinRTO. Don't change it here.  */
simtime_picosec UecSrc::_min_rto = timeFromUs((uint32_t)DEFAULT_UEC_RTO_MIN);

/* this default will be overridden from packet size*/
uint16_t UecSrc::_hdr_size = 64;
uint16_t UecSrc::_mss      = 4096;
uint16_t UecSrc::_mtu      = _mss + _hdr_size;

bool UecSrc::_debug = false;
bool UecSrc::_trace_rtx = false;
bool UecSrc::_log_reaction_events = false;

bool UecSrc::_sender_based_cc   = false;
bool UecSrc::_receiver_based_cc = false;

UecSrc::Sender_CC          UecSrc::_sender_cc_algo      = UecSrc::NSCC;
UecSrc::LoadBalancing_Algo UecSrc::_load_balancing_algo = UecSrc::BITMAP;

linkspeed_bps   UecSrc::_reference_network_linkspeed = 0;  // set by initNsccParams
simtime_picosec UecSrc::_reference_network_rtt       = timeFromUs(12u);
mem_b           UecSrc::_reference_network_bdp       = 0;  // set by initNsccParams
linkspeed_bps   UecSrc::_network_linkspeed           = 0;  // set by initNsccParams
simtime_picosec UecSrc::_network_rtt                 = 0;  // set by initNsccParams
mem_b           UecSrc::_network_bdp                 = 0;  // set by initNsccParams
double   UecSrc::_scaling_factor_a = 1;  // for 400Gbps. cf. spec must be set to BDP/(100Gbps*12us)
double   UecSrc::_scaling_factor_b = 0;  // Needs to be inialized in initNscc
uint32_t UecSrc::_qa_scaling =
    1;  // quick adapt scaling - how much of the achieved bytes should we use as new CWND?
double UecSrc::_gamma    = 0.8;  // used for aggressive decrease
double UecSrc::_alpha    = UecSrc::_scaling_factor_a * 1000 * 4000 / timeFromUs(6u);
double UecSrc::_fi       = 1;  // fair_increase constant
double UecSrc::_fi_scale = .25 * UecSrc::_scaling_factor_a;
mem_b  UecSrc::_min_cwnd = 0;

UecSrc::RSSParams        UecSrc::_rss_params = {8, timeFromUs(200.), UecSrc::MEAN_RTT, 3, 0, 0, 25};
UecSrc::FlowBenderParams UecSrc::_flowbender_params            = {.05, 1};
UecSrc::USSParams        UecSrc::_uss_params                   = {8, 3};
uint16_t                 UecSrc::ecmp_background_traffic_nodes = 0;
int                      UecSrc::USS_LOG_FREQUENCY             = 10;

double UecSrc::_delay_alpha = 0.0125;  // 0.125;

simtime_picosec UecSrc::_adjust_period_threshold = timeFromUs(12u);
simtime_picosec UecSrc::_target_Qdelay           = timeFromUs(6u);
uint32_t        UecSrc::_adjust_bytes_threshold =
    (simtime_picosec)32000 * _target_Qdelay / timeFromUs(12u);
double UecSrc::_qa_threshold = 4 * UecSrc::_target_Qdelay;

double   UecSrc::_eta                               = 0;
bool     UecSrc::_enable_qa_gate                    = false;
bool     UecSrc::_enable_sleek                      = false;
bool     UecSrc::_enable_precise_fast_loss_recovery = false;
int32_t  UecSrc::_pflr_scheme_id                    = -1;
bool     UecSrc::_pflr_print_debug_msg              = false;
bool     UecSrc::_pflr_disable_probe                = false;
bool     UecSrc::_pflr_disable_nack                 = false;
bool     UecSrc::_pflr_proactive_probe              = false;
int      UecSrc::_pflr_proactive_probe_pkt_count    = -1;
bool     UecSrc::_pflr_proactive_rtx_probe          = false;
bool     UecSrc::_pflr_pace_rtx                    = false;
double   UecSrc::_pflr_rtx_jitter_ratio             = 0.0;
uint32_t UecSrc::_pflr4_no_packet_per_slot          = 32;
bool     UecSrc::_pflr4_use_ev_recovery             = false;
uint32_t UecSrc::_pflr5_counter_map_bit_count       = 2;

// RACK-TLP static defaults
RackTlpMode  UecSrc::_rack_tlp_mode    = RackTlpMode::OFF;
std::string  UecSrc::_rack_tlp_log_dir = "";
bool UecSrc::_tlp_confirmed_loss_cwnd = false;

bool UecSrc::use_exp_avg_ecn = true;
// The params in the comments are those mentioned in smartt sigcomm24
// submission. These produced oscillations (fast increase then mult/fair
// decrease, then fast increase again...), the updated parameters don't.
double UecSrc::fast_increase_scaling_factor = 1;     // 2
double UecSrc::prop_increase_scaling_factor = 2;     // 2
double UecSrc::fair_increase_scaling_factor = 0.02;  // 0.006
double UecSrc::fair_decrease_scaling_factor = 0.5;   // 0.8
double UecSrc::mult_decrease_scaling_factor = 1;     // 2
double UecSrc::target_rtt_scaling_factor    = 1.5;
bool   UecSrc::use_fast_increase            = true;

void UecSrc::initNsccParams(simtime_picosec network_rtt,
                            linkspeed_bps   linkspeed,
                            simtime_picosec target_Qdelay) {
    _reference_network_linkspeed = speedFromGbps(100);
    _reference_network_rtt       = timeFromUs(12u);
    _reference_network_bdp = timeAsSec(_reference_network_rtt) * (_reference_network_linkspeed / 8);

    _network_linkspeed = linkspeed;
    _network_rtt       = network_rtt;
    _network_bdp       = timeAsSec(_network_rtt) * (_network_linkspeed / 8);

    _min_cwnd = _mtu;

    if (target_Qdelay > 0) {
        _target_Qdelay = target_Qdelay;
    } else {
        _target_Qdelay = timeFromUs(6u);
    }

    _qa_threshold = 4 * _target_Qdelay;

    _scaling_factor_a = (double)_network_bdp / (double)_reference_network_bdp;
    _scaling_factor_b = (double)_target_Qdelay / (double)_reference_network_rtt;  // no unit

    _alpha = 4.0 * _scaling_factor_a * _scaling_factor_b * _mss / _target_Qdelay;  // bytes/picosec
    _fi    = 5 * _scaling_factor_a;
    _eta   = 0.15 * _mss * _scaling_factor_a;

    _qa_scaling =
        1;  // quick adapt scaling - how much of the achieved bytes should we use as new CWND?
    _gamma    = 0.8;  // used for aggressive decrease
    _fi_scale = .25 * _scaling_factor_a;

    _delay_alpha = 0.0125;

    _adjust_period_threshold = _reference_network_rtt;
    _adjust_bytes_threshold  = (uint32_t)(16000 * _scaling_factor_b);

    cout << "Initializing static NSCC parameters:" << " _reference_network_linkspeed="
         << _reference_network_linkspeed << " _reference_network_rtt=" << _reference_network_rtt
         << " _reference_network_bdp=" << _reference_network_bdp
         << " _target_Qdelay=" << _target_Qdelay << " _network_linkspeed=" << _network_linkspeed
         << " _network_rtt=" << _network_rtt << " _network_bdp=" << _network_bdp
         << " _qa_threshold=" << _qa_threshold << " _scaling_factor_a=" << _scaling_factor_a
         << " _scaling_factor_b=" << _scaling_factor_b << " _alpha=" << _alpha << " _fi=" << _fi
         << " _eta=" << _eta << " _qa_scaling=" << _qa_scaling << " _gamma=" << _gamma
         << " _fi_scale=" << _fi_scale << " _delay_alpha=" << _delay_alpha
         << " _adjust_period_threshold=" << _adjust_period_threshold
         << " _adjust_bytes_threshold=" << _adjust_bytes_threshold << endl;
}

void UecSrc::initNscc(mem_b cwnd, simtime_picosec peer_rtt) {
    _sender_based_cc = true;
    _base_rtt        = peer_rtt;
    _base_bdp        = timeAsSec(_base_rtt) * (_nic.linkspeed() / 8);
    _bdp             = _base_bdp;
    _cwnd = _base_bdp;
    _maxwnd          = 1.5 * _bdp;
    flowlet_timeout = 0.5 * _base_rtt;
    /* if (cwnd == 0) {
        _cwnd = _maxwnd;
    } else {
        _cwnd = cwnd;
    } */

    if (is_ecmp_bg) {
        nextEntropy = &UecSrc::nextEntropy_ecmp;
        processEv   = &UecSrc::processEv_ecmp;
        //_load_balancing_algo = UecSrc::ECMP;
        _crt_path   = 0;
    }

    /* cout << "Initialize per-instance NSCC parameters:" << " flowid " << _flow.flow_id()
         << " _base_rtt=" << _base_rtt << " _base_bdp=" << _base_bdp << " _bdp=" << _bdp
         << " _min_cwnd=" << _min_cwnd << " _maxwnd=" << _maxwnd << " _cwnd=" << _cwnd << endl; */
}

void UecSrc::initRccc(mem_b cwnd, simtime_picosec peer_rtt) {
    _receiver_based_cc = true;
    _base_rtt          = peer_rtt;
    _base_bdp          = timeAsSec(_base_rtt) * (_nic.linkspeed() / 8);
    _bdp               = _base_bdp;
    _maxwnd            = 1.5 * _bdp;
    if (cwnd == 0) {
        _cwnd = _maxwnd;
    } else {
        _cwnd = cwnd;
    }

    cout << "Initialize per-instance RCCC parameters:" << " flowid " << _flow.flow_id()
         << " _base_rtt=" << _base_rtt << " _base_bdp=" << _base_bdp << " _bdp=" << _bdp
         << " _maxwnd=" << _maxwnd << " _cwnd=" << _cwnd << endl;
}

UecSrc::UecSrc(
    TrafficLogger* trafficLogger, EventList& eventList, UecNIC& nic, uint32_t no_of_ports, bool rts)
    : EventSource(eventList, "uecSrc"), _nic(nic), _flow(trafficLogger) {
    _node_num = _global_node_count++;
    _nodename = "uecSrc " + to_string(_node_num);

    _no_of_ports = no_of_ports;
    _ports.resize(no_of_ports);
    for (uint32_t p = 0; p < _no_of_ports; p++) {
        _ports[p] = new UecSrcPort(*this, p);
    }

    _rtx_timeout_pending = false;
    _rtx_timeout         = timeInf;
    _rto_timer_handle    = eventlist().nullHandle();

    // RACK-TLP init
    _rack = {};
    _tlp  = {};
    _tlp.has_rtt_sample = true;   // RFC 8985 §7.1: use initial RTT for first PTO
    _rack_stats = {};
    _tlp_pto_pending = false;
    _tlp_pto_timeout = 0;
    _tlp_pto_handle  = eventlist().nullHandle();

    _rtx_pace_pending = false;
    _proactive_tail_check_scheduled = false;
    _proactive_tail_rounds = 0;
    _rtx_pace_timer   = 0;
    _rtx_pace_next    = 0;
    _rtx_pace_handle  = eventlist().nullHandle();

    _probe_timer_handle = eventlist().nullHandle();
    _probe_timer_when   = 0;
    _probe_seqno        = 0;
    _probe_send_time    = 0;

    _flow_logger = NULL;

    _rtt = _min_rto;
    _raw_rtt = 0;

    _mdev   = 0;
    _rto    = _min_rto;
    _logger = NULL;

    _maxwnd              = 50 * _mtu;
    _cwnd                = _maxwnd;
    _flow_size           = 0;
    _done_sending        = false;
    _backlog             = 0;
    _rtx_backlog         = 0;
    _pull_target         = INIT_PULL;
    _pull                = INIT_PULL;
    _credit              = _maxwnd;
    _speculating         = true;
    _in_flight           = 0;
    _highest_sent        = 0;
    _send_blocked_on_nic = false;
    _no_of_paths         = _path_entropy_size;
    _path_random         = rand() % 0xffff;  // random upper bits of EV
    _path_xor            = rand() % _no_of_paths;
    _current_ev_index    = 0;
    _inc_bytes           = 0;
    _rss_number_of_rounds_to_skip = 0;

    // must be at least two, to allow us to encode assumed_bad state.
    _max_penalty   = 15;
    _ev_skip_count = 0;
    _ev_bad_count  = 0;
    _last_rts      = 0;

    // stats for debugging
    _stats = {};

    //printf("Using proactive probe with pflr scheme %d -- Num Sub %d\n", _pflr_scheme_id, _rss_params._rss_number_of_subflows);
    //printf("Using Load Balancing Algo %d\n", _load_balancing_algo);

    //printf("Number of Paths is %d\n", _no_of_paths);

    if (is_ecmp_bg) {
        nextEntropy = &UecSrc::nextEntropy_ecmp;
        processEv   = &UecSrc::processEv_ecmp;
        _crt_path   = 0;
    } else {
        if (_load_balancing_algo == BITMAP) {
            nextEntropy = &UecSrc::nextEntropy_bitmap;
            processEv   = &UecSrc::processEv_bitmap;
            _crt_path   = 0;

            // reset path penalties
            _ev_skip_bitmap.resize(_no_of_paths);
            for (uint32_t i = 0; i < _no_of_paths; i++) {
                _ev_skip_bitmap[i] = 0;
            }
        } else if (_load_balancing_algo == REPS) {
            nextEntropy = &UecSrc::nextEntropy_REPS;
            processEv   = &UecSrc::processEv_REPS;
            _crt_path   = 0;
        } else if (_load_balancing_algo == FLOWLET ){
            nextEntropy = &UecSrc::nextEntropy_flowlet;
            processEv = &UecSrc::processEv_flowlet;
            flowlet_entropy = _node_num % _no_of_paths;
        }  else if (_load_balancing_algo == OBLIVIOUS) {
            nextEntropy = &UecSrc::nextEntropy_oblivious;
            processEv   = &UecSrc::processEv_oblivious;
            _crt_path   = 0;
        } else if (_load_balancing_algo == MIXED) {
            nextEntropy = &UecSrc::nextEntropy_mixed;
            processEv   = &UecSrc::processEv_mixed;
            _crt_path   = 0;

            // reset path penalties
            _ev_skip_bitmap.resize(_no_of_paths);
            for (uint32_t i = 0; i < _no_of_paths; i++) {
                _ev_skip_bitmap[i] = 0;
            }
        } else if (_load_balancing_algo == RSS) {
            //printf("Entering RSS init\n");
            nextEntropy = &UecSrc::nextEntropy_rss;
            processEv   = &UecSrc::processEv_rss;
            _crt_path   = 0;
            _rss_state_mean_rtt.resize(_rss_params._rss_number_of_subflows);
            _rss_state_worse_rtt.resize(_rss_params._rss_number_of_subflows);
            _rss_state_ecn.resize(_rss_params._rss_number_of_subflows);
            _rss_state_entropies.resize(_rss_params._rss_number_of_subflows);
            _rss_state_number_of_ev_pkts.resize(_rss_params._rss_number_of_subflows);
            _rss_next_update_time = UecSrc::_rss_params._rss_update_interval;
            _rss_jitter_range = (_rss_params._rss_update_interval / 100) * _rss_params.period_jitter;

            _rss_last_feedback_time.assign(_rss_params._rss_number_of_subflows, eventlist().now());
            _rss_noack_deadline.assign(_rss_params._rss_number_of_subflows,
                                    eventlist().now() + _rss_noack_timeout);
            _rss_last_reroute_time.assign(_rss_params._rss_number_of_subflows, 0);

            // Per-subflow RACK state (Falcon-style: avoids spurious loss
            // marks from cross-subflow reordering in spray-based LB).
            // Note: _rack_tlp_mode is static and may not be set yet at
            // construction time; lazy init is done in rackOnAckUpdate().

            //printf("Subflow %d - Update Interval %lu - Subflow bits %d - th %f - to skop %d - jitter %d\n", _rss_params._rss_number_of_subflows,
            //    _rss_params._rss_update_interval, _rss_params._rss_number_of_subflow_bits, _rss_params.threshold, _rss_params.max_number_of_rounds_to_skip, _rss_params.period_jitter);
            if (UecSrc::usePflr() && _pflr_scheme_id != 0) {
                _current_evs.resize(_rss_params._rss_number_of_subflows);
                _previous_evs.resize(_rss_params._rss_number_of_subflows);
                _ev_status.resize(_rss_params._rss_number_of_subflows);
                if (_pflr_scheme_id == 4) {
                    _pflr4_init_highest_sent =
                        _pflr4_no_packet_per_slot * _rss_params._rss_number_of_subflows;
                    _pflr4_slots_ev_queue.resize(_rss_params._rss_number_of_subflows);
                    _pflr4_slots_ev_map_old_ev.resize(_rss_params._rss_number_of_subflows);
                    _pflr4_slots_ev_map_new_ev.resize(_rss_params._rss_number_of_subflows);
                    _pflr4_slots_ev_map_timeout.resize(_rss_params._rss_number_of_subflows);
                    _pflr4_slots_ev_map_countdown.resize(_rss_params._rss_number_of_subflows);
                }
            }
            if (UecSrc::usePflr() && _pflr_proactive_probe) {
                _slots_last_proactive_probe_time.resize(_rss_params._rss_number_of_subflows);
                _slots_last_proactive_probe_psn.resize(_rss_params._rss_number_of_subflows);
                _slots_last_data_packet_info.resize(_rss_params._rss_number_of_subflows);
                for (int i = 0; i < _rss_params._rss_number_of_subflows; i++) {
                    _pflr_slots_scheduled_probe_send_time.push_back(0);
                    _pflr_slots_probe_send_handle.push_back(eventlist().nullHandle());
                    _pflr_slots_probe_is_pending.push_back(0);
                }
                _pflr_probe_is_bootstrapped = 0;
            }
            for (uint32_t i = 0; i < _rss_params._rss_number_of_subflows; i++) {
                _rss_state_mean_rtt[i]  = 0;
                _rss_state_worse_rtt[i] = UINT64_MAX;
                _rss_state_entropies[i] = (rand() << _rss_params._rss_number_of_subflow_bits) +
                                        i;  // last _rss_number_of_subflow_bits are the subflow id
                if (UecSrc::usePflr() && _pflr_scheme_id != 0) {
                    uint16_t slot_ev = 0;
                    if (_pflr_scheme_id == 2) {
                        uint16_t n_bits_slot_id =
                            (uint16_t)log2(_rss_params._rss_number_of_subflows);  // #bits for slot id
                        uint16_t shifted_slot_id = i << (16 - n_bits_slot_id);
                        uint16_t mask = (1 << (16 - n_bits_slot_id)) - 1;  // Create a mask for LSBs
                        uint16_t masked_generation = 0 & mask;  // Apply mask to random number
                        // Combine shifted sid and masked random number
                        slot_ev = shifted_slot_id | masked_generation;
                    } else if ((_pflr_scheme_id == 3) || (_pflr_scheme_id == 4)) {
                        slot_ev = i;  // slot id, first PSN
                    } else {
                        abort();
                    }
                    _rss_state_entropies[i] = slot_ev;
                    _current_evs[i]         = slot_ev;
                    _previous_evs[i]        = slot_ev;
                    _ev_status[i]           = EvStatus::INITIALIZED;
                }
                _rss_state_number_of_ev_pkts[i] = 0;
                _rss_state_ecn[i]               = 0;
            }
        } else if (_load_balancing_algo == ECMP) {
            nextEntropy = &UecSrc::nextEntropy_ecmp;
            processEv   = &UecSrc::processEv_ecmp;
            _crt_path   = 0;
        } else if (_load_balancing_algo == FLOWBENDER) {
            nextEntropy       = &UecSrc::nextEntropy_flowbender;
            processEv         = &UecSrc::processEv_flowbender;
            _crt_path         = 0;
            _flowbender_stats = {0, 0, 0, 0, static_cast<uint16_t>(rand())};
        } else if (_load_balancing_algo == USS) {
            nextEntropy = &UecSrc::nextEntropy_uss;
            processEv   = &UecSrc::processEv_uss;
            _crt_path   = 0;
            _rss_state_mean_rtt.resize(_uss_params._number_of_subflows);  // recycling the RSS vectors
            _rss_state_worse_rtt.resize(_uss_params._number_of_subflows);
            _rss_state_ecn.resize(_uss_params._number_of_subflows);
            _rss_state_entropies.resize(_uss_params._number_of_subflows);
            _rss_state_number_of_ev_pkts.resize(_uss_params._number_of_subflows);
            _rss_number_of_rounds_to_skip = 0;
            if (UecSrc::usePflr() && _pflr_scheme_id != 0) {
                _current_evs.resize(_uss_params._number_of_subflows);
                _previous_evs.resize(_uss_params._number_of_subflows);
                _ev_status.resize(_uss_params._number_of_subflows);
            }
            for (uint32_t i = 0; i < _uss_params._number_of_subflows; i++) {
                _rss_state_entropies[i] = (rand() << _uss_params._number_of_subflow_bits) +
                                        i;  // last _number_of_subflow_bits are the subflow id
                _rss_state_number_of_ev_pkts[i] = 0;
                if (UecSrc::usePflr() && _pflr_scheme_id != 0) {
                    if (_pflr_scheme_id == 1) {
                        uint16_t n_bits_slot_id =
                            (uint16_t)log2(_uss_params._number_of_subflows);  // #bits for slot id
                        uint16_t shifted_slot_id = i << (16 - n_bits_slot_id);
                        uint16_t mask = (1 << (16 - n_bits_slot_id)) - 1;  // Create a mask for LSBs
                        uint16_t masked_generation = 0 & mask;  // Apply mask to random number
                        // Combine shifted sid and masked random number
                        uint16_t slot_ev        = shifted_slot_id | masked_generation;
                        _rss_state_entropies[i] = slot_ev;
                        _current_evs[i]         = slot_ev;
                        _previous_evs[i]        = slot_ev;
                        _ev_status[i]           = EvStatus::INITIALIZED;
                    } else {
                        abort();
                    }
                }
                _rss_state_mean_rtt[i]  = 0;
                _rss_state_worse_rtt[i] = UINT64_MAX;
                _rss_state_ecn[i]       = 0;
            }
        }
    }

    

    // by default, end silently
    _end_trigger = 0;

    _dstaddr = UINT32_MAX;
    //_route = NULL;
    _mtu = Packet::data_packet_size();
    _mss = _mtu - _hdr_size;

    _debug_src = UecSrc::_debug;
    _bdp       = 0;
    _base_rtt  = 0;

    _fi_count = 0;

    /* printf("Using sender-based CC %d %d, receiver-based CC %d\n", _sender_based_cc, _sender_cc_algo,
           _receiver_based_cc); */

    if (_sender_based_cc) {
        switch (_sender_cc_algo) {
            case DCTCP:
                updateCwndOnAck  = &UecSrc::updateCwndOnAck_DCTCP;
                updateCwndOnNack = &UecSrc::updateCwndOnNack_DCTCP;
                break;
            case NSCC:
                updateCwndOnAck  = &UecSrc::updateCwndOnAck_NSCC;
                updateCwndOnNack = &UecSrc::updateCwndOnNack_NSCC;
                break;
            case SMARTT:
                updateCwndOnAck  = &UecSrc::updateCwndOnAck_SMARTT;
                updateCwndOnNack = &UecSrc::updateCwndOnNack_SMARTT;
                smartt_main_loop = &UecSrc::smartt_vanilla_main_loop;
                break;
            case SMARTT_ECN_AIMD:
                updateCwndOnAck  = &UecSrc::updateCwndOnAck_SMARTT;
                updateCwndOnNack = &UecSrc::updateCwndOnNack_SMARTT;
                smartt_main_loop = &UecSrc::smartt_ecn_aimd_main_loop;
                break;
            case SMARTT_ECN_AIFD:
                updateCwndOnAck  = &UecSrc::updateCwndOnAck_SMARTT;
                updateCwndOnNack = &UecSrc::updateCwndOnNack_SMARTT;
                smartt_main_loop = &UecSrc::smartt_ecn_aifd_main_loop;
                break;
            case SMARTT_ECN_FIMD:
                updateCwndOnAck  = &UecSrc::updateCwndOnAck_SMARTT;
                updateCwndOnNack = &UecSrc::updateCwndOnNack_SMARTT;
                smartt_main_loop = &UecSrc::smartt_ecn_fimd_main_loop;
                break;
            case SMARTT_ECN_FIFD:
                updateCwndOnAck  = &UecSrc::updateCwndOnAck_SMARTT;
                updateCwndOnNack = &UecSrc::updateCwndOnNack_SMARTT;
                smartt_main_loop = &UecSrc::smartt_ecn_fifd_main_loop;
                break;
            case SMARTT_RTT:
                updateCwndOnAck  = &UecSrc::updateCwndOnAck_SMARTT;
                updateCwndOnNack = &UecSrc::updateCwndOnNack_SMARTT;
                smartt_main_loop = &UecSrc::smartt_rtt_main_loop;
                break;
            case CONSTANT:
                updateCwndOnAck  = &UecSrc::dontUpdateCwndOnAck;
                updateCwndOnNack = &UecSrc::dontUpdateCwndOnNack;
                break;
            default:
                cout << "Unknown CC algo specified " << _sender_cc_algo << endl;
                assert(0);
        }
    }
    // if (_node_num == 2) _debug_src = true; // use this to enable debugging on one flow at a
    //  time
    _received_bytes = 0;
    _recvd_bytes    = 0;

    _highest_recv_seqno = 0;
    _highest_rtx_sent   = 0;
}

void UecSrc::resetLBToECMP() {
    nextEntropy        = &UecSrc::nextEntropy_ecmp;
    processEv          = &UecSrc::processEv_ecmp;
    _crt_path          = 0;
    backgroundECMPFlow = true;
}

uint16_t UecSrc::getNoSlots() {
    uint16_t _no_slots = 1;
    if (UecSrc::_load_balancing_algo == UecSrc::RSS) {
        _no_slots = UecSrc::_rss_params._rss_number_of_subflows;
    } else if (UecSrc::_load_balancing_algo == UecSrc::USS) {
        _no_slots = UecSrc::_uss_params._number_of_subflows;
    }
    return _no_slots;
}

bool UecSrc::usePflr() {
    bool use_pflr        = UecSrc::_enable_precise_fast_loss_recovery;

    //printf("Using PFLR %d\n", use_pflr);
    return use_pflr;
    /* bool is_matched_algo = false;
    if (UecSrc::_load_balancing_algo == UecSrc::RSS) {
        is_matched_algo = true;
    } else if (UecSrc::_load_balancing_algo == UecSrc::USS) {
        is_matched_algo = true;
    }
    return (use_pflr && is_matched_algo); */
}

void UecSrc::delFromSendTimes(simtime_picosec time, UecDataPacket::seq_t seq_no) {
    // cout << eventlist().now() << " flowid " << _flow.flow_id() << " _send_times.erase " << time
    // << " for " << seq_no << endl;
    auto snd_seq_range = _send_times.equal_range(time);
    auto snd_it        = snd_seq_range.first;
    while (snd_it != snd_seq_range.second) {
        if (snd_it->second == seq_no) {
            _send_times.erase(snd_it);
            break;
        } else {
            ++snd_it;
        }
    }
}

// Register all the metrics that are gonna be collected for this object.
void UecSrc::registerMetrics() {
    return;
    _flow_metric = DataCollector::RegisterCsvMetric("flowsInfo",
                                                    {"srcNode_dstNode_flowId",
                                                     "flowSizeBytes",
                                                     "startTimeNs",
                                                     "endTimeNs",
                                                     "fctNs",
                                                     "baseRttNs",
                                                     "targetRttNs",
                                                     "rtoNs",
                                                     "BdpBytes",
                                                     "MaxCwndBytes"});
    _ack_metric  = DataCollector::RegisterTimeseriesMetric(
        "ack_" + _src_dst_flowid, {"rttNs", "ackedBytes", "isNack", "hasECN"});
    _cc_event_metric = DataCollector::RegisterTimeseriesMetric("ccEvent_" + _src_dst_flowid,
                                                               {"ccEventID", "cwndBytes"});
    _cwnd_metric =
        DataCollector::RegisterTimeseriesMetric("cwnd_" + _src_dst_flowid, {"cwndBytes", "RTT"});
    _rss_subflow_metrics.resize(UecSrc::_rss_params._rss_number_of_subflows);
    for (int i = 0; i < UecSrc::_rss_params._rss_number_of_subflows; i++) {
        _rss_subflow_metrics[i] = DataCollector::RegisterTimeseriesMetric(
            "rssSubflow_" + _src_dst_flowid + "_" + to_string(i),
            {"mean_rtt", "worse_rtt", "ecn", "entropies", "number_of_ev_packets"});
    }
    _flowbender_metrics =
        DataCollector::RegisterTimeseriesMetric("flowbender_" + _src_dst_flowid,
                                                {"number_of_packets_in_current_round",
                                                 "ecn",
                                                 "consecutive_congested_rounds",
                                                 "last_update_time",
                                                 "entropy"});
    _uss_subflow_metrics.resize(UecSrc::_uss_params._number_of_subflows);
    for (int i = 0; i < UecSrc::_uss_params._number_of_subflows; i++) {
        _uss_subflow_metrics[i] = DataCollector::RegisterTimeseriesMetric(
            "ussSubflow_" + _src_dst_flowid + "_" + to_string(i),
            {"mean_rtt", "worse_rtt", "ecn", "entropies", "number_of_ev_packets"});
    }
}

void UecSrc::connectPort(uint32_t        port_num,
                         Route&          routeout,
                         Route&          routeback,
                         UecSink&        sink,
                         simtime_picosec start_time) {
    _ports[port_num]->setRoute(routeout);
    //_route = &routeout;

    if (port_num == 0) {
        _sink = &sink;
        //_flow.set_id(get_id());  // identify the packet flow with the UEC source that generated it
        _flow._name = _name;

        if (start_time != TRIGGER_START) {
            eventlist().sourceIsPending(*this, start_time);
        }
    }
    assert(_sink == &sink);

    _sink->connectPort(port_num, *this, routeback);

    _src_dst_flowid =
        std::to_string(_srcaddr) + "_" + std::to_string(_dstaddr) + "_" + std::to_string(flowId());
    registerMetrics();
}

void UecSrc::receivePacket(Packet& pkt, uint32_t portnum) {


    /* printf("Flow %s - Time %f - CWND %lld\n",
           _flow.str().c_str(),
           timeAsUs(eventlist().now()),
           _cwnd); */

    /* if (flowId() == 4512) {
        printf("Received Ack at time %f\n", timeAsUs(eventlist().now()));
    } */

    switch (pkt.type()) {
        case UECDATA: {
            _stats.bounces_received++;
            // TBD - this is likely a Back-to-sender packet
            cout << "UecSrc::receivePacket receive UECDATA packets\n";

            abort();
        }
        case UECRTS: {
            cout << "UecSrc::receivePacket receive UECRTS packets\n";

            abort();
        }
        case UECACK: {
            if (((const UecAckPacket&)pkt).is_probe_ack()) {
                processProbeAck((const UecAckPacket&)pkt);
            } else {
                processAck((const UecAckPacket&)pkt);
            }
            pkt.free();
            return;
        }
        case UECNACK: {
            processNack((const UecNackPacket&)pkt);
            pkt.free();
            return;
        }
        case UECPULL: {
            processPull((const UecPullPacket&)pkt);
            pkt.free();
            return;
        }
        default: {
            cout << "UecSrc::receivePacket receive default\n";

            abort();
        }
    }
}

mem_b UecSrc::handleAckno(UecDataPacket::seq_t ackno) {
    auto i = _tx_bitmap.find(ackno);
    if (i == _tx_bitmap.end()) {
        // The ackno is either in tx_bitmap or in rtx_queue
        // or in neither, but never in both.
        // Hence, if it's not in _tx_bitmap, check if it's
        // in _rtx_queue and remove and correct.
        // If ackno is in neither, there is nothing else
        // to do here.
        auto rtx_i = _rtx_queue.find(ackno);
        if (rtx_i != _rtx_queue.end()) {
            // packet was in RTX queue
            mem_b pkt_size = rtx_i->second;
            _rtx_queue.erase(rtx_i);
            _rtx_backlog -= pkt_size;
            _in_flight += pkt_size;  // don't double count - we decremented when we marked for rtx
            if (_debug_src) {
                cout << "found pkt " << ackno << " in rtx queue\n";
            }
        }
        return 0;
    } else {
        // If ackno is in tx_bitmap, it means we have recentely
        // send out an packet, either for the first time or
        // an rtx packet. Since the current ack tells us that
        // it has been received already, we can remove it from
        // _tx_bitmap.
        simtime_picosec send_time = i->second.send_time;

        mem_b pkt_size = i->second.pkt_size;

        if (_debug_src)
            cout << _flow.str() << " " << _nodename << " handleAck " << ackno << " flow "
                 << _flow.str() << endl;
        if (_flow.flow_id() == _debug_flowid) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                 << " handleAck ackno " << ackno << endl;
        }

        _tx_bitmap.erase(i);
        // _send_times.erase(send_time);
        delFromSendTimes(send_time, ackno);

        if (send_time == _rto_send_time) {
            recalculateRTO();
        }

        return pkt_size;
    }

    abort();  // dead code below
    /*

    // mem_b pkt_size = i->second.pkt_size;
    simtime_picosec send_time = i->second.send_time;

    mem_b pkt_size = i->second.pkt_size;

    if (_debug_src)
        cout << _flow.str() << " " << _nodename << " handleAck " << ackno << " flow " << _flow.str()
    << endl; if(_flow.flow_id() == _debug_flowid ){ cout << timeAsUs(eventlist().now()) << " flowid
    " << _flow.flow_id() << " handleAck ackno " << ackno
             << endl;
    }
    _tx_bitmap.erase(i);
    // _send_times.erase(send_time);
    delFromSendTimes(send_time, ackno);

    if (send_time == _rto_send_time) {
        recalculateRTO();
    }

    return pkt_size;
    */
}

mem_b UecSrc::handleCumulativeAck(UecDataPacket::seq_t cum_ack) {
    mem_b newly_acked = 0;

    // free up anything cumulatively acked
    while (!_rtx_queue.empty()) {
        auto seqno = _rtx_queue.begin()->first;

        if (seqno < cum_ack) {
            mem_b pkt_size = _rtx_queue.begin()->second;
            _rtx_queue.erase(_rtx_queue.begin());
            _rtx_backlog -= pkt_size;
            _in_flight += pkt_size;  // don't double count - we decremented when we marked for rtx
        } else {
            break;
        }
    }

    auto i = _tx_bitmap.begin();
    while (i != _tx_bitmap.end()) {
        auto seqno = i->first;
        // cumulative ack is next expected packet, not yet received
        if (seqno >= cum_ack) {
            // nothing else acked
            break;
        }
        simtime_picosec send_time = i->second.send_time;

        newly_acked += i->second.pkt_size;

        if (_debug_src)
            cout << _flow.str() << " " << _nodename << " handleCumAck " << seqno << " flow "
                 << _flow.str() << endl;
        if (_flow.flow_id() == _debug_flowid) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                 << " handleCumulativeAck seqno " << seqno << endl;
        }
        _tx_bitmap.erase(i);
        i = _tx_bitmap.begin();
        // _send_times.erase(send_time);
        delFromSendTimes(send_time, seqno);
        if (send_time == _rto_send_time) {
            recalculateRTO();
        }
    }
    return newly_acked;
}

const Route* UecSrc::getPortRoute(uint32_t port_num) const {
    return _ports[port_num]->route();
}

void UecSrc::handlePull(UecBasePacket::pull_quanta pullno) {
    if (pullno > _pull) {
        UecBasePacket::pull_quanta extra_credit = pullno - _pull;
        _credit += UecBasePacket::unquantize(extra_credit);
        if (_credit > _maxwnd)
            _credit = _maxwnd;
        _pull = pullno;
    }
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " credit "
             << _credit << endl;
    }
}

bool UecSrc::checkFinished(UecDataPacket::seq_t cum_ack) {
    // cum_ack gives the next expected packet
    if (_done_sending) {
        // if (UecSrc::_debug) cout << _nodename << " checkFinished done sending " << " cum_acc "
        // << cum_ack << " mss " << _mss << " c*m " << cum_ack * _mss << endl;
        return true;
    }
    if (_debug_src)
        cout << _flow.str() << " " << _nodename << " checkFinished " << " cum_acc " << cum_ack
             << " mss " << _mss << " RTS sent " << _stats.rts_pkts_sent << " total bytes "
             << ((int64_t)cum_ack - _stats.rts_pkts_sent) * _mss << " flow_size " << _flow_size
             << " done_sending " << _done_sending << endl;

    if ((((int64_t)cum_ack - _stats.rts_pkts_sent) * _mss) >= (int64_t)_flow_size) {
        cout << "Flow " << _name << " flowId " << flowId() << " " << _nodename << " finished at "
             << timeAsUs(eventlist().now()-_flow_start_time) << " global time " << timeAsUs(eventlist().now()) << " total packets " << cum_ack << " RTS "
             << _stats.rts_pkts_sent << " total bytes "
             << ((mem_b)cum_ack - _stats.rts_pkts_sent) * _mss << " in_flight now " << _in_flight
             << " current time " << eventlist().now() << " start time " << _flow_start_time << " lost packets " << _lost_packets
             << " cwnd " << _cwnd << " is_bg " << is_ecmp_bg << " " << from << " "  << to << " " << flowId()
             << " rack_marks " << _rack_stats.rack_loss_marks
             << " tlp_sent " << _rack_stats.tlp_probes_sent
             << " tlp_repairs " << _rack_stats.tlp_probe_repairs
             << " tlp_spurious " << _rack_stats.tlp_probe_spurious
             << " rto_events " << _rack_stats.rto_events
             << " spurious_rtx " << _rack_stats.spurious_retrans
             << endl;
        logMetricFlow();
        _speculating = false;
        if (_end_trigger) {
            _end_trigger->activate();
        }
        if (_flow_logger) {
            _flow_logger->logEvent(_flow, *this, FlowEventLogger::FINISH, _flow_size, cum_ack);
        }


        EventOver flow_over(from, to, _flow_size, tag, eventlist().now(), AtlahsEventType::SEND_EVENT_OVER);
        flow_over.node = lgs_node.get();
        flow_over.start_time_event = _flow_start_time;
        flow_over.flow_id = flowId();
        if (_atlahs_api) {
            if (_atlahs_api->print_stats_flows) {
                _atlahs_api->flowInfos.push_back(FlowInfo(timeAsUs(_flow_start_time), timeAsUs(eventlist().now()), timeAsUs(eventlist().now() - _flow_start_time), _flow_size, 1, _cwnd));
            }
            _atlahs_api->EventFinished(flow_over);
        }

        // Ensure no stale retransmission state survives a completed flow.
        cancelRTO();
        tlpCancelPTO();
        if (_rtx_pace_pending) {
            eventlist().cancelPendingSourceByHandle(*this, _rtx_pace_handle);
            _rtx_pace_pending = false;
            _rtx_pace_handle = eventlist().nullHandle();
        }
        _proactive_tail_check_scheduled = false;
        _proactive_tail_rounds = 0;
        _rtx_pace_next = 0;
        _rtx_pace_timer = 0;
        _rtx_queue.clear();
        _rtx_backlog = 0;
        _tx_bitmap.clear();
        _send_times.clear();
        _tlp.probe_in_flight = false;
        _tlp.has_rtt_sample  = true;
        for (auto& tlp_sf : _tlp_per_subflow) {
            tlp_sf.probe_in_flight = false;
            tlp_sf.has_rtt_sample  = true;
            tlp_sf.pto_deadline    = 0;
        }
        _in_flight           = 0;

        _done_sending = true;
        return true;
    }
    return false;
}

uint32_t UecSrc::pflr4GetStageId() {
    uint32_t stage_id = 0;
    if (_highest_sent >= _pflr4_init_highest_sent) {
        stage_id = 1;
    }
    return stage_id;
}

void UecSrc::pflr4SenderReceiveEv(UecBasePacket::seq_t seq_no, uint16_t ev) {
    if (usePflr() && !backgroundECMPFlow && (_pflr_scheme_id == 4)) {
        auto     _no_slots = getNoSlots();
        uint32_t slot_id   = seq_no % _no_slots;
        _pflr4_slots_ev_queue[slot_id].push_back(ev);
    }
}

void UecSrc::pflr0SenderReceiveAck(UecBasePacket::seq_t seq_no, uint16_t ev, bool is_probe_ack) {
    if (usePflr() && !backgroundECMPFlow && (_pflr_scheme_id == 0)) {
        cout << "Current Sender record before: \n";
        for (const auto& pair : pflr0_sender_record) {
            cout << "EV: " << pair.first << " -> [ ";
            for (auto psn : pair.second) {
                cout << psn << " ";
            }
            std::cout << "]\n";
        }
        // if ev is in the recore
        auto pair = pflr0_sender_record.find(ev);
        if (pair == pflr0_sender_record.end()) {
            return;
        }

        auto& sender_record = pair->second;  // Get the vector corresponding to the key
        auto  position =
            find(sender_record.begin(), sender_record.end(), seq_no);  // Locate the value
        cout << "find at " << position - sender_record.begin() << endl;

        if (position != sender_record.end()) {
            // rtx all previous element
            while (position >= sender_record.begin()) {
                auto current_psn = *position;
                position         = sender_record.erase(position) - 1;
                // send nack
                if ((current_psn == seq_no) && (!is_probe_ack)) {
                    continue;
                }
                auto i = _tx_bitmap.find(current_psn);
                if (i != _tx_bitmap.end()) {
                    mem_b pkt_size = i->second.pkt_size;

                    auto            seqno     = i->first;
                    simtime_picosec send_time = i->second.send_time;

                    // The average queue delay is not updated, since the packet was trimmed.
                    _tx_bitmap.erase(i);
                    assert(_tx_bitmap.find(seqno) == _tx_bitmap.end());  // xxx remove when working

                    delFromSendTimes(send_time, seqno);
                    cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
                         << " pflr0 queue rtx packet for psn " << seqno << endl;
                    queueForRtx(seqno, pkt_size);
                    // cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id() << " queue
                    // rtx packet for psn " << seqno << endl;
                    if (send_time == _rto_send_time) {
                        recalculateRTO();
                    }
                }
            }
        }

        if (sender_record.empty()) {
            pflr0_sender_record.erase(pair);
        }
        cout << "Current Sender record after: \n";
        for (const auto& pair : pflr0_sender_record) {
            cout << "EV: " << pair.first << " -> [ ";
            for (auto psn : pair.second) {
                cout << psn << " ";
            }
            std::cout << "]\n";
        }
    }
}

void UecSrc::processProbeAck(const UecAckPacket& pkt) {
    assert(usePflr() && !backgroundECMPFlow && (_pflr_scheme_id == 0));

    cout << timeAsUs(eventlist().now()) << " flow " << pkt.flow_id()
         << " receive probe ack packet for psn " << pkt.acked_psn() << " ev " << pkt.ev() << endl;
    pflr0SenderReceiveAck(pkt.acked_psn(), pkt.ev(), true);
    stopSpeculating();
    sendIfPermitted();
}

void UecSrc::processAck(const UecAckPacket& pkt) {
    auto cum_ack  = pkt.cumulative_ack();
    bool rtx_echo = pkt.rtx_echo();
    // handle flight_size based on recvd_bytes in packet.
    uint64_t newly_recvd_bytes = 0;

    if (_load_balancing_algo == RSS) {
        int sid = rssSubflowFromEv(pkt.ev());
        _rss_last_feedback_time[sid] = eventlist().now();
        _rss_noack_deadline[sid]     = eventlist().now() + _rss_noack_timeout;
    }

    if (pkt.recvd_bytes() > _recvd_bytes) {
        newly_recvd_bytes = pkt.recvd_bytes() - _recvd_bytes;
        _recvd_bytes      = pkt.recvd_bytes();

        _achieved_bytes += newly_recvd_bytes;
        _received_bytes += newly_recvd_bytes;
        _bytes_ignored += newly_recvd_bytes;
    }

    /* cout << timeAsUs(eventlist().now()) << " flow " << pkt.flow_id()
         << " receive ack packet for psn " << pkt.acked_psn() << " ev " << pkt.ev() << endl; */
    pflr4SenderReceiveEv(pkt.acked_psn(), pkt.ev());
    if (!pkt.is_rts_ack()) {
        pflr0SenderReceiveAck(pkt.acked_psn(), pkt.ev(), false);
    }
    if (_debug_src) {
        cout << "processAck " << cum_ack << " ref_epsn " << pkt.acked_psn() << " recvd_bytes "
             << _recvd_bytes << " newly_recvd_bytes " << newly_recvd_bytes << endl;
    }
    _stats.acks_received++;

    // Debug
    /* if (flowId() == 4512 || flowId() == 3566) {
        cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
            << " received ack packet for psn " << pkt.acked_psn() << " ev " << pkt.ev() << 
            " last " << timeAsUs(eventlist().now()-last_ack_sent_time) << endl;
        last_ack_sent_time = eventlist().now();
    } */
    

    // decrease flightsize.
    _in_flight -= newly_recvd_bytes;
    // We cannot run this next line's check here since
    // _in_flight could be corrected (increased) in either
    // handleCumulativeAck or handleAckno.
    // assert(_in_flight >= 0);

    if (_sender_based_cc && pkt.rcv_wnd_pen() < 255) {
        sint64_t window_decrease = newly_recvd_bytes - newly_recvd_bytes * pkt.rcv_wnd_pen() / 255;
        _cwnd                    = max(_cwnd - window_decrease, (mem_b)_mtu);
    }

    // compute RTT sample
    auto     acked_psn = pkt.acked_psn();
    auto     i         = _tx_bitmap.find(acked_psn);
    auto     rtx_time  = _rtx_times.find(acked_psn);
    uint32_t ooo       = pkt.ooo();

    mem_b           pkt_size;
    simtime_picosec delay;
    simtime_picosec send_time = 0;
    // a timestamp is valid if
    // 1. the received ack is new packet and no retransmission at local record;
    // or 2. the received ack is a retransmitted packet and local record shows this packet only gets
    // retransmitted once.
    bool validate_ts = (((rtx_time->second == 0 && rtx_echo == false) ||
                         (rtx_time->second == 1 && rtx_echo == true))) &&
                       (pkt.packet_type_echo() != UecBasePacket::DATA_PROBE);
    // Count spurious retransmissions: packet was retransmitted but original arrived
    if (_rack_tlp_mode != RackTlpMode::OFF && rtx_time != _rtx_times.end() && rtx_time->second > 0 && !rtx_echo) {
        _rack_stats.spurious_retrans++;
    }
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " rtx_times "
             << rtx_time->second << " rtx_echo " << rtx_echo << " validate_ts " << validate_ts
             << " packet_type " << pkt.packet_type_echo() << " _probe_psn " << _probe_seqno
             << " psn " << pkt.acked_psn() << endl;
    }
    if (i != _tx_bitmap.end() && validate_ts) {
        // auto seqno = i->first;
        send_time = i->second.send_time;
        pkt_size  = i->second.pkt_size;
        _raw_rtt  = eventlist().now() - send_time;
        update_base_rtt(_raw_rtt, pkt_size);
        logMetricAck(_raw_rtt, newly_recvd_bytes, false, pkt.ecn_echo());
        if (_raw_rtt >= _base_rtt) {
            update_delay(_raw_rtt, true, pkt.ecn_echo());
            delay = _raw_rtt - _base_rtt;
        } else {
            delay = get_avg_delay();
        }
    } else {
        // this can happen when the ACK arrives later than a cumulative ACK covering the NACKed
        // packet.
        if (UecSrc::_debug)
            cout << "Can't find send record for seqno " << acked_psn << endl;
        if (pkt.packet_type_echo() == UecBasePacket::DATA_PROBE) {
            if (_probe_seqno == pkt.acked_psn()) {
                _raw_rtt = eventlist().now() - _probe_send_time;
                if (_raw_rtt < _base_rtt) {
                    delay    = 0;
                    _raw_rtt = _base_rtt;
                } else {
                    delay = _raw_rtt - _base_rtt;
                    update_delay(_raw_rtt, true, pkt.ecn_echo());
                }
                if (_flow.flow_id() == _debug_flowid) {
                    cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                         << " _probe_seqno " << _probe_seqno << " delay " << timeAsUs(delay)
                         << endl;
                }
            } else {
                delay = get_avg_delay();
            }
            pkt_size = 0;
        } else {
            pkt_size = _mtu;
            delay    = get_avg_delay();
        }
    }

    handleCumulativeAck(cum_ack);

    if (_debug_src)
        cout << "At " << timeAsUs(eventlist().now()) << " " << _flow.str() << " " << _nodename
             << " processAck cum_ack: " << cum_ack << " flow " << _flow.str() << endl;

    auto ackno = pkt.ref_ack();

    uint64_t bitmap = pkt.bitmap();

    if (_debug_src)
        cout << "    ref_ack: " << ackno << " bitmap: " << bitmap << endl;

    while (bitmap > 0) {
        if (bitmap & 1) {
            if (_debug_src)
                cout << "    Sack " << ackno << " flow " << _flow.str() << endl;

            handleAckno(ackno);
            if (_highest_recv_seqno < ackno) {
                _highest_recv_seqno = ackno;
            }
        }
        ackno++;
        bitmap >>= 1;
    }

    // We ran both potential _in_flight correcting functions
    // now check if we are in the negative.
    // assert(_in_flight >= 0);

    // Resolve TLP state on every ACK. An ACK for the original copy is the
    // evidence that a retransmission probe was spurious, and it may not have
    // a valid retransmission timestamp.
    if (_rack_tlp_mode != RackTlpMode::OFF) {
        // Per-subflow TLP: resolve probe state for the ACK'd packet's subflow
        int ack_sid = rackSubflowForSeqno(acked_psn);
        if (!_tlp_per_subflow.empty() && ack_sid >= 0) {
            TlpState& tlp_sf = _tlp_per_subflow[ack_sid];
            resolve_tlp_ack(tlp_sf, acked_psn, rtx_echo);
            tlp_sf.has_rtt_sample = true;
            tlp_sf.pto_deadline = 0;  // needs recomputation
        } else {
            // Legacy single-TLP path
            resolve_tlp_ack(_tlp, acked_psn, rtx_echo);
            _tlp.has_rtt_sample = true;
        }

        if (send_time > 0 && _rack_tlp_mode != RackTlpMode::TLP_ONLY) {
            rackOnAckUpdate(acked_psn, send_time);
            rackDetectLosses();
        }
        // Re-arm PTO after ACK processing (enables cascade probing)
        tlpComputeAndArmPTO();
    }

    _loss_counter--;
    PathFeedback pkt_feedback = {pkt.ecn_echo() ? PATH_ECN : PATH_GOOD, _raw_rtt};
    (this->*processEv)(pkt.ev(), pkt_feedback);

    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " track_avg_rtt "
             << timeAsUs(get_avg_delay()) << " rtt " << timeAsUs(_raw_rtt) << " skip "
             << pkt.ecn_echo() << " ev " << pkt.ev() << " cum_ack " << cum_ack << " bitmap_base "
             << pkt.ref_ack() << " ooo " << ooo << " cwnd " << _cwnd / get_avg_pktsize()
             << " _achieved_bytes " << _achieved_bytes << " acked_psn " << acked_psn
             << " sending_time " << timeAsUs(send_time) << endl;
    }
    if (_sender_based_cc) {
        /*if (pkt.ecn_echo()){
            (this->*updateCwndOnAck)(pkt.ecn_echo(), delay, pkt_size);
            (this->*updateCwndOnAck)(false, delay, newly_recvd_bytes - pkt_size);
        }
        else */
        (this->*updateCwndOnAck)(pkt.ecn_echo(), delay, newly_recvd_bytes);
        //logMetricCwnd(_cwnd, _raw_rtt);
    }

    if (_debug_src) {
        cout << "At " << timeAsUs(eventlist().now()) << " " << _flow.str() << " " << _nodename
             << " processAck: " << cum_ack << " flow " << _flow.str() << " cwnd " << _cwnd
             << " flightsize " << _in_flight << " delay " << timeAsUs(delay) << " newlyrecvd "
             << newly_recvd_bytes << " skip " << pkt.ecn_echo() << " raw rtt " << _raw_rtt << endl;
    }

    if (_sender_based_cc && _enable_sleek) {
        // probe packets
        if (_probe_timer_when != 0) {
            if (_probe_timer_handle->second != this) {
                if (_flow.flow_id() == _debug_flowid) {
                    cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                         << " an assert soon" << endl;
                }
            }
            eventlist().cancelPendingSourceByHandle(*this, _probe_timer_handle);
            _probe_timer_when   = 0;
            _probe_timer_handle = eventlist().nullHandle();
        }
        if (cum_ack < _highest_sent || _backlog > 0) {
            if (_backlog == 0) {
                _probe_timer_when = eventlist().now() + (_base_rtt + _target_Qdelay);
            } else {
                _probe_timer_when = eventlist().now() + 3 * _base_rtt;
            }
            _probe_timer_handle = eventlist().sourceIsPendingGetHandle(*this, _probe_timer_when);
        }
        if (pkt.packet_type_echo() == UecBasePacket::DATA_PROBE && delay < _target_Qdelay) {
            _loss_recovery_mode = true;
            // _avg_delay = delay;
            _recovery_seqno   = _highest_sent;
            _highest_rtx_sent = cum_ack;
            if (_flow.flow_id() == _debug_flowid) {
                cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                     << " enter_loss_probe " << " _avg_delay " << timeAsUs(_avg_delay) << endl;
            }
        }
        runSleek(ooo, cum_ack);
        sendIfPermitted();
    }

    stopSpeculating();

    if (checkFinished(cum_ack)) {
        return;
    }

    // RACK-TLP: arm PTO after ACK processing
    if (_rack_tlp_mode == RackTlpMode::RACK_TLP ||
        _rack_tlp_mode == RackTlpMode::RACK_TLP_NO_6675 ||
        _rack_tlp_mode == RackTlpMode::TLP_ONLY) {
        tlpComputeAndArmPTO();
    }

    sendIfPermitted();
}

/*
    Register per flow metrics (flow size, flow start time, flow end time, flow completion time, base
   rtt, target rtt, rto, bdp and max cwnd)
*/
void UecSrc::logMetricFlow() {
    return;
    uint64_t        start_time = static_cast<uint64_t>(timeAsNs(_flow_start_time));
    uint64_t        end_time   = static_cast<uint64_t>(timeAsNs(eventlist().now()));
    uint64_t        fct_ns     = end_time - start_time;
    simtime_picosec target_rtt = target_rtt_scaling_factor * _base_rtt;
    if (_sender_cc_algo == NSCC) {
        target_rtt = _base_rtt + _target_Qdelay;
    }
    _flow_metric->LogData({
        _src_dst_flowid,
        std::to_string(_flow_size),
        std::to_string(start_time),
        std::to_string(end_time),
        std::to_string(fct_ns),
        std::to_string((simtime_picosec)timeAsNs(_base_rtt)),
        std::to_string((simtime_picosec)timeAsNs(target_rtt)),
        std::to_string((simtime_picosec)timeAsNs(_rto)),
        std::to_string(_bdp),
        std::to_string(_maxwnd),
    });
}

// For each receiving packet at the sender, logs: rtt, ackedBytes, isNack and if the packet has ECN.
void UecSrc::logMetricAck(simtime_picosec rtt, int ackedBytes, bool isNack, bool hasECN) {
    return;
    _ack_metric->LogData({std::to_string((simtime_picosec)timeAsNs(rtt)),
                          std::to_string(ackedBytes),
                          std::to_string(isNack),
                          std::to_string(hasECN)});
}

// For each change in the cwnd, log its value and the event that triggered it.
void UecSrc::logMetricCCEvent(CCEventType cc_action, uint64_t cwnd) {
    return;
    _cc_event_metric->LogData({std::to_string(cc_action), std::to_string(cwnd)});
}

// For each change in the cwnd, log its value.
void UecSrc::logMetricCwnd(uint64_t cwnd, simtime_picosec raw_rtt) {
    return;
    _cwnd_metric->LogData({std::to_string(cwnd), to_string((simtime_picosec)timeAsNs(raw_rtt))});
}

// When using RSS, log at every period
void UecSrc::logMetricRssSubflow(vector<simtime_picosec> path_feedback_mean_rtt,
                                 vector<simtime_picosec> path_feedback_worse_rtt,
                                 vector<float>           path_feedback_ecn,
                                 vector<uint16_t>        entropy,
                                 vector<uint16_t>        packet_count) {
    return;
    for (int i = 0; i < _rss_params._rss_number_of_subflows; i++) {
        bitset<16> bits(entropy[i]);
        _rss_subflow_metrics[i]->LogData(
            {to_string((simtime_picosec)timeAsNs(path_feedback_mean_rtt[i])),
             to_string((simtime_picosec)timeAsNs(path_feedback_worse_rtt[i])),
             to_string(path_feedback_ecn[i]),
             bits.to_string(),
             to_string(packet_count[i])});
    }
}

// When using FlowBender, log at every RTT
void UecSrc::logMetricFlowBender(UecSrc::FlowBenderStats _flowbender_stats) {
    return;
    _flowbender_metrics->LogData(
        {to_string(_flowbender_stats._number_of_packets_in_current_round),
         to_string(round(static_cast<double>(_flowbender_stats._current_rtt_ecn_packet_count) /
                         _flowbender_stats._number_of_packets_in_current_round * 100) /
                   100),
         to_string(_flowbender_stats._current_consecutive_congested_rtt),
         to_string((simtime_picosec)timeAsNs(_flowbender_stats.last_update)),
         to_string(_flowbender_stats._entropy)});
}

// When using USS, log at every period, defined by UecSrc::USS_LOG_FREQUENCY
void UecSrc::logMetricUssSubflow(vector<simtime_picosec> path_feedback_mean_rtt,
                                 vector<simtime_picosec> path_feedback_worse_rtt,
                                 vector<float>           path_feedback_ecn,
                                 vector<uint16_t>        entropy,
                                 vector<uint16_t>        packet_count) {
    return;
    for (int i = 0; i < UecSrc::_uss_params._number_of_subflows; i++) {
        bitset<16> bits(entropy[i]);
        _uss_subflow_metrics[i]->LogData(
            {to_string((simtime_picosec)timeAsNs(path_feedback_mean_rtt[i])),
             to_string((simtime_picosec)timeAsNs(path_feedback_worse_rtt[i])),
             to_string(path_feedback_ecn[i]),
             bits.to_string(),
             to_string(packet_count[i])});
    }
}

void UecSrc::updateCwndOnAck_DCTCP(bool skip, simtime_picosec rtt, mem_b newly_acked_bytes) {
    /* cout << timeAsUs(eventlist().now()) << " DCTCP start " << _name << " cwnd " << _cwnd
         << " with params skip " << skip << " acked bytes " << newly_acked_bytes << endl;  */

    if (skip == false)  // additive increase, 1 PKT /RTT
    {
        _cwnd += newly_acked_bytes * _mtu / _cwnd;

    } else {  // multiplicative decrease, done per mark, more aggressive than DCTCP (less smoothing)
              // but much simpler and more responsive since we don't need to track alpha.
        _cwnd -= newly_acked_bytes / 3;
        _cwnd = max((mem_b)_mtu, _cwnd);
    }
    set_cwnd_bounds();
}

void UecSrc::updateCwndOnNack_DCTCP(bool skip, mem_b nacked_bytes) {
    /* printf("Nack DCTCP start %s cwnd %lld with params skip %d nacked_bytes %lld\n",
           _name.c_str(),
           _cwnd,
           skip,
           nacked_bytes); */
    _cwnd -= nacked_bytes;
    _cwnd = max(_cwnd, (mem_b)_mtu);
}

bool UecSrc::can_send_NSCC(mem_b pkt_size) {
    return (pkt_size > 0) &&
           (((!_loss_recovery_mode && _cwnd >= _in_flight + pkt_size) ||
             (_loss_recovery_mode && (!_rtx_queue.empty() || _cwnd >= _in_flight + pkt_size))));
}

void UecSrc::set_cwnd_bounds() {
    if (_cwnd < _min_cwnd)
        _cwnd = _min_cwnd;

    if (_cwnd > _maxwnd)
        _cwnd = _maxwnd;
    //printf("Min cwnd %lld, maxwnd %lld, current cwnd %lld\n", _min_cwnd, _maxwnd, _cwnd);
}

bool UecSrc::quick_adapt(bool is_loss, simtime_picosec delay) {
    if (_receiver_based_cc)
        return false;

    if (_debug_src) {
        cout << "At " << timeAsUs(eventlist().now()) << " " << _flow.str()
             << " quickadapt called is loss " << is_loss << " delay " << delay << " qa_endtime "
             << timeAsUs(_qa_endtime) << " trigger qa " << _trigger_qa << endl;
    }
    if (eventlist().now() > _qa_endtime) {
        bool qa_gate = true;
        if (_enable_qa_gate) {
            qa_gate = (_achieved_bytes < _maxwnd / 8);
        }
        if (_qa_endtime != 0 && (_trigger_qa || is_loss || (delay > _qa_threshold)) && qa_gate) {
            if (_debug_src) {
                cout << "At " << timeAsUs(eventlist().now()) << " " << _flow.str()
                     << " running quickadapt, CWND is " << _cwnd << " setting it to "
                     << _achieved_bytes << endl;
            }

            if (_cwnd < _achieved_bytes) {
                if (_debug_src) {
                    cout << "This shouldn't happen: QUICK ADAPT MIGHT INCREASE THE CWND" << endl;
                }
            } else {
                _cwnd = max(_achieved_bytes, (mem_b)_min_cwnd);  //* _qa_scaling;
                if (_flow.flow_id() == _debug_flowid)
                    cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                         << " quick_adapt  _nscc_cwnd " << _cwnd << " is_loss " << is_loss << endl;
                _bytes_to_ignore = _in_flight;
                _bytes_ignored   = 0;
                _trigger_qa      = false;
                _achieved_bytes  = 0;
                cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                         << " quick_adapt  _nscc_cwnd " << _cwnd << " is_loss " << is_loss << endl;
                _qa_endtime      = eventlist().now() + _base_rtt + _target_Qdelay;
                logMetricCCEvent(CCEventType::QUICK_ADAPT, _cwnd);
                return true;
            }
        }
        _achieved_bytes = 0;
        _qa_endtime     = eventlist().now() + _base_rtt + _target_Qdelay;
    }
    return false;
}

void UecSrc::fair_increase(uint32_t newly_acked_bytes) {
    _inc_bytes += _fi * _mtu * newly_acked_bytes;  // increase by 16Million!
}

void UecSrc::proportional_increase(uint32_t newly_acked_bytes, simtime_picosec delay) {
    fast_increase(newly_acked_bytes, delay);
    if (_increase)
        return;

    // make sure targetQdelay > delay;
    assert(_target_Qdelay > delay);

    _inc_bytes += (uint32_t)round(_alpha * (_target_Qdelay - delay) * (double)newly_acked_bytes);

    fair_increase(newly_acked_bytes);
}

void UecSrc::fast_increase(uint32_t newly_acked_bytes, simtime_picosec delay) {
    if (delay < timeFromUs(1u)) {
        _fi_count += newly_acked_bytes;
        if (_fi_count > _cwnd || _increase) {
            _cwnd += newly_acked_bytes * _fi_scale;

            if (_cwnd > _maxwnd)
                _cwnd = _maxwnd;
            logMetricCCEvent(CCEventType::FAST_INCREASE, _cwnd);
            _increase = true;
            return;
        }
    } else {
        _fi_count = 0;
    }
    _increase = false;
}

void UecSrc::multiplicative_decrease(uint32_t newly_acked_bytes) {
    _increase                 = false;
    _fi_count                 = 0;
    simtime_picosec avg_delay = get_avg_delay();
    if (avg_delay > _target_Qdelay) {
        if (eventlist().now() - _last_dec_time > _base_rtt) {
            _cwnd *= max(1 - _gamma * (avg_delay - _target_Qdelay) / avg_delay,
                         0.5); /*_max_md_jump instead of 1*/
            _last_dec_time = eventlist().now();
        }
    }
}

void UecSrc::fulfill_adjustment() {
    assert(_bdp > 0);
    if (_debug_src) {
        cout << "Running fulfill adjustment cwnd " << _cwnd << " inc " << _inc_bytes << " bdp "
             << _bdp << endl;
    }
    _cwnd += min((mem_b)_received_bytes, (mem_b)_inc_bytes / _cwnd);

    _inc_bytes = 0;

    // unclear what received_bytes this is referring to.
    _received_bytes   = 0;
    _last_adjust_time = eventlist().now();
}

void UecSrc::mark_packet_for_retransmission(UecBasePacket::seq_t psn, uint16_t pktsize) {
    _in_flight -= pktsize;
    // assert (_in_flight>=0);
    _cwnd = max(_cwnd - pktsize, (mem_b)_mtu);
    if (_flow.flow_id() == _debug_flowid)
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
             << " mark_packet_for_retransmission  _cwnd " << _cwnd << endl;
    logMetricCCEvent(CCEventType::TIMEOUT_DECREASE, _cwnd);
    //_rtx_count ++;
}

void UecSrc::resolve_tlp_ack(TlpState& tlp,
                             UecDataPacket::seq_t acked_psn,
                             bool rtx_echo) {
    TlpAckOutcome outcome = classifyTlpAck(tlp, acked_psn, rtx_echo);
    if (outcome == TlpAckOutcome::NONE) {
        return;
    }

    if (outcome == TlpAckOutcome::RETRANSMISSION_REPAIRED) {
        _rack_stats.tlp_probe_repairs++;
        if (_sender_based_cc && _tlp_confirmed_loss_cwnd) {
            mem_b loss_bytes = (tlp.probe_size > 0) ? tlp.probe_size : _mtu;
            (this->*updateCwndOnNack)(false, loss_bytes);
            logMetricCCEvent(CCEventType::TIMEOUT_DECREASE, _cwnd);
        }
    } else if (outcome == TlpAckOutcome::RETRANSMISSION_SPURIOUS) {
        _rack_stats.tlp_probe_spurious++;
    }

    tlp.probe_in_flight = false;
    tlp.probe_size = 0;
}

void UecSrc::dontUpdateCwndOnAck(bool skip, simtime_picosec delay, mem_b newly_acked_bytes) {}

void UecSrc::updateCwndOnAck_NSCC(bool skip, simtime_picosec delay, mem_b newly_acked_bytes) {
    // bool can_decrease = _exp_avg_ecn > _ecn_thresh;

    if (_bytes_ignored < _bytes_to_ignore && skip)
        return;

    if (quick_adapt(false, delay))
        return;

    if (!skip && delay >= _target_Qdelay) {
        fair_increase(newly_acked_bytes);
        if (_flow.flow_id() == _debug_flowid || UecSrc::_debug) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " "
                 << _flow.str() << " fair_increase _nscc_cwnd " << _cwnd << " newly_acked_bytes "
                 << newly_acked_bytes << " fi " << _fi << endl;
        }
        logMetricCCEvent(CCEventType::FAIR_INCREASE, _cwnd);
    } else if (!skip && delay < _target_Qdelay) {
        proportional_increase(newly_acked_bytes, delay);
        if (_flow.flow_id() == _debug_flowid || UecSrc::_debug) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " "
                 << _flow.str() << " proportional_increase _nscc_cwnd " << _cwnd << endl;
        }
        logMetricCCEvent(CCEventType::PROPORTIONAL_INCREASE, _cwnd);
    } else if (skip && delay >= _target_Qdelay) {
        multiplicative_decrease(newly_acked_bytes);
        if (_flow.flow_id() == _debug_flowid || UecSrc::_debug) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " "
                 << _flow.str() << " multiplicative_decrease _nscc_cwnd " << _cwnd << endl;
        }
        logMetricCCEvent(CCEventType::PROPORTIONAL_DECREASE, _cwnd);
    } else if (skip && delay < _target_Qdelay) {
        // NOOP, just switch path
    }

    // Check here, fulfill_adjustment requires valid cwnd.
    set_cwnd_bounds();

    // if ( _received_bytes > _adjust_bytes_threshold || eventlist().now() - _last_adjust_time >
    // _adjust_period_threshold ) {
    if (_received_bytes > _adjust_bytes_threshold ||
        eventlist().now() - _last_adjust_time > _adjust_period_threshold) {
        if (_flow.flow_id() == _debug_flowid || UecSrc::_debug) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " "
                 << _flow.str() << " fulfill_adjustmentx _nscc_cwnd " << _cwnd << " inc_bytes "
                 << _inc_bytes << endl;
        }
        fulfill_adjustment();
        if (_flow.flow_id() == _debug_flowid || UecSrc::_debug) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " "
                 << _flow.str() << " fulfill_adjustment _nscc_cwnd " << _cwnd << endl;
        }
    }

    if (eventlist().now() - _last_eta_time > _adjust_period_threshold) {
        _cwnd += _eta;
        _last_eta_time = eventlist().now();
        if (_flow.flow_id() == _debug_flowid) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " "
                 << _flow.str() << " eta _nscc_cwnd " << _cwnd << " target_q_delay "
                 << timeAsUs(_target_Qdelay) << endl;
        }
    }

    set_cwnd_bounds();

    if (_flow.flow_id() == _debug_flowid)
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " final _nscc_cwnd "
             << _cwnd << " _basertt " << timeAsUs(_base_rtt) << endl;
}

void UecSrc::updateCwndOnNack_NSCC(bool skip, mem_b nacked_bytes) {
    _cwnd -= nacked_bytes;

    set_cwnd_bounds();

    if (_flow.flow_id() == _debug_flowid)
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
             << " onnack  _nscc_cwnd " << _cwnd << endl;
    _trigger_qa = true;
    if (_bytes_ignored >= _bytes_to_ignore)
        quick_adapt(true, get_avg_delay());
}

void UecSrc::smartt_update_fast_increase_state(bool skip, simtime_picosec delay) {

    /* printf("smartt_update_fast_increase_state: skip %d, delay %f - %d %d %d\n", skip,
          timeAsUs(delay), counter_consecutive_good_bytes, target_window, _cwnd); */

    if (delay <= timeFromUs(1u) && !skip) {
        counter_consecutive_good_bytes += _mtu;
    } else {
        target_window                  = _cwnd;
        counter_consecutive_good_bytes = 0;
        increasing                     = false;
    }
}

bool UecSrc::smartt_should_fast_increase() {
    return ((increasing || counter_consecutive_good_bytes > target_window) && use_fast_increase);
}

void UecSrc::smartt_fast_increase() {
    /* printf("smartt_fast_increase: _bytes_ignored %d, _bytes_to_ignore %d, _cwnd %f, %f\n",
           _bytes_ignored, _bytes_to_ignore, _cwnd, fast_increase_scaling_factor * _mtu); */
    if (_bytes_ignored > _bytes_to_ignore || true) {
        _cwnd += fast_increase_scaling_factor * _mtu;
        _cwnd = min(_cwnd, _maxwnd);
        logMetricCCEvent(CCEventType::FAST_INCREASE, _cwnd);
    }

    increasing = true;
}

bool UecSrc::smartt_check_and_do_quick_adapt(bool skip, bool trimmed) {
    // Checks if we have recvd acks after a previous quick adapt. If so then
    // checks if we need to do quick adapt now.
    if (_bytes_ignored >= _bytes_to_ignore) {
        smartt_quick_adapt(false);
    }

    // Checks if we have recvd acks after a previous quick adapt. If not, we
    // shouldn't make cwnd adjustments and wait for quick adapt changes to
    // relfect in the network. Returns true if cwnd adjustments are allowed.
    if (_bytes_ignored < _bytes_to_ignore && skip) {
        return false;
    }
    return true;
}

void UecSrc::smartt_quick_adapt(bool trimmed) {
    if (eventlist().now() >= _qa_endtime) {
        previous_window_end  = _qa_endtime;
        saved_acked_bytes    = _achieved_bytes;
        _achieved_bytes      = 0;
        uint64_t _target_rtt = _base_rtt + _base_rtt / 2;
        _qa_endtime          = eventlist().now() + _target_rtt;
        // Enable Fast Drop
        if ((trimmed || need_quick_adapt) && previous_window_end != 0 && _cwnd/4 > saved_acked_bytes) {



            _cwnd = max((double)(saved_acked_bytes * 1), (double)_mtu);

            // Reset counters, update logs.
            _bytes_to_ignore = _in_flight;
            _bytes_ignored   = 0;
            logMetricCCEvent(CCEventType::QUICK_ADAPT, _cwnd);
            need_quick_adapt = false;
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                         << " quick_adapt2  _nscc_cwnd " << _cwnd << " ignore " << _bytes_to_ignore << endl;
        }
    }
}

void UecSrc::smartt_ecn_aimd_main_loop(bool skip, simtime_picosec delay) {
    bool can_decrease_exp_avg = smartt_wtd_can_decrease();
    if (!skip) {
        _cwnd += ((double)_mtu / _cwnd) *
                 (fair_increase_scaling_factor *
                  _network_bdp);  // TODO: Ideally this should be 1, currently it is 0.2
        logMetricCCEvent(CCEventType::FAIR_INCREASE, _cwnd);
    } else {
        if (can_decrease_exp_avg) {
            _cwnd -= _mtu * fair_decrease_scaling_factor;  // TODO: Ideally this should be 0.5
            logMetricCCEvent(CCEventType::FAIR_DECREASE, _cwnd);
        }
        logMetricCCEvent(CCEventType::NO_CHANGE, _cwnd);
    }
}

void UecSrc::smartt_ecn_aifd_main_loop(bool skip, simtime_picosec delay) {
    bool can_decrease_exp_avg = smartt_wtd_can_decrease();
    if (!skip) {
        _cwnd += ((double)_mtu / _cwnd) * (fair_increase_scaling_factor * _network_bdp);
        logMetricCCEvent(CCEventType::FAIR_INCREASE, _cwnd);
        /* printf("FLow %s FI smartt_ecn_aifd_main_loop: skip %d, delay %f, _cwnd %f\n",
               _flow.str().c_str(), skip, timeAsUs(delay), _cwnd); */
    } else {
        if (can_decrease_exp_avg) {
            _cwnd -= (static_cast<double>(_cwnd) / _bdp * _mtu * fair_decrease_scaling_factor);
            /* printf("FLow %s FD smartt_ecn_aifd_main_loop: skip %d, delay %f, _cwnd %f\n",
               _flow.str().c_str(), skip, timeAsUs(delay), _cwnd); */
            logMetricCCEvent(CCEventType::FAIR_DECREASE, _cwnd);
        }
        logMetricCCEvent(CCEventType::NO_CHANGE, _cwnd);
    }
}

void UecSrc::smartt_ecn_fimd_main_loop(bool skip, simtime_picosec delay) {
    bool can_decrease_exp_avg = smartt_wtd_can_decrease();
    if (!skip) {
        _cwnd += ((double)_mtu / _cwnd) * (fair_increase_scaling_factor * _bdp);
        logMetricCCEvent(CCEventType::FAIR_INCREASE, _cwnd);
    } else {
        if (can_decrease_exp_avg) {
            _cwnd -= _mtu * fair_decrease_scaling_factor;
            logMetricCCEvent(CCEventType::FAIR_DECREASE, _cwnd);
        }
        logMetricCCEvent(CCEventType::NO_CHANGE, _cwnd);
    }
}

void UecSrc::smartt_ecn_fifd_main_loop(bool skip, simtime_picosec delay) {
    bool can_decrease_exp_avg = smartt_wtd_can_decrease();
    if (!skip) {
        _cwnd += ((double)_mtu / _cwnd) * (fair_increase_scaling_factor * _bdp);
        logMetricCCEvent(CCEventType::FAIR_INCREASE, _cwnd);
    } else {
        if (can_decrease_exp_avg) {
            _cwnd -= (static_cast<double>(_cwnd) / _bdp * _mtu * fair_decrease_scaling_factor);
            logMetricCCEvent(CCEventType::FAIR_DECREASE, _cwnd);
        }
        logMetricCCEvent(CCEventType::NO_CHANGE, _cwnd);
    }
}

void UecSrc::smartt_rtt_main_loop(bool skip, simtime_picosec delay) {
    bool     can_decrease_exp_avg = smartt_wtd_can_decrease();
    uint32_t rtt                  = delay + _base_rtt;
    uint32_t _target_rtt          = target_rtt_scaling_factor * _base_rtt;

    //  Case 1 Hybrid Based Increase || RTT Increase
    if (!skip && rtt < _target_rtt) {
        _cwnd += (min(uint32_t((((_target_rtt - rtt) / (double)rtt) * prop_increase_scaling_factor *
                                _mtu * (_mtu / (double)_cwnd))),
                      uint32_t(_mtu)));

        _cwnd += ((double)_mtu / _cwnd) * (fair_increase_scaling_factor * _bdp);
        logMetricCCEvent(CCEventType::PROPORTIONAL_INCREASE, _cwnd);
    }

    //  Case 2 Hybrid Based Decrease || RTT Decrease
    else if (skip && rtt > _target_rtt) {
        if (can_decrease_exp_avg) {
            _cwnd -=
                min(((mult_decrease_scaling_factor * ((rtt - (double)_target_rtt) / rtt) * _mtu) +
                     _cwnd / (double)_bdp * fair_decrease_scaling_factor * _mtu),
                    (double)_mtu / 1);
            logMetricCCEvent(CCEventType::PROPORTIONAL_DECREASE, _cwnd);
        }
        logMetricCCEvent(CCEventType::NO_CHANGE, _cwnd);
    }

    //  Case 3 Gentle Decrease (Window based)
    else if (skip && rtt < _target_rtt) {
        logMetricCCEvent(CCEventType::NO_CHANGE, _cwnd);
    }

    //  Case 4 Do nothing but fairness
    else if (!skip && rtt > _target_rtt) {
        _cwnd += ((double)_mtu / _cwnd) * (fair_increase_scaling_factor * _bdp);
        logMetricCCEvent(CCEventType::FAIR_INCREASE, _cwnd);
    }

    else {
        logMetricCCEvent(CCEventType::NO_CHANGE, _cwnd);
    }
}

void UecSrc::smartt_vanilla_main_loop(bool skip, simtime_picosec delay) {
    bool     can_decrease_exp_avg = smartt_wtd_can_decrease();
    uint32_t rtt                  = delay + _base_rtt;
    uint32_t _target_rtt          = target_rtt_scaling_factor * _base_rtt;

    //  Case 1 Hybrid Based Increase || RTT Increase
    if (!skip && rtt < _target_rtt) {
        _cwnd += (min(uint32_t((((_target_rtt - rtt) / (double)rtt) * prop_increase_scaling_factor *
                                _mtu * (_mtu / (double)_cwnd))),
                      uint32_t(_mtu)));

        _cwnd += ((double)_mtu / _cwnd) * (fair_increase_scaling_factor * _bdp);
        logMetricCCEvent(CCEventType::PROPORTIONAL_INCREASE, _cwnd);
    }

    //  Case 2 Hybrid Based Decrease || RTT Decrease
    else if (skip && rtt > _target_rtt) {
        if (can_decrease_exp_avg) {
            _cwnd -=
                min(((mult_decrease_scaling_factor * ((rtt - (double)_target_rtt) / rtt) * _mtu) +
                     _cwnd / (double)_bdp * fair_decrease_scaling_factor * _mtu),
                    (double)_mtu / 1);
            logMetricCCEvent(CCEventType::PROPORTIONAL_DECREASE, _cwnd);
        }
        logMetricCCEvent(CCEventType::NO_CHANGE, _cwnd);
    }

    //  Case 3 Gentle Decrease (Window based)
    else if (skip && rtt < _target_rtt) {
        if (can_decrease_exp_avg) {
            _cwnd -= (static_cast<double>(_cwnd) / _bdp * _mtu * fair_decrease_scaling_factor);
            logMetricCCEvent(CCEventType::FAIR_DECREASE, _cwnd);
        }
        logMetricCCEvent(CCEventType::NO_CHANGE, _cwnd);
    }

    //  Case 4 Do nothing but fairness
    else if (!skip && rtt > _target_rtt) {
        _cwnd += ((double)_mtu / _cwnd) * (fair_increase_scaling_factor * _bdp);
        logMetricCCEvent(CCEventType::FAIR_INCREASE, _cwnd);
    }

    else {
        logMetricCCEvent(CCEventType::NO_CHANGE, _cwnd);
    }
}

void UecSrc::smartt_update_wtd_state(bool skip) {
    // skip = ECN mark
    exp_avg_ecn = exp_avg_alpha * skip + (1 - exp_avg_alpha) * exp_avg_ecn;
}

bool UecSrc::smartt_wtd_can_decrease() {
    if (!use_exp_avg_ecn) {
        return true;
    }
    if (exp_avg_ecn > exp_avg_ecn_value) {
        return true;
    }
    return false;
}

void UecSrc::updateCwndOnAck_SMARTT(bool skip, simtime_picosec delay, mem_b newly_acked_bytes) {
    smartt_update_wtd_state(skip);
    smartt_update_fast_increase_state(skip, delay);
    bool cwnd_adjust_allowed = smartt_check_and_do_quick_adapt(skip, false);

    if (cwnd_adjust_allowed || !skip) {
        if (smartt_should_fast_increase()) {
            /* printf("FI\n"); */
            smartt_fast_increase();
        } else {
            (this->*smartt_main_loop)(skip, delay);
        }
    }
    clamp_cwnd();
}

void UecSrc::clamp_cwnd() {
    if (_cwnd < _mtu)
        _cwnd = _mtu;

    if (_cwnd > _maxwnd)
        _cwnd = _maxwnd;
}

void UecSrc::updateCwndOnNack_SMARTT(bool skip, mem_b nacked_bytes) {
    _achieved_bytes += 64;  // ACK Size, hardcoded for now
    if (_bytes_ignored >= _bytes_to_ignore) {
        _cwnd -= nacked_bytes;
        _cwnd            = max(_cwnd, (mem_b)_mtu);
        need_quick_adapt = true;
        smartt_quick_adapt(true);
    }
}

void UecSrc::dontUpdateCwndOnNack(bool skip, mem_b nacked_bytes) {}

void UecSrc::update_base_rtt(simtime_picosec raw_rtt, uint16_t packet_size) {
    if (_base_rtt > _raw_rtt && packet_size == _mtu) {
        _base_rtt = _raw_rtt;
        _bdp      = timeAsUs(_raw_rtt) * _nic.linkspeed() / 8000000;
        _maxwnd   = 1.5 * _bdp;

        if (UecSrc::_debug)
            cout << "Reinit BDP and MAXWND to " << _bdp << " " << _maxwnd << " in pkts "
                 << _maxwnd / _mtu << endl;
        if (_bdp == 0)
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " _bdp " << _bdp
                 << " " << _maxwnd << " in pkts " << _maxwnd / _mtu << " raw_rtt "
                 << timeAsUs(_raw_rtt) << endl;
    }
}

void UecSrc::update_delay(simtime_picosec raw_rtt, bool update_avg, bool skip) {
    simtime_picosec delay = _raw_rtt - _base_rtt;
    if (update_avg) {
        if (skip == false && delay > _target_Qdelay) {
            _avg_delay = _delay_alpha * _base_rtt * 0.25 + (1 - _delay_alpha) * _avg_delay;
        } else {
            if (delay > 5 * _base_rtt) {
                double r   = 0.0125;
                _avg_delay = r * delay + (1 - r) * _avg_delay;
            } else {
                _avg_delay = _delay_alpha * delay + (1 - _delay_alpha) * _avg_delay;
            }
        }
    }
    if (_debug_src) {
        cout << "Update delay with sample " << timeAsUs(delay) << " avg is " << timeAsUs(_avg_delay)
             << " base rtt is " << _base_rtt << endl;
    }
}

simtime_picosec UecSrc::get_avg_delay() {
    return _avg_delay;
}

uint16_t UecSrc::get_avg_pktsize() {
    return _mss;  // does not include header
}

void UecSrc::runSleek(uint32_t ooo, UecBasePacket::seq_t cum_ack) {
    mem_b avg_size  = get_avg_pktsize();
    mem_b threshold = min((mem_b)(1.5 * _cwnd), _maxwnd);
    threshold       = max(threshold, 5 * avg_size);

    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " rtx_threshold "
             << threshold / avg_size << " ooo " << ooo << " _highest_rtx_sent " << _highest_rtx_sent
             << " cwnd_in_pkts " << _cwnd / avg_size << " cum_ack " << cum_ack
             << " _probe_timer_when " << timeAsUs(_probe_timer_when) << " highest_sent "
             << _highest_sent << " _backlog " << _backlog << endl;
    }

    if (cum_ack >= _recovery_seqno && _loss_recovery_mode) {
        _loss_recovery_mode = false;
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " exit_loss "
             << endl;
    }

    if (ooo < threshold / avg_size && !_loss_recovery_mode)
        return;

    if (!_loss_recovery_mode && _rtx_queue.empty()) {
        _loss_recovery_mode = true;
        _recovery_seqno     = _highest_sent;
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " enter_loss "
             << " _highest_sent " << _highest_sent << endl;
    }

    // move the packet to the RTX queue
    //_loss_counter < _cwnd/get_avg_pktsize()
    // rtx_seqno < (cum_ack + _cwnd/get_avg_pktsize())
    // _loss_counter < _cwnd/get_avg_pktsize()
    for (UecBasePacket::seq_t rtx_seqno = cum_ack;
         rtx_seqno < _recovery_seqno && rtx_seqno < (cum_ack + _cwnd / get_avg_pktsize());
         rtx_seqno++) {
        if (rtx_seqno < _highest_rtx_sent)
            continue;

        auto i = _tx_bitmap.find(rtx_seqno);
        if (i == _tx_bitmap.end()) {
            // this means this packet seqno has been acked.
            continue;
        }

        if (_rtx_queue.find(rtx_seqno) != _rtx_queue.end()) {
            continue;
        }

        if (_flow.flow_id() == _debug_flowid) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " rtx_seqno "
                 << rtx_seqno << " _highest_recv_seqno " << _highest_recv_seqno
                 << " recovery_seqno " << _recovery_seqno << endl;
        }

        _stats._sleek_counter++;

        mem_b pkt_size = i->second.pkt_size;
        assert(pkt_size >= _hdr_size);  // check we're not seeing NACKed RTS packets.
        auto            seqno     = i->first;
        simtime_picosec send_time = i->second.send_time;
        _tx_bitmap.erase(i);
        assert(_tx_bitmap.find(seqno) == _tx_bitmap.end());  // xxx remove when working

        _in_flight -= pkt_size;
        // mark_packet_for_retransmission(seqno, pkt_size);

        // _send_times.erase(send_time);
        delFromSendTimes(send_time, rtx_seqno);
        _highest_rtx_sent = seqno + 1;
        cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
             << " sleek queue rtx packet for psn " << seqno << endl;
        queueForRtx(seqno, pkt_size);

        if (send_time == _rto_send_time) {
            if (_flow.flow_id() == _debug_flowid) {
                cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                     << " rtx_seqno " << rtx_seqno << " send_time " << timeAsUs(send_time)
                     << " _rto_send_time " << timeAsUs(_rto_send_time) << " recalculateRTO" << endl;
            }
            recalculateRTO();
        }
        // penalizePath(ev, 1);
    }
    sendIfPermitted();
}

void UecSrc::processNack(const UecNackPacket& pkt) {
    _stats.nacks_received++;

    // auto pullno = pkt.pullno();
    // handlePull(pullno);

    if (_load_balancing_algo == RSS) {
        int sid = rssSubflowFromEv(pkt.ev());
        _rss_last_feedback_time[sid] = eventlist().now();
        _rss_noack_deadline[sid]     = eventlist().now() + _rss_noack_timeout;
    }

    auto nacked_seqno = pkt.ref_ack();
    /* cout << timeAsUs(eventlist().now()) << " " << eventlist().now() << " flow " << pkt.flow_id()
         << " receive nack packet for psn " << pkt.ref_ack() << " ev " << pkt.ev() << " " << _done_sending << endl; */
    pflr4SenderReceiveEv(pkt.ref_ack(), pkt.ev());
    if (_debug_src) {
        cout << _flow.str() << " " << _nodename << " processNack nacked: " << nacked_seqno
             << " flow " << _flow.str() << endl;
    }

    uint16_t ev = pkt.ev();
    // what should we do when we get a NACK with ECN_ECHO set?  Presumably ECE is superfluous?
    // bool ecn_echo = pkt.ecn_echo();

    // move the packet to the RTX queue
    auto i = _tx_bitmap.find(nacked_seqno);
    if (i == _tx_bitmap.end()) {
        if (_debug_src)
            cout << _flow.str() << " " << "Didn't find NACKed packet in _active_packets flow "
                 << _flow.str() << endl;

        // this abort is here because this is unlikely to happen in
        // simulation - when it does, it is usually due to a bug
        // elsewhere.  But if you discover a case where this happens
        // for real, remove the abort and uncomment the return below.
        //abort();
        //printf("Should abort here\n");
        // this can happen when the NACK arrives later than a cumulative ACK covering the NACKed
        // packet.
        return;
    }

    mem_b pkt_size = i->second.pkt_size;

    assert(pkt_size >= _hdr_size);  // check we're not seeing NACKed RTS packets.
    if (pkt_size == _hdr_size) {
        _stats.rts_nacks++;
    }

    auto            seqno     = i->first;
    simtime_picosec send_time = i->second.send_time;

    // The average queue delay is not updated, since the packet was trimmed.
    _raw_rtt = eventlist().now() - send_time;
    logMetricAck(_raw_rtt, _mtu, true, pkt.ecn_echo());
    if (_raw_rtt > _base_rtt) {
        update_delay(_raw_rtt, false, true);
    }

    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " ev " << ev
             << " seqno " << seqno << " trimming " << endl;
    }
    if (_sender_based_cc) {
        (this->*updateCwndOnNack)(ev, pkt_size);
        //logMetricCwnd(_cwnd, _raw_rtt);
    }

    if (_debug_src)
        cout << _flow.str() << " " << _nodename << " erasing send record, seqno: " << seqno
             << " flow " << _flow.str() << endl;
    _tx_bitmap.erase(i);
    assert(_tx_bitmap.find(seqno) == _tx_bitmap.end());  // xxx remove when working

    _in_flight -= pkt_size;
    // assert(_in_flight >= 0);

    // _send_times.erase(send_time);
    delFromSendTimes(send_time, seqno);

    stopSpeculating();
/* cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
        << " nack queue rtx packet for psn " << seqno << endl; */
    queueForRtx(seqno, pkt_size);

    if (send_time == _rto_send_time) {
        recalculateRTO();
    }
    PathFeedback nack_feedback = {PATH_NACK, _raw_rtt};
    (this->*processEv)(ev, nack_feedback);

    if (_pflr_pace_rtx && !_rtx_queue.empty()) {
        // Cancel pending proactive tail check - NACKs are handling recovery
        if (_proactive_tail_check_scheduled && _rtx_pace_pending) {
            eventlist().cancelPendingSourceByHandle(*this, _rtx_pace_handle);
            _rtx_pace_pending = false;
            _rtx_pace_handle = eventlist().nullHandle();
            _proactive_tail_check_scheduled = false;
        }
        _proactive_tail_rounds = 0;  // NACK = real feedback, reset
        if (!_rtx_pace_pending) {
            scheduleRtxPacedSend();
        }
        // Reset RTO from now - the original send_time-based RTO would fire
        // before paced retransmissions complete. We already know the loss
        // (via NACK), so RTO only needs to catch retransmission failures.
        cancelRTO();
        startRTO(eventlist().now());
    } else {
        sendIfPermitted();
    }
}

void UecSrc::processPull(const UecPullPacket& pkt) {
    _stats.pulls_received++;

    auto pullno = pkt.pullno();
    if (_debug_src)
        cout << timeAsUs(eventlist().now()) << " flow " << _flow.str() << " " << _nodename
             << " processPull " << pullno << " flow " << _flow.str() << " SP " << pkt.is_slow_pull()
             << endl;
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " processPull "
             << pullno << " SP " << pkt.is_slow_pull() << endl;
    }
    stopSpeculating();
    handlePull(pullno);
    sendIfPermitted();
}

void UecSrc::doNextEvent() {
    /* if (flowId() == 5822) {
        cout << timeAsUs(eventlist().now()) << " flow " << _name << " doNextEvent " << _rtx_timeout_pending
             << " flowid " << _flow.flow_id() << endl;
    } */
    if (_rtx_timeout_pending && eventlist().now() == _rtx_timeout) {
        clearRTO();
        assert(_logger == 0);

        if (_logger)
            _logger->logUec(*this, UecLogger::UEC_TIMEOUT);

        rtxTimerExpired();
    }

    // RACK-TLP: PTO timeout check
    if (_tlp_pto_pending && eventlist().now() == _tlp_pto_timeout) {
        tlpSendProbe();
    }

    // PFLD RTX pacing timer
    if (_rtx_pace_pending && eventlist().now() == _rtx_pace_timer) {
        _rtx_pace_pending = false;
        _rtx_pace_handle = eventlist().nullHandle();
        if (!_done_sending && !_rtx_queue.empty()) {
            sendIfPermitted();
            if (!_rtx_queue.empty() && _pflr_pace_rtx) {
                scheduleRtxPacedSend();
            }
        }
        // Delayed tail check: timer fired after SRTT wait.  Anything still
        // in _send_times was NOT acknowledged - retransmit it.
        if (_proactive_tail_check_scheduled && _rtx_queue.empty()
            && !_send_times.empty() && !_done_sending) {
            _proactive_tail_check_scheduled = false;
            _proactive_tail_rounds++;
            // Collect seqnos to retransmit, then clean up _send_times/_tx_bitmap
            // so createSendRecord in sendRtxPacket won't create duplicates.
            std::vector<std::pair<UecBasePacket::seq_t, mem_b>> to_rtx;
            for (auto it = _send_times.begin(); it != _send_times.end(); ++it) {
                auto seqno = it->second;
                if (_rtx_queue.find(seqno) != _rtx_queue.end())
                    continue;
                auto tx_rec = _tx_bitmap.find(seqno);
                if (tx_rec != _tx_bitmap.end()) {
                    to_rtx.push_back({seqno, tx_rec->second.pkt_size});
                }
            }
            for (auto& [seqno, pkt_size] : to_rtx) {
                // Remove old send_times and tx_bitmap entries before queueing
                auto tx_rec = _tx_bitmap.find(seqno);
                if (tx_rec != _tx_bitmap.end()) {
                    delFromSendTimes(tx_rec->second.send_time, seqno);
                    _tx_bitmap.erase(tx_rec);
                }
                if (_sender_based_cc) {
                    _in_flight -= pkt_size;
                }
                _rtx_queue.emplace(seqno, pkt_size);
                _rtx_backlog += pkt_size;
            }
            if (!_rtx_queue.empty()) {
                scheduleRtxPacedSend();
                cancelRTO();
                startRTO(eventlist().now());
            }
        }
        // After paced RTX queue drains, schedule a delayed check after SRTT.
        // By waiting SRTT, any successfully-delivered retransmissions will have
        // their ACKs back and be removed from _send_times.  Whatever remains
        // is genuinely lost (tail losses or secondary drops) and can be
        // retransmitted with zero spurious retransmissions.
        // Guard _backlog == 0 prevents firing on long-lived flows.
        // Limit to 3 rounds to avoid infinite loops when drops persist.
        if (_rtx_queue.empty() && !_send_times.empty() && !_done_sending && _backlog == 0
            && !_proactive_tail_check_scheduled && _proactive_tail_rounds < 3) {
            simtime_picosec srtt = (_raw_rtt > 0) ? _raw_rtt :
                                   (_base_rtt > 0) ? _base_rtt : _rtt;
            _proactive_tail_check_scheduled = true;
            _rtx_pace_next = eventlist().now() + srtt;
            _rtx_pace_timer = _rtx_pace_next;
            _rtx_pace_pending = true;
            _rtx_pace_handle = eventlist().sourceIsPendingGetHandle(
                *this, _rtx_pace_next);
        }
    }

    if (_highest_sent == 0) {
        if (_debug_src)
            cout << _flow.str() << " " << "Starting flow " << _name << endl;
        startFlow();

        // schedule first proactive probe
        if (_pflr_proactive_probe && (_pflr_proactive_probe_pkt_count == 0)){
            if (!_pflr_probe_is_bootstrapped){
                simtime_picosec estimated_rtt = _base_rtt;
                for(int slot_id=0; slot_id < getNoSlots(); slot_id++){
                    if ((_pflr_slots_probe_is_pending[slot_id]) && !(_pflr_slots_probe_send_handle[slot_id] == eventlist().nullHandle())) {
                        // cancel previous
                        cout << timeAsUs(eventlist().now()) << " flow "<< _flow.flow_id() << " probe is pending at " << _pflr_slots_scheduled_probe_send_time[slot_id] <<", cancel " << endl;
                        eventlist().cancelPendingSourceByHandle(*this, _pflr_slots_probe_send_handle[slot_id]);
                    }
                    _pflr_slots_scheduled_probe_send_time[slot_id] = eventlist().now() + estimated_rtt;
                    _pflr_slots_probe_send_handle[slot_id] = eventlist().sourceIsPendingGetHandle(*this, eventlist().now() + estimated_rtt);
                    if (_pflr_slots_probe_send_handle[slot_id] == eventlist().nullHandle()) {
                        _pflr_slots_probe_is_pending[slot_id] = 0;
                    } else {
                        _pflr_slots_probe_is_pending[slot_id] = 1;
                    }
                }
                _pflr_probe_is_bootstrapped = 1;
                //cout << timeAsUs(eventlist().now()) << " flow "<< _flow.flow_id() << " dynamic probe initialized " << endl;
            }
        }
    }

    if (_load_balancing_algo == RSS) {
        for (int sid = 0; sid < _rss_params._rss_number_of_subflows; ++sid) {
            if (eventlist().now() >= _rss_noack_deadline[sid]) {
                //rssBumpEntropyForSubflow(sid, "no-ack-timeout");
                _rss_noack_deadline[sid] = eventlist().now() + _rss_noack_timeout;
            }
        }
    }

    if (_sender_based_cc && _enable_sleek) {
        if (_probe_timer_when != 0 && _probe_timer_when == eventlist().now()) {
            cout << timeAsUs(eventlist().now()) << " doNextEvent probe " << _rtx_timeout_pending
                 << " flowid " << _flow.flow_id() << endl;
            sendProbe();
        }
    }
    // pfld proactive probe
    if (_pflr_proactive_probe && (_pflr_proactive_probe_pkt_count == 0)) {
        auto it = std::find(_pflr_slots_scheduled_probe_send_time.begin(),
                            _pflr_slots_scheduled_probe_send_time.end(),
                            eventlist().now());
        if (it != _pflr_slots_scheduled_probe_send_time.end()) {
            int slot_id = std::distance(_pflr_slots_scheduled_probe_send_time.begin(), it);
            _pflr_slots_scheduled_probe_send_time[slot_id] = 0;
           if ((_highest_sent > (long unsigned int)slot_id) && _pflr_slots_probe_is_pending[slot_id]){
                _pflr_slots_probe_is_pending[slot_id] = 0;
                // enqueue probe
                auto seq_no = _slots_last_data_packet_info[slot_id].first;
                auto ev     = _slots_last_data_packet_info[slot_id].second;
                if ((_slots_last_proactive_probe_psn[slot_id] < seq_no) && (_backlog > 0)) {
                    enqueueProbe(seq_no, ev, UecDataPacket::PROACTIVE_DATA);
                    sendIfPermitted();
                }
            }
        }
    }
}

void UecSrc::setFlowsize(uint64_t flow_size_in_bytes) {
    _flow_size = flow_size_in_bytes;
}

void UecSrc::startFlow() {
    //cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id() << " cwnd " << _cwnd << endl;
    _flow_start_time = eventlist().now();
    //_cwnd = _maxwnd;
    _credit = _maxwnd;

    if (_debug_src)
        cout << _flow.str() << " " << "startflow " << _flow._name << " CWND " << _cwnd << " at "
             << timeAsUs(eventlist().now()) << " flow " << _flow.str() << endl;

    if (flowId() == 4512 || flowId() == 1 || flowId() == 500 || flowId() == 499) {
        cout << "Flow " << _name << " flowId " << flowId() << " " << _nodename << " starting at "
         << timeAsUs(eventlist().now()) << endl;
    }
    last_ack_sent_time = eventlist().now();
    

    if (_flow_logger) {
        _flow_logger->logEvent(_flow, *this, FlowEventLogger::START, _flow_size, 0);
    }
    clearRTO();
    _in_flight   = 0;
    _pull_target = INIT_PULL;
    _pull        = INIT_PULL;
    _last_rts    = 0;
    // backlog is total amount of data we expect to send, including headers
    _backlog             = ceil(((double)_flow_size) / _mss) * _hdr_size + _flow_size;
    _rtx_backlog         = 0;
    _send_blocked_on_nic = false;
    while (_send_blocked_on_nic == false && credit() > 0 && _backlog > 0) {
        if (_debug_src) {
            cout << _flow.str() << " " << "requestSending 0 " << endl;
        }

        const Route* route = _nic.requestSending(*this);
        if (_flow.flow_id() == _debug_flowid) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                 << " requestSending " << _nic.activeSources() << endl;
        }
        if (route) {
            // if we're here, there's no NIC queue
            mem_b sent_bytes = sendNewPacket(*route);
            if (sent_bytes > 0) {
                _nic.startSending(*this, sent_bytes, route);
            } else {
                _nic.cantSend(*this);
            }
        } else {
            _send_blocked_on_nic = true;
            return;
        }
    }
}

mem_b UecSrc::credit() const {
    return _credit;
}

void UecSrc::spendCredit(mem_b pktsize) {
    if (_receiver_based_cc) {
        assert(_credit > 0);
        _credit -= pktsize;
    }
}

void UecSrc::stopSpeculating() {
    // this doesn't really do a lot, except prevent us retransmitting
    // on an RTO before we've heard back from the receiver
    if (_speculating) {
        _speculating = false;
        if (_credit > 0)
            _credit = 0;

        if (_flow.flow_id() == _debug_flowid) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                 << " stopSpeculating _credit " << _credit << endl;
        }
    }
}

UecBasePacket::pull_quanta UecSrc::computePullTarget() {
    if (!_receiver_based_cc)
        return 0;

    mem_b pull_target = _backlog + _rtx_backlog;
    // mem_b pull_target = _backlog;

    if (_sender_based_cc) {
        if (pull_target > _cwnd + _mtu) {
            pull_target = _cwnd + _mtu;
        }
    }

    if (pull_target > _maxwnd) {
        pull_target = _maxwnd;
    }

    pull_target -= _credit;

    if (_speculating && pull_target < _mtu &&
        _backlog >
            0)  // always request at least an MTU of credit if we have a backlog, regardless of how
                // much credit we have already have. Saves our bacon for short transfers
        pull_target = _mtu;

    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " _credit "
             << _credit << " pull_target " << _pull_target << endl;
    }

    if (_nic.activeSources() > 1)
        pull_target /= _nic.activeSources();

    pull_target += UecBasePacket::unquantize(_pull);

    UecBasePacket::pull_quanta quant_pull_target = UecBasePacket::quantize_ceil(pull_target);

    if (_debug_src) {
        cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " " << " " << nodename()
             << " pull_target: " << UecBasePacket::unquantize(quant_pull_target) << " beforequant "
             << pull_target << " pull " << UecBasePacket::unquantize(_pull) << " diff "
             << UecBasePacket::unquantize(quant_pull_target - _pull) << " credit " << _credit
             << " backlog " << _backlog << " rtx_backlog " << _rtx_backlog << " active sources "
             << _nic.activeSources() << " cwnd " << _cwnd << " maxwnd " << _maxwnd << endl;
    }
    return quant_pull_target;
}

mem_b UecSrc::getNextPacketSize() {
    if (_rtx_queue.empty()) {
        if (_backlog == 0) {
            return 0;
        }
        assert(((mem_b)_highest_sent - _stats.rts_pkts_sent) * _mss < _flow_size);
        mem_b full_pkt_size = _mtu;
        if (_backlog < _mtu) {
            full_pkt_size = _backlog;
        }
        return full_pkt_size;
    } else {
        assert(!_rtx_queue.empty());
        mem_b full_pkt_size = _rtx_queue.begin()->second;
        return full_pkt_size;
    }
}

void UecSrc::sendIfPermitted() {
/*     cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id() << " cwnd " << _cwnd << endl;
 */    // send if the NIC, credit and window allow.
    // enqueueDynamicProactiveProbe();
    const bool probe_is_next = _rtx_queue.empty() && !_probe_queue.empty();
    if (!probe_is_next && (_receiver_based_cc && credit() <= 0)) {
        // can send if we have *any* credit, but we don't
        return;
    }
    // cout << timeAsUs(eventlist().now()) << " " << nodename() << " FOO " << _cwnd << " " <<
    // _in_flight << endl;
    mem_b next_packet_size = getNextPacketSize();
    if (!probe_is_next && _sender_based_cc) {
        if (!can_send_NSCC(next_packet_size)) {
            return;
        }
    }

    if (_rtx_queue.empty()) {
        if (_backlog == 0) {
            if (_probe_queue.empty()) {
                return;
            }
        }
    }
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
             << " sendIfPermitted requestSending _send_blocked_on_nic " << _send_blocked_on_nic
             << " activesenders " << _nic.activeSources() << endl;
    }
    if (_send_blocked_on_nic) {
        // the NIC already knows we want to send
        if (_flow.flow_id() == _debug_flowid) {
            for (auto it = _nic._active_srcs.begin(); it != _nic._active_srcs.end(); ++it) {
                UecSrc* queued_src = *it;
                cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                     << " sendIfPermitted block" << queued_src->flow()->flow_id() << " _nic "
                     << _nic.src_id() << endl;
                ;
            }
        }
        return;
    }

    // we can send if the NIC lets us.
    if (_debug_src)
        cout << _flow.str() << " " << "requestSending 1\n";
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
             << " sendIfPermitted requestSending " << endl;
    }
    const Route* route = _nic.requestSending(*this);
    if (route) {
        mem_b sent_bytes = sendPacket(*route);
        if (sent_bytes > 0) {
            _nic.startSending(*this, sent_bytes, route);
            sendIfPermitted();
        } else {
            _nic.cantSend(*this);
        }
    } else {
        // we can't send yet, but NIC will call us back when we can
        _send_blocked_on_nic = true;
        return;
    }
}

// if sendPacket got called, we have already asked the NIC for
// permission, and we've already got both credit and cwnd to send, so
// we will likely be sending something (sendNewPacket can return 0 if
// we only had speculative credit we're not allowed to use though)
mem_b UecSrc::sendPacket(const Route& route) {
    // Loss recovery has priority over optional diagnostic probes.
    if (!_rtx_queue.empty()) {
        return sendRtxPacket(route);
    } else if (!_probe_queue.empty()) {
        return sendProbePacket(route);
    } else {
        return sendNewPacket(route);
    }
}

void UecSrc::startRTO(simtime_picosec send_time) {
    if (!_rtx_timeout_pending) {
        // timer is not running - start it
        _rtx_timeout_pending = true;
        _rtx_timeout         = send_time + _rto;
        _rto_send_time       = send_time;

        if (_rtx_timeout < eventlist().now())
            _rtx_timeout = eventlist().now();

        if (_debug_src)
            cout << "Start timer at " << timeAsUs(eventlist().now()) << " source " << _flow.str()
                 << " expires at " << timeAsUs(_rtx_timeout) << " flow " << _flow.str() << endl;

        _rto_timer_handle = eventlist().sourceIsPendingGetHandle(*this, _rtx_timeout);
        if (_rto_timer_handle == eventlist().nullHandle()) {
            // this happens when _rtx_timeout is past the configured simulation end time.
            _rtx_timeout_pending = false;
            if (_debug_src)
                cout << "Cancel timer because too late for flow " << _flow.str() << endl;
        }
    } else {
        // timer is already running
        if (send_time + _rto < _rtx_timeout) {
            // RTO needs to expire earlier than it is currently set
            cancelRTO();
            startRTO(send_time);
        }
    }
}

void UecSrc::clearRTO() {
    // clear the state
    _rto_timer_handle    = eventlist().nullHandle();
    _rtx_timeout_pending = false;

    if (_debug_src)
        cout << "Clear RTO " << timeAsUs(eventlist().now()) << " source " << _flow.str() << endl;
}

void UecSrc::cancelRTO() {
    if (_rtx_timeout_pending) {
        // cancel the timer
        eventlist().cancelPendingSourceByHandle(*this, _rto_timer_handle);
        clearRTO();
    }
}

uint16_t UecSrc::nextEntropy_flowlet(UecBasePacket::seq_t seq_no){
    if (eventlist().now() > last_pkt_flowlet + flowlet_timeout && eventlist().now() > flowlet_timeout_wait) {
        flowlet_entropy = rand() % _no_of_paths;
        flowlet_timeout_wait = eventlist().now() + _rto * 1;
        printf("%s Flowlet reroute - New entropy is %d (\n", _nodename.c_str(), flowlet_entropy);
    }

    return flowlet_entropy;
}

void UecSrc::processEv_flowlet(uint16_t path_id, PathFeedback feedback) {
    last_pkt_flowlet = eventlist().now();      
}

void UecSrc::processEv_mixed(uint16_t path_id, PathFeedback path_feedback) {
    processEv_bitmap(path_id, path_feedback);
    processEv_REPS(path_id, path_feedback);
}

void UecSrc::processEv_bitmap(uint16_t path_id, PathFeedback path_feedback) {
    // _no_of_paths must be a power of 2
    uint16_t mask = _no_of_paths - 1;
    path_id &= mask;  // only take the relevant bits for an index

    if (path_feedback.feedback_bit == PATH_GOOD && !_ev_skip_bitmap[path_id])
        _ev_skip_count++;

    uint8_t penalty = 0;

    if (path_feedback.feedback_bit == PathFeedbackBit::PATH_ECN)
        penalty = 1;
    else if (path_feedback.feedback_bit == PathFeedbackBit::PATH_NACK)
        penalty = 4;
    else if (path_feedback.feedback_bit == PathFeedbackBit::PATH_TIMEOUT)
        penalty = _max_penalty;

    _ev_skip_bitmap[path_id] += penalty;
    if (_ev_skip_bitmap[path_id] > _max_penalty) {
        _ev_skip_bitmap[path_id] = _max_penalty;
    }
}

void UecSrc::processEv_REPS(uint16_t path_id, PathFeedback path_feedback) {
    if (path_feedback.feedback_bit == PATH_GOOD) {
        _next_pathid.push_back(path_id);
        if (_debug) {
            cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " REPS Add " << path_id
                 << " " << _next_pathid.size() << endl;
        }
    }
}

void UecSrc::processEv_oblivious(uint16_t path_id, PathFeedback path_feedback) {
    return;
}

void UecSrc::pflr4PrintEvMap(uint16_t slot_id) {
    cout << "Queue : ";
    for (uint32_t idx = 0; idx < _pflr4_slots_ev_queue[slot_id].size(); idx++) {
        cout << to_string(_pflr4_slots_ev_queue[slot_id][idx]) << ", ";
    }
    cout << endl;
    cout << "Old EV: ";
    for (uint32_t idx = 0; idx < _pflr4_slots_ev_map_old_ev[slot_id].size(); idx++) {
        cout << to_string(_pflr4_slots_ev_map_old_ev[slot_id][idx]) << ", ";
    }
    cout << endl;
    cout << "New EV: ";
    for (uint32_t idx = 0; idx < _pflr4_slots_ev_map_new_ev[slot_id].size(); idx++) {
        cout << to_string(_pflr4_slots_ev_map_new_ev[slot_id][idx]) << ", ";
    }
    cout << endl;
    cout << "Counts: ";
    for (uint32_t idx = 0; idx < _pflr4_slots_ev_map_countdown[slot_id].size(); idx++) {
        cout << to_string(_pflr4_slots_ev_map_countdown[slot_id][idx]) << ", ";
    }
    cout << endl;
}

void UecSrc::pflr0RegisterSend(UecBasePacket::seq_t seq_no, uint16_t ev) {
    // remove all earlier record of seq_no
    for (auto& pair : pflr0_sender_record) {
        auto& record = pair.second;
        record.erase(remove(record.begin(), record.end(), seq_no), record.end());
    }
    // print content
    // cout << "Current Sender record: \n";
    // for (const auto& pair : pflr0_sender_record) {
    //     cout << "EV: " << pair.first << " -> [ ";
    //     for (auto psn : pair.second) {
    //         cout << psn << " ";
    //     }
    //     std::cout << "]\n";
    // }

    // Append the value to the list (whether it's a new key or not)
    pflr0_sender_record[ev].push_back(seq_no);
    // remove empty ev
    for (auto pair = pflr0_sender_record.begin(); pair != pflr0_sender_record.end();) {
        if (pair->second.empty()) {
            pair = pflr0_sender_record.erase(pair);  // Erase and advance the iterator
        } else {
            ++pair;  // Just advance the iterator
        }
    }
}

// when translation timeout (not used for now)
void UecSrc::pflr4CleanUpTimeout(uint16_t slot_id) {
    auto now = eventlist().now();
    while ((!_pflr4_slots_ev_map_timeout[slot_id].empty()) &&
           (now >= _pflr4_slots_ev_map_timeout[slot_id].front())) {
        // first element is timeout
        _pflr4_slots_ev_map_old_ev[slot_id].erase(_pflr4_slots_ev_map_old_ev[slot_id].begin());
        _pflr4_slots_ev_map_new_ev[slot_id].erase(_pflr4_slots_ev_map_new_ev[slot_id].begin());
        _pflr4_slots_ev_map_timeout[slot_id].erase(_pflr4_slots_ev_map_timeout[slot_id].begin());
        _pflr4_slots_ev_map_countdown[slot_id].erase(
            _pflr4_slots_ev_map_countdown[slot_id].begin());
    }
    // cout << "Old EV size. Cleaned up timeout, flow " << _flow.flow_id() << " slot " << slot_id <<
    // " size: " << _pflr4_slots_ev_map_old_ev[slot_id].size() << endl;
}

// when send
uint16_t UecSrc::pflr4LookupEv(uint16_t slot_id, uint16_t old_ev) {
    uint16_t current_ev = old_ev;
    for (size_t idx = 0; idx < _pflr4_slots_ev_map_old_ev[slot_id].size(); idx++) {
        if (_pflr4_slots_ev_map_old_ev[slot_id][idx] == current_ev) {
            current_ev =
                _pflr4_slots_ev_map_new_ev[slot_id][idx];  // If found, return corresponding new ev
            // decrease counter
            _pflr4_slots_ev_map_countdown[slot_id][idx] -= 1;
        }
    }
    // remove if goes to 0
    if (_pflr4_slots_ev_map_old_ev[slot_id].size() > 0) {
        for (size_t idx = _pflr4_slots_ev_map_old_ev[slot_id].size() - 1; idx >= 0; idx--) {
            if (_pflr4_slots_ev_map_countdown[slot_id][idx] <= 0) {
                _pflr4_slots_ev_map_old_ev[slot_id].erase(
                    _pflr4_slots_ev_map_old_ev[slot_id].begin() + idx);
                _pflr4_slots_ev_map_new_ev[slot_id].erase(
                    _pflr4_slots_ev_map_new_ev[slot_id].begin() + idx);
                _pflr4_slots_ev_map_timeout[slot_id].erase(
                    _pflr4_slots_ev_map_timeout[slot_id].begin() + idx);
                _pflr4_slots_ev_map_countdown[slot_id].erase(
                    _pflr4_slots_ev_map_countdown[slot_id].begin() + idx);
            }
        }
    }
    return current_ev;
}

// when change
void UecSrc::pflr4RegisterTranslation(uint16_t slot_id, uint16_t old_ev, uint16_t new_ev) {
    // first cleanup all timeouts
    // pflr4CleanUpTimeout(slot_id);
    // update all old translations
    // for (int idx = 0; idx < _pflr4_slots_ev_map_new_ev[slot_id].size(); idx++) {
    //     _pflr4_slots_ev_map_new_ev[slot_id][idx] = new_ev;
    // }
    // append new translations
    auto now = eventlist().now();
    _pflr4_slots_ev_map_old_ev[slot_id].push_back(old_ev);
    _pflr4_slots_ev_map_new_ev[slot_id].push_back(new_ev);
    _pflr4_slots_ev_map_timeout[slot_id].push_back(now + _rto);
    _pflr4_slots_ev_map_countdown[slot_id].push_back(_pflr4_no_packet_per_slot);
    // cout << "Old EV size. registered translation, flow " << _flow.flow_id() << " slot " <<
    // slot_id << " size: " << _pflr4_slots_ev_map_old_ev[slot_id].size() << endl;
}

void UecSrc::pflr4RegisterRto(UecBasePacket::seq_t seq_no) {
    auto item_position = _pflr4_rto_rtx_table.find(seq_no);
    if (item_position != _pflr4_rto_rtx_table.end()) {
        cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id() << " double rto for psn "
             << seq_no << endl;
    }
    _pflr4_rto_rtx_table.insert(seq_no);
}

bool UecSrc::pflr4IsRtoRtx(UecBasePacket::seq_t seq_no) {
    auto item_position = _pflr4_rto_rtx_table.find(seq_no);
    if (item_position != _pflr4_rto_rtx_table.end()) {
        _pflr4_rto_rtx_table.erase(item_position);
        return true;
    } else {
        return false;
    }
}


int UecSrc::rssSubflowFromEv(uint16_t path_id) const {
    int subflow_id = path_id & ((1 << _rss_params._rss_number_of_subflow_bits) - 1);
    if (UecSrc::usePflr() && _pflr_scheme_id != 0) {
        if (_pflr_scheme_id == 2) {
            uint16_t n_bits_slot_id = (uint16_t)log2(_rss_params._rss_number_of_subflows);
            subflow_id = path_id >> (16 - n_bits_slot_id);
        } else if (_pflr_scheme_id == 3 || _pflr_scheme_id == 4) {
            subflow_id = path_id % _rss_params._rss_number_of_subflows;
        }
    }
    return subflow_id;
}

void UecSrc::rssBumpEntropyForSubflow(int sid, const char* reason) {
    // Validate subflow index
    if (_load_balancing_algo != RSS) return;
    if (sid < 0 || sid >= (int)_rss_state_entropies.size()) return;

    auto now = eventlist().now();

    // Cooldown guard (only if the vector was initialized)
    if (sid < (int)_rss_last_reroute_time.size()) {
        if (now - _rss_last_reroute_time[sid] < _rss_reroute_cooldown) return;
        _rss_last_reroute_time[sid] = now;
    }

    // Case 1: No PFLR (or baseline scheme / background ECMP) -> legacy entropy bump.
    if (!usePflr() || _pflr_scheme_id == 0 || backgroundECMPFlow) {
        _rss_state_entropies[sid] =
            (rand() << _rss_params._rss_number_of_subflow_bits) + sid;
        return;
    }

    // From here on: PFLR-aware bump.
    // Do nothing if no backlog (nothing more to send soon).
    if (_backlog == 0) return;

    // Scheme-specific handling
    switch (_pflr_scheme_id) {
        case 2: {
            // Generation-style monotonic EV (already used in periodic update path).
            uint16_t new_ev      = _current_evs[sid] + 1;
            _previous_evs[sid]   = _current_evs[sid];
            _current_evs[sid]    = new_ev;
            _rss_state_entropies[sid] = new_ev;
            _ev_status[sid]      = EvStatus::IS_NEW;

            // Optional probe to close previous section
            if (!_pflr_disable_probe && _highest_sent > 0) {
                UecBasePacket::seq_t last_sent_psn = _highest_sent - 1;
                UecBasePacket::seq_t last_sent_psn_this_slot =
                    last_sent_psn - (last_sent_psn - sid) % _rss_params._rss_number_of_subflows;
                enqueueProbe(last_sent_psn_this_slot, _previous_evs[sid], UecDataPacket::SECTION_END);
            }
            break;
        }

        case 3:
        case 4: {
            // In these schemes the EV encodes a future PSN for that slot.
            if (_highest_sent == 0) return;              // Nothing sent yet.
            if (_pflr_scheme_id == 4 && pflr4GetStageId() == 0) return; // Still initializing.

            uint32_t last_sent_psn = _highest_sent - 1;
            uint32_t last_sent_psn_this_slot =
                last_sent_psn - (last_sent_psn - sid) % _rss_params._rss_number_of_subflows;
            uint32_t next_psn_this_slot =
                last_sent_psn_this_slot + _rss_params._rss_number_of_subflows;

            _previous_evs[sid]        = _current_evs[sid];
            _current_evs[sid]         = (uint16_t)next_psn_this_slot;
            _rss_state_entropies[sid] = _current_evs[sid];
            _ev_status[sid]           = EvStatus::IS_NEW;

            if (_pflr_scheme_id == 4) {
                // Register translation so outstanding packets using old EV can be mapped.
                pflr4RegisterTranslation(sid, _previous_evs[sid], _current_evs[sid]);
            }

            if (!_pflr_disable_probe) {
                enqueueProbe(last_sent_psn_this_slot,
                             _previous_evs[sid],
                             UecDataPacket::SECTION_END);
            }
            break;
        }

        default: {
            // Fallback: legacy random (should not normally happen).
            _rss_state_entropies[sid] =
                (rand() << _rss_params._rss_number_of_subflow_bits) + sid;
            break;
        }
    }

    // (Optional) Debug: uncomment if needed
    // if (_pflr_print_debug_msg) {
    //     cout << timeAsUs(now) << " flow " << _flow.flow_id()
    //          << " rssBumpEntropyForSubflow sid=" << sid
    //          << " reason=" << reason
    //          << " new_ev=" << _rss_state_entropies[sid]
    //          << " scheme=" << _pflr_scheme_id << endl;
    // }
}

void UecSrc::processEv_rss(uint16_t path_id, PathFeedback path_feedback) {
    int subflow_id = path_id & ((1 << _rss_params._rss_number_of_subflow_bits) - 1);
    if (UecSrc::usePflr() && _pflr_scheme_id != 0 && !backgroundECMPFlow) {
        if (_pflr_scheme_id == 2) {
            uint16_t n_bits_slot_id =
                (uint16_t)log2(_rss_params._rss_number_of_subflows);  // #bits for slot id
            subflow_id = path_id >> (16 - n_bits_slot_id);
        } else if (_pflr_scheme_id == 3) {
            subflow_id = path_id % _rss_params._rss_number_of_subflows;
        } else if (_pflr_scheme_id == 4) {
            subflow_id = path_id % _rss_params._rss_number_of_subflows;
        } else {
            abort();
        }
    }
    _rss_state_number_of_ev_pkts[subflow_id]++;
    if (path_feedback.feedback_bit == PathFeedbackBit::PATH_ECN) {
        _rss_state_ecn[subflow_id]++;  // Update ECN counter
    }
    _rss_state_mean_rtt[subflow_id] += path_feedback.rtt_estimate;  // Update RTT estimate
    if (path_feedback.rtt_estimate > _rss_state_worse_rtt[subflow_id]) {
        _rss_state_worse_rtt[subflow_id] = path_feedback.rtt_estimate;
    }
    int index_best, index_worse;
    if (_rss_next_update_time <= eventlist().now()) {
        simtime_picosec jitter = static_cast<simtime_picosec>(
            (static_cast<float>(rand()) / static_cast<float>(INT32_MAX)) * 2 * _rss_jitter_range);
        if (jitter >= _rss_jitter_range)
            _rss_next_update_time =
                eventlist().now() + _rss_params._rss_update_interval + jitter - _rss_jitter_range;
        else
            _rss_next_update_time = eventlist().now() + _rss_params._rss_update_interval - jitter;
        vector<float> _rss_fraction_of_ecn_packets(_rss_params._rss_number_of_subflows);


        /* if () {

        } else {
            
        } */

        for (int pid = 0; pid < _rss_params._rss_number_of_subflows; pid++) {
            if (_rss_state_number_of_ev_pkts[pid] == 0) {
                assert(_rss_state_ecn[pid] == 0);
                assert(_rss_state_mean_rtt[pid] == 0);
                assert(_rss_state_worse_rtt[pid] == UINT64_MAX);
                _rss_fraction_of_ecn_packets[pid] = 0;
            } else {
                _rss_fraction_of_ecn_packets[pid] =
                    static_cast<float>(_rss_state_ecn[pid]) / _rss_state_number_of_ev_pkts[pid];
                _rss_state_mean_rtt[pid] =
                    _rss_state_mean_rtt[pid] / (_rss_state_number_of_ev_pkts[pid]);
            }
        }
        /* logMetricRssSubflow(_rss_state_mean_rtt,
                            _rss_state_worse_rtt,
                            _rss_fraction_of_ecn_packets,
                            _rss_state_entropies,
                            _rss_state_number_of_ev_pkts); */
        bool should_skip_round = false;
        if (_rss_params._rss_worse_entropy_metric == MEAN_RTT) {
            auto worse_path_id =
                max_element(_rss_state_mean_rtt.begin(), _rss_state_mean_rtt.end());
            auto best_path_id = min_element(
                _rss_state_mean_rtt.begin(), _rss_state_mean_rtt.end(), [](int a, int b) {
                    return a > 0 && (b <= 0 || a < b);
                });  // Discard 0 (i.e. not set) values from the min
            index_worse = distance(_rss_state_mean_rtt.begin(), worse_path_id);
            index_best  = distance(_rss_state_mean_rtt.begin(), best_path_id);
            should_skip_round =
                ((_rss_state_mean_rtt[index_worse] - _rss_state_mean_rtt[index_best]) <
                 _rss_params.threshold * _rss_state_mean_rtt[index_best]);


            /* printf("Should skip round %d - rss state mean worse %lu - %lu < %f (%f)\n", should_skip_round,
                   _rss_state_mean_rtt[index_worse], _rss_state_mean_rtt[index_best], _rss_params.threshold * _rss_state_mean_rtt[index_best], _rss_params.threshold); */
        } else if (_rss_params._rss_worse_entropy_metric == ECN) {
            auto worse_path_id = max_element(_rss_fraction_of_ecn_packets.begin(),
                                             _rss_fraction_of_ecn_packets.end());
            auto best_path_id  = min_element(_rss_fraction_of_ecn_packets.begin(),
                                            _rss_fraction_of_ecn_packets.end());
            index_worse        = distance(_rss_fraction_of_ecn_packets.begin(), worse_path_id);
            index_best         = distance(_rss_fraction_of_ecn_packets.begin(), best_path_id);
            should_skip_round  = ((_rss_fraction_of_ecn_packets[index_worse] -
                                  _rss_fraction_of_ecn_packets[index_best]) <
                                 _rss_params.threshold * _rss_fraction_of_ecn_packets[index_worse]);
        } else if (_rss_params._rss_worse_entropy_metric == WORSE_RTT) {
            auto worse_path_id = max_element(
                _rss_state_worse_rtt.begin(), _rss_state_worse_rtt.end(), [](size_t a, size_t b) {
                    return a < UINT64_MAX && a > b;
                });  // Discard UINT64_MAX (i.e. not set) values from the max
            auto best_path_id =
                min_element(_rss_state_worse_rtt.begin(), _rss_state_worse_rtt.end());
            index_worse = distance(_rss_state_worse_rtt.begin(), worse_path_id);
            index_best  = distance(_rss_state_worse_rtt.begin(), best_path_id);
            should_skip_round =
                ((_rss_state_worse_rtt[index_worse] - _rss_state_worse_rtt[index_best]) <
                 _rss_params.threshold * _rss_state_worse_rtt[index_best]);
        } else {
            throw logic_error("Not Implemented");
        }
        should_skip_round = should_skip_round || (_rss_number_of_rounds_to_skip > 0);
        if (UecSrc::usePflr() && !backgroundECMPFlow && (_pflr_scheme_id == 4) &&
            (pflr4GetStageId() == 0)) {
            // pflr 4 initialization stage, no ev change
            should_skip_round = should_skip_round && true;
        }
        //should_skip_round = true;
        if (!should_skip_round) {
            _rss_state_entropies[index_worse] =
                (rand() << _rss_params._rss_number_of_subflow_bits) + index_worse;
            if (UecSrc::usePflr() && _pflr_scheme_id != 0 && !backgroundECMPFlow) {
                auto     slot_id       = index_worse;
                uint32_t last_sent_psn = _highest_sent - 1;  // highest_send = next psn
                uint32_t last_sent_psn_this_slot =
                    last_sent_psn - (last_sent_psn - slot_id) % _rss_params._rss_number_of_subflows;
                uint32_t next_psn_this_slot =
                    last_sent_psn_this_slot + _rss_params._rss_number_of_subflows;
                // uint32_t remaining_packet_counts = (_backlog + full_pkt_size - 1)/full_pkt_size;
                // change to new EV
                if (_pflr_scheme_id == 2) {
                    _rss_state_entropies[index_worse] = _current_evs[index_worse] + 1;
                    _previous_evs[index_worse]        = _current_evs[index_worse];
                    _current_evs[index_worse] =
                        _current_evs[index_worse] + 1;  // next pkt psn as ev
                    _ev_status[index_worse] = EvStatus::IS_NEW;
                } else if ((_pflr_scheme_id == 3) || (_pflr_scheme_id == 4)) {
                    _rss_state_entropies[index_worse] = next_psn_this_slot;
                    _previous_evs[index_worse]        = _current_evs[index_worse];
                    _current_evs[index_worse]         = next_psn_this_slot;  // next pkt psn as ev
                    _ev_status[index_worse]           = EvStatus::IS_NEW;
                    if (_pflr_scheme_id == 4) {
                        // register ev change in the translation table
                        pflr4RegisterTranslation(
                            slot_id, _previous_evs[index_worse], _current_evs[index_worse]);
                        // cout << slot_id << " " << index_worse << endl;
                        // cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id() << "
                        // slot " << slot_id << " registered ev translation" << endl;
                    }
                } else {
                    abort();
                }
                // push the probe packet info into queue
                // only when we have packet to sent
                if (!_pflr_disable_probe) {
                    if (_backlog > 0) {
                        enqueueProbe(last_sent_psn_this_slot,
                                     _previous_evs[slot_id],
                                     UecDataPacket::SECTION_END);
                    }
                }
                /* cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id() << " slot "
                     << index_worse << " change to new EV: " << _previous_evs[index_worse] << " -> "
                     << _current_evs[index_worse] << endl; */
            }
        }
        for (int i = 0; i < _rss_params._rss_number_of_subflows; i++) {
            _rss_state_number_of_ev_pkts[i] = 0;
            _rss_state_ecn[i]               = 0;
            _rss_state_mean_rtt[i]          = 0;
            _rss_state_worse_rtt[i]         = UINT64_MAX;
        }
        if (_rss_number_of_rounds_to_skip > 0)
            _rss_number_of_rounds_to_skip--;
        else if (_rss_params.max_number_of_rounds_to_skip != 0) {
            _rss_number_of_rounds_to_skip = rand() % _rss_params.max_number_of_rounds_to_skip;
        }
    }
    return;
}

void UecSrc::processEv_ecmp(uint16_t path_id, PathFeedback path_feedback) {
    return;
}

void UecSrc::processEv_flowbender(
    uint16_t     path_id,
    PathFeedback path_feedback) {  // We could also track the number of bytes since the last update
                                   // and compare it to the cwnd
    _flowbender_stats._number_of_packets_in_current_round++;
    if (path_feedback.feedback_bit == PathFeedbackBit::PATH_ECN) {
        _flowbender_stats._current_rtt_ecn_packet_count++;
    }
    if (eventlist().now() > _flowbender_stats.last_update + _base_rtt) {
        if ((static_cast<double>(_flowbender_stats._current_rtt_ecn_packet_count) /
             _flowbender_stats._number_of_packets_in_current_round) >
            _flowbender_params._ecn_threshold) {
            if (++_flowbender_stats._current_consecutive_congested_rtt >=
                _flowbender_params.consecutive_rtt_number) {
                _flowbender_stats._entropy = static_cast<uint16_t>(rand());
            }
        }
        logMetricFlowBender(_flowbender_stats);
        _flowbender_stats = {0, 0, 0, eventlist().now(), _flowbender_stats._entropy};
    }
    return;
}

void UecSrc::processEv_uss(uint16_t path_id, PathFeedback path_feedback) {
    int subflow_id = path_id & ((1 << _uss_params._number_of_subflow_bits) - 1);
    if (UecSrc::usePflr() && _pflr_scheme_id != 0 && !backgroundECMPFlow) {
        if (_pflr_scheme_id == 1) {
            uint16_t n_bits_slot_id =
                (uint16_t)log2(_uss_params._number_of_subflows);  // #bits for slot id
            subflow_id = path_id >> (16 - n_bits_slot_id);
        } else {
            abort();
        }
    }
    _rss_state_number_of_ev_pkts[subflow_id]++;
    if (path_feedback.feedback_bit == PathFeedbackBit::PATH_ECN) {
        _rss_state_ecn[subflow_id]++;  // Update ECN counter
    }
    _rss_state_mean_rtt[subflow_id] += path_feedback.rtt_estimate;  // Update RTT estimate
    if (path_feedback.rtt_estimate > _rss_state_worse_rtt[subflow_id]) {
        _rss_state_worse_rtt[subflow_id] = path_feedback.rtt_estimate;
    }
    if (_rss_state_number_of_ev_pkts[subflow_id] >= USS_LOG_FREQUENCY) {
        vector<float> _fraction_of_ecn_packets(_uss_params._number_of_subflows);
        for (int pid = 0; pid < _uss_params._number_of_subflows; pid++) {
            if (_rss_state_number_of_ev_pkts[pid] == 0) {
                assert(_rss_state_ecn[pid] == 0);
                assert(_rss_state_mean_rtt[pid] == 0);
                assert(_rss_state_worse_rtt[pid] == UINT64_MAX);
                _fraction_of_ecn_packets[pid] = 0;
            } else {
                _fraction_of_ecn_packets[pid] =
                    static_cast<float>(_rss_state_ecn[pid]) / _rss_state_number_of_ev_pkts[pid];
                _rss_state_mean_rtt[pid] =
                    _rss_state_mean_rtt[pid] / (_rss_state_number_of_ev_pkts[pid]);
            }
        }
        logMetricUssSubflow(_rss_state_mean_rtt,
                            _rss_state_worse_rtt,
                            _fraction_of_ecn_packets,
                            _rss_state_entropies,
                            _rss_state_number_of_ev_pkts);
        for (int i = 0; i < _uss_params._number_of_subflows; i++) {
            _rss_state_number_of_ev_pkts[i] = 0;
            _rss_state_ecn[i]               = 0;
            _rss_state_mean_rtt[i]          = 0;
            _rss_state_worse_rtt[i]         = UINT64_MAX;
        }
    }
    return;
}

uint16_t UecSrc::nextEntropy_bitmap(UecBasePacket::seq_t seq_no) {
    // _no_of_paths must be a power of 2
    uint16_t mask    = _no_of_paths - 1;
    uint16_t entropy = (_current_ev_index ^ _path_xor) & mask;
    bool     flag    = false;
    int      counter = 0;
    while (_ev_skip_bitmap[entropy] > 0) {
        if (flag == false) {
            _ev_skip_bitmap[entropy]--;
            if (!_ev_skip_bitmap[entropy]) {
                assert(_ev_skip_count > 0);
                _ev_skip_count--;
            }
        }

        flag = true;
        counter++;
        if (counter > _no_of_paths) {
            break;
        }
        _current_ev_index++;
        if (_current_ev_index == _no_of_paths) {
            _current_ev_index = 0;
            _path_xor         = rand() & mask;
        }
        entropy = (_current_ev_index ^ _path_xor) & mask;
    }

    // set things for next time
    _current_ev_index++;
    if (_current_ev_index == _no_of_paths) {
        _current_ev_index = 0;
        _path_xor         = rand() & mask;
    }

    entropy |= _path_random ^ (_path_random & mask);  // set upper bits
    return entropy;
}

uint16_t UecSrc::nextEntropy_REPS(UecBasePacket::seq_t seq_no) {
    uint64_t allpathssizes = _mss * _no_of_paths;
    if (_mss * _highest_sent < min((uint64_t)_cwnd, allpathssizes)) {
        _crt_path++;
        if (_crt_path == _no_of_paths) {
            _crt_path = 0;
        }

        if (_debug)
            cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " REPS FirstWindow "
                 << _crt_path << " highest sent " << _mss * _highest_sent << " maxwnd " << _maxwnd
                 << " allpaths " << allpathssizes << endl;

    } else {
        if (_next_pathid.empty()) {
            assert(_no_of_paths > 0);
            _crt_path = random() % _no_of_paths;

            if (_debug)
                cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " REPS Steady "
                     << _crt_path << endl;

        } else {
            _crt_path = _next_pathid.front();
            _next_pathid.pop_front();

            if (_debug)
                cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " REPS Recycle "
                     << _crt_path << " " << _next_pathid.size() << endl;
        }
    }
    return _crt_path;
}

uint16_t UecSrc::nextEntropy_mixed(UecBasePacket::seq_t seq_no) {
    if (_next_pathid.empty()) {
        return nextEntropy_bitmap(seq_no);
    } else {
        _crt_path = _next_pathid.front();
        _next_pathid.pop_front();

        if (_debug)
            cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " MIXED Recycle "
                 << _crt_path << " " << _next_pathid.size() << endl;
    }
    return _crt_path;
}

uint16_t UecSrc::nextEntropy_oblivious(UecBasePacket::seq_t seq_no) {
    // _no_of_paths must be a power of 2
    uint16_t mask    = _no_of_paths - 1;
    uint16_t entropy = (_current_ev_index ^ _path_xor) & mask;

    // set things for next time
    _current_ev_index++;
    if (_current_ev_index == _no_of_paths) {
        _current_ev_index = 0;
        _path_xor         = rand() & mask;
    }

    entropy |= _path_random ^ (_path_random & mask);  // set upper bits
    return entropy;
}

uint16_t UecSrc::nextEntropy_rss(UecBasePacket::seq_t seq_no) {
    return _rss_state_entropies[seq_no % _rss_params._rss_number_of_subflows];
}

uint16_t UecSrc::nextEntropy_ecmp(UecBasePacket::seq_t seq_no) {
    return static_cast<uint16_t>(_flow.flow_id());
}

uint16_t UecSrc::nextEntropy_flowbender(UecBasePacket::seq_t seq_no) {
    return _flowbender_stats._entropy;
}

uint16_t UecSrc::nextEntropy_uss(UecBasePacket::seq_t seq_no) {
    return _rss_state_entropies[seq_no % _uss_params._number_of_subflows];
}

void UecSrc::enqueueProbe(UecBasePacket::seq_t         seq_no,
                          uint16_t                     ev,
                          UecDataPacket::PflrProbeType probe_type) {
    // enqueue
    /* if (eventlist().now() < 1000000000) {
        return;
    } */

    if (RSS != UecSrc::_load_balancing_algo) {
        return;
    }

    if (probe_type == UecDataPacket::PflrProbeType::PROACTIVE_DATA) {
        // proactive RTX is not allowed
    }

    _probe_queue.push_back({seq_no, ev});
    _probe_type_queue.push_back(probe_type);
    // record the current time
    // auto slot_id = seq_no%UecSrc::getNoSlots();
    // if ((probe_type == UecDataPacket::SECTION_END) || (probe_type ==
    // UecDataPacket::PROACTIVE_DATA)) {
    //     // not counting proactive RTX, as it only carries one packet info
    //     _slots_last_proactive_probe_time[slot_id] = eventlist().now();
    //     _slots_last_proactive_probe_psn[slot_id] = seq_no;
    // }
}

void UecSrc::enqueueDynamicProactiveProbe() {
    // enqueue probe if needed
    if (_pflr_proactive_probe && (_pflr_proactive_probe_pkt_count == 0)) {
        for (size_t slot_id = 0; slot_id < UecSrc::getNoSlots(); slot_id++) {
            if (_highest_sent <= slot_id) {
                // nothing sent in this slot, do nothing
                continue;
            }
            simtime_picosec estimated_rtt =
                _base_rtt;
            if (_rss_state_number_of_ev_pkts[slot_id] > 0) {
                // use stat
                auto rss_stat_rtt =
                    _rss_state_mean_rtt[slot_id] / (_rss_state_number_of_ev_pkts[slot_id]);
                if (rss_stat_rtt < estimated_rtt) {
                    estimated_rtt = rss_stat_rtt;
                }
            }
            auto current_time = eventlist().now();
            if ((current_time >= (_slots_last_proactive_probe_time[slot_id] + estimated_rtt)) &&
                (_backlog > 0)) {
                // enqueue an proactive probe packet
                auto seq_no = _slots_last_data_packet_info[slot_id].first;
                auto ev     = _slots_last_data_packet_info[slot_id].second;
                cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
                     << " enqueue proactive data probe for psn " << seq_no << " ev " << ev << endl;
                cout << "Using rtt " << timeAsUs(estimated_rtt) << endl;
                if (_slots_last_proactive_probe_psn[slot_id] < seq_no) {
                    enqueueProbe(seq_no, ev, UecDataPacket::PROACTIVE_DATA);
                }
            }
        }
    }
}

mem_b UecSrc::sendProbePacket(const Route& route) {
    assert(!_probe_queue.empty());
    mem_b probe_pkt_size = _hdr_size;
    _pull_target         = computePullTarget();

    auto probe_pkt_info = _probe_queue[0];  // psn, ev pair
    _probe_queue.erase(_probe_queue.begin());

    auto probe_type = _probe_type_queue[0];  // is proactive = is not changing section
    _probe_type_queue.erase(_probe_type_queue.begin());

    auto* p = UecDataPacket::newpkt(_flow,
                                    route,
                                    probe_pkt_info.first,
                                    probe_pkt_size,
                                    UecDataPacket::PROBE,
                                    _pull_target,
                                    _dstaddr);
    p->set_pflr_probe_type(probe_type);
    uint16_t ev = probe_pkt_info.second;

    

    p->set_pathid(ev);
    p->flow().logTraffic(*p, *this, TrafficLogger::PKT_CREATESEND);
    p->sendOn();

    
    // update next probe time
    if ((p->pflr_probe_type() == UecDataPacket::PflrProbeType::SECTION_END) ||
        (p->pflr_probe_type() == UecDataPacket::PflrProbeType::PROACTIVE_DATA)) {
        if (_pflr_proactive_probe_pkt_count == 0) {
            int slot_id                               = probe_pkt_info.first % UecSrc::getNoSlots();

            /* printf("Slot ID is %d %d %d %d\n", slot_id, probe_pkt_info.first, UecSrc::getNoSlots(), _slots_last_proactive_probe_time.size());
            fflush(stdout); */

            _slots_last_proactive_probe_time[slot_id] = eventlist().now();
            _slots_last_proactive_probe_psn[slot_id]  = probe_pkt_info.first;
            simtime_picosec estimated_rtt =
                _base_rtt;
            if (_rss_state_number_of_ev_pkts[slot_id] > 0) {
                // use stat
                auto rss_stat_rtt =
                    _rss_state_mean_rtt[slot_id] / (_rss_state_number_of_ev_pkts[slot_id]);
                if (rss_stat_rtt < estimated_rtt) {
                    estimated_rtt = rss_stat_rtt;
                }
            }
            if (_backlog > 0) {
                // schedule next Probe
                // if ((_pflr_slots_probe_is_pending[slot_id]) &&
                // !(_pflr_slots_probe_send_handle[slot_id] == eventlist().nullHandle())) {
                //     // cancel previous
                //     eventlist().cancelPendingSourceByHandle(*this,
                //     _pflr_slots_probe_send_handle[slot_id]);
                // }
                _pflr_slots_scheduled_probe_send_time[slot_id] = eventlist().now() + estimated_rtt;
                _pflr_slots_probe_send_handle[slot_id] =
                    eventlist().sourceIsPendingGetHandle(*this, eventlist().now() + estimated_rtt);
                if (_pflr_slots_probe_send_handle[slot_id] == eventlist().nullHandle()) {
                    _pflr_slots_probe_is_pending[slot_id] = 0;
                } else {
                    _pflr_slots_probe_is_pending[slot_id] = 1;
                }
            }
        }
    }
    // if (_pflr_print_debug_msg) {
    /* if (p->pflr_probe_type() == UecDataPacket::PflrProbeType::SECTION_END) {
        cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
             << " sending end probe packet for psn " << probe_pkt_info.first << " ev " << ev << endl;
    } else if (p->pflr_probe_type() == UecDataPacket::PflrProbeType::PROACTIVE_DATA) {
        cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
             << " sending proactive data probe packet for psn " << probe_pkt_info.first << " ev "
             << ev << endl;
    } else if (p->pflr_probe_type() == UecDataPacket::PflrProbeType::PROACTIVE_RTX) {
        cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
             << " sending proactive rtx probe packet for psn " << probe_pkt_info.first << " ev "
             << ev << endl;
    } else {
        abort();
    } */
    // }
    return probe_pkt_size;
}

mem_b UecSrc::sendNewPacket(const Route& route) {
    if (UecSrc::usePflr() && !backgroundECMPFlow) {
        if (_pflr_scheme_id == 4) {
            if (pflr4GetStageId() != 0) {
                // 0: init, use initial psn & can always send
                // 1: stable, use received psn's transformation
                // if not in starting phase and no EV to use, cannot send pkt
                auto current_psn = _highest_sent;
                auto slot_id     = current_psn % UecSrc::getNoSlots();
                if (_pflr4_slots_ev_queue[slot_id].empty()) {
                    // cannot send anything
                    cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
                         << " want to send pflr4 data packet for psn " << current_psn
                         << " but no ev " << endl;
                    return 0;
                } else {
                    // we can send something based on the first ev
                    // pflr4CleanUpTimeout(slot_id);
                    pflr4PrintEvMap(slot_id);
                    uint16_t pflr4_ev =
                        _pflr4_slots_ev_queue[slot_id].front();  // Get the first element
                    _pflr4_slots_ev_queue[slot_id].erase(
                        _pflr4_slots_ev_queue[slot_id].begin());  // Remove it from the vector
                    cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id() << " slot "
                         << slot_id << " psn " << current_psn << " reuse EV: " << pflr4_ev << endl;
                    pflr4_ev = pflr4LookupEv(slot_id, pflr4_ev);
                    cout << "after update: " << endl;
                    pflr4PrintEvMap(slot_id);
                    assert(pflr4_ev == _current_evs[slot_id]);
                }
            } else {
                cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
                     << " pflr4 initializing, sending psn " << _highest_sent << " with slot id"
                     << endl;
            }
        }
    }
    if (_debug_src)
        cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " " << _nodename
             << " sendNewPacket highest_sent " << _highest_sent << " h*m " << _highest_sent * _mss
             << " backlog " << _backlog << " flow " << _flow.str() << endl;
    assert(_backlog > 0);
    assert(((mem_b)_highest_sent - _stats.rts_pkts_sent) * _mss < _flow_size);
    mem_b full_pkt_size = _mtu;
    if (_backlog < _mtu) {
        full_pkt_size = _backlog;
    }

    // check we're allowed to send according to state machine
    if (_receiver_based_cc)
        assert(credit() > 0);

    spendCredit(full_pkt_size);

    _backlog -= full_pkt_size;
    assert(_backlog >= 0);
    _in_flight += full_pkt_size;
    auto ptype = UecDataPacket::DATA_PULL;
    if (_speculating) {
        ptype = UecDataPacket::DATA_SPEC;
    }
    _pull_target = computePullTarget();

    auto* p = UecDataPacket::newpkt(
        _flow, route, _highest_sent, full_pkt_size, ptype, _pull_target, _dstaddr);
    uint16_t ev = (this->*nextEntropy)(_highest_sent);
    p->set_pathid(ev);
    p->flow().logTraffic(*p, *this, TrafficLogger::PKT_CREATESEND);

    if (_load_balancing_algo == RSS) {
        int sid = /* for new */ (_highest_sent % _rss_params._rss_number_of_subflows);
        // or for rtx: (seq_no % _rss_params._rss_number_of_subflows);
        _rss_noack_deadline[sid] = eventlist().now() + _rss_noack_timeout;
    }
    
    // loss detection
    // mem_b probe_pkt_size = sendProbePacketOnNewEv(_highest_sent, route);
    auto current_psn             = _highest_sent;
    auto remaining_packet_counts = (_backlog + full_pkt_size - 1) / full_pkt_size;  // round up
    if (UecSrc::usePflr() && (!_pflr_disable_probe) && !backgroundECMPFlow) {
        if (remaining_packet_counts < UecSrc::getNoSlots()) {
            // is the last packet in the slot
            if (_pflr_scheme_id != 0) {
                //assert(ev == _current_evs[current_psn % UecSrc::getNoSlots()]);
            }
            //printf("Here2\n");
            enqueueProbe(current_psn, ev, UecDataPacket::SECTION_END);
        } else if (_pflr_proactive_probe) {
            // record the last sent data packet
            //printf("Size %d - Num sloots %d -Recording last data packet info for psn %d ev %d\n", _slots_last_data_packet_info.size(), UecSrc::getNoSlots(), current_psn, ev);
            //fflush(stdout); 
            _slots_last_data_packet_info[current_psn % UecSrc::getNoSlots()] = {current_psn, ev};
            // is a proactive probe packet
            // send one every _pflr_proactive_probe_pkt_count packets on each slots
            auto slot_offset = current_psn / UecSrc::getNoSlots();
            if (_pflr_proactive_probe_pkt_count > 0 && true) {
                // fixed proactive probe
                if (((slot_offset + 1) % _pflr_proactive_probe_pkt_count) == 0) {
                    if (_pflr_scheme_id != 0) {
                        assert(ev == _current_evs[current_psn % UecSrc::getNoSlots()]);
                    }
                    enqueueProbe(current_psn, ev, UecDataPacket::PROACTIVE_DATA);
                }
            }
        }
    }
    // // if (UecSrc::usePflr() && _pflr_print_debug_msg && !backgroundECMPFlow) {
    /* cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
         << " sending data packet for psn " << _highest_sent << " ev " << ev << 
         " last " << timeAsUs(eventlist().now()-last_data_sent_time) << endl; */
    // // cout << "_backlog " << _backlog << " remaining_packet_counts " << remaining_packet_counts
    // << " need_send_probe_packet " << need_send_probe_packet << endl;
    // // }
    if (UecSrc::usePflr() && _pflr_scheme_id == 0) {
        pflr0RegisterSend(current_psn, ev);
    }
    last_data_sent_time = (eventlist().now());

    if (_backlog == 0 || (_receiver_based_cc && _credit <= 0) ||
        (_sender_based_cc && (_in_flight + full_pkt_size) >= _cwnd))
        p->set_ar(true);

    createSendRecord(_highest_sent, full_pkt_size);
    if (_debug_src)
        cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " sending pkt "
             << _highest_sent << " size " << full_pkt_size << " pull target " << _pull_target
             << " ack request " << p->ar() << " cwnd " << _cwnd << " ev " << ev << " in_flight "
             << _in_flight << endl;
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " sending pkt "
             << _highest_sent << " size " << full_pkt_size << " cwnd " << _cwnd << " ev " << ev
             << " in_flight " << _in_flight << " pull_target " << _pull_target << " pull " << _pull
             << " ar " << p->ar() << endl;
    }


    /* if (_flow.str() == "Uec_3_4") {
        printf("Sending new packet %d at time %f\n", _flow.flow_id(), timeAsUs(eventlist().now()));
    } */
    p->sendOn();
    _highest_sent++;
    _stats.new_pkts_sent++;
    startRTO(eventlist().now());
    // RFC 8985 §7.1: arm PTO after sending data
    tlpComputeAndArmPTO();
    return full_pkt_size;
}

mem_b UecSrc::sendRtxPacket(const Route& route) {
    assert(!_rtx_queue.empty());
    auto  seq_no        = _rtx_queue.begin()->first;
    mem_b full_pkt_size = _rtx_queue.begin()->second;
    if (UecSrc::usePflr() && !backgroundECMPFlow) {
        if (_pflr_scheme_id == 4) {
            auto slot_id = seq_no % UecSrc::getNoSlots();
            if (_pflr4_use_ev_recovery && pflr4IsRtoRtx(seq_no)) {
                // is RTO
                pflr4PrintEvMap(slot_id);
                uint16_t pflr4_ev = slot_id;
                if (_pflr4_slots_ev_map_old_ev[slot_id].size() > 0) {
                    pflr4_ev =
                        _pflr4_slots_ev_map_old_ev[slot_id]
                                                  [_pflr4_slots_ev_map_old_ev[slot_id].size() - 1];
                }
                cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id() << " slot "
                     << slot_id << " psn " << seq_no << " rto rtx reuse EV: " << pflr4_ev << endl;
                pflr4_ev = pflr4LookupEv(slot_id, pflr4_ev);
                cout << "Translated to ev: " << pflr4_ev << endl;
                cout << "after update: " << endl;
                pflr4PrintEvMap(slot_id);
                assert(pflr4_ev == _current_evs[slot_id]);
            } else {
                auto slot_id = seq_no % UecSrc::getNoSlots();
                if (pflr4GetStageId() != 0) {
                    // 0: init, use initial psn & can always send
                    // 1: stable, use received psn's transformation
                    // if not in starting phase and no EV to use, cannot send pkt
                    if (_pflr4_slots_ev_queue[slot_id].empty()) {
                        // cannot send anything
                        cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
                             << " want to send pflr4 rtx packet for psn " << seq_no << " but no ev "
                             << endl;
                        return 0;
                    } else {
                        // we can send something based on the first ev
                        // pflr4CleanUpTimeout(slot_id);
                        pflr4PrintEvMap(slot_id);
                        uint16_t pflr4_ev =
                            _pflr4_slots_ev_queue[slot_id].front();  // Get the first element
                        _pflr4_slots_ev_queue[slot_id].erase(
                            _pflr4_slots_ev_queue[slot_id].begin());  // Remove it from the vector
                        cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
                             << " slot " << slot_id << " psn " << seq_no
                             << " rtx reuse EV: " << pflr4_ev << endl;
                        pflr4_ev = pflr4LookupEv(slot_id, pflr4_ev);
                        cout << "after update: " << endl;
                        pflr4PrintEvMap(slot_id);
                        assert(pflr4_ev == _current_evs[slot_id]);
                    }
                }
            }
        }
    }
    spendCredit(full_pkt_size);

    _rtx_queue.erase(_rtx_queue.begin());
    _rtx_backlog -= full_pkt_size;
    assert(_rtx_backlog >= 0);
    _in_flight += full_pkt_size;
    _pull_target = computePullTarget();



    auto* p = UecDataPacket::newpkt(
        _flow, route, seq_no, full_pkt_size, UecDataPacket::DATA_RTX, _pull_target, _dstaddr);
    uint16_t ev = (this->*nextEntropy)(seq_no);
    p->set_pathid(ev);
    p->is_rtx = true;
    p->flow().logTraffic(*p, *this, TrafficLogger::PKT_CREATESEND);

    createSendRecord(seq_no, full_pkt_size);
    if (UecSrc::usePflr() && _pflr_scheme_id == 0) {
        pflr0RegisterSend(seq_no, ev);
    }
    if (UecSrc::usePflr() && _pflr_scheme_id != 0 && (!_pflr_disable_probe) &&
        !backgroundECMPFlow) {
        if (_pflr_proactive_rtx_probe) {
            // every rtx has a proactive probe packet
            assert(ev == _current_evs[seq_no % UecSrc::getNoSlots()]);
            enqueueProbe(seq_no, ev, UecDataPacket::PROACTIVE_RTX);
        }
    }

    if (_debug_src)
        cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " " << _nodename
             << " sending rtx pkt " << seq_no << " size " << full_pkt_size << " cwnd " << _cwnd
             << " in_flight " << _in_flight << " pull_target " << _pull_target << " pull " << _pull
             << endl;
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " sending rtx pkt "
             << seq_no << " size " << full_pkt_size << " cwnd " << _cwnd << " ev " << ev
             << " rtx_times " << _rtx_times[seq_no] << " in_flight " << _in_flight
             << " pull_target " << _pull_target << " pull " << _pull << endl;
    }
    p->set_ar(true);
    if (UecSrc::_log_reaction_events) {
        cout << "RTX: FlowID " << _flow.flow_id() << " - Packet ID " << seq_no
             << " - Time " << eventlist().now() << endl;
    }
    p->sendOn();
    _stats.rtx_pkts_sent++;
    startRTO(eventlist().now());
    // RFC 8985 §7.1: arm PTO after retransmitting data
    tlpComputeAndArmPTO();
    /* cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
         << " sending rtxX packet for psn " << seq_no << " ev " << ev << endl; */
    return full_pkt_size;
}

void UecSrc::sendProbe() {
    const Route* route = _nic.requestSending(*this);
    if (route) {
        if (_flow.flow_id() == _debug_flowid) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " sendProbe "
                 << endl;
        }
        _probe_seqno++;
        auto* p = UecDataPacket::newpkt(
            _flow, *route, _probe_seqno, _hdr_size, UecBasePacket::DATA_PROBE, 0, _dstaddr);
        uint16_t ev = (this->*nextEntropy)(_probe_seqno);
        p->set_pathid(ev);
        p->sendOn();
        cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
             << " sending sleek probe packet for psn " << _probe_seqno << " ev " << ev << endl;
        _probe_send_time = eventlist().now();
        _nic.startSending(*this, _hdr_size, route);
        _probe_timer_when   = eventlist().now() + 5 * _base_rtt;
        _probe_timer_handle = eventlist().sourceIsPendingGetHandle(*this, _probe_timer_when);

    } else {
        _probe_timer_when   = eventlist().now() + _base_rtt;
        _probe_timer_handle = eventlist().sourceIsPendingGetHandle(*this, _probe_timer_when);
    }
}

void UecSrc::sendRTS() {
    if (_last_rts > 0 && eventlist().now() - _last_rts < _rtt) {
        // Don't send more than one RTS per RTT, or we can create an
        // incast of RTS.  Once per RTT is enough to restart things if we lost
        // a whole window.
        return;
    }
    if (_debug_src)
        cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " " << _nodename
             << " sendRTS, flow " << _flow.str() << " epsn " << _highest_sent << " last RTS "
             << timeAsUs(_last_rts) << " in_flight " << _in_flight << " pull_target "
             << _pull_target << " pull " << _pull << endl;
    createSendRecord(_highest_sent, _hdr_size);
    auto* p = UecRtsPacket::newpkt(_flow, NULL, _highest_sent, _pull_target, _dstaddr);
    p->set_dst(_dstaddr);
    uint16_t ev = (this->*nextEntropy)(_highest_sent);
    p->set_pathid(ev);

    // p->sendOn();
    _nic.sendControlPacket(p, this, NULL);

    _highest_sent++;
    _stats.rts_pkts_sent++;
    _last_rts = eventlist().now();
    startRTO(eventlist().now());
}

void UecSrc::createSendRecord(UecBasePacket::seq_t seqno, mem_b full_pkt_size) {
    if (_debug_src)
        cout << _flow.str() << " " << _nodename << " createSendRecord seqno: " << seqno << " size "
             << full_pkt_size << endl;

    // Defensive: if seqno already tracked (e.g. spurious TLP re-probe), clean
    // up the stale entry so the multimap _send_times never accumulates duplicates.
    auto existing = _tx_bitmap.find(seqno);
    if (existing != _tx_bitmap.end()) {
        delFromSendTimes(existing->second.send_time, seqno);
        _tx_bitmap.erase(existing);
    }

    _tx_bitmap.emplace(seqno, sendRecord(full_pkt_size, eventlist().now()));
    _send_times.emplace(eventlist().now(), seqno);

    if (_rtx_times.find(seqno) == _rtx_times.end()) {
        _rtx_times.emplace(seqno, 0);
    } else {
        _rtx_times[seqno] = _rtx_times[seqno] + 1;
    }
}

void UecSrc::queueForRtx(UecBasePacket::seq_t seqno, mem_b pkt_size) {
    assert(_rtx_queue.find(seqno) == _rtx_queue.end());
    _rtx_queue.emplace(seqno, pkt_size);
    _rtx_backlog += pkt_size;
    if (_trace_rtx) {
        cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
             << " queue_rtx packet for psn " << seqno << endl;
    }
    if (_pflr_pace_rtx) {
        // Don't send immediately - let processNack schedule paced send
        return;
    }
    if (!_speculating || !_receiver_based_cc)
        sendIfPermitted();
}

void UecSrc::timeToSend(const Route& route) {
/*     cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id() << " cwnd " << _cwnd << endl;
 */    if (_debug_src)
        cout << "timeToSend" << " flow " << _flow.str() << " at " << timeAsUs(eventlist().now())
             << endl;

    // time_to_send is called back from the UecNIC when it's time for
    // this src to send.  To get called back, the src must have
    // previously told the NIC it is ready to send by calling
    // UecNIC::requestSending()

    // before returning, UecSrc needs to call either
    // UecNIC::startSending or UecNIC::cantSend from this function
    // to update the NIC as to what happened, so they stay in sync.
    // This also true when the flow is complete, let's make sure
    // we are in sync either way.
    _send_blocked_on_nic = false;
    // enqueueDynamicProactiveProbe();
    if (_backlog == 0 && _rtx_queue.empty() && (_probe_queue.empty())) {
        _nic.cantSend(*this);
        return;
    }

    mem_b next_packet_size = getNextPacketSize();
    bool probe_is_next = _rtx_queue.empty() && !_probe_queue.empty();
    if (!probe_is_next && _sender_based_cc) {
        if (!can_send_NSCC(next_packet_size)) {
            if (_debug_src)
                cout << _flow.str() << " " << _node_num << "cantSend, limited by sender CWND "
                     << _cwnd << " _in_flight " << _in_flight << "\n";

            _nic.cantSend(*this);
            return;
        }
    }
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
             << " _receiver_based_cc " << _receiver_based_cc << " credit " << credit() << endl;
    }
    // do we have enough credit if we're using receiver CC?
    if (!probe_is_next && (_receiver_based_cc && credit() <= 0)) {
        if (_debug_src)
            cout << "cantSend" << " flow " << _flow.str() << endl;
        ;
        _nic.cantSend(*this);
        return;
    }

    // OK, we're probably good to send
    mem_b bytes_sent = 0;
    if (!_rtx_queue.empty()) {
        bytes_sent = sendRtxPacket(route);
    } else if (!_probe_queue.empty()) {
        bytes_sent = sendProbePacket(route);
    } else {
        bytes_sent = sendNewPacket(route);
    }
    // if (_rtx_queue.empty()) {
    //     bytes_sent = sendNewPacket(route);
    // } else {
    //     bytes_sent = sendRtxPacket(route);
    // }

    // let the NIC know we sent, so it can calculate next send time.
    if (bytes_sent > 0) {
        _nic.startSending(*this, bytes_sent, NULL);
    } else {
        _nic.cantSend(*this);
        return;
    }

    next_packet_size = getNextPacketSize();
    probe_is_next = _rtx_queue.empty() && !_probe_queue.empty();
    if (!probe_is_next && _sender_based_cc) {
        if (!can_send_NSCC(next_packet_size)) {
            return;
        }
    }

    // do we have enough credit to send again?
    if (!probe_is_next && (_receiver_based_cc && credit() <= 0)) {
        return;
    }

    if (_probe_queue.empty() && (_backlog == 0 && _rtx_queue.empty())) {
        // we're done - nothing more to send.
        return;
    }

    // we're ready to send again.  Let the NIC know.
    assert(!_send_blocked_on_nic);
    if (_debug_src)
        cout << "requestSending2" << " flow " << _flow.str() << endl;
    ;
    const Route* newroute = _nic.requestSending(*this);
    // we've just sent - NIC will say no, but will call us back when we can send.
    if (newroute) {
        throw std::runtime_error("NIC: requestSending returned non-NULL in timeToSend");
    }

    _send_blocked_on_nic = true;
}

void UecSrc::recalculateRTO() {
    // we're no longer waiting for the packet we set the timer for -
    // figure out what the timer should be now.
    cancelRTO();
    if (_send_times.empty()) {
        // nothing left that we're waiting for
        return;
    }
    auto earliest_send_time = _send_times.begin()->first;
    startRTO(earliest_send_time);
}

void UecSrc::rtxTimerExpired() {
    assert(eventlist().now() == _rtx_timeout);
    clearRTO();

    // Cancel any pending proactive tail check to avoid stale timer firing
    if (_proactive_tail_check_scheduled && _rtx_pace_pending) {
        eventlist().cancelPendingSourceByHandle(*this, _rtx_pace_handle);
        _rtx_pace_pending = false;
        _rtx_pace_handle = eventlist().nullHandle();
    }
    _proactive_tail_check_scheduled = false;

    // TLP probes or ACKs may have drained _send_times before this RTO fires.
    if (_send_times.empty()) {
        return;
    }

    auto first_entry = _send_times.begin();
    auto seqno = first_entry->second;

    auto send_record = _tx_bitmap.find(seqno);
    if (send_record == _tx_bitmap.end()) {
        // Stale _send_times entry - remove it and bail out.
        _send_times.erase(first_entry);
        if (!_send_times.empty())
            recalculateRTO();
        return;
    }
    mem_b pkt_size = send_record->second.pkt_size;

    // update flightsize?

    //_send_times.erase(first_entry);
    delFromSendTimes(send_record->second.send_time, seqno);

    if (_load_balancing_algo == RSS) {
        int sid = seqno % _rss_params._rss_number_of_subflows;
        rssBumpEntropyForSubflow(sid, "rto");
    }

   /*  if (_debug_src)
        cout << _nodename << " rtx timer expired for seqno " << seqno << " flow " << _flow.str()
             << " packet sent at " << timeAsUs(send_record->second.send_time) << " now time is "
             << timeAsUs(eventlist().now()) << endl;
    if (_flow.flow_id() == UecSrc::_debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
             << " rtx timer expired for seqno " << seqno << " packet sent at "
             << timeAsUs(send_record->second.send_time) << " now time is "
             << timeAsUs(eventlist().now()) << " _loss_recovery_mode " << _loss_recovery_mode
             << endl;
    } */

    // Yanfang: this is a hack, we remove timestamp for these seqno,
    // I would expect that that the fast loss recovery will retransmit this packet, when the
    // send_times record the sending timestamp for this packet
    if (_sender_based_cc && _enable_sleek) {
        if (_loss_recovery_mode) {
            if (_rtx_times[seqno] < 1) {
                recalculateRTO();
            } else {
                _highest_rtx_sent = seqno;
            }
            return;
        }
    }
    cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
         << " rtx timer expired for psn " << seqno << endl;

    _lost_packets++;
    _rack_stats.rto_events++;

    // Cancel TLP PTO on RTO
    tlpCancelPTO();
    _tlp.probe_in_flight = false;
    _tlp.has_rtt_sample  = true;
    for (auto& tlp_sf : _tlp_per_subflow) {
        tlp_sf.probe_in_flight = false;
        tlp_sf.has_rtt_sample  = true;
        tlp_sf.pto_deadline    = 0;
    }

    _tx_bitmap.erase(send_record);
    recalculateRTO();

    if (_sender_based_cc)
        mark_packet_for_retransmission(seqno, pkt_size);

    if (!_rtx_queue.empty()) {
        // there's already a queue, so clearly we shouldn't just
        // resend right now.  But send an RTS (no more than once per
        // RTT) to cover the case where the receiver doesn't know
        // we're waiting.
        stopSpeculating();
        /* cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
             << " rto queue rtx packet for psn " << seqno << endl; */
        queueForRtx(seqno, pkt_size);
        if (UecSrc::usePflr() && !backgroundECMPFlow) {
            if ((_pflr_scheme_id == 4) && _pflr4_use_ev_recovery) {
                pflr4RegisterRto(seqno);
            }
        }

        // With paced RTX, ensure a paced send timer is running to drain the queue
        if (_pflr_pace_rtx && !_rtx_pace_pending && !_rtx_queue.empty()) {
            scheduleRtxPacedSend();
        }

        if (_receiver_based_cc) {
            if (_debug_src)
                cout << "sendRTS 1" << " flow " << _flow.str() << endl;
            sendRTS();
        }
        return;
    }

    // there's no queue, so maybe we could just resend now?
    /* cout << timeAsUs(eventlist().now()) << " flow " << _flow.flow_id()
         << " rto queue rtx packet for psn " << seqno << endl; */
    queueForRtx(seqno, pkt_size);
    if (UecSrc::usePflr() && !backgroundECMPFlow) {
        if ((_pflr_scheme_id == 4) && _pflr4_use_ev_recovery) {
            pflr4RegisterRto(seqno);
        }
    }

    if (_sender_based_cc) {
        if (_cwnd < pkt_size + _in_flight) {
            // window won't allow us to send yet.
            if (_pflr_pace_rtx && !_rtx_pace_pending && !_rtx_queue.empty()) {
                scheduleRtxPacedSend();
            }
            if (_debug_src)
                cout << "sendRTS 3" << " flow " << _flow.str() << endl;
            sendRTS();
            return;
        }
    }

    if (_receiver_based_cc && _credit <= 0) {
        // we don't have any credit to send.  Send an RTS (no more than once per RTT)
        // to cover the case where the receiver doesn't know to send
        // us credit
        if (_debug_src)
            cout << "sendRTS 2" << " flow " << _flow.str() << endl;

        sendRTS();
        return;
    }

    // we've got enough pulled credit or window already to send this, so see if the NIC
    // is ready right now
    if (_debug_src)
        cout << "requestSending 4\n"
             << " flow " << _flow.str() << endl;

    const Route* route = _nic.requestSending(*this);
    if (route) {
        bool bytes_sent = sendRtxPacket(*route);
        if (bytes_sent > 0) {
            _nic.startSending(*this, bytes_sent, route);
        } else {
            // NIC blocked - schedule paced send to retry later
            if (_pflr_pace_rtx && !_rtx_pace_pending && !_rtx_queue.empty()) {
                scheduleRtxPacedSend();
            }
            _nic.cantSend(*this);
            return;
        }
    }
}

void UecSrc::activate() {
    startFlow();

    /* simtime_picosec now = eventlist().now();
    uint64_t random_delay = (uint64_t)(random() % 100000);
    // 3. Schedule this source to run its doNextEvent() method after the delay.
    eventlist().sourceIsPending(*this, now + random_delay); */
}

void UecSrc::setEndTrigger(Trigger& end_trigger) {
    _end_trigger = &end_trigger;
};

// ═══════════════════════════════════════════════════════════
//  RFC 8985 RACK-TLP Implementation
// ═══════════════════════════════════════════════════════════

int UecSrc::rackSubflowForSeqno(UecDataPacket::seq_t seqno) const {
    if (_rack_per_subflow.empty()) return -1;
    return seqno % _rss_params._rss_number_of_subflows;
}

void UecSrc::rackOnAckUpdate(UecDataPacket::seq_t acked_psn, simtime_picosec send_time) {
    // Called from processAck/processNack after we know the send_time of the ACK'd packet.
    // Update RACK state per § 6.2 of RFC 8985.
    // When RSS is active, update per-subflow state (Falcon-style) to avoid
    // spurious loss marks from cross-subflow reordering.
    if (_rack_tlp_mode == RackTlpMode::OFF ||
        _rack_tlp_mode == RackTlpMode::TLP_ONLY) return;

    // Lazy init: allocate per-subflow RACK state on first use.
    // _rack_tlp_mode is static and only set after construction.
    if (_rack_per_subflow.empty() &&
        _load_balancing_algo == RSS &&
        _rss_params._rss_number_of_subflows > 1) {
        _rack_per_subflow.resize(_rss_params._rss_number_of_subflows);
        _tlp_per_subflow.resize(_rss_params._rss_number_of_subflows);
    }

    simtime_picosec now = eventlist().now();
    simtime_picosec pkt_rtt = now - send_time;

    // Select the correct RackState: per-subflow if RSS, else global.
    int sid = rackSubflowForSeqno(acked_psn);
    RackState& rack = (sid >= 0) ? _rack_per_subflow[sid] : _rack;

    // Update min_rtt
    if (pkt_rtt < rack.min_rtt) {
        rack.min_rtt = pkt_rtt;
    }

    // RACK: update xmit_ts if this ACK acknowledges a packet sent later
    // than the current rack.xmit_ts (or same time but higher seqno).
    if (send_time > rack.xmit_ts ||
        (send_time == rack.xmit_ts && acked_psn > rack.end_seq)) {
        rack.xmit_ts = send_time;
        rack.end_seq = acked_psn;
        rack.ack_ts  = now;
        rack.rtt     = pkt_rtt;
    }

    // Compute reordering window (§ 6.2):
    // reo_wnd = min_rtt / 4, lower bounded at 1μs.
    simtime_picosec quarter_rtt = rack.min_rtt / 4;
    simtime_picosec min_reo = timeFromUs(1u);
    rack.reo_wnd = (quarter_rtt > min_reo) ? quarter_rtt : min_reo;

    // If DSACK-based widening is active, widen further
    if (rack.reo_wnd_incr > 0) {
        rack.reo_wnd += rack.reo_wnd_incr * (rack.min_rtt / 4);
    }

    if (_rack_logger.is_open()) {
        _rack_logger.log(now, flowId(), acked_psn, "rack_update",
                         _cwnd / _mtu, _in_flight / _mtu);
    }
}

void UecSrc::rackDetectLosses() {
    // Mark packets as lost if they were sent before rack.xmit_ts - reo_wnd.
    // Called after rackOnAckUpdate.
    // When RSS is active, each subflow has independent RACK state so that
    // cross-subflow reordering does not trigger spurious loss marks.
    if (_rack_tlp_mode == RackTlpMode::OFF ||
        _rack_tlp_mode == RackTlpMode::TLP_ONLY) return;

    simtime_picosec now = eventlist().now();
    bool use_per_subflow = !_rack_per_subflow.empty();

    // Collect lost packets across all subflows.
    std::vector<std::pair<UecDataPacket::seq_t, mem_b>> lost;

    if (use_per_subflow) {
        // Per-subflow RACK: scan _send_times and check each packet against
        // its own subflow's threshold.
        for (auto it = _send_times.begin(); it != _send_times.end(); ++it) {
            auto seqno = it->second;
            int sid = rackSubflowForSeqno(seqno);
            const RackState& rack = _rack_per_subflow[sid];
            // Guard: no sample yet, or xmit_ts too small for a valid threshold
            // (unsigned subtraction would underflow).
            if (rack.xmit_ts <= rack.reo_wnd) continue;
            simtime_picosec threshold = rack.xmit_ts - rack.reo_wnd;
            if (it->first > threshold) continue;  // not old enough for this subflow
            auto rec = _tx_bitmap.find(seqno);
            if (rec != _tx_bitmap.end()) {
                lost.push_back({seqno, rec->second.pkt_size});
            }
        }
    } else {
        // Legacy single-state RACK for non-RSS modes.
        if (_rack.xmit_ts <= _rack.reo_wnd) return;
        simtime_picosec threshold = _rack.xmit_ts - _rack.reo_wnd;
        for (auto it = _send_times.begin(); it != _send_times.end(); ++it) {
            if (it->first > threshold) break;
            auto seqno = it->second;
            auto rec = _tx_bitmap.find(seqno);
            if (rec != _tx_bitmap.end()) {
                lost.push_back({seqno, rec->second.pkt_size});
            }
        }
    }

    for (auto& [seqno, pkt_size] : lost) {
        // Remove from tracking
        auto rec = _tx_bitmap.find(seqno);
        if (rec == _tx_bitmap.end()) continue;
        simtime_picosec st = rec->second.send_time;
        _tx_bitmap.erase(rec);
        delFromSendTimes(st, seqno);

        // Queue for retransmission
        if (_sender_based_cc)
            mark_packet_for_retransmission(seqno, pkt_size);
        queueForRtx(seqno, pkt_size);
        _rack_stats.rack_loss_marks++;

        if (_rack_logger.is_open()) {
            _rack_logger.log(now, flowId(), seqno, "rack_loss",
                             _cwnd / _mtu, _in_flight / _mtu);
        }
    }

    if (!lost.empty()) {
        recalculateRTO();
    }
}

void UecSrc::rackArmReorderTimer() {
    // Simplified: we rely on the existing RTO timer rather than adding
    // a separate reorder timer. The RACK detection in rackDetectLosses()
    // fires on every ACK, which is frequent enough for sub-RTT detection.
}

void UecSrc::tlpComputeAndArmPTO() {
    // Dispatch: if RSS per-subflow TLP is active, arm per-subflow PTOs;
    // otherwise use the legacy single-PTO path.
    if (_rack_tlp_mode != RackTlpMode::RACK_TLP &&
        _rack_tlp_mode != RackTlpMode::RACK_TLP_NO_6675 &&
        _rack_tlp_mode != RackTlpMode::TLP_ONLY) return;

    if (!_tlp_per_subflow.empty()) {
        // Per-subflow: re-scan all subflows and pick earliest PTO
        tlpArmEarliestPTO();
    } else {
        // Single-PTO legacy path
        tlpComputeAndArmPTO(-1);
    }
}

void UecSrc::tlpComputeAndArmPTO(int subflow_id) {
    // Arm PTO for a specific subflow (or the global flow when subflow_id == -1).
    // Per RFC 8985 § 7.2: PTO = 2 × SRTT.
    if (_rack_tlp_mode != RackTlpMode::RACK_TLP &&
        _rack_tlp_mode != RackTlpMode::RACK_TLP_NO_6675 &&
        _rack_tlp_mode != RackTlpMode::TLP_ONLY) return;

    if (!_rtx_queue.empty()) return;
    if (_tx_bitmap.empty()) return;
    if (_done_sending && _backlog == 0) return;

    TlpState& tlp = (subflow_id >= 0 && !_tlp_per_subflow.empty())
                     ? _tlp_per_subflow[subflow_id] : _tlp;

    if (tlp.probe_in_flight) return;
    if (!tlp.has_rtt_sample) return;

    // Check that this subflow actually has packets in flight
    if (subflow_id >= 0) {
        bool has_inflight = false;
        for (auto& [seqno, rec] : _tx_bitmap) {
            if (rackSubflowForSeqno(seqno) == subflow_id) {
                has_inflight = true;
                break;
            }
        }
        if (!has_inflight) {
            tlp.pto_deadline = 0;
            return;
        }
    }

    simtime_picosec now = eventlist().now();

    // PTO = max(2 * SRTT, 10μs) using base_rtt before first ACK
    simtime_picosec srtt;
    if (_raw_rtt > 0) {
        srtt = _raw_rtt;
    } else if (_base_rtt > 0) {
        srtt = _base_rtt;
    } else {
        srtt = _rtt;
    }
    simtime_picosec pto = 2 * srtt;
    simtime_picosec min_pto = timeFromUs(10u);
    if (pto < min_pto) pto = min_pto;

    simtime_picosec pto_deadline = now + pto;

    // Don't arm if RTO would fire sooner
    if (_rtx_timeout_pending) {
        simtime_picosec rto_for_current = now + _rto;
        if (rto_for_current <= pto_deadline) {
            tlp.pto_deadline = 0;
            return;
        }
    }

    tlp.pto_deadline = pto_deadline;

    // For single-PTO (legacy), set the timer directly
    if (subflow_id < 0) {
        tlpCancelPTO();
        _tlp_pto_timeout = pto_deadline;
        _tlp_pto_subflow = -1;
        _tlp_pto_handle = eventlist().sourceIsPendingGetHandle(*this, pto_deadline);
        if (_tlp_pto_handle != eventlist().nullHandle()) {
            _tlp_pto_pending = true;
        }
    }
    // For per-subflow, caller (tlpArmEarliestPTO) handles the timer
}

void UecSrc::tlpArmEarliestPTO() {
    // Find the subflow with the earliest PTO deadline and set the single
    // event timer to that time. When it fires, we probe that subflow.
    if (_tlp_per_subflow.empty()) return;

    // First, recompute per-subflow deadlines for subflows that need it
    int n = (int)_tlp_per_subflow.size();
    simtime_picosec now = eventlist().now();
    for (int sid = 0; sid < n; sid++) {
        TlpState& tlp = _tlp_per_subflow[sid];

        // Check if this subflow has packets in flight
        bool has_inflight = false;
        for (auto& [seqno, rec] : _tx_bitmap) {
            if (rackSubflowForSeqno(seqno) == sid) {
                has_inflight = true;
                break;
            }
        }
        if (!has_inflight) {
            // Cumulative ACKs can retire packets from a subflow other than
            // the ACK packet's own subflow.  Discard that subflow's cached
            // PTO state so it cannot later schedule an event in the past.
            tlp.pto_deadline = 0;
            tlp.probe_in_flight = false;
            tlp.has_rtt_sample = true;
            continue;
        }
        if (tlp.probe_in_flight) continue;
        if (!tlp.has_rtt_sample) continue;
        if (tlp.pto_deadline > now) continue;  // valid deadline already computed

        // A deadline at or before now has expired or become stale while a
        // different subflow was selected.  Re-arm it from the current time.
        tlp.pto_deadline = 0;

        // Compute PTO deadline
        simtime_picosec srtt = (_raw_rtt > 0) ? _raw_rtt :
                               (_base_rtt > 0) ? _base_rtt : _rtt;
        simtime_picosec pto = 2 * srtt;
        simtime_picosec min_pto = timeFromUs(10u);
        if (pto < min_pto) pto = min_pto;
        simtime_picosec deadline = now + pto;

        if (_rtx_timeout_pending) {
            simtime_picosec rto_cur = now + _rto;
            if (rto_cur <= deadline) continue;
        }
        tlp.pto_deadline = deadline;
    }

    // Find earliest
    simtime_picosec earliest = UINT64_MAX;
    int earliest_sid = -1;
    for (int sid = 0; sid < n; sid++) {
        if (_tlp_per_subflow[sid].pto_deadline > 0 &&
            _tlp_per_subflow[sid].pto_deadline < earliest) {
            earliest = _tlp_per_subflow[sid].pto_deadline;
            earliest_sid = sid;
        }
    }

    if (earliest_sid < 0) return;  // no subflow needs PTO

    // If current timer is already set to this or earlier, keep it
    if (_tlp_pto_pending && _tlp_pto_timeout <= earliest) return;

    tlpCancelPTO();
    _tlp_pto_timeout = earliest;
    _tlp_pto_subflow = earliest_sid;
    _tlp_pto_handle = eventlist().sourceIsPendingGetHandle(*this, earliest);
    if (_tlp_pto_handle != eventlist().nullHandle()) {
        _tlp_pto_pending = true;
    }
}

void UecSrc::tlpCancelPTO() {
    if (_tlp_pto_pending) {
        eventlist().cancelPendingSourceByHandle(*this, _tlp_pto_handle);
        _tlp_pto_pending = false;
        _tlp_pto_handle = eventlist().nullHandle();
    }
}

void UecSrc::tlpSendProbe() {
    // PTO expired: dispatch to per-subflow or legacy single-probe path.
    simtime_picosec now = eventlist().now();
    _tlp_pto_pending = false;
    _tlp_pto_handle = eventlist().nullHandle();

    if (!_tlp_per_subflow.empty() && _tlp_pto_subflow >= 0) {
        // Per-subflow mode: probe the specific subflow, then check others
        int sid = _tlp_pto_subflow;
        _tlp_pto_subflow = -1;
        _tlp_per_subflow[sid].pto_deadline = 0;  // consumed
        tlpSendProbeForSubflow(sid);

        // Re-arm timer for any other subflows that need probing
        tlpArmEarliestPTO();
        return;
    }

    // Legacy single-probe path
    _rack_stats.tlp_probes_sent++;
    if (_rack_logger.is_open()) {
        _rack_logger.log(now, flowId(), _highest_sent, "tlp_probe",
                         _cwnd / _mtu, _in_flight / _mtu);
    }

    bool probe_sent = false;
    bool probe_queued = false;
    if (_backlog > 0) {
        uint64_t prev_highest_sent = _highest_sent;
        uint64_t prev_rtx_sent     = static_cast<uint64_t>(_stats.rtx_pkts_sent);
        _tlp.is_retrans            = false;
        sendIfPermitted();
        probe_sent = (_highest_sent > prev_highest_sent) ||
                     (static_cast<uint64_t>(_stats.rtx_pkts_sent) > prev_rtx_sent);
        if (probe_sent) {
            _tlp.end_seq = (_highest_sent > 0) ? (_highest_sent - 1) : 0;
            _tlp.probe_size = _mtu;
        }
        _tlp.probe_in_flight = probe_sent;
        _tlp.has_rtt_sample  = !probe_sent;
        return;
    }
    if (!_send_times.empty()) {
        auto last = _send_times.end();
        --last;
        auto seqno = last->second;
        auto rec = _tx_bitmap.find(seqno);
        if (rec != _tx_bitmap.end()) {
            mem_b pkt_size = rec->second.pkt_size;
            simtime_picosec st = rec->second.send_time;
            _tx_bitmap.erase(rec);
            delFromSendTimes(st, seqno);
            // The retransmission replaces this outstanding segment.  Without
            // removing the original bytes from the in-flight accounting, a
            // probe queued behind a busy NIC is rejected by the later cwnd
            // check and can leave a trace replay with no future event.
            if (_sender_based_cc) {
                _in_flight -= pkt_size;
            }
            _tlp.is_retrans = true;
            _tlp.end_seq = seqno;
            _tlp.probe_size = pkt_size;
            _tlp.probe_in_flight = true;
            _tlp.has_rtt_sample = false;
            queueForRtx(seqno, pkt_size);
            probe_queued = true;
            const Route* route = _nic.requestSending(*this);
            if (route) {
                bool bytes_sent = sendRtxPacket(*route);
                if (bytes_sent > 0) {
                    _nic.startSending(*this, bytes_sent, route);
                } else {
                    _nic.cantSend(*this);
                }
            }
        }
    }
    if (!probe_queued) {
        _tlp.probe_in_flight = false;
        _tlp.has_rtt_sample  = true;
    }
}

void UecSrc::tlpSendProbeForSubflow(int subflow_id) {
    // Send a TLP probe for a specific RSS subflow.
    // Retransmit the last unacked packet belonging to this subflow.
    _rack_stats.tlp_probes_sent++;
    TlpState& tlp = _tlp_per_subflow[subflow_id];

    if (_rack_logger.is_open()) {
        _rack_logger.log(eventlist().now(), flowId(), _highest_sent,
                         "tlp_probe_sf", _cwnd / _mtu, _in_flight / _mtu);
    }

    // Find the last (most recent) unacked packet on this subflow
    UecDataPacket::seq_t probe_seqno = 0;
    simtime_picosec probe_send_time = 0;
    mem_b probe_pkt_size = 0;
    bool found = false;

    // Scan _tx_bitmap for packets belonging to this subflow (pick highest PSN)
    for (auto& [seqno, rec] : _tx_bitmap) {
        if (rackSubflowForSeqno(seqno) == subflow_id) {
            if (!found || seqno > probe_seqno) {
                probe_seqno = seqno;
                probe_send_time = rec.send_time;
                probe_pkt_size = rec.pkt_size;
                found = true;
            }
        }
    }

    if (!found) {
        tlp.probe_in_flight = false;
        tlp.has_rtt_sample  = true;
        return;
    }

    // Remove from tracking and queue for retransmission
    auto rec = _tx_bitmap.find(probe_seqno);
    _tx_bitmap.erase(rec);
    delFromSendTimes(probe_send_time, probe_seqno);
    // Account for the original outstanding segment before the replacement
    // probe is queued. sendRtxPacket() adds these bytes back when it sends.
    // This is essential when the NIC callback, rather than this function,
    // performs the send and therefore enforces the normal cwnd check.
    if (_sender_based_cc) {
        _in_flight -= probe_pkt_size;
    }

    tlp.is_retrans = true;
    tlp.end_seq = probe_seqno;
    tlp.probe_size = probe_pkt_size;
    tlp.probe_in_flight = true;
    tlp.has_rtt_sample = false;
    queueForRtx(probe_seqno, probe_pkt_size);

    // Try to send immediately
    const Route* route = _nic.requestSending(*this);
    if (route) {
        mem_b bytes_sent = sendRtxPacket(*route);
        if (bytes_sent > 0) {
            _nic.startSending(*this, bytes_sent, route);
        } else {
            _nic.cantSend(*this);
        }
    }

    // Keep the probe active if the NIC queues it for a later callback. An ACK
    // for the original copy can then classify it as spurious.
}

void UecSrc::scheduleRtxPacedSend() {
    if (_rtx_queue.empty()) return;

    simtime_picosec now = eventlist().now();
    simtime_picosec srtt = (_raw_rtt > 0) ? _raw_rtt :
                           (_base_rtt > 0) ? _base_rtt : _rtt;
    // Pace at subflow granularity: SRTT / num_subflows
    // With N subflows, each subflow has ~1 lost pkt, so this gives
    // effective 1-pkt-per-subflow-RTT pacing like RACK-TLP probing.
    uint16_t nsub = (_rss_params._rss_number_of_subflows > 1) ? _rss_params._rss_number_of_subflows : 1;
    simtime_picosec interval = srtt / nsub;
    if (interval < 1000) interval = 1000;  // floor at 1ns

    simtime_picosec send_at = max(now, _rtx_pace_next);
    // Add random initial jitter to desynchronize senders during incast
    if (_pflr_rtx_jitter_ratio > 0.0 && _rtx_pace_next <= now) {
        // First RTX in this batch: add U[0, ratio*SRTT] jitter
        double max_jitter = _pflr_rtx_jitter_ratio * (double)srtt;
        simtime_picosec jitter = (simtime_picosec)(((double)rand() / RAND_MAX) * max_jitter);
        send_at += jitter;
    }
    _rtx_pace_next = send_at + interval;

    if (send_at <= now) {
        sendIfPermitted();
        return;
    }

    // Don't cancel existing timer - just schedule if none pending
    if (!_rtx_pace_pending) {
        _rtx_pace_timer = send_at;
        _rtx_pace_handle = eventlist().sourceIsPendingGetHandle(*this, send_at);
        _rtx_pace_pending = (_rtx_pace_handle != eventlist().nullHandle());
    }
}
