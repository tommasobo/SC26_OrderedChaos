// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef _SHARED_QUEUE_H
#define _SHARED_QUEUE_H

#include <array>
#include <list>
#include <memory>
#include <queue>

#include "data_collector.h"
#include "eventlist.h"
#include "logger_types.h"
#include "queue.h"

class Switch;
class QosScheduler;

/**
 * @class SharedQueue
 * @brief A queue implementation that combines QoS support, ECN marking, and DWRR scheduling.
 *
 * This queue models a switch output port. A switch would have one QosSwitchBuffer instance for the
 * whole switch but per-output-switch-port SharedQueue instances. Buffer management happens at the
 * switch level, but SharedQueue keeps track of the port-level packets and occupancy details for
 * things like (1) packet scheduling and (2) ECN marking.
 *
 * Number of QoS classes. QoS classes are indexed from 0 to NUM_QOS_CLASSES - 1.
 */
class SharedQueue : public Queue {
public:
    static constexpr uint8_t DEFAULT_NUM_QOS_CLASSES = 3;

    /// ECN configuration for each QoS class.
    struct EcnConfig {
        bool     enabled                   = false;
        uint32_t min_threshold             = 0;
        uint32_t max_threshold             = 0;
        uint32_t p_max_percent_probability = 100;
    };

    struct TrimConfig {
        bool     enabled   = true;
        uint32_t trim_size = 64;
    };

    /**
     * @brief Constructs the SharedQueue.
     * @param bitrate The link speed in bits per second.
     * @param alpha Controls the buffer usage threshold.
     * @param eventlist Reference to the main EventList.
     * @param logger QueueLogger to log events (optional if nullptr).
     */
    SharedQueue(linkspeed_bps                                  bitrate,
                EventList&                                     eventlist,
                QueueLogger*                                   logger,
                std::array<EcnConfig, DEFAULT_NUM_QOS_CLASSES> ecn_config,
                std::array<uint32_t, DEFAULT_NUM_QOS_CLASSES>  dwrr_scheduler_weights,
                std::array<double, DEFAULT_NUM_QOS_CLASSES>    alpha,
                TrimConfig                                     trim_config);

    /// Receives a packet and may drop it if it exceeds the threshold.
    void receivePacket(Packet& pkt) override;

    /// Handles the next event with this queue as the source. Just calls completeService().
    void doNextEvent() override;

    void setSwitch(Switch* sw) override;

    void setName(const std::string& name) override;

    /// @warning For testing. Do not use in non-test code.
    void setTrimConfigForTesting(TrimConfig trim_config);

private:
    /// Begins servicing (dequeue) for the head packet.
    void beginService() override;

    /// Completes servicing (dequeue) for the head packet.
    void completeService() override;

    /// Determines if the packet can be admitted to the queue or not.
    bool canAdmit(Packet& pkt);

    /// Drops the packet and logs the event.
    void dropPacket(Packet& pkt);

    /// Updates the queue state due to dequeuing the packet.
    Packet* dequeuePacket(uint8_t qos);

    /// Puts the admitted packet into the appropriate queue based on its QoS class and updates
    /// state.
    void enqueuePacket(Packet& pkt);

    /// Determines if the qos class that is being dequeued should be marked with ECN.
    bool shouldEcnMark(uint8_t qos);

    /// Determines if the packet can be admitted to the queue or not based on the packet qos
    /// occupancy, the qos class alpha value, and the remaining switch buffer size.
    bool canStorePacket(Packet& pkt);

    void logEnqueueStats(Packet& pkt);
    void logDequeueStats(Packet& pkt);
    void logDropStats(Packet& pkt, bool trimmed);
    void logTrimStats(Packet& pkt);

    struct {
        TimeSeriesMetric* enqueue_stats = nullptr;
        TimeSeriesMetric* dequeue_stats = nullptr;
        TimeSeriesMetric* drop_stats    = nullptr;
        TimeSeriesMetric* trim_stats    = nullptr;
    } _metrics;

    /// @name QoS and DWRR variables
    /// These are private attributes related to QoS (Quality of Service) and DWRR (Deficit Weighted
    /// Round Robin).
    /// @{
    /// Buffer holding packets for each QoS class
    std::array<CircularBuffer<Packet*>, DEFAULT_NUM_QOS_CLASSES> _buffer_per_qos;
    /// Total number of bytes queued for each QoS class
    std::array<uint32_t, DEFAULT_NUM_QOS_CLASSES> _bytes_queued_per_qos;
    /// The alpha value for each QoS class. The larger the alpha value, the more buffer space is
    /// allocated to the QoS class.
    std::array<double, DEFAULT_NUM_QOS_CLASSES> _alpha;

    /// Scheduler to determine which QoS class to dequeue next
    std::unique_ptr<QosScheduler> _qos_scheduler;
    /// QoS class that was chosen to get dequeued next
    uint8_t _qos_to_dequeue_next;
    /// @}

    /// ECN related configuration for each QoS class.
    std::array<EcnConfig, DEFAULT_NUM_QOS_CLASSES> _ecn_config;

    /// Trim related configuration for the whole queue(port).
    TrimConfig _trim_config;
};

#endif  // _SHARED_QUEUE_H
