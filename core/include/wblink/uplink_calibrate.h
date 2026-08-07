// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §10.7 ground-uplink calibration orchestrator.
//
// The counterpart to §10.6's Calibrator: same PowerSeek ramp, same evidence
// primitive since Pass 153 — the shared §3.16 dwell exchange (N MTU-padded
// PROBEs out, one TALLY back, self-denominating loss). Single rung since
// Pass 153: only the configured air.uplink_rate rung is calibrated (Pass
// 131's eight-rung widening served a hypothetical shadow-the-downlink
// policy that never existed).
//
// The Pass 125/126/128/132 counting apparatus — epoch anchoring, blackout
// fallback, drain windows, the §3.16 cumulative counters and their
// dual-clock gate — is gone whole: the dwell_id is the synchronisation.
//
// Pure and time-injected: it consumes decoded TALLYs and ticks, and emits
// polled actions plus a drained probe stream. It never touches a radio or a
// file.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "wblink/calib_dwell.h"      // DwellSender (§3.16 Pass 153)
#include "wblink/calibrate.h"        // PowerSeek, SeekParams, CalibState

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
    // §10.3 (Pass 134): the §9.3 table's tx_power_level for THE rung —
    // single-rung since Pass 153 — tapering seek.max_qdb so the ground cannot
    // walk its own uplink PA to full power on a high-order rung. The ground
    // half carries the stronger case for the mask than §10.6 does: this
    // placement auto-applies at boot with no operator between the measurement
    // and the actuator.
    uint8_t rung_level = 4;
    // §10.6/§10.7 (Pass 151): same meaning as CalibrateParams — false in
    // offset space, where the efuse per-rate table the offset is measured
    // against already carries this backoff.
    bool taper_rung_ceiling = true;
    uint32_t settle_ms = 300;  // shared with §10.6: TXAGC settle
    // §3.16 (Pass 153) dwell burst sizes in PROBE FRAMES, shared with §10.6.
    // Pass 132's decidability rule holds: 1000/N <= loss_ok_milli.
    uint16_t dwell_probe_frames = 500;
    uint16_t dwell_verify_frames = 1000;
    DwellSendParams dwell;  // pacing + tally re-elicitation bounds
    uint32_t hard_cap_ms = 600000;
    // The configured air.uplink_rate rung — the ONLY rung §10.7 calibrates
    // since Pass 153 (reverts Pass 131's widening; the artifact schema stays
    // list-shaped so scope can widen again by appending entries).
    UplinkRate rate{};
};

// One rung's result. `placements` in the §10.7 artifact is a list of these —
// one since Pass 153 (the air.uplink_rate rung); a loader accepts any length.
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
// single-shot signal lives here so no exit path can skip it. Probe emission is
// NOT an action: the app drains next_probe() like §10.6's Calibrator, and the
// craft's per-dwell TALLY (§3.16) is fed back through on_tally().
struct UplinkCalibActions {
    std::optional<UplinkRate> set_rate;
    std::optional<int32_t> set_qdb;
    bool restore = false;
    bool artifact_ready = false;
};

class UplinkCalibrator {
  public:
    explicit UplinkCalibrator(const UplinkCalibParams& p)
        : p_(p), dwell_(p.dwell), seek_(p.seek) {}

    // §10.3/§11.7 0x0A (Pass 135): move the sweep's flat ceiling — the value
    // rung_ceiling_qdb() tapers — after construction, so a runtime power tier
    // bounds the NEXT run. Refused while running: the seek is mid-descent
    // against the old bound and re-basing it would score a dwell at one
    // ceiling against another.
    bool set_max_qdb(int32_t qdb) {
        if (state_ == CalibState::kRunning) return false;
        p_.seek.max_qdb = qdb;
        return true;
    }

    bool start(uint64_t now_ms) {
        if (state_ == CalibState::kRunning) return false;
        state_ = CalibState::kRunning;
        started_ms_ = now_ms;
        fail_reason_ = nullptr;
        restore_pending_ = false;
        artifact_pending_ = false;
        placements_.clear();
        // §3.16: a fresh run_id opens a new run at the craft's receiver (and
        // pauses its feed, D-C). Injected clock, never wall time; non-zero.
        run_id_ = static_cast<uint32_t>(now_ms) | 1u;
        dwell_seq_ = 0;
        // Commit the rung once (it IS the operating rung — an assert, not a
        // move) and start the sweep from the floor.
        pending_rate_ = p_.rate;
        const SeekStep s = seek_.begin(rung_ceiling_qdb());
        qdb_ = s.qdb;
        pending_qdb_ = s.qdb;
        begin_dwell(now_ms, p_.dwell_probe_frames);
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
    // `done` there is the same false success C2 removed, one layer out. The
    // caller invokes this when the store write fails; the restore edge
    // re-arms so the actuator returns to the owner that was there before.
    void fail_persist() {
        if (state_ != CalibState::kDone) return;
        finish(CalibState::kFailed, "artifact_write_failed");
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
    // new_tick(); the app encodes (run_id, dwell_id, seq, count) and injects
    // through the uplink adapter, padded to the negotiated budget. Gated on
    // the settle window so TXAGC has settled before the first probe.
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

    UplinkCalibActions tick(uint64_t now_ms) {
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
        if (dwell_.state() == DwellState::kNoEvidence) {
            // §3.16 evidence blackout — no tally despite bounded
            // re-elicitation. At low power this is EXPECTED (no probe reached
            // the craft, so it has no dwell to answer for): it reads as a
            // 1000‰ not-clean dwell and the sweep climbs past it. Terminal
            // only with nothing clean anywhere and nowhere left to climb.
            const double obs =
                static_cast<double>(p_.seek.rssi_guard_dbm) - 60.0;
            return score(a, now_ms, 1000, obs, dwell_count_, 0,
                         /*blackout=*/true);
        }
        if (dwell_.state() != DwellState::kDone) {
            return a;  // settling, burst in flight, or tally awaited
        }
        const DwellResult res = dwell_.result();
        dwell_.reset();
        const uint16_t loss = res.loss_milli();
        const double rssi =
            res.received > 0
                ? static_cast<double>(static_cast<int32_t>(res.rssi_sum_dbm)) /
                      res.received
                : static_cast<double>(p_.seek.rssi_guard_dbm) - 60.0;
        return score(a, now_ms, loss, rssi, res.sent, res.received);
    }

    CalibState state() const { return state_; }
    const char* fail_reason() const { return fail_reason_; }
    // The completed placement list. One entry since Pass 153; only complete
    // when state() is kDone — §10.7 persists nothing on a failed run.
    const std::vector<UplinkPlacement>& placements() const {
        return placements_;
    }
    // Kept for the §15.3 uplink_calib_rung shape: always 0 in single-rung v2.
    uint8_t rung() const { return 0; }
    const UplinkRate& rate() const { return p_.rate; }
    int32_t qdb() const { return qdb_; }

    // What the last completed dwell actually observed — the campaign's
    // readable record. `seq` increments per completed dwell so a caller can
    // log edges without polling state.
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
    // §10.3 mask, same derivation as §10.6's rung_max_qdb, for the one rung.
    int32_t rung_ceiling_qdb() const {
        if (!p_.taper_rung_ceiling) return p_.seek.max_qdb;
        const int32_t lvl = static_cast<int32_t>(p_.rung_level);
        return std::clamp(
            p_.seek.max_qdb + (lvl - kPowerLevelBaseline) * kQdbPerLevel,
            p_.seek.min_qdb, p_.seek.max_qdb);
    }

    void begin_dwell(uint64_t now_ms, uint16_t frames) {
        dwell_start_ms_ = now_ms + p_.settle_ms;
        dwell_count_ = frames;
        ++dwell_seq_;  // §3.16: strictly increasing within the run
        (void)dwell_.begin(run_id_, dwell_seq_, frames, now_ms);
    }

    // Score a completed (or evidence-blacked-out) dwell through the seek.
    UplinkCalibActions score(UplinkCalibActions a, uint64_t now_ms,
                             uint16_t loss, double rssi, uint32_t sent,
                             uint32_t received, bool blackout = false) {
        const bool verify = seek_.in_verify();
        const DwellVerdict v = loss > p_.seek.loss_bad_milli
                                   ? DwellVerdict::kBad
                                   : DwellVerdict::kClean;
        last_ = DwellRecord{last_.seq + 1,
                            0,
                            qdb_,
                            verify,
                            sent,
                            received,
                            loss,
                            static_cast<int32_t>(std::lround(rssi)),
                            dwell_count_};
        const SeekStep s = seek_.on_dwell(v, rssi, loss);
        qdb_ = s.qdb;
        if (s.power_changed) a.set_qdb = s.qdb;
        switch (s.kind) {
            case SeekStep::Kind::kFailed:
                // Single rung: an unreachable rung IS a failed run (the
                // multi-rung "cap the sweep" rule has nothing left to cap).
                // A terminal step under an evidence blackout is the
                // evidence-loss abort (§3.16), not the seek's own reason.
                finish(CalibState::kFailed,
                       blackout ? "evidence_lost" : s.fail_reason);
                a.restore = take_restore_();
                return a;
            case SeekStep::Kind::kDone:
                // §10.7 does NOT inherit §10.6's "record the still-failing
                // floor" rule: this artifact AUTO-APPLIES at boot and gates
                // the sequencer's downlink phase, so success with a placement
                // that just measured unusable would defeat the order law
                // through a state no interlock inspects. Judge the PLACED
                // point (Pass 151), not the last dwell.
                if (seek_.placed_loss_milli() > p_.seek.loss_ok_milli) {
                    finish(CalibState::kFailed, "verify_failed");
                    a.restore = take_restore_();
                    return a;
                }
                {
                    UplinkPlacement pl;
                    pl.mcs = p_.rate.mcs;
                    pl.short_gi = p_.rate.short_gi;
                    pl.placement_qdb = s.qdb;
                    pl.placement_rssi_dbm =
                        static_cast<int8_t>(std::lround(seek_.placed_rssi()));
                    pl.placement_loss_milli = seek_.placed_loss_milli();
                    // Bracket from the seek — only it applies the "bad ABOVE
                    // a clean probe" rule (a duplicate here once booked the
                    // cold floor as the overload ceiling).
                    pl.last_clean_qdb = seek_.last_clean_qdb();
                    pl.has_first_bad = seek_.has_bad();
                    pl.first_bad_qdb = seek_.first_bad_qdb();
                    placements_.push_back(pl);
                }
                // §10.7 (Pass 134 addendum, single-rung form): the swept rung
                // placing at its §10.3 ceiling with no bracket booked is the
                // close-range no-wall geometry — the run fails and persists
                // nothing, leaving the last-good artifact in place. Device
                // evidence forced this: a bench-range run with fully live
                // feedback placed everything at the ceiling and persisted the
                // mask read back as a measurement.
                if (!placements_.back().has_first_bad &&
                    placements_.back().placement_qdb >= rung_ceiling_qdb()) {
                    finish(CalibState::kFailed, "no_wall_found");
                    a.restore = take_restore_();
                    return a;
                }
                finish(CalibState::kDone, nullptr);
                a.restore = take_restore_();
                a.artifact_ready = take_artifact_();
                return a;
            case SeekStep::Kind::kVerify:
                begin_dwell(now_ms, p_.dwell_verify_frames);
                return a;
            case SeekStep::Kind::kProbe:
                begin_dwell(now_ms, p_.dwell_probe_frames);
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

    UplinkCalibParams p_;
    DwellSender dwell_;
    PowerSeek seek_;
    CalibState state_ = CalibState::kIdle;
    const char* fail_reason_ = nullptr;
    std::vector<UplinkPlacement> placements_;
    uint64_t started_ms_ = 0;
    uint64_t dwell_start_ms_ = 0;
    uint32_t run_id_ = 0;
    uint16_t dwell_seq_ = 0;
    uint16_t dwell_count_ = 0;
    int32_t qdb_ = 0;
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
