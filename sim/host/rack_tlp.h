// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// RFC 8985 RACK-TLP Loss Detection for htsim
//
// This module implements the RACK (Recent ACKnowledgment) and TLP
// (Tail Loss Probe) algorithms from RFC 8985, adapted for htsim's
// PSN-based (packet sequence number) transport rather than byte-based TCP.
//
// Modes:
//   A (OFF)              - baseline DupThresh + existing Sleek/PFLR recovery
//   B (RACK_ONLY)        - RACK loss detection, no TLP
//   C (RACK_TLP)         - full RACK + TLP (RFC 8985)
//   D (RACK_TLP_NO_6675) - RACK + TLP, disable RFC6675-style dup-thresh marking
//   E (TLP_ONLY)         - TLP probes only, keep baseline loss detection
//
#ifndef RACK_TLP_H
#define RACK_TLP_H

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include "helpers.h"         // simtime_picosec, timeFromUs, etc.
#include "uec_packet.h"      // UecBasePacket::seq_t

// Forward declaration
class UecSrc;

// ─────────────────────────────────────────────
//  Enums
// ─────────────────────────────────────────────
enum class RackTlpMode : uint8_t {
    OFF             = 0,  // A - baseline (no RACK, no TLP)
    RACK_ONLY       = 1,  // B - RACK loss detection only
    RACK_TLP        = 2,  // C - RACK + TLP
    RACK_TLP_NO_6675 = 3, // D - RACK + TLP, no RFC 6675 dup-thresh
    TLP_ONLY        = 4   // E - TLP only, no RACK loss detection
};

// ─────────────────────────────────────────────
//  RACK state  (§ 6.1)
// ─────────────────────────────────────────────
struct RackState {
    simtime_picosec xmit_ts   = 0;               // Tx timestamp of most-recently-delivered seg
    uint64_t        end_seq   = 0;               // PSN of that segment
    simtime_picosec ack_ts    = 0;               // Time when it was ACKed
    simtime_picosec rtt       = 0;               // RTT of that segment
    simtime_picosec min_rtt   = UINT64_MAX;      // Minimum observed RTT
    simtime_picosec reo_wnd   = 0;               // Reordering window

    // DSACK-based reo_wnd adaptation (§ 6.2)
    bool            dsack_round_active   = false;
    uint64_t        dsack_round_end_seq  = 0;    // SND.NXT when DSACK round started
    uint32_t        reo_wnd_incr         = 0;    // 0-2: how many times we widened reo_wnd
    static constexpr uint32_t MAX_REO_WND_INCR = 2;
};

// ─────────────────────────────────────────────
//  TLP state  (§ 7.1)
// ─────────────────────────────────────────────
struct TlpState {
    uint64_t        end_seq              = 0;     // SND.NXT at probe send
    bool            is_retrans           = false;  // Was the probe a retransmission?
    bool            probe_in_flight      = false;
    uint16_t        probe_size           = 0;     // Bytes in the probe segment
    simtime_picosec max_ack_delay        = 0;     // Peer's max delayed-ACK timer (configurable)
    bool            has_rtt_sample       = true;  // At least one RTT sample since last probe
    simtime_picosec pto_deadline         = 0;     // When PTO should fire (0 = not armed)
};

enum class TlpAckOutcome : uint8_t {
    NONE,
    NEW_DATA_ACKED,
    RETRANSMISSION_REPAIRED,
    RETRANSMISSION_SPURIOUS
};

inline TlpAckOutcome classifyTlpAck(const TlpState& tlp,
                                    uint64_t acked_psn,
                                    bool rtx_echo) {
    if (!tlp.probe_in_flight || acked_psn < tlp.end_seq) {
        return TlpAckOutcome::NONE;
    }
    if (!tlp.is_retrans) {
        return TlpAckOutcome::NEW_DATA_ACKED;
    }
    return rtx_echo ? TlpAckOutcome::RETRANSMISSION_REPAIRED
                    : TlpAckOutcome::RETRANSMISSION_SPURIOUS;
}

// ─────────────────────────────────────────────
//  Instrumentation counters
// ─────────────────────────────────────────────
struct RackTlpStats {
    uint32_t rack_loss_marks     = 0;
    uint32_t tlp_probes_sent     = 0;
    uint32_t tlp_probe_repairs   = 0;    // probe triggered real recovery
    uint32_t tlp_probe_spurious  = 0;    // probe was spurious
    uint32_t rto_events          = 0;
    uint32_t recovery_episodes   = 0;
    uint32_t spurious_retrans    = 0;
    uint32_t dup_thresh_marks    = 0;    // RFC 6675 style marks (mode A/B only)
};

// ─────────────────────────────────────────────
//  CSV event logger
// ─────────────────────────────────────────────
class RackTlpLogger {
public:
    RackTlpLogger() = default;
    ~RackTlpLogger();

    // Open the CSV file (call once per flow).  path = directory.
    void open(const std::string& dir, uint32_t flow_id);
    bool is_open() const { return _ofs.is_open(); }

    // Log one event row.
    void log(simtime_picosec time, uint32_t flow_id, uint64_t seq,
             const char* event_type, int64_t cwnd, int64_t inflight);

private:
    std::ofstream _ofs;
};

#endif  // RACK_TLP_H
