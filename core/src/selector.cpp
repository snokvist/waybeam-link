// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/selector.h"

#include <algorithm>

namespace wblink {

namespace {

// HT20 PHY rates, kbps, long GI, MCS0–7 (§9.5 Pass-6 budget law).
constexpr uint32_t kHt20LgiKbps[8] = {6500,  13000, 19500, 26000,
                                      39000, 52000, 58500, 65000};

// §9.2 physics prior: lower MCS ⇒ higher delivery probability. Values only
// need the ORDERING to be right — staleness degrades to the safe ranking.
double physics_prior(size_t rung) {
    return 0.99 - 0.06 * static_cast<double>(rung);
}

}  // namespace

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
    rung_prob_.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        rung_prob_[i] = physics_prior(i);
    }
    demoted_from_ms_.assign(n, 0);
    promoted_into_ms_.assign(n, 0);
    rung_ = clamp_rung(floor_rung());  // §9.8: start at the safe floor
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

size_t Selector::max_prob_rung() const {
    size_t best = 0;
    for (size_t i = 1; i < rung_prob_.size(); ++i) {
        if (rung_prob_[i] > rung_prob_[best]) {
            best = i;
        }
    }
    return best;
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

uint64_t Selector::report_age_ms(uint64_t now_ms) const {
    if (!have_report_ || now_ms <= last_report_ms_) {
        return 0;
    }
    return now_ms - last_report_ms_;
}

bool Selector::flap_frozen(uint64_t now_ms) const {
    return now_ms < freeze_until_ms_;
}

bool Selector::on_report(const LinkReport& r, uint64_t now_ms) {
    // ReportGate has already authorized this identity. A session change is a
    // reporter reboot and a source change is a silence-based re-latch; both
    // start a new monotonic epoch/smoothing domain (§3.5 Pass 41).
    const std::pair<uint16_t, uint32_t> identity{r.prefix.originator,
                                                 r.prefix.session_id};
    if (!report_source_ || *report_source_ != identity) {
        report_source_ = identity;
        have_report_ = false;
        last_epoch_ = 0;
        slope_ewma_ = 0.0;
        fade_ticks_ = 0;
    }
    // §9.8: the watchdog wants monotonic-forward epochs; replays/stale
    // reordered reports never freshen the link.
    if (have_report_ && r.report_epoch <= last_epoch_) {
        return false;
    }
    last_epoch_ = r.report_epoch;

    const double rssi = static_cast<double>(r.rssi_mean);
    const double loss = static_cast<double>(r.loss_postdiv_prearq);
    if (!have_report_) {
        rssi_ewma_ = rssi;
        loss_ewma_milli_ = loss;
        prev_rssi_ = rssi;
        prev_report_ms_ = now_ms;
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
    // §9.2: feed the current rung's success EWMA, age the rest toward the
    // physics prior.
    for (size_t i = 0; i < rung_prob_.size(); ++i) {
        if (i == rung_) {
            rung_prob_[i] +=
                policy_.ewma_alpha * ((1.0 - loss / 1000.0) - rung_prob_[i]);
        } else {
            rung_prob_[i] += policy_.rung_age_rate *
                             (physics_prior(i) - rung_prob_[i]);
        }
    }
    have_report_ = true;
    last_report_ms_ = now_ms;
    failsafe_next_step_ms_ = 0;  // fresh feedback ends any fail-safe descent
    return true;
}

void Selector::set_pressure(bool on, uint64_t now_ms) {
    if (on && !pressure_) {
        pressure_since_ms_ = now_ms;
    }
    pressure_ = on;
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
                            SelectorActions& a) {
    note_demote_for_flap(now_ms);
    // §9.7 soft reentry marks the whole vacated span: a multi-rung jump
    // (toward the max-prob rung, §9.2) leaves every rung above the target
    // "just demoted from" — re-promoting into any of them is a reentry.
    for (size_t r = target + 1; r <= rung_ && r < demoted_from_ms_.size();
         ++r) {
        demoted_from_ms_[r] = now_ms;
    }
    last_demote_ms_ = now_ms;
    pending_target_ = target;
    phase_ = Phase::kBitrateLead;
    phase_deadline_ms_ = now_ms + policy_.bitrate_lead_ms;
    state_ = "DEMOTE";
    // §9.5: bitrate LEADS the downward move.
    const uint32_t br = clamp_bitrate_kbps(
        derive_bitrate_kbps(table_->profiles[target]), policy_.max_bitrate_kbps);
    if (br != bitrate_kbps_) {
        bitrate_kbps_ = br;
        a.bitrate_kbps = br;
    }
}

void Selector::start_promote(size_t target, uint64_t now_ms,
                             SelectorActions& a) {
    pending_target_ = target;
    phase_ = Phase::kMcsGrace;
    phase_deadline_ms_ = now_ms + policy_.mcs_up_grace_ms;
    state_ = "PROMOTE";
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
        // §9.8 (Pass 84): the descent target is floor_profile, NOT the §9.7
        // pin. min_profile is an adaptation envelope (how low the selector may
        // CHOOSE to go while it can see feedback); floor_profile is the safety
        // floor (where the link goes when feedback is GONE). Clamping here let
        // a `min_profile: 1` airtime choice silently remove MCS0 from the one
        // path that runs when the link is worst — "never fail optimistic".
        const size_t floor = floor_rung();
        if (rung_ > floor && now_ms >= failsafe_next_step_ms_) {
            failsafe_next_step_ms_ = now_ms + policy_.failsafe_step_ms;
            start_demote(rung_ - 1, now_ms, a);
        }
        return;  // NEVER promote on stale feedback
    }

    const bool settling =
        now_ms - last_change_ms_ < policy_.mcs_settle_ms;
    const bool demote_ok =
        now_ms - last_demote_ms_ >= policy_.down_cooldown_ms;

    // Rule 1 — reactive demote (suppressed while settling and under local
    // pressure, §9.5/§9.9).
    if (!settling && !pressure_ && demote_ok &&
        loss_ewma_milli_ >= static_cast<double>(policy_.demote_milli) &&
        rung_ > lo) {
        // §9.2: demote TOWARD the max-probability rung.
        const size_t maxp = clamp_rung(max_prob_rung());
        const size_t target = std::max(lo, std::min(rung_ - 1, maxp));
        start_demote(target, now_ms, a);
        return;
    }
    // Rule 2 — RSSI floor.
    if (demote_ok && rssi_ewma_ <= policy_.rssi_floor_dbm && rung_ > lo) {
        start_demote(rung_ - 1, now_ms, a);
        return;
    }
    // Rule 3 — RSSI fade.
    if (demote_ok && fade_ticks_ >= policy_.fade_ticks && rung_ > lo) {
        fade_ticks_ = 0;
        start_demote(rung_ - 1, now_ms, a);
        return;
    }
    // Rule 4 — backpressure escape (clean RF, sustained pressure).
    if (pressure_ && !rssi_guard_active() &&
        now_ms - pressure_since_ms_ >= policy_.pressure_escape_ms &&
        now_ms - last_demote_ms_ >= policy_.down_cooldown_ms && rung_ < hi &&
        !flap_frozen(now_ms)) {
        last_demote_ms_ = now_ms;  // reuse the cooldown as the climb pacer
        start_promote(rung_ + 1, now_ms, a);
        return;
    }
    // Rule 5 — RSSI-margin promote (§9.4). Gated on clean delivered loss
    // (§9.0 robustness-first: promoting while loss sits at the demote
    // threshold — reachable when settle suppresses rule 1 — is never right).
    if (rung_ < hi && !rssi_guard_active() && !flap_frozen(now_ms) &&
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
            start_promote(next, now_ms, a);
            return;
        }
    }
    // Rule 6 — hold.
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
