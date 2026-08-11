// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "callback_pipe.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "eventlist.h"
#include "packet_flow.h"

constexpr uint32_t PIPE_DELAY_PICOSECONDS = 10000000;  // 10us
constexpr uint32_t PACKET_SIZE            = 1000;

class MockPacket : public Packet {
public:
    MockPacket(uint32_t size) {
        _size = size;
        _flow = new PacketFlow(nullptr);
    }

    ~MockPacket() { delete _flow; }

    PktPriority priority() const override { return Packet::PRIO_LO; }

    MOCK_METHOD(PacketSink*, sendOn, (), (override));
    MOCK_METHOD(PacketSink*, currentHop, (), (override));
};

class MockPacketSink : public PacketSink {
public:
    const string& nodename() override { return _nodename; }

    MOCK_METHOD(void, receivePacket, (Packet&), (override));

private:
    std::string _nodename = "mockpacketsink";
};

class CallbackPipeTest : public ::testing::Test {
protected:
    std::unique_ptr<CallbackPipe>   pipe_;
    std::unique_ptr<EventList>      eventlist_;
    std::unique_ptr<MockPacketSink> callback_sink_;

    virtual void SetUp() {
        eventlist_ = std::make_unique<EventList>();
        // Make end time large enough to avoid early termination
        eventlist_->setEndtime(timeFromSec(100));

        callback_sink_ = std::make_unique<MockPacketSink>();

        // Create pipe with 10us delay and callback sink
        pipe_ = std::make_unique<CallbackPipe>(
            PIPE_DELAY_PICOSECONDS, *eventlist_, callback_sink_.get());
    }

    virtual void TearDown() {}
};

TEST_F(CallbackPipeTest, ConstructorSetsName) {
    EXPECT_EQ(pipe_->nodename(), "callbackpipe(10us)");
}

TEST_F(CallbackPipeTest, PacketDeliveredToCallback) {
    std::unique_ptr<MockPacket> pkt = std::make_unique<MockPacket>(PACKET_SIZE);

    // Packet should be delivered to callback sink after the pipe delay
    EXPECT_CALL(*callback_sink_, receivePacket(testing::Ref(*pkt))).Times(1);

    pipe_->receivePacket(*pkt);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(eventlist_->now(), PIPE_DELAY_PICOSECONDS);
}

TEST_F(CallbackPipeTest, PacketDeliveredToCurrentHopWhenNoCallback) {
    // Create pipe with null callback
    pipe_ = std::make_unique<CallbackPipe>(PIPE_DELAY_PICOSECONDS, *eventlist_, nullptr);

    std::unique_ptr<MockPacket> pkt = std::make_unique<MockPacket>(PACKET_SIZE);
    MockPacketSink              current_hop;

    // Packet should be delivered back to current hop
    EXPECT_CALL(*pkt, currentHop()).WillOnce(testing::Return(&current_hop));
    EXPECT_CALL(current_hop, receivePacket(testing::Ref(*pkt))).Times(1);

    pipe_->receivePacket(*pkt);
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(eventlist_->now(), PIPE_DELAY_PICOSECONDS);
}

TEST_F(CallbackPipeTest, MultiplePacketsDeliveredInOrder) {
    std::unique_ptr<MockPacket> pkt1 = std::make_unique<MockPacket>(PACKET_SIZE);
    std::unique_ptr<MockPacket> pkt2 = std::make_unique<MockPacket>(PACKET_SIZE);

    testing::Sequence seq;
    EXPECT_CALL(*callback_sink_, receivePacket(testing::Ref(*pkt1))).Times(1).InSequence(seq);
    EXPECT_CALL(*callback_sink_, receivePacket(testing::Ref(*pkt2))).Times(1).InSequence(seq);

    pipe_->receivePacket(*pkt1);
    pipe_->receivePacket(*pkt2);

    // First packet should be delivered after pipe delay
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(eventlist_->now(), PIPE_DELAY_PICOSECONDS);

    // Second packet should be delivered immediately after first
    EXPECT_TRUE(eventlist_->doNextEvent());
    EXPECT_EQ(eventlist_->now(), PIPE_DELAY_PICOSECONDS);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}