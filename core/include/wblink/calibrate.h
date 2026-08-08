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

#include "wblink/calib_dwell.h"
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
    // §10.5/§10.7 (Pass 153): offset space only — a sweep that booked no
    // overload bracket starts its verify no higher than this bound (the
    // efuse reference, 0). Above the reference sits per-unit PA compression
    // a close-range flat field cannot see; placing there requires a
    // measured wall. Unset in absolute space.
    std::optional<int32_t> no_bracket_cap_qdb;
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
        have_best_ = false;
        best_loss_ = 0;
        best_rssi_ = 0.0;
        best_qdb_ = p_.min_qdb;
        placed_loss_ = 0;
        placed_rssi_ = 0.0;
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
    // Valid once on_dwell() has returned kDone: the evidence belonging to the
    // PLACED power, which after a Pass 151 step-back is not the last dwell's.
    uint16_t placed_loss_milli() const { return placed_loss_; }
    double placed_rssi() const { return placed_rssi_; }
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
        // §10.5 (Pass 153): no LIVE bracket booked → the verify walk starts
        // at the reference, not above it. Live means the highest clean probe
        // still sits below the booked wall; a bad probe the sweep later
        // climbed past cleanly is a noise transient (the flat-field noise of
        // docs/findings.md 2026-08-07), and releasing the cap on it would
        // verify from the ceiling — the noise-selected placement the cap
        // exists to forbid. The walk only descends from here, so capping the
        // start caps the placement; everything below is then measured by the
        // walk itself, never inherited from this clamp.
        const bool live_bracket =
            has_bad_ && first_bad_qdb_ > last_clean_->qdb;
        if (p_.no_bracket_cap_qdb && !live_bracket &&
            qdb_ > *p_.no_bracket_cap_qdb) {
            qdb_ = std::max(p_.min_qdb, *p_.no_bracket_cap_qdb);
        }
        phase_ = Phase::kVerify;
        return {SeekStep::Kind::kVerify, qdb_, true, nullptr};
    }

    SeekStep verify_(double rssi, uint16_t loss) {
        // Two independent reasons to step down, and they compose (Pass 151).
        //
        // §10.6 addendum 2 — RECOVERY: near-cliff instability passes a short
        // probe dwell and fails only under sustained exposure (measured: a
        // placement that probed clean verified at 942permille), so loss_ok is
        // enforced here and a failing placement steps down regardless of
        // whether the step improved anything.
        //
        // §10.6 Pass 151 — MINIMUM HUNT: on a compressing PA the loss minimum
        // is INTERIOR, so "the first acceptable reading" and "the best
        // reading" are different points and the old rule stopped at the
        // former. Measured on the craft's 8822EU at MCS 5 — 0 dB: 19permille,
        // -2 dB: 6permille, -4 dB: 2permille, -6 dB: 1permille. The only way
        // to know a lower power is better is to try one, so the first verify
        // always trials a step down.
        // The BEST reading of this verify walk is what the rung places at.
        // Tracking only the previous reading was wrong whenever nothing was
        // acceptable: the recovery descent then walked straight past its own
        // best and placed wherever the budget ran out. Measured on the
        // ground's uplink at 10 m — verify read 30, 25, 20, then 45permille —
        // it placed the 45 and failed the rung, having measured the 20 three
        // dwells earlier. A plain have/value pair rather than std::optional:
        // GCC's -Wmaybe-uninitialized cannot see through the optional across
        // inlining and warns on the ARM cross-build.
        const bool is_best = !have_best_ || loss < best_loss_;
        if (is_best) {
            have_best_ = true;
            best_loss_ = loss;
            best_rssi_ = rssi;
            best_qdb_ = qdb_;
        }

        const bool unacceptable = loss > p_.loss_ok_milli;
        const bool can_descend =
            verify_descents_ < p_.verify_descent_budget && qdb_ > p_.min_qdb;

        // Descend for either reason, and they compose: RECOVERY while the
        // reading is not yet acceptable (§10.6 addendum 2), and the MINIMUM
        // HUNT while each step is still improving on the best seen. The first
        // verify is always a best, so the trial step is structural.
        if ((unacceptable || is_best) && can_descend) {
            if (loss > p_.loss_bad_milli) note_bad_(rssi);
            ++verify_descents_;
            qdb_ = std::max(p_.min_qdb, qdb_ - p_.seek_step_qdb);
            return {SeekStep::Kind::kVerify, qdb_, true, nullptr};
        }
        // Place at the best reading, which is usually a power we have already
        // stepped past — power_changed walks the actuator back up to it. It
        // may still be unacceptable if nothing was: §10.6 records that floor,
        // §10.7 refuses it (both unchanged).
        placed_loss_ = best_loss_;
        placed_rssi_ = best_rssi_;
        const bool moved = qdb_ != best_qdb_;
        qdb_ = best_qdb_;
        return {SeekStep::Kind::kDone, qdb_, moved, nullptr};
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
    // Pass 151: the verify phase hunts the loss minimum, so kDone may place at
    // a point the seek has already stepped past. The caller must record THAT
    // point's evidence, not the last dwell's, or the artifact reports a
    // placement paired with a different power's loss and RSSI.
    bool have_best_ = false;
    uint16_t best_loss_ = 0;
    double best_rssi_ = 0.0;
    int32_t best_qdb_ = 0;
    uint16_t placed_loss_ = 0;
    double placed_rssi_ = 0.0;
};

struct CalibrateParams {
    uint16_t loss_ok_milli = 15;  // §15.2 policy.calibration seeds
    uint16_t loss_bad_milli = 50;
    int32_t seek_step_qdb = 16;  // 4 dB per seek probe (Pass 121)
    int rssi_guard_dbm = -6;     // token RX-abuse backstop (P121 addendum)
    int32_t min_qdb = 4;         // 1 dBm
    int32_t max_qdb = 108;       // 27 dBm
    uint32_t settle_ms = 300;  // TXAGC settle (Pass 153: no report window)
    // §3.16 (Pass 153): dwell burst sizes in PROBE FRAMES, shared with §10.7.
    // Pass 132's decidability rule holds: 1000/N <= loss_ok_milli.
    uint16_t dwell_probe_frames = 500;
    uint16_t dwell_verify_frames = 1000;
    DwellSendParams dwell;  // pacing + tally re-elicitation bounds
    uint32_t hard_cap_ms = 600000;
    // The §9.3 table's tx_power_level per MCS — the authored curve is the
    // level-4 baseline, so placements are compensated per §10.2.
    std::array<uint8_t, 8> levels{4, 4, 3, 3, 2, 2, 1, 1};
    // §10.6 (Pass 151) whether `levels` also narrows the per-rung sweep
    // CEILING, as distinct from encoding the §10.2 curve. False in offset
    // space, for two reasons. The taper exists so a sweep cannot walk a
    // high-order rung to full power looking for a wall the PA reaches first —
    // but a relative backend's offset is measured against the efuse
    // **per-rate** table, which already applies exactly that backoff, so
    // taking it again double-counts. And it does not merely shade the result:
    // against the default 24 qdb window a level-1 rung tapers to min == max
    // and cannot sweep at all. The curve compensation below is unaffected —
    // it is an encoding, and round-trips in either space.
    bool taper_rung_ceiling = true;
};

// §10.3 (Pass 134) per-rung sweep ceiling: the flat sanity ceiling tapered by
// the rung's §10.2 level intent, so a sweep cannot walk a high-order rung to
// full power looking for a wall the PA reaches first. Derived from the §9.3
// table both ends already agree on by hash — no new key, no new wire. The
// caller has already folded the adapter's §10.3 max_power_qdb into max_qdb.
inline int32_t rung_max_qdb(const CalibrateParams& p, size_t rung) {
    if (!p.taper_rung_ceiling) return p.max_qdb;
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
        : p_(p), dwell_(p.dwell), seek_(seek_params(p)) {}

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
        // §10.5 (Pass 153): taper_rung_ceiling is the space discriminator
        // (Pass 151) — offset space caps unbracketed placements at the
        // efuse reference. §10.7 sets the same cap on its own SeekParams.
        if (!p.taper_rung_ceiling) s.no_bracket_cap_qdb = 0;
        return s;
    }

    // §11.7 CALIBRATE start/abort semantics: false = REJECTED (already
    // running / not running). The app layers the other REJECT conditions
    // (actuator + latched reporter) — core cannot see them.
    bool start(uint64_t now_ms) {
        if (state_ == CalibState::kRunning) return false;
        state_ = CalibState::kRunning;
        started_ms_ = now_ms;
        // §3.16: a fresh run_id opens a new run at the receiver. Derived from
        // the injected clock (never wall time) and kept non-zero.
        run_id_ = static_cast<uint32_t>(now_ms) | 1u;
        dwell_seq_ = 0;
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

    // One ACCEPTED §3.16 TALLY (post source/destination gate). Stale
    // run/dwell ids are ignored inside the sender.
    void on_tally(uint32_t run_id, uint16_t dwell_id, uint16_t received,
                  uint32_t rssi_sum_dbm, uint8_t rx_mcs,
                  uint8_t adapter_fingerprint) {
        if (state_ != CalibState::kRunning) return;
        (void)dwell_.on_tally(run_id, dwell_id, received, rssi_sum_dbm,
                              rx_mcs, adapter_fingerprint);
    }

    // §3.16 probe emission — drained by the app once per tick after
    // new_tick(); the app encodes (run_id, dwell_id, seq, dwell_count) and
    // injects, padded to the negotiated budget. Gated on the settle window so
    // TXAGC has settled before the first probe of a dwell goes out.
    void new_tick() { dwell_.new_tick(); }
    DwellProbeOut next_probe(uint64_t now_ms) {
        if (state_ != CalibState::kRunning || dwell_start_ms_ == 0 ||
            now_ms < dwell_start_ms_) {
            return DwellProbeOut{};
        }
        return dwell_.next_probe(now_ms);
    }
    uint32_t probe_run_id() const { return run_id_; }
    uint16_t probe_dwell_id() const { return dwell_.dwell_id(); }
    uint16_t probe_dwell_count() const { return dwell_count_; }

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
        if (dwell_.state() == DwellState::kNoEvidence) {
            // §3.16 evidence blackout: the dwell's tally never arrived
            // despite bounded re-elicitation. Addendum 4/5 semantics with the
            // trigger renamed — a blackout is a dwell that delivered nothing,
            // which is simply NOT CLEAN. Above a clean probe that places at
            // it (the addendum-4 retreat); during verify it takes the bounded
            // step-down (addendum 5); below the first clean probe the sweep
            // just keeps climbing. Only a blackout with nothing clean
            // anywhere and nowhere left to climb aborts.
            const double obs = double(p_.rssi_guard_dbm) - 60.0;
            const SeekStep s = seek_.on_dwell(DwellVerdict::kBad, obs, 1000);
            // A rung can never COMPLETE on a dwell that carried no tally: the
            // placement would be authored from silence. Only a step that
            // keeps probing/verifying is progress; anything terminal under a
            // blackout is the evidence-loss abort.
            if (s.kind == SeekStep::Kind::kProbe ||
                s.kind == SeekStep::Kind::kVerify) {
                return apply_step(a, now_ms, s);
            }
            finish(CalibState::kFailed, "evidence_lost", now_ms);
            a.restore = take_restore_();
            return a;
        }
        if (enter_rung_) {
            enter_rung_ = false;
            a.pin_rung = rung_;
            a.set_qdb = qdb_;
            begin_dwell(now_ms, p_.dwell_probe_frames);
            return a;
        }
        if (dwell_.state() != DwellState::kDone) {
            return a;  // burst in flight or tally still awaited
        }
        // Dwell complete — the tally is the evidence. received == 0 is an
        // ordinary 1000‰ reading (§3.16), not a blackout; its RSSI mean is
        // undefined, so the guard fallback stands in.
        const DwellResult& res = dwell_.result();
        const uint16_t loss = res.loss_milli();
        const double rssi =
            res.received > 0
                ? double(static_cast<int32_t>(res.rssi_sum_dbm)) /
                      res.received
                : double(p_.rssi_guard_dbm) - 60.0;
        dwell_.reset();  // consumed; next begin_dwell re-arms
        // §10.6 evaluates loss against its own walls, so the verdict is
        // derived here rather than asserted.
        const DwellVerdict v = loss > p_.loss_bad_milli ? DwellVerdict::kBad
                                                        : DwellVerdict::kClean;
        const bool was_verify = seek_.in_verify();
        const SeekStep s = seek_.on_dwell(v, rssi, loss);
        if (was_verify && s.kind == SeekStep::Kind::kDone) {
            // Pass 151: read the placed point's own evidence from the seek.
            // `loss`/`rssi` here are the LAST dwell's, which after a
            // minimum-hunt step-back belongs to a different power.
            artifact_.placement_qdb[rung_] = s.qdb;
            artifact_.placement_rssi[rung_] =
                static_cast<int8_t>(std::lround(seek_.placed_rssi()));
            artifact_.placement_loss_milli[rung_] = seek_.placed_loss_milli();
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
    void begin_dwell(uint64_t now_ms, uint16_t frames) {
        dwell_start_ms_ = now_ms + p_.settle_ms;
        dwell_count_ = frames;
        ++dwell_seq_;  // §3.16: strictly increasing within the run
        (void)dwell_.begin(run_id_, dwell_seq_, frames, now_ms);
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
                begin_dwell(now_ms, p_.dwell_verify_frames);
                return a;
            case SeekStep::Kind::kDone:
            case SeekStep::Kind::kProbe:
                begin_dwell(now_ms, p_.dwell_probe_frames);
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
            // §10.6 (Pass 153): absolute space only — in offset space the
            // ceiling is offset 0, the §10.5 boot-safe placement, and a
            // flat-at-ceiling read is the expected close-range result.
            if (p_.taper_rung_ceiling && found_no_wall_anywhere()) {
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
        begin_dwell(now_ms, p_.dwell_probe_frames);
        return a;
    }

    void finish(CalibState s, const char* reason, uint64_t) {
        state_ = s;
        fail_reason_ = reason;
        restore_pending_ = true;
        artifact_pending_ = (s == CalibState::kDone);
        dwell_start_ms_ = 0;
        dwell_.reset();
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
    uint64_t dwell_start_ms_ = 0;
    uint32_t run_id_ = 0;
    uint16_t dwell_seq_ = 0;
    uint16_t dwell_count_ = 0;
    DwellSender dwell_{DwellSendParams{}};
    uint8_t rung_ = 0;
    int32_t qdb_ = 0;
    int32_t placement_qdb_ = 0;
    bool enter_rung_ = true;
    bool restore_pending_ = false;
    bool artifact_pending_ = false;
    PowerSeek seek_{SeekParams{}};
};

}  // namespace wblink
