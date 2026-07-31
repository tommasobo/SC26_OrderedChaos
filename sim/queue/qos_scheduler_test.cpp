#include "qos_scheduler.h"

#include <gtest/gtest.h>

#include <array>

#include "circular_buffer.h"
#include "shared_queue.h"

class DummyPacket : public Packet {
public:
    DummyPacket(uint32_t size) { _size = size; }

    uint32_t size() const { return _size; }

    PktPriority priority() const override { return PktPriority::PRIO_LO; }
};

class DwrrQosSchedulerTest : public ::testing::Test {
protected:
    virtual void SetUp() {
        std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES> weights = {10, 20, 30};
        _scheduler = std::make_unique<DwrrQosScheduler>(weights);
    }

    virtual void TearDown() { cleanUpQosQueues(); }

    void initQosQueues(std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES> bytes_per_qos) {
        cleanUpQosQueues();
        _bytes_per_qos = {0, 0, 0};
        for (int i = 0; i < SharedQueue::DEFAULT_NUM_QOS_CLASSES; ++i) {
            if (bytes_per_qos[i] > 0) {
                Packet* pkt = new DummyPacket(bytes_per_qos[i]);
                _buffer_per_qos[i].push(pkt);
                _bytes_per_qos[i] += bytes_per_qos[i];
            }
        }
    }

    void cleanUpQosQueues() {
        for (int i = 0; i < SharedQueue::DEFAULT_NUM_QOS_CLASSES; ++i) {
            while (!_buffer_per_qos[i].empty()) {
                delete _buffer_per_qos[i].pop();
            }
        }
    }

    std::unique_ptr<DwrrQosScheduler>                                         _scheduler;
    std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES>                _weights;
    std::array<uint32_t, SharedQueue::DEFAULT_NUM_QOS_CLASSES>                _bytes_per_qos;
    std::array<CircularBuffer<Packet*>, SharedQueue::DEFAULT_NUM_QOS_CLASSES> _buffer_per_qos;
};

TEST_F(DwrrQosSchedulerTest, SelectNextQosClass) {
    // Set up the qos queues.
    // All queues have 1 packet each of size 10 bytes and all have enough tokens to send the packet.
    initQosQueues({10, 10, 10});

    // The scheduler should select the first QoS class starting from 0 that has
    // a packet to send and enough tokens to send the packet.
    uint8_t qos_class = _scheduler->selectNextQosClass(_bytes_per_qos, _buffer_per_qos);
    EXPECT_EQ(qos_class, 0);
}

TEST_F(DwrrQosSchedulerTest, SelectNextQosClassWithNoTokens) {
    // Set up the qos queues with the following bytes:
    // All queues have 1 packet each of size 10 bytes but qos 0 has no tokens and the other two
    // have enough tokens to send the packet.
    initQosQueues({0, 10, 10});

    // The scheduler should select the first QoS class starting from 0 that has
    // a packet to send and enough tokens to send the packet.
    uint8_t qos_class = _scheduler->selectNextQosClass(_bytes_per_qos, _buffer_per_qos);
    EXPECT_EQ(qos_class, 1);
}

TEST_F(DwrrQosSchedulerTest, EmptyQosQueues) {
    // Set up the qos queues with the following bytes:
    // All queues have 0 bytes.
    initQosQueues({0, 0, 0});

    // The scheduler should throw an exception because there are no packets to send.
    EXPECT_THROW(_scheduler->selectNextQosClass(_bytes_per_qos, _buffer_per_qos),
                 std::runtime_error);
}

TEST_F(DwrrQosSchedulerTest, CheckHighestPriorityQosClassAndRefreshTokens) {
    // Set up the qos queues with the following bytes:
    // All queues have 1 packet, qos 0 has 15 bytes, qos 1 has 10 bytes, and qos 2 has 10 bytes.
    initQosQueues({15, 10, 10});

    // The scheduler should select the first QoS class starting from 0 that has
    // a packet to send and enough tokens to send the packet. QoS 0 does not have enough
    // tokens (10) to send the packet (15 bytes).
    uint8_t qos_class = _scheduler->selectNextQosClass(_bytes_per_qos, _buffer_per_qos);
    // Expected tokens: [10, 10, 30].
    EXPECT_EQ(qos_class, 1);

    qos_class = _scheduler->selectNextQosClass(_bytes_per_qos, _buffer_per_qos);
    // Expected tokens: [10, 10, 20].
    EXPECT_EQ(qos_class, 2);

    qos_class = _scheduler->selectNextQosClass(_bytes_per_qos, _buffer_per_qos);
    // Expected tokens: [10, 0, 20].
    EXPECT_EQ(qos_class, 1);

    qos_class = _scheduler->selectNextQosClass(_bytes_per_qos, _buffer_per_qos);
    // Expected tokens: [10, 0, 10].
    EXPECT_EQ(qos_class, 2);

    qos_class = _scheduler->selectNextQosClass(_bytes_per_qos, _buffer_per_qos);
    // Expected tokens: [10, 0, 0].
    EXPECT_EQ(qos_class, 2);

    // Here the scheduler should refresh the tokens and select the first QoS class again.
    // After refreshing, the tokens should be [20, 20, 30].
    qos_class = _scheduler->selectNextQosClass(_bytes_per_qos, _buffer_per_qos);
    // QoS 0 now has enough tokens to send the 15 byte packet.
    EXPECT_EQ(qos_class, 0);

    // .. and so on.
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}