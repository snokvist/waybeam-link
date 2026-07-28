// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §9 TX-side adaptive selector — the decision cascade,
// transition sequencing, flap layers, and fail-safe, over the §9.3 profile
// ladder. RX reports (§7.3), the TX decides and actuates (§9.1).
//
//  - Cascade, first match wins (§9.1): acute loss / persistent loss /
//    RSSI-floor / RSSI-fade / backpressure-escape / promote / hold.
//  - §9.2 Pass 110: recurrent loss locks the vacated rung. Timed strikes
//    eventually latch for the current RF environment; promotion never probes
//    across the lowest active lockout.
//  - §9.4 v0 promote: RSSI margin over the node-local per-rung floor
//    (Pass-6 ruling: rung_rssi_floor_dbm config array, not the wire table).
//  - §9.5 sequencing: demote = bitrate leads by bitrate_lead_ms, then the
//    MCS/power commit; promote = commit first, bitrate after mcs_up_grace_ms.
//    Pass 110's qualified classifiers supersede the legacy EWMA settle gate.
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
    // §9.1 cascade (Pass 110; seeds are §17 RE-DERIVE).
    // §17: re-derived 2026-07-26 (bench, craft .232) — 20 demoted off the top
    // rung on a real ~2-4% loss ceiling at excellent RSSI on a DFS channel
    // (5220 MHz), well under any decode-error threshold; 45 held zero demotes
    // over 30s at the same loss level. See docs/step11-bench.md §4.8.
    uint16_t demote_milli = 45;
    uint16_t emergency_loss_milli = 200;
    uint32_t loss_min_uniq = 32;
    uint8_t loss_persist_score = 5;
    uint32_t rung_lockout_ms = 30000;
    uint8_t rung_lockout_latch_count = 4;
    int8_t rssi_floor_dbm = -85;
    double rssi_fade_db_per_s = 10.0;  // demote when slope <= -this
    int8_t rssi_fade_arm_dbm = -65;
    uint8_t fade_ticks = 3;
    uint32_t down_cooldown_ms = 200;
    double ewma_alpha = 0.3;
    double slope_alpha = 0.5;
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
    uint32_t max_bitrate_kbps = 0;  // §9.6 Pass 75 encoder ceiling; 0 = off
    // §9.8 (gate-4 seeds)
    uint32_t report_timeout_ms = 500;
    uint32_t failsafe_hold_ms = 1000;
    uint32_t failsafe_step_ms = 1000;
    // §9.9
    uint32_t pressure_escape_ms = 2000;
};

// §9.1 Pass 110 transition cause. Numeric values are the §3.15 wire registry.
enum class SelectorReason : uint8_t {
    kNone = 0,
    kBoot = 1,
    kLossEmergency = 2,
    kLossPersistent = 3,
    kReportTimeout = 4,
    kRssiFloor = 5,
    kRssiFade = 6,
    kBackpressure = 7,
    kPromote = 8,
    kRepin = 9,
    kEnvironmentReset = 10,
};

const char* selector_reason_name(SelectorReason reason);

struct SelectorLockout {
    bool active = false;
    bool latched = false;
    bool conflict = false;
    uint8_t profile = 0xFF;
    uint8_t ceiling_profile = 0;
    uint8_t strikes = 0;
    uint32_t remaining_ms = 0;
    uint8_t active_mask = 0;
    uint8_t latched_mask = 0;
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

// §9.6 Pass 75: clamp a derived bitrate to an encoder-capability ceiling
// (`max_kbps` 0 = unlimited). Rung-independent — applied to every derived
// target before actuation/reporting/cap coupling.
uint32_t clamp_bitrate_kbps(uint32_t derived_kbps, uint32_t max_kbps);

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

    // Pass 110 environmental reset. The first tuple establishes the baseline;
    // a later successful (channel,bw) change clears channel-conditioned state
    // without changing the active profile/bitrate or report anti-replay epoch.
    bool on_rf_environment(uint16_t channel_mhz, uint8_t bw,
                           uint64_t now_ms);

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
    // The next evaluate() honours it via clamp_rung() — no restart. Pass 100: a
    // range re-pin whose envelope excludes the current rung SNAPS into [min,max]
    // (down-clamp unconditional; up-clamp defers to §9.8 on stale feedback).
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
    SelectorReason reason() const { return reason_; }
    uint16_t loss_window_milli() const { return loss_window_milli_; }
    uint16_t loss_ewma_milli() const;
    uint32_t loss_uniq() const { return loss_uniq_; }
    uint8_t loss_score() const;
    uint8_t safe_floor_profile() const;
    SelectorLockout lockout(uint64_t now_ms) const;

  private:
    enum class Phase : uint8_t { kBoot, kIdle, kBitrateLead, kMcsGrace };

    struct RungLossState {
        uint8_t score = 0;
        uint8_t strikes = 0;
        uint64_t blocked_until_ms = 0;
        bool latched = false;
    };

    size_t ladder_size() const;
    size_t clamp_rung(size_t r) const;  // min/max_profile pins (profile IDs)
    size_t rung_of_id(uint8_t id, size_t fallback) const;  // §9.7 id -> rung
    size_t floor_rung() const;
    size_t safe_floor_rung() const;
    size_t lockout_ceiling_rung(uint64_t now_ms, size_t lo, size_t hi,
                                bool* conflict = nullptr) const;
    bool rung_locked(size_t rung, uint64_t now_ms) const;
    bool rssi_guard_active() const;
    void start_demote(size_t target, uint64_t now_ms, SelectorActions& a,
                      SelectorReason reason, bool charge_rung = false);
    void start_promote(size_t target, uint64_t now_ms, SelectorActions& a,
                       SelectorReason reason);
    void charge_lockout(size_t rung, uint64_t now_ms);
    void reset_environment_state(uint64_t now_ms);
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
    bool have_smoothing_ = false;
    double rssi_ewma_ = 0.0;
    double loss_ewma_milli_ = 0.0;
    double slope_ewma_ = 0.0;  // dB/s
    double prev_rssi_ = 0.0;
    uint64_t prev_report_ms_ = 0;
    uint8_t fade_ticks_ = 0;

    // §9.1/§9.2 Pass 110 raw-window classifier and per-rung lockout.
    std::vector<RungLossState> rung_loss_;
    uint16_t loss_window_milli_ = 0;
    uint32_t loss_uniq_ = 0;
    size_t loss_observed_rung_ = 0;
    bool emergency_pending_ = false;

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
    std::optional<std::pair<uint16_t, uint8_t>> rf_environment_;

    const char* state_ = "BOOT";
    SelectorReason reason_ = SelectorReason::kBoot;
};

}  // namespace wblink
