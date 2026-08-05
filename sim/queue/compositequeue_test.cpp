// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "compositequeue.h"

#include <gtest/gtest.h>

#include "eventlist.h"
#include "packet_flow.h"
#include "route.h"
#include "uec_packet.h"

class TestCompositeQueue : public CompositeQueue {
public:
    using CompositeQueue::CompositeQueue;

    size_t lowPacketCount() { return _enqueued_low.size(); }
    size_t highPacketCount() { return _enqueued_high.size(); }
};

class FreePacketSink : public PacketSink {
public:
    void receivePacket(Packet& pkt) override { pkt.free(); }
    const std::string& nodename() override { return _name; }

private:
    std::string _name = "free_packet_sink";
};

TEST(CompositeQueueProbeHeadroom, KeepsProbeUntrimmedInLowPriorityFifo) {
    EventList eventlist;
    EventList::setEndtime(timeFromSec(1));

    constexpr mem_b data_size = 4160;
    TestCompositeQueue queue(speedFromMbps(uint64_t{400000}),
                             data_size,
                             eventlist,
                             nullptr,
                             UecBasePacket::ACKSIZE,
                             false,
                             false,
                             true,
                             0.0);

    PacketFlow flow(nullptr);
    Route route;
    auto* data = UecDataPacket::newpkt(
        flow, route, 1, data_size, UecBasePacket::DATA_PULL, 0);
    auto* probe = UecDataPacket::newpkt(
        flow, route, 1, UecBasePacket::ACKSIZE, UecBasePacket::PROBE, 0);

    queue.receivePacket(*data);
    queue.receivePacket(*probe);

    EXPECT_EQ(queue.lowPacketCount(), 2u);
    EXPECT_EQ(queue.highPacketCount(), 0u);
    EXPECT_FALSE(probe->header_only());
    EXPECT_EQ(queue._queuesize_low, data_size + UecBasePacket::ACKSIZE);
    EXPECT_EQ(queue.reservedProbeBytes(), UecBasePacket::get_ack_size());
    EXPECT_EQ(queue.regularLowQueueBytes(), data_size);

    EventList::reset();
    data->free();
    probe->free();
}

TEST(CompositeQueueProbeCoalescing, SuppressesOnlyProbePairedWithExplicitTrim) {
    EventList eventlist;
    EventList::setEndtime(timeFromSec(1));

    constexpr mem_b data_size = 4160;
    TestCompositeQueue queue(speedFromMbps(uint64_t{400000}),
                             data_size,
                             eventlist,
                             nullptr,
                             UecBasePacket::ACKSIZE,
                             false,
                             false,
                             true,
                             0.0);
    FreePacketSink sink;
    PacketFlow flow(nullptr);
    Route route;
    route.push_back(&sink);

    CompositeQueue::_coalesce_trimmed_pfld_probe = true;
    CompositeQueue::_coalesced_pfld_probe_count = 0;
    auto* filler = UecDataPacket::newpkt(
        flow, route, 1, data_size, UecBasePacket::DATA_PULL, 0);
    auto* trimmed = UecDataPacket::newpkt(
        flow, route, 2, data_size, UecBasePacket::DATA_PULL, 0);
    trimmed->set_has_paired_pfld_probe(true);
    auto* paired_probe = UecDataPacket::newpkt(
        flow, route, 2, UecBasePacket::ACKSIZE, UecBasePacket::PROBE, 0);
    paired_probe->set_pflr_probe_type(UecDataPacket::PflrProbeType::PROACTIVE_DATA);
    auto* section_end_probe = UecDataPacket::newpkt(
        flow, route, 2, UecBasePacket::ACKSIZE, UecBasePacket::PROBE, 0);
    auto* unrelated_probe = UecDataPacket::newpkt(
        flow, route, 3, UecBasePacket::ACKSIZE, UecBasePacket::PROBE, 0);
    unrelated_probe->set_pflr_probe_type(UecDataPacket::PflrProbeType::PROACTIVE_DATA);

    queue.receivePacket(*filler);
    queue.receivePacket(*trimmed);
    ASSERT_TRUE(trimmed->header_only());
    ASSERT_EQ(queue.lowPacketCount(), 1u);
    ASSERT_EQ(queue.highPacketCount(), 1u);

    queue.receivePacket(*section_end_probe);
    EXPECT_EQ(queue.lowPacketCount(), 2u);
    EXPECT_EQ(queue.highPacketCount(), 1u);
    EXPECT_EQ(CompositeQueue::_coalesced_pfld_probe_count, uint64_t{0});

    queue.receivePacket(*paired_probe);
    EXPECT_EQ(queue.lowPacketCount(), 2u);
    EXPECT_EQ(queue.highPacketCount(), 1u);
    EXPECT_EQ(CompositeQueue::_coalesced_pfld_probe_count, uint64_t{1});

    queue.receivePacket(*unrelated_probe);
    EXPECT_EQ(queue.lowPacketCount(), 3u);
    EXPECT_EQ(queue.highPacketCount(), 1u);
    EXPECT_EQ(queue.reservedProbeBytes(), 2 * UecBasePacket::get_ack_size());

    while (eventlist.doNextEvent()) {
    }
    CompositeQueue::_coalesce_trimmed_pfld_probe = false;
    EventList::reset();
}
