// SPDX-License-Identifier: GPL-2.0-or-later
// §10.3/§10.5/§10.7/§11.7 0x0A — the ground uplink's power owner
// (#109 Phase 2c).
//
// Moved out of `app/main.cpp` unchanged. It was already built for injection —
// `apply_qdb`, `apply_auto` and `artifact_qdb` are all callbacks — which is
// why the whole precedence chain is reachable from a test with no radio, no
// file and no socket. Until now the only way to reach it was to `#include`
// the whole of `app/main.cpp`, which is how the two order-dependent tier
// defects found during Pass 166/167 verification stayed invisible to CI.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "wblink/power.h"

namespace wblink {
namespace node {

inline std::string power_tier_json(int tier, const std::vector<int32_t>& presets,
                                   std::optional<int32_t> ceiling,
                                   bool effective) {
    std::string s = "{\"tier\":" + std::to_string(tier) + ",\"presets_qdb\":[";
    for (size_t i = 0; i < presets.size(); ++i) {
        if (i != 0) s += ",";
        s += std::to_string(presets[i]);
    }
    s += "],\"ceiling_qdb\":";
    s += ceiling ? std::to_string(*ceiling) : "null";
    // §10.3 (Pass 134): a tier on a node with no curve is recorded and moves
    // nothing. Report that rather than implying the setting reached hardware.
    s += ",\"effective\":";
    s += effective ? "true" : "false";
    s += "}";
    return s;
}

// §10.3/§10.5/§10.7/§11.7 0x0A — the ground uplink's power owner (Pass 138).
//
// ONE precedence path: §10.5 latch, then an explicit configured map, then a
// matching artifact, then backend auto. run_rx spelled that ordering out FOUR
// times — the actuation, the §15.3 fill, the startup log, and the §15.5
// `effective` flag — under a comment warning that "a second copy of this
// ordering is how the two drift". Two copies had in fact already drifted, and
// both were Pass 136 bug fixes: `effective` claimed a tier bound hardware when
// it did not, and the tier's own apply path reached no actuator at all in two
// of three configurations. The hazard was documented rather than removed; this
// removes it.
//
// Actuators are injected, exactly as TxCore does with apply_power, so every
// rule below is reachable from a test with no radio, no file and no socket.
struct UplinkPower {
    enum class Owner { kNone, kOverride, kConfigMap, kArtifact };

    std::function<void(int32_t qdb)> apply_qdb;
    std::function<void()> apply_auto;
    // §10.7 applicability is a function of the pairing tuple, which lives in
    // run_rx and changes with selection/CSA — so this stays a callback. It
    // returns an already-ceiling-clamped value, and has the side effect of
    // updating the stale flag; both are properties of its own resolve.
    std::function<std::optional<int32_t>()> artifact_qdb;

    std::vector<int32_t> presets_qdb;
    std::optional<int32_t> ceiling_qdb;   // §10.3, moved by a tier
    std::optional<PowerCurve> curve;      // §10.2 config power_map, if any
    uint8_t mcs = 0;                      // the uplink's fixed operating point
    int tier = -1;                        // §11.7 0x0A, -1 = unset
    std::optional<int32_t> owner_qdb;     // curve resolve under ceiling_qdb
    std::optional<int32_t> override_qdb;  // §10.5 latch (volatile)

    std::optional<int32_t> artifact() const {
        return artifact_qdb ? artifact_qdb() : std::nullopt;
    }

    // Owner + the value that owner REQUESTS, resolved together in the one
    // place the precedence is written. Walking the chain separately per
    // accessor is how run_rx ended up with four copies of it, and the first
    // draft of this struct promptly grew three of its own.
    //
    // Resolving once also matters because artifact() is not free: it re-runs
    // the §10.7 pairing check and updates the stale flag as a side effect.
    struct Resolved {
        Owner owner = Owner::kNone;
        std::optional<int32_t> qdb;
    };
    Resolved resolve() const {
        if (override_qdb) return {Owner::kOverride, override_qdb};
        if (owner_qdb) return {Owner::kConfigMap, owner_qdb};
        if (const std::optional<int32_t> a = artifact()) {
            return {Owner::kArtifact, a};
        }
        return {};
    }

    Owner owner() const { return resolve().owner; }

    const char* owner_name() const {
        switch (owner()) {
            case Owner::kOverride: return "§10.5 override";
            case Owner::kConfigMap: return "config power_map";
            case Owner::kArtifact: return "artifact";
            case Owner::kNone: break;
        }
        return "backend auto";
    }

    // What §15.3 REPORTS. §10.5 is explicit that a latch reports the REQUEST,
    // not the clamped value the hardware got.
    std::optional<int32_t> reported_qdb() const { return resolve().qdb; }

    // What the ACTUATOR receives. Only the latch needs clamping here: the
    // curve resolve takes the ceiling as an argument and the artifact resolve
    // clamps at its source, so clamping them twice would be a no-op that
    // invited someone to "simplify" one of the three away.
    std::optional<int32_t> hw_qdb() const {
        const Resolved r = resolve();
        if (r.owner == Owner::kOverride && ceiling_qdb) {
            return std::min(*r.qdb, *ceiling_qdb);
        }
        return r.qdb;
    }

    // The single convergence point. Every §10.5/§10.7/§11.7 path ends here.
    void apply() {
        if (const std::optional<int32_t> q = hw_qdb()) {
            if (apply_qdb) apply_qdb(*q);
        } else if (apply_auto) {
            apply_auto();
        }
    }


    // §10.3 Pass 134/136: a ceiling binds only where a number of ours reaches
    // the actuator — a curve, an artifact, or a held latch clamped by it.
    bool effective() const { return owner() != Owner::kNone; }

    // Re-resolve the configured map under the CURRENT ceiling. Resolved once
    // at startup and never again, this pinned the boot ceiling for the life of
    // the process, so a tier could not lower a power_map-owned uplink.
    void resolve_owner() {
        if (!curve) return;
        owner_qdb =
            resolve_power_qdb(*curve, mcs, kPowerLevelBaseline, ceiling_qdb);
    }

    // §11.7 0x0A. false = REJECTED (no list, or index past its end). The
    // calibrator's sweep bound is the CALLER's to move — it is not power.
    bool set_tier(int t) {
        if (t < 0 || static_cast<size_t>(t) >= presets_qdb.size()) return false;
        tier = t;
        ceiling_qdb = presets_qdb[static_cast<size_t>(t)];
        resolve_owner();
        apply();
        return true;
    }

    std::string json() const {
        return power_tier_json(tier, presets_qdb, ceiling_qdb, effective());
    }
};

}  // namespace node
}  // namespace wblink
