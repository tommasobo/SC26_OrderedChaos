// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
// #include "types.h"
#include <math.h>
#include <string.h>
#include <unistd.h>

#include <filesystem>
#include <list>
#include <sstream>
#include <mcheck.h>


#include "logsim-interface.h"
#include "compositequeue.h"
#include "connection_matrix.h"
#include "data_collector.h"
#include "event_source.h"
#include "eventlist.h"
#include "fat_tree_switch.h"
#include "fat_tree_topology.h"
#include "helpers.h"
#include "logfile.h"
#include "oversubscribed_cc.h"
#include "packet.h"
#include "packet_flow.h"
#include "pcie_model.h"
#include "pipe.h"
#include "topology.h"
#include "uec_logger.h"
#include "uec_pull_pacer.h"
#include "uec_sink.h"
#include "uec_sink_port.h"
#include "uec_src.h"
#include "uec_src_port.h"

namespace fs = std::filesystem;

// Simulation params

// #define PRINTPATHS 1

#include "topology.h"

int DEFAULT_NODES = 128;
#define DEFAULT_QUEUE_SIZE 35
// #define DEFAULT_CWND 50

EventList& eventlist = EventList::getTheEventList();

enum RouteStrategy {
    NOT_SET,
    SINGLE_PATH,
    SCATTER_PERMUTE,
    SCATTER_RANDOM,
    PULL_BASED,
    SCATTER_ECMP,
    ECMP_FIB,
    ECMP_FIB_ECN,
    REACTIVE_ECN
};

void exit_error(char* progr) {
    cout << "Usage " << progr
         << " [-nodes N]\n\t[-conns C]\n\t[-cwnd cwnd_size]\n\t[-q "
            "queue_size]\n\t[-recv_oversub_cc] Use receiver-driven AIMD to reduce total window "
            "when trims are not last hop\n\t[-queue_type "
            "composite|random|lossless|lossless_input|]\n\t[-tm traffic_matrix_file]\n\t[-strat "
            "route_strategy (single,rand,perm,pull,ecmp,\n\tecmp_host "
            "path_count,ecmp_ar,ecmp_rr,\n\tecmp_host_ar ar_thresh)]\n\t[--load_balancing_algo "
            "bitmap|reps|oblivious|mixed|ecmp|rss]\n\t[-log log_level]\n\t[-seed "
            "random_seed]\n\t[-end end_time_in_usec]\n\t[-mtu MTU]\n\t[-hop_latency x] per hop "
            "wire latency in us,default 1 \n\t[-disable_fd] disable fair decrease to get higher "
            "throught, \n\t[-target_q_delay x] target_queuing_delay in us, default is 6us "
            "\n\t[-switch_latency x] switching latency in us, default 0\n\t[-host_queue_type  "
            "swift|prio|fair_prio]\n\t[-logtime dt] sample time for sinklogger, "
             "etc\n\t[-rss_parameters rtt|ecn rss_number_of_subflows rss_update_period "
             "skip_threshold (0 for default) max_number_of_rounds_to_skip (0 for default) "
            "period_jitter(%)] update period is in us\n\t[-lgs_compute_time_override ns] override "
             "all trace local-compute ops with a fixed duration in ns\n\t[-rto_us us] set an "
             "absolute fixed RTO and override -rto_ratio\n\t[-flowbender_parameters ecn_threshold "
             "consecutive_rtt_number]\n\t[-USS_parameters number_of_subflows]"
          << endl;
    exit(1);
}

int main(int argc, char** argv) {
    //mtrace();
    bool            param_queuesize_set = false;
    mem_b           queuesize           = DEFAULT_QUEUE_SIZE;
    linkspeed_bps   linkspeed           = speedFromMbps((double)HOST_NIC);
    int             packet_size         = 4150;
    uint32_t        path_entropy_size   = 256;
    uint32_t        no_of_conns = 0, cwnd = 0, no_of_nodes = 0;
    uint32_t        tiers                   = 3;      // we support 2 and 3 tier fattrees
    uint32_t        planes                  = 1;      // multi-plane topologies
    uint32_t        ports                   = 1;      // ports per NIC
    bool            disable_trim            = false;  // Disable trimming, drop instead
    bool            low_priority_trim       = false;  // low priority trim if disable trimming
    bool            no_droping_low_header   = false;
    double          switch_random_drop_prob = 0;
    uint16_t        trimsize                = 64;                // size of a trimmed packet
    simtime_picosec logtime                 = timeFromMs(0.25);  // ms;
    stringstream    filename(ios_base::out);
    simtime_picosec hop_latency    = timeFromUs((uint32_t)1);
    simtime_picosec switch_latency = timeFromUs((uint32_t)0);
    queue_type      qt             = COMPOSITE;
    std::string     tiers_latency;
    UecSrc::_rss_params                   = {8, timeFromUs(200.), UecSrc::MEAN_RTT, 32, 0, 0, 25};
    UecSrc::_flowbender_params            = {0.05, 1};
    UecSrc::_uss_params                   = {8, 3};
    UecSrc::ecmp_background_traffic_nodes = 0;

    UecSrc::_load_balancing_algo = UecSrc::MIXED;

    bool log_sink        = false;
    bool log_flow_events = true;

    bool            log_tor_downqueue = false;
    bool            log_tor_upqueue   = false;
    bool            log_traffic       = false;
    bool            log_switches      = false;
    bool            log_queue_usage   = false;
    double          ecn_thresh        = 0.5;  // default marking threshold for ECN load balancing
    simtime_picosec target_Qdelay     = 0;

    bool  param_ecn_set = false;
    bool  ecn           = true;
    mem_b ecn_low = 0.2 * queuesize, ecn_high = 0.8 * queuesize;

    bool receiver_driven = true;
    bool sender_driven   = false;

    int number_iterations = 1;
    int    rto_ratio = 1;
    double rto_us    = -1.0;

    int force_finish = 0; // if > 0, forces the simulation to finish after this many flows complete

    RouteStrategy route_strategy = NOT_SET;

    int    seed       = 13;
    int    path_burst = 1;
    int    i          = 1;
    double pcie_rate  = 1.1;

    filename << "logout.dat";
    int  end_time                        = 1000;  // in microseconds
    bool force_disable_oversubscribed_cc = false;
    bool enable_accurate_base_rtt        = true;
    string goal_filename = "";
    // unsure how to set this.
    queue_type snd_type = FAIR_PRIO;

    float                         ar_sticky_delta = 10;
    FatTreeSwitch::sticky_choices ar_sticky       = FatTreeSwitch::PER_PACKET;

    char* tm_file   = NULL;
    char* topo_file = NULL;
    int percentage_bg = 0;
    // bool disable_fair_decrease = true;
    bool   enable_qa_gate               = true;
    double fast_increase_scaling_factor = UecSrc::get_fast_increase_scaling_factor();
    double prop_increase_scaling_factor = UecSrc::get_prop_increase_scaling_factor();
    double fair_increase_scaling_factor = UecSrc::get_fair_decrease_scaling_factor();
    double fair_decrease_scaling_factor = UecSrc::get_fair_decrease_scaling_factor();
    double mult_decrease_scaling_factor = UecSrc::get_mult_decrease_scaling_factor();
    bool   use_exp_avg_ecn              = UecSrc::get_use_exp_avg_ecn();
    int64_t lgs_compute_time_override_ns = -1;

    string data_collection_dir = "";

    while (i < argc) {
        if (!strcmp(argv[i], "-data_collection_config")) {
            DataCollector::InitWithConfig(argv[i + 1]);
            cout << "Data collector initialized with config file " << argv[i + 1] << endl;
            i++;
        } else if (!strcmp(argv[i], "-data_collection_dir")) {
            data_collection_dir = argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "-o")) {
            filename.str(std::string());
            filename << argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "-goal")) {
            goal_filename = argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "-conns")) {
            no_of_conns = atoi(argv[i + 1]);
            cout << "no_of_conns " << no_of_conns << endl;
            i++;
        } else if (!strcmp(argv[i], "-end")) {
            end_time = atoi(argv[i + 1]);
            cout << "endtime(us) " << end_time << endl;
            i++;
        } else if (!strcmp(argv[i], "-nodes")) {
            no_of_nodes = atoi(argv[i + 1]);
            cout << "no_of_nodes " << no_of_nodes << endl;
            i++;
        } else if (!strcmp(argv[i], "-tiers")) {
            tiers = atoi(argv[i + 1]);
            cout << "tiers " << tiers << endl;
            assert(tiers == 2 || tiers == 3);
            i++;
        } else if (!strcmp(argv[i], "-iterations")) {
            number_iterations = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-force_finish")) {
            force_finish = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rto_ratio")) {
            rto_ratio = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rto_us")) {
            rto_us = atof(argv[i + 1]);
            if (rto_us <= 0) {
                cerr << "-rto_us must be positive" << endl;
                exit(1);
            }
            i++;
        } else if (!strcmp(argv[i], "-lgs_percent")) {
            LogSimInterface::percentage_lgs = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-lgs_compute_time_override")) {
            lgs_compute_time_override_ns = atoll(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-planes")) {
            planes = atoi(argv[i + 1]);
            ports  = planes;
            cout << "planes " << planes << endl;
            cout << "ports per NIC " << ports << endl;
            assert(planes >= 1 && planes <= 8);
            i++;
        } else if (!strcmp(argv[i], "-fasti_scaling_factor")) {
            fast_increase_scaling_factor = std::stod(argv[i + 1]);
            UecSrc::set_fast_increase_scaling_factor(fast_increase_scaling_factor);
            printf("Fast increase: %f\n", fast_increase_scaling_factor);
            i++;
        } else if (!strcmp(argv[i], "-pi_scaling_factor")) {
            prop_increase_scaling_factor = std::stod(argv[i + 1]);
            UecSrc::set_prop_increase_scaling_factor(prop_increase_scaling_factor);
            printf("Prop increase: %f\n", prop_increase_scaling_factor);
            i++;
        } else if (!strcmp(argv[i], "-fi_scaling_factor")) {
            fair_increase_scaling_factor = std::stod(argv[i + 1]);
            UecSrc::set_fair_increase_scaling_factor(fair_increase_scaling_factor);
            printf("Fair increase: %f\n", fair_increase_scaling_factor);
            i++;
        } else if (!strcmp(argv[i], "-fd_scaling_factor")) {
            fair_decrease_scaling_factor = std::stod(argv[i + 1]);
            UecSrc::set_fair_decrease_scaling_factor(fair_decrease_scaling_factor);
            printf("Fair decrease: %f\n", fair_decrease_scaling_factor);
            i++;
        } else if (!strcmp(argv[i], "-md_scaling_factor")) {
            mult_decrease_scaling_factor = std::stod(argv[i + 1]);
            UecSrc::set_mult_decrease_scaling_factor(mult_decrease_scaling_factor);
            printf("Mult decrease: %f\n", mult_decrease_scaling_factor);
            i++;
        } else if (!strcmp(argv[i], "-sender_cc_only")) {
            UecSrc::_sender_based_cc    = true;
            UecSrc::_receiver_based_cc  = false;
            UecSink::_oversubscribed_cc = false;
            sender_driven               = true;
            receiver_driven             = false;
            cout << "sender based CC enabled ONLY" << endl;
            //        } else if (!strcmp(argv[i],"-disable_fd")) {
            //            disable_fair_decrease = true;
            //            cout << "fair_decrease disabled" << endl;
        } else if (!strcmp(argv[i], "-enable_qa_gate")) {
            enable_qa_gate = true;
        } else if (!strcmp(argv[i], "-full_fail")) {
            FatTreeTopology::full_fail = true;
        } else if (!strcmp(argv[i], "-target_q_delay")) {
            target_Qdelay = timeFromUs(atof(argv[i + 1]));
            cout << "target_q_delay" << atof(argv[i + 1]) << " us" << endl;
            i++;
        } else if (!strcmp(argv[i], "-sender_cc_algo")) {
            UecSrc::_sender_based_cc = true;
            sender_driven            = true;

            if (!strcmp(argv[i + 1], "dctcp"))
                UecSrc::_sender_cc_algo = UecSrc::DCTCP;
            else if (!strcmp(argv[i + 1], "nscc"))
                UecSrc::_sender_cc_algo = UecSrc::NSCC;
            else if (!strcmp(argv[i + 1], "constant"))
                UecSrc::_sender_cc_algo = UecSrc::CONSTANT;
            else if (!strcmp(argv[i + 1], "smartt"))
                UecSrc::_sender_cc_algo = UecSrc::SMARTT;
            else if (!strcmp(argv[i + 1], "smartt_ecn_aimd"))
                UecSrc::_sender_cc_algo = UecSrc::SMARTT_ECN_AIMD;
            else if (!strcmp(argv[i + 1], "smartt_ecn_aifd"))
                UecSrc::_sender_cc_algo = UecSrc::SMARTT_ECN_AIFD;
            else if (!strcmp(argv[i + 1], "smartt_ecn_fimd"))
                UecSrc::_sender_cc_algo = UecSrc::SMARTT_ECN_FIMD;
            else if (!strcmp(argv[i + 1], "smartt_ecn_fifd"))
                UecSrc::_sender_cc_algo = UecSrc::SMARTT_ECN_FIFD;
            else if (!strcmp(argv[i + 1], "smartt_rtt"))
                UecSrc::_sender_cc_algo = UecSrc::SMARTT_RTT;
            else {
                cout << "UNKNOWN CC ALGO " << argv[i + 1] << endl;
                exit(1);
            }
            cout << "sender based algo " << argv[i + 1] << endl;
            i++;
        } else if (!strcmp(argv[i], "-use_wait_to_decrease")) {
            use_exp_avg_ecn = atoi(argv[i + 1]);
            printf("Use wait to decrease: %d\n", use_exp_avg_ecn);
            UecSrc::set_use_exp_avg_ecn(use_exp_avg_ecn);
            i++;
        } else if (!strcmp(argv[i], "-sender_cc")) {
            UecSrc::_sender_based_cc    = true;
            UecSink::_oversubscribed_cc = false;
            sender_driven               = true;
            cout << "sender based CC enabled " << endl;
        } else if (!strcmp(argv[i], "-load_balancing_algo")) {
            if (!strcmp(argv[i + 1], "bitmap")) {
                UecSrc::_load_balancing_algo = UecSrc::BITMAP;
            } else if (!strcmp(argv[i + 1], "reps")) {
                UecSrc::_load_balancing_algo = UecSrc::REPS;
            } else if (!strcmp(argv[i + 1], "flowlet")) {
                UecSrc::_load_balancing_algo = UecSrc::FLOWLET;
            } else if (!strcmp(argv[i + 1], "oblivious")) {
                UecSrc::_load_balancing_algo = UecSrc::OBLIVIOUS;
            } else if (!strcmp(argv[i + 1], "mixed")) {
                UecSrc::_load_balancing_algo = UecSrc::MIXED;
            } else if (!strcmp(argv[i + 1], "rss")) {
                UecSrc::_load_balancing_algo = UecSrc::RSS;
            } else if (!strcmp(argv[i + 1], "ecmp")) {
                UecSrc::_load_balancing_algo = UecSrc::ECMP;
            } else if (!strcmp(argv[i + 1], "flowbender")) {
                UecSrc::_load_balancing_algo = UecSrc::FLOWBENDER;
            } else if (!strcmp(argv[i + 1], "uss")) {
                UecSrc::_load_balancing_algo = UecSrc::USS;
            } else {
                cout << "Unknown load balancing algorithm of type " << argv[i + 1]
                     << ", expecting bitmap, reps, oblivious, mixed, ecmp, flowbender, uss, or rss"
                     << endl;
                exit_error(argv[0]);
            }
            cout << "Load balancing algorithm set to " << argv[i + 1] << endl;
            i++;
        } else if (!strcmp(argv[i], "-background_traffic")) {
            UecSrc::ecmp_background_traffic_nodes = static_cast<uint16_t>(atoi(argv[i + 1]));
            cout << "Number of nodes using ECMP to emulate background traffic: " << argv[i + 1]
                 << endl;
            i++;
        } else if (!strcmp(argv[i], "-rss_parameters")) {
            UecSrc::RSSWorseEntropyMetric _rss_worse_entropy_metric = UecSrc::MEAN_RTT;
            if (!strcmp(argv[i + 1], "mean_rtt")) {
                _rss_worse_entropy_metric = UecSrc::MEAN_RTT;
            } else if (!strcmp(argv[i + 1], "ecn")) {
                _rss_worse_entropy_metric = UecSrc::ECN;
            } else if (!strcmp(argv[i + 1], "worse_rtt")) {
                _rss_worse_entropy_metric = UecSrc::WORSE_RTT;
            } else {
                exit_error(argv[0]);
            }
            int _rss_number_of_subflows = atoi(argv[i + 2]);
            if (_rss_number_of_subflows <= 0 ||
                _rss_number_of_subflows >
                    numeric_limits<uint8_t>::max()) {  // We really need to check that it holds in
                                                       // 16 bits but this makes sure we have at
                                                       // least 8 bits of random entropy
                exit_error(argv[0]);
            }
            UecSrc::_rss_params = {
                static_cast<uint16_t>(_rss_number_of_subflows),
                timeFromUs(atof(argv[i + 3])),
                _rss_worse_entropy_metric,
                (32 - __builtin_clz(static_cast<uint16_t>(_rss_number_of_subflows) - 1)) |
                    1,  // TODO(aghalayini): why the | 1?
                atof(argv[i + 4]),
                static_cast<uint32_t>(atoi(argv[i + 5])),
                static_cast<uint32_t>(atoi(argv[i + 6]))};
            i += 6;
            cout << "Setting RSS parameters: " << UecSrc::_rss_params._rss_number_of_subflows
                 << " subflows, update the worst path's entropy every "
                 << UecSrc::_rss_params._rss_update_interval << "us, use "
                 << UecSrc::_rss_params._rss_worse_entropy_metric << " to determine worse path.\n";
        }
        // precise fast loss recovery
        else if (!strcmp(argv[i], "-precisefastlossrecovery")) {
            UecSrc::_enable_precise_fast_loss_recovery = true;
            cout << "Using precise fast loss recovery (pflr) scheme " << endl;
            // scheme ID
            UecSrc::_pflr_scheme_id  = atoi(argv[i + 1]);
            UecSink::_pflr_scheme_id = atoi(argv[i + 1]);
            cout << "pflr scheme id: " << UecSrc::_pflr_scheme_id << endl;
            cout << "Currently supported pflr schemes: " << endl;
            cout << "ID 0: linked list impl" << endl;
            cout << "ID 1: Static EV on slots. Receiver keeps slot state, probe packet for last "
                    "packet EV = (slot id, rand)."
                 << endl;
            cout << "ID 2: Dynamic EV with generations. Receiver keeps slot state, probe packet "
                    "when change EV. EV = (slot id, generation)"
                 << endl;
            cout << "ID 3: Base PSN scheme. EV = LSB(PSN). Receiver keeps bitmap." << endl;
            cout << "ID 4: EV is stored inside the network." << endl;
            cout << "ID 5: Scheme 3 + counter map for loss in RTX." << endl;
            i++;
        }
        // else if (!strcmp(argv[i],"-pflr_print_debug_msg")){
        //     UecSrc::_pflr_print_debug_msg = true;
        //     UecSink::_pflr_print_debug_msg = true;
        //     cout << "print debug message for pflr " << endl;
        // }
        else if (!strcmp(argv[i], "-pflr_proactive_probe")) {
            UecSrc::_pflr_proactive_probe = true;
            cout << "Pflr use proactive probe for data packet" << endl;
            UecSrc::_pflr_proactive_probe_pkt_count = atoi(argv[i + 1]);
            cout << "Pflr proactive probe every " << UecSrc::_pflr_proactive_probe_pkt_count
                 << " packets " << endl;
            i++;
        } else if (!strcmp(argv[i], "-pflr_disable_probe")) {
            // The sender already supports running PFLD without probe packets;
            // expose that existing switch for a reproducible probe ablation.
            UecSrc::_pflr_disable_probe = true;
            cout << "Pflr disable tail, data, and retransmission probes" << endl;
        } else if (!strcmp(argv[i], "-pflr_receiver_trim_probe_coalescing")) {
            UecSink::_pflr_receiver_trim_probe_coalescing = true;
            cout << "Pflr coalesce trim and ordinary probe NACKs at receiver" << endl;
        } else if (!strcmp(argv[i], "-pflr_accept_header_promoted_probes")) {
            UecSink::_pflr_accept_header_promoted_probes = true;
            cout << "Pflr accept probes promoted out of the low-priority FIFO" << endl;
        } else if (!strcmp(argv[i], "-pflr_proactive_rtx_probe")) {
            UecSrc::_pflr_proactive_rtx_probe = true;
            cout << "Pflr use proactive probe for rtx packet" << endl;
        } else if (!strcmp(argv[i], "-pflr_pace_rtx")) {
            UecSrc::_pflr_pace_rtx = true;
            cout << "Pflr pacing NACK-triggered retransmissions (cwnd/RTT)" << endl;
        } else if (!strcmp(argv[i], "-pflr_rtx_jitter")) {
            UecSrc::_pflr_rtx_jitter_ratio = atof(argv[i + 1]);
            cout << "Pflr RTX jitter ratio: " << UecSrc::_pflr_rtx_jitter_ratio << " * SRTT" << endl;
            i++;
        } else if (!strcmp(argv[i], "-pflr4_pkt_per_slot")) {
            UecSrc::_pflr4_no_packet_per_slot = atoi(argv[i + 1]);
            cout << "Pflr4 # pkt per slot: " << UecSrc::_pflr4_no_packet_per_slot << endl;
            i++;
        } else if (!strcmp(argv[i], "-pflr4_use_ev_recovery")) {
            UecSrc::_pflr4_use_ev_recovery = true;
            cout << "Pflr4 use ev recovery scheme" << endl;
        } else if (!strcmp(argv[i], "-pflr5_counter_map_bit_count")) {
            UecSrc::_pflr5_counter_map_bit_count = atoi(argv[i + 1]);
            cout << "Pflr5 counter map bit count: " << UecSrc::_pflr5_counter_map_bit_count << endl;
            i++;
        } else if (!strcmp(argv[i], "-rack_tlp")) {
            int mode = atoi(argv[i + 1]);
            UecSrc::_rack_tlp_mode = static_cast<RackTlpMode>(mode);
            cout << "RACK-TLP mode set to " << mode
                 << " (0=off, 1=rack, 2=rack+tlp, 3=rack+tlp-no6675, 4=tlp-only)" << endl;
            i++;
        } else if (!strcmp(argv[i], "-rack_tlp_log")) {
            UecSrc::_rack_tlp_log_dir = argv[i + 1];
            cout << "RACK-TLP logging to " << argv[i + 1] << endl;
            i++;
        } else if (!strcmp(argv[i], "-tlp_confirmed_loss_cwnd")) {
            UecSrc::_tlp_confirmed_loss_cwnd = true;
            cout << "TLP confirmed repairs invoke the loss cwnd response" << endl;
        } else if (!strcmp(argv[i], "-log_reaction_events")) {
            UecSrc::_log_reaction_events = true;
            cout << "Legacy Drop:/RTX: reaction-event logging enabled" << endl;
        } else if (!strcmp(argv[i], "-log_subflow_routes")) {
            FatTreeSwitch::_log_subflow_routes = true;
            cout << "Per-switch RSS subflow route logging enabled" << endl;
        } else if (!strcmp(argv[i], "-flowbender_parameters")) {
            int flowbender_consecutive_rtt_number = atoi(argv[i + 2]);
            if (flowbender_consecutive_rtt_number <= 0 ||
                flowbender_consecutive_rtt_number > numeric_limits<uint16_t>::max()) {
                exit_error(argv[0]);
            }
            UecSrc::_flowbender_params = {atof(argv[i + 1]),
                                          static_cast<uint16_t>(flowbender_consecutive_rtt_number)};
            i += 2;
        } else if (!strcmp(argv[i], "-uss_parameters")) {
            int number_of_subflows = atoi(argv[i + 1]);
            if (number_of_subflows <= 0 || number_of_subflows > numeric_limits<uint16_t>::max()) {
                exit_error(argv[0]);
            }
            UecSrc::_uss_params = {
                static_cast<uint16_t>(number_of_subflows),
                (32 - __builtin_clz(static_cast<uint16_t>(number_of_subflows) - 1)) |
                    1};  // TODO(aghalayini): why the | 1?
            i += 1;
            cout << "Setting USS parameters: " << UecSrc::_uss_params._number_of_subflows
                 << " subflows.\n";
        } else if (!strcmp(argv[i], "-queue_type")) {
            if (!strcmp(argv[i + 1], "composite")) {
                qt = COMPOSITE;
            } else if (!strcmp(argv[i + 1], "composite_ecn")) {
                qt = COMPOSITE_ECN;
            } else if (!strcmp(argv[i + 1], "aeolus")) {
                qt = AEOLUS;
            } else if (!strcmp(argv[i + 1], "aeolus_ecn")) {
                qt = AEOLUS_ECN;
            } else {
                cout << "Unknown queue type " << argv[i + 1] << endl;
                exit_error(argv[0]);
            }
            cout << "queue_type " << qt << endl;
            i++;
        } else if (!strcmp(argv[i], "-debug")) {
            UecSrc::_debug = true;
        } else if (!strcmp(argv[i], "-trace_rtx")) {
            UecSrc::_trace_rtx = true;
        } else if (!strcmp(argv[i], "-probe_high_priority")) {
            CompositeQueue::_probe_high_priority = true;
            cout << "TLP probes will bypass queue drops (high-priority mode)" << endl;
        } else if (!strcmp(argv[i], "-host_queue_type")) {
            if (!strcmp(argv[i + 1], "swift")) {
                snd_type = SWIFT_SCHEDULER;
            } else if (!strcmp(argv[i + 1], "prio")) {
                snd_type = PRIORITY;
            } else if (!strcmp(argv[i + 1], "fair_prio")) {
                snd_type = FAIR_PRIO;
            } else {
                cout << "Unknown host queue type " << argv[i + 1]
                     << " expecting one of swift|prio|fair_prio" << endl;
                exit_error(argv[0]);
            }
            cout << "host queue_type " << snd_type << endl;
            i++;
        } else if (!strcmp(argv[i], "-log")) {
            if (!strcmp(argv[i + 1], "flow_events")) {
                log_flow_events = true;
            } else if (!strcmp(argv[i + 1], "sink")) {
                cout << "logging sinks\n";
                log_sink = true;
            } else if (!strcmp(argv[i + 1], "tor_downqueue")) {
                cout << "logging tor downqueues\n";
                log_tor_downqueue = true;
            } else if (!strcmp(argv[i + 1], "tor_upqueue")) {
                cout << "logging tor upqueues\n";
                log_tor_upqueue = true;
            } else if (!strcmp(argv[i + 1], "switch")) {
                cout << "logging total switch queues\n";
                log_switches = true;
            } else if (!strcmp(argv[i + 1], "traffic")) {
                cout << "logging traffic\n";
                log_traffic = true;
            } else if (!strcmp(argv[i + 1], "queue_usage")) {
                cout << "logging queue usage\n";
                log_queue_usage = true;
            } else {
                exit_error(argv[0]);
            }
            i++;
        } else if (!strcmp(argv[i], "-cwnd")) {
            cwnd = atoi(argv[i + 1]);
            cout << "cwnd " << cwnd << endl;
            i++;
        } else if (!strcmp(argv[i], "-tm")) {
            tm_file = argv[i + 1];
            cout << "traffic matrix input file: " << tm_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-topo")) {
            topo_file = argv[i + 1];
            cout << "FatTree topology input file: " << topo_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-q")) {
            param_queuesize_set = true;
            queuesize           = atoi(argv[i + 1]);
            cout << "Setting queuesize to " << queuesize << " packets " << endl;
            i++;
        } else if (!strcmp(argv[i], "-sack_threshold")) {
            UecSink::_bytes_unacked_threshold = atoi(argv[i + 1]);
            cout << "Setting receiver SACK bytes threshold to " << UecSink::_bytes_unacked_threshold
                 << " bytes " << endl;
            i++;
        } else if (!strcmp(argv[i], "-oversubscribed_cc")) {
            UecSink::_oversubscribed_cc = true;
            cout << "Using receiver oversubscribed CC " << endl;
        } else if (!strcmp(argv[i], "-Ai")) {
            OversubscribedCC::_Ai = atof(argv[i + 1]);
            cout << "Using Ai " << OversubscribedCC::_Ai << endl;
            i += 1;
        } else if (!strcmp(argv[i], "-Md")) {
            OversubscribedCC::_Md = atof(argv[i + 1]);
            cout << "Using Md " << OversubscribedCC::_Md << endl;
            i += 1;
        } else if (!strcmp(argv[i], "-alpha")) {
            OversubscribedCC::_alpha = atof(argv[i + 1]);
            cout << "Using Alpha " << OversubscribedCC::_alpha << endl;
            i += 1;
        } else if (!strcmp(argv[i], "-force_disable_oversubscribed_cc")) {
            UecSink::_oversubscribed_cc     = false;
            force_disable_oversubscribed_cc = true;
            cout << "Disabling receiver oversubscribed CC even with OS topology" << endl;
        } else if (!strcmp(argv[i], "-disable_accurate_base_rtt")) {
            enable_accurate_base_rtt = false;
            cout << "Disabling accurate base rtt configuration, each flow takes network wide rtt "
                    "as the base rtt upper bound."
                 << endl;
        } else if (!strcmp(argv[i], "-sleek")) {
            UecSrc::_enable_sleek = true;
            cout << "Using SLEEK, the sender-based fast loss recovery heuristic " << endl;
        } else if (!strcmp(argv[i], "-ecn")) {
            // fraction of queuesize, between 0 and 1
            param_ecn_set = true;
            ecn           = true;
            ecn_low       = atoi(argv[i + 1]);
            ecn_high      = atoi(argv[i + 2]);
            i += 2;
        } else if (!strcmp(argv[i], "-disable_trim")) {
            disable_trim = true;
            cout << "Trimming disabled, dropping instread." << endl;
        } else if (!strcmp(argv[i], "-low_priority_trim")) {
            low_priority_trim = true;
            cout << "Use low priority **if trim is disabled**." << endl;
        } else if (!strcmp(argv[i], "-switch_random_drop_prob")) {
            switch_random_drop_prob = atof(argv[i + 1]);
            cout << "switch_random_drop_prob " << switch_random_drop_prob << endl;
            i++;
        } else if (!strcmp(argv[i], "-no_droping_low_header")) {
            no_droping_low_header = true;
            cout << "Not dropping header pkts in low priority queue" << endl;
        } else if (!strcmp(argv[i], "-trimsize")) {
            // size of trimmed packet in bytes
            trimsize = atoi(argv[i + 1]);
            cout << "trimmed packet size: " << trimsize << " bytes\n";
            i += 1;
        } else if (!strcmp(argv[i], "-logtime")) {
            double log_ms = atof(argv[i + 1]);
            logtime       = timeFromMs(log_ms);
            cout << "logtime " << logtime << " ms" << endl;
            i++;
        } else if (!strcmp(argv[i], "-logtime_us")) {
            double log_us = atof(argv[i + 1]);
            logtime       = timeFromUs(log_us);
            cout << "logtime " << log_us << " us" << endl;
            i++;
        } else if (!strcmp(argv[i], "-failed")) {
            // Number of aggregate switches whose links use the failed-link ratio.
            int num_failed = atoi(argv[i + 1]);
            FatTreeTopology::set_failed_links(num_failed);
            i++;
        } else if (!strcmp(argv[i], "-failed_link_ratio")) {
            double ratio = atof(argv[i + 1]);
            if (ratio <= 0.0 || ratio > 1.0) {
                cout << "failed_link_ratio must be in (0, 1]" << endl;
                exit(1);
            }
            FatTreeTopology::set_failed_link_ratio(ratio);
            cout << "Failed aggregate link rate ratio " << ratio << endl;
            i++;
        } else if (!strcmp(argv[i], "-failed_bidirectional")) {
            FatTreeTopology::set_failed_bidirectional(true);
            cout << "Failed aggregate links are reduced in both directions" << endl;
        } else if (!strcmp(argv[i], "-percentage_bg")) {
            percentage_bg = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-to_fail")) {
            int fail = atoi(argv[i + 1]);
            FatTreeTopology::_num_to_fail = fail;
            i++;
        } else if (!strcmp(argv[i], "-fail_psn")) {
            // number of failed links (failed to 10% linkspeed)
            int fail_psn_num = atoi(argv[i + 1]);
            CompositeQueue::_fail_psn_num = (fail_psn_num);
            i++;
        } else if (!strcmp(argv[i], "-linkspeed")) {
            // linkspeed specified is in Mbps
            linkspeed = speedFromMbps(atof(argv[i + 1]));
            i++;
        } else if (!strcmp(argv[i], "-seed")) {
            seed = atoi(argv[i + 1]);
            cout << "random seed " << seed << endl;
            i++;
        } else if (!strcmp(argv[i], "-mtu")) {
            packet_size = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-paths")) {
            path_entropy_size = atoi(argv[i + 1]);
            cout << "no of paths " << path_entropy_size << endl;
            i++;
        } else if (!strcmp(argv[i], "-path_burst")) {
            path_burst = atoi(argv[i + 1]);
            cout << "path burst " << path_burst << endl;
            i++;
        } else if (!strcmp(argv[i], "-hop_latency")) {
            hop_latency = timeFromUs(atof(argv[i + 1]));
            cout << "Hop latency set to " << timeAsUs(hop_latency) << endl;
            i++;
        } else if (!strcmp(argv[i], "-pcie")) {
            UecSink::_model_pcie = true;
            pcie_rate            = atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-switch_latency")) {
            switch_latency = timeFromUs(atof(argv[i + 1]));
            cout << "Switch latency set to " << timeAsUs(switch_latency) << endl;
            i++;
        } else if (!strcmp(argv[i], "-ar_sticky_delta")) {
            ar_sticky_delta = atof(argv[i + 1]);
            cout << "Adaptive routing sticky delta " << ar_sticky_delta << "us" << endl;
            i++;
        } else if (!strcmp(argv[i], "-ar_granularity")) {
            if (!strcmp(argv[i + 1], "packet"))
                ar_sticky = FatTreeSwitch::PER_PACKET;
            else if (!strcmp(argv[i + 1], "flow"))
                ar_sticky = FatTreeSwitch::PER_FLOWLET;
            else {
                cout << "Expecting -ar_granularity packet|flow, found " << argv[i + 1] << endl;
                exit(1);
            }
            i++;
        } else if (!strcmp(argv[i], "-ar_method")) {
            if (!strcmp(argv[i + 1], "pause")) {
                cout << "Adaptive routing based on pause state " << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_pause;
            } else if (!strcmp(argv[i + 1], "queue")) {
                cout << "Adaptive routing based on queue size " << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_queuesize;
            } else if (!strcmp(argv[i + 1], "bandwidth")) {
                cout << "Adaptive routing based on bandwidth utilization " << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_bandwidth;
            } else if (!strcmp(argv[i + 1], "pqb")) {
                cout << "Adaptive routing based on pause, queuesize and bandwidth utilization "
                     << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_pqb;
            } else if (!strcmp(argv[i + 1], "pq")) {
                cout << "Adaptive routing based on pause, queuesize" << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_pq;
            } else if (!strcmp(argv[i + 1], "pb")) {
                cout << "Adaptive routing based on pause, bandwidth utilization" << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_pb;
            } else if (!strcmp(argv[i + 1], "qb")) {
                cout << "Adaptive routing based on queuesize, bandwidth utilization" << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_qb;
            } else {
                cout
                    << "Unknown AR method expecting one of pause, queue, bandwidth, pqb, pq, pb, qb"
                    << endl;
                exit(1);
            }
            i++;
        } else if (!strcmp(argv[i], "-strat")) {
            if (!strcmp(argv[i + 1], "ecmp_host")) {
                route_strategy = ECMP_FIB;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
            } else if (!strcmp(argv[i + 1], "rr_ecmp")) {
                // this is the host route strategy;
                route_strategy = ECMP_FIB_ECN;
                qt             = COMPOSITE_ECN_LB;
                // this is the switch route strategy.
                FatTreeSwitch::set_strategy(FatTreeSwitch::RR_ECMP);
            } else if (!strcmp(argv[i + 1], "ecmp_host_ecn")) {
                route_strategy = ECMP_FIB_ECN;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
                qt = COMPOSITE_ECN_LB;
            } else if (!strcmp(argv[i + 1], "reactive_ecn")) {
                // Jitu's suggestion for something really simple
                // One path at a time, but switch whenever we get a trim or ecn
                // this is the host route strategy;
                route_strategy = REACTIVE_ECN;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
                qt = COMPOSITE_ECN_LB;
            } else if (!strcmp(argv[i + 1], "ecmp_ar")) {
                route_strategy    = ECMP_FIB;
                path_entropy_size = 1;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ADAPTIVE_ROUTING);
            } else if (!strcmp(argv[i + 1], "ecmp_host_ar")) {
                route_strategy = ECMP_FIB;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP_ADAPTIVE);
                // the stuff below obsolete
                // FatTreeSwitch::set_ar_fraction(atoi(argv[i+2]));
                // cout << "AR fraction: " << atoi(argv[i+2]) << endl;
                // i++;
            } else if (!strcmp(argv[i + 1], "ecmp_rr")) {
                // switch round robin
                route_strategy    = ECMP_FIB;
                path_entropy_size = 1;
                FatTreeSwitch::set_strategy(FatTreeSwitch::RR);
            }
            i++;
        } else {
            cout << "Unknown parameter " << argv[i] << endl;
            exit_error(argv[0]);
        }
        i++;
    }

    if (!param_queuesize_set || !param_ecn_set) {
        cout << "queuesizes and ecn threshold should be input from the parameters, otherwise, "
                "queuesize = BDP of 100Gbps and 12us RTT and ecn_low is 20\% of queuesize and 80\% "
                "of queuesize."
             << endl;
        // abort(); We should restore to default values here, not abort
    }
    if (!data_collection_dir.empty()) {
        DataCollector::setDataDir(data_collection_dir);
        cout << "Data collection dir set as " << data_collection_dir << endl;
    }

    printf("Using Load balancing scheme num %d\n",
           UecSrc::_load_balancing_algo);
    assert(trimsize >= 64 && trimsize <= (uint32_t)packet_size);

    srand(seed);
    srandom(seed);
    cout << "Parsed args\n";
    Packet::set_packet_size(packet_size);

    if (route_strategy == NOT_SET) {
        route_strategy = ECMP_FIB;
        FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
    }

    queuesize = memFromPkt(queuesize);

    if (ecn) {
        ecn_low  = memFromPkt(ecn_low);
        ecn_high = memFromPkt(ecn_high);
        cout << "Setting ECN for queues with size " << queuesize << ", with parameters low "
             << ecn_low << " high " << ecn_high << " enable on tor downlink " << !receiver_driven
             << endl;
        FatTreeTopology::set_ecn_parameters(true, !receiver_driven, ecn_low, ecn_high);
    }

    if (enable_qa_gate) {
        UecSrc::_enable_qa_gate = true;
        cout << "enable quick adapt gate" << endl;
    }

    // if(disable_fair_decrease){
    //     UecSrc::disableFairDecrease();
    // }

    /*
    UecSink::_oversubscribed_congestion_control = oversubscribed_congestion_control;
    */

    FatTreeSwitch::_ar_sticky               = ar_sticky;
    FatTreeSwitch::_sticky_delta            = timeFromUs(ar_sticky_delta);
    FatTreeSwitch::_ecn_threshold_fraction  = ecn_thresh;
    FatTreeSwitch::_disable_trim            = disable_trim;
    FatTreeSwitch::_low_priority_trim       = low_priority_trim;
    FatTreeSwitch::_no_droping_low_header   = no_droping_low_header;
    FatTreeSwitch::_switch_random_drop_prob = switch_random_drop_prob;
    FatTreeSwitch::_trim_size               = trimsize;

    eventlist.setEndtime(timeFromMs((double)end_time));

    // 2 priority queues; 3 hops for incast
    

    switch (route_strategy) {
        case ECMP_FIB_ECN:
        case REACTIVE_ECN:
            if (qt != COMPOSITE_ECN_LB) {
                fprintf(stderr, "Route Strategy is ECMP ECN.  Must use an ECN queue\n");
                exit(1);
            }
            if (ecn_thresh <= 0 || ecn_thresh >= 1) {
                fprintf(stderr,
                        "Route Strategy is ECMP ECN.  ecn_thresh must be between 0 and 1\n");
                exit(1);
            }
            // no break, fall through
        case ECMP_FIB:
            if (path_entropy_size > 10000) {
                fprintf(stderr, "Route Strategy is ECMP.  Must specify path count using -paths\n");
                exit(1);
            }
            break;
        case NOT_SET:
            fprintf(stderr,
                    "Route Strategy not set.  Use the -strat param.  \nValid values are perm, "
                    "rand, pull, rg and single\n");
            exit(1);
        default:
            break;
    }

    // prepare the loggers

    cout << "Logging to " << filename.str() << endl;
    fs::path path(filename.str());
    try {
        if (!fs::create_directories(path.parent_path())) {
            std::cout << "Logfile directory already exist or could not be created." << std::endl;
        }
    } catch (const fs::filesystem_error& e) {
        // std::cerr << "Filesystem error: " << e.what() << std::endl;
    }

    // Logfile
    Logfile logfile(filename.str(), eventlist);

    cout << "Linkspeed set to " << linkspeed / 1000000000 << "Gbps" << endl;
    logfile.setStartTime(timeFromSec(0));

    UecSinkLoggerSampling* sink_logger = NULL;
    if (log_sink) {
        sink_logger = new UecSinkLoggerSampling(logtime, eventlist);
        logfile.addLogger(*sink_logger);
    }
    TrafficLoggerSimple* traffic_logger = NULL;
    if (log_traffic) {
        traffic_logger = new TrafficLoggerSimple();
        logfile.addLogger(*traffic_logger);
    }
    FlowEventLoggerSimple* event_logger = NULL;
    if (log_flow_events) {
        event_logger = new FlowEventLoggerSimple();
        logfile.addLogger(*event_logger);
    }

    // UecSrc::setMinRTO(50000); //increase RTO to avoid spurious retransmits
    UecSrc::_path_entropy_size = path_entropy_size;

    UecSrc*  uec_src;
    UecSink* uec_snk;

    // Route* routeout, *routein;

    // scanner interval must be less than min RTO
    // UecRtxTimerScanner UecRtxScanner(timeFromUs((uint32_t)9), eventlist);

    QueueLoggerFactory* qlf = 0;
    if (log_tor_downqueue || log_tor_upqueue) {
        qlf = new QueueLoggerFactory(&logfile, QueueLoggerFactory::LOGGER_SAMPLING, eventlist);
        qlf->set_sample_period(timeFromUs(10.0));
    } else if (log_queue_usage) {
        qlf = new QueueLoggerFactory(&logfile, QueueLoggerFactory::LOGGER_EMPTY, eventlist);
        qlf->set_sample_period(timeFromUs(10.0));
    }

    ConnectionMatrix* conns = new ConnectionMatrix(no_of_nodes);


    // LGS Stuff
    LogSimInterface *lgs = NULL;

    if (goal_filename.size() == 0) {
        if (tm_file) {
            cout << "Loading connection matrix from  " << tm_file << endl;

            if (!conns->load(tm_file)) {
                cout << "Failed to load connection matrix " << tm_file << endl;
                exit(-1);
            }
        } else {
            cout << "Loading connection matrix from  standard input" << endl;
            conns->load(cin);
        }
    }
    

    if (conns->N != no_of_nodes && no_of_nodes != 0) {
        cout << "Connection matrix number of nodes is " << conns->N << " while I am using "
             << no_of_nodes << endl;
        //exit(-1);
    }

    no_of_nodes = conns->N;

    simtime_picosec network_max_unloaded_rtt = 0;
    // Register Metrics
    CsvMetric* _global_metric = DataCollector::RegisterCsvMetric("globalInfo",
                                                                 {"linkSpeedGbps",
                                                                  "linkDelayNs",
                                                                  "packetSizeBytes",
                                                                  "sackThresholdBytes",
                                                                  "queueSizeBytes",
                                                                  "kMinBytes",
                                                                  "kMaxBytes",
                                                                  "loadBalancingAlgo",
                                                                  "asymmetricNumberOfSlowSwitches",
                                                                  "asymmetricTopoSlowSwitchSpeed"});

    vector<FatTreeTopology*> topo;
    topo.resize(planes);
    for (uint32_t p = 0; p < planes; p++) {
        if (topo_file) {
            topo[p] = FatTreeTopology::load(topo_file, qlf, eventlist, queuesize, qt, snd_type);
            tiers_latency = topo[p]->get_tiers_latency();
            if (topo[p]->no_of_nodes() != no_of_nodes) {
                /* cerr << "Mismatch between connection matrix (" << no_of_nodes
                     << " nodes) and topology (" << topo[p]->no_of_nodes() << " nodes)" << endl; */
                //exit(1);
            }
        } else {
            FatTreeTopology::set_tiers(tiers);
            topo[p]       = new FatTreeTopology(no_of_nodes,
                                          linkspeed,
                                          queuesize,
                                          qlf,
                                          &eventlist,
                                          qt,
                                          hop_latency,
                                          switch_latency,
                                          snd_type);
            tiers_latency = topo[p]->get_tiers_latency();
        }

        if (topo[p]->get_oversubscription_ratio() > 1 && !UecSrc::_sender_based_cc &&
            !force_disable_oversubscribed_cc) {
            UecSink::_oversubscribed_cc = true;
            OversubscribedCC::setOversubscriptionRatio(topo[p]->get_oversubscription_ratio());
            cout << "Using simple receiver oversubscribed CC. Oversubscription ratio is "
                 << topo[p]->get_oversubscription_ratio() << endl;
        }

        if (log_switches) {
            topo[p]->add_switch_loggers(logfile, timeFromUs(20.0));
        }

        if (p == 0) {
            network_max_unloaded_rtt = 2 * topo[p]->get_diameter_latency() +
                                       (Packet::data_packet_size() * 8 / speedAsGbps(linkspeed) *
                                        topo[p]->get_diameter() * 1000) +
                                       (UecBasePacket::get_ack_size() * 8 / speedAsGbps(linkspeed) *
                                        topo[p]->get_diameter() * 1000);

            cout << "topo " << p << " diameter " << topo[p]->get_diameter() << " latency "
                 << timeAsUs(topo[p]->get_diameter_latency()) << " us " << endl;
        } else {
            // We only allow identical network rtts for now
            assert(network_max_unloaded_rtt == topo[p]->get_diameter_latency());
        }
    }
    cout << "network_max_unloaded_rtt " << timeAsUs(network_max_unloaded_rtt) << endl;


    if (rto_us > 0) {
        UecSrc::_min_rto = timeFromUs(rto_us);
    } else {
        UecSrc::_min_rto = timeFromUs((timeAsUs(network_max_unloaded_rtt) + queuesize * 6.0 * 8 * 1000000 / linkspeed) * rto_ratio);
    }

    cout << "Setting queuesize to " << queuesize << endl;
    cout << "Setting min RTO to " << timeAsUs(UecSrc::_min_rto) << endl;

    if (UecSink::_oversubscribed_cc)
        OversubscribedCC::_base_rtt = network_max_unloaded_rtt;

    // handle link failures specified in the connection matrix.
    for (size_t c = 0; c < conns->failures.size(); c++) {
        failure* crt = conns->failures.at(c);

        cout << "Adding link failure switch type" << crt->switch_type << " Switch ID "
             << crt->switch_id << " link ID " << crt->link_id << endl;
        // xxx we only support failures in plane 0 for now.
        topo[0]->add_failed_link(crt->switch_type, crt->switch_id, crt->link_id);
    }

    // Initialize congestion control algorithms
    if (receiver_driven) {
        // TBD
    }
    if (sender_driven) {
        // UecSrc::parameterScaleToTargetQ();
        UecSrc::initNsccParams(network_max_unloaded_rtt, linkspeed, target_Qdelay);
    }

    vector<UecPullPacer*>     pacers;
    vector<PCIeModel*>        pcie_models;
    vector<OversubscribedCC*> oversubscribed_ccs;

    vector<UecNIC*> nics;

    for (size_t ix = 0; ix < no_of_nodes; ix++) {
        pacers.push_back(new UecPullPacer(linkspeed,
                                          0.99,
                                          UecBasePacket::unquantize(UecSink::_credit_per_pull),
                                          eventlist,
                                          ports));

        if (UecSink::_model_pcie)
            pcie_models.push_back(
                new PCIeModel(linkspeed * pcie_rate, UecSrc::_mtu, eventlist, pacers[ix]));

        if (UecSink::_oversubscribed_cc)
            oversubscribed_ccs.push_back(new OversubscribedCC(eventlist, pacers[ix]));

        UecNIC* nic = new UecNIC(ix, eventlist, linkspeed, ports);
        nics.push_back(nic);
    }

    // used just to print out stats data at the end
    list<const Route*> routes;

    vector<connection*>* all_conns = conns->getAllConnections();
    vector<UecSrc*>      uec_srcs;
    vector<UecSink*>     uec_snks;

    map<flowid_t, TriggerTarget*> flowmap;
    if (planes != 1) {
        cout << "We are taking the plane 0 to calculate the network rtt; If all the planes have "
                "the same tiers, you can remove this check."
             << endl;
        assert(false);
    }

    mem_b  cwnd_b = cwnd * Packet::data_packet_size();
    string slow_switch_speed;
    string asymmetric_number_of_slow_switches;
    if (topo[0]->asymmetric_mode(1) == FatTreeTopology::SYM) {
        slow_switch_speed                  = to_string(speedAsGbps(linkspeed));
        asymmetric_number_of_slow_switches = "0";
    } else {
        slow_switch_speed = to_string(speedAsGbps(topo[0]->asymmetric_downlink_speed(1)));
        asymmetric_number_of_slow_switches = to_string(topo[0]->asymmetric_number_of_switches());
    }
    _global_metric->LogData({std::to_string(speedAsGbps(linkspeed)),
                             tiers_latency,
                             std::to_string(Packet::data_packet_size()),
                             std::to_string(UecSink::_bytes_unacked_threshold),
                             std::to_string(queuesize),
                             std::to_string(ecn_low),
                             std::to_string(ecn_high),
                             std::to_string(UecSrc::_load_balancing_algo),
                             asymmetric_number_of_slow_switches,
                             slow_switch_speed});



    // Create the connections LGS
    

    for (int iteration = 0; iteration < number_iterations; iteration++) {
        printf("Iteration Number %d\n", iteration);
        srand(seed);
        srandom(seed);
        seed++;
        if (goal_filename.size() > 0) {
            fflush(stdout);

            AtlahsHtsimApi *api = new AtlahsHtsimApi();
            api->setTopology(topo[0]);
            api->cwnd_b = cwnd_b;
            api->setEventList(&eventlist);
            api->setComputeEvent(new ComputeEvent(eventlist));
            api->finish_after = force_finish;
            api->setNullEvent(new NullEvent(eventlist));
            lgs = new LogSimInterface(NULL, traffic_logger, eventlist, topo[0], nullptr);
            lgs->htsim_api = api;
            api->setLogSimInterface(lgs);
            lgs->set_protocol(UEC_PROTOCOL);
            lgs->htsim_api->linkspeed = linkspeed;
            lgs->local_compute_time_override_ns = lgs_compute_time_override_ns;

            

            double linkSpeedBytesPerSec = (linkspeed/1000000000 * 1e9) / 8.0;

            // Calculate G in cycles
            lgs->htsim_api->htsim_G  = 1e9 / linkSpeedBytesPerSec;

            printf("<HTSIM> G %f\n", lgs->htsim_api->htsim_G);
            if (lgs_compute_time_override_ns >= 0) {
                printf("<HTSIM> overriding local compute ops to %ld ns\n", lgs_compute_time_override_ns);
            }

            lgs->htsim_api->total_nodes = no_of_nodes;
            lgs->htsim_api->Setup();
            printf("Started LGS\n");
            
            start_lgs(goal_filename, *lgs);
            printf("Iteration Terminated\n");
        }
    }

    if (goal_filename.size() > 0) {
        printf("Finished all\n");
        fflush(stdout);
        return 0;
    }

    for (size_t c = 0; c < all_conns->size(); c++) {
        connection* crt  = all_conns->at(c);
        int         src  = crt->src;
        int         dest = crt->dst;
        assert(planes > 0);
        simtime_picosec transmission_delay =
            (Packet::data_packet_size() * 8 / speedAsGbps(linkspeed) * topo[0]->get_diameter() *
             1000) +
            (UecBasePacket::get_ack_size() * 8 / speedAsGbps(linkspeed) * topo[0]->get_diameter() *
             1000);
        simtime_picosec base_rtt_bw_two_points =
            2 * topo[0]->get_two_point_diameter_latency(src, dest) + transmission_delay;

        // cout << "Connection " << crt->src << "->" <<crt->dst << " starting at " << crt->start <<
        // " size " << crt->size << endl;

        uec_src = new UecSrc(traffic_logger, eventlist, *nics.at(src), ports);

        if (percentage_bg > 0 && (rand() % 100) < percentage_bg) {
            uec_src->is_ecmp_bg = true;                 // ~percentage_bg% on average
        }

        // If cwnd is 0 initXXcc will set a sensible default value
        if (receiver_driven) {
            // uec_src->setCwnd(cwnd*Packet::data_packet_size());
            // uec_src->setMaxWnd(cwnd*Packet::data_packet_size());

            if (enable_accurate_base_rtt) {
                uec_src->initRccc(cwnd_b, base_rtt_bw_two_points);
            } else {
                uec_src->initRccc(cwnd_b, network_max_unloaded_rtt);
            }
        }
        uec_src->setName(fmt::format("Uec_{}_{}", src, dest));
        if (sender_driven) {
            if (enable_accurate_base_rtt) {
                uec_src->initNscc(cwnd_b, base_rtt_bw_two_points);
            } else {
                uec_src->initNscc(cwnd_b, network_max_unloaded_rtt);
            }
        }
        uec_srcs.push_back(uec_src);
        uec_src->setSrc(src);
        uec_src->setDst(dest);
        if (src < UecSrc::ecmp_background_traffic_nodes) {
            uec_src->resetLBToECMP();
        }
        if (log_flow_events) {
            uec_src->logFlowEvents(*event_logger);
        }

        if (receiver_driven)
            uec_snk = new UecSink(NULL, pacers[dest], *nics.at(dest), ports);
        else  // each connection has its own pacer, so receiver driven mode does not kick in!
            uec_snk = new UecSink(NULL,
                                  linkspeed,
                                  1.1,
                                  UecBasePacket::unquantize(UecSink::_credit_per_pull),
                                  eventlist,
                                  *nics.at(dest),
                                  ports);
        uec_snks.push_back(uec_snk);
        
        logfile.writeName(*uec_src);
        uec_snk->setSrc(src);
        if (src < UecSrc::ecmp_background_traffic_nodes) {
            uec_snk->resetLBToECMP();
        }

        if (UecSink::_model_pcie) {
            uec_snk->setPCIeModel(pcie_models[dest]);
        }

        if (UecSink::_oversubscribed_cc) {
            uec_snk->setOversubscribedCC(oversubscribed_ccs[dest]);
        }

        ((DataReceiver*)uec_snk)->setName(fmt::format("Uec_sink_{}_{}", src, dest));
        logfile.writeName(*(DataReceiver*)uec_snk);

        if (crt->flowid) {
            uec_src->setFlowId(crt->flowid);
            uec_snk->setFlowId(crt->flowid);
            printf("Setting flow id %lu for connection %d -> %d\n",
                   crt->flowid,
                   crt->src,
                   crt->dst);
            assert(flowmap.find(crt->flowid) == flowmap.end());  // don't have dups
            flowmap[crt->flowid] = uec_src;
        }

        if (crt->size > 0) {
            uec_src->setFlowsize(crt->size);
        }

        if (crt->trigger) {
            Trigger* trig = conns->getTrigger(crt->trigger, eventlist);
            trig->add_target(*uec_src);
        }
        if (crt->send_done_trigger) {
            Trigger* trig = conns->getTrigger(crt->send_done_trigger, eventlist);
            uec_src->setEndTrigger(*trig);
        }

        if (crt->recv_done_trigger) {
            Trigger* trig = conns->getTrigger(crt->recv_done_trigger, eventlist);
            uec_snk->setEndTrigger(*trig);
        }

        // uec_snk->set_priority(crt->priority);

        // UecRtxScanner.registerUec(*UecSrc);
        for (uint32_t p = 0; p < planes; p++) {
            switch (route_strategy) {
                case ECMP_FIB:
                case ECMP_FIB_ECN:
                case REACTIVE_ECN: {
                    Route* srctotor = new Route();
                    srctotor->push_back(
                        topo[p]->queues_ns_nlp[src][topo[p]->HOST_POD_SWITCH(src)][0]);
                    srctotor->push_back(
                        topo[p]->pipes_ns_nlp[src][topo[p]->HOST_POD_SWITCH(src)][0]);
                    srctotor->push_back(topo[p]
                                            ->queues_ns_nlp[src][topo[p]->HOST_POD_SWITCH(src)][0]
                                            ->getRemoteEndpoint());

                    Route* dsttotor = new Route();
                    dsttotor->push_back(
                        topo[p]->queues_ns_nlp[dest][topo[p]->HOST_POD_SWITCH(dest)][0]);
                    dsttotor->push_back(
                        topo[p]->pipes_ns_nlp[dest][topo[p]->HOST_POD_SWITCH(dest)][0]);
                    dsttotor->push_back(topo[p]
                                            ->queues_ns_nlp[dest][topo[p]->HOST_POD_SWITCH(dest)][0]
                                            ->getRemoteEndpoint());

                    uec_src->connectPort(p, *srctotor, *dsttotor, *uec_snk, crt->start);
                    // uec_src->setPaths(path_entropy_size);
                    // uec_snk->setPaths(path_entropy_size);

                    // register src and snk to receive packets from their respective TORs.
                    assert(topo[p]->switches_lp[topo[p]->HOST_POD_SWITCH(src)]);
                    assert(topo[p]->switches_lp[topo[p]->HOST_POD_SWITCH(src)]);
                    topo[p]->switches_lp[topo[p]->HOST_POD_SWITCH(src)]->addHostPort(
                        src, uec_snk->flowId(), uec_src->getPort(p));
                    topo[p]->switches_lp[topo[p]->HOST_POD_SWITCH(dest)]->addHostPort(
                        dest, uec_src->flowId(), uec_snk->getPort(p));
                    break;
                }
                default:
                    abort();
            }
        }

        // set up the triggers
        // xxx

        if (log_sink) {
            sink_logger->monitorSink(uec_snk);
        }
    }

    Logged::dump_idmap();
    // Record the setup
    int pktsize = Packet::data_packet_size();
    logfile.write(fmt::format("# pktsize={} bytes", pktsize));
    logfile.write(fmt::format("# hostnicrate={} Mbps", linkspeed / 1000000));
    // logfile.write("# corelinkrate = " + ntoa(HOST_NIC*CORE_TO_HOST) + " pkt/sec");
    // logfile.write("# buffer = " + ntoa((double) (queues_na_ni[0][1]->_maxsize) / ((double)
    // pktsize)) + " pkt");
    cout << "conns: " << no_of_conns << ", end: " << end_time << ", no_of_nodes: " << no_of_nodes
         << ", tiers: " << tiers << ", planes:" << planes << ", ports:" << ports
         << ", UecSrc::_target_Qdelay:" << UecSrc::_target_Qdelay
         << ", _sender_cc_algo:" << UecSrc::_sender_cc_algo
         << ", _load_balancing_algo:" << UecSrc::_load_balancing_algo << ", queue type:" << qt
         << ", queue_prio:" << snd_type << ", cwnd:" << cwnd << ", tm_file:" << tm_file
         << ", topo_file:" << topo_file << ", queuesize:" << queuesize
         << ", _bytes_unacked_threshold:" << UecSink::_bytes_unacked_threshold
         << ", _oversubscribed_cc:" << UecSink::_oversubscribed_cc
         << ", force_disable_oversubscribed_cc:" << force_disable_oversubscribed_cc
         << ", enable_accurate_base_rtt:" << enable_accurate_base_rtt
         << ", _enable_sleek:" << UecSrc::_enable_sleek << ", ecn:" << ecn
         << ", ecn_low:" << ecn_low << ", ecn_high:" << ecn_high
         << ", disable_trim:" << disable_trim << ", trimsize:" << trimsize
         << ", linkspeed:" << linkspeed << ", packet_size:" << packet_size << ", seed:" << seed
         << ", path_entropy_size:" << path_entropy_size << ", path_burst:" << path_burst
         << ", hop_latency:" << hop_latency << ", pcie_rate:" << pcie_rate
         << ", switch_latency:" << switch_latency << ", ar_sticky_delta:" << ar_sticky_delta
         << ", route_strategy:" << route_strategy << "\n";
    // GO!
    cout << "Starting simulation" << endl;
    while (eventlist.doNextEvent()) {
    }

    cout << "Done" << endl;
    int new_pkts = 0, rtx_pkts = 0, bounce_pkts = 0, rts_pkts = 0, ack_pkts = 0, nack_pkts = 0,
        pull_pkts = 0, sleek_pkts = 0;
    for (size_t ix = 0; ix < uec_srcs.size(); ix++) {
        const struct UecSrc::Stats& s = uec_srcs[ix]->stats();
        new_pkts += s.new_pkts_sent;
        rtx_pkts += s.rtx_pkts_sent;
        rts_pkts += s.rts_pkts_sent;
        bounce_pkts += s.bounces_received;
        ack_pkts += s.acks_received;
        nack_pkts += s.nacks_received;
        pull_pkts += s.pulls_received;
        sleek_pkts += s._sleek_counter;

        uec_snks[ix]->logMetricSink();
    }
    cout << "New: " << new_pkts << " Rtx: " << rtx_pkts << " RTS: " << rts_pkts
         << " Bounced: " << bounce_pkts << " ACKs: " << ack_pkts << " NACKs: " << nack_pkts
         << " Pulls: " << pull_pkts << " sleek_pkts: " << sleek_pkts << endl;
    /*
    list <const Route*>::iterator rt_i;
    int counts[10]; int hop;
    for (int i = 0; i < 10; i++)
        counts[i] = 0;
    cout << "route count: " << routes.size() << endl;
    for (rt_i = routes.begin(); rt_i != routes.end(); rt_i++) {
        const Route* r = (*rt_i);
        //print_route(*r);
#ifdef PRINTPATHS
        cout << "Path:" << endl;
#endif
        hop = 0;
        for (int i = 0; i < r->size(); i++) {
            PacketSink *ps = r->at(i);
            CompositeQueue *q = dynamic_cast<CompositeQueue*>(ps);
            if (q == 0) {
#ifdef PRINTPATHS
                cout << ps->nodename() << endl;
#endif
            } else {
#ifdef PRINTPATHS
                cout << q->nodename() << " " << q->num_packets() << "pkts "
                     << q->num_headers() << "hdrs " << q->num_acks() << "acks " << q->num_nacks() <<
"nacks " << q->num_stripped() << "stripped"
                     << endl;
#endif
                counts[hop] += q->num_stripped();
                hop++;
            }
        }
#ifdef PRINTPATHS
        cout << endl;
#endif
    }
    for (int i = 0; i < 10; i++)
        cout << "Hop " << i << " Count " << counts[i] << endl;
    */
}
