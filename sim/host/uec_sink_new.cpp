#include "uec_sink.h"

#include <cmath>

#include "data_collector.h"
#include "uec_config.h"
#include "uec_nic.h"
#include "uec_packet.h"
#include "uec_pull_pacer.h"
#include "uec_sink_port.h"
#include "uec_src.h"

mem_b UecSink::_bytes_unacked_threshold = 16384;
int   UecSink::TGT_EV_SIZE              = 7;
bool  UecSink::_model_pcie              = false;

// send 4 packets of credit per pull, as per default in UEC spec
uint16_t UecSink::_mtus_per_pull = 4;

// units of UEC_PULL_QUANTA bytes (typically 256) - note round down to mss rather than mtu
UecBasePacket::pull_quanta UecSink::_credit_per_pull =
    (UecSrc::_mss * UecSink::_mtus_per_pull) >> UEC_PULL_SHIFT;

bool UecSink::_oversubscribed_cc =
    false;  // can only be enabled when receiver_based_cc is set to true

////////////////////////////////////////////////////////////////
//  UEC SINK
////////////////////////////////////////////////////////////////
// bool UecSink::_pflr_print_debug_msg = false;
int32_t UecSink::_pflr_scheme_id = -1;

// bool UecSink::_pflr_disable_probe = false;
// bool UecSink::_pflr_disable_nack = false;

UecSink::UecSink(TrafficLogger* trafficLogger,
                 UecPullPacer*  pullPacer,
                 UecNIC&        nic,
                 uint32_t       no_of_ports)
    : DataReceiver("uecSink"),
      _nic(nic),
      _flow(trafficLogger),
      _pullPacer(pullPacer),
      _expected_epsn(0),
      _high_epsn(0),
      _retx_backlog(0),
      _latest_pull(INIT_PULL),
      _highest_pull_target(INIT_PULL),
      _received_bytes(0),
      _accepted_bytes(0),
      _recvd_bytes(0),
      _rcv_cwnd_pen(255),
      _end_trigger(NULL),
      _epsn_rx_bitmap(0),
      _out_of_order_count(0),
      _ack_request(false),
      _entropy(0) {
    _nodename    = "uecSink";  // TBD: would be nice at add nodenum to nodename
    _no_of_ports = no_of_ports;
    _ports.resize(no_of_ports);
    for (uint32_t p = 0; p < _no_of_ports; p++) {
        _ports[p] = new UecSinkPort(*this, p);
    }


    dropped_map[5563] = 41;
    dropped_map[5822] = 239;
    dropped_map[1161] = 232;
    dropped_map[3824] = 173;
    dropped_map[4210] = 192;
    dropped_map[942] = 149;
    dropped_map[1576] = 216;
    dropped_map[5257] = 9;
    dropped_map[1861] = 159;
    dropped_map[7783] = 56;


    _rss_last_subflow_acked_seq_no.resize(UecSrc::_rss_params._rss_number_of_subflows);
    _rss_last_subflow_acked_entropy.resize(UecSrc::_rss_params._rss_number_of_subflows);
    for (uint32_t subflow = 0; subflow < UecSrc::_rss_params._rss_number_of_subflows; subflow++) {
        _rss_last_subflow_acked_seq_no[subflow]  = 0;
        _rss_last_subflow_acked_entropy[subflow] = 0;
    }
    // loss detection
    if (UecSrc::usePflr() && !backgroundECMPFlow) {
        if (_pflr_scheme_id == 0) {
            // do nothing
        } else if (_pflr_scheme_id == 1) {
            auto _no_slots = UecSrc::getNoSlots();
            _pflr1_slots_expect_psn.resize(_no_slots);
            for (int i = 0; i < _no_slots; i++) {
                _pflr1_slots_expect_psn[i] = i;
            }
        } else if (_pflr_scheme_id == 2) {
            auto _no_slots = UecSrc::getNoSlots();
            _pflr2_slots_start_psn.resize(_no_slots);
            _pflr2_slots_expect_psn.resize(_no_slots);
            _pflr2_slots_generation.resize(_no_slots);
            for (int i = 0; i < _no_slots; i++) {
                _pflr2_slots_start_psn[i].push_back(i);
                _pflr2_slots_expect_psn[i].push_back(i);
                _pflr2_slots_generation[i].push_back(0);
            }
        } else if (_pflr_scheme_id == 3) {
            // do nothing
        } else {
            cout << "not supported pfld scheme" << endl;
            abort();
        }
    }

    _stats        = {0, 0, 0, 0, 0, 0, 0, 0};
    _in_pull      = false;
    _in_slow_pull = false;

    _pcie        = NULL;
    _receiver_cc = NULL;
}

UecSink::UecSink(TrafficLogger* trafficLogger,
                 linkspeed_bps  linkSpeed,
                 double         rate_modifier,
                 uint16_t       mtu,
                 EventList&     eventList,
                 UecNIC&        nic,
                 uint32_t       no_of_ports)
    : DataReceiver("uecSink"),
      _nic(nic),
      _flow(trafficLogger),
      _expected_epsn(0),
      _high_epsn(0),
      _retx_backlog(0),
      _latest_pull(INIT_PULL),
      _highest_pull_target(INIT_PULL),
      _received_bytes(0),
      _accepted_bytes(0),
      _recvd_bytes(0),
      _rcv_cwnd_pen(255),
      _end_trigger(NULL),
      _epsn_rx_bitmap(0),
      _out_of_order_count(0),
      _ack_request(false),
      _entropy(0) {
    if (UecSrc::_receiver_based_cc)
        _pullPacer = new UecPullPacer(linkSpeed, rate_modifier, mtu, eventList, no_of_ports);
    else
        _pullPacer = NULL;

    _no_of_ports = no_of_ports;
    _ports.resize(no_of_ports);
    for (uint32_t p = 0; p < _no_of_ports; p++) {
        _ports[p] = new UecSinkPort(*this, p);
    }
    
    //dropped_map[5563] = 41;
    dropped_map[5822] = 239;
    //dropped_map[1161] = 232;
    //dropped_map[3824] = 173;
    //dropped_map[4210] = 192;
    //dropped_map[942] = 149;
    //dropped_map[1576] = 216;
    //dropped_map[5257] = 9;
    //dropped_map[1861] = 159;
    //dropped_map[7783] = 56;
    
    _rss_last_subflow_acked_seq_no.resize(UecSrc::_rss_params._rss_number_of_subflows);
    _rss_last_subflow_acked_entropy.resize(UecSrc::_rss_params._rss_number_of_subflows);
    for (uint32_t subflow = 0; subflow < UecSrc::_rss_params._rss_number_of_subflows; subflow++) {
        _rss_last_subflow_acked_seq_no[subflow]  = 0;
        _rss_last_subflow_acked_entropy[subflow] = 0;
    }
    // loss detection
    if (UecSrc::usePflr() && !backgroundECMPFlow) {
        if (_pflr_scheme_id == 0) {
            // do nothing
        } else if (_pflr_scheme_id == 1) {
            auto _no_slots = UecSrc::getNoSlots();
            _pflr1_slots_expect_psn.resize(_no_slots);
            for (int i = 0; i < _no_slots; i++) {
                _pflr1_slots_expect_psn[i] = i;
            }
        } else if (_pflr_scheme_id == 2) {
            auto _no_slots = UecSrc::getNoSlots();
            _pflr2_slots_start_psn.resize(_no_slots);
            _pflr2_slots_expect_psn.resize(_no_slots);
            _pflr2_slots_generation.resize(_no_slots);
            for (int i = 0; i < _no_slots; i++) {
                _pflr2_slots_start_psn[i].push_back(i);
                _pflr2_slots_expect_psn[i].push_back(i);
                _pflr2_slots_generation[i].push_back(0);
            }
        } else if (_pflr_scheme_id == 3) {
            // do nothing
        } else {
            cout << "not supported pfld scheme" << endl;
            abort();
        }
    }

    _stats        = {0, 0, 0, 0, 0, 0, 0};
    _in_pull      = false;
    _in_slow_pull = false;

    _pcie        = NULL;
    _receiver_cc = NULL;
}

void UecSink::resetLBToECMP() {
    backgroundECMPFlow = true;
}

// Register all the metrics that are gonna be collected for this object.
void UecSink::registerMetrics() {
    _sink_stats = DataCollector::RegisterCsvMetric("sinkStats",
                                                   {"srcNode_dstNode_flowId",
                                                    "received",
                                                    "bytes_received",
                                                    "duplicates",
                                                    "out_of_order",
                                                    "trimmed",
                                                    "pulls",
                                                    "rts",
                                                    "ecn_received",
                                                    "ecn_bytes_received"});
}

// At the end of the experiment, log sink metrics
void UecSink::logMetricSink() {
    _sink_stats->LogData({_src->getSrcDstFlowid(),
                          to_string(_stats.received),
                          to_string(_stats.bytes_received),
                          to_string(_stats.duplicates),
                          to_string(_stats.out_of_order),
                          to_string(_stats.trimmed),
                          to_string(_stats.pulls),
                          to_string(_stats.rts),
                          to_string(_stats.ecn_received),
                          to_string(_stats.ecn_bytes_received)});
}

void UecSink::connectPort(uint32_t port_num, UecSrc& src, const Route& route) {
    _src = &src;
    _ports[port_num]->setRoute(route);
    registerMetrics();
}

void UecSink::handlePullTarget(UecBasePacket::seq_t pt) {
    if (!UecSrc::_receiver_based_cc)
        return;

    if (_src->debug())
        cout << " UecSink " << _nodename << " src " << _src->nodename() << " handlePullTarget pt "
             << pt << " highest_pt " << _highest_pull_target << endl;
    if (_src->flow()->flow_id() == UecSrc::_debug_flowid) {
        cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
             << " handlePullTarget pt " << pt << " highest_pt " << _highest_pull_target << endl;
    }
    if (pt > _highest_pull_target) {
        if (_src->debug())
            cout << "    pull target advanced\n";
        _highest_pull_target = pt;

        if (_retx_backlog == 0 && !_in_pull) {
            if (_src->debug())
                cout << "    requesting pull\n";
            _in_pull = true;
            _pullPacer->requestPull(this);
        }
    }
}

void UecSink::pflrSendNack(UecBasePacket::seq_t seq_no, uint16_t ev) {
    // logging
    cout << timeAsUs(getSrc()->eventlist().now()) << " flow " << _src->flow()->flow_id()
         << " sending pflr nack packet for psn " << seq_no << " ev " << ev << endl;
    // send nack
    UecNackPacket* nack_packet = nack(ev, seq_no);
    _nic.sendControlPacket(nack_packet, NULL, this);
}

void UecSink::processData(UecDataPacket& pkt) {

    // if the key of dropped map is equal to the flow id and the value is equal to the psn then enter the if statement
    if (dropped_map.find(pkt.flow_id()) != dropped_map.end() &&
        dropped_map[pkt.flow_id()] == pkt.id()) {
        cout << timeAsUs(getSrc()->eventlist().now()) << " flow " << pkt.flow_id()
             << " drop data packet for psn " << pkt.id() << " epsn " << pkt.epsn() << endl;
        dropped_map.erase(pkt.flow_id());
        printf("Size now is %lu\n", dropped_map.size());
        fflush(stdout);
        return;  // drop this packet
    }

    bool force_ack = false;
    if (pkt.packet_type() == UecBasePacket::DATA_PROBE) {
        UecAckPacket* ack_packet = sack(pkt.path_id(),
                                        sackBitmapBase(pkt.epsn()),
                                        pkt.epsn(),
                                        (bool)(pkt.flags() & ECN_CE),
                                        pkt.retransmitted());
        ack_packet->set_packet_type_echo(pkt.packet_type());
        _nic.sendControlPacket(ack_packet, NULL, this);
        return;
    }
    // PCIeModel processing


    if (_model_pcie) {
        if (!_pcie->addBacklog(pkt.size())) {
            // will drop this packet!
            cout << "PCIE trim" << endl;
            // should trim this packet.
            pkt.strip_payload();
            processTrimmed(pkt);
            return;
        }
    }

    /* cout << timeAsUs(getSrc()->eventlist().now()) << " flow " << _flow.flow_id()
         << " received data packet for psn " << pkt.epsn() << " ev " << 0 << 
         " last " << timeAsUs(getSrc()->eventlist().now()-last_data_sent_time) << endl; */
    last_data_sent_time = getSrc()->eventlist().now();

    

    /* if (pkt.flow_id() == 5563) {
        cout << timeAsUs(getSrc()->eventlist().now()) << " flowid " << pkt.flow_id()
             << " recv data psn " << pkt.epsn() << " id " << pkt.id() <<  " size map " << dropped_map.size() << endl;
    } */


    // ensure we never overflow receive bitmap.
    if (pkt.epsn() > _expected_epsn + uecMaxInFlightPkts * UecSrc::_mtu) {
        abort();
    }

    if (_src->debug())
        cout << " UecSink " << _nodename << " src " << _src->nodename()
             << " processData: " << pkt.epsn() << " time " << timeAsNs(getSrc()->eventlist().now())
             << " when expected epsn is " << _expected_epsn << " size " << pkt.size()
             << " ooo count " << _out_of_order_count << " flow " << _src->flow()->str() << endl;

    _accepted_bytes += pkt.size();

    handlePullTarget(pkt.pull_target());
    if (_src->flow()->flow_id() == UecSrc::_debug_flowid) {
        cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
             << " recv " << pkt.epsn() << endl;
    }
    if (pkt.epsn() > _high_epsn) {
        // highest_received is used to bound the sack bitmap. This is a 64 bit number in simulation,
        // never wraps. In practice need to handle sequence number wrapping.
        _high_epsn = pkt.epsn();
    }

    /* if (pkt.retransmitted()) {
        cout << timeAsUs(getSrc()->eventlist().now()) << " flow " << _src->flow()->flow_id()
             << " receive rtx packet for psn " << pkt.epsn() << " ev " << pkt.path_id() << endl;
    } else {
        cout << timeAsUs(getSrc()->eventlist().now()) << " flow " << _src->flow()->flow_id()
             << " receive data packet for psn " << pkt.epsn() << " ev " << pkt.path_id() << endl;
    } */

        /* printf("Process Data %d %d\n", UecSrc::usePflr(), backgroundECMPFlow); */

    /* cout << timeAsUs(getSrc()->eventlist().now()) << " flow " << _src->flow()->flow_id()
         << " received psn " << pkt.epsn() << endl; */

    if (UecSrc::usePflr() && !backgroundECMPFlow) {
        if (_pflr_scheme_id == 0) {
            // do nothing
        } else if (_pflr_scheme_id == 1) {
            if (pkt.retransmitted()) {
                // do nothing
            } else {
                
                auto _no_slots = UecSrc::getNoSlots();
                // get info from received packet
                uint16_t pkt_ev  = pkt.path_id();  // pkt ev, indicates previous ev for probe packet
                uint32_t slot_id = pkt.epsn() % _no_slots;
                if (_pflr1_slots_expect_psn[slot_id] <= pkt.epsn()) {
                    // nack missing ones
                    for (auto intermediate_psn = _pflr1_slots_expect_psn[slot_id];
                         intermediate_psn < pkt.epsn();
                         intermediate_psn += _no_slots) {
                        // nack missing psn
                        //printf("Using PFLDR scheme 1 - Received PSN %d - Resending PSN %d - Expected PSN %d\n", pkt.epsn(), intermediate_psn, _pflr1_slots_expect_psn[slot_id]);
                        pflrSendNack(intermediate_psn, pkt_ev);
                    }
                    _pflr1_slots_expect_psn[slot_id] = pkt.epsn() + _no_slots;
                    //printf("New Expected PSN %d\n", _pflr1_slots_expect_psn[slot_id]);
                } else {
                    // do nothing
                    cout << "pflr state error" << endl;
                }
            }
        } else if (_pflr_scheme_id == 2) {
            if (pkt.retransmitted()) {
                // do nothing
            } else {
                auto _no_slots = UecSrc::getNoSlots();
                // get info from received packet
                uint16_t pkt_ev  = pkt.path_id();  // pkt ev, indicates previous ev for probe packet
                uint32_t slot_id = pkt.epsn() % _no_slots;
                // extract generation
                uint16_t n_bits_slot_id = (uint16_t)log2(_no_slots);     // #bits for slot id
                uint16_t mask       = (1 << (16 - n_bits_slot_id)) - 1;  // Create a mask for LSBs
                uint16_t generation = pkt_ev & mask;  // Apply mask to extract generation
                // if generation exist
                auto it = std::find(_pflr2_slots_generation[slot_id].begin(),
                                    _pflr2_slots_generation[slot_id].end(),
                                    generation);
                if (it != _pflr2_slots_generation[slot_id].end()) {
                    // generation exist
                    auto index = std::distance(_pflr2_slots_generation[slot_id].begin(), it);
                    // nack missing PSN
                    if (_pflr2_slots_expect_psn[slot_id][index] <= pkt.epsn()) {
                        for (auto intermediate_psn = _pflr2_slots_expect_psn[slot_id][index];
                             intermediate_psn < pkt.epsn();
                             intermediate_psn += _no_slots) {
                            // nack missing psn
                            pflrSendNack(intermediate_psn, pkt_ev);
                        }
                        _pflr2_slots_expect_psn[slot_id][index] = pkt.epsn() + _no_slots;
                    } else if (pkt.epsn() < _pflr2_slots_start_psn[slot_id][index]) {
                        for (auto intermediate_psn = pkt.epsn() + _no_slots;
                             intermediate_psn < _pflr2_slots_start_psn[slot_id][index];
                             intermediate_psn += _no_slots) {
                            // nack missing psn
                            pflrSendNack(intermediate_psn, pkt_ev);
                        }
                        _pflr2_slots_start_psn[slot_id][index] = pkt.epsn();
                    } else {
                        cout << "pflr state error" << endl;
                    }
                } else {
                    // a new generation
                    if (generation <= _pflr2_slots_generation[slot_id].back()) {
                        cout << "pflr state error" << endl;
                    }
                    _pflr2_slots_start_psn[slot_id].push_back(pkt.epsn());
                    _pflr2_slots_expect_psn[slot_id].push_back(pkt.epsn() + _no_slots);
                    _pflr2_slots_generation[slot_id].push_back(generation);
                }
                // remove resolved generation
                while (_pflr2_slots_generation[slot_id].size() > 1 &&
                       _pflr2_slots_expect_psn[slot_id][0] == _pflr2_slots_start_psn[slot_id][1]) {
                    _pflr2_slots_generation[slot_id].erase(
                        _pflr2_slots_generation[slot_id].begin());
                    _pflr2_slots_start_psn[slot_id].erase(_pflr2_slots_start_psn[slot_id].begin());
                    _pflr2_slots_expect_psn[slot_id].erase(
                        _pflr2_slots_expect_psn[slot_id].begin());
                }
            }
        } else if (_pflr_scheme_id == 3) {
            
            auto _no_slots = UecSrc::getNoSlots();
            //printf("Using PFLDR scheme 3 Flow %s - Received Packet PSN %d\n", _src->flow()->str().c_str(), pkt.epsn());


            // get info from received packet
            uint16_t pkt_ev = pkt.path_id();  // pkt ev, indicates previous ev for probe packet

            /* // extend the bitmap if needed
            if (pkt.epsn() >= static_cast<UecDataPacket::seq_t>(_pflr3_receive_bitmap.size()) +
                                  _pflr3_bitmap_start_psn) {
                _pflr3_receive_bitmap.resize(pkt.epsn() - _pflr3_bitmap_start_psn + 1, 0);
                _pflr3_nack_bitmap.resize(pkt.epsn() - _pflr3_bitmap_start_psn + 1, 0);
            }
            // set the received pkt
            _pflr3_receive_bitmap[pkt.epsn() - _pflr3_bitmap_start_psn] = 1; */

            /* ----------------------------------------------------------------
            * Make sure our bitmap access is always in‑bounds.
            *  - packets older than _pflr3_bitmap_start_psn are duplicates;
            *  - packets on or after that PSN slide the window if necessary.
            * --------------------------------------------------------------*/
            if (pkt.epsn() >= _pflr3_bitmap_start_psn) {
                size_t idx = pkt.epsn() - _pflr3_bitmap_start_psn;

                if (idx >= _pflr3_receive_bitmap.size()) {
                    _pflr3_receive_bitmap.resize(idx + 1, 0);
                    _pflr3_nack_bitmap.resize(idx + 1, 0);
                }
                _pflr3_receive_bitmap[idx] = 1;      // safe write
            } else {
                /* Packet is older than the current window → already processed.
                Log or ignore as appropriate. */
            }


            if (!pkt.retransmitted()) {
                UecDataPacket::seq_t base_psn = static_cast<UecDataPacket::seq_t>(pkt_ev);
                // check
                if ((base_psn % _no_slots) != (pkt.epsn() % _no_slots)) {
                    cout << "pflr state error" << endl;
                }
                // send nack
                UecDataPacket::seq_t first_psn = base_psn;
                while (first_psn < _pflr3_bitmap_start_psn) {
                    first_psn += _no_slots;
                }
                for (auto intermediate_psn = first_psn; intermediate_psn <= pkt.epsn();
                     intermediate_psn += _no_slots) {
                    if (_pflr3_receive_bitmap[intermediate_psn - _pflr3_bitmap_start_psn] == 1) {
                        // pkt received
                        continue;
                    } else if (_pflr3_receive_bitmap[intermediate_psn - _pflr3_bitmap_start_psn] ==
                                   0 &&
                               _pflr3_nack_bitmap[intermediate_psn - _pflr3_bitmap_start_psn] ==
                                   0) {
                        // pkt not received and not nacked
                        // nack missing psn
                        printf("Sending NACK for PSN %d\n", intermediate_psn);
                        pflrSendNack(intermediate_psn, pkt_ev);
                        _pflr3_nack_bitmap[intermediate_psn - _pflr3_bitmap_start_psn] = 1;
                    }
                }
            }
            // remove the received packet in the beginning
            while (!_pflr3_receive_bitmap.empty() && _pflr3_receive_bitmap.front() == 1) {
                _pflr3_receive_bitmap.erase(_pflr3_receive_bitmap.begin());
                _pflr3_nack_bitmap.erase(_pflr3_nack_bitmap.begin());
                _pflr3_bitmap_start_psn += 1;
            }
        } else {
            //cout << "unrecognized _pflr_scheme_id: " << _pflr_scheme_id << endl;
            //abort();
        }
    }

    // // debug msg
    // if (_pflr_print_debug_msg) {
    //     // cout << "Bitmap after update      : ";
    //     // for (uint32_t idx = 0;idx < _slots_bitmap[slot_id].size();idx++){
    //     //     cout << to_string(_slots_bitmap[slot_id][idx]) << ", ";
    //     // }
    //     // cout << endl;
    //     // cout << "Is NACK sent after update: ";
    //     // for (uint32_t idx = 0;idx < _slots_bitmap_is_nack_sent[slot_id].size();idx++){
    //     //     cout << to_string(_slots_bitmap_is_nack_sent[slot_id][idx]) << ", ";
    //     // }
    //     // cout << endl;
    //     // cout << "Section info before: " << endl;
    //     // for (uint32_t section_id = 0; section_id < _slots_bitmap_sections_ev[slot_id].size();
    //     section_id ++){
    //     //     // if section is not finished
    //     //     cout << "EV " << _slots_bitmap_sections_ev[slot_id][section_id];
    //     //     cout << " Start " << _slots_bitmap_sections_start_psn[slot_id][section_id] << "
    //     idx " << _slots_bitmap_sections_start_psn[slot_id][section_id]/ _no_slots;
    //     //     cout << " End " << _slots_bitmap_sections_end_psn[slot_id][section_id] << " idx "
    //     << _slots_bitmap_sections_end_psn[slot_id][section_id]/ _no_slots;
    //     //     cout << " zPSN " << _slots_bitmap_zpsn[slot_id][section_id] << " idx " <<
    //     _slots_bitmap_zpsn[slot_id][section_id]/ _no_slots;
    //     //     cout << endl;
    //     // }
    // }
    // if (_pflr_print_debug_msg) {
    //     cout << "Section info after update: " << endl;
    //     for (uint32_t section_id = 0; section_id < _slots_bitmap_sections_ev[slot_id].size();
    //     section_id ++){
    //         // if section is not finished
    //         cout << "EV " << _slots_bitmap_sections_ev[slot_id][section_id];
    //         cout << " Start " << _slots_bitmap_sections_start_psn[slot_id][section_id] << " idx "
    //         << _slots_bitmap_sections_start_psn[slot_id][section_id]/ _no_slots; cout << " End "
    //         << _slots_bitmap_sections_end_psn[slot_id][section_id] << " idx " <<
    //         _slots_bitmap_sections_end_psn[slot_id][section_id]/ _no_slots; cout << " zPSN " <<
    //         _slots_bitmap_zpsn[slot_id][section_id] << " idx " <<
    //         _slots_bitmap_zpsn[slot_id][section_id]/ _no_slots; cout << endl;
    //     }
    // }

    // should send an ACK; if incoming packet is ECN marked, the ACK will be sent straight away;
    // otherwise ack will be delayed until we have cumulated enough bytes / packets.
    bool ecn = (bool)(pkt.flags() & ECN_CE);

    if (ecn) {
        _stats.ecn_received++;
        _stats.ecn_bytes_received += pkt.size();

        if (_oversubscribed_cc)
            _receiver_cc->ecn_received(pkt.size());
    }

    if (pkt.epsn() < _expected_epsn || _epsn_rx_bitmap[pkt.epsn()]) {
        if (UecSrc::_debug)
            cout << _nodename << " src " << _src->nodename() << " duplicate psn " << pkt.epsn()
                 << endl;

        _stats.duplicates++;

        // if (_src->flow()->flow_id() == UecSrc::_debug_flowid){
        cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
             << " Spurious " << pkt.epsn() << endl;
        // }
        // sender is confused and sending us duplicates: ACK straight away.
        // this code is different from the proposed hardware implementation, as it keeps track of
        // the ACK state of OOO packets.
        UecAckPacket* ack_packet = sack(pkt.path_id(),
                                        ecn ? pkt.epsn() : sackBitmapBase(pkt.epsn()),
                                        pkt.epsn(),
                                        ecn,
                                        pkt.retransmitted());
        _nic.sendControlPacket(ack_packet, NULL, this);

        _accepted_bytes = 0;  // careful about this one.
        return;
    }

    if (_received_bytes == 0) {
        force_ack = true;
    }
    // packet is in window, count the bytes we got.
    // should only count for non RTS and non trimmed packets.
    _received_bytes += pkt.size() - UecAckPacket::ACKSIZE;

    _recvd_bytes += pkt.size();
    if (_src->debug()) {
        cout << _nodename << " recvd_bytes: " << _recvd_bytes << endl;
    }

    assert(_received_bytes <= _src->flowsize());
    if (_src->debug() && _received_bytes >= _src->flowsize())
        cout << _nodename << " received " << _received_bytes << " at "
             << timeAsUs(EventList::getTheEventList().now()) << endl;

    if (pkt.ar()) {
        // this triggers an immediate ack; also triggers another ack later when the ooo queue drains
        // (_ack_request tracks this state)
        force_ack    = true;
        _ack_request = true;
    }

    if (_src->debug())
        cout << _nodename << " src " << _src->nodename()
             << " >>    cumulative ack was: " << _expected_epsn << " flow " << _src->flow()->str()
             << endl;

    if (pkt.epsn() == _expected_epsn) {
        while (_epsn_rx_bitmap[++_expected_epsn]) {
            // clean OOO state, this will wrap at some point.
            _epsn_rx_bitmap[_expected_epsn] = 0;
            _out_of_order_count--;
        }
        if (_src->debug())
            cout << " UecSink " << _nodename << " src " << _src->nodename()
                 << " >>    cumulative ack now: " << _expected_epsn << " ooo count "
                 << _out_of_order_count << " flow " << _src->flow()->str() << endl;

        if (_out_of_order_count == 0 && _ack_request) {
            force_ack    = true;
            _ack_request = false;
        }
    } else {
        _epsn_rx_bitmap[pkt.epsn()] = 1;
        _out_of_order_count++;
        _stats.out_of_order++;
    }
    if (_src->flow()->flow_id() == UecSrc::_debug_flowid) {
        cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
             << " checkSack: " << pkt.epsn() << " ooo_count " << _out_of_order_count << " ecn "
             << ecn << " shouldSack " << shouldSack() << " forceack " << force_ack << endl;
    }
    if (ecn || shouldSack() || force_ack) {
        UecAckPacket* ack_packet = sack(pkt.path_id(),
                                        (ecn || pkt.ar()) ? pkt.epsn() : sackBitmapBase(pkt.epsn()),
                                        pkt.epsn(),
                                        ecn,
                                        pkt.retransmitted());

        if (_src->debug()) {
            cout << " UecSink " << _nodename << " src " << _src->nodename()
                 << " sendAckNow: " << _expected_epsn << " ref_epsn " << pkt.epsn() << " ooo_count "
                 << _out_of_order_count << " recvd_bytes " << _recvd_bytes << " flow "
                 << _src->flow()->str() << " ecn " << ecn << " shouldSack " << shouldSack()
                 << " forceack " << force_ack << endl;
        }

        if (_src->flow()->flow_id() == UecSrc::_debug_flowid) {
            cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
                 << " sendAckNow: " << _expected_epsn << " ref_epsn " << pkt.epsn() << " ooo_count "
                 << _out_of_order_count << " recvd_bytes " << _recvd_bytes << " flow "
                 << _src->flow()->str() << " ecn " << ecn << " shouldSack " << shouldSack()
                 << " forceack " << force_ack << endl;
        }
        _accepted_bytes = 0;

        // ack_packet->sendOn();
        _nic.sendControlPacket(ack_packet, NULL, this);
        /* cout << timeAsUs(getSrc()->eventlist().now()) << " flow " << _src->flow()->flow_id()
             << " sending ack packet for psn " << pkt.epsn() << " ev " << pkt.path_id() << endl; */
    }
}

void UecSink::processProbe(UecDataPacket& pkt) {
    if (_model_pcie) {
        abort();
    }

    if ((!UecSrc::usePflr()) || backgroundECMPFlow) {
        cout << "pflr state error" << endl;
        abort();
    }
    /* cout << timeAsUs(getSrc()->eventlist().now()) << " flow " << _src->flow()->flow_id()
         << " receive probe packet for psn " << pkt.epsn() << " ev " << pkt.path_id() << endl; */

    if (_pflr_scheme_id == 0) {
        // send probe ack
        bool          ecn        = (bool)(pkt.flags() & ECN_CE);
        UecAckPacket* ack_packet = sack(pkt.path_id(), pkt.epsn(), pkt.epsn(), ecn, false);
        ack_packet->set_probe_ack();
        _nic.sendControlPacket(ack_packet, NULL, this);

        /* cout << timeAsUs(getSrc()->eventlist().now()) << " flow " << _src->flow()->flow_id()
             << " sending probe ack packet for psn " << pkt.epsn() << " ev " << pkt.path_id()
             << endl; */

        return;
    } else if (_pflr_scheme_id == 1) {
        auto _no_slots = UecSrc::getNoSlots();
        // get info from received packet
        uint16_t pkt_ev  = pkt.path_id();  // pkt ev, indicates previous ev for probe packet
        uint32_t slot_id = pkt.epsn() % _no_slots;
        if (pkt.pflr_probe_type() == UecDataPacket::SECTION_END) {
            if (_pflr1_slots_expect_psn[slot_id] <= pkt.epsn()) {
                // nack missing ones
                for (auto intermediate_psn = _pflr1_slots_expect_psn[slot_id];
                     intermediate_psn <= pkt.epsn();
                     intermediate_psn += _no_slots) {
                    // nack missing psn
                    pflrSendNack(intermediate_psn, pkt_ev);
                }
                _pflr1_slots_expect_psn[slot_id] = pkt.epsn() + _no_slots;
            } else {
                // do nothing
                cout << "pflr state error" << endl;
            }
        } else if ((pkt.pflr_probe_type() == UecDataPacket::PROACTIVE_RTX) ||
                   (pkt.pflr_probe_type() == UecDataPacket::PROACTIVE_DATA)) {
            // do nothing
            cout << "pflr state error" << endl;
        }
    } else if (_pflr_scheme_id == 2) {
        // send probe ack
        bool          ecn        = (bool)(pkt.flags() & ECN_CE);
        UecAckPacket* ack_packet = sack(pkt.path_id(), pkt.epsn(), pkt.epsn(), ecn, false);
        ack_packet->set_probe_ack();
        _nic.sendControlPacket(ack_packet, NULL, this);

        cout << timeAsUs(getSrc()->eventlist().now()) << " flow " << _src->flow()->flow_id()
             << " sending probe ack packet for psn " << pkt.epsn() << " ev " << pkt.path_id()
             << endl;

        return;
    } else if (_pflr_scheme_id == 3) {
        auto _no_slots = UecSrc::getNoSlots();
        // get info from received packet
        uint16_t pkt_ev = pkt.path_id();  // pkt ev, indicates previous ev for probe packet

        // extend the bitmap if needed
        if (pkt.epsn() >= static_cast<UecDataPacket::seq_t>(_pflr3_receive_bitmap.size()) +
                              _pflr3_bitmap_start_psn) {
            _pflr3_receive_bitmap.resize(pkt.epsn() - _pflr3_bitmap_start_psn + 1, 0);
            _pflr3_nack_bitmap.resize(pkt.epsn() - _pflr3_bitmap_start_psn + 1, 0);
        }
        if ((pkt.pflr_probe_type() == UecDataPacket::SECTION_END) ||
            (pkt.pflr_probe_type() == UecDataPacket::PROACTIVE_DATA)) {
            // set the received pkt
            UecDataPacket::seq_t base_psn = static_cast<UecDataPacket::seq_t>(pkt_ev);
            // check
            if ((base_psn % _no_slots) != (pkt.epsn() % _no_slots)) {
                cout << "pflr state error" << endl;
            }
            // send nack
            UecDataPacket::seq_t first_psn = base_psn;
            while (first_psn < _pflr3_bitmap_start_psn) {
                first_psn += _no_slots;
            }
            //printf("Using PFLDR scheme 3 Flow %s - Received Probe PSN %d\n", _src->flow()->str().c_str(), pkt.epsn());
            for (auto intermediate_psn = first_psn; intermediate_psn <= pkt.epsn();
                 intermediate_psn += _no_slots) {
                if (_pflr3_receive_bitmap[intermediate_psn - _pflr3_bitmap_start_psn] == 1) {
                    // pkt received
                    continue;
                } else if (_pflr3_receive_bitmap[intermediate_psn - _pflr3_bitmap_start_psn] == 0 &&
                           _pflr3_nack_bitmap[intermediate_psn - _pflr3_bitmap_start_psn] == 0) {
                    // pkt not received and not nacked
                    // nack missing psn
                    printf("Sending NACK2 for PSN %d\n", intermediate_psn);
                    pflrSendNack(intermediate_psn, pkt_ev);
                    _pflr3_nack_bitmap[intermediate_psn - _pflr3_bitmap_start_psn] = 1;
                }
            }
        } else if (pkt.pflr_probe_type() == UecDataPacket::PROACTIVE_RTX) {
            if (_pflr3_receive_bitmap[pkt.epsn() - _pflr3_bitmap_start_psn] == 0) {
                // pkt not received and not nacked
                // nack missing psn
                printf("Sending NACK3 for PSN %d\n", pkt.epsn());
                pflrSendNack(pkt.epsn(), pkt_ev);
            }
        } else {
            cout << "pflr state error" << endl;
            abort();
        }
    } else {
        cout << "unrecognized _pflr_scheme_id: " << _pflr_scheme_id << endl;
        abort();
    }
}

void UecSink::processTrimmed(const UecDataPacket& pkt) {
    _stats.trimmed++;
    if (_oversubscribed_cc) {
        bool is_last_hop = (pkt.nexthop() - pkt.trim_hop() - 2) == 0;
        _receiver_cc->trimmed_received(is_last_hop);
    }

    // We may already have (or have cumulatively ACKed) this PSN.
    if (pkt.epsn() < _expected_epsn || _epsn_rx_bitmap[pkt.epsn()]) {
        if (_src->debug())
            cout << " UecSink processTrimmed got a packet we already have: " << pkt.epsn()
                 << " time " << timeAsNs(getSrc()->eventlist().now()) << " flow "
                 << _src->flow()->str() << endl;

        UecAckPacket* ack_packet =
            sack(pkt.path_id(), sackBitmapBase(pkt.epsn()), pkt.epsn(), false, pkt.retransmitted());
        _nic.sendControlPacket(ack_packet, NULL, this);
        return;
    }

    if (_src->debug())
        cout << " UecSink processTrimmed packet " << pkt.epsn() << " time "
             << timeAsNs(getSrc()->eventlist().now()) << " flow " << _src->flow()->str() << endl;

    handlePullTarget(pkt.pull_target());

    // ===== PFLD-3 aware path (optional optimization) =====
    if (UecSrc::usePflr() && !backgroundECMPFlow && _pflr_scheme_id == 3) {

        printf("Received TRIMMED packet for PSN %d\n", pkt.epsn());
        // If we haven't initialized PFLD-3 bitmaps via data/probe yet, just NACK once.
        if (_pflr3_receive_bitmap.empty()) {
            UecNackPacket* nack_packet = nack(pkt.path_id(), pkt.epsn());
            _nic.sendControlPacket(nack_packet, NULL, this);
        } else {
            // Ensure bitmaps cover this PSN
            if (pkt.epsn() >= _pflr3_bitmap_start_psn + _pflr3_receive_bitmap.size()) {
                size_t extend = pkt.epsn() - (_pflr3_bitmap_start_psn + _pflr3_receive_bitmap.size()) + 1;
                _pflr3_receive_bitmap.insert(_pflr3_receive_bitmap.end(), extend, 0);
                _pflr3_nack_bitmap.insert(_pflr3_nack_bitmap.end(), extend, 0);
            }
            size_t idx = pkt.epsn() - _pflr3_bitmap_start_psn;
            // If not received and not already NACKed, NACK and mark NACK bitmap
            if (_pflr3_receive_bitmap[idx] == 0 && _pflr3_nack_bitmap[idx] == 0) {
                pflrSendNack(pkt.epsn(), pkt.path_id());
                _pflr3_nack_bitmap[idx] = 1;
            }
        }

        if (UecSrc::_receiver_based_cc && !_in_pull) {
            if (_src->debug())
                cout << "PullPacer RequestPull: " << _src->flow()->str() << " at "
                     << timeAsUs(getSrc()->eventlist().now()) << endl;
            _in_pull = true;
            _pullPacer->requestPull(this);
        }
        return;
    }
    // ===== default path (unchanged behavior) =====
    if (_src->debug())
        cout << "RTX_backlog++ trim: " << pkt.epsn() << " from " << getSrc()->nodename()
             << " rtx_backlog " << rtx_backlog() << " at " << timeAsUs(getSrc()->eventlist().now())
             << " flow " << _src->flow()->str() << endl;

    UecNackPacket* nack_packet = nack(pkt.path_id(), pkt.epsn());
    _nic.sendControlPacket(nack_packet, NULL, this);

    if (UecSrc::_receiver_based_cc && !_in_pull) {
        if (_src->debug())
            cout << "PullPacer RequestPull: " << _src->flow()->str() << " at "
                 << timeAsUs(getSrc()->eventlist().now()) << endl;
        _in_pull = true;
        _pullPacer->requestPull(this);
    }
}



const Route* UecSink::getPortRoute(uint32_t port_num) const {
    return _ports[port_num]->route();
}

UecSinkPort* UecSink::getPort(uint32_t port_num) {
    return _ports[port_num];
}

void UecSink::processRts(const UecRtsPacket& pkt) {
    assert(pkt.ar());
    if (_src->debug())
        cout << " UecSink " << _nodename << " src " << _src->nodename()
             << " processRts: " << pkt.epsn() << " time " << timeAsNs(getSrc()->eventlist().now())
             << endl;

    handlePullTarget(pkt.pull_target());

    // what happens if this is not an actual retransmit, i.e. the host decides with the ACK that it
    // is

    if (_src->debug())
        cout << "RTX_backlog++ RTS: " << _src->flow()->str() << " rtx_backlog " << rtx_backlog()
             << " at " << timeAsUs(getSrc()->eventlist().now()) << endl;

    if (UecSrc::_receiver_based_cc && !_in_pull) {
        _in_pull = true;
        _pullPacer->requestPull(this);
    }

    bool ecn = (bool)(pkt.flags() & ECN_CE);
    assert(!ecn);  // not expecting ECN set on control packets

    if (pkt.epsn() < _expected_epsn || _epsn_rx_bitmap[pkt.epsn()]) {
        if (_src->debug())
            cout << _nodename << " src " << _src->nodename() << " duplicate RTS psn " << pkt.epsn()
                 << endl;

        _stats.duplicates++;

        // sender is confused and sending us duplicates: ACK straight away.
        // this code is different from the proposed hardware implementation, as it keeps track of
        // the ACK state of OOO packets.
        UecAckPacket* ack_packet =
            sack(pkt.path_id(), sackBitmapBase(pkt.epsn()), pkt.epsn(), ecn, pkt.retransmitted());
        ack_packet->set_rts_ack();
        _nic.sendControlPacket(ack_packet, NULL, this);

        _accepted_bytes = 0;  // careful about this one.
        return;
    }

    if (pkt.epsn() == _expected_epsn) {
        while (_epsn_rx_bitmap[++_expected_epsn]) {
            // clean OOO state, this will wrap at some point.
            _epsn_rx_bitmap[_expected_epsn] = 0;
            _out_of_order_count--;
        }
        if (_src->debug())
            cout << " UecSink " << _nodename << " src " << _src->nodename()
                 << " >>    cumulative ack now: " << _expected_epsn << " ooo count "
                 << _out_of_order_count << " flow " << _src->flow()->str() << endl;

        if (_out_of_order_count == 0 && _ack_request) {
            _ack_request = false;
        }
    } else {
        _epsn_rx_bitmap[pkt.epsn()] = 1;
        _out_of_order_count++;
        _stats.out_of_order++;
    }

    UecAckPacket* ack_packet = sack(pkt.path_id(),
                                    (ecn || pkt.ar()) ? pkt.epsn() : sackBitmapBase(pkt.epsn()),
                                    pkt.epsn(),
                                    ecn,
                                    pkt.retransmitted());
    ack_packet->set_rts_ack();

    if (_src->debug())
        cout << " UecSink " << _nodename << " src " << _src->nodename()
             << " send ack now: " << _expected_epsn << " ooo count " << _out_of_order_count
             << " flow " << _src->flow()->str() << endl;

    _nic.sendControlPacket(ack_packet, NULL, this);
}

void UecSink::receivePacket(Packet& pkt, uint32_t port_num) {
    _stats.received++;
    _stats.bytes_received += pkt.size();  // should this include just the payload?

    if (_oversubscribed_cc)
        _receiver_cc->data_received(pkt.size());

    switch (pkt.type()) {
        case UECDATA: {
            auto data_pkt = (UecDataPacket&)pkt;
            if (pkt.header_only() || pkt.header_low_only()) {
                processTrimmed((const UecDataPacket&)pkt);
                // cout << "UecSink::receivePacket receive trimmed packet\n";
                // assert(false);
            } else if (data_pkt.is_probe_packet()) {
                processProbe(data_pkt);
            } else {
                processData(data_pkt);
            }
            pkt.free();
            break;
        }
        case UECRTS:
            processRts((const UecRtsPacket&)pkt);
            pkt.free();
            break;
        default:
            cout << "UecSink::receivePacket receive weird packets\n";
            abort();
    }
}

uint16_t UecSink::nextEntropy() {
    int spraymask     = (1 << TGT_EV_SIZE) - 1;
    int fixedmask     = ~spraymask;
    int idx           = _entropy & spraymask;
    int fixed_entropy = _entropy & fixedmask;
    int ev            = ++idx & spraymask;

    _entropy = fixed_entropy | ev;  // save for next pkt

    return ev;
}

UecPullPacket* UecSink::pull(UecBasePacket::pull_quanta& extra_credit) {
    // called when pull pacer is ready to give another credit to this connection.
    // TODO: need to credit in multiple of MTU here.

    if (_retx_backlog > 0) {
        if (_retx_backlog > UecSink::_credit_per_pull)
            _retx_backlog -= UecSink::_credit_per_pull;
        else
            _retx_backlog = 0;

        if (UecSrc::_debug)
            cout << "RTX_backlog--: " << getSrc()->nodename() << " rtx_backlog " << rtx_backlog()
                 << " at " << timeAsUs(getSrc()->eventlist().now()) << " flow "
                 << _src->flow()->str() << endl;
    }

    if (extra_credit == 0) {
        // only send as much credit as the sender asked for
        auto prev_pull = _latest_pull;
        _latest_pull += UecSink::_credit_per_pull;
        if (_latest_pull > _highest_pull_target) {
            // don't go above pull_target, but also don't go backwards
            _latest_pull = max(_highest_pull_target, prev_pull);
        }
        extra_credit = _latest_pull - prev_pull;
    } else {
        // it's a slow pull, ignore pull target and just grant what we're told
        _latest_pull += extra_credit;
    }

    UecPullPacket* pkt = NULL;
    pkt                = UecPullPacket::newpkt(_flow, NULL, _latest_pull, false, _srcaddr);
    pkt->set_pathid(nextEntropy());

    return pkt;
}

bool UecSink::shouldSack() {
    return _accepted_bytes >= _bytes_unacked_threshold;
}

UecBasePacket::seq_t UecSink::sackBitmapBase(UecBasePacket::seq_t epsn) {
    return max((int64_t)epsn - 63, (int64_t)(_expected_epsn + 1));
}

UecBasePacket::seq_t UecSink::sackBitmapBaseIdeal() {
    uint8_t              lowest_value    = UINT8_MAX;
    UecBasePacket::seq_t lowest_position = 0;

    // find the lowest non-zero value in the sack bitmap; that is the candidate for the base, since
    // it is the oldest packet that we are yet to sack. on sack bitmap construction that covers a
    // given seqno, the value is incremented.
    for (UecBasePacket::seq_t crt = _expected_epsn; crt <= _high_epsn; crt++)
        if (_epsn_rx_bitmap[crt] && _epsn_rx_bitmap[crt] < lowest_value) {
            lowest_value    = _epsn_rx_bitmap[crt];
            lowest_position = crt;
        }

    if (lowest_position + 64 > _high_epsn)
        lowest_position = _high_epsn - 64;

    if (lowest_position <= _expected_epsn)
        lowest_position = _expected_epsn + 1;

    return lowest_position;
}

uint64_t UecSink::buildSackBitmap(UecBasePacket::seq_t ref_epsn) {
    // take the next 64 entries from ref_epsn and create a SACK bitmap with them
    if (_src->debug())
        cout << " UecSink: building sack for ref_epsn " << ref_epsn << endl;
    uint64_t bitmap = (uint64_t)(_epsn_rx_bitmap[ref_epsn] != 0) << 63;

    for (int i = 1; i < 64; i++) {
        bitmap = bitmap >> 1 | (uint64_t)(_epsn_rx_bitmap[ref_epsn + i] != 0) << 63;
        if (_src->debug() && (_epsn_rx_bitmap[ref_epsn + i] != 0))
            cout << "     Sack: " << ref_epsn + i << endl;

        if (_epsn_rx_bitmap[ref_epsn + i]) {
            // remember that we sacked this packet
            if (_epsn_rx_bitmap[ref_epsn + i] < UINT8_MAX)
                _epsn_rx_bitmap[ref_epsn + i]++;
        }
    }
    if (_src->debug())
        cout << "       bitmap is: " << bitmap << endl;
    return bitmap;
}

UecAckPacket* UecSink::sack(uint16_t             path_id,
                            UecBasePacket::seq_t seqno,
                            UecBasePacket::seq_t acked_psn,
                            bool                 ce,
                            bool                 rtx) {
    uint64_t      bitmap = buildSackBitmap(seqno);
    UecAckPacket* pkt    = UecAckPacket::newpkt(_flow,
                                             NULL,
                                             _expected_epsn,
                                             seqno,
                                             acked_psn,
                                             path_id,
                                             ce,
                                             _recvd_bytes,
                                             _rcv_cwnd_pen,
                                             _srcaddr);
    pkt->set_bitmap(bitmap);
    pkt->set_ooo(_out_of_order_count);
    pkt->set_rtx_echo(rtx);
    pkt->set_packet_type_echo(UecBasePacket::DATA_PULL);
    return pkt;
}

UecNackPacket* UecSink::nack(uint16_t path_id, UecBasePacket::seq_t seqno) {
    UecNackPacket* pkt =
        UecNackPacket::newpkt(_flow, NULL, seqno, path_id, _recvd_bytes, _rcv_cwnd_pen, _srcaddr);
    return pkt;
}

void UecSink::setEndTrigger(Trigger& end_trigger) {
    _end_trigger = &end_trigger;
};

/*static unsigned pktByteTimes(unsigned size) {
    // IPG (96 bit times) + preamble + SFD + ether header + FCS = 38B
    return max(size, 46u) + 38;
}*/

uint32_t UecSink::reorder_buffer_size() {
    uint32_t count = 0;
    // it's not very efficient to count each time, but if we only do
    // this occasionally when the sink logger runs, it should be OK.
    for (uint32_t i = 0; i < uecMaxInFlightPkts; i++) {
        if (_epsn_rx_bitmap[i])
            count++;
    }
    return count;
}
