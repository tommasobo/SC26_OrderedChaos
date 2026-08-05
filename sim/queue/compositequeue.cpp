// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "compositequeue.h"

#include <math.h>

#include <iostream>
#include <sstream>
#include <unordered_map>  // Add this for the hashmap
#include <unordered_set>  // Add this for tracking dropped PSNs
#include <random>         // For random PSN selection
#include <fstream>        // Minimal logging to file
#include <cctype>         // isalnum for filename sanitization

#include "helpers.h"
#include "types.h"
#include "uec_packet.h"
#include "uec_src.h"

static int  global_queue_id    = 0;
static bool print_switch_trace = false;
#define DEBUG_QUEUE_ID -1  // set to queue ID to enable debugging

// Change: _fail_psn_num now means "number of packets to drop per flow"
int CompositeQueue::_fail_psn_num = -1;  // if zero, doesn't drop anything, otherwise the number of PSNs to drop per flow
bool CompositeQueue::_probe_high_priority = false;
bool CompositeQueue::_coalesce_trimmed_pfld_probe = false;
uint64_t CompositeQueue::_coalesced_pfld_probe_count = 0;

// Add: Track dropped PSNs per flow
std::unordered_map<uint32_t, std::unordered_set<int>> dropped_psns_per_flow;

// Add: Random generator for PSN selection
std::mt19937 rng(std::random_device{}());

static inline void logLegacyReactionDrop(EventList& eventlist, Packet& pkt) {
    if (!UecSrc::_log_reaction_events) {
        return;
    }
    if (pkt.type() != UECDATA || pkt.header_only() || pkt.size() <= 500) {
        return;
    }
    cout << "Drop: FlowID " << pkt.flow_id() << " - Packet ID " << pkt.id()
         << " - Time " << eventlist.now() << endl;
}

CompositeQueue::CompositeQueue(linkspeed_bps bitrate,
                               mem_b         maxsize,
                               EventList&    eventlist,
                               QueueLogger*  logger,
                               uint16_t      trim_size,
                               bool          disable_trim,
                               bool          low_priority_trim,
                               bool          no_droping_low_header,
                               double        switch_random_drop_prob)
    : Queue(bitrate, maxsize, eventlist, logger) {
    _disable_trim            = disable_trim;
    _low_priority_trim       = disable_trim && low_priority_trim;
    _no_droping_low_header   = no_droping_low_header;
    _switch_random_drop_prob = switch_random_drop_prob;
    _trim_size               = trim_size;
    _ratio_high              = 100000;
    _ratio_low               = 1;
    _crt                     = 0;
    _num_headers             = 0;
    _num_packets             = 0;
    _num_acks                = 0;
    _num_nacks               = 0;
    _num_pulls               = 0;
    _num_drops               = 0;
    _num_stripped            = 0;
    _num_bounced             = 0;
    _ecn_minthresh           = maxsize * 2;  // don't set ECN by default
    _ecn_maxthresh           = maxsize * 2;  // don't set ECN by default

    _return_to_sender = false;

    _queuesize_high = _queuesize_low = 0;
    _reserved_probe_bytes             = 0;
    _serv                            = QUEUE_INVALID;
    stringstream ss;
    ss << "compqueue(" << bitrate / 1000000 << "Mb/s," << maxsize << "bytes)";
    _nodename = ss.str();
    _queue_id = global_queue_id++;
    if (_queue_id == DEBUG_QUEUE_ID)
        cout << "queueid " << _queue_id << " bitrate " << bitrate / 1000000 << "Mb/s," << endl;
}

void CompositeQueue::beginService() {
    if (!_enqueued_high.empty() && !_enqueued_low.empty()) {
        _crt++;

        if (_crt >= (_ratio_high + _ratio_low))
            _crt = 0;

        if (_crt < _ratio_high) {
            _serv = QUEUE_HIGH;
            eventlist().sourceIsPendingRel(*this, drainTime(_enqueued_high.back()));
        } else {
            assert(_crt < _ratio_high + _ratio_low);
            _serv = QUEUE_LOW;
            eventlist().sourceIsPendingRel(*this, drainTime(_enqueued_low.back()));
        }
        return;
    }

    if (!_enqueued_high.empty()) {
        _serv = QUEUE_HIGH;
        eventlist().sourceIsPendingRel(*this, drainTime(_enqueued_high.back()));
    } else if (!_enqueued_low.empty()) {
        _serv = QUEUE_LOW;
        eventlist().sourceIsPendingRel(*this, drainTime(_enqueued_low.back()));
    } else {
        throw std::runtime_error("CompositeQueue: beginService: no packets in queue");
    }
}

bool CompositeQueue::decide_ECN() {
    const mem_b regular_queue_bytes = regularLowQueueBytes();
    // ECN mark on deque
    if (regular_queue_bytes > _ecn_maxthresh) {
        return true;
    } else if (regular_queue_bytes > _ecn_minthresh) {
        uint64_t p =
            (0x7FFFFFFF * (regular_queue_bytes - _ecn_minthresh)) /
            (_ecn_maxthresh - _ecn_minthresh);
        if ((uint64_t)random() < p) {
            return true;
        }
    }
    return false;
}

void CompositeQueue::completeService() {
    Packet* pkt;
    if (_serv == QUEUE_LOW) {
        assert(!_enqueued_low.empty());
        pkt = _enqueued_low.pop();
        if (_monitorQueue) {
            _queue_deq_metric->LogData({std::to_string(_queuesize_low),
                                        std::to_string(pkt->size()),
                                        std::to_string(pkt->type()),
                                        to_string(pkt->pathid()),
                                        to_string(pkt->dst())});
        }
        _queuesize_low -= pkt->size();
        if (_no_droping_low_header && pkt->type() == UECDATA &&
            static_cast<UecDataPacket*>(pkt)->is_probe_packet()) {
            assert(_reserved_probe_bytes >= pkt->size());
            _reserved_probe_bytes -= pkt->size();
        }

        // ECN mark on deque
        if (decide_ECN()) {
            pkt->set_flags(pkt->flags() | ECN_CE);
        }
        if (_queue_id == DEBUG_QUEUE_ID) {
            cout << timeAsUs(eventlist().now()) << " name " << _nodename << " _queuesize_low "
                 << _queuesize_low * 8 / ((_bitrate / 1000000.0)) << " _queueid " << _queue_id
                 << " switch " << _switch->getID() << " ecn " << decide_ECN() << " _queuesize_high "
                 << _queuesize_high * 8 / ((_bitrate / 1000000.0)) << endl;
        }
        if (_logger)
            _logger->logQueue(*this, QueueLogger::PKT_SERVICE, *pkt);
        _num_packets++;
    } else if (_serv == QUEUE_HIGH) {
        assert(!_enqueued_high.empty());
        pkt = _enqueued_high.pop();
        _queuesize_high -= pkt->size();
        if (_logger)
            _logger->logQueue(*this, QueueLogger::PKT_SERVICE, *pkt);
        if (pkt->type() == NDPACK)
            _num_acks++;
        else if (pkt->type() == NDPNACK)
            _num_nacks++;
        else if (pkt->type() == NDPPULL)
            _num_pulls++;
        else {
            // cout << "Hdr: type=" << pkt->type() << endl;
            _num_headers++;
            // ECN mark on deque of a header, if low priority queue is still over threshold
            //            if (decide_ECN()) {
            //                pkt->set_flags(pkt->flags() | ECN_CE);
            //            }
        }
    } else {
        throw std::runtime_error("CompositeQueue: completeService: invalid service");
    }

    pkt->flow().logTraffic(*pkt, *this, TrafficLogger::PKT_DEPART);
    pkt->sendOn();

    //_virtual_time += drainTime(pkt);

    _serv = QUEUE_INVALID;

    if (!_enqueued_high.empty() || !_enqueued_low.empty())
        beginService();
}

void CompositeQueue::doNextEvent() {
    completeService();
}

void printUecDataPacketAction(simtime_picosec now, string action_name, Packet* pkt) {
    if (pkt->type() == UECDATA) {
        auto   uec_data_pkt     = (UecDataPacket*)pkt;
        string packet_type_name = "data";
        if (pkt->header_low_only()) {
            packet_type_name = "trim";
        } else if (uec_data_pkt->is_probe_packet()) {
            packet_type_name = "probe";
        } else if (uec_data_pkt->retransmitted()) {
            packet_type_name = "rtx";
        } else if (uec_data_pkt->packet_type() == UecBasePacket::DATA_PROBE) {
            packet_type_name = "sleek probe";
        } else {
            packet_type_name = "data";
        }
        cout << timeAsUs(now) << " flow " << uec_data_pkt->flow_id() << " " << action_name << " "
             << packet_type_name;
        cout << " packet for psn " << uec_data_pkt->epsn() << " ev " << uec_data_pkt->path_id()
             << endl;
    }
}

void printQueueSize(
    simtime_picosec now, int queue_id, string queue_name, mem_b queue_size, mem_b max_size) {
    return;
        if (false) {
        cout << timeAsUs(now) << " queue " << queue_id << " " << queue_name << " size "
             << queue_size << " max " << max_size << endl;
    }
}

void CompositeQueue::receivePacket(Packet& pkt) {
    if (_monitorQueue) {
        _queue_enq_metric->LogData({std::to_string(_queuesize_low),
                                    std::to_string(pkt.size()),
                                    std::to_string(pkt.type()),
                                    to_string(pkt.pathid()),
                                    to_string(pkt.dst())});
    }
    if (_queue_id == DEBUG_QUEUE_ID) {
        cout << timeAsUs(eventlist().now()) << " name " << _nodename << " arrive "
             << _queuesize_low * 8 / ((_bitrate / 1000000.0)) << " _queueid " << _queue_id
             << " switch " << _switch->getID() << " flowid " << pkt.flow_id() << " ev "
             << pkt.pathid() << endl;
    }
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_ARRIVE);
    if (_logger)
        _logger->logQueue(*this, QueueLogger::PKT_ARRIVE, pkt);

    /* if (pkt.drop_this) {
        printf("Dropping the last probe packet for flow %u\n", pkt.flow_id());
        pkt.free();
        return;
    } */

    //printf("Queue %s - Packet PSN is %d\n", _nodename.c_str(), pkt.id());


    /* if (_nodename.find("DST") != std::string::npos && !pkt.is_ack) {
        printf("%f - Flow Id %d %s - Packet PSN %d - Packet Size %d - Packet Type %d - Ev %d = %s\n",
             timeAsUs(eventlist().now()), pkt.flow_id(), pkt.flow_name.c_str(), pkt.id(), pkt.size(), pkt.type(), pkt.pathid(), _nodename.c_str());
    } */

    bool is_probing = false;
    if (pkt.type() == UECDATA) {
        auto uec_data_pkt = (UecDataPacket&)pkt;
        if (uec_data_pkt.is_probe_packet()) {
            is_probing = true;
        }
    }

    /* if (_nodename == "compqueue(400000Mb/s,62816000bytes)LS0->US1(0)") {
            if (pkt.type() == UECDATA) {
            auto uec_data_pkt = (UecDataPacket&)pkt;
            if (uec_data_pkt.is_probe_packet()) {
                is_probing = true;
            }
        }
        printf("Switch %s - Received packet PSN %d, size %d, flowid %u, is probing %d\n",
               _nodename.c_str(), pkt.id(), pkt.size(), pkt.flow_id(), is_probing);
    */

    // random drop
    //if ((!pkt.header_only()) && ((pkt.id() == _fail_psn_num || pkt.id() == _fail_psn_num - 1) || (_fail_psn_num == -1)) && (_switch_random_drop_prob > 0) && _never_dropped && (_nodename == "compqueue(400000Mb/s,628160bytes)LS25->DST100(0)" || _fail_psn_num == -1)) {
    if ((!pkt.header_only()) && pkt.size() > 500) {
    // a low priority packet
        // double normalized_drop_prob = ((double) pkt.size()) / ((double) UecSrc::_mtu) *
        // _switch_random_drop_prob;

        /* if (fail_queue) {
            //printf("Dropping packet22 PSN %d for new flow %u\n", pkt.id(), 1);
            pkt.free();
            _num_drops++;
            return;
        } */


        if (false) {
            if (_fail_psn_num != -1 && pkt.id() == _fail_psn_num) {
                uint32_t fid = pkt.flow_id();
                if (dropped_flows.find(fid) == dropped_flows.end()) {
                    dropped_flows[fid] = true;
                    //printf("Dropping packet PSN %d for new flow %u\n", pkt.id(), fid);
                    pkt.free();
                    _num_drops++;
                    return;
                }
            }
        } else {
            double normalized_drop_prob = 1 - pow(1 - _switch_random_drop_prob, ((double) pkt.size()) / ((double) UecSrc::_mtu));

            // cout << "pkt size " << pkt.size() << ", normalized drop prob " << normalized_drop_prob <<
            // endl;
            if (drand() <= normalized_drop_prob) {
                // drand: 0 to 1 inclusive
                // Skip dropping TLP probes if high-priority probe mode is on
                bool skip_drop = false;
                if (_probe_high_priority && pkt.type() == UECDATA) {
                    auto* uec_pkt = (UecDataPacket*)&pkt;
                    if (uec_pkt->is_probe_packet()) skip_drop = true;
                }
                if (!skip_drop) {
                    if (print_switch_trace) {
                        printUecDataPacketAction(eventlist().now(), "drop", &pkt);
                    }
                    if (_fail_psn_num != -1 && _total_drops == 0) {
                        _never_dropped = false;
                    }
                    logLegacyReactionDrop(eventlist(), pkt);
                    pkt.free();
                    _num_drops++;
                    _total_drops--;
                    return;
                }
            }
        }

        
    }

    // A trim header already certifies the loss of its data packet and will
    // cause a NACK. Suppress only the matching proactive probe at the queue
    // that performed the trim. Other probe types remain untouched.
    if (_coalesce_trimmed_pfld_probe && is_probing) {
        auto& probe = static_cast<UecDataPacket&>(pkt);
        if (probe.pflr_probe_type() == UecDataPacket::PflrProbeType::PROACTIVE_DATA &&
            consumeTrimmedPfldProbe(pkt)) {
            _coalesced_pfld_probe_count++;
            pkt.free();
            return;
        }
    }

    // Probes use reserved headroom but remain in the normal low-priority FIFO,
    // so they cannot overtake data already queued on this path.
    if (_no_droping_low_header && is_probing) {
        Packet* pkt_p = &pkt;
        _enqueued_low.push(pkt_p);
        _queuesize_low += pkt.size();
        _reserved_probe_bytes += pkt.size();
        if (_logger)
            _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);

        if (_serv == QUEUE_INVALID) {
            beginService();
        }
        return;
    }

    // low priority trim, only trim incomming packet
    // then drop as normal packet
    if ((!pkt.header_low_only()) && _low_priority_trim) {
        assert(_disable_trim);
        if (regularLowQueueBytes() + pkt.size() > _maxsize) {
            if (print_switch_trace) {
                printUecDataPacketAction(eventlist().now(), "trim", &pkt);
            }
            logLegacyReactionDrop(eventlist(), pkt);
            rememberTrimmedPfldProbe(pkt);
            // trim pkt and treated as a small data packet
            pkt.strip_payload_low(_trim_size);
            // cout << "CQ trim at " << _nodename << endl;
            _num_stripped++;
            pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_TRIM);
            if (_logger)
                _logger->logQueue(*this, QueueLogger::PKT_TRIM, pkt);
        }
    }

    if (!pkt.header_only()) {
        bool condition = (regularLowQueueBytes() + pkt.size() <= _maxsize);
        if (_no_droping_low_header) {
            // if not dropping low priority header pkt(probe, low trim)
            // discard the random choosing step
            condition = regularLowQueueBytes() + pkt.size() <= _maxsize;
        }
        if (condition) {
            // regular packet; don't drop the arriving packet

            // we are here because either the queue isn't full or,
            // it might be full and we randomly chose an
            // enqueued packet to trim

            if (regularLowQueueBytes() + pkt.size() > _maxsize) {
                // we're going to drop an existing packet from the queue
                if (_enqueued_low.empty()) {
                    // cout << "QUeuesize " << _queuesize_low << " packetsize " << pkt.size() << "
                    // maxsize " << _maxsize << endl;
                    throw std::runtime_error(
                        "CompositeQueue: receivePacket: low priority queue is empty");
                }
                // take last packet from low prio queue, make it a header and place it in the high
                // prio queue
                Packet* booted_pkt = _enqueued_low.pop_front();
                _queuesize_low -= booted_pkt->size();
                if (_logger)
                    _logger->logQueue(*this, QueueLogger::PKT_UNQUEUE, *booted_pkt);

                if (_disable_trim) {
                    if (print_switch_trace) {
                        printUecDataPacketAction(eventlist().now(), "drop", booted_pkt);
                    }
                    logLegacyReactionDrop(eventlist(), *booted_pkt);
                    booted_pkt->free();
                    _num_drops++;
                    /* cout << "A [ " << _enqueued_low.size() << " " << _enqueued_high.size()
                         << " ] DROP " << " flowid " << booted_pkt->flow_id() << endl; */
                    } else {
                        // cout << "A [ " << _enqueued_low.size() << " " << _enqueued_high.size() << " ]
                        // STRIP" << endl; cout << "booted_pkt->size(): " << booted_pkt->size();
                        if (print_switch_trace) {
                            printUecDataPacketAction(eventlist().now(), "trim", booted_pkt);
                        }
                        logLegacyReactionDrop(eventlist(), *booted_pkt);
                        rememberTrimmedPfldProbe(*booted_pkt);
                        booted_pkt->strip_payload(_trim_size);
                    // cout << "CQ trim at " << _nodename << endl;
                    _num_stripped++;
                    booted_pkt->flow().logTraffic(*booted_pkt, *this, TrafficLogger::PKT_TRIM);
                    if (_logger)
                        _logger->logQueue(*this, QueueLogger::PKT_TRIM, pkt);

                    if (_queuesize_high + booted_pkt->size() > 2 * _maxsize) {
                        if (_return_to_sender && booted_pkt->reverse_route() &&
                            booted_pkt->bounced() == false) {
                            // return the packet to the sender
                            if (_logger)
                                _logger->logQueue(*this, QueueLogger::PKT_BOUNCE, *booted_pkt);
                            booted_pkt->flow().logTraffic(pkt, *this, TrafficLogger::PKT_BOUNCE);
                            // XXX what to do with it now?
#if 0
                            printf("Bounce2 at %s\n", _nodename.c_str());
                            printf("Fwd route:\n");
                            print_route(*(booted_pkt->route()));
                            printf("nexthop: %d\n", booted_pkt->nexthop());
#endif
                            booted_pkt->bounce();
#if 0
                            printf("\nRev route:\n");
                            print_route(*(booted_pkt->reverse_route()));
                            printf("nexthop: %d\n", booted_pkt->nexthop());
#endif
                            _num_bounced++;
                            booted_pkt->sendOn();
                        } else {
                            booted_pkt->flow().logTraffic(
                                *booted_pkt, *this, TrafficLogger::PKT_DROP);
                            logLegacyReactionDrop(eventlist(), *booted_pkt);
                            booted_pkt->free();
                            if (_logger)
                                _logger->logQueue(*this, QueueLogger::PKT_DROP, pkt);
                        }
                    } else {
                        _enqueued_high.push(booted_pkt);
                        _queuesize_high += booted_pkt->size();
                        if (_logger)
                            _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, *booted_pkt);
                    }
                }
            }

            // assert(_queuesize_low+pkt.size()<= _maxsize);
            Packet* pkt_p = &pkt;
            _enqueued_low.push(pkt_p);
            _queuesize_low += pkt.size();
            if (_logger)
                _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);

            if (_serv == QUEUE_INVALID) {
                beginService();
            }

            // cout << "BL[ " << _enqueued_low.size() << " " << _enqueued_high.size() << " ]" <<
            // endl;
            /* printQueueSize(eventlist().now(), _queue_id, "low", _queuesize_low, _maxsize); */
            return;
        } else {
            if (_disable_trim) {
                // if not dropping low priority header pkt
                if (_no_droping_low_header) {
                    // first see if it is probe/low trim
                    bool is_low_trim = _low_priority_trim && pkt.header_low_only();
                    bool is_probe    = false;
                    if (pkt.type() == UECDATA) {
                        auto uec_data_pkt = (UecDataPacket&)pkt;
                        if (uec_data_pkt.is_probe_packet()) {
                            is_probe = true;
                        }
                    }
                    if (is_low_trim || is_probe) {
                        // no dropping, so push the packet anyway
                        Packet* pkt_p = &pkt;
                        _enqueued_low.push(pkt_p);
                        _queuesize_low += pkt.size();
                        if (_logger)
                            _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);

                        if (_serv == QUEUE_INVALID) {
                            beginService();
                        }
                        /* printQueueSize(
                            eventlist().now(), _queue_id, "low", _queuesize_low, _maxsize); */
                        return;
                    }
                }
                if (print_switch_trace) {
                    printUecDataPacketAction(eventlist().now(), "drop", &pkt);
                }
                // Skip dropping TLP probes if high-priority probe mode
                {
                    bool skip_overflow_drop = false;
                    if (_probe_high_priority && pkt.type() == UECDATA) {
                        auto* uec_pkt = (UecDataPacket*)&pkt;
                        if (uec_pkt->is_probe_packet()) skip_overflow_drop = true;
                    }
                    if (skip_overflow_drop) {
                        // Force-enqueue the probe despite buffer overflow
                        Packet* pkt_p = &pkt;
                        _enqueued_low.push(pkt_p);
                        _queuesize_low += pkt.size();
                        if (_serv == QUEUE_INVALID) beginService();
                        return;
                    }
                }
               if (pkt.type() == UECDATA) {
                    auto uec_data_pkt2 = (UecDataPacket&)pkt;
                    /* cout << "B [ " << _enqueued_low.size() << " " << _enqueued_high.size()
                         << " ] DROP " << " flow " << pkt.flow_id() << " PSN " << uec_data_pkt2.epsn() << endl; */
                }
               
                logLegacyReactionDrop(eventlist(), pkt);
                pkt.free();
                _num_drops++;
                return;
            }
            // strip packet the arriving packet - low priority queue is full
            // cout << "B [ " << _enqueued_low.size() << " " << _enqueued_high.size() << " ] STRIP"
            // << endl;
            if (print_switch_trace) {
                printUecDataPacketAction(eventlist().now(), "trim", &pkt);
            }
            logLegacyReactionDrop(eventlist(), pkt);
            rememberTrimmedPfldProbe(pkt);
            pkt.strip_payload(_trim_size);
            // cout << "CQ trim at " << _nodename << endl;
            _num_stripped++;
            pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_TRIM);
            if (_logger)
                _logger->logQueue(*this, QueueLogger::PKT_TRIM, pkt);
        }
    }
    assert(pkt.header_only());

    if (_queuesize_high + pkt.size() > 2 * _maxsize) {
        // drop header
        // cout << "drop!\n";
        if (_return_to_sender && pkt.reverse_route() && pkt.bounced() == false) {
            // return the packet to the sender
            if (_logger)
                _logger->logQueue(*this, QueueLogger::PKT_BOUNCE, pkt);
            pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_BOUNCE);
            // XXX what to do with it now?
#if 0
            printf("Bounce1 at %s\n", _nodename.c_str());
            printf("Fwd route:\n");
            print_route(*(pkt.route()));
            printf("nexthop: %d\n", pkt.nexthop());
#endif
            pkt.bounce();
#if 0
            printf("\nRev route:\n");
            print_route(*(pkt.reverse_route()));
            printf("nexthop: %d\n", pkt.nexthop());
#endif
            _num_bounced++;
            pkt.sendOn();
            return;
        } else {
            if (_logger)
                _logger->logQueue(*this, QueueLogger::PKT_DROP, pkt);
            pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_DROP);
            /* cout << "B[ " << _enqueued_low.size() << " " << _enqueued_high.size() << " ] DROP "
                 << pkt.flow().flow_id() << endl; */
            logLegacyReactionDrop(eventlist(), pkt);
            pkt.free();
            _num_drops++;
            return;
        }
    }

    // if (pkt.type()==NDP)
    //   cout << "H " << pkt.flow().str() << endl;
    Packet* pkt_p = &pkt;
    _enqueued_high.push(pkt_p);
    _queuesize_high += pkt.size();
    if (_logger)
        _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);

    // cout << "BH[ " << _enqueued_low.size() << " " << _enqueued_high.size() << " ]" << endl;

    if (_serv == QUEUE_INVALID) {
        beginService();
    }
}

void CompositeQueue::rememberTrimmedPfldProbe(const Packet& pkt) {
    if (!_coalesce_trimmed_pfld_probe || pkt.type() != UECDATA || pkt.header_only()) {
        return;
    }
    const auto& data = static_cast<const UecDataPacket&>(pkt);
    if (data.is_probe_packet() || !data.has_paired_pfld_probe()) {
        return;
    }
    const uint64_t key = (static_cast<uint64_t>(pkt.flow_id()) << 32) |
                         static_cast<uint64_t>(data.epsn());
    _trimmed_pfld_probe_keys.insert(key);
}

bool CompositeQueue::consumeTrimmedPfldProbe(const Packet& pkt) {
    const auto& probe = static_cast<const UecDataPacket&>(pkt);
    const uint64_t key = (static_cast<uint64_t>(pkt.flow_id()) << 32) |
                         static_cast<uint64_t>(probe.epsn());
    const auto found = _trimmed_pfld_probe_keys.find(key);
    if (found == _trimmed_pfld_probe_keys.end()) {
        return false;
    }
    _trimmed_pfld_probe_keys.erase(found);
    return true;
}

// Add a hashmap to track flow IDs that have already dropped the specified PSN

mem_b CompositeQueue::queuesize() const {
    return _queuesize_low + _queuesize_high;
}
