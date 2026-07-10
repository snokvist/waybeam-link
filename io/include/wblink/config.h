// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: JSON config model (PROTOCOL.md §15.2) + profile-table
// loading (§9.3). Every policy constant is a named field defaulting to the
// spec seed value — bench re-derivation (§17) is config, not recompile.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wblink/table.h"
#include "wblink/types.h"

namespace wblink {

enum class Role : uint8_t { kTx, kRx };
enum class Dir : uint8_t { kIn, kOut };
// §15.1: shm/unix are v1; v0 is UDP-only and the loader rejects other kinds.
enum class BindKind : uint8_t { kUdp };

struct BindCfg {
    BindKind kind = BindKind::kUdp;
    std::string listen;  // "host:port", set iff the binding is an ingress
    std::string send;    // "host:port", set iff the binding is an egress
};

struct NodeCfg {
    uint16_t originator = 0;
    Role role = Role::kRx;
    uint16_t preferred_originator = 0;  // §12 preemption; 0 = none
};

struct AdapterCfg {
    std::string name;
    std::string bus;
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
};

// §9.1 cascade + §9.4/§9.5/§9.7 constants (seeds; RE-DERIVE per §17).
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
};

struct ArqPolicy {
    double airtime_frac = 0.15;
    uint8_t attempt_cap = 3;
    uint32_t holddown_ms = 20;
    uint32_t fwd_clamp_blocks = 4;  // §6.6 clamp K, in blocks
};

struct FecPolicy {
    FecScheme scheme = FecScheme::kNone;  // §14: none until gate 2 says so
    double overhead_frac = 0.0;
};

struct ReturnPolicy {
    uint32_t guard_us = 300;
    uint32_t return_window_us = 2000;
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
    FecPolicy fec;
    ReturnPolicy ret;  // JSON key "return" (C++ keyword)
    CsaPolicy csa;
};

struct StatsCfg {
    double hz = 1.0;
    std::optional<BindCfg> bind;  // egress only
};

struct Config {
    NodeCfg node;
    std::string profile_table_path;
    std::vector<AdapterCfg> adapters;
    std::vector<StreamCfg> streams;
    Policy policy;
    StatsCfg stats;
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
