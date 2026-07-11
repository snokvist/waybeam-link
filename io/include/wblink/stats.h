// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: streaming stats (PROTOCOL.md §15.3). One NDJSON line per
// emit, field names and nesting exactly as the spec sample so operators can
// grep/jq against the spec. The writer is hand-rolled with fixed field order:
// deterministic output (golden-testable) and no allocation churn at emit time
// (the line buffer is reused).
//
// Fields that later build steps populate (ARQ counters, link state, return
// health) are present-but-zero from day one, so the schema is complete and
// consumers never see fields appear.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "wblink/binding.h"

namespace wblink {

struct AdapterStats {
    std::string name;
    uint64_t rx = 0;
    uint64_t dup = 0;
    int32_t rssi_best = 0;
    int32_t rssi_mean = 0;
    int32_t snr = 0;
    int32_t noise = 0;
    uint64_t tx_submitted = 0;
    uint64_t tx_failed = 0;
    uint64_t tx_timeout = 0;
    uint64_t drop = 0;          // radio backend RX-queue overflow drops
    uint64_t tsf_fallback = 0;  // §7.2 TSF reads that fell back to host time
    // Per-frame TX-status CCX reports (Pass 8, TX adapter only): reports
    // stalling while tx_submitted advances = the TX-wedge signal.
    uint64_t tx_reports = 0;
    uint64_t tx_report_fails = 0;
    bool adapter_stalled = false;  // §6.5 liveness watchdog verdict
    bool tx_wedged = false;        // §9.10 CCX-liveness verdict (TX adapter)
};

struct StreamStats {
    uint8_t stream_id = 0;
    std::string type;  // registry name, e.g. "RTP"
    uint32_t seq = 0;
    uint64_t delivered = 0;
    // §6.1 diversity accounting (gate-2 ρ inputs): unique packets accepted
    // and duplicate copies merged away across adapters.
    uint64_t uniq = 0;
    uint64_t diversity = 0;
    // §3.7: these two must never be conflated.
    uint32_t loss_prediversity_milli = 0;
    uint32_t loss_postdiv_prearq_milli = 0;
    uint64_t recovered_arq = 0;
    uint64_t recovered_fec = 0;
    uint64_t dropped_superseded = 0;
    uint64_t dropped_deadline = 0;
    uint64_t nacks_sent = 0;
    // §17 gate-3 estimator: cumulative NACK→RETRANSMIT latency histograms,
    // ms upper bounds 1,2,4,8,16,32,64,+inf. nack_rtt = most-recent-NACK
    // anchor (pure round-trip); arq_rec = first-NACK anchor (recovery vs
    // the I-frame deadline).
    std::array<uint64_t, 8> nack_rtt_hist{};
    uint64_t nack_rtt_max_ms = 0;
    std::array<uint64_t, 8> arq_rec_hist{};
    uint64_t arq_rec_max_ms = 0;
    uint64_t resends_sent = 0;
    uint64_t double_send_suppressed = 0;
    uint64_t decode_errors = 0;
    uint8_t active_profile = 0;
    uint8_t table_version = 0;
};

struct ReturnStats {
    uint32_t reports_expected = 0;
    uint32_t reports_received = 0;
    uint32_t return_window_hits = 0;
    uint32_t return_window_misses = 0;
    // §3.0 Pass 12 unicast returns: sent unicast vs fell back to broadcast
    // (no SA latched for the target yet).
    uint64_t unicast_sent = 0;
    uint64_t unicast_fallback = 0;
};

struct LinkStats {
    uint16_t target_originator = 0;
    uint32_t target_session = 0;
    uint8_t profile = 0;
    uint8_t mcs = 0;
    int32_t tx_power_qdb = 0;
    uint32_t report_epoch = 0;
    uint32_t report_age_ms = 0;
    std::string state = "HOLD";
    bool flap_freeze = false;
    std::string csa_state = "IDLE";
};

struct StatsSnapshot {
    uint64_t t_ms = 0;
    uint16_t node = 0;
    uint32_t session = 0;
    std::vector<AdapterStats> adapters;
    std::vector<StreamStats> streams;
    ReturnStats ret;
    LinkStats link;
};

// Appends one §15.3 NDJSON line (terminated with '\n') to out.
void format_stats_line(const StatsSnapshot& snap, std::string& out);

class StatsEmitter {
  public:
    // udp may be nullptr (stdout only). Neither sink is owned.
    StatsEmitter(bool to_stdout, UdpEgress* udp)
        : to_stdout_(to_stdout), udp_(udp) {}

    void emit(const StatsSnapshot& snap);

  private:
    bool to_stdout_;
    UdpEgress* udp_;
    std::string line_;  // reused across emits
};

}  // namespace wblink
