// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: JSON config model (PROTOCOL.md §15.2) + profile-table
// loading (§9.3). Every policy constant is a named field defaulting to the
// spec seed value — bench re-derivation (§17) is config, not recompile.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wblink/csa.h"
#include "wblink/nal.h"
#include "wblink/table.h"
#include "wblink/types.h"

namespace wblink {

enum class Role : uint8_t { kTx, kRx };
enum class Dir : uint8_t { kIn, kOut };
// §15.1: udp + the frame-shm shm kind are live; unix stays v1-reserved.
enum class BindKind : uint8_t { kUdp, kFrameShm };

struct BindCfg {
    BindKind kind = BindKind::kUdp;
    std::string listen;  // udp: "host:port", set iff the binding is an ingress
    std::string send;    // udp: "host:port", set iff the binding is an egress
    std::string name;    // frame-shm: POSIX SHM ring name (§15.4)
};

// §14.1 per-stream FEC policy (frame-shm streams). scheme=kNone => the
// TX FrameFramer fragments + ARQs but emits no repair symbols.
struct StreamFecCfg {
    FecScheme scheme = FecScheme::kNone;
    uint16_t i_rate_permille = 250;
    uint16_t p_rate_permille = 100;
    uint16_t min_k = 3;
    uint16_t min_r = 2;  // §14.1 (Pass 98) minimum repair floor per FEC'd frame
};

struct JsccShadowCfg {
    uint16_t fec_floor_permille = 0;
    uint16_t fec_cap_permille = 0;
    uint32_t arq_guard_us = 0;
    uint32_t feedback_timeout_ms = 0;
    uint16_t min_rtt_samples = 0;
    bool enforce = false;  // §14.2 Pass 38: actuate valid decisions
};

struct NodeCfg {
    uint16_t originator = 0;
    Role role = Role::kRx;
    // §2/§13 passive spectator (Pass 74): a display node with no uplink —
    // FEC+diversity best-effort, no ARQ/returns, passive tune+latch, re-scout
    // on CSA move. Permits zero role:"tx" adapters. Fail-closed opt-in.
    bool spectator = false;
    // §3.9 Pass 106: emit a RECOVERY_REQUEST when an RTP stream first latches.
    // The link cannot observe decoder readiness, so a first latch is the only
    // bootstrap-relevant moment it can detect. Default on — the failure it
    // prevents is silent (healthy counters, black screen).
    bool recovery_on_latch = true;
    uint16_t preferred_originator = 0;  // §12 preemption; 0 = none
    // §3.0 L2 partition tag. TX always stamps it (absent ⇒ stamps 0); the
    // RX filter enforces equality only when it is configured.
    std::optional<uint8_t> net_id;
};

struct AdapterCfg {
    std::string name;
    std::string bus;
    // Linux monitor-mode netdev (e.g. "wlan0", "wlx84fc…") — the kernel-monitor
    // backend (air.kind "kernel-monitor") binds AF_PACKET to it. Empty for the
    // udp/radio backends (devourer matches on `bus`).
    std::string ifname;
    Role role = Role::kRx;
    uint16_t channel_mhz = 0;  // center freq MHz, band-agnostic (§11.1 style)
    uint8_t bw = 20;           // 20 / 40 / 80
    std::string power_map;     // §10.2 per-adapter absolute power table path
    std::optional<int32_t> max_power_qdb;  // §10.3 opt-in sanity ceiling
    // §10.3/§11.7 0x0A (Pass 135): selectable ceilings, <=5 per the §11.7
    // preset-index bound. Each entry is clamped to max_power_qdb at load, so
    // the runtime path can only ever LOWER power.
    std::vector<int32_t> power_presets_qdb;
};

struct StreamCfg {
    uint8_t stream_id = 0;
    uint8_t stream_type = 0;  // §3.4 registry value
    Dir dir = Dir::kIn;
    BindCfg bind;
    // out-streams only: optionally pin the latch to one sender (§2 v0 latch
    // policy — first admitted tuple matching type [+ originator]).
    std::optional<uint16_t> originator;
    // RTP in-streams (§4.1): "size" (default) / "h264" / "h265".
    RtpClassifier classifier = RtpClassifier::kSize;
    FrameArqMode arq_mode = FrameArqMode::kIdrOnly;
    // §14.1 FEC for frame-shm streams (ignored on udp streams).
    StreamFecCfg fec;
    std::optional<JsccShadowCfg> jscc_shadow;
};

// §9.1 cascade + §9.4/§9.5/§9.7/§9.8/§9.9 constants (seeds; RE-DERIVE per
// §17). Mirrors core SelectorPolicy; the app maps seconds -> ms.
struct SelectPolicy {
    // §17 re-derive, 2026-07-26 — see core/include/wblink/selector.h and
    // docs/step11-bench.md §4.8.
    uint16_t demote_milli = 45;
    uint16_t emergency_loss_milli = 200;  // §9.1 Pass 110
    uint32_t loss_min_uniq = 32;
    uint8_t loss_persist_score = 5;
    double rung_lockout_s = 30.0;
    uint8_t rung_lockout_latch_count = 4;
    int8_t rssi_floor_dbm = -85;
    double rssi_fade_db_per_s = 10.0;
    int8_t rssi_fade_arm_dbm = -65;
    double promote_rssi_hyst_db = 6.0;
    double promote_dwell_s = 0.5;
    double mcs_settle_s = 5.0;
    double down_cooldown_s = 0.2;
    double ewma_alpha = 0.3;
    // §9.5 sequencing + §9.7 flap layers + §9.8/§9.9 (step 8).
    double bitrate_lead_s = 0.5;
    double mcs_up_grace_s = 0.25;
    double reentry_backoff_s = 5.0;
    double reentry_dwell_s = 2.0;
    uint8_t flap_freeze_count = 3;
    double flap_freeze_window_s = 10.0;
    double flap_freeze_s = 10.0;
    double pressure_escape_s = 2.0;
    double failsafe_hold_s = 1.0;  // §9.8 gate-4 seeds
    double failsafe_step_s = 1.0;
    // §9.4 Pass-6 ruling: node-local per-rung RSSI floors.
    std::array<int8_t, 8> rung_rssi_floor_dbm{-88, -85, -83, -80,
                                              -77, -73, -71, -70};
    uint8_t min_profile = 0;    // §9.7 pin; min==max freezes adaptation
    uint8_t max_profile = 255;  // 255 = unpinned
};

// §9.11 FPS ladder (Pass 39; requires venc.enabled). Values must be §9.6
// ladder members with min <= preferred <= max; v1 commands within
// [min, preferred]. Pass 99: the ladder object is instantiated on every
// venc craft (construct != run); `enabled` sets only the BOOT run-state, so
// FPS_LADDER on/off toggles the loop at runtime with no link restart.
struct FpsLadderCfg {
    bool enabled = false;  // boot run-state (not construct-gate; Pass 99)
    uint16_t min = 60;
    uint16_t preferred = 100;
    uint16_t max = 144;
    uint32_t min_p_frame_bytes = 10000;
    uint32_t restore_hysteresis_bytes = 1000;
    uint32_t sample_timeout_ms = 500;
    uint32_t reduce_after_ms = 3000;
    uint32_t reduce_dwell_ms = 4000;
    uint32_t restore_after_ms = 8000;
    uint32_t settle_ms = 1500;
};

// §9.6 venc bitrate actuation. Disabled by default: dev/bench runs have no
// encoder; on the craft this is the ONLY writer of video0.bitrate.
struct VencCfg {
    std::string host = "127.0.0.1:80";
    bool enabled = false;           // bitrate writes (§9.6)
    bool recovery_enabled = false;  // rate-limited IDR requests (§3.9)
    // §9.6 Pass 75 encoder-capability ceiling: clamp the §9.5-derived bitrate
    // to min(derived, this). 0 = unlimited. Must be >= 1000 if set.
    uint32_t max_bitrate_kbps = 0;
    // §9.6 Pass 37 horizon frame caps (maxIBytes/maxPBytes; §17 seeds).
    bool frame_caps = true;         // cap writes (gated by `enabled` too)
    uint16_t fps_hint = 100;        // cadence fallback until measured
    uint16_t i_headroom_permille = 1000;
    uint16_t p_headroom_permille = 1000;
    uint32_t cap_ceiling_bytes = 196608;
    uint32_t settle_ms = 750;       // encoder-output settling window
    FpsLadderCfg fps_ladder;        // §9.11 (Pass 39)
    // §11.7 v2 command presets (Pass 71): ≤5 entries each, cmd_arg indexes.
    std::vector<uint16_t> preset_fps;            // §9.11 ladder members
    std::vector<std::string> preset_resolution;  // venc video0.size strings
    std::vector<std::string> preset_framing;     // venc video0.framing strings
    // §15.5 operating-mode selection (Pass 96). The link is the control
    // authority for the user-facing mode (docs/venc-mode-matrix.md §16): the
    // hub POSTs a mode name here and the link owns it. `active_mode` is the
    // label restored at boot; `mode_apply_cmd`, when set, is the on-craft
    // applier the mode endpoint forks (sensor.mode/video0.size are venc
    // restart_required, so applying a mode is a script + venc restart — the
    // range pin itself is applied live and never restarts the link/CSA).
    std::string active_mode;     // e.g. "imx335-100fps-highrange"; "" = unset
    std::string mode_apply_cmd;  // e.g. "/etc/waybeam-link/modes/apply-mode.sh"
    // §15.5 Pass 104: directory the mode catalog (GET /api/v1/modes) is
    // enumerated from. Empty = derive it from `mode_apply_cmd`'s directory (the
    // §16 layout co-locates the applier and the mode files), so a deployed
    // craft serves the catalog with no extra config.
    std::string modes_dir;
};

struct ArqPolicy {
    double airtime_frac = 0.15;
    uint8_t attempt_cap = 3;
    uint32_t holddown_ms = 20;
    uint32_t fwd_clamp_blocks = 4;  // §6.6 clamp K, in blocks
    // §5.2 ring + §5.3/§12 scheduler knobs (all §17-overridable seeds).
    uint32_t ring_window_ms = 50;
    uint32_t ring_byte_budget = 256 * 1024;
    uint32_t classifier_size_threshold = 8 * 1024;  // §4.1 size heuristic
    uint32_t release_timeout_ms = 500;              // §12 contested release
    uint32_t min_recoverable_ms = 0;                // §5.3; gate-3 measured
    uint32_t budget_interval_ms = 100;
    uint32_t budget_floor_bytes = 4096;
    uint32_t max_block_pkts = 64;  // §13 bitmap sanity clamp
    // §4.1 Pass 40: no ARQ class above this cadence (0 = no cutoff).
    uint16_t arq_max_fps = 100;
};

// §6 RX-side knobs (all §17-overridable seeds).
struct RxCfgPolicy {
    uint32_t stall_timeout_ms = 200;   // §6.5
    uint32_t dwell_ceiling_ms = 20;    // §6.2-3
    uint8_t admit_n = 3;               // §2
    uint32_t admit_window_ms = 1000;   // §2
    uint8_t renack_attempts = 3;       // §6.4
    uint32_t renack_backoff_ms = 6;    // §6.4
    uint32_t idle_teardown_ms = 5000;  // §2
    uint32_t fwd_clamp_pkts = 256;     // §6.6 seq clamp
    uint32_t clamp_resync_ms = 500;    // §6.6 sustained-clamp resync window
};

struct FecPolicy {
    FecScheme scheme = FecScheme::kNone;  // §14: none until gate 2 says so
    double overhead_frac = 0.0;
};

struct ReturnPolicy {
    // §7.2 TSF quiet-gap (seeds; RE-DERIVE at gate 4). Off by default —
    // §7.1 opportunistic return is the shipping baseline.
    bool quiet_gap = false;
    uint32_t guard_us = 300;
    uint32_t return_window_us = 2000;
    // §3.0 Pass 12: send returns as hardware-ACKed unicast QoS-Data to the
    // target's latched SA (ground half of the gate-4 A/B; craft half is
    // air.ack_responder). Off = pinned broadcast returns.
    bool unicast = false;
    // §7.2 Pass 78: anchored LINK_REPORT batches repeat once at the next
    // return window (spread across two listen gaps). 1 disables.
    uint32_t report_redundancy = 2;
};

struct CsaPolicy {
    // SECRET (§15.2): present only on craft+ground configs; must never appear
    // in stats, logs, or dump_config_summary().
    std::string psk;
    double settle_s = 3.0;
    // §11.5 (Pass 92): DERIVED, never a second literal. csa_params() copies
    // this over the engine's own default unconditionally, so restating the
    // number here silently overrides core — which is exactly how Pass 89's
    // 150 -> 500 ruling failed to reach a single running binary.
    uint32_t verify_timeout_ms = kCsaVerifyTimeoutMsDefault;
    // §11.6 Pass 80: post-retune RX-liveness deadline (0 disables). Silence
    // for this long after a CSA retune => one full monitor re-init. §15.2
    // (Pass 92): MUST exceed verify_timeout_ms — a verify window that outlives
    // this guard lets a monitor re-init fire mid-switch. Enforced at load.
    uint32_t rx_liveness_ms = 750;
    uint32_t min_interval_s = 5;
    uint32_t ack_timeout_ms = 1000;
    uint32_t bind_release_s = 90;    // §11.5a command-source binding release
    bool persist_channel = false;    // §11.5 boot onto last-committed channel
    uint16_t home_chan = 0;  // config-pinned power-on default (§11.1, §11.5)
    std::vector<uint16_t> channel_allowlist;
};

// §15.2 policy.cmd — the §11.7 VEHICLE_CMD campaign seeds (§17 RE-DERIVE).
struct CmdPolicy {
    uint32_t copies = 3;
    uint32_t copy_interval_ms = 20;
    uint32_t echo_copies = 2;
    uint32_t ack_timeout_ms = 1000;
    uint32_t retry_cap = 3;
    uint32_t min_interval_ms = 250;
};

// §10.6 (Pass 120) craft-resident calibration seeds — mirrors
// core CalibrateParams plus the artifact directory.
struct CalibrationPolicy {
    int loss_ok_milli = 15;
    int loss_bad_milli = 50;
    int seek_step_qdb = 16;  // Pass 121 max-power seek
    int rssi_guard_dbm = -6;
    int min_qdb = 4;
    int max_qdb = 108;
    // Pass 132: 800 -> 300. It was sized as TXAGC settle plus one report
    // window, and the report-window term was carrying the §10.7 case where a
    // dwell had to wait for the cadence to produce a sample. A burst starts
    // emitting the instant settle ends, so only the TXAGC term is real; §10.6
    // still covers its own 100 ms window inside the 300.
    int settle_ms = 300;
    int probe_dwell_ms = 1200;
    int verify_dwell_ms = 2500;
    int report_loss_abort_ms = 3000;
    int hard_cap_ms = 600000;
    // §10.6 (Pass 134): whole-run accepted-report rate floor. The 3 s abort
    // above catches SILENCE; this catches a return path at HALF rate, which
    // reads as fewer observed losses and places every rung at its ceiling.
    // Seeded well under the 10 Hz nominal so ordinary dwell-edge gaps and
    // bounded overload blackouts never trip it.
    int calib_min_report_hz = 6;
    std::string artifact_dir = "/etc/waybeam-link/calibration";
    // §10.7 (Pass 125) ground-uplink gates. The walls, step, min/max, settle
    // and artifact_dir above are shared; only these differ, because the uplink
    // measures sparse LINK_REPORT epochs instead of live video. They are
    // EPOCH COUNTS, not milliseconds: a slow report cadence must lengthen the
    // run, never let an unobserved dwell score as clean. The craft-only ms
    // dwells and report_loss_abort_ms are unused on the ground.
    // Pass 132 burst sizes. 100 puts one lost probe at 10permille (inside
    // loss_ok_milli) and five at 50permille (the bad wall). The old 40 put one
    // loss at 25permille, BETWEEN the walls, which is the only reason
    // `uplink_ambiguous_epochs` existed — it is retired, and a config still
    // carrying the key simply loads with it ignored.
    int uplink_probe_epochs = 100;
    int uplink_verify_epochs = 200;
    // Silence after a burst so the craft's counter reflects all of it before
    // scoring. Must exceed one §3.16 period (500 ms at 2 Hz).
    int uplink_drain_ms = 600;
    int uplink_liveness_ms = 2000;
};

struct Policy {
    double report_hz = 10.0;
    uint32_t report_timeout_ms = 500;
    SelectPolicy select;
    ArqPolicy arq;
    RxCfgPolicy rx;
    FecPolicy fec;
    ReturnPolicy ret;  // JSON key "return" (C++ keyword)
    CsaPolicy csa;
    CmdPolicy cmd;
    CalibrationPolicy calibration;
};

// Air backends. "udp" is dev tooling (one UDP socket = one virtual adapter,
// NOT §15); "radio" is the devourer path (§3.0) — its adapters come from
// the top-level adapters array, nothing is duplicated here.
struct AirUdpCfg {
    std::vector<std::string> tx;  // frame targets (tx: video; rx: NACKs)
    std::vector<std::string> rx;  // listen sockets = virtual adapters
    // Bench-only per-adapter synthetic RX drop (0–1000), parity with the
    // monitor/radio backends — manufactures known loss on the udp-air path.
    uint16_t rx_drop_permille = 0;
    bool broadcast = false;
    uint16_t originator = 0;  // self-filter identity in broadcast mode
    uint32_t pace_mbps = 0;   // broadcast serialization rate; 0 = unpaced
};
struct AirCfg {
    // kMonitor = the kernel-driver monitor-mode backend (AF_PACKET raw inject +
    // radiotap RX, §3.0). Like kRadio, its adapters come from the top-level
    // adapters array (each carrying an `ifname`); it reuses rx_drop_permille.
    enum class Kind : uint8_t { kNone, kUdp, kUdpBroadcast, kRadio, kMonitor };
    Kind kind = Kind::kNone;
    AirUdpCfg udp;
    // Bench-only synthetic RX loss on the radio backend: drop this many
    // permille of filter-passed frames, independently per adapter (gate-2/3
    // exercise without physical fades). 0 = off (the shipping default).
    uint16_t rx_drop_permille = 0;
    // §14.2 kernel-monitor effective serialization calibration. Zero keeps
    // transport airtime unknown and JSCC in authored fixed-policy fallback.
    uint16_t airtime_efficiency_permille = 0;
    // §9.10 TX-wedge watchdog (radio backend, §17 seeds). window 0 disables.
    uint32_t wedge_window_ms = 1000;
    uint32_t wedge_min_submits = 8;
    // §3.0 Pass 12: arm the TX adapter's hardware ACK responder with its
    // own SA (craft half of the gate-4 A/B). Opt-in — makes a passive
    // monitor transmit ACKs.
    bool ack_responder = false;
    // §10.7 (Pass 125) the rx-node's uplink operating point. Before this an
    // rx node never called set_tx_mode at all and rode the TxRate struct
    // default, which happens to be exactly these seeds — so committed
    // behaviour is unchanged. What changes is that the rung is ASSERTED:
    // §10.7 records it in the calibration artifact and §3.16's last_rx_mcs
    // cross-checks it, and neither means anything against a default nobody
    // chose. A future multi-rung uplink widens these values, not the
    // mechanism.
    uint8_t uplink_mcs = 0;
    bool uplink_sgi = false;
    uint8_t uplink_bw = 20;
};

// Loopback-mode synthetic loss (§16.2). Dev tooling, not §15.
struct LoopbackCfg {
    uint8_t adapters = 2;
    uint64_t seed = 1;
    double correlation = 0.0;
    double uniform_p = 0.0;
    // Gilbert-Elliott, mirrors core GeParams; nullopt = uniform only.
    std::optional<std::array<double, 4>> ge;  // p_gb, p_bg, loss_g, loss_b
    double return_loss_p = 0.0;  // loss on the NACK return direction
    // Synthetic RSSI fed to the reporter so the §9 selector can be
    // exercised without radios; the optional fade window scripts a dip.
    int8_t rssi_dbm = -60;
    struct RssiFade {
        uint64_t start_ms = 0;
        uint64_t end_ms = 0;
        int8_t dbm = -90;
    };
    std::optional<RssiFade> rssi_fade;
};

struct StatsCfg {
    double hz = 1.0;
    std::optional<BindCfg> bind;  // egress only
    // §15.3 stdout NDJSON. Default true (the documented bench/dev stream). Set
    // false on production nodes to stop the unrotated /tmp (craft) or journald
    // (ground) growth — the REST/SSE stats plane (§15.5) is unaffected, so the
    // hub keeps scraping while the raw stream is silenced. A troubleshooting
    // toggle: flip it true when a node needs the on-box stream again.
    bool to_stdout = true;
};

// §15.5 REST control plane. Empty bind = server off (default). "addr:port";
// bind 127.0.0.1 for host-local, a routable addr on a trusted net (no auth).
struct ControlCfg {
    std::string bind;  // "" = disabled
};

// §15.5a ground scout (channel searcher). channels empty ⇒ sweep the
// csa.channel_allowlist. All values §17-overridable seeds.
struct ScoutCfg {
    uint32_t dwell_ms = 300;
    std::vector<uint16_t> channels;
};

// §14.3 Cache Controller (both roles default off; all values §17 seeds).
struct CacheEndpointCfg {
    uint16_t originator = 0;
    std::string endpoint;  // "host:port"
};
struct CacheRepairCfg {
    bool enabled = false;
    uint8_t stream_id = 0;  // must name a frame-shm egress stream
    std::string listen;     // reply/status RX socket
    std::vector<CacheEndpointCfg> caches;
    uint32_t tail_grace_ms = 1;
    uint32_t local_quiet_ms = 2;
    uint32_t min_collect_ms = 4;
    uint32_t hard_close_ms = 8;
    uint32_t request_timeout_ms = 4;
    uint32_t nack_grace_ms = 3;
    uint16_t repair_fraction_permille = 200;
    uint8_t absolute_symbol_limit = 8;
    uint8_t max_cache_attempts = 2;
    uint8_t reply_limit = 4;
    uint16_t health_floor_permille = 800;
    uint32_t status_timeout_ms = 1500;
    uint32_t assignment_interval_ms = 500;
};
struct CacheStoreCfg {
    bool enabled = false;
    std::string listen;
    std::vector<uint8_t> stream_ids;
    uint16_t blocks = 96;
    uint8_t reply_limit = 4;
    std::vector<std::string> status_to;  // aggregator endpoints
    uint32_t status_interval_ms = 500;
    uint16_t max_requests_per_s = 400;
    // Pass 67: optional single owning receiver. Its endpoint is the exact
    // source address of CACHE_ASSIGN datagrams (normally repair.listen).
    std::optional<CacheEndpointCfg> controller;
};
struct CacheCfg {
    CacheRepairCfg repair;
    CacheStoreCfg store;
};

struct Config {
    NodeCfg node;
    std::string profile_table_path;
    std::vector<AdapterCfg> adapters;
    std::vector<StreamCfg> streams;
    Policy policy;
    StatsCfg stats;
    ControlCfg control;    // §15.5 REST control plane (off unless bind set)
    ScoutCfg scout;        // §15.5a ground scout (channel searcher)
    CacheCfg cache;        // §14.3 cache repair/store (off by default)
    VencCfg venc;          // §9.6 encoder actuation
    AirCfg air;            // dev backend; empty until devourer lands
    LoopbackCfg loopback;  // loopback-mode loss injection
};

// Minimal expected-style result (C++20 has no std::expected).
template <typename T>
struct Result {
    std::optional<T> value;
    std::string error;
    explicit operator bool() const { return value.has_value(); }
    static Result ok(T v) { return Result{std::move(v), {}}; }
    static Result fail(std::string e) { return Result{std::nullopt, std::move(e)}; }
};

// Parse + validate (§15.1 rules: one binding per stream, in XOR out,
// UDP-only in v0, <=4 UDP bindings node-wide including the stats binding).
Result<Config> load_config_json(const std::string& json_text);
Result<Config> load_config(const std::string& path);

// Profile-table JSON (profiles/table.example.json format). Validates unique
// ids, floor_profile exists, bitrate_min_kbps >= 1000 (venc hard floor,
// §9.6), fractions within [0,1]. Fractions are scaled per §3.6
// (llround(frac*1000)) into the integer per-mille fields core hashes.
Result<ProfileTable> load_profile_table_json(const std::string& json_text);
Result<ProfileTable> load_profile_table(const std::string& path);

// Human-readable one-screen summary; csa.psk is redacted, never printed.
std::string dump_config_summary(const Config& cfg);

}  // namespace wblink
