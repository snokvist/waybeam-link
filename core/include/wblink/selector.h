// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §9 TX-side adaptive selector — the decision cascade,
// transition sequencing, flap layers, and fail-safe, over the §9.3 profile
// ladder. RX reports (§7.3), the TX decides and actuates (§9.1).
//
//  - Cascade, first match wins (§9.1): reactive-demote / RSSI-floor /
//    RSSI-fade / backpressure-escape / promote / hold.
//  - §9.2 max-probability rung: one success-EWMA per rung; unvisited rungs
//    age toward a physics prior (lower MCS ⇒ higher delivery probability);
//    multi-rung demotes jump TOWARD the max-prob rung, not blind current−1.
//  - §9.4 v0 promote: RSSI margin over the node-local per-rung floor
//    (Pass-6 ruling: rung_rssi_floor_dbm config array, not the wire table).
//  - §9.5 sequencing: demote = bitrate leads by bitrate_lead_ms, then the
//    MCS/power commit; promote = commit first, bitrate after mcs_up_grace_ms;
//    mcs_settle_ms suppresses the loss rule after any move.
//  - §9.5 budget (Pass-6 ruling): the per-rung bitrate is DERIVED —
//    HT20 PHY rate × airtime fraction × (1 − FEC overhead) − reserves,
//    floored at bitrate_min_kbps, integer math.
//  - §9.7 flap layers: soft reentry, hard flap-freeze, min==max pin.
//  - §9.8 fail-safe: report_epoch watchdog; stale feedback NEVER promotes;
//    damped step-down toward the table's floor profile. failsafe_hold_ms /
//    failsafe_step_ms are §17 seeds re-derived at bench gate 4.
//  - §9.9: local pressure suppresses the loss rule only; sustained pressure
//    with clean RF escapes upward one rung per down_cooldown.
//
// Report source identity is pre-filtered by ReportGate (§3.5 Pass 41). The
// selector tracks the accepted (originator, session) so a reporter reboot or
// silence-based re-latch resets epoch/smoothing state instead of looking like
// a replay from the previous identity.
//
// Pure tick-driven: time injected, actions returned for the caller to
// actuate (framer operating point, scheduler budgets, venc HTTP, adapter
// power). No clocks, no sockets, no floats on any wire-visible path.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

#include "wblink/table.h"
#include "wblink/wire.h"

namespace wblink {

struct SelectorPolicy {
    // §9.1 cascade (seeds; RE-DERIVE flags per spec).
    uint16_t demote_milli = 20;
    int8_t rssi_floor_dbm = -85;
    double rssi_fade_db_per_s = 10.0;  // demote when slope <= -this
    int8_t rssi_fade_arm_dbm = -65;
    uint8_t fade_ticks = 3;
    uint32_t down_cooldown_ms = 200;
    double ewma_alpha = 0.3;
    double slope_alpha = 0.5;
    // §9.2
    double rung_age_rate = 0.05;  // per-report drift toward the physics prior
    // §9.4 (Pass-6: node-local floors, one per rung index)
    std::array<int8_t, 8> rung_rssi_floor_dbm{-88, -85, -83, -80,
                                              -77, -73, -71, -70};
    double promote_rssi_hyst_db = 6.0;
    uint32_t promote_dwell_ms = 500;
    // §9.5
    uint32_t bitrate_lead_ms = 500;
    uint32_t mcs_up_grace_ms = 250;
    uint32_t mcs_settle_ms = 5000;
    // §9.7
    uint32_t reentry_backoff_ms = 5000;
    uint32_t reentry_dwell_ms = 2000;
    uint8_t flap_freeze_count = 3;
    uint32_t flap_freeze_window_ms = 10000;
    uint32_t flap_freeze_ms = 10000;
    uint8_t min_profile = 0;    // §9.7 pin (indexes into the ladder by id)
    uint8_t max_profile = 255;  // 255 = unpinned top
    // §9.8 (gate-4 seeds)
    uint32_t report_timeout_ms = 500;
    uint32_t failsafe_hold_ms = 1000;
    uint32_t failsafe_step_ms = 1000;
    // §9.9
    uint32_t pressure_escape_ms = 2000;
};

struct ProfileCommit {
    uint8_t profile_id = 0;
    uint8_t mcs = 0;
    GuardInterval gi = GuardInterval::kLong;
    uint8_t tx_power_level = 0;
};

// Drained from tick(): at most one commit and one bitrate move per tick.
struct SelectorActions {
    std::optional<ProfileCommit> commit;
    std::optional<uint32_t> bitrate_kbps;
};

// §9.5 budget law (Pass-6): derived per-rung bitrate target, integer only.
uint32_t derive_bitrate_kbps(const Profile& p);

class Selector {
  public:
    // The table is the §9.3 ladder in vector order (ids ascending). The
    // selector starts at floor_profile and works upward — fail-safe start.
    Selector(const SelectorPolicy& policy, const ProfileTable* table);

    // Returns false only for a non-forward epoch within the current accepted
    // reporter identity. Caller uses this to keep downstream consumers from
    // treating a replay as fresh evidence.
    bool on_report(const LinkReport& r, uint64_t now_ms);
    void set_pressure(bool on, uint64_t now_ms);  // §9.9 gauge
    SelectorActions tick(uint64_t now_ms);

    // §11.3 CSA freeze: no demote/promote and the §9.8 watchdog is paused
    // until `until_ms` — the retune blackout + re-acquire silence must not
    // trip a spurious fail-safe descent for a healthy switch. Extends only
    // (a later CSA can lengthen an active freeze, never shorten it).
    void csa_freeze(uint64_t until_ms) {
        if (until_ms > csa_freeze_until_ms_) {
            csa_freeze_until_ms_ = until_ms;
        }
    }

    // §9.7 live profile pin (control plane §15.5). Clamps the operating-point
    // ladder to [min, max] by profile id; min==max freezes, max==255 unpins.
    // The next evaluate() honours it via clamp_rung() — no restart.
    void set_profile_pin(uint8_t min_profile, uint8_t max_profile) {
        policy_.min_profile = min_profile;
        policy_.max_profile = max_profile;
    }

    // Observability (§9.8 "observable", §15 stats link{}).
    const char* state() const { return state_; }
    uint8_t profile_id() const;
    uint8_t mcs() const;
    uint8_t tx_power_level() const;
    uint32_t bitrate_kbps() const { return bitrate_kbps_; }
    uint32_t report_epoch() const { return last_epoch_; }
    // 0 before the first report; saturates rather than underflows.
    uint64_t report_age_ms(uint64_t now_ms) const;
    bool flap_frozen(uint64_t now_ms) const;

  private:
    enum class Phase : uint8_t { kBoot, kIdle, kBitrateLead, kMcsGrace };

    size_t ladder_size() const;
    size_t clamp_rung(size_t r) const;  // min/max_profile pins
    size_t floor_rung() const;
    size_t max_prob_rung() const;
    bool rssi_guard_active() const;
    void start_demote(size_t target, uint64_t now_ms, SelectorActions& a);
    void start_promote(size_t target, uint64_t now_ms, SelectorActions& a);
    void note_demote_for_flap(uint64_t now_ms);
    void evaluate(uint64_t now_ms, SelectorActions& a);

    SelectorPolicy policy_;
    const ProfileTable* table_;

    size_t rung_ = 0;  // current ladder index
    Phase phase_ = Phase::kBoot;
    size_t pending_target_ = 0;
    uint64_t phase_deadline_ms_ = 0;

    uint64_t last_change_ms_ = 0;  // settle + dwell anchor
    uint64_t last_demote_ms_ = 0;  // down_cooldown
    uint32_t bitrate_kbps_ = 0;

    // Report intake (§9.8 watchdog + smoothing).
    std::optional<std::pair<uint16_t, uint32_t>> report_source_;
    uint32_t last_epoch_ = 0;
    uint64_t last_report_ms_ = 0;
    bool have_report_ = false;
    double rssi_ewma_ = 0.0;
    double loss_ewma_milli_ = 0.0;
    double slope_ewma_ = 0.0;  // dB/s
    double prev_rssi_ = 0.0;
    uint64_t prev_report_ms_ = 0;
    uint8_t fade_ticks_ = 0;

    // §9.2 per-rung success EWMAs.
    std::vector<double> rung_prob_;

    // §9.7 flap machinery.
    std::deque<uint64_t> demote_times_;
    uint64_t freeze_until_ms_ = 0;
    std::vector<uint64_t> demoted_from_ms_;  // per rung, 0 = never
    std::vector<uint64_t> promoted_into_ms_;

    // §9.8 / §9.9 / §11.3.
    uint64_t csa_freeze_until_ms_ = 0;
    uint64_t failsafe_next_step_ms_ = 0;
    bool pressure_ = false;
    uint64_t pressure_since_ms_ = 0;

    const char* state_ = "BOOT";
};

}  // namespace wblink
