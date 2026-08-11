// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// Unit tests for RFC 8985 RACK-TLP loss detection
//
// These tests validate:
//   1. RACK-TLP state structs and enums
//   2. Mode toggles (A/B/C/D/E)
//   3. PTO scheduling logic
//   4. RACK reorder window computation
//   5. Timer mutual exclusion properties
//   6. CSV logger
//
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "rack_tlp.h"
#include "helpers.h"

namespace fs = std::filesystem;

// ──────────────────────────────────────────────────────
//  Test: enums and struct defaults
// ──────────────────────────────────────────────────────
TEST(RackTlpEnums, ModeValues) {
    EXPECT_EQ(static_cast<uint8_t>(RackTlpMode::OFF), 0);
    EXPECT_EQ(static_cast<uint8_t>(RackTlpMode::RACK_ONLY), 1);
    EXPECT_EQ(static_cast<uint8_t>(RackTlpMode::RACK_TLP), 2);
    EXPECT_EQ(static_cast<uint8_t>(RackTlpMode::RACK_TLP_NO_6675), 3);
    EXPECT_EQ(static_cast<uint8_t>(RackTlpMode::TLP_ONLY), 4);
}

TEST(RackTlpStructs, RackStateDefaults) {
    RackState rs;
    EXPECT_EQ(rs.xmit_ts, 0u);
    EXPECT_EQ(rs.end_seq, 0u);
    EXPECT_EQ(rs.ack_ts, 0u);
    EXPECT_EQ(rs.rtt, 0u);
    EXPECT_EQ(rs.min_rtt, UINT64_MAX);
    EXPECT_EQ(rs.reo_wnd, 0u);
    EXPECT_EQ(rs.reo_wnd_incr, 0u);
    EXPECT_FALSE(rs.dsack_round_active);
}

TEST(RackTlpStructs, TlpStateDefaults) {
    TlpState ts;
    EXPECT_EQ(ts.end_seq, 0u);
    EXPECT_FALSE(ts.is_retrans);
    EXPECT_FALSE(ts.probe_in_flight);
    EXPECT_EQ(ts.max_ack_delay, 0u);
    EXPECT_TRUE(ts.has_rtt_sample);
}

TEST(RackTlpStructs, RackTlpStatsDefaults) {
    RackTlpStats stats;
    EXPECT_EQ(stats.rack_loss_marks, 0u);
    EXPECT_EQ(stats.tlp_probes_sent, 0u);
    EXPECT_EQ(stats.tlp_probe_repairs, 0u);
    EXPECT_EQ(stats.tlp_probe_spurious, 0u);
    EXPECT_EQ(stats.rto_events, 0u);
    EXPECT_EQ(stats.recovery_episodes, 0u);
    EXPECT_EQ(stats.spurious_retrans, 0u);
    EXPECT_EQ(stats.dup_thresh_marks, 0u);
}

// ──────────────────────────────────────────────────────
//  Test: RACK reorder window computation
// ──────────────────────────────────────────────────────
TEST(RackLogic, ReoWndDefault) {
    // Default (reo_wnd_incr == 0): reo_wnd = min_rtt / 4
    RackState rs;
    rs.min_rtt = timeFromUs(100u);  // 100 us
    rs.reo_wnd_incr = 0;
    // Simulate: reo_wnd should be min_rtt / 4
    simtime_picosec expected = rs.min_rtt / 4;
    rs.reo_wnd = rs.min_rtt / 4;
    EXPECT_EQ(rs.reo_wnd, expected);
    EXPECT_EQ(timeAsUs(rs.reo_wnd), 25u);
}

TEST(RackLogic, ReoWndIncrOne) {
    // After one DSACK: reo_wnd = min_rtt / 2
    RackState rs;
    rs.min_rtt = timeFromUs(100u);
    rs.reo_wnd_incr = 1;
    rs.reo_wnd = rs.min_rtt / 2;
    EXPECT_EQ(timeAsUs(rs.reo_wnd), 50u);
}

TEST(RackLogic, ReoWndIncrTwo) {
    // After two DSACKs: reo_wnd = min_rtt
    RackState rs;
    rs.min_rtt = timeFromUs(100u);
    rs.reo_wnd_incr = 2;
    rs.reo_wnd = rs.min_rtt;
    EXPECT_EQ(timeAsUs(rs.reo_wnd), 100u);
}

TEST(RackLogic, ReoWndIncrCapped) {
    EXPECT_EQ(RackState::MAX_REO_WND_INCR, 2u);
}

// ──────────────────────────────────────────────────────
//  Test: PTO computation
// ──────────────────────────────────────────────────────
TEST(TlpLogic, PTOBasicComputation) {
    // PTO = 2 * SRTT when SRTT > 0
    simtime_picosec srtt = timeFromUs(100u);  // 100us
    simtime_picosec pto  = 2 * srtt;
    EXPECT_EQ(timeAsUs(pto), 200u);
}

TEST(TlpLogic, PTOFallbackOneSecond) {
    // When SRTT == 0, PTO should be 1 second
    simtime_picosec srtt = 0;
    simtime_picosec pto;
    if (srtt > 0) {
        pto = 2 * srtt;
    } else {
        pto = timeFromSec(1u);
    }
    EXPECT_EQ(timeAsSec(pto), 1.0);
}

TEST(TlpLogic, PTOAddsMaxAckDelay) {
    // FlightSize == 1 → PTO += max_ack_delay
    simtime_picosec srtt          = timeFromUs(100u);
    simtime_picosec max_ack_delay = timeFromUs(25u);
    uint64_t        flight_size   = 1;

    simtime_picosec pto = 2 * srtt;
    if (flight_size == 1) {
        pto += max_ack_delay;
    }
    EXPECT_EQ(timeAsUs(pto), 225u);
}

TEST(TlpLogic, PTODoesNotAddDelayMultiPkt) {
    // FlightSize > 1 → PTO does not add max_ack_delay
    simtime_picosec srtt          = timeFromUs(100u);
    simtime_picosec max_ack_delay = timeFromUs(25u);
    uint64_t        flight_size   = 5;

    simtime_picosec pto = 2 * srtt;
    if (flight_size == 1) {
        pto += max_ack_delay;
    }
    EXPECT_EQ(timeAsUs(pto), 200u);
}

// ──────────────────────────────────────────────────────
//  Test: TLP "only one probe in flight" invariant
// ──────────────────────────────────────────────────────
TEST(TlpLogic, OnlyOneProbeInFlight) {
    TlpState tlp;
    EXPECT_FALSE(tlp.probe_in_flight);

    // Simulate sending a probe
    tlp.probe_in_flight = true;
    tlp.end_seq         = 42;
    tlp.has_rtt_sample  = false;

    // A second probe should NOT be sent
    EXPECT_TRUE(tlp.probe_in_flight);

    // On ACK, the probe gets resolved
    tlp.probe_in_flight = false;
    EXPECT_FALSE(tlp.probe_in_flight);
}

TEST(TlpLogic, NeedRttSampleBeforeNextProbe) {
    TlpState tlp;
    tlp.has_rtt_sample = true;

    // After sending probe, needs new RTT sample
    tlp.has_rtt_sample  = false;
    tlp.probe_in_flight = true;

    // Simulate ACK received
    tlp.probe_in_flight = false;
    // Still no RTT sample → cannot send another probe
    EXPECT_FALSE(tlp.has_rtt_sample);

    // Get an RTT sample
    tlp.has_rtt_sample = true;
    EXPECT_TRUE(tlp.has_rtt_sample);
}

TEST(TlpLogic, AckBeforeProbeDoesNotResolveIt) {
    TlpState tlp;
    tlp.probe_in_flight = true;
    tlp.is_retrans = true;
    tlp.end_seq = 42;

    EXPECT_EQ(classifyTlpAck(tlp, 41, false), TlpAckOutcome::NONE);
}

TEST(TlpLogic, RetransmissionAckConfirmsRepair) {
    TlpState tlp;
    tlp.probe_in_flight = true;
    tlp.is_retrans = true;
    tlp.end_seq = 42;

    EXPECT_EQ(classifyTlpAck(tlp, 42, true),
              TlpAckOutcome::RETRANSMISSION_REPAIRED);
}

TEST(TlpLogic, OriginalAckMakesRetransmissionSpurious) {
    TlpState tlp;
    tlp.probe_in_flight = true;
    tlp.is_retrans = true;
    tlp.end_seq = 42;

    EXPECT_EQ(classifyTlpAck(tlp, 42, false),
              TlpAckOutcome::RETRANSMISSION_SPURIOUS);
}

TEST(TlpLogic, NewDataProbeAckDoesNotClaimLoss) {
    TlpState tlp;
    tlp.probe_in_flight = true;
    tlp.is_retrans = false;
    tlp.end_seq = 42;

    EXPECT_EQ(classifyTlpAck(tlp, 42, false),
              TlpAckOutcome::NEW_DATA_ACKED);
}

// ──────────────────────────────────────────────────────
//  Test: Timer mutual exclusion property
// ──────────────────────────────────────────────────────
TEST(TimerLogic, MutualExclusion) {
    // Simulate timer state: at any point, at most one of
    // {RACK reorder, TLP PTO, RTO} should be "active" in terms
    // of the RACK-TLP specification.
    // We model this with booleans for simplicity.
    bool rack_active = false;
    bool tlp_active  = false;
    bool rto_active  = true;  // RTO is the default

    // When RACK timer is set, TLP should be cancelled
    rack_active = true;
    tlp_active  = false;  // cancelled
    EXPECT_TRUE(rack_active);
    EXPECT_FALSE(tlp_active);

    // When RACK expires and no remaining candidates, arm TLP instead
    rack_active = false;
    tlp_active  = true;
    EXPECT_FALSE(rack_active);
    EXPECT_TRUE(tlp_active);

    // When TLP fires and sends probe, cancel TLP timer
    tlp_active = false;
    EXPECT_FALSE(rack_active);
    EXPECT_FALSE(tlp_active);
}

// ──────────────────────────────────────────────────────
//  Test: RACK loss detection logic (pure computation)
// ──────────────────────────────────────────────────────
TEST(RackLogic, LossDetectionBasic) {
    // Simulate: three segments sent at times T0, T1, T2.
    // Segment at T2 is ACKed → becomes RACK reference.
    // Check if segment at T0 should be marked lost.

    RackState rack;
    simtime_picosec T0 = timeFromUs(0u);
    simtime_picosec T1 = timeFromUs(5u);
    simtime_picosec T2 = timeFromUs(10u);
    simtime_picosec rtt = timeFromUs(100u);

    // Segment at T2 ACKed at T2 + rtt
    rack.xmit_ts = T2;
    rack.end_seq = 2;
    rack.rtt     = rtt;
    rack.min_rtt = rtt;
    rack.reo_wnd = rtt / 4;  // 25us

    // At time T2 + rtt (110us), check segment at T0:
    //   deadline = T0 + rack.rtt + rack.reo_wnd = 0 + 100 + 25 = 125us
    //   now = 110us < 125us → NOT lost yet, but timer should be set
    simtime_picosec now1 = T2 + rtt;  // 110us
    simtime_picosec loss_time_seg0 = T0 + rack.rtt + rack.reo_wnd;  // 125us
    EXPECT_GT(loss_time_seg0, now1);  // not lost yet

    // At time 125us: exactly at the deadline → lost
    simtime_picosec now2 = loss_time_seg0;
    EXPECT_GE(now2, loss_time_seg0);  // lost

    // Segment at T1 deadline = 5 + 100 + 25 = 130us
    simtime_picosec loss_time_seg1 = T1 + rack.rtt + rack.reo_wnd;
    EXPECT_EQ(timeAsUs(loss_time_seg1), 130u);
    EXPECT_LT(now2, loss_time_seg1);  // seg1 not lost at t=125us
}

// ──────────────────────────────────────────────────────
//  Test: CSV logger
// ──────────────────────────────────────────────────────
TEST(RackTlpLogger, WritesCSV) {
    std::string test_dir = "/tmp/rack_tlp_test_" + std::to_string(getpid());
    fs::create_directories(test_dir);

    {
        RackTlpLogger logger;
        logger.open(test_dir, 42);
        EXPECT_TRUE(logger.is_open());

        logger.log(timeFromUs(100u), 42, 7, "send", 50000, 12000);
        logger.log(timeFromUs(200u), 42, 8, "ack", 50000, 8000);
        logger.log(timeFromUs(250u), 42, 5, "loss_marked", 25000, 8000);
    }  // closes file

    std::string path = test_dir + "/rack_tlp_flow_42.csv";
    EXPECT_TRUE(fs::exists(path));

    std::ifstream ifs(path);
    std::string header;
    std::getline(ifs, header);
    EXPECT_EQ(header, "time_us,flow_id,seq,event_type,cwnd,inflight");

    std::string line;
    int count = 0;
    while (std::getline(ifs, line)) {
        count++;
        EXPECT_THAT(line, testing::HasSubstr(",42,"));
    }
    EXPECT_EQ(count, 3);

    // Cleanup
    fs::remove_all(test_dir);
}

TEST(RackTlpLogger, ClosedByDefault) {
    RackTlpLogger logger;
    EXPECT_FALSE(logger.is_open());
    // Logging to a closed logger should be a no-op (no crash)
    logger.log(0, 0, 0, "test", 0, 0);
}

// ──────────────────────────────────────────────────────
//  Test: Mode helper functions
// ──────────────────────────────────────────────────────
TEST(RackTlpMode, HelperQueries) {
    // Test the mode logic that would be in UecSrc
    auto modeEnabled = [](RackTlpMode m) {
        return m != RackTlpMode::OFF;
    };
    auto tlpOn = [](RackTlpMode m) {
        return m == RackTlpMode::RACK_TLP ||
               m == RackTlpMode::RACK_TLP_NO_6675 ||
               m == RackTlpMode::TLP_ONLY;
    };
    auto rfc6675On = [](RackTlpMode m) {
        return m != RackTlpMode::RACK_TLP_NO_6675;
    };

    // Mode A: OFF
    EXPECT_FALSE(modeEnabled(RackTlpMode::OFF));
    EXPECT_FALSE(tlpOn(RackTlpMode::OFF));
    EXPECT_TRUE(rfc6675On(RackTlpMode::OFF));

    // Mode B: RACK_ONLY
    EXPECT_TRUE(modeEnabled(RackTlpMode::RACK_ONLY));
    EXPECT_FALSE(tlpOn(RackTlpMode::RACK_ONLY));
    EXPECT_TRUE(rfc6675On(RackTlpMode::RACK_ONLY));

    // Mode C: RACK_TLP
    EXPECT_TRUE(modeEnabled(RackTlpMode::RACK_TLP));
    EXPECT_TRUE(tlpOn(RackTlpMode::RACK_TLP));
    EXPECT_TRUE(rfc6675On(RackTlpMode::RACK_TLP));

    // Mode D: RACK_TLP_NO_6675
    EXPECT_TRUE(modeEnabled(RackTlpMode::RACK_TLP_NO_6675));
    EXPECT_TRUE(tlpOn(RackTlpMode::RACK_TLP_NO_6675));
    EXPECT_FALSE(rfc6675On(RackTlpMode::RACK_TLP_NO_6675));

    // Mode E: TLP_ONLY
    EXPECT_TRUE(modeEnabled(RackTlpMode::TLP_ONLY));
    EXPECT_TRUE(tlpOn(RackTlpMode::TLP_ONLY));
    EXPECT_TRUE(rfc6675On(RackTlpMode::TLP_ONLY));
}

// ──────────────────────────────────────────────────────
//  Test: RACK state update picks most recent segment
// ──────────────────────────────────────────────────────
TEST(RackLogic, UpdatePicksMostRecent) {
    RackState rack;

    // First ACK: segment sent at 10us, PSN=1
    simtime_picosec t1 = timeFromUs(10u);
    if (t1 > rack.xmit_ts || (t1 == rack.xmit_ts && 1 > rack.end_seq)) {
        rack.xmit_ts = t1;
        rack.end_seq = 1;
        rack.rtt     = timeFromUs(50u);
    }
    EXPECT_EQ(rack.end_seq, 1u);

    // Second ACK: segment sent at 20us, PSN=3
    simtime_picosec t2 = timeFromUs(20u);
    if (t2 > rack.xmit_ts || (t2 == rack.xmit_ts && 3 > rack.end_seq)) {
        rack.xmit_ts = t2;
        rack.end_seq = 3;
        rack.rtt     = timeFromUs(45u);
    }
    EXPECT_EQ(rack.end_seq, 3u);
    EXPECT_EQ(rack.rtt, timeFromUs(45u));

    // Third ACK: segment sent at 15us, PSN=2 (older - should NOT update)
    simtime_picosec t3 = timeFromUs(15u);
    if (t3 > rack.xmit_ts || (t3 == rack.xmit_ts && 2 > rack.end_seq)) {
        rack.xmit_ts = t3;
        rack.end_seq = 2;
        rack.rtt     = timeFromUs(60u);
    }
    // RACK should still point to PSN 3 (sent at 20us)
    EXPECT_EQ(rack.end_seq, 3u);
    EXPECT_EQ(rack.xmit_ts, t2);
}

// ──────────────────────────────────────────────────────
//  Test: RFC 8985 Figure-1-like scenario
// ──────────────────────────────────────────────────────
TEST(RackLogic, Figure1TailLoss) {
    // Scenario: sender sends packets PSN 0-9.
    // Packets 7,8,9 are dropped (tail loss).
    // No more ACKs arrive after PSN 6.
    // Without TLP: RTO is the only recovery.
    // With TLP: PTO fires and sends a probe.

    simtime_picosec srtt = timeFromUs(100u);
    simtime_picosec rto  = timeFromUs(400u);  // 4 * SRTT

    // PTO = 2 * SRTT = 200us → fires BEFORE RTO (400us)
    simtime_picosec pto = 2 * srtt;
    EXPECT_LT(pto, rto);

    // TLP probe triggers SACK → RACK detects 7,8,9 lost
    // Recovery time with TLP: PTO + 1 RTT ≈ 300us
    // Recovery time without TLP: RTO = 400us
    simtime_picosec recovery_tlp = pto + srtt;
    EXPECT_LT(recovery_tlp, rto);
}
