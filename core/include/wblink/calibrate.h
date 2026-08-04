// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §10.6 craft-resident link calibration (Pass 120).
//
// A pure, time-injected state machine in the ms domain, shaped like the §11
// engines: inputs are accepted §3.5 report samples and ticks; outputs are
// polled actions (pin rung / set power / restore / artifact-ready). Nothing
// here touches a radio, a file, or a clock — app/ actuates through the §9.7
// pin and §10.5 set_power_qdb seams and persists the artifact (io/).
//
// Per rung (Pass 130 max-power sweep): climb from min_qdb in seek_step_qdb
// steps until the loss wall (report loss > loss_bad_milli), the
// rssi_guard_dbm overload backstop, or max_qdb. Placement = the highest clean
// probe; the wall's bracketing RSSIs ARE the overload-ceiling record. Then
// VERIFY (longer dwell, record placement loss) → next rung, sweeping again
// from min_qdb. Steps below the first clean probe are simply not clean — a
// blacked-out floor needs no special rule, and nothing ever descends.
//
// §10.6 safety envelope: report-loss abort (no accepted report for
// report_loss_abort_ms), hard cap, and a single-shot `restore` action on
// EVERY exit — the app maps it to the R4 order: power first, then the boot
// selector window, then the §10.4 resolve.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "wblink/power.h"  // kPowerLevelBaseline / kQdbPerLevel

namespace wblink {

// §10.6/§10.7 shared seek/verify mechanics (Pass 125). Pure in the strongest
// sense: no clocks, no dwell management, no rung orchestration. The caller
// decides WHEN a dwell completed and WHAT evidence it carried; this decides
// where the power goes next. Extracted so the craft's 8-rung ms-gated loop
// and the ground's 1-rung epoch-gated loop share one bench-validated ramp
// instead of growing a second, differently-buggy copy.
struct SeekParams {
    uint16_t loss_ok_milli = 15;
    uint16_t loss_bad_milli = 50;
    int32_t seek_step_qdb = 16;
    int rssi_guard_dbm = -6;
    int32_t min_qdb = 4;
    int32_t max_qdb = 108;
    uint8_t verify_descent_budget = 3;
};

// What a completed dwell observed.
//
// kNoEvidence is the craft's blank dwell (§10.6: zero samples arrived) — hold
// at this power and let the caller's clocks arbitrate. The ground never uses
// it: under live §3.16 feedback a stalled counter IS evidence of a dead
// uplink, so §10.7 supplies kBad. That asymmetry is the whole point of the
// two-clocks split, and it is why this is an explicit input rather than
// something inferred from a zero sample count.
enum class DwellVerdict : uint8_t { kClean, kBad, kNoEvidence };

struct SeekStep {
    enum class Kind : uint8_t {
        kProbe,   // dwell again at qdb, probe cadence
        kVerify,  // dwell at qdb, verify cadence — placement candidate
        kDone,    // qdb is the verified placement
        kFailed,  // fail_reason is set
    };
    Kind kind = Kind::kProbe;
    int32_t qdb = 0;
    bool power_changed = false;  // caller actuates qdb when set
    const char* fail_reason = nullptr;
};

class PowerSeek {
  public:
    explicit PowerSeek(const SeekParams& p) : p_(p) {}

    // Sweep upward from min_qdb. Both directions seed here: §10.6 restarts the
    // sweep for every rung and §10.7 has one. Seeding a rung mid-range (from
    // the previous placement) was a runtime optimisation that made the RESULT
    // depend on where the sweep began, which is not a property a measurement
    // should have.
    // §10.3 (Pass 134): the caller may narrow the ceiling per rung. The seek
    // owns no policy here — it is handed the bound and climbs to it.
    SeekStep begin(int32_t max_qdb) {
        p_.max_qdb = std::max(p_.min_qdb, max_qdb);
        return begin();
    }
    SeekStep begin() {
        qdb_ = p_.min_qdb;
        last_clean_.reset();
        verify_descents_ = 0;
        phase_ = Phase::kSweep;
        last_clean_rssi_ = 127;
        has_bad_ = false;
        first_bad_rssi_ = 0;
        first_bad_qdb_ = 0;
        return {SeekStep::Kind::kProbe, qdb_, true, nullptr};
    }

    SeekStep on_dwell(DwellVerdict v, double rssi, uint16_t loss_milli) {
        if (v == DwellVerdict::kNoEvidence) {
            // §10.6 blank dwell: no evidence either way — hold here and let
            // the caller's report clock arbitrate. §10.7 never produces it.
            return {phase_ == Phase::kVerify ? SeekStep::Kind::kVerify
                                             : SeekStep::Kind::kProbe,
                    qdb_, false, nullptr};
        }
        if (phase_ == Phase::kVerify) return verify_(rssi, loss_milli);

        if (v == DwellVerdict::kClean && loss_milli <= p_.loss_bad_milli) {
            last_clean_ = Probe{qdb_, rssi};
            last_clean_rssi_ = static_cast<int8_t>(std::lround(rssi));
            // Token backstop against RX front-end abuse. The loss wall is the
            // intended limiter at every rung; this only catches the case where
            // we are cooking the receiver before loss has risen.
            if (rssi > p_.rssi_guard_dbm) return place_();
        } else if (last_clean_) {
            // A bad probe above a clean one books the rung's overload bracket
            // — but it does NOT end the sweep (Pass 133). Terminating here
            // assumed clean-then-bad means "ceiling found", which is only true
            // if the clean reading was real. At the bottom of the range on a
            // marginal link it often is not: measured at 10 m, rung 6 read
            // 100/100 at 1.0 dBm (rssi -77, not physical), then bad one step
            // up, and the sweep PLACED after 2 of 8 steps — never trying the
            // 17.0 dBm region where the adjacent rung had just placed
            // cleanly. The run then failed, and reported the rung unreachable
            // when it had simply never been measured.
            //
            // So: record the bracket and keep climbing. The placement is the
            // HIGHEST clean probe over the FULL range, which is what "maximum
            // clean TX power" means and what makes the result a property of
            // the channel rather than of the first two readings.
            note_bad_(rssi);
        } else {
            // Nothing clean anywhere yet, so this is still the dead bottom of
            // the range — a blacked-out floor is simply "not clean" and the
            // sweep keeps climbing. No floor rule, no retreat, no descent.
        }
        if (qdb_ >= p_.max_qdb) {
            if (last_clean_) return place_();
            return {SeekStep::Kind::kFailed, qdb_, false, "no_clean_point"};
        }
        qdb_ = std::min(p_.max_qdb, qdb_ + p_.seek_step_qdb);
        return {SeekStep::Kind::kProbe, qdb_, true, nullptr};
    }

    int32_t qdb() const { return qdb_; }
    bool in_verify() const { return phase_ == Phase::kVerify; }
    int8_t last_clean_rssi() const { return last_clean_rssi_; }
    bool has_bad() const { return has_bad_; }
    int8_t first_bad_rssi() const { return first_bad_rssi_; }
    // The bracket in the POWER domain, for §10.7's artifact. Same rule as the
    // RSSI bracket: only a bad probe ABOVE a clean one is overload evidence.
    int32_t first_bad_qdb() const { return first_bad_qdb_; }
    int32_t last_clean_qdb() const {
        return last_clean_ ? last_clean_->qdb : p_.min_qdb;
    }

  private:
    enum class Phase : uint8_t { kSweep, kVerify };
    struct Probe { int32_t qdb; double rssi; };

    void note_bad_(double rssi) {
        if (!has_bad_) {
            has_bad_ = true;
            first_bad_rssi_ = static_cast<int8_t>(std::lround(rssi));
            first_bad_qdb_ = qdb_;
        }
    }
    SeekStep place_() {
        qdb_ = last_clean_->qdb;
        phase_ = Phase::kVerify;
        return {SeekStep::Kind::kVerify, qdb_, true, nullptr};
    }

    SeekStep verify_(double rssi, uint16_t loss) {
        // §10.6 addendum 2: near-cliff instability passes a short probe dwell
        // and fails only under sustained exposure (measured: a placement that
        // probed clean verified at 942permille), so loss_ok is enforced here
        // and a failing placement steps down and re-verifies, bounded.
        if (loss > p_.loss_ok_milli &&
            verify_descents_ < p_.verify_descent_budget && qdb_ > p_.min_qdb) {
            if (loss > p_.loss_bad_milli) note_bad_(rssi);
            ++verify_descents_;
            qdb_ = std::max(p_.min_qdb, qdb_ - p_.seek_step_qdb);
            return {SeekStep::Kind::kVerify, qdb_, true, nullptr};
        }
        return {SeekStep::Kind::kDone, qdb_, false, nullptr};
    }

    SeekParams p_;
    Phase phase_ = Phase::kSweep;
    int32_t qdb_ = 0;
    std::optional<Probe> last_clean_;
    uint8_t verify_descents_ = 0;
    int8_t last_clean_rssi_ = 127;
    bool has_bad_ = false;
    int8_t first_bad_rssi_ = 0;
    int32_t first_bad_qdb_ = 0;
};

struct CalibrateParams {
    uint16_t loss_ok_milli = 15;  // §15.2 policy.calibration seeds
    uint16_t loss_bad_milli = 50;
    int32_t seek_step_qdb = 16;  // 4 dB per seek probe (Pass 121)
    int rssi_guard_dbm = -6;     // token RX-abuse backstop (P121 addendum)
    int32_t min_qdb = 4;         // 1 dBm
    int32_t max_qdb = 108;       // 27 dBm
    uint32_t settle_ms = 800;    // TXAGC settle + one report window
    uint32_t probe_dwell_ms = 1200;
    uint32_t verify_dwell_ms = 2500;
    uint32_t report_loss_abort_ms = 3000;
    uint32_t hard_cap_ms = 600000;
    // The §9.3 table's tx_power_level per MCS — the authored curve is the
    // level-4 baseline, so placements are compensated per §10.2.
    std::array<uint8_t, 8> levels{4, 4, 3, 3, 2, 2, 1, 1};
};

// §10.3 (Pass 134) per-rung sweep ceiling: the flat sanity ceiling tapered by
// the rung's §10.2 level intent, so a sweep cannot walk a high-order rung to
// full power looking for a wall the PA reaches first. Derived from the §9.3
// table both ends already agree on by hash — no new key, no new wire. The
// caller has already folded the adapter's §10.3 max_power_qdb into max_qdb.
inline int32_t rung_max_qdb(const CalibrateParams& p, size_t rung) {
    const int32_t lvl = rung < p.levels.size()
                            ? static_cast<int32_t>(p.levels[rung])
                            : kPowerLevelBaseline;
    // Bounded on BOTH sides. The taper may only ever lower: a §9.3 table
    // authoring tx_power_level > kPowerLevelBaseline would otherwise produce
    // a rung ceiling ABOVE the operator's flat ceiling, and §10.3 would stop
    // being a ceiling — on both directions at once, since the table is shared.
    return std::clamp(p.max_qdb + (lvl - kPowerLevelBaseline) * kQdbPerLevel,
                      p.min_qdb, p.max_qdb);
}

struct CalibCeiling {
    int8_t last_clean_rssi = 127;  // 127 = never probed
    bool has_bad = false;
    int8_t first_bad_rssi = 0;
};

struct CalibArtifact {
    std::array<int32_t, 8> curve_qdb{};  // §10.2 level-4 baseline
    std::array<int32_t, 8> placement_qdb{};
    std::array<int8_t, 8> placement_rssi{};
    std::array<uint16_t, 8> placement_loss_milli{};
    std::array<CalibCeiling, 8> ceilings{};
};

// §3.15 calibration-word states (byte 0 bits 0-1).
enum class CalibState : uint8_t { kIdle = 0, kRunning = 1, kDone = 2,
                                  kFailed = 3 };

// Edge-triggered actions polled once per tick; the app actuates in order:
// restore FIRST when set (R4: power before rung), then set_qdb, then pin.
struct CalibActions {
    std::optional<uint8_t> pin_rung;  // §9.7 min==max pin
    std::optional<int32_t> set_qdb;   // §10.5 set_power_qdb
    bool restore = false;             // terminal: undo pin + re-place power
    bool artifact_ready = false;      // artifact() is complete — persist it
};

class Calibrator {
  public:
    explicit Calibrator(const CalibrateParams& p)
        : p_(p), seek_(seek_params(p)) {}

    // §10.6's seek knobs are a subset of CalibrateParams; §10.7 builds the
    // same SeekParams from the same config block (§15.2) and differs only in
    // how it gates dwells.
    static SeekParams seek_params(const CalibrateParams& p) {
        SeekParams s;
        s.loss_ok_milli = p.loss_ok_milli;
        s.loss_bad_milli = p.loss_bad_milli;
        s.seek_step_qdb = p.seek_step_qdb;
        s.rssi_guard_dbm = p.rssi_guard_dbm;
        s.min_qdb = p.min_qdb;
        s.max_qdb = p.max_qdb;
        return s;
    }

    // §11.7 CALIBRATE start/abort semantics: false = REJECTED (already
    // running / not running). The app layers the other REJECT conditions
    // (actuator + latched reporter) — core cannot see them.
    bool start(uint64_t now_ms) {
        if (state_ == CalibState::kRunning) return false;
        state_ = CalibState::kRunning;
        started_ms_ = now_ms;
        last_report_ms_ = now_ms;  // armed; the 3 s clock runs from start
        // An abort whose restore was not yet drained (abort → start between
        // ticks) must not fire that restore mid-new-run.
        restore_pending_ = false;
        artifact_pending_ = false;
        fail_reason_ = nullptr;
        artifact_ = {};
        rung_ = 0;
        // Pass 121: rung 0 ramps from the floor. Pass 125's floor rule is
        // what makes that safe when the floor is genuinely unusable.
        qdb_ = seek_.begin(rung_max_qdb(p_, 0)).qdb;
        enter_rung_ = true;
        dwell_start_ms_ = 0;  // set when the pin/power action is emitted
        return true;
    }
    bool abort(uint64_t now_ms) {
        if (state_ != CalibState::kRunning) return false;
        finish(CalibState::kFailed, "abort", now_ms);
        return true;
    }

    // One ACCEPTED §3.5 report (post report-gate). Samples inside the
    // settle window are discarded; the report-loss clock always resets.
    void on_report(int8_t rssi_mean, uint16_t loss_milli, uint32_t uniq,
                   uint64_t now_ms) {
        if (state_ != CalibState::kRunning) return;
        last_report_ms_ = now_ms;
        if (dwell_start_ms_ == 0 || now_ms < dwell_start_ms_) return;
        rssi_sum_ += rssi_mean;
        ++rssi_n_;
        loss_sum_ += static_cast<uint64_t>(loss_milli) * std::max(uniq, 1u);
        loss_w_ += std::max(uniq, 1u);
    }

    CalibActions tick(uint64_t now_ms) {
        CalibActions a;
        if (state_ != CalibState::kRunning) {
            a.restore = take_restore_();
            a.artifact_ready = take_artifact_();
            return a;
        }
        if (now_ms - started_ms_ > p_.hard_cap_ms) {
            finish(CalibState::kFailed, "hard_cap", now_ms);
            a.restore = take_restore_();
            return a;
        }
        if (now_ms - last_report_ms_ > p_.report_loss_abort_ms) {
            // Addendum 4/5 fall out of the sweep: a report blackout is a
            // dwell that delivered nothing, which is simply NOT CLEAN. Above a
            // clean probe that places at it (the addendum-4 retreat); during
            // verify it takes the bounded step-down (addendum 5); below the
            // first clean probe the sweep just keeps climbing. Only a blackout
            // with nothing clean anywhere and nowhere left to climb aborts.
            const double obs = rssi_n_ != 0 ? double(rssi_sum_) / rssi_n_
                                            : double(p_.rssi_guard_dbm) - 60.0;
            const SeekStep s = seek_.on_dwell(DwellVerdict::kBad, obs, 1000);
            // A rung can never COMPLETE on dwells that carried no reports:
            // the placement would be authored from silence. Only a step that
            // keeps probing/verifying is progress; anything terminal under a
            // blackout is the report-loss abort.
            if (s.kind == SeekStep::Kind::kProbe ||
                s.kind == SeekStep::Kind::kVerify) {
                last_report_ms_ = now_ms;  // re-arm at the new power
                return apply_step(a, now_ms, s);
            }
            finish(CalibState::kFailed, "report_loss", now_ms);
            a.restore = take_restore_();
            return a;
        }
        if (enter_rung_) {
            enter_rung_ = false;
            a.pin_rung = rung_;
            a.set_qdb = qdb_;
            begin_dwell(now_ms, p_.probe_dwell_ms);
            return a;
        }
        if (dwell_start_ms_ == 0 || now_ms < dwell_end_ms_) {
            return a;
        }
        // Dwell complete — evaluate with whatever samples arrived. A blank
        // dwell carries no evidence either way: hold at this power and let
        // the report clocks arbitrate (addendum 4 retreat, or abort). This is
        // the craft-only kNoEvidence verdict; §10.7 never produces it.
        if (rssi_n_ == 0) {
            return apply_step(
                a, now_ms, seek_.on_dwell(DwellVerdict::kNoEvidence, 0, 0));
        }
        const double rssi = double(rssi_sum_) / rssi_n_;
        const uint16_t loss = loss_w_ ? static_cast<uint16_t>(
            std::min<uint64_t>(1000, loss_sum_ / loss_w_)) : 0;
        // §10.6 evaluates loss against its own walls, so the verdict is
        // derived here rather than asserted: the craft always has samples to
        // judge with. The ground's §10.7 adapter is the one that asserts kBad
        // for a counter stall (§3.16 two clocks).
        const DwellVerdict v = loss > p_.loss_bad_milli ? DwellVerdict::kBad
                                                        : DwellVerdict::kClean;
        const bool was_verify = seek_.in_verify();
        const SeekStep s = seek_.on_dwell(v, rssi, loss);
        if (was_verify && s.kind == SeekStep::Kind::kDone) {
            artifact_.placement_qdb[rung_] = s.qdb;
            artifact_.placement_rssi[rung_] =
                static_cast<int8_t>(std::lround(rssi));
            artifact_.placement_loss_milli[rung_] = loss;
            placement_qdb_ = s.qdb;
            sync_ceiling();
            return next_rung(a, now_ms);
        }
        return apply_step(a, now_ms, s);
    }

    CalibState state() const { return state_; }
    uint8_t rung() const { return rung_; }
    const char* fail_reason() const { return fail_reason_; }
    const CalibArtifact& artifact() const { return artifact_; }
    // §3.15 calibration-word byte 0.
    uint8_t word() const {
        return static_cast<uint8_t>(
            (static_cast<uint8_t>(state_) & 0x03u) |
            ((state_ == CalibState::kRunning ? rung_ : 0) & 0x07u) << 2);
    }

  private:
    void begin_dwell(uint64_t now_ms, uint32_t dwell_ms) {
        dwell_start_ms_ = now_ms + p_.settle_ms;
        dwell_end_ms_ = dwell_start_ms_ + dwell_ms;
        rssi_sum_ = 0; rssi_n_ = 0; loss_sum_ = 0; loss_w_ = 0;
    }

    // The rung's overload bracket lives in the artifact but is discovered by
    // the seek — mirror it out after every step that can move it.
    void sync_ceiling() {
        auto& c = artifact_.ceilings[rung_];
        c.last_clean_rssi = seek_.last_clean_rssi();
        c.has_bad = seek_.has_bad();
        c.first_bad_rssi = seek_.first_bad_rssi();
    }

    // Turn one PowerSeek decision into §10.6 actions + dwell bookkeeping.
    CalibActions apply_step(CalibActions a, uint64_t now_ms,
                            const SeekStep& s) {
        sync_ceiling();
        qdb_ = s.qdb;
        if (s.power_changed) a.set_qdb = s.qdb;
        switch (s.kind) {
            case SeekStep::Kind::kFailed:
                finish(CalibState::kFailed, s.fail_reason, now_ms);
                a.restore = take_restore_();
                return a;
            case SeekStep::Kind::kVerify:
                begin_dwell(now_ms, p_.verify_dwell_ms);
                return a;
            case SeekStep::Kind::kDone:
            case SeekStep::Kind::kProbe:
                begin_dwell(now_ms, p_.probe_dwell_ms);
                return a;
        }
        return a;
    }

    // §10.6 (Pass 134): a run whose whole product is "no wall exists at any
    // rung" did not measure the thing it exists to measure. Every rung at its
    // §10.3 ceiling with no overload bracket booked anywhere is the signature
    // of a starved feedback path, not of a good link — fewer accepted reports
    // means fewer observed losses means every probe reads clean. Measured:
    // a §7.2 flush regression halved the report rate and produced
    // placement_loss_milli [5,2,3,2,0,0,0,0] with seven rungs at 27 dBm.
    bool found_no_wall_anywhere() const {
        for (size_t m = 0; m < 8; ++m) {
            if (artifact_.ceilings[m].has_bad) return false;
            if (artifact_.placement_qdb[m] < rung_max_qdb(p_, m)) return false;
        }
        return true;
    }

    // Deliberately no in-run report-rate floor. Since Pass 133 the sweep no
    // longer stops at the first wall, so every rung climbs through the whole
    // blacked-out region above its own overload point — on a marginal link
    // that is most of a run's wall time, by design. A rate floor evaluated
    // during a sweep cannot be told from wall evidence (Pass 121 addendum 4)
    // and would fail exactly the runs that are measuring correctly. Report
    // health is a property of the link AT REST and is a §11.7 start
    // precondition in the app layer.
    CalibActions next_rung(CalibActions a, uint64_t now_ms) {
        if (rung_ >= 7) {
            if (found_no_wall_anywhere()) {
                finish(CalibState::kFailed, "no_wall_found", now_ms);
                a.restore = take_restore_();
                return a;  // persists nothing; last-good artifact survives
            }
            for (size_t m = 0; m < 8; ++m) {
                artifact_.curve_qdb[m] =
                    artifact_.placement_qdb[m] -
                    (static_cast<int32_t>(p_.levels[m]) -
                     kPowerLevelBaseline) * kQdbPerLevel;
            }
            finish(CalibState::kDone, nullptr, now_ms);
            a.restore = take_restore_();
            a.artifact_ready = take_artifact_();
            return a;
        }
        ++rung_;
        // Every rung sweeps from the floor. Seeding mid-range from the
        // previous placement was a runtime optimisation that made the result
        // depend on where the sweep began; a measurement should not have that
        // property, and the extra probes cost well under the hard cap.
        const SeekStep s = seek_.begin(rung_max_qdb(p_, rung_));
        qdb_ = s.qdb;
        a.pin_rung = rung_;
        a.set_qdb = s.qdb;
        begin_dwell(now_ms, p_.probe_dwell_ms);
        return a;
    }

    void finish(CalibState s, const char* reason, uint64_t) {
        state_ = s;
        fail_reason_ = reason;
        restore_pending_ = true;
        artifact_pending_ = (s == CalibState::kDone);
        dwell_start_ms_ = 0;
    }
    bool take_restore_() {
        const bool r = restore_pending_;
        restore_pending_ = false;
        return r;
    }
    bool take_artifact_() {
        const bool r = artifact_pending_;
        artifact_pending_ = false;
        return r;
    }

    CalibrateParams p_;
    CalibState state_ = CalibState::kIdle;
    const char* fail_reason_ = nullptr;
    CalibArtifact artifact_{};
    uint64_t started_ms_ = 0;
    uint64_t last_report_ms_ = 0;
    uint64_t dwell_start_ms_ = 0;
    uint64_t dwell_end_ms_ = 0;
    int64_t rssi_sum_ = 0;
    uint32_t rssi_n_ = 0;
    uint64_t loss_sum_ = 0;
    uint64_t loss_w_ = 0;
    uint8_t rung_ = 0;
    int32_t qdb_ = 0;
    int32_t placement_qdb_ = 0;
    bool enter_rung_ = true;
    bool restore_pending_ = false;
    bool artifact_pending_ = false;
    PowerSeek seek_{SeekParams{}};
};

}  // namespace wblink
