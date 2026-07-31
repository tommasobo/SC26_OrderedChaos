#include "qos_scheduler.h"

#include <array>
#include <stdexcept>

#include "circular_buffer.h"
#include "switch_buffer.h"

DwrrQosScheduler::DwrrQosScheduler(
    std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES> weights, uint8_t cap_factor)
    : _last_qos_chosen(SharedQueue::DEFAULT_NUM_QOS_CLASSES - 1), _cap_factor(cap_factor) {
    _current_tokens = weights;
    _weights        = weights;
}

uint8_t DwrrQosScheduler::selectNextQosClass(
    std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES>&                bytes_per_qos,
    std::array<CircularBuffer<Packet*>, SharedQueue::DEFAULT_NUM_QOS_CLASSES>& buffer_per_qos) {
    int qos = trySelectNextQosClass(bytes_per_qos, buffer_per_qos);
    if (qos == -1) {
        // There has to be at least one packet to dequeue after refreshing tokens since the
        // queue is not empty.
        refreshTokens();
        qos = trySelectNextQosClass(bytes_per_qos, buffer_per_qos);
        if (qos == -1) {
            throw std::runtime_error("Even after refreshing tokens, no packet can be dequeued.");
        }
    }
    return qos;
}

void DwrrQosScheduler::refreshTokens() {
    // Refresh tokens for all QoS classes.
    for (int i = 0; i < SharedQueue::DEFAULT_NUM_QOS_CLASSES; ++i) {
        uint32_t max_tokens = _weights[i] * _cap_factor;
        _current_tokens[i]  = std::min(_current_tokens[i] + _weights[i], max_tokens);
    }
}

int DwrrQosScheduler::trySelectNextQosClass(
    std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES>&                bytes_per_qos,
    std::array<CircularBuffer<Packet*>, SharedQueue::DEFAULT_NUM_QOS_CLASSES>& buffer_per_qos) {
    bool all_queues_empty = true;
    for (int i = 0; i < SharedQueue::DEFAULT_NUM_QOS_CLASSES; ++i) {
        _last_qos_chosen = (_last_qos_chosen + 1) % SharedQueue::DEFAULT_NUM_QOS_CLASSES;
        if (bytes_per_qos[_last_qos_chosen] > 0) {
            all_queues_empty = false;
            Packet* pkt      = buffer_per_qos[_last_qos_chosen].back();
            if (_current_tokens[_last_qos_chosen] >= pkt->size()) {
                _current_tokens[_last_qos_chosen] -= pkt->size();
                return _last_qos_chosen;
            }
        }
    }
    if (all_queues_empty) {
        throw std::runtime_error("All queues are empty.");
    }
    return -1;
}