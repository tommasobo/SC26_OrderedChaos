// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// RFC 8985 RACK-TLP - CSV logger implementation
//
#include "rack_tlp.h"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────
//  RackTlpLogger
// ─────────────────────────────────────────────
RackTlpLogger::~RackTlpLogger() {
    if (_ofs.is_open())
        _ofs.close();
}

void RackTlpLogger::open(const std::string& dir, uint32_t flow_id) {
    fs::create_directories(dir);
    std::string path = dir + "/rack_tlp_flow_" + std::to_string(flow_id) + ".csv";
    _ofs.open(path, std::ios::out | std::ios::trunc);
    if (_ofs.is_open()) {
        _ofs << "time_us,flow_id,seq,event_type,cwnd,inflight\n";
    }
}

void RackTlpLogger::log(simtime_picosec time, uint32_t flow_id, uint64_t seq,
                         const char* event_type, int64_t cwnd, int64_t inflight) {
    if (!_ofs.is_open())
        return;
    // Convert picoseconds → microseconds with 3-decimal precision
    double time_us = static_cast<double>(time) / 1e6;
    _ofs << time_us << "," << flow_id << "," << seq << ","
         << event_type << "," << cwnd << "," << inflight << "\n";
}
