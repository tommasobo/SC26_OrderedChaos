// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "route.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packet.h"
#include "packet_flow.h"
#include "pipe.h"

class MockPacketSink : public PacketSink {
public:
    const string& nodename() override { return _nodename; }

    MOCK_METHOD(void, receivePacket, (Packet&), (override));

private:
    std::string _nodename = "mockpacketsink";
};

class RouteTest : public ::testing::Test {
protected:
    std::unique_ptr<Route>          route_;
    std::unique_ptr<EventList>      eventlist_;
    std::unique_ptr<MockPacketSink> sink1_;
    std::unique_ptr<MockPacketSink> sink2_;
    std::unique_ptr<Pipe>           pipe1_;

    virtual void SetUp() {
        eventlist_ = std::make_unique<EventList>();
        route_     = std::make_unique<Route>();
        sink1_     = std::make_unique<MockPacketSink>();
        sink2_     = std::make_unique<MockPacketSink>();
        pipe1_     = std::make_unique<Pipe>(1000, *eventlist_);  // 1ns delay pipe
    }

    virtual void TearDown() {}
};

TEST_F(RouteTest, EmptyRouteHasZeroSize) {
    EXPECT_EQ(route_->size(), 0);
    EXPECT_EQ(route_->hop_count(), 0);
}

TEST_F(RouteTest, PushBackAddsToRoute) {
    route_->push_back(sink1_.get());
    route_->push_back(sink2_.get());
    EXPECT_EQ(route_->size(), 2);
    EXPECT_EQ(route_->at(0), sink1_.get());
    EXPECT_EQ(route_->at(1), sink2_.get());
}

TEST_F(RouteTest, PushFrontAddsToRoute) {
    route_->push_back(sink1_.get());
    route_->push_front(sink2_.get());
    EXPECT_EQ(route_->size(), 2);
    EXPECT_EQ(route_->at(0), sink2_.get());
    EXPECT_EQ(route_->at(1), sink1_.get());
}

TEST_F(RouteTest, PushAtInsertsAtPosition) {
    route_->push_back(sink1_.get());
    route_->push_back(sink2_.get());
    MockPacketSink sink3;
    route_->push_at(&sink3, 1);
    EXPECT_EQ(route_->size(), 3);
    EXPECT_EQ(route_->at(0), sink1_.get());
    EXPECT_EQ(route_->at(1), &sink3);
    EXPECT_EQ(route_->at(2), sink2_.get());
}

TEST_F(RouteTest, HopCountOnlyIncrementsForPipes) {
    route_->push_back(sink1_.get());  // Not a pipe, shouldn't increment hop count
    EXPECT_EQ(route_->hop_count(), 0);

    route_->push_back(pipe1_.get());  // Is a pipe, should increment hop count
    EXPECT_EQ(route_->hop_count(), 1);

    route_->push_back(sink2_.get());  // Not a pipe, shouldn't increment hop count
    EXPECT_EQ(route_->hop_count(), 1);
}

TEST_F(RouteTest, ThrowsOnNullSinkPushBack) {
    EXPECT_THROW(route_->push_back(nullptr), std::runtime_error);
}

TEST_F(RouteTest, ThrowsOnNullSinkPushFront) {
    EXPECT_THROW(route_->push_front(nullptr), std::runtime_error);
}

TEST_F(RouteTest, ThrowsOnNullSinkPushAt) {
    route_->push_back(sink1_.get());
    EXPECT_THROW(route_->push_at(nullptr, 0), std::runtime_error);
}

TEST_F(RouteTest, ReverseRouteConfiguration) {
    auto reverse_route = std::make_unique<Route>();
    route_->set_reverse(reverse_route.get());
    EXPECT_EQ(route_->reverse(), reverse_route.get());
}

TEST_F(RouteTest, IteratorAccess) {
    route_->push_back(sink1_.get());
    route_->push_back(sink2_.get());

    std::vector<PacketSink*> expected_sinks = {sink1_.get(), sink2_.get()};
    std::vector<PacketSink*> actual_sinks(route_->begin(), route_->end());

    EXPECT_EQ(actual_sinks, expected_sinks);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}