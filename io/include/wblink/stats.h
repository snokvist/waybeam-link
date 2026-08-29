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
#include <optional>
#include <string>
#include <vector>

#include "wblink/binding.h"
#include "wblink/radiotap.h"  // kRxMcsBuckets (§15.3 Pass 118 histogram)
#include "wblink/types.h"

namespace wblink {

struct AdapterStats {
    std::string name;
    uint64_t rx = 0;
    uint64_t dup = 0;
    // §15.3 Pass 158: on the radio backend these four are a per-stats-tick
    // quality window — rssi_best is the window PEAK (saturation-proof),
    // snr the mean dB, noise the passive floor rssi−snr. evm (mean dB,
    // lower = cleaner) is the saturation discriminator; evm_valid=false
    // means no frame carried EVM, not perfect coding.
    int32_t rssi_best = 0;
    int32_t rssi_mean = 0;
    int32_t snr = 0;
    int32_t noise = 0;
    int32_t evm = 0;
    bool evm_valid = false;
    uint64_t tx_submitted = 0;
    uint64_t tx_failed = 0;
    // §15.2: USB bulk-OUT transfers the submitted frames went out in, and
    // those that did not complete OK. tx_submitted counts frames whether or
    // not air.usb_tx_agg packed them, so tx_submitted / tx_bulk is what says
    // whether packing actually happened. 0 = the backend reports no bulk
    // accounting (udp air, or a device without TxStats), not zero transfers.
    uint64_t tx_bulk = 0;
    uint64_t tx_bulk_failed = 0;
    uint64_t tx_timeout = 0;
    uint64_t drop = 0;          // radio backend RX-queue overflow drops
    uint64_t filtered = 0;      // malformed/non-network/self receive frames
    uint64_t kernel_drop = 0;   // Linux SO_RXQ_OVFL socket queue drops
    // §3.0 BPF pre-filter rejections. No producer since Pass 164 deleted the
    // kernel-monitor backend; kept at 0 so the §15.3 schema stays stable.
    uint64_t bpf_filtered = 0;
    uint64_t tsf_fallback = 0;  // §7.2 TSF reads that fell back to host time
    // Per-frame TX-status CCX reports (Pass 8, TX adapter only): reports
    // stalling while tx_submitted advances = the TX-wedge signal.
    uint64_t tx_reports = 0;
    uint64_t tx_report_fails = 0;
    // §15.3 Pass 118: accepted frames per HT MCS 0..7 (the §9.3 ladder) plus
    // the frames whose PHY rate the backend could not resolve. Sums to rx.
    // Advisory observation only — no control path reads these.
    uint64_t rx_mcs[kRxMcsBuckets] = {};
    uint64_t rx_mcs_unknown = 0;
    // §15.3 Pass 157: received-coding counters + the static die truth of
    // whether they can ever be nonzero (devourer ldpc_rx_flag). Advisory,
    // like rx_mcs; 0/false off the radio backend.
    uint64_t rx_ldpc = 0;
    uint64_t rx_stbc = 0;
    bool ldpc_flag_ok = false;
    bool adapter_stalled = false;  // §6.5 liveness watchdog verdict (heuristic)
    bool rx_dead = false;          // §15.3 Pass 101: RX loop terminated (definitive)
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
    // Successfully delivered frame attribution. Unlike recovered_arq (packet
    // sequence gaps) and recovered_fec (frames), the *_symbols fields share a
    // symbol unit and distinguish retransmitted source from repair rows.
    uint64_t fec_recovered_source_symbols = 0;
    uint64_t arq_recovered_source_symbols = 0;
    uint64_t arq_recovered_repair_symbols = 0;
    uint64_t frames_with_arq = 0;
    uint64_t frames_fec_only = 0;
    uint64_t frames_fec_after_arq = 0;
    uint64_t frame_count = 0;
    uint64_t frame_bytes = 0;
    uint32_t frame_size_last = 0;
    uint32_t frame_size_min = 0;
    uint32_t frame_size_max = 0;
    uint64_t frame_interval_us = 0;
    uint64_t frame_jitter_us = 0;
    // §6.3a frame-shm reassembler outcomes (0 on udp streams): fast-path
    // (all-source, no decode), finalized-below-k, and pre-decode rejects.
    uint64_t frames_fast = 0;
    uint64_t frames_unrecoverable = 0;
    uint64_t frames_egress_rejected = 0;
    uint64_t malformed = 0;
    // §6.3b spatial salvage outcomes (0 with conceal.mode "off").
    uint64_t frames_salvaged = 0;
    uint64_t frames_frozen = 0;
    uint64_t salvage_failed = 0;
    uint64_t slices_synthesized = 0;
    uint64_t jscc_shadow_blocks = 0;
    uint16_t jscc_predicted_loss_symbols = 0;
    uint16_t jscc_observed_loss_symbols = 0;
    uint64_t jscc_underpredicted_blocks = 0;
    uint64_t jscc_predicted_parity_symbols = 0;
    uint16_t jscc_predicted_repair_symbols = 0;
    uint16_t jscc_observed_repair_symbols = 0;
    uint64_t jscc_repair_underpredicted_blocks = 0;
    uint64_t jscc_repair_demand_censored_blocks = 0;
    uint64_t jscc_repair_predicted_parity_symbols = 0;
    uint64_t jscc_decision_frames = 0;
    uint64_t jscc_valid_decisions = 0;
    uint64_t jscc_fallback_decisions = 0;
    bool jscc_decision_valid = false;
    std::string jscc_fallback;
    std::string jscc_reason;
    uint16_t jscc_input_k = 0;
    uint16_t jscc_input_predicted_symbols = 0;
    uint16_t jscc_input_floor_symbols = 0;
    uint16_t jscc_input_cap_symbols = 0;
    uint32_t jscc_input_deadline_us = 0;
    uint32_t jscc_input_source_tx_us = 0;
    uint32_t jscc_input_rtt_p95_us = 0;
    uint32_t jscc_input_resend_us = 0;
    uint32_t jscc_input_guard_us = 0;
    uint16_t jscc_output_parity_symbols = 0;
    uint32_t jscc_output_remaining_us = 0;
    bool jscc_output_arq_eligible = false;
    bool jscc_output_discard = false;
    uint32_t jscc_feedback_epoch = 0;
    uint32_t jscc_feedback_age_ms = 0;
    // §14.2 enforcement (Pass 38): actuated valid decisions + rule-2 drops.
    uint64_t jscc_enforced_frames = 0;
    uint64_t jscc_discarded_frames = 0;
    uint64_t jscc_exempt_frames = 0;  // §14.2 Pass 149 §14.1a exemption
    // §15.3 Pass 109: ingress producer health rides the "VHLT" marker.
    // When valid, full_drops is the delta since attach/reset and low_water is
    // a gauge. shm_ring_full remains independent consumer-side evidence.
    // shm_low_water_slots is ring occupancy in SLOTS (<= 1 healthy) and is
    // only meaningful when shm_health_valid -- 0 is a healthy reading, not a
    // sentinel.
    bool shm_health_valid = false;
    uint64_t shm_full_drops = 0;
    uint16_t shm_low_water_slots = 0;
    // Producer discards that are NOT congestion -- an access unit the producer
    // could not build. Never fold into shm_full_drops: a rate controller must
    // not slow down for these.
    uint64_t shm_other_drops = 0;
    uint64_t shm_oversize_drops = 0;
    uint64_t shm_bad_slots = 0;
    uint64_t shm_ring_full = 0;
    uint64_t dropped_superseded = 0;
    uint64_t dropped_deadline = 0;
    uint64_t nacks_sent = 0;
    // §17 gate-3 estimator: cumulative NACK→RETRANSMIT latency histograms,
    // ms upper bounds 1,2,4,8,16,32,64,+inf. nack_rtt = most-recent-NACK
    // anchor (pure round-trip); arq_rec = first-NACK anchor (recovery vs
    // the I-frame deadline).
    std::array<uint64_t, 8> nack_rtt_hist{};
    uint64_t nack_rtt_max_ms = 0;
    uint16_t nack_rtt_samples = 0;
    uint32_t nack_rtt_p95_us = 0;
    std::array<uint64_t, 8> arq_rec_hist{};
    uint64_t arq_rec_max_ms = 0;
    uint64_t resends_sent = 0;
    // §12 Pass 116: current ARQ lock holder (0 = parked). Was tracked in
    // SchedulerCounters but never emitted — an invisible arbitration state.
    uint16_t arq_lock_holder = 0;
    uint64_t double_send_suppressed = 0;
    uint64_t source_symbols_sent = 0;
    uint64_t repair_symbols_sent = 0;
    uint64_t fec_oversize_frames = 0;
    uint64_t mtu_fec_guard_frames = 0;
    uint64_t idr_frames = 0;
    uint64_t arq_frames = 0;
    uint64_t arq_cutoff_frames = 0;  // §4.1 Pass 40 cadence suppression
    uint64_t fec_enhance_frames = 0;  // §14.1a observed droppable density
    uint64_t decode_errors = 0;
    uint8_t active_profile = 0;
    uint8_t table_version = 0;
};

struct ReturnStats {
    uint32_t reports_expected = 0;
    uint32_t reports_received = 0;
    uint64_t reports_rejected = 0;   // §3.5 Pass 41 acceptance filter
    uint64_t feedback_rejected = 0;  // §3.10 gate, same filter (Pass 115)
    // §3.5 Pass 115: current report-authority holder, 0 = no latch. TX-only;
    // 0 on ground/loopback like every other role-specific field here.
    uint16_t report_latch_holder = 0;
    uint32_t return_window_hits = 0;
    uint32_t return_window_misses = 0;
    // §3.0 Pass 12 unicast returns: sent unicast vs fell back to broadcast
    // (no SA latched for the target yet).
    uint64_t unicast_sent = 0;
    uint64_t unicast_fallback = 0;
};

struct TimingMetricStats {
    uint64_t samples = 0;
    uint32_t p95_us = 0;
    uint32_t max_us = 0;
};

// Host-local phase timing. Cross-host values are deliberately composed only
// when the same host observes both endpoints (ground sees NACK TX + resend RX;
// vehicle sees NACK RX + resend submission), so no clock sync is implied.
struct ArqTimingStats {
    TimingMetricStats eob_to_nack_build;
    TimingMetricStats nack_build_to_inject;
    TimingMetricStats nack_inject_to_retransmit;
    TimingMetricStats nack_build_to_retransmit;
    TimingMetricStats nack_receive_to_resend;
};

struct LinkStats {
    uint16_t target_originator = 0;
    uint32_t target_session = 0;
    uint8_t profile = 0;
    uint8_t mcs = 0;
    int32_t tx_power_qdb = 0;
    int tx_power_tier = -1;                // §11.7 0x0A, -1 = no preset list
    int32_t tx_power_ceiling_qdb = 0;      // §10.3 effective ceiling, 0 = none
    bool tx_power_tier_effective = false;  // false: recorded but reaches no HW
    bool tx_power_override = false;  // §10.5 latch active (Pass 114)
    uint32_t report_epoch = 0;
    uint32_t report_age_ms = 0;
    std::string state = "HOLD";
    std::string transition_reason = "NONE";
    uint16_t loss_window_milli = 0;
    uint16_t loss_ewma_milli = 0;
    uint32_t loss_uniq = 0;
    uint8_t loss_score = 0;
    uint8_t safe_floor_profile = 0;
    bool selector_state_valid = false;
    uint32_t selector_state_age_ms = 0;
    bool lockout_active = false;
    bool lockout_latched = false;
    uint8_t lockout_profile = 0xFF;
    uint8_t lockout_ceiling_profile = 0;
    uint32_t lockout_remaining_ms = 0;
    uint8_t lockout_strikes = 0;
    uint8_t lockout_active_mask = 0;
    uint8_t lockout_latched_mask = 0;
    bool lockout_conflict = false;
    // §3.15a/§15.3: the §3.5 latch holder as THIS node understands it — the
    // local gate on a TX, the craft's §3.15 summary on an RX. `known` false
    // means not reported (legacy craft, or no fresh summary) and is NOT the
    // same answer as holder 0.
    uint16_t report_latch_holder = 0;
    bool report_latch_known = false;
    // §15.3 Pass 159/160 role-dependent verdict view (0 = Unknown/none):
    // craft = last ACCEPTED §3.16 verdict + age; radio ground = the cause
    // it computes (age 0). promote_blocked_saturated is craft-only.
    uint8_t verdict = 0;
    uint32_t verdict_age_ms = 0;
    uint64_t promote_blocked_saturated = 0;
    // §15.3 Pass 163: climbs suppressed by the §9.4 probe veto; craft-only.
    uint64_t promote_blocked_probe = 0;
    // §15.3 Pass 186: the §9.4 probe made observable. Role-dependent like
    // `verdict` above — on a CRAFT, probe_per is the last value RECEIVED in a
    // §3.5 report (what the veto reads) and probe_candidate_mcs is the rate
    // this node flies on probe slots; on a radio GROUND, probe_per is what
    // this window COMPUTES this tick (so age is 0 by construction) and the
    // three tallies are the guard evidence behind it. kNoProbe is "no
    // opinion" and is NOT the same answer as 0.
    uint16_t probe_per = kNoProbe;
    uint32_t probe_per_age_ms = 0;
    uint8_t probe_candidate_mcs = kProbeMcsNone;
    uint32_t probe_successes = 0;  // ground-only (guards 1-3)
    uint32_t probe_failures = 0;   // ground-only (guards 1-3)
    uint32_t probe_observed = 0;   // ground-only (guard 4) — the working proof
    bool flap_freeze = false;
    std::string csa_state = "IDLE";
    // §11 follow-me: current RF operating channel (center MHz). 0 when the node
    // does not track a runtime channel (tx/loopback); the rx node reports its
    // live committed channel so ground consumers can show where the link is.
    uint16_t channel_mhz = 0;
    // §9.6 actuator state (Pass 37): last COMMANDED values (0 = never
    // pushed) + the settling window; zero/false without venc.enabled.
    uint32_t venc_bitrate_kbps = 0;
    uint64_t venc_pushes = 0;
    uint64_t venc_failures = 0;
    // §9.6 write path (Pass 192). venc_live_fallback: the persisting /set
    // fallback is latched, so the NEXT commanded change writes venc's config
    // file. venc_persisted_writes: /set writes that already reached it, for
    // the life of the process (0 = this link has never written encoder flash).
    // Neither is derivable from venc_failures, which counts transport errors.
    bool venc_live_fallback = false;
    uint64_t venc_persisted_writes = 0;
    bool venc_settling = false;
    uint16_t venc_fps = 0;  // §9.11 last commanded fps (0 = never)
    uint32_t venc_p_frame_bytes = 0;  // §9.11 non-IDR payload EWMA
    uint32_t venc_p_frame_target_bytes = 0;
    std::string venc_fps_ladder_state = "DISABLED";
    // §11.7 command surface — role-neutral defaults on every node (§15.3).
    bool cmd_arq = true;               // craft: applied ARQ command state
    bool cmd_selector_frozen = false;  // craft: applied SELECTOR command
    bool cmd_fps_ladder = false;       // craft: ladder running (unconfigured = false)
    uint32_t cmd_last_nonce = 0;       // craft: last consumed nonce (0 = never)
    // §11.7 v2 (Pass 71): applied preset, 1-based index (0 = none this
    // craft session — venc may still run a preset persisted earlier).
    uint8_t cmd_fps_select = 0;
    uint8_t cmd_resolution_select = 0;  // staged
    uint8_t cmd_framing_select = 0;     // staged
    std::string vcmd_state = "idle";   // issuer: §15.5 GET campaign state
    uint32_t vcmd_nonce = 0;           // issuer: last campaign nonce
    bool arq_rx_enabled = true;        // rx: §6.4 NACK-emission gate
    // §9.3a Pass 122 complete-DATA-packet budgets (bytes).
    std::string mtu_mode = "default";  // ground preference; "remote" on craft
    uint16_t mtu_requested = kDefaultMaxPayload;
    uint16_t mtu_effective = kDefaultMaxPayload;
    uint16_t mtu_supported = kDefaultMaxPayload;
    // §10.6 (Pass 120) calibration surface — mirrors the §3.15 word.
    std::string calib_state = "idle";  // idle|running|done|failed
    uint8_t calib_rung = 0;            // meaningful while running
    uint8_t calib_fingerprint = 0;     // CRC-8 of persisted artifact, 0=none
    bool calib_stale = false;          // persisted artifact pairing mismatch
    // §10.7 (Pass 125) ground-uplink surface. DISTINCT from the mirror above:
    // those fields are the craft's downlink state relayed over §3.15, these
    // are this node's own. Role-neutral zero/idle defaults elsewhere.
    std::string uplink_calib_state = "idle";  // idle|running|done|failed
    // Always 0 in v1 — the shape mirrors calib_rung so a future multi-rung
    // uplink changes no stats schema and no Hub parser.
    uint8_t uplink_calib_rung = 0;
    int32_t uplink_calib_power_qdb = 0;
    uint8_t uplink_calib_fingerprint = 0;
    bool uplink_calib_stale = false;
    // §3.16 (Pass 153) probe-exchange counters, role-neutral: the local
    // node's current/last calibration run. feed_paused is the §10.6
    // input-starve state (craft-local; false on a ground node).
    uint64_t calib_probes_sent = 0;
    uint64_t calib_tallies_rx = 0;
    uint8_t calib_rx_mcs = kUplinkRxMcsUnknown;  // 255 = unknown
    bool feed_paused = false;
};

// §15.3 cache blocks — present only when the §14.3 role is enabled.
struct CacheRepairStatsOut {
    uint64_t requests = 0;
    uint64_t replies = 0;
    uint64_t symbols_accepted = 0;
    uint64_t symbols_rejected = 0;
    uint64_t blocks_closed_deficit = 0;
    uint64_t blocks_repaired = 0;
    uint64_t blocks_futile = 0;
    uint64_t requests_suppressed = 0;
    uint64_t nack_graces_armed = 0;
    uint64_t blocks_repaired_before_nack = 0;
    uint32_t caches_fresh = 0;  // gauge
    TimingMetricStats request_to_first_reply;
    TimingMetricStats request_to_completion;
};

struct CacheStoreStatsOut {
    uint64_t requests_received = 0;
    uint64_t requests_answered = 0;
    uint64_t requests_rejected = 0;
    uint64_t symbols_sent = 0;
    uint64_t status_sent = 0;
    uint32_t blocks_held = 0;      // gauge
    uint16_t health_permille = 0;  // gauge
};

// §7.5 (Pass 183) uplink data-plane counters — one entry per uplink stream,
// appended to the §15.3 "streams" array with only its own fields. `tx` picks
// the side: true on the originating ground (submitted/sent/dropped_*), false
// on the accepting craft (accepted/rej_*/dup).
struct UplinkDataStreamStats {
    uint8_t stream_id = 0;
    std::string type;  // registry name, "CONTROL" / "TELEMETRY"
    bool tx = false;
    uint64_t submitted = 0;
    uint64_t sent = 0;
    uint64_t dropped_stale = 0;
    uint64_t dropped_budget = 0;
    uint64_t dropped_oversize = 0;
    uint64_t accepted = 0;
    uint64_t rej_unbound = 0;
    uint64_t rej_stream = 0;
    uint64_t dup = 0;
};

struct StatsSnapshot {
    uint64_t t_ms = 0;
    uint16_t node = 0;
    uint32_t session = 0;
    std::vector<AdapterStats> adapters;
    std::vector<StreamStats> streams;
    std::vector<UplinkDataStreamStats> uplink_streams;  // §7.5
    std::optional<CacheRepairStatsOut> cache_repair;
    std::optional<CacheStoreStatsOut> cache_store;
    ReturnStats ret;
    ArqTimingStats arq_timing;
    LinkStats link;
};

// Appends one §15.3 NDJSON line (terminated with '\n') to out.
void format_stats_line(const StatsSnapshot& snap, std::string& out);

// One complete §15.3 line, trailing '\n' included. NOT NUL-terminated.
//
// This exists because the local egress was hardcoded to stdout (B8, issue
// #144). A library consumer has no stdout worth reading, so it lost not log
// noise but the node's entire counter surface — everything the ground UI, the
// selector history and the bench analyzers read.
using StatsSinkFn = void (*)(void* cookie, const char* line, size_t n);

class StatsEmitter {
  public:
    // udp may be nullptr (stdout only). Neither sink is owned.
    StatsEmitter(bool to_stdout, UdpEgress* udp)
        : to_stdout_(to_stdout), udp_(udp) {}

    // Installs a local sink, REPLACING the stdout write — not adding to it, so
    // a consumer cannot accidentally double-emit, and `stats.stdout` in the
    // config keeps meaning what it meant for every node that installs nothing.
    // The UDP binding is independent and is unaffected either way.
    //
    // Call before the first emit(); the emitter is not thread-safe and neither
    // is this. Passing nullptr restores the stdout behaviour.
    void set_local_sink(StatsSinkFn fn, void* cookie) {
        sink_ = fn;
        sink_cookie_ = cookie;
    }

    void emit(const StatsSnapshot& snap);

    // The most recently emitted §15.3 line (trailing '\n' included), or empty
    // before the first emit. The §15.5 control plane re-serves this for
    // GET /stats and pushes it to SSE subscribers — no re-serialization.
    const std::string& last_line() const { return line_; }

  private:
    bool to_stdout_;
    UdpEgress* udp_;
    StatsSinkFn sink_ = nullptr;
    void* sink_cookie_ = nullptr;
    std::string line_;  // reused across emits
};

}  // namespace wblink
