// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef COMPOSITE_QUEUE_H
#define COMPOSITE_QUEUE_H

/*
 * A composite queue that transforms packets into headers when there is no space and services
 * headers with priority.
 */

#define QUEUE_INVALID 0
#define QUEUE_LOW 1
#define QUEUE_HIGH 2

#include <cassert>
#include <list>
#include <unordered_map>
#include <unordered_set>

#include "event_source.h"
#include "eventlist.h"
#include "helpers.h"
#include "logger_types.h"
#include "metric.h"
#include "packet.h"
#include "packet_flow.h"
#include "queue.h"
#include "types.h"

class CompositeQueue : public Queue {
public:
    CompositeQueue(linkspeed_bps bitrate,
                   mem_b         maxsize,
                   EventList&    eventlist,
                   QueueLogger*  logger,
                   uint16_t      trim_size,
                   bool          disable_trim            = false,
                   bool          low_priority_trim       = false,
                   bool          no_droping_low_header   = false,
                   double        switch_random_drop_prob = 0);
    virtual void receivePacket(Packet& pkt);
    virtual void doNextEvent();
    // should really be private, but loggers want to see
    mem_b _queuesize_low, _queuesize_high;

    mem_b reservedProbeBytes() const { return _reserved_probe_bytes; }

    mem_b regularLowQueueBytes() const {
        assert(_queuesize_low >= _reserved_probe_bytes);
        return _queuesize_low - _reserved_probe_bytes;
    }

    int num_headers() const { return _num_headers; }

    int num_packets() const { return _num_packets; }

    int num_stripped() const { return _num_stripped; }

    int num_bounced() const { return _num_bounced; }

    int num_acks() const { return _num_acks; }

    int num_nacks() const { return _num_nacks; }

    int num_pulls() const { return _num_pulls; }

    virtual mem_b queuesize() const;

    virtual void setName(const string& name) {
        Logged::setName(name);
        _nodename += name;
        if (name.rfind("LS0->US", 0) == 0) {  // Avoid registering too many metrics
            _monitorQueue     = true;
            _queue_enq_metric = DataCollector::RegisterTimeseriesMetric(
                "queueEnq_" + name,
                {"queueSizeBytes", "packetSizeBytes", "packetType", "ev", "dst"});
            _queue_deq_metric = DataCollector::RegisterTimeseriesMetric(
                "queueDeq_" + name,
                {"queueSizeBytes", "packetSizeBytes", "packetType", "ev", "dst"});
        }
    }

    void setRTS(bool return_to_sender) { _return_to_sender = return_to_sender; }

    virtual const string& nodename() { return _nodename; }

    void set_ecn_threshold(mem_b ecn_thresh) {
        _ecn_minthresh = ecn_thresh;
        _ecn_maxthresh = ecn_thresh;
    }

    void set_ecn_thresholds(mem_b min_thresh, mem_b max_thresh) {
        _ecn_minthresh = min_thresh;
        _ecn_maxthresh = max_thresh;
        if (_queue_id == 2)
            cout << "queue_id " << _queue_id << " ecn_low " << _ecn_minthresh << " ecn_high "
                 << _ecn_maxthresh << endl;
    }

    int _num_packets;
    int _num_headers;  // only includes data packets stripped to headers, not acks or nacks
    int _num_acks;
    int _num_nacks;
    int _num_pulls;
    int _num_stripped;  // count of packets we stripped
    int _num_bounced;   // count of packets we bounced

    int _total_drops = 1;  // total number of packets dropped by this queue
    
    static int _fail_psn_num;  // number of packets that fail to be sent (for debugging)
    static bool _probe_high_priority;  // if true, TLP probes skip random/overflow drops
    static bool _coalesce_trimmed_pfld_probe;
    static uint64_t _coalesced_pfld_probe_count;

protected:
    // Mechanism
    void beginService();     // start serving the item at the head of the queue
    void completeService();  // wrap up serving the item at the head of the queue
    bool decide_ECN();

    bool   _disable_trim;
    bool   _low_priority_trim;
    bool   _no_droping_low_header;
    double _switch_random_drop_prob;
    bool _never_dropped = true;  // used to avoid dropping the first packet in the queue

    int _serv;
    int _ratio_high, _ratio_low, _crt;
    // below minthresh, 0% marking, between minthresh and maxthresh
    // increasing random mark propbability, abve maxthresh, 100%
    // marking.
    mem_b _ecn_minthresh;
    mem_b _ecn_maxthresh;
    mem_b _reserved_probe_bytes;

    uint16_t _trim_size;
    std::unordered_map<uint32_t, bool> dropped_flows; // Key: flow ID, Value: true if PSN dropped
    std::unordered_set<uint64_t> _trimmed_pfld_probe_keys;

    void rememberTrimmedPfldProbe(const Packet& pkt);
    bool consumeTrimmedPfldProbe(const Packet& pkt);

    bool _return_to_sender;

    int                     _queue_id;
    CircularBuffer<Packet*> _enqueued_low;
    CircularBuffer<Packet*> _enqueued_high;
};

#endif
