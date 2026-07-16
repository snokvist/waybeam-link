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
    uint16_t demote_milli = 20;
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

// §9.6 venc bitrate actuation. Disabled by default: dev/bench runs have no
// encoder; on the craft this is the ONLY writer of video0.bitrate.
struct VencCfg {
    std::string host = "127.0.0.1:80";
    bool enabled = false;           // bitrate writes (§9.6)
    bool recovery_enabled = false;  // rate-limited IDR requests (§3.9)
    // §9.6 Pass 37 horizon frame caps (maxIBytes/maxPBytes; §17 seeds).
    bool frame_caps = true;         // cap writes (gated by `enabled` too)
    uint16_t fps_hint = 60;         // cadence fallback until measured
    uint16_t i_headroom_permille = 1000;
    uint16_t p_headroom_permille = 1000;
    uint32_t cap_ceiling_bytes = 196608;
    uint32_t settle_ms = 750;       // encoder-output settling window
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
};

// §6 RX-side knobs (all §17-overridable seeds).
struct RxCfgPolicy {
    uint32_t stall_timeout_ms = 200;   // §6.5
    uint32_t dwell_ceiling_ms = 20;    // §6.2-3
    uint8_t admit_n = 3;               // §2
    uint32_t admit_window_ms = 1000;   // §2
    uint8_t renack_attempts = 3;       // §6.4
    uint32_t renack_backoff_ms = 15;   // §6.4
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
};

struct CsaPolicy {
    // SECRET (§15.2): present only on craft+ground configs; must never appear
    // in stats, logs, or dump_config_summary().
    std::string psk;
    double settle_s = 3.0;
    uint32_t verify_timeout_ms = 150;
    uint32_t min_interval_s = 5;
    uint32_t ack_timeout_ms = 1000;
    uint32_t rendezvous_timeout_s = 5;
    uint16_t home_chan = 0;  // config-pinned rendezvous (§11.1)
    std::vector<uint16_t> channel_allowlist;
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
    // §9.10 TX-wedge watchdog (radio backend, §17 seeds). window 0 disables.
    uint32_t wedge_window_ms = 1000;
    uint32_t wedge_min_submits = 8;
    // §3.0 Pass 12: arm the TX adapter's hardware ACK responder with its
    // own SA (craft half of the gate-4 A/B). Opt-in — makes a passive
    // monitor transmit ACKs.
    bool ack_responder = false;
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
};

// §15.5 REST control plane. Empty bind = server off (default). "addr:port";
// bind 127.0.0.1 for host-local, a routable addr on a trusted net (no auth).
struct ControlCfg {
    std::string bind;  // "" = disabled
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
    uint16_t repair_fraction_permille = 200;
    uint8_t absolute_symbol_limit = 8;
    uint8_t max_cache_attempts = 2;
    uint8_t reply_limit = 4;
    uint16_t health_floor_permille = 800;
    uint32_t status_timeout_ms = 1500;
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
