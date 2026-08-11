// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "shared_queue.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <iostream>
#include <memory>

#include "data_collector.h"
#include "qos_scheduler.h"
#include "switch.h"
#include "uec_src.h"

static inline void logLegacyReactionDrop(EventList& eventlist, Packet& pkt) {
    if (!UecSrc::_log_reaction_events) {
        return;
    }
    if (pkt.type() != UECDATA || pkt.header_only() || pkt.size() <= 500) {
        return;
    }
    std::cout << "Drop: FlowID " << pkt.flow_id() << " - Packet ID " << pkt.id()
              << " - Time " << eventlist.now() << std::endl;
}

SharedQueue::SharedQueue(linkspeed_bps                                  bitrate,
                         EventList&                                     eventlist,
                         QueueLogger*                                   logger,
                         std::array<EcnConfig, DEFAULT_NUM_QOS_CLASSES> ecn_config,
                         std::array<uint32_t, DEFAULT_NUM_QOS_CLASSES>  dwrr_scheduler_weights,
                         std::array<double, DEFAULT_NUM_QOS_CLASSES>    alpha,
                         TrimConfig                                     trim_config)
    : Queue(bitrate, INT_FAST64_MAX, eventlist, logger),
      _bytes_queued_per_qos{0, 0, 0},
      _qos_scheduler{std::make_unique<DwrrQosScheduler>(dwrr_scheduler_weights)},
      _qos_to_dequeue_next{0},
      _ecn_config{ecn_config},
      _trim_config{trim_config} {
    for (uint8_t qos = 0; qos < DEFAULT_NUM_QOS_CLASSES; qos++) {
        if (alpha[qos] <= 0) {
            throw std::runtime_error("Alpha value must be greater than 0.");
        }
        _alpha[qos] = alpha[qos];
    }
}

void SharedQueue::doNextEvent() {
    completeService();
}

void SharedQueue::setName(const std::string& name) {
    _nodename = name;
}

void SharedQueue::logEnqueueStats(Packet& pkt) {
    if (_metrics.enqueue_stats == nullptr) {
        _metrics.enqueue_stats =
            DataCollector::RegisterTimeseriesMetric("shared_queue_enqueue_stats/" + _nodename,
                                                    {"qos_0_occupancy_bytes",
                                                     "qos_1_occupancy_bytes",
                                                     "qos_2_occupancy_bytes",
                                                     "packet_size",
                                                     "qos_class",
                                                     "packet_type"},
                                                    false);
    }
    if (!_metrics.enqueue_stats->enable()) {
        return;
    }
    _metrics.enqueue_stats->LogData({std::to_string(_bytes_queued_per_qos[0]),
                                     std::to_string(_bytes_queued_per_qos[1]),
                                     std::to_string(_bytes_queued_per_qos[2]),
                                     std::to_string(pkt.size()),
                                     std::to_string(pkt.priority()),
                                     pkt.str()});
}

void SharedQueue::logDequeueStats(Packet& pkt) {
    if (_metrics.dequeue_stats == nullptr) {
        _metrics.dequeue_stats =
            DataCollector::RegisterTimeseriesMetric("shared_queue_dequeue_stats/" + _nodename,
                                                    {"qos_0_occupancy_bytes",
                                                     "qos_1_occupancy_bytes",
                                                     "qos_2_occupancy_bytes",
                                                     "packet_size",
                                                     "qos_class",
                                                     "packet_type"},
                                                    false);
    }
    if (!_metrics.dequeue_stats->enable()) {
        return;
    }
    _metrics.dequeue_stats->LogData({std::to_string(_bytes_queued_per_qos[0]),
                                     std::to_string(_bytes_queued_per_qos[1]),
                                     std::to_string(_bytes_queued_per_qos[2]),
                                     std::to_string(pkt.size()),
                                     std::to_string(pkt.priority()),
                                     pkt.str()});
}

void SharedQueue::logDropStats(Packet& pkt, bool trimmed) {
    if (_metrics.drop_stats == nullptr) {
        _metrics.drop_stats = DataCollector::RegisterTimeseriesMetric(
            "shared_queue_drop_stats/" + _nodename,
            {"packet_size", "qos_class", "packet_type", "was_trimmed_first"},
            false);
    }
    if (!_metrics.drop_stats->enable()) {
        return;
    }
    _metrics.drop_stats->LogData({std::to_string(pkt.size()),
                                  std::to_string(pkt.priority()),
                                  pkt.str(),
                                  std::to_string(trimmed)});
}

void SharedQueue::logTrimStats(Packet& pkt) {
    if (_metrics.trim_stats == nullptr) {
        _metrics.trim_stats =
            DataCollector::RegisterTimeseriesMetric("shared_queue_trim_stats/" + _nodename,
                                                    {"packet_size", "qos_class", "packet_type"},
                                                    false);
    }
    if (!_metrics.trim_stats->enable()) {
        return;
    }
    _metrics.trim_stats->LogData(
        {std::to_string(pkt.size()), std::to_string(pkt.priority()), pkt.str()});
}

void SharedQueue::receivePacket(Packet& pkt) {
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_ARRIVE);
    if (_logger)
        _logger->logQueue(*this, QueueLogger::PKT_ARRIVE, pkt);
    // If the packet doesn't fit in the queue, drop it or trim it.
    if (!canAdmit(pkt)) {
        if (!_trim_config.enabled || pkt.header_only()) {
            // Trimming is disabled or the packet is header-only. Immediately drop the packet.
            dropPacket(pkt);
            logDropStats(pkt, false);
            return;
        }
        // Here: trimming is enabled and the packet is not header-only.
        // Trim the packet and check if it fits in the queue. If it doesn't, drop it. Else,
        // enqueue the trimmed packet.
        logLegacyReactionDrop(eventlist(), pkt);
        pkt.strip_payload(_trim_config.trim_size);
        if (!canAdmit(pkt)) {
            dropPacket(pkt);
            logDropStats(pkt, true);
            return;
        }
        logTrimStats(pkt);
        // The packet fits in the queue after trimming.
        pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_TRIM);
        if (_logger)
            _logger->logQueue(*this, QueueLogger::PKT_TRIM, pkt);
    }
    // Ok to enqueue the packet.
    bool queueWasEmpty = (_queuesize == 0);
    enqueuePacket(pkt);
    if (queueWasEmpty) {
        // Only schedule the dequeue event if the queue was empty because otherwise it is
        // already scheduled.
        beginService();
    }
}

void SharedQueue::setSwitch(Switch* sw) {
    if (sw->getSwitchBuffer() == nullptr) {
        throw std::runtime_error(
            "Switch buffer is not set. SharedQueue expects a shared switch buffer model.");
    }
    BaseQueue::setSwitch(sw);
}

void SharedQueue::enqueuePacket(Packet& pkt) {
    Packet::PktPriority qos = pkt.priority();
    if (qos >= DEFAULT_NUM_QOS_CLASSES) {
        throw std::runtime_error("Invalid QoS class in packet.");
    }
    Packet* pkt_p = &pkt;
    _buffer_per_qos[qos].push(pkt_p);
    _bytes_queued_per_qos[qos] += pkt.size();
    _queuesize += pkt.size();
    _switch->getSwitchBuffer()->useBuffer(pkt_p);
    logEnqueueStats(pkt);
    if (_logger)
        _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);
}

Packet* SharedQueue::dequeuePacket(uint8_t qos) {
    assert(_queuesize > 0);
    Packet* pkt = _buffer_per_qos[qos].pop();
    _bytes_queued_per_qos[qos] -= pkt->size();
    _queuesize -= pkt->size();
    _switch->getSwitchBuffer()->freeBuffer(pkt);
    pkt->flow().logTraffic(*pkt, *this, TrafficLogger::PKT_DEPART);
    if (_logger)
        _logger->logQueue(*this, QueueLogger::PKT_SERVICE, *pkt);
    // Used to compute queue utilization.
    log_packet_send(drainTime(pkt));
    logDequeueStats(*pkt);
    return pkt;
}

void SharedQueue::beginService() {
    _qos_to_dequeue_next =
        _qos_scheduler->selectNextQosClass(_bytes_queued_per_qos, _buffer_per_qos);
    // Schedule the completion of service.
    Packet* pkt = _buffer_per_qos[_qos_to_dequeue_next].back();
    eventlist().sourceIsPendingRel(*this, drainTime(pkt));
}

void SharedQueue::dropPacket(Packet& pkt) {
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_DROP);
    if (_logger)
        _logger->logQueue(*this, QueueLogger::PKT_DROP, pkt);
    logLegacyReactionDrop(eventlist(), pkt);
    pkt.free();
    _num_drops++;
}

void SharedQueue::completeService() {
    // Dequeue the packet based on the selected QoS class.
    Packet* pkt = dequeuePacket(_qos_to_dequeue_next);
    // Mark the packet with ECN if needed after dequeuing.
    if (shouldEcnMark(_qos_to_dequeue_next)) {
        pkt->set_flags(pkt->flags() | ECN_CE);
    }
    // Tell the packet to move on to the next pipe.
    pkt->sendOn();
    if (_queuesize > 0) {
        // Automatically schedule the next dequeue event if there are more packets in the queue.
        beginService();
    }
}

bool SharedQueue::canAdmit(Packet& pkt) {
    return canStorePacket(pkt);
}

bool SharedQueue::shouldEcnMark(uint8_t qos) {
    if (!_ecn_config[qos].enabled) {
        return false;
    }
    // If ECN is enabled, we need to check if the packet should be marked.
    const uint32_t& bytes_in_buffer           = _bytes_queued_per_qos[qos];
    const uint32_t& ecn_max_threshold         = _ecn_config[qos].max_threshold;
    const uint32_t& ecn_min_threshold         = _ecn_config[qos].min_threshold;
    const uint32_t& p_max_percent_probability = _ecn_config[qos].p_max_percent_probability;
    if (bytes_in_buffer >= ecn_max_threshold) {
        return true;
    } else if (bytes_in_buffer <= ecn_min_threshold) {
        return false;
    } else {
        // Random probability of marking based on the buffer size, the max probability, and the
        // min and max thresholds.
        uint32_t random_num = rand() % 100;
        uint32_t compare_to = p_max_percent_probability * (bytes_in_buffer - ecn_min_threshold) /
                              (ecn_max_threshold - ecn_min_threshold);
        return random_num < compare_to;
    }
}

void SharedQueue::setTrimConfigForTesting(TrimConfig trim_config) {
    _trim_config = trim_config;
}

bool SharedQueue::canStorePacket(Packet& pkt) {
    Packet::PktPriority qos = pkt.priority();
    if (qos >= DEFAULT_NUM_QOS_CLASSES) {
        throw std::runtime_error("Invalid QoS class.");
    }
    auto   remaining_buffer_size = _switch->getSwitchBuffer()->getRemainingBufferSize();
    double limit                 = _alpha[qos] * remaining_buffer_size;
    return _bytes_queued_per_qos[qos] + pkt.size() <= limit;
}
