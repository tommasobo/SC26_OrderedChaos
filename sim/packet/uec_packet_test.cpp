// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "uec_packet.h"

#include <gtest/gtest.h>

#include "packet_flow.h"
#include "route.h"

TEST(UecProbePacket, LowPriorityTrimmingPreservesProbeSemantics) {
    PacketFlow flow(nullptr);
    Route      route;
    auto* probe = UecDataPacket::newpkt(
        flow,
        route,
        42,
        4160,
        UecBasePacket::PROBE,
        0);
    probe->set_pathid(7);
    probe->set_pflr_probe_type(UecDataPacket::PROACTIVE_RTX);

    probe->strip_payload_low(UecBasePacket::ACKSIZE);

    EXPECT_TRUE(probe->header_low_only());
    EXPECT_TRUE(probe->is_probe_packet());
    EXPECT_EQ(probe->packet_type(), UecBasePacket::PROBE);
    EXPECT_EQ(probe->pflr_probe_type(), UecDataPacket::PROACTIVE_RTX);
    EXPECT_EQ(probe->epsn(), 42u);
    EXPECT_EQ(probe->path_id(), 7);

    probe->free();
}
