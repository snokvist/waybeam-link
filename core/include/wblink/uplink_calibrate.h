// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §10.7 ground-uplink calibration orchestrator.
//
// The counterpart to §10.6's Calibrator: same PowerSeek ramp, same eight-rung
// loop, different evidence. The craft measures against live video (thousands
// of loss samples a second, so dwells are wall-clock); the ground measures by
// sending a COUNTED BURST of probe reports and asking the craft how many
// arrived (Pass 132).
//
// That one choice is why this file is short. Pass 125 measured loss on a
// stream whose rate it did not control, and every mechanism it needed to do
// that — the `emitted = E_B - E_A` anchoring identity, the local-epoch
// blackout fallback, that fallback's scoring rule, the one-shot ambiguous
// extension — existed to answer "how many did the ground send?". The ground
// always knew: Reporter commits an epoch only on successful injection, so its
// own epoch delta is exactly the frames the radio took. Burst, drain, divide.
//
// Probe traffic is synthetic, which §10.7 originally forbade. That rule was
// written for a craft in flight; calibration is stationary and pre-flight.
//
// Pure and time-injected: it consumes §3.16 QualitySamples and ticks, and
// emits polled actions. It does not own the UplinkQualityGate (that needs the
// app's notion of the selected craft) and never touches a radio or a file.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "wblink/calibrate.h"        // PowerSeek, SeekParams, CalibState
#include "wblink/uplink_quality.h"   // QualitySample

namespace wblink {

// §10.7 rung -> rate identity. Resolved by the CALLER from the §9.3 profile
// table and passed in, so core/ keeps no table dependency — the same shape as
// §10.6's `levels` array. Seeds match the authored table (long GI on 0/1,
// short on 2..7); a placement measured at a GI the uplink cannot be commanded
// to would be a number about nothing.
struct UplinkRate {
    uint8_t mcs = 0;
    bool short_gi = false;
    friend bool operator==(const UplinkRate&, const UplinkRate&) = default;
};

inline constexpr size_t kUplinkRungs = 8;

struct UplinkCalibParams {
    SeekParams seek;
    // §10.3 (Pass 134): the §9.3 table's tx_power_level per rung, tapering
    // seek.max_qdb so the ground cannot walk its own uplink PA to full power
    // on a high-order rung. The ground half carries the stronger case for the
    // mask than §10.6 does — this placement auto-applies at boot with no
    // operator between the measurement and the actuator.
    std::array<uint8_t, kUplinkRungs> levels{4, 4, 3, 3, 2, 2, 1, 1};
    // §10.6/§10.7 (Pass 151): same meaning as CalibrateParams — false in
    // offset space, where the efuse per-rate table the offset is measured
    // against already carries this backoff, and where re-applying it collapses
    // a level-1 rung to min == max.
    bool taper_rung_ceiling = true;
    uint32_t settle_ms = 300;       // shared with §10.6: TXAGC settle
    // §10.7 burst sizes (Pass 132). 100 makes one lost probe 10permille —
    // inside loss_ok_milli — and five 50permille, exactly the bad wall. That
    // is why there is no longer an ambiguous extension: the old 40-probe gate
    // put one loss at 25permille, BETWEEN the walls, and the extension to 80
    // existed only to resolve a reading the gate was too small to make. With
    // the ground choosing the burst size, the right fix is a big enough burst.
    uint32_t probe_epochs = 100;
    uint32_t verify_epochs = 200;
    // Silence after the burst, so the craft's counter reflects the whole of it
    // before the dwell is scored. Longer than one §3.16 period (500 ms at
    // 2 Hz) so at least one packet built after the last probe landed arrives.
    // This is what removes the in-flight boundary error that the Pass 125
    // epoch-anchoring identity existed to handle.
    uint32_t drain_ms = 600;
    uint32_t liveness_ms = 2000;
    uint32_t hard_cap_ms = 600000;
    std::array<UplinkRate, kUplinkRungs> rungs{{{0, false}, {1, false},
                                                {2, true},  {3, true},
                                                {4, true},  {5, true},
                                                {6, true},  {7, true}}};
};

// One rung's result. `placements` in the §10.7 artifact is a list of these —
// eight since Pass 131, one per rung in ascending order.
struct UplinkPlacement {
    uint8_t mcs = 0;
    bool short_gi = false;
    int32_t placement_qdb = 0;
    int8_t placement_rssi_dbm = 0;
    uint16_t placement_loss_milli = 0;
    int32_t last_clean_qdb = 0;
    bool has_first_bad = false;
    int32_t first_bad_qdb = 0;
};

// Edge-triggered, polled once per tick. The app actuates set_rate then
// set_qdb — RATE FIRST, mirroring §10.6's R4 rule that the actuator gating the
// other goes first — and on `restore` returns BOTH to their §10.7 owners. The
// single-shot signal lives here so no exit path can skip it: since Pass 131
// the run borrows the uplink's operating rung as well as its power, and a run
// that dies at rung 5 must not leave the uplink transmitting at MCS5.
struct UplinkCalibActions {
    std::optional<UplinkRate> set_rate;
    std::optional<int32_t> set_qdb;
    // §10.7 (Pass 132): how many more probe reports the ground should emit
    // right now. 0 means STOP — the burst is complete and the drain window is
    // open. Absent means "unchanged". The app hands this to
    // Reporter::set_probe_budget; the restore edge clears probe mode.
    std::optional<uint32_t> probe_budget;
    bool restore = false;
    bool artifact_ready = false;
};

class UplinkCalibrator {
  public:
    explicit UplinkCalibrator(const UplinkCalibParams& p)
        : p_(p), seek_(p.seek) {}

    // §10.3/§11.7 0x0A (Pass 135): move the sweep's flat ceiling — the value
    // rung_ceiling_qdb() tapers per rung — after construction, so a runtime
    // power tier bounds the NEXT run. The params are copied by the
    // constructor, so mutating the caller's UplinkCalibParams does nothing;
    // that was the original shape of this and it was silently inert.
    // Refused while running: the seek is mid-descent against the old bound
    // and re-basing it would score a dwell at one ceiling against another.
    // Only p_ needs it: enter_rung() hands seek_ its bound explicitly via
    // seek_.begin(rung_ceiling_qdb(rung_)), and that reads p_.seek.max_qdb.
    bool set_max_qdb(int32_t qdb) {
        if (state_ == CalibState::kRunning) return false;
        p_.seek.max_qdb = qdb;
        return true;
    }

    bool start(uint64_t now_ms, uint32_t local_epoch) {
        if (state_ == CalibState::kRunning) return false;
        state_ = CalibState::kRunning;
        started_ms_ = now_ms;
        fail_reason_ = nullptr;
        restore_pending_ = false;
        artifact_pending_ = false;
        placements_.clear();
        ceiling_reason_ = nullptr;
        rung_ = 0;
        enter_rung(now_ms, local_epoch);
        return true;
    }

    bool abort(uint64_t now_ms) {
        if (state_ != CalibState::kRunning) return false;
        finish(CalibState::kFailed, "abort");
        (void)now_ms;
        return true;
    }

    // Operator ruling: persistence IS the deliverable. §10.7 is a one-time
    // commissioning step whose whole premise is that it is persisted on both
    // sides, so a run whose artifact never reached disk did not commission
    // anything — it applied a placement that dies at the next boot. Reporting
    // `done` there is the same false success C2 removed, one layer out: the
    // Hub menu binds the state field, and `fingerprint: 0` was the only
    // signal that anything went wrong. The caller invokes this when the store
    // write fails; the restore edge re-arms so the actuator returns to the
    // owner that was there before the run.
    void fail_persist() {
        if (state_ != CalibState::kDone) return;
        finish(CalibState::kFailed, "artifact_write_failed");
    }

    // One accepted §3.16 packet. Only `reports_delta` and `rssi_sum_delta`
    // are consumed: the denominator is the ground's own burst count, so the
    // craft's `last_report_epoch` is observability now, not arithmetic.
    // Samples inside the settle window are discarded exactly as §10.6
    // discards its report samples.
    void on_sample(const QualitySample& s, uint64_t now_ms) {
        if (state_ != CalibState::kRunning) return;
        // The craft restarted its counter domain mid-burst, so everything
        // counted so far belongs to the old domain and the burst has no
        // trustworthy numerator. Re-run it at this power rather than score it.
        if (s.resynced) {
            restart_dwell(now_ms);
            return;
        }
        if (!s.progressed) return;
        if (now_ms < dwell_start_ms_) return;
        received_ += s.reports_delta;
        rssi_sum_ += s.rssi_sum_delta;
    }

    // local_epoch is the ground's own Reporter::epoch() — a count of reports
    // the radio ACTUALLY took (§3.5 commits on injection only), which is why
    // it can serve as the dwell denominator directly. quality_live is the
    // §3.16 LIVENESS clock: packet arrival, never counter progress. Losing it
    // means the run has no observer. A stalled counter under live feedback is
    // still evidence, not a timeout — it just reads as a 1000permille burst.
    UplinkCalibActions tick(uint64_t now_ms, uint32_t local_epoch,
                            bool quality_live, bool burst_spent) {
        UplinkCalibActions a;
        if (state_ != CalibState::kRunning) {
            a.restore = take_restore_();
            a.artifact_ready = take_artifact_();
            return a;
        }
        if (pending_rate_) {
            a.set_rate = *pending_rate_;
            pending_rate_.reset();
        }
        if (pending_qdb_) {
            a.set_qdb = *pending_qdb_;
            pending_qdb_.reset();
        }
        if (now_ms - started_ms_ > p_.hard_cap_ms) {
            finish(CalibState::kFailed, "hard_cap");
            a.restore = take_restore_();
            return a;
        }
        if (!quality_live) {
            finish(CalibState::kFailed, "quality_lost");
            a.restore = take_restore_();
            return a;
        }
        if (now_ms < dwell_start_ms_) return a;  // settling
        // Arm the burst at the END of settle, so the probes are counted at
        // the power they are meant to measure and not at the previous one.
        if (!dwell_epoch_armed_) {
            dwell_epoch_armed_ = true;
            dwell_local_epoch_ = local_epoch;
        }

        // Issue the burst ONCE, then wait for the reporter to say it has been
        // built. Topping the budget up from the committed epoch count instead
        // is a live defect, found on the bench: the §7.2 return path batches
        // reports and commits their epochs at the next quiet gap, so `sent`
        // lags what has been built by the whole held batch. Re-arming against
        // the lagging number emitted 3480 probes for a 100-probe burst and
        // flooded the return path until the craft's §3.16 could not get out.
        // Budget spent at build time, burst ended at build time: one clock.
        if (!burst_issued_) {
            burst_issued_ = true;
            a.probe_budget = target_epochs_;
            return a;
        }
        if (!burst_spent) return a;   // still emitting
        // Burst complete. Go silent and let the craft's counter settle before
        // scoring — that silence is what makes `received_` reflect the WHOLE
        // burst instead of however much had landed at an arbitrary instant.
        if (drain_until_ms_ == 0) {
            drain_until_ms_ = now_ms + p_.drain_ms;
            return a;
        }
        if (now_ms < drain_until_ms_) return a;
        // `sent` is read at the END of the drain, so the held batch has been
        // injected and its epochs committed. It is the exact count of probes
        // the radio took -- which may be under the burst size if any build
        // was refused, and that is fine: the denominator is what went out.
        return evaluate(a, now_ms, local_epoch,
                        local_epoch - dwell_local_epoch_);
    }

    CalibState state() const { return state_; }
    const char* fail_reason() const { return fail_reason_; }
    // Every completed rung, ascending. Empty until the first rung verifies,
    // and only complete (8 entries) when state() is kDone — §10.7 persists
    // nothing on a partial run, because a truncated artifact would auto-apply
    // at the next boot looking like a finished commissioning.
    const std::vector<UplinkPlacement>& placements() const {
        return placements_;
    }
    uint8_t rung() const { return rung_; }
    // Why the sweep stopped short of eight rungs, or nullptr if it did not.
    // A `done` run with fewer than kUplinkRungs placements reached a geometry
    // ceiling; this says which wall it hit there.
    const char* ceiling_reason() const { return ceiling_reason_; }
    const UplinkRate& rate() const { return p_.rungs[rung_]; }
    int32_t qdb() const { return qdb_; }
    // Probes delivered so far in this burst. The burst SIZE is dwell_target;
    // progress toward ending the dwell is the ground's own emission count,
    // which the caller already has.
    uint32_t dwell_progress() const { return received_; }
    uint32_t dwell_target() const { return target_epochs_; }

    // What the last completed dwell actually observed. §10.7's per-run record
    // (duration, samples/dwell, loss, RSSI, bracket) is the campaign's
    // deliverable, and a run that fails is exactly the one whose dwells have
    // to be readable — "verify_failed" with no numbers behind it is not a
    // finding, it is a rumour. `seq` increments per completed dwell so a
    // caller can log edges without polling state.
    struct DwellRecord {
        uint32_t seq = 0;
        uint8_t rung = 0;
        int32_t qdb = 0;
        bool verify = false;
        uint32_t sent = 0;
        uint32_t received = 0;
        uint16_t loss_milli = 0;
        int32_t rssi_mean = 0;
        uint32_t target = 0;
    };
    const DwellRecord& last_dwell() const { return last_; }

  private:
    // Commit rung_ and start its sweep from the floor. Every rung sweeps from
    // min_qdb: seeding one mid-range from the previous placement made the
    // result depend on where the sweep began (Pass 130), and that is no more
    // acceptable across rungs than it was within one.
    // §10.3 mask, same derivation as §10.6's rung_max_qdb — the seek's own
    // max_qdb is the flat ceiling the caller already narrowed by the
    // adapter's max_power_qdb.
    int32_t rung_ceiling_qdb(size_t rung) const {
        if (!p_.taper_rung_ceiling) return p_.seek.max_qdb;
        const int32_t lvl = rung < p_.levels.size()
                                ? static_cast<int32_t>(p_.levels[rung])
                                : kPowerLevelBaseline;
        // Bounded on both sides, same reason as §10.6's rung_max_qdb: the
        // taper may only lower, never raise the operator's flat ceiling.
        return std::clamp(
            p_.seek.max_qdb + (lvl - kPowerLevelBaseline) * kQdbPerLevel,
            p_.seek.min_qdb, p_.seek.max_qdb);
    }

    void enter_rung(uint64_t now_ms, uint32_t local_epoch) {
        pending_rate_ = p_.rungs[rung_];
        have_clean_ = false;
        confirming_ = false;
        const SeekStep s = seek_.begin(rung_ceiling_qdb(rung_));
        qdb_ = s.qdb;
        pending_qdb_ = s.qdb;
        begin_dwell(now_ms, local_epoch, p_.probe_epochs);
    }

    // Re-arm the current dwell at the current power, keeping its target. The
    // ground epoch anchor re-arms with it (the first post-settle tick), so
    // both clocks restart together.
    void restart_dwell(uint64_t now_ms) {
        dwell_start_ms_ = now_ms + p_.settle_ms;
        dwell_epoch_armed_ = false;
        drain_until_ms_ = 0;
        burst_issued_ = false;
        received_ = 0;
        rssi_sum_ = 0;
    }

    void begin_dwell(uint64_t now_ms, uint32_t local_epoch, uint32_t target) {
        dwell_start_ms_ = now_ms + p_.settle_ms;
        dwell_local_epoch_ = local_epoch;
        dwell_epoch_armed_ = false;
        drain_until_ms_ = 0;
        burst_issued_ = false;
        target_epochs_ = target;
        received_ = 0;
        rssi_sum_ = 0;
    }

    // Score a completed burst. `sent` is the ground's own exact count.
    UplinkCalibActions evaluate(UplinkCalibActions a, uint64_t now_ms,
                                uint32_t local_epoch, uint32_t sent) {
        const bool verify = seek_.in_verify();
        // A burst that delivered nothing is 1000permille and its RSSI is the
        // synthetic guard value — the seek's floor evidence, unchanged. Any
        // delivery at all scores the measured loss against the burst size and
        // takes RSSI from the probes that actually arrived.
        uint16_t loss = 1000;
        double rssi = static_cast<double>(p_.seek.rssi_guard_dbm) - 60.0;
        if (sent > 0 && received_ > 0) {
            const uint32_t lost = sent > received_ ? sent - received_ : 0;
            loss = static_cast<uint16_t>(
                std::min<uint64_t>(1000, uint64_t{lost} * 1000 / sent));
            rssi = static_cast<double>(rssi_sum_) /
                   static_cast<double>(received_);
        }
        DwellVerdict v = loss > p_.seek.loss_bad_milli ? DwellVerdict::kBad
                                                       : DwellVerdict::kClean;
        // §10.7 (Pass 133): the FIRST clean probe of a rung is re-run once at
        // the same power before it is allowed to establish `last_clean`.
        // Measured at 10 m: rung 6 read 100/100 at 1.0 dBm on a link whose
        // RSSI there was -77 dBm — not physical, a small-sample fluke — and
        // that became the placement candidate, so the rung failed
        // `verify_failed` instead of the honest `no_clean_point`. Only the
        // first is confirmed: later clean probes climb from an already
        // established clean point, and confirming every one would double the
        // probe count on a rung that sweeps clean to the top.
        if (!verify && v == DwellVerdict::kClean && !have_clean_ &&
            !confirming_) {
            confirming_ = true;
            begin_dwell(now_ms, local_epoch, target_epochs_);
            return a;   // same power, same burst size; its verdict decides
        }
        confirming_ = false;
        if (!verify && v == DwellVerdict::kClean) have_clean_ = true;
        last_ = DwellRecord{last_.seq + 1,   rung_, qdb_,  verify,
                            sent,            received_,
                            loss,            static_cast<int32_t>(std::lround(rssi)),
                            target_epochs_};
        const SeekStep s = seek_.on_dwell(v, rssi, loss);
        qdb_ = s.qdb;
        if (s.power_changed) a.set_qdb = s.qdb;
        switch (s.kind) {
            case SeekStep::Kind::kFailed:
                return rung_unreachable(a, s.fail_reason);
            case SeekStep::Kind::kDone:
                // §10.7 does NOT inherit §10.6's "record the still-failing
                // floor" rule. The craft's artifact is a record the operator
                // reads; the ground's AUTO-APPLIES to the live uplink at boot
                // and gates the sequencer's downlink phase. PowerSeek reaches
                // kDone with loss > loss_ok_milli only when the descent budget
                // or the floor is exhausted — placing there would report
                // success for an uplink that just measured unusable, persist
                // it, and let CalibSequencer start the downlink across it,
                // defeating the order law through a SUCCESS state that no
                // interlock inspects. Fail instead; nothing is persisted.
                //
                // Pass 151: judge the PLACED point, not the last dwell — a
                // minimum-hunt step-back places one step above where the
                // final dwell was measured.
                if (seek_.placed_loss_milli() > p_.seek.loss_ok_milli) {
                    return rung_unreachable(a, "verify_failed");
                }
                {
                    UplinkPlacement pl;
                    pl.mcs = p_.rungs[rung_].mcs;
                    pl.short_gi = p_.rungs[rung_].short_gi;
                    pl.placement_qdb = s.qdb;
                    pl.placement_rssi_dbm =
                        static_cast<int8_t>(std::lround(seek_.placed_rssi()));
                    pl.placement_loss_milli = seek_.placed_loss_milli();
                    // Read the bracket from the seek rather than tracking a
                    // second copy here. The duplicate booked the COLD floor as
                    // the overload ceiling — measured on the bench as
                    // `first_bad_qdb: 4, last_clean_qdb: 108` — because only
                    // the seek applies the "bad ABOVE a clean probe" rule.
                    pl.last_clean_qdb = seek_.last_clean_qdb();
                    pl.has_first_bad = seek_.has_bad();
                    pl.first_bad_qdb = seek_.first_bad_qdb();
                    placements_.push_back(pl);
                }
                return next_rung(a, now_ms, local_epoch);
            case SeekStep::Kind::kVerify:
                begin_dwell(now_ms, local_epoch, p_.verify_epochs);
                return a;
            case SeekStep::Kind::kProbe:
                begin_dwell(now_ms, local_epoch, p_.probe_epochs);
                return a;
        }
        return a;
    }

    // §10.7 (Pass 133): a rung with no usable placement CAPS the sweep rather
    // than failing the whole run. Rungs get monotonically harder — a rung that
    // cannot hold a clean placement at this geometry guarantees every rung
    // above it cannot either — so continuing is wasted time and discarding the
    // rungs that DID calibrate is wasted evidence. Measured at 10 m: rungs 0-5
    // placed cleanly (0-15permille) and were thrown away because MCS6 needs
    // ~-70 dBm and the uplink delivers -77 there.
    //
    // `placements.size()` IS the ceiling — no schema change, and the loader
    // already accepts any length. Rung 0 is the exception: with nothing
    // measured there is no result, so that stays a failure.
    UplinkCalibActions rung_unreachable(UplinkCalibActions a,
                                        const char* reason) {
        if (placements_.empty()) {
            finish(CalibState::kFailed, reason);
            a.restore = take_restore_();
            return a;
        }
        ceiling_reason_ = reason;
        finish(CalibState::kDone, nullptr);
        a.restore = take_restore_();
        a.artifact_ready = take_artifact_();
        return a;
    }

    // §10.7 (Pass 134 addendum): the same refusal §10.6 carries. The original
    // Pass 134 ruling argued the ground did not need it — that a ground
    // reading clean at every power on every rung must have a dead §3.16
    // counter stream, which the liveness expiry already catches. DEVICE
    // EVIDENCE FALSIFIED THAT: a bench-range run with a fully live counter
    // stream (verify dwells returned real 0-10permille values) placed all eight
    // rungs at their §10.3 ceilings with no bracket anywhere, reached `done`,
    // and persisted a curve that is the mask read back rather than a
    // measurement. At 1 m no wall exists — that is a property of the
    // geometry, not of the feedback path. The ground carries the STRONGER
    // case for the refusal, because its artifact auto-applies at boot with no
    // operator between the measurement and the actuator.
    bool found_no_wall_anywhere() const {
        if (placements_.size() != kUplinkRungs) return false;
        for (size_t m = 0; m < placements_.size(); ++m) {
            if (placements_[m].has_first_bad) return false;
            if (placements_[m].placement_qdb < rung_ceiling_qdb(m)) return false;
        }
        return true;
    }

    // A rung verified. Advance, or finish the run on the last one. Mirrors
    // §10.6's Calibrator::next_rung deliberately: same loop, same order, so
    // the two directions cannot drift into different rung semantics.
    UplinkCalibActions next_rung(UplinkCalibActions a, uint64_t now_ms,
                                 uint32_t local_epoch) {
        if (size_t{rung_} + 1 >= kUplinkRungs) {
            if (found_no_wall_anywhere()) {
                finish(CalibState::kFailed, "no_wall_found");
                a.restore = take_restore_();
                return a;  // persists nothing; last-good artifact survives
            }
            finish(CalibState::kDone, nullptr);
            a.restore = take_restore_();
            a.artifact_ready = take_artifact_();
            return a;
        }
        ++rung_;
        enter_rung(now_ms, local_epoch);
        // enter_rung queues the actuation for the next tick; hand it over now
        // so the rung boundary costs no extra tick of dwell time.
        if (pending_rate_) {
            a.set_rate = *pending_rate_;
            pending_rate_.reset();
        }
        if (pending_qdb_) {
            a.set_qdb = *pending_qdb_;
            pending_qdb_.reset();
        }
        return a;
    }

    void finish(CalibState s, const char* reason) {
        state_ = s;
        fail_reason_ = reason;
        // Every exit restores. §10.7: abort, shutdown, retune conflict and
        // failure must never leave the last probe power on the adapter.
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

    UplinkCalibParams p_;
    PowerSeek seek_;
    uint8_t rung_ = 0;
    CalibState state_ = CalibState::kIdle;
    const char* fail_reason_ = nullptr;
    std::vector<UplinkPlacement> placements_;
    uint64_t started_ms_ = 0;
    uint64_t dwell_start_ms_ = 0;
    uint32_t dwell_local_epoch_ = 0;
    uint32_t target_epochs_ = 0;
    uint64_t drain_until_ms_ = 0;
    bool burst_issued_ = false;
    bool have_clean_ = false;
    bool confirming_ = false;
    const char* ceiling_reason_ = nullptr;
    uint32_t received_ = 0;
    int64_t rssi_sum_ = 0;
    int32_t qdb_ = 0;
    bool dwell_epoch_armed_ = false;
    bool restore_pending_ = false;
    bool artifact_pending_ = false;
    std::optional<UplinkRate> pending_rate_;
    std::optional<int32_t> pending_qdb_;
    DwellRecord last_{};
};

// §10.7 (Pass 125) bi-directional sequencer. Calibration is a one-time
// commissioning step, so the operator gets ONE action; this runs the uplink
// phase, then — only on success — the §11.7 downlink campaign.
//
// It lives on the ground Link node because that is where both actuators
// already are: this node owns its uplink power and is the §11.7 issuer, and
// every §10.7 interlock is local to it. Pure state here, actuation by the
// caller, so the ordering law is testable without a radio.
enum class CalibPhase : uint8_t { kIdle, kUplink, kDownlink, kDone, kFailed };

struct SeqActions {
    bool start_downlink = false;  // issue §11.7 CALIBRATE arg=1
    bool abort_downlink = false;  // issue §11.7 CALIBRATE arg=0
};

class CalibSequencer {
  public:
    // downlink_start_timeout_ms bounds the wait for the craft's §3.15 word to
    // show `running`: a VCMD can be lost, and a sequencer that waits forever
    // for an acknowledgement that is never coming is indistinguishable from a
    // hung run.
    //
    // downlink_cap_ms bounds the phase AFTER the craft picks it up. Without it
    // a craft that dies mid-run — the §3.15 word simply stops arriving, which
    // is `-1`, deliberately not a terminal state — leaves `active()` latched
    // forever, and every future start (either direction) is refused by an
    // interlock guarding a sequence that ended long ago. Seeded above §10.6's
    // own 600 s hard cap so a legitimately slow craft run always wins the race.
    explicit CalibSequencer(uint32_t downlink_start_timeout_ms = 15000,
                            uint32_t downlink_cap_ms = 660000)
        : start_timeout_ms_(downlink_start_timeout_ms),
          cap_ms_(downlink_cap_ms) {}

    void start(uint64_t now_ms) {
        phase_ = CalibPhase::kUplink;
        fail_reason_ = nullptr;
        entered_ms_ = now_ms;
        saw_running_ = false;
    }

    // Cancel whichever phase is live. Returns the actuation the caller owes:
    // an uplink-phase abort is local (the caller aborts its calibrator), a
    // downlink-phase abort has to go over air.
    SeqActions abort(uint64_t now_ms) {
        SeqActions a;
        if (phase_ == CalibPhase::kDownlink) a.abort_downlink = true;
        if (phase_ == CalibPhase::kUplink || phase_ == CalibPhase::kDownlink) {
            finish(CalibPhase::kFailed, "abort", now_ms);
        }
        return a;
    }

    // uplink_state is the local calibrator's; craft_calib_state is the
    // mirrored §3.15 nibble (-1 = the craft is not airing one).
    SeqActions tick(CalibState uplink_state, int craft_calib_state,
                    uint64_t now_ms) {
        SeqActions a;
        switch (phase_) {
            case CalibPhase::kUplink:
                if (uplink_state == CalibState::kRunning) break;
                if (uplink_state == CalibState::kDone) {
                    // Only here. §10.7's order law is the entire reason the
                    // sequence exists: a downlink run measured across a badly
                    // placed uplink aborts on its own report clock or places
                    // against a thinning report stream.
                    phase_ = CalibPhase::kDownlink;
                    entered_ms_ = now_ms;
                    saw_running_ = false;
                    a.start_downlink = true;
                } else {
                    finish(CalibPhase::kFailed, "uplink_phase_failed", now_ms);
                }
                break;
            case CalibPhase::kDownlink:
                if (saw_running_ && now_ms - entered_ms_ > cap_ms_) {
                    // The craft stopped airing a terminal word. Cancel over
                    // air so it does not keep running against a sequence this
                    // node has given up on.
                    a.abort_downlink = true;
                    finish(CalibPhase::kFailed, "downlink_timeout", now_ms);
                    break;
                }
                if (craft_calib_state == 1) {
                    saw_running_ = true;
                    break;
                }
                if (!saw_running_) {
                    // The craft has not picked the command up yet. A stale
                    // done/failed from a PREVIOUS run is still being aired,
                    // so it must not be read as this phase's result.
                    if (now_ms - entered_ms_ > start_timeout_ms_) {
                        finish(CalibPhase::kFailed, "downlink_no_ack", now_ms);
                    }
                    break;
                }
                if (craft_calib_state == 2) {
                    finish(CalibPhase::kDone, nullptr, now_ms);
                } else if (craft_calib_state == 3) {
                    finish(CalibPhase::kFailed, "downlink_phase_failed",
                           now_ms);
                }
                // -1 (word gone) or 0 (idle) after running: keep waiting; the
                // §3.15 word is sticky, so a real terminal state will land.
                break;
            case CalibPhase::kIdle:
            case CalibPhase::kDone:
            case CalibPhase::kFailed:
                break;
        }
        return a;
    }

    CalibPhase phase() const { return phase_; }
    bool active() const {
        return phase_ == CalibPhase::kUplink || phase_ == CalibPhase::kDownlink;
    }
    const char* fail_reason() const { return fail_reason_; }
    static const char* phase_name(CalibPhase p) {
        switch (p) {
            case CalibPhase::kUplink: return "uplink";
            case CalibPhase::kDownlink: return "downlink";
            case CalibPhase::kDone: return "done";
            case CalibPhase::kFailed: return "failed";
            case CalibPhase::kIdle: break;
        }
        return "idle";
    }

  private:
    void finish(CalibPhase p, const char* reason, uint64_t now_ms) {
        phase_ = p;
        fail_reason_ = reason;
        entered_ms_ = now_ms;
    }

    uint32_t start_timeout_ms_;
    uint32_t cap_ms_;
    CalibPhase phase_ = CalibPhase::kIdle;
    const char* fail_reason_ = nullptr;
    uint64_t entered_ms_ = 0;
    bool saw_running_ = false;
};

}  // namespace wblink
