// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §10.6 craft-resident link calibration (Pass 120).
//
// A pure, time-injected state machine in the ms domain, shaped like the §11
// engines: inputs are accepted §3.5 report samples and ticks; outputs are
// polled actions (pin rung / set power / restore / artifact-ready). Nothing
// here touches a radio, a file, or a clock — app/ actuates through the §9.7
// pin and §10.5 set_power_qdb seams and persists the artifact (io/).
//
// Per rung (Pass 121 max-power seek): SEEK — ramp power upward in
// seek_step_qdb steps until the first wall: loss wall (report loss >
// loss_bad_milli), cap wall (a ≥2 dB commanded step moves rssi_mean by
// < cap_rise_db — delivered power stopped following commanded), the
// rssi_guard_dbm overload bound, or max_qdb. Placement = the last clean,
// responsive probe; the wall's bracketing RSSIs ARE the overload-ceiling
// record. Then VERIFY (longer dwell, record placement loss) → next rung,
// starting one step below this placement. A first probe already past the
// loss wall descends until clean (floor min_qdb).
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

struct CalibrateParams {
    uint16_t loss_ok_milli = 15;  // §15.2 policy.calibration seeds
    uint16_t loss_bad_milli = 50;
    int32_t seek_step_qdb = 16;  // 4 dB per seek probe (Pass 121)
    int cap_rise_db = 1;         // < this rise on a ≥2 dB step = cap wall
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
    explicit Calibrator(const CalibrateParams& p) : p_(p) {}

    // §11.7 CALIBRATE start/abort semantics: false = REJECTED (already
    // running / not running). The app layers the other REJECT conditions
    // (actuator + latched reporter) — core cannot see them.
    bool start(uint64_t now_ms) {
        if (state_ == CalibState::kRunning) return false;
        state_ = CalibState::kRunning;
        started_ms_ = now_ms;
        last_report_ms_ = now_ms;  // armed; the 3 s clock runs from start
        fail_reason_ = nullptr;
        artifact_ = {};
        rung_ = 0;
        qdb_ = p_.min_qdb;  // Pass 121: rung 0 ramps from the floor
        last_clean_.reset();
        verify_descents_ = 0;
        cap_confirm_ = false;
        enter_rung_ = true;
        phase_ = Phase::kSeek;
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
        // Dwell complete — evaluate with whatever samples arrived (the
        // report-loss guard bounds "no samples at all").
        const double rssi = rssi_n_ ? double(rssi_sum_) / rssi_n_ : -128.0;
        const uint16_t loss = loss_w_ ? static_cast<uint16_t>(
            std::min<uint64_t>(1000, loss_sum_ / loss_w_)) : 0;
        switch (phase_) {
            case Phase::kSeek: {
                auto& c = artifact_.ceilings[rung_];
                if (loss > p_.loss_bad_milli) {  // loss wall
                    if (!c.has_bad) {
                        c.has_bad = true;
                        c.first_bad_rssi =
                            static_cast<int8_t>(std::lround(rssi));
                    }
                    if (last_clean_) {
                        return place(a, now_ms, last_clean_->qdb);
                    }
                    if (qdb_ <= p_.min_qdb) {  // floor still bad: record it
                        return place(a, now_ms, p_.min_qdb);
                    }
                    qdb_ = std::max(p_.min_qdb, qdb_ - p_.seek_step_qdb);
                    a.set_qdb = qdb_;
                    begin_dwell(now_ms, p_.probe_dwell_ms);
                    return a;
                }
                c.last_clean_rssi = static_cast<int8_t>(std::lround(rssi));
                // Cap wall: an upward ≥2 dB commanded step that the report
                // RSSI did not follow — delivered power stopped tracking.
                // Addendum 3: confirm with one repeat dwell — in the TXAGC
                // compression knee a one-off deficit is dwell noise.
                if (last_clean_ && qdb_ - last_clean_->qdb >= 8 &&
                    rssi - last_clean_->rssi < p_.cap_rise_db) {
                    if (!cap_confirm_) {
                        cap_confirm_ = true;
                        begin_dwell(now_ms, p_.probe_dwell_ms);
                        return a;  // re-dwell at the same power
                    }
                    return place(a, now_ms, last_clean_->qdb);
                }
                cap_confirm_ = false;
                last_clean_ = Probe{qdb_, rssi};
                if (rssi > p_.rssi_guard_dbm || qdb_ >= p_.max_qdb) {
                    return place(a, now_ms, qdb_);  // guard / cap-clean
                }
                qdb_ = std::min(p_.max_qdb, qdb_ + p_.seek_step_qdb);
                a.set_qdb = qdb_;
                begin_dwell(now_ms, p_.probe_dwell_ms);
                return a;
            }
            case Phase::kVerify: {
                // Addendum 2: near-cliff instability can pass short probe
                // dwells and fail only under sustained exposure — enforce
                // loss_ok here with a bounded step-down.
                if (loss > p_.loss_ok_milli && verify_descents_ < 3 &&
                    qdb_ > p_.min_qdb) {
                    auto& c = artifact_.ceilings[rung_];
                    if (loss > p_.loss_bad_milli && !c.has_bad) {
                        c.has_bad = true;
                        c.first_bad_rssi =
                            static_cast<int8_t>(std::lround(rssi));
                    }
                    ++verify_descents_;
                    qdb_ = std::max(p_.min_qdb, qdb_ - p_.seek_step_qdb);
                    a.set_qdb = qdb_;
                    begin_dwell(now_ms, p_.verify_dwell_ms);
                    return a;
                }
                artifact_.placement_qdb[rung_] = qdb_;
                artifact_.placement_rssi[rung_] =
                    static_cast<int8_t>(std::lround(rssi));
                artifact_.placement_loss_milli[rung_] = loss;
                placement_qdb_ = qdb_;
                return next_rung(a, now_ms);
            }
        }
        return a;
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
    enum class Phase : uint8_t { kSeek, kVerify };
    struct Probe { int32_t qdb; double rssi; };

    void begin_dwell(uint64_t now_ms, uint32_t dwell_ms) {
        dwell_start_ms_ = now_ms + p_.settle_ms;
        dwell_end_ms_ = dwell_start_ms_ + dwell_ms;
        rssi_sum_ = 0; rssi_n_ = 0; loss_sum_ = 0; loss_w_ = 0;
    }

    // Seek hit a wall (or the cap/guard): drop to the placement power and
    // enter the VERIFY dwell there.
    CalibActions place(CalibActions a, uint64_t now_ms, int32_t qdb) {
        qdb_ = qdb;
        a.set_qdb = qdb_;
        phase_ = Phase::kVerify;
        begin_dwell(now_ms, p_.verify_dwell_ms);
        return a;
    }

    CalibActions next_rung(CalibActions a, uint64_t now_ms) {
        if (rung_ >= 7) {
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
        // Neighbor rungs' walls sit near each other: ramp from one step
        // below the previous placement, not from the floor.
        qdb_ = std::max(p_.min_qdb, placement_qdb_ - p_.seek_step_qdb);
        last_clean_.reset();
        verify_descents_ = 0;
        cap_confirm_ = false;
        phase_ = Phase::kSeek;
        a.pin_rung = rung_;
        a.set_qdb = qdb_;
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
    uint8_t verify_descents_ = 0;
    int32_t qdb_ = 0;
    int32_t placement_qdb_ = 0;
    std::optional<Probe> last_clean_;
    bool enter_rung_ = true;
    bool cap_confirm_ = false;
    bool restore_pending_ = false;
    bool artifact_pending_ = false;
    Phase phase_ = Phase::kSeek;
};

}  // namespace wblink
