// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §10.7 (Pass 125) ground-uplink calibration orchestrator.
//
// The counterpart to §10.6's Calibrator: same PowerSeek ramp, different
// gating. The craft measures against live video (thousands of loss samples a
// second, so dwells are wall-clock), while the ground measures against sparse
// LINK_REPORT epochs — so every dwell here is gated on an EPOCH COUNT. A slow
// report cadence must lengthen the run, never let an unobserved dwell score
// as clean.
//
// Pure and time-injected: it consumes §3.16 QualitySamples and ticks, and
// emits polled actions. It does not own the UplinkQualityGate (that needs the
// app's notion of the selected craft) and never touches a radio or a file.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

#include "wblink/calibrate.h"        // PowerSeek, SeekParams, CalibState
#include "wblink/uplink_quality.h"   // QualitySample

namespace wblink {

struct UplinkCalibParams {
    SeekParams seek;
    uint32_t settle_ms = 800;       // shared with §10.6: TXAGC settle
    uint32_t probe_epochs = 40;     // §10.7 seeds
    uint32_t ambiguous_epochs = 80;
    uint32_t verify_epochs = 200;
    uint32_t liveness_ms = 2000;
    uint32_t hard_cap_ms = 600000;
};

// One rung's result. `placements` in the §10.7 artifact is a list of these —
// v1 emits exactly one, and a future multi-rung uplink appends rather than
// bumping the schema.
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

// Edge-triggered, polled once per tick. The app actuates set_qdb, and on
// `restore` returns the actuator to its §10.7 owner (config map, then a
// matching artifact, then backend auto) — that ordering is the app's, but the
// single-shot signal is here so no exit path can skip it.
struct UplinkCalibActions {
    std::optional<int32_t> set_qdb;
    bool restore = false;
    bool artifact_ready = false;
};

class UplinkCalibrator {
  public:
    UplinkCalibrator(const UplinkCalibParams& p, uint8_t mcs, bool short_gi)
        : p_(p), seek_(p.seek), mcs_(mcs), sgi_(short_gi) {}

    bool start(uint64_t now_ms, uint32_t local_epoch) {
        if (state_ == CalibState::kRunning) return false;
        state_ = CalibState::kRunning;
        started_ms_ = now_ms;
        fail_reason_ = nullptr;
        restore_pending_ = false;
        artifact_pending_ = false;
        placement_ = {};
        placement_.mcs = mcs_;
        placement_.short_gi = sgi_;
        last_clean_qdb_ = p_.seek.min_qdb;
        have_first_bad_ = false;
        first_bad_qdb_ = 0;  // stale value would ride into the next placement
        // Sweep from the floor. At range the bottom steps are normally dead;
        // they are simply not clean and the sweep climbs past them.
        const SeekStep s = seek_.begin();
        qdb_ = s.qdb;
        pending_qdb_ = s.qdb;
        begin_dwell(now_ms, local_epoch, p_.probe_epochs);
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

    // One accepted §3.16 packet. Deltas telescope across the dwell, so
    // summing them from the dwell's first accepted packet to its last IS
    // (E_B - E_A) / (R_B - R_A) / (S_B - S_A) — the §10.7 anchors, with no
    // ground-side epoch bookkeeping. Samples inside the settle window are
    // discarded exactly as §10.6 discards its report samples.
    void on_sample(const QualitySample& s, uint64_t now_ms) {
        if (state_ != CalibState::kRunning) return;
        // The craft restarted its counter domain mid-dwell. Everything
        // accumulated so far belongs to the old domain, and the §10.7 loss
        // identity is a delta between two anchors of the SAME domain — mixing
        // them would score a placement against a number that does not mean
        // what it says. Restart the dwell at this power rather than carry it.
        if (s.resynced) {
            restart_dwell(now_ms);
            return;
        }
        if (!s.progressed) return;
        if (now_ms < dwell_start_ms_) return;
        emitted_ += s.epoch_delta;
        received_ += s.reports_delta;
        rssi_sum_ += s.rssi_sum_delta;
    }

    // local_epoch is the ground's own Reporter::epoch(). It is needed for one
    // case only: a counter blackout, where the craft's anchors cannot advance
    // because nothing is arriving, so the dwell would otherwise never end.
    // quality_live is the §3.16 LIVENESS clock — packet arrival, never
    // counter progress. Losing it means the run has no observer; stalled
    // counters under live feedback are evidence, not a timeout.
    UplinkCalibActions tick(uint64_t now_ms, uint32_t local_epoch,
                            bool quality_live) {
        UplinkCalibActions a;
        if (state_ != CalibState::kRunning) {
            a.restore = take_restore_();
            a.artifact_ready = take_artifact_();
            return a;
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
        // Anchor the ground's own epoch clock at the END of settle, not at
        // begin_dwell: the craft's `emitted_` starts accumulating here, so an
        // anchor taken before settle runs ahead by settle_ms worth of epochs
        // and would trip the fallback below on a perfectly healthy dwell.
        if (!dwell_epoch_armed_) {
            dwell_epoch_armed_ = true;
            dwell_local_epoch_ = local_epoch;
        }

        if (emitted_ >= target_epochs_) {
            return evaluate(a, now_ms, local_epoch, false);
        }
        // Counter blackout: live feedback, the craft's anchors did not span
        // the dwell. Fall back to the ground's own emission count and score
        // 1000permille — §10.7. The condition is that `emitted_` fell short,
        // NOT that `received_` is zero: a PARTIAL blackout (some epochs land,
        // then the uplink dies) leaves received_ > 0 with the anchors frozen
        // short of target, and keying on received_ == 0 wedged that dwell
        // until the 600 s hard cap — precisely the floor case §10.7 exists
        // for. Reaching here already implies emitted_ < target_epochs_.
        if (local_epoch - dwell_local_epoch_ >= target_epochs_) {
            return evaluate(a, now_ms, local_epoch, true);
        }
        return a;
    }

    CalibState state() const { return state_; }
    const char* fail_reason() const { return fail_reason_; }
    const UplinkPlacement& placement() const { return placement_; }
    int32_t qdb() const { return qdb_; }
    uint32_t dwell_progress() const { return emitted_; }
    uint32_t dwell_target() const { return target_epochs_; }

    // What the last completed dwell actually observed. §10.7's per-run record
    // (duration, samples/dwell, loss, RSSI, bracket) is the campaign's
    // deliverable, and a run that fails is exactly the one whose dwells have
    // to be readable — "verify_failed" with no numbers behind it is not a
    // finding, it is a rumour. `seq` increments per completed dwell so a
    // caller can log edges without polling state.
    struct DwellRecord {
        uint32_t seq = 0;
        int32_t qdb = 0;
        bool verify = false;
        bool blackout = false;
        uint32_t emitted = 0;
        uint32_t received = 0;
        uint16_t loss_milli = 0;
        int32_t rssi_mean = 0;
        uint32_t target = 0;
    };
    const DwellRecord& last_dwell() const { return last_; }

  private:
    // Re-arm the current dwell at the current power, keeping its target. The
    // ground epoch anchor re-arms with it (the first post-settle tick), so
    // both clocks restart together.
    void restart_dwell(uint64_t now_ms) {
        dwell_start_ms_ = now_ms + p_.settle_ms;
        dwell_epoch_armed_ = false;
        emitted_ = 0;
        received_ = 0;
        rssi_sum_ = 0;
    }

    void begin_dwell(uint64_t now_ms, uint32_t local_epoch, uint32_t target) {
        dwell_start_ms_ = now_ms + p_.settle_ms;
        dwell_local_epoch_ = local_epoch;
        dwell_epoch_armed_ = false;
        target_epochs_ = target;
        emitted_ = 0;
        received_ = 0;
        rssi_sum_ = 0;
        extended_ = false;
    }

    UplinkCalibActions evaluate(UplinkCalibActions a, uint64_t now_ms,
                                uint32_t local_epoch, bool blackout) {
        const bool verify = seek_.in_verify();
        uint16_t loss = 1000;
        double rssi = static_cast<double>(p_.seek.rssi_guard_dbm) - 60.0;
        // The denominator is the craft's own anchor span when it spans the
        // dwell, and the ground's local emission count when it does not.
        //
        // A flat 1000permille for the second case is WRONG, and cost a run on
        // the bench: the craft's `last_report_epoch` only advances for reports
        // it ACCEPTED, so on a link with any loss at all it lags the ground's
        // local clock by exactly the lost count. The local clock therefore
        // reaches the target first and a perfectly healthy dwell — measured
        // received=198 of emitted=199, ~5permille — scored 1000permille and
        // failed its verify. Scoring the local span against `received_`
        // degrades correctly at both ends: a total blackout still has
        // received_==0 and still scores 1000permille (the original §10.7
        // rule, preserved), while a one-epoch lag scores the real loss.
        const uint32_t span = blackout ? local_epoch - dwell_local_epoch_
                                       : emitted_;
        if (span > 0 && received_ > 0) {
            const uint32_t lost = span > received_ ? span - received_ : 0;
            loss = static_cast<uint16_t>(
                std::min<uint64_t>(1000, uint64_t{lost} * 1000 / span));
            rssi = static_cast<double>(rssi_sum_) /
                   static_cast<double>(received_);
        }
        // §10.7 ambiguous extension: between the walls, once, and only on a
        // probe. A LONGER dwell is the only thing that resolves it, which is
        // why config rejects ambiguous_epochs <= probe_epochs.
        if (!verify && !extended_ && !blackout &&
            loss > p_.seek.loss_ok_milli && loss <= p_.seek.loss_bad_milli &&
            p_.ambiguous_epochs > target_epochs_) {
            extended_ = true;
            target_epochs_ = p_.ambiguous_epochs;
            return a;  // keep accumulating into the same dwell
        }
        const DwellVerdict v = loss > p_.seek.loss_bad_milli
                                   ? DwellVerdict::kBad
                                   : DwellVerdict::kClean;
        last_ = DwellRecord{last_.seq + 1,   qdb_,  verify, blackout,
                            emitted_,        received_,
                            loss,            static_cast<int32_t>(std::lround(rssi)),
                            target_epochs_};
        if (v == DwellVerdict::kClean) {
            last_clean_qdb_ = qdb_;
        } else if (!have_first_bad_) {
            have_first_bad_ = true;
            first_bad_qdb_ = qdb_;
        }
        const SeekStep s = seek_.on_dwell(v, rssi, loss);
        qdb_ = s.qdb;
        if (s.power_changed) a.set_qdb = s.qdb;
        switch (s.kind) {
            case SeekStep::Kind::kFailed:
                finish(CalibState::kFailed, s.fail_reason);
                a.restore = take_restore_();
                return a;
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
                if (loss > p_.seek.loss_ok_milli) {
                    finish(CalibState::kFailed, "verify_failed");
                    a.restore = take_restore_();
                    return a;
                }
                placement_.placement_qdb = s.qdb;
                placement_.placement_rssi_dbm =
                    static_cast<int8_t>(std::lround(rssi));
                placement_.placement_loss_milli = loss;
                placement_.last_clean_qdb = last_clean_qdb_;
                placement_.has_first_bad = have_first_bad_;
                placement_.first_bad_qdb = first_bad_qdb_;
                finish(CalibState::kDone, nullptr);
                a.restore = take_restore_();
                a.artifact_ready = take_artifact_();
                return a;
            case SeekStep::Kind::kVerify:
                begin_dwell(now_ms, local_epoch, p_.verify_epochs);
                return a;
            case SeekStep::Kind::kProbe:
                begin_dwell(now_ms, local_epoch, p_.probe_epochs);
                return a;
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
    uint8_t mcs_ = 0;
    bool sgi_ = false;
    CalibState state_ = CalibState::kIdle;
    const char* fail_reason_ = nullptr;
    UplinkPlacement placement_{};
    uint64_t started_ms_ = 0;
    uint64_t dwell_start_ms_ = 0;
    uint32_t dwell_local_epoch_ = 0;
    uint32_t target_epochs_ = 0;
    uint32_t emitted_ = 0;
    uint32_t received_ = 0;
    int64_t rssi_sum_ = 0;
    int32_t qdb_ = 0;
    int32_t last_clean_qdb_ = 0;
    int32_t first_bad_qdb_ = 0;
    bool have_first_bad_ = false;
    bool extended_ = false;
    bool dwell_epoch_armed_ = false;
    bool restore_pending_ = false;
    bool artifact_pending_ = false;
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
