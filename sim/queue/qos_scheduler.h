#pragma once

#include <array>
#include <stdexcept>

#include "circular_buffer.h"
#include "shared_queue.h"

/**
 * @brief Abstract base class for QoS (Quality of Service) scheduling algorithms
 *
 * This class defines the interface for implementing different QoS scheduling
 * policies that determine the order in which packets from different QoS classes
 * are processed.
 */
class QosScheduler {
public:
    virtual ~QosScheduler() = default;

    /**
     * @brief Selects the next QoS class to process based on the scheduling policy
     *
     * @param bytes_per_qos Array containing the total bytes queued for each QoS class
     * @param buffer_per_qos Array of circular buffers containing queued packets for each QoS class
     * @return uint8_t The selected QoS class index to process next
     */
    virtual uint8_t selectNextQosClass(
        std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES>& bytes_per_qos,
        std::array<CircularBuffer<Packet*>, SharedQueue::DEFAULT_NUM_QOS_CLASSES>&
            buffer_per_qos) = 0;
};

/**
 * @brief Deficit Weighted Round Robin (DWRR) QoS scheduler implementation
 *
 * DWRR is a scheduling algorithm that provides fair queuing with different weights
 * for different QoS classes. Each class is assigned a quantum (weight) of service.
 * The scheduler maintains a deficit counter that accumulates unused quantum. If
 * a class has no tokens left, it is skipped until the deficit counter is reset.
 */
class DwrrQosScheduler : public QosScheduler {
public:
    /**
     * @brief Constructs a DWRR scheduler with specified weights for each QoS class
     *
     * @param weights Array of weights/quanta for each QoS class. Higher weights
     *               give more bandwidth to that QoS class
     * @param cap_factor Specifies the maximum token count for each QoS class after
     *                   refreshing tokens as _weights * _cap_factor (default 2).
     */
    DwrrQosScheduler(std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES> weights,
                     uint8_t                                                    cap_factor = 2);

    /**
     * @brief Selects the next QoS class to process using the DWRR algorithm
     *
     * This implementation first tries to select a QoS class with the current token
     * counts. If no class can be selected, it refreshes all tokens and tries again.
     *
     * @throws std::runtime_error if no packet can be dequeued even after refreshing tokens,
     *         or if all queues are unexpectedly empty
     */
    uint8_t selectNextQosClass(
        std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES>&                bytes_per_qos,
        std::array<CircularBuffer<Packet*>, SharedQueue::DEFAULT_NUM_QOS_CLASSES>& buffer_per_qos)
        override;

private:
    /// Refreshes the token counts for all QoS classes to their initial weights
    /// This is called when no QoS class can be selected with current token counts,
    /// giving each class a fresh quantum of service.
    void refreshTokens();

    /**
     * @brief Attempts to select the next QoS class that has enough tokens. There has to be at least
     * one packet in one of the QoS classes.
     *
     * Iterates through QoS classes in round-robin order starting after the last
     * chosen class. For each non-empty class, checks if it has enough tokens to
     * send its head packet.
     *
     * If no class can be selected, returns -1. This means that the token counts are
     * insufficient to send any packets but not all queues are empty.
     *
     * @param bytes_per_qos Array containing the total bytes queued for each QoS class
     * @param buffer_per_qos Array of circular buffers containing queued packets for each QoS class
     * @return int Selected QoS class index, or -1 if no class can be selected with current tokens
     * @throws std::runtime_error if all queues are empty when they shouldn't be
     */
    int trySelectNextQosClass(
        std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES>&                bytes_per_qos,
        std::array<CircularBuffer<Packet*>, SharedQueue::DEFAULT_NUM_QOS_CLASSES>& buffer_per_qos);

    /// Current token count (remaining quantum) for each QoS class.
    std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES> _current_tokens;

    /// Initial weight/quantum assigned to each QoS class.
    std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES> _weights;

    /// Index of the last QoS class that was selected.
    uint8_t _last_qos_chosen;

    /// Specifies the maximum token count for each QoS class after refreshing tokens as
    /// _weights * _cap_factor.
    uint8_t _cap_factor;
};
