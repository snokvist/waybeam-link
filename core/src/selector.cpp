// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/selector.h"

#include <algorithm>
#include <limits>

namespace wblink {

namespace {

// HT20 PHY rates, kbps, long GI, MCS0–7 (§9.5 Pass-6 budget law).
constexpr uint32_t kHt20LgiKbps[8] = {6500,  13000, 19500, 26000,
                                      39000, 52000, 58500, 65000};

}  // namespace

const char* selector_reason_name(SelectorReason reason) {
    switch (reason) {
        case SelectorReason::kNone:
            return "NONE";
        case SelectorReason::kBoot:
            return "BOOT";
        case SelectorReason::kLossEmergency:
            return "LOSS_EMERGENCY";
        case SelectorReason::kLossPersistent:
            return "LOSS_PERSISTENT";
        case SelectorReason::kReportTimeout:
            return "REPORT_TIMEOUT";
        case SelectorReason::kRssiFloor:
            return "RSSI_FLOOR";
        case SelectorReason::kRssiFade:
            return "RSSI_FADE";
        case SelectorReason::kBackpressure:
            return "BACKPRESSURE";
        case SelectorReason::kPromote:
            return "PROMOTE";
        case SelectorReason::kRepin:
            return "REPIN";
        case SelectorReason::kEnvironmentReset:
            return "ENVIRONMENT_RESET";
    }
    return "NONE";
}

uint32_t derive_bitrate_kbps(const Profile& p) {
    if (p.mcs >= 8) {
        return p.bitrate_min_kbps;
    }
    uint64_t kbps = kHt20LgiKbps[p.mcs];
    if (p.gi == GuardInterval::kShort) {
        kbps = kbps * 10 / 9;
    }
    kbps = kbps * p.airtime_budget_permille / 1000;
    kbps = kbps * (1000 - p.fec_overhead_permille) / 1000;
    const uint64_t reserve_kbps =
        (static_cast<uint64_t>(p.reserve_control_bps) +
         p.reserve_telemetry_bps) /
        1000;
    kbps = kbps > reserve_kbps ? kbps - reserve_kbps : 0;
    return static_cast<uint32_t>(std::max<uint64_t>(kbps, p.bitrate_min_kbps));
}

// §9.6 Pass 75: encoder-capability ceiling on the §9.5 derived target.
uint32_t clamp_bitrate_kbps(uint32_t derived_kbps, uint32_t max_kbps) {
    return (max_kbps != 0 && derived_kbps > max_kbps) ? max_kbps : derived_kbps;
}

Selector::Selector(const SelectorPolicy& policy, const ProfileTable* table)
    : policy_(policy), table_(table) {
    const size_t n = ladder_size();
    rung_loss_.resize(n);
    demoted_from_ms_.assign(n, 0);
    promoted_into_ms_.assign(n, 0);
    rung_ = safe_floor_rung();  // §9.8: start at the resolved safe floor
}

size_t Selector::ladder_size() const {
    return table_ != nullptr ? table_->profiles.size() : 0;
}

// §9.7 (Pass 83): min_profile/max_profile are profile IDs, resolved like
// floor_profile — never raw ladder indices. An id absent from the table is a
// config error (rejected at load); here we saturate so {"max": 255} still
// unpins and a runtime re-pin can never land out of range.
size_t Selector::rung_of_id(uint8_t id, size_t fallback) const {
    const size_t n = ladder_size();
    for (size_t i = 0; i < n; ++i) {
        if (table_->profiles[i].id == id) {
            return i;
        }
    }
    return fallback;
}

size_t Selector::clamp_rung(size_t r) const {
    const size_t n = ladder_size();
    if (n == 0) {
        return 0;
    }
    size_t lo = rung_of_id(policy_.min_profile, 0);
    size_t hi = rung_of_id(policy_.max_profile, n - 1);
    if (lo > hi) {
        lo = hi;
    }
    return std::clamp(r, lo, hi);
}

size_t Selector::floor_rung() const {
    const size_t n = ladder_size();
    for (size_t i = 0; i < n; ++i) {
        if (table_->profiles[i].id == table_->floor_profile) {
            return i;
        }
    }
    return 0;
}

size_t Selector::safe_floor_rung() const {
    return std::max(clamp_rung(0), clamp_rung(floor_rung()));
}

bool Selector::rung_locked(size_t rung, uint64_t now_ms) const {
    if (rung >= rung_loss_.size()) {
        return false;
    }
    const RungLossState& s = rung_loss_[rung];
    return s.latched || now_ms < s.blocked_until_ms;
}

size_t Selector::lockout_ceiling_rung(uint64_t now_ms, size_t lo, size_t hi,
                                      bool* conflict) const {
    if (conflict != nullptr) {
        *conflict = false;
    }
    for (size_t r = 0; r <= hi && r < rung_loss_.size(); ++r) {
        if (!rung_locked(r, now_ms)) {
            continue;
        }
        if (r <= lo) {
            if (conflict != nullptr) {
                *conflict = true;
            }
            return hi;  // operator envelope retains precedence
        }
        return r - 1;
    }
    return hi;
}

uint8_t Selector::effective_ceiling_profile(uint64_t now_ms) const {
    const size_t n = ladder_size();
    if (n == 0) {
        return policy_.max_profile;  // no ladder: nothing to narrow it with
    }
    // clamp_rung() rather than a fresh rung_of_id() pair, so this cannot drift
    // from evaluate()'s lo/hi if the §9.7 resolution ever changes.
    const size_t lo = clamp_rung(0);
    const size_t hi = clamp_rung(n - 1);
    bool conflict = false;
    const size_t ceiling = lockout_ceiling_rung(now_ms, lo, hi, &conflict);
    const size_t eff = conflict ? hi : std::min(hi, ceiling);
    return table_->profiles[eff].id;
}

uint8_t Selector::profile_id() const {
    return ladder_size() > 0 ? table_->profiles[rung_].id : 0;
}
uint8_t Selector::mcs() const {
    return ladder_size() > 0 ? table_->profiles[rung_].mcs : 0;
}
uint8_t Selector::tx_power_level() const {
    return ladder_size() > 0 ? table_->profiles[rung_].tx_power_level : 0;
}

uint16_t Selector::loss_ewma_milli() const {
    const double bounded = std::clamp(loss_ewma_milli_, 0.0, 1000.0);
    return static_cast<uint16_t>(bounded + 0.5);
}

uint8_t Selector::loss_score() const {
    return rung_ < rung_loss_.size() ? rung_loss_[rung_].score : 0;
}

uint8_t Selector::safe_floor_profile() const {
    const size_t floor = safe_floor_rung();
    return floor < ladder_size() ? table_->profiles[floor].id : 0;
}

SelectorLockout Selector::lockout(uint64_t now_ms) const {
    SelectorLockout out;
    const size_t n = std::min<size_t>(rung_loss_.size(), 8);
    for (size_t r = 0; r < n; ++r) {
        if (rung_locked(r, now_ms)) {
            out.active_mask |= static_cast<uint8_t>(1u << r);
        }
        if (rung_loss_[r].latched) {
            out.latched_mask |= static_cast<uint8_t>(1u << r);
        }
    }
    if (ladder_size() == 0) {
        return out;
    }
    const size_t lo = clamp_rung(0);
    const size_t hi = clamp_rung(ladder_size() - 1);
    bool conflict = false;
    const size_t ceiling = lockout_ceiling_rung(now_ms, lo, hi, &conflict);
    out.conflict = conflict;
    out.ceiling_profile = table_->profiles[ceiling].id;
    for (size_t r = 0; r <= hi && r < rung_loss_.size(); ++r) {
        if (!rung_locked(r, now_ms)) {
            continue;
        }
        out.active = true;
        out.latched = rung_loss_[r].latched;
        out.profile = table_->profiles[r].id;
        out.strikes = rung_loss_[r].strikes;
        if (!out.latched && rung_loss_[r].blocked_until_ms > now_ms) {
            out.remaining_ms = static_cast<uint32_t>(std::min<uint64_t>(
                rung_loss_[r].blocked_until_ms - now_ms, 0xFFFFFFFFull));
        }
        break;
    }
    return out;
}

uint64_t Selector::report_age_ms(uint64_t now_ms) const {
    if (!have_report_ || now_ms <= last_report_ms_) {
        return 0;
    }
    return now_ms - last_report_ms_;
}

bool Selector::flap_frozen(uint64_t now_ms) const {
    return now_ms < freeze_until_ms_;
}

void Selector::on_verdict(uint8_t verdict, uint64_t now_ms) {
    // §9.4 Pass 160: value + age only. Acceptance (latch, epoch monotone)
    // ran in the caller; >kMax cannot arrive (decode error upstream).
    verdict_ = verdict;
    verdict_ms_ = now_ms;
}

bool Selector::saturated_fresh(uint64_t now_ms) const {
    // Unknown / stale = absence of evidence, gates nothing (§9.4 Pass 160).
    return verdict_ == link_verdict::kSaturated && verdict_ms_ != 0 &&
           now_ms >= verdict_ms_ &&
           now_ms - verdict_ms_ < policy_.verdict_ttl_ms;
}

bool Selector::probe_veto_fresh(uint64_t now_ms) const {
    // §9.4 Pass 163: fresh up-candidate PER at/above the threshold vetoes a
    // climb. kNoProbe / stale = absence of evidence, gates nothing.
    return probe_per_ != kNoProbe &&
           probe_per_ >= policy_.probe_veto_permille && probe_per_ms_ != 0 &&
           now_ms >= probe_per_ms_ &&
           now_ms - probe_per_ms_ < policy_.probe_veto_ttl_ms;
}

bool Selector::on_report(const LinkReport& r, uint64_t now_ms) {
    // ReportGate has already authorized this identity. A session change is a
    // reporter reboot and a source change is a silence-based re-latch; both
    // start a new monotonic epoch/smoothing domain (§3.5 Pass 41).
    const std::pair<uint16_t, uint32_t> identity{r.prefix.originator,
                                                 r.prefix.session_id};
    if (!report_source_ || *report_source_ != identity) {
        const bool changed = report_source_.has_value();
        report_source_ = identity;
        have_report_ = false;
        last_epoch_ = 0;
        reset_environment_state(now_ms);
        if (changed) {
            reason_ = SelectorReason::kEnvironmentReset;
        }
    }
    // §9.8: the watchdog wants monotonic-forward epochs; replays/stale
    // reordered reports never freshen the link.
    if (have_report_ && r.report_epoch <= last_epoch_) {
        return false;
    }
    last_epoch_ = r.report_epoch;

    // §9.4 Pass 163: up-candidate probe evidence. kNoProbe leaves the last
    // value to age out through probe_veto_ttl_ms rather than clearing an
    // active veto on a momentarily under-filled window.
    if (r.probe_per != kNoProbe) {
        probe_per_ = r.probe_per;
        probe_per_ms_ = now_ms;
    }

    const double rssi = static_cast<double>(r.rssi_mean);
    const double loss = static_cast<double>(r.loss_postdiv_prearq);
    if (!have_smoothing_) {
        rssi_ewma_ = rssi;
        loss_ewma_milli_ = loss;
        prev_rssi_ = rssi;
        prev_report_ms_ = now_ms;
        have_smoothing_ = true;
    } else {
        rssi_ewma_ += policy_.ewma_alpha * (rssi - rssi_ewma_);
        loss_ewma_milli_ += policy_.ewma_alpha * (loss - loss_ewma_milli_);
        if (now_ms > prev_report_ms_) {
            const double dt_s =
                static_cast<double>(now_ms - prev_report_ms_) / 1000.0;
            const double slope = (rssi - prev_rssi_) / dt_s;
            slope_ewma_ += policy_.slope_alpha * (slope - slope_ewma_);
        }
        prev_rssi_ = rssi;
        prev_report_ms_ = now_ms;
    }
    // §9.1 rule 3 arming: fade must persist fade_ticks consecutive reports.
    if (rssi_ewma_ <= policy_.rssi_fade_arm_dbm &&
        slope_ewma_ <= -policy_.rssi_fade_db_per_s) {
        if (fade_ticks_ < 255) {
            ++fade_ticks_;
        }
    } else {
        fade_ticks_ = 0;
    }

    // Pass 110: raw 100 ms evidence, qualified by the interval denominator.
    // The leaky score is per rung; an under-filled window changes nothing.
    loss_window_milli_ = r.loss_postdiv_prearq;
    loss_uniq_ = r.uniq;
    loss_observed_rung_ = rung_;
    emergency_pending_ =
        r.uniq >= policy_.loss_min_uniq &&
        r.loss_postdiv_prearq >= policy_.emergency_loss_milli;
    if (rung_ < rung_loss_.size() && r.uniq >= policy_.loss_min_uniq &&
        !rung_locked(rung_, now_ms)) {
        RungLossState& rs = rung_loss_[rung_];
        if (r.loss_postdiv_prearq >= policy_.demote_milli) {
            if (rs.score < policy_.loss_persist_score) {
                ++rs.score;
            }
        } else if (rs.score > 0) {
            --rs.score;
        }
    }
    have_report_ = true;
    last_report_ms_ = now_ms;
    failsafe_next_step_ms_ = 0;  // fresh feedback ends any fail-safe descent
    return true;
}

bool Selector::on_rf_environment(uint16_t channel_mhz, uint8_t bw,
                                 uint64_t now_ms) {
    const std::pair<uint16_t, uint8_t> next{channel_mhz, bw};
    if (!rf_environment_) {
        rf_environment_ = next;
        return false;
    }
    if (*rf_environment_ == next) {
        return false;
    }
    rf_environment_ = next;
    reset_environment_state(now_ms);
    reason_ = SelectorReason::kEnvironmentReset;
    return true;
}

void Selector::set_pressure(bool on, uint64_t now_ms) {
    if (on && !pressure_) {
        pressure_since_ms_ = now_ms;
    }
    pressure_ = on;
}

void Selector::reset_environment_state(uint64_t now_ms) {
    for (RungLossState& rs : rung_loss_) {
        rs = {};
    }
    loss_window_milli_ = 0;
    loss_uniq_ = 0;
    loss_observed_rung_ = rung_;
    emergency_pending_ = false;
    have_smoothing_ = false;
    rssi_ewma_ = 0.0;
    loss_ewma_milli_ = 0.0;
    slope_ewma_ = 0.0;
    prev_rssi_ = 0.0;
    prev_report_ms_ = 0;
    fade_ticks_ = 0;
    demote_times_.clear();
    freeze_until_ms_ = 0;
    std::fill(demoted_from_ms_.begin(), demoted_from_ms_.end(), 0);
    std::fill(promoted_into_ms_.begin(), promoted_into_ms_.end(), 0);
    failsafe_next_step_ms_ = 0;
    last_demote_ms_ = 0;
    last_change_ms_ = now_ms;
    // §9.4 Pass 163: probe evidence is scoped to the reporter/environment
    // that measured it — another context must never gate (or clear) a climb.
    probe_per_ = kNoProbe;
    probe_per_ms_ = 0;
}

void Selector::charge_lockout(size_t rung, uint64_t now_ms) {
    if (rung >= rung_loss_.size() || rung <= safe_floor_rung() ||
        rung_locked(rung, now_ms)) {
        return;
    }
    RungLossState& rs = rung_loss_[rung];
    if (rs.strikes < policy_.rung_lockout_latch_count) {
        ++rs.strikes;
    }
    rs.score = 0;
    if (rs.strikes >= policy_.rung_lockout_latch_count) {
        rs.latched = true;
        rs.blocked_until_ms = 0;
    } else {
        const uint64_t room = std::numeric_limits<uint64_t>::max() - now_ms;
        rs.blocked_until_ms =
            policy_.rung_lockout_ms > room
                ? std::numeric_limits<uint64_t>::max()
                : now_ms + policy_.rung_lockout_ms;
    }
}

bool Selector::rssi_guard_active() const {
    return rssi_ewma_ <= policy_.rssi_floor_dbm ||
           fade_ticks_ >= policy_.fade_ticks;
}

void Selector::note_demote_for_flap(uint64_t now_ms) {
    // §9.7 hard freeze counts fast RE-demotes: a demote from a rung we
    // promoted into within the window.
    const uint64_t promoted = promoted_into_ms_[rung_];
    if (promoted != 0 && now_ms >= promoted &&
        now_ms - promoted < policy_.flap_freeze_window_ms) {
        demote_times_.push_back(now_ms);
    }
    while (!demote_times_.empty() &&
           now_ms - demote_times_.front() > policy_.flap_freeze_window_ms) {
        demote_times_.pop_front();
    }
    if (demote_times_.size() >= policy_.flap_freeze_count) {
        freeze_until_ms_ = now_ms + policy_.flap_freeze_ms;
        demote_times_.clear();
    }
}

void Selector::start_demote(size_t target, uint64_t now_ms,
                            SelectorActions& a, SelectorReason reason,
                            bool charge_rung) {
    // §9.4 Pass 163: see start_promote — vacated-rung probe evidence dies
    // with the rung. (Demotes themselves never consult it, §9.0.)
    probe_per_ = kNoProbe;
    probe_per_ms_ = 0;
    if (charge_rung) {
        charge_lockout(rung_, now_ms);
    }
    note_demote_for_flap(now_ms);
    // §9.7 soft reentry marks the whole vacated span. Pass 110 permits a
    // multi-rung span only for an acute move to the resolved safe floor.
    for (size_t r = target + 1; r <= rung_ && r < demoted_from_ms_.size();
         ++r) {
        demoted_from_ms_[r] = now_ms;
    }
    last_demote_ms_ = now_ms;
    pending_target_ = target;
    phase_ = Phase::kBitrateLead;
    phase_deadline_ms_ = now_ms + policy_.bitrate_lead_ms;
    state_ = "DEMOTE";
    reason_ = reason;
    // §9.5: bitrate LEADS the downward move.
    const uint32_t br = clamp_bitrate_kbps(
        derive_bitrate_kbps(table_->profiles[target]), policy_.max_bitrate_kbps);
    if (br != bitrate_kbps_) {
        bitrate_kbps_ = br;
        a.bitrate_kbps = br;
    }
}

void Selector::start_promote(size_t target, uint64_t now_ms,
                             SelectorActions& a, SelectorReason reason) {
    // §9.4 Pass 163: a rung change changes the up-candidate — evidence from
    // the vacated operating point must not gate (or spare) the next climb.
    probe_per_ = kNoProbe;
    probe_per_ms_ = 0;
    pending_target_ = target;
    phase_ = Phase::kMcsGrace;
    phase_deadline_ms_ = now_ms + policy_.mcs_up_grace_ms;
    state_ = "PROMOTE";
    reason_ = reason;
    promoted_into_ms_[target] = now_ms;
    // §9.5: MCS/power move first, bitrate lags.
    rung_ = target;
    last_change_ms_ = now_ms;
    const Profile& p = table_->profiles[target];
    a.commit = ProfileCommit{p.id, p.mcs, p.gi, p.tx_power_level};
}

void Selector::evaluate(uint64_t now_ms, SelectorActions& a) {
    // §11.3 CSA freeze: cascade halted, watchdog paused. On expiry the
    // blackout window is excused from the report age so a healthy switch
    // does not resume straight into FAILSAFE.
    if (csa_freeze_until_ms_ != 0) {
        if (now_ms < csa_freeze_until_ms_) {
            state_ = "CSA_FREEZE";
            return;
        }
        if (have_report_ && last_report_ms_ < csa_freeze_until_ms_) {
            last_report_ms_ = csa_freeze_until_ms_;
        }
        csa_freeze_until_ms_ = 0;
    }
    const size_t lo = clamp_rung(0);
    const size_t hi = clamp_rung(ladder_size() - 1);
    const size_t floor = safe_floor_rung();
    if (lo == hi) {
        state_ = "PINNED";  // §9.7 min==max
        // A runtime re-pin (set_profile_pin) must snap the operating point to
        // the pinned rung — that is the whole bench / known-bad-link use case.
        // The ctor clamps rung_ for config-time pins, but a live re-pin only
        // moves policy_, so jump here directly (direction-agnostic, no flap
        // bookkeeping: the pin overrides adaptation outright).
        if (rung_ != lo) {
            rung_ = lo;
            last_change_ms_ = now_ms;
            reason_ = SelectorReason::kRepin;
            const Profile& p = table_->profiles[lo];
            a.commit = ProfileCommit{p.id, p.mcs, p.gi, p.tx_power_level};
            // §9.5: the pinned rung's bitrate must move WITH the commit, as
            // kBoot and start_demote both do. Committing the MCS alone left
            // venc at the prior rung's rate — a downward pin to MCS0 then
            // oversubscribed the link ~3.6x and delivered ~98% unrecoverable
            // (measured on hardware). The pin is direction-agnostic, so this
            // covers a pin up as well.
            const uint32_t br = clamp_bitrate_kbps(
                derive_bitrate_kbps(p), policy_.max_bitrate_kbps);
            if (br != bitrate_kbps_) {
                bitrate_kbps_ = br;
                a.bitrate_kbps = br;
            }
        }
        return;
    }

    // §9.7 range re-pin clamp (Pass 100): a runtime re-pin to a range whose new
    // envelope EXCLUDES the current rung snaps the operating point INTO [lo, hi]
    // — the range analogue of the min==max snap above. A down-clamp (rung_ > hi)
    // is a demote and unconditional. An up-clamp (rung_ < lo) is a promotion, so
    // it defers to §9.8 while feedback is stale ("never fail optimistic": a
    // raised min_profile must not pull the rung UP on a lost link — §9.8/Pass 84
    // keeps floor_profile below min_profile as the safety floor). Without this a
    // lowered max waited for a loss/§9.8 demote trigger that never comes on a
    // clean link, so a high-range mode held a high MCS until the first loss.
    // evaluate() runs only in kIdle, so there is no in-flight transition to race.
    if (rung_ > hi ||
        (rung_ < lo && have_report_ &&
         report_age_ms(now_ms) <= policy_.report_timeout_ms)) {
        const size_t target = (rung_ > hi) ? hi : lo;
        rung_ = target;
        last_change_ms_ = now_ms;
        const Profile& p = table_->profiles[target];
        a.commit = ProfileCommit{p.id, p.mcs, p.gi, p.tx_power_level};
        // §9.5 (Pass 97): commit MCS + bitrate together, never MCS alone.
        const uint32_t br =
            clamp_bitrate_kbps(derive_bitrate_kbps(p), policy_.max_bitrate_kbps);
        if (br != bitrate_kbps_) {
            bitrate_kbps_ = br;
            a.bitrate_kbps = br;
        }
        state_ = "REPIN";
        reason_ = SelectorReason::kRepin;
        return;
    }

    // §9.8 fail-safe: stale feedback ⇒ hold, then damped descent to floor.
    if (!have_report_ ||
        report_age_ms(now_ms) > policy_.report_timeout_ms) {
        state_ = "FAILSAFE";
        if (report_age_ms(now_ms) <
            policy_.report_timeout_ms + policy_.failsafe_hold_ms) {
            return;  // hold phase
        }
        // §9.8 (Pass 102, supersedes Pass 84): the descent floors at
        // max(min_profile, floor_profile), NOT the table floor_profile alone.
        // Under the mode harness (docs/venc-mode-matrix.md §16) min_profile
        // (`lo`) is the Range band's lowest rung, and the mode's resolution +
        // fps are co-designed so the §16.1 bpp floor is cleared at exactly that
        // rung and no lower. Descending below it lands on a rung the mode never
        // verified — a bpp violation, the "mode mechanics fighting each other"
        // case. floor_profile stays the absolute floor and still binds when it
        // sits ABOVE min_profile. (A min==max pin never reaches here — the
        // PINNED branch returns above; its band floor is the pin regardless.)
        if (rung_ > floor && now_ms >= failsafe_next_step_ms_) {
            failsafe_next_step_ms_ = now_ms + policy_.failsafe_step_ms;
            start_demote(rung_ - 1, now_ms, a,
                          SelectorReason::kReportTimeout);
        }
        return;  // NEVER promote on stale feedback
    }

    const bool demote_ok =
        now_ms - last_demote_ms_ >= policy_.down_cooldown_ms;
    bool blocked_saturated = false;  // §9.4 Pass 160, counted once per tick
    bool blocked_probe = false;      // §9.4 Pass 163, same counting rule

    // Rule 1 — acute loss: one confidence-qualified raw report moves
    // directly to the resolved safe floor. The event belongs to the rung on
    // which it was observed; a completed concurrent demote invalidates it.
    if (emergency_pending_) {
        emergency_pending_ = false;
        if (loss_observed_rung_ == rung_) {
            reason_ = SelectorReason::kLossEmergency;
            if (rung_ > floor) {
                start_demote(floor, now_ms, a,
                              SelectorReason::kLossEmergency, true);
                return;
            }
        }
    }
    // Rule 2 — persistent moderate loss: leaky score, exactly one rung.
    if (!pressure_ && demote_ok && loss_observed_rung_ == rung_ &&
        rung_ < rung_loss_.size() &&
        rung_loss_[rung_].score >= policy_.loss_persist_score &&
        rung_ > floor) {
        start_demote(rung_ - 1, now_ms, a,
                      SelectorReason::kLossPersistent, true);
        return;
    }
    // Rule 3 — RSSI floor.
    if (demote_ok && rssi_ewma_ <= policy_.rssi_floor_dbm && rung_ > floor) {
        start_demote(rung_ - 1, now_ms, a, SelectorReason::kRssiFloor);
        return;
    }
    // Rule 4 — RSSI fade.
    if (demote_ok && fade_ticks_ >= policy_.fade_ticks && rung_ > floor) {
        fade_ticks_ = 0;
        start_demote(rung_ - 1, now_ms, a, SelectorReason::kRssiFade);
        return;
    }
    bool lockout_conflict = false;
    const size_t ceiling =
        lockout_ceiling_rung(now_ms, lo, hi, &lockout_conflict);
    const size_t adaptive_hi = lockout_conflict ? hi : std::min(hi, ceiling);
    // Rule 5 — backpressure escape (clean RF, sustained pressure).
    if (pressure_ && !rssi_guard_active() &&
        now_ms - pressure_since_ms_ >= policy_.pressure_escape_ms &&
        now_ms - last_demote_ms_ >= policy_.down_cooldown_ms &&
        rung_ < adaptive_hi &&
        !flap_frozen(now_ms)) {
        // §9.4 Pass 160: a fresh Saturated verdict suppresses EVERY climb —
        // gating only rule 6 would reroute the saturation flap through here.
        // Pass 163: the probe veto gates both climbs for the same reason.
        if (saturated_fresh(now_ms)) {
            blocked_saturated = true;
        } else if (probe_veto_fresh(now_ms)) {
            blocked_probe = true;
        } else {
            last_demote_ms_ = now_ms;  // reuse the cooldown as the climb pacer
            start_promote(rung_ + 1, now_ms, a,
                          SelectorReason::kBackpressure);
            return;
        }
    }
    // Rule 6 — RSSI-margin promote (§9.4). Gated on clean delivered loss
    // (§9.0 robustness-first: promoting while loss sits at the demote
    // threshold is never right).
    if (rung_ < adaptive_hi && !rssi_guard_active() &&
        !flap_frozen(now_ms) &&
        loss_ewma_milli_ < static_cast<double>(policy_.demote_milli)) {
        const size_t next = rung_ + 1;
        const size_t fi = std::min(next, policy_.rung_rssi_floor_dbm.size() - 1);
        const double need =
            static_cast<double>(policy_.rung_rssi_floor_dbm[fi]) +
            policy_.promote_rssi_hyst_db;
        // §9.7 soft reentry: re-promoting into a recently-demoted-from rung
        // needs the longer dwell.
        const uint64_t demoted = demoted_from_ms_[next];
        const bool recent_demote =
            demoted != 0 && now_ms >= demoted &&
            now_ms - demoted < policy_.reentry_backoff_ms;
        const uint64_t dwell = recent_demote ? policy_.reentry_dwell_ms
                                             : policy_.promote_dwell_ms;
        if (rssi_ewma_ >= need && now_ms - last_change_ms_ >= dwell) {
            // §9.4 Pass 160 saturation gate: strong RSSI is exactly what a
            // saturating front end shows — the one case margin cannot see.
            // Pass 163 probe veto: the candidate rate measurably failing at
            // the CURRENT rung's power is the other case margin cannot see.
            if (saturated_fresh(now_ms)) {
                blocked_saturated = true;
            } else if (probe_veto_fresh(now_ms)) {
                blocked_probe = true;
            } else {
                start_promote(next, now_ms, a, SelectorReason::kPromote);
                return;
            }
        }
    }
    // Rule 7 — hold. One suppression count per tick however many climb
    // rules the fresh-Saturated verdict blocked (a gauge of blocked TIME,
    // not of rules).
    if (blocked_saturated) {
        ++promote_blocked_saturated_;
    }
    if (blocked_probe) {
        ++promote_blocked_probe_;
    }
    state_ = flap_frozen(now_ms) ? "FREEZE" : "HOLD";
}

SelectorActions Selector::tick(uint64_t now_ms) {
    SelectorActions a;
    if (ladder_size() == 0) {
        return a;
    }
    switch (phase_) {
        case Phase::kBoot: {
            // Bootstrap at the floor rung: commit + bitrate together (there
            // is no prior operating point to sequence against).
            phase_ = Phase::kIdle;
            last_change_ms_ = now_ms;
            const Profile& p = table_->profiles[rung_];
            a.commit = ProfileCommit{p.id, p.mcs, p.gi, p.tx_power_level};
            bitrate_kbps_ =
                clamp_bitrate_kbps(derive_bitrate_kbps(p), policy_.max_bitrate_kbps);
            a.bitrate_kbps = bitrate_kbps_;
            state_ = "HOLD";
            return a;
        }
        case Phase::kBitrateLead:
            if (now_ms >= phase_deadline_ms_) {
                phase_ = Phase::kIdle;
                rung_ = pending_target_;
                last_change_ms_ = now_ms;
                const Profile& p = table_->profiles[rung_];
                a.commit = ProfileCommit{p.id, p.mcs, p.gi, p.tx_power_level};
            }
            return a;
        case Phase::kMcsGrace:
            if (now_ms >= phase_deadline_ms_) {
                phase_ = Phase::kIdle;
                const uint32_t br = clamp_bitrate_kbps(
                    derive_bitrate_kbps(table_->profiles[rung_]),
                    policy_.max_bitrate_kbps);
                if (br != bitrate_kbps_) {
                    bitrate_kbps_ = br;
                    a.bitrate_kbps = br;
                }
            }
            return a;
        case Phase::kIdle:
            evaluate(now_ms, a);
            return a;
    }
    return a;
}

}  // namespace wblink
