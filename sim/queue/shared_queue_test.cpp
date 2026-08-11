// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "shared_queue.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "eventlist.h"
#include "helpers.h"
#include "loggers.h"
#include "packet.h"
#include "qos_scheduler.h"
#include "switch.h"
#include "switch_buffer.h"

using json = nlohmann::json;

// Required configuration for setting up the SharedQueue.
const SharedQueue::EcnConfig QOS_0_ECN_CONFIG = {
    .enabled = true, .min_threshold = 3000, .max_threshold = 4000, .p_max_percent_probability = 80};
const SharedQueue::EcnConfig                QOS_1_ECN_CONFIG = {.enabled                   = true,
                                                                .min_threshold             = 1000,
                                                                .max_threshold             = 1500,
                                                                .p_max_percent_probability = 100};
const SharedQueue::EcnConfig                QOS_2_ECN_CONFIG = {.enabled                   = false,
                                                                .min_threshold             = 1000,
                                                                .max_threshold             = 1500,
                                                                .p_max_percent_probability = 100};
const std::array<SharedQueue::EcnConfig, 3> ECN_CONFIG       = {
    QOS_0_ECN_CONFIG, QOS_1_ECN_CONFIG, QOS_2_ECN_CONFIG};
const SharedQueue::TrimConfig TRIM_CONFIG            = {.enabled = true, .trim_size = 64};
const std::array<uint32_t, 3> DWRR_SCHEDULER_WEIGHTS = {10000, 20000, 30000};
constexpr uint32_t            SWITCH_BUFFER_SIZE     = 10000;
const std::array<double, 3>   SWITCH_BUFFER_ALPHA    = {0.5, 0.66, 0.75};

json getTestDataCollectorConfig() {
    json config;
    config["output_location"] = "stdout";
    config["filters"]         = json::array();
    config["filters"].push_back(json::object({{"regex", ".*"}, {"enabled", false}}));
    return config;
}

class MockPacket : public Packet {
public:
    MockPacket(uint32_t size, PktPriority priority) : Packet(), _priority(priority) {
        _size = size;
        _flow = new PacketFlow(nullptr);
    }

    ~MockPacket() { delete _flow; }

    MOCK_METHOD(PacketSink*, sendOn, (), (override));

    PktPriority priority() const override { return _priority; }

private:
    PktPriority _priority;
};

// Test fixtures are not run in parallel. They are run in the same process but in sequence to ensure
// test isolation.
class SharedQueueTest : public ::testing::Test {
protected:
    std::unique_ptr<Switch>      sw_;
    std::unique_ptr<SharedQueue> queue_;
    std::unique_ptr<EventList>   eventlist_;

    virtual void SetUp() {
        eventlist_ = make_unique<EventList>();
        // Make end time large enough to avoid early termination if needed.
        eventlist_->setEndtime(timeFromSec(100));

        // Initialize the switch buffer.
        std::unique_ptr<SwitchBuffer> buffer = make_unique<SwitchBuffer>(SWITCH_BUFFER_SIZE);
        sw_ = make_unique<Switch>(*eventlist_, "switch", std::move(buffer));

        // Initialize the SharedQueue.
        initQueue(speedFromMbps(10000.0),
                  ECN_CONFIG,
                  DWRR_SCHEDULER_WEIGHTS,
                  SWITCH_BUFFER_ALPHA,
                  TRIM_CONFIG);
    }

    void initQueue(linkspeed_bps                         speed,
                   std::array<SharedQueue::EcnConfig, 3> ecn_config,
                   std::array<uint32_t, 3>               dwrr_scheduler_weights,
                   std::array<double, 3>                 switch_buffer_alpha,
                   SharedQueue::TrimConfig               trim_config) {
        queue_ = make_unique<SharedQueue>(speed,
                                          *eventlist_,
                                          nullptr,
                                          ecn_config,
                                          dwrr_scheduler_weights,
                                          switch_buffer_alpha,
                                          trim_config);
        queue_->setName(testing::UnitTest::GetInstance()->current_test_info()->name());
        // Integrate the queue with the switch.  SharedQueue needs to know about the switch to get
        // the switch buffer which also needs to be non-null.
        queue_->setSwitch(sw_.get());
    }

    virtual void TearDown() {}
};

TEST_F(SharedQueueTest, ReceivePacketSuccess) {
    std::unique_ptr<MockPacket> pkt = std::make_unique<MockPacket>(5000, Packet::PRIO_LO);
    queue_->receivePacket(*pkt);
    // Packet should be enqueued successfully.
    EXPECT_EQ(queue_->queuesize(), pkt->size());

    EXPECT_CALL(*pkt, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);
    EXPECT_EQ(eventlist_->now(), queue_->drainTime(pkt.get()));
}

TEST_F(SharedQueueTest, PacketDropWhenExceedLimit) {
    // Trimming is disabled.
    SharedQueue::TrimConfig trim_config = {.enabled = false, .trim_size = 64};
    queue_->setTrimConfigForTesting(trim_config);
    std::unique_ptr<MockPacket> pkt = std::make_unique<MockPacket>(5001, Packet::PRIO_LO);
    queue_->receivePacket(*pkt);
    // Packet should be dropped because it exceeds the threshold.
    EXPECT_EQ(queue_->queuesize(), 0);

    EXPECT_CALL(*pkt, sendOn()).Times(0);
    EXPECT_FALSE(eventlist_->doNextEvent());
}

TEST_F(SharedQueueTest, PacketTrimmedWhenExceedLimit) {
    // Trimming is enabled.
    SharedQueue::TrimConfig trim_config = {.enabled = true, .trim_size = 64};
    queue_->setTrimConfigForTesting(trim_config);
    std::unique_ptr<MockPacket> pkt = std::make_unique<MockPacket>(5001, Packet::PRIO_LO);
    queue_->receivePacket(*pkt);
    // Packet should be trimmed to the trim size.
    EXPECT_EQ(queue_->queuesize(), TRIM_CONFIG.trim_size);

    EXPECT_CALL(*pkt, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);
}

TEST_F(SharedQueueTest, ShouldNotEcnMarkQos0BelowMinThreshold) {
    std::unique_ptr<MockPacket> pktFirst =
        std::make_unique<MockPacket>(QOS_0_ECN_CONFIG.min_threshold, Packet::PRIO_LO);

    queue_->receivePacket(*pktFirst);

    EXPECT_CALL(*pktFirst, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);
    // ECN should not be marked.
    EXPECT_FALSE(pktFirst->flags() & ECN_CE);
}

TEST_F(SharedQueueTest, ShouldEcnMarkQos0AboveMaxThreshold) {
    std::unique_ptr<MockPacket> pktFirst = std::make_unique<MockPacket>(100, Packet::PRIO_LO);

    std::unique_ptr<MockPacket> pktSecond =
        std::make_unique<MockPacket>(QOS_0_ECN_CONFIG.max_threshold, Packet::PRIO_LO);

    queue_->receivePacket(*pktFirst);
    queue_->receivePacket(*pktSecond);

    EXPECT_CALL(*pktFirst, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), QOS_0_ECN_CONFIG.max_threshold);  // larger than max threshold
    // ECN should be marked for the first packet which is serviced first.
    EXPECT_TRUE(pktFirst->flags() & ECN_CE);
}

TEST_F(SharedQueueTest, ShouldEcnMarkQos0BetweenMinAndMaxThreshold) {
    std::unique_ptr<MockPacket> pktFirst;
    std::unique_ptr<MockPacket> pktSecond;

    // Queue size is exactly at (min_threshold+max_threshold)/2 --> ECN should be marked with
    // probability p_max_percent_probability/2.
    uint32_t mid_threshold = (QOS_0_ECN_CONFIG.min_threshold + QOS_0_ECN_CONFIG.max_threshold) / 2;
    uint32_t total_ecn_marked = 0;
    uint32_t total_tries      = 10000;
    for (uint32_t i = 0; i < total_tries; i++) {
        pktFirst  = std::make_unique<MockPacket>(100, Packet::PRIO_LO);
        pktSecond = std::make_unique<MockPacket>(mid_threshold, Packet::PRIO_LO);

        queue_->receivePacket(*pktFirst);
        queue_->receivePacket(*pktSecond);

        EXPECT_CALL(*pktFirst, sendOn()).Times(1);
        EXPECT_TRUE(eventlist_->doNextEvent());
        EXPECT_CALL(*pktSecond, sendOn()).Times(1);
        EXPECT_TRUE(eventlist_->doNextEvent());

        if (pktFirst->flags() & ECN_CE) {
            total_ecn_marked++;
        }
    }
    EXPECT_NEAR(100.0 * total_ecn_marked / total_tries,
                QOS_0_ECN_CONFIG.p_max_percent_probability / 2.0,
                1.0);
    // Test case 4: ECN disabled (using QoS 2 which has ECN disabled from SetUp())
}

TEST_F(SharedQueueTest, ShouldNotEcnMarkIfDisabled) {
    // QoS 2 has ECN disabled.
    const Packet::PktPriority   QOS_TO_TEST = Packet::PRIO_HI;
    std::unique_ptr<MockPacket> pktFirst    = std::make_unique<MockPacket>(100, QOS_TO_TEST);

    std::unique_ptr<MockPacket> pktSecond =
        std::make_unique<MockPacket>(QOS_2_ECN_CONFIG.max_threshold, QOS_TO_TEST);

    queue_->receivePacket(*pktFirst);
    queue_->receivePacket(*pktSecond);

    EXPECT_CALL(*pktFirst, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), QOS_2_ECN_CONFIG.max_threshold);  // larger than max threshold
    // ECN should not be marked because ECN is disabled for QoS 2.
    EXPECT_FALSE(pktFirst->flags() & ECN_CE);

    EXPECT_CALL(*pktSecond, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);
    // ECN should also not be marked for the second packet.
    EXPECT_FALSE(pktSecond->flags() & ECN_CE);
}

TEST_F(SharedQueueTest, UseAndFreeBufferQos0Success) {
    // For QoS 0 with alpha=1, limit is (1 * 10000) / (1 + 1) = 5000
    std::unique_ptr<MockPacket> pktQos0 = std::make_unique<MockPacket>(4000, Packet::PRIO_LO);
    queue_->receivePacket(*pktQos0);
    EXPECT_EQ(queue_->queuesize(), pktQos0->size());
    EXPECT_CALL(*pktQos0, sendOn()).Times(1);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(queue_->queuesize(), 0);
}

TEST_F(SharedQueueTest, UseBufferQos0Failure) {
    // Disable trimming.
    SharedQueue::TrimConfig trim_config = {.enabled = false, .trim_size = 64};
    queue_->setTrimConfigForTesting(trim_config);
    // For QoS 0 with alpha=1, limit is (1 * 10000) / (1 + 1) = 5000
    std::unique_ptr<MockPacket> pktQos0 = std::make_unique<MockPacket>(6000, Packet::PRIO_LO);
    queue_->receivePacket(*pktQos0);
    EXPECT_EQ(queue_->queuesize(), 0);
    EXPECT_CALL(*pktQos0, sendOn()).Times(0);
    EXPECT_FALSE(eventlist_->doNextEvent());
}

TEST_F(SharedQueueTest, BufferSharingAccurate) {
    // Disable trimming.
    SharedQueue::TrimConfig trim_config = {.enabled = false, .trim_size = 64};
    queue_->setTrimConfigForTesting(trim_config);
    // For QoS 0 with alpha=1, initial limit is (1 * 10000) / (1 + 1) = 5000
    // For QoS 2 with alpha=4, initial limit is (4 * 10000) / (1 + 4) = 8000
    std::unique_ptr<MockPacket> pktQos0 = std::make_unique<MockPacket>(4000, Packet::PRIO_LO);
    std::unique_ptr<MockPacket> pktQos2 = std::make_unique<MockPacket>(7000, Packet::PRIO_HI);

    queue_->receivePacket(*pktQos2);
    EXPECT_EQ(queue_->queuesize(), pktQos2->size());

    // After using 7000 bytes, remaining buffer is 3000
    // New QoS 0 limit: (1 * 3000) / (1 + 1) = 1500
    // New QoS 2 limit: (4 * 3000) / (1 + 4) = 2400
    // Neither packet should fit now and the buffer should be unchanged.
    queue_->receivePacket(*pktQos0);
    EXPECT_EQ(queue_->queuesize(), pktQos2->size());
    queue_->receivePacket(*pktQos2);
    EXPECT_EQ(queue_->queuesize(), pktQos2->size());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    DataCollector::InitWithJsonObject(getTestDataCollectorConfig());
    return RUN_ALL_TESTS();
}
