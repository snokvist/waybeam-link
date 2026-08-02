// SPDX-License-Identifier: GPL-2.0-or-later
// §10.7 (Pass 125) ground-uplink calibrator tests. A synthetic uplink drives
// the epoch-gated loop end to end: the clean ramp, the floor start that every
// real run begins in, the ambiguous extension, liveness abort, and the
// restore-on-every-exit law.
//
// The gating is what differs from §10.6 and so is what is pinned here: dwells
// advance on REPORT EPOCHS, not wall time, and a stalled counter under live
// feedback is a 1000permille observation rather than a timeout.
#include "wblink/uplink_calibrate.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int g_fail = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #x);                                            \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

using namespace wblink;

// The uplink as the craft sees it: the ground's commanded power maps to a
// delivered RSSI, and reports are lost below a floor or above an overload
// ceiling. Unlike §10.6's model this one HAS a floor — that is the normal
// state at the bottom of every ramp.
struct Uplink {
    int32_t qdb = 0;
    int8_t floor_rssi = -128;  // below this, nothing is delivered
    int8_t ceil_rssi = 127;    // at/above this, overload
    int32_t cap_qdb = 100000;  // silently latched RF cap
    int8_t rssi() const {
        return static_cast<int8_t>(-41 + 0.85 * (std::min(qdb, cap_qdb) / 4.0));
    }
    // Delivered fraction in permille of what the ground emitted.
    uint32_t delivered(uint32_t emitted) const {
        const int8_t r = rssi();
        if (r < floor_rssi || r >= ceil_rssi) return 0;
        return emitted;  // clean: every epoch lands
    }
};

// Drive: the ground emits reports at 10 Hz; the craft's cumulative counters
// advance by whatever was delivered; §3.16 packets arrive at 2 Hz.
struct Rig {
    UplinkCalibrator cal;
    Uplink up;
    uint64_t now = 1000;
    uint32_t local_epoch = 0;   // ground Reporter::epoch()
    uint32_t craft_reports = 0; // craft cumulative reports_received
    uint32_t craft_epoch = 0;   // craft cumulative last_report_epoch
    int32_t craft_rssi_sum = 0;
    int restores = 0;
    int artifacts = 0;
    bool quality_live = true;

    explicit Rig(const UplinkCalibParams& p) : cal(p, 0, false) {}

    // One 100 ms step: emit a report, deliver it (or not), and every 5th step
    // hand the calibrator the §3.16 delta.
    void step() {
        now += 100;
        ++local_epoch;
        const uint32_t got = up.delivered(1);
        craft_reports += got;
        if (got != 0) {
            craft_epoch = local_epoch;
            craft_rssi_sum += up.rssi();
        }
        if (local_epoch % 5 == 0) {  // 2 Hz feedback
            QualitySample s;
            s.accepted = true;
            if (craft_reports != last_reports_) {
                s.progressed = true;
                s.reports_delta = craft_reports - last_reports_;
                s.epoch_delta = craft_epoch - last_epoch_;
                s.rssi_sum_delta = craft_rssi_sum - last_rssi_;
                last_reports_ = craft_reports;
                last_epoch_ = craft_epoch;
                last_rssi_ = craft_rssi_sum;
            }
            cal.on_sample(s, now);
        }
        const UplinkCalibActions a = cal.tick(now, local_epoch, quality_live);
        if (a.set_qdb) up.qdb = *a.set_qdb;
        if (a.restore) ++restores;
        if (a.artifact_ready) ++artifacts;
    }

    void run(uint64_t until_ms) {
        while (now < until_ms && cal.state() == CalibState::kRunning) step();
        // Drain the single-shot restore/artifact edge after the terminal step.
        const UplinkCalibActions a = cal.tick(now, local_epoch, quality_live);
        if (a.restore) ++restores;
        if (a.artifact_ready) ++artifacts;
    }

    uint32_t last_reports_ = 0;
    uint32_t last_epoch_ = 0;
    int32_t last_rssi_ = 0;
};

UplinkCalibParams fast_params() {
    UplinkCalibParams p;
    p.settle_ms = 200;
    p.probe_epochs = 5;
    p.ambiguous_epochs = 10;
    p.verify_epochs = 20;
    p.liveness_ms = 2000;
    return p;
}

// A clean uplink ramps to max and verifies there.
void test_clean_ramp() {
    Rig r(fast_params());
    CHECK(r.cal.start(r.now, r.local_epoch));
    r.run(r.now + 600000);
    CHECK(r.cal.state() == CalibState::kDone);
    CHECK(r.restores == 1);   // §10.7: every exit restores, exactly once
    CHECK(r.artifacts == 1);
    const UplinkPlacement& pl = r.cal.placement();
    CHECK(pl.placement_qdb == UplinkCalibParams{}.seek.max_qdb);
    CHECK(pl.placement_loss_milli <= 15);
    CHECK(pl.mcs == 0);
    CHECK(!pl.short_gi);
}

// THE case Pass 125 exists for: the ramp starts at min_qdb, which on a real
// uplink at range delivers nothing. The seek must ascend through the dead
// floor to a working placement — not place at the floor, not abort.
void test_floor_start() {
    Rig r(fast_params());
    r.up.floor_rssi = -30;  // qdb 4/20/36 are dead; 52 delivers
    CHECK(r.cal.start(r.now, r.local_epoch));
    r.run(r.now + 600000);
    CHECK(r.cal.state() == CalibState::kDone);
    const UplinkPlacement& pl = r.cal.placement();
    CHECK(pl.placement_qdb >= 52);
    CHECK(pl.placement_qdb > UplinkCalibParams{}.seek.min_qdb);
    CHECK(pl.placement_loss_milli <= 15);
    CHECK(r.restores == 1);
}

// A counter blackout is an OBSERVATION, not a timeout: feedback keeps
// arriving (liveness holds) while the counters sit still, and the dwell must
// still end — on the ground's own emission count — and score 1000permille.
void test_counter_blackout_is_evidence() {
    UplinkCalibParams p = fast_params();
    Rig r(p);
    r.up.floor_rssi = -30;
    CHECK(r.cal.start(r.now, r.local_epoch));
    // The first dwell delivers nothing at all. Run just past the probe gate
    // plus settle and confirm the run is alive and has left the floor.
    const uint64_t deadline = r.now + p.settle_ms + 100 * (p.probe_epochs + 4);
    while (r.now < deadline && r.cal.state() == CalibState::kRunning) r.step();
    CHECK(r.cal.state() == CalibState::kRunning);  // never aborted
    CHECK(r.up.qdb > UplinkCalibParams{}.seek.min_qdb);  // ascended
    CHECK(r.restores == 0);
}

// An uplink that never delivers at any power fails with a reason that says
// so, and still restores.
void test_no_clean_point() {
    Rig r(fast_params());
    r.up.floor_rssi = 0;  // unreachable: max_qdb only reaches -18
    CHECK(r.cal.start(r.now, r.local_epoch));
    r.run(r.now + 600000);
    CHECK(r.cal.state() == CalibState::kFailed);
    CHECK(r.cal.fail_reason() != nullptr);
    if (r.cal.fail_reason() != nullptr) {
        CHECK(std::string(r.cal.fail_reason()) == "no_clean_point");
    }
    CHECK(r.restores == 1);
    CHECK(r.artifacts == 0);
}

// Liveness loss — the craft stopped talking — aborts and restores. This is
// the ONLY quality condition that ends a run.
void test_liveness_abort() {
    Rig r(fast_params());
    CHECK(r.cal.start(r.now, r.local_epoch));
    for (int i = 0; i < 10; ++i) r.step();
    CHECK(r.cal.state() == CalibState::kRunning);
    r.quality_live = false;
    r.step();
    CHECK(r.cal.state() == CalibState::kFailed);
    CHECK(r.cal.fail_reason() != nullptr);
    if (r.cal.fail_reason() != nullptr) {
        CHECK(std::string(r.cal.fail_reason()) == "quality_lost");
    }
    CHECK(r.restores == 1);
}

// Abort is idempotent and restores exactly once.
void test_abort() {
    Rig r(fast_params());
    CHECK(!r.cal.abort(r.now));  // not running
    CHECK(r.cal.start(r.now, r.local_epoch));
    CHECK(!r.cal.start(r.now, r.local_epoch));  // already running
    for (int i = 0; i < 5; ++i) r.step();
    CHECK(r.cal.abort(r.now));
    CHECK(!r.cal.abort(r.now));  // idempotent
    const UplinkCalibActions a = r.cal.tick(r.now, r.local_epoch, true);
    CHECK(a.restore);
    CHECK(!a.artifact_ready);
    CHECK(r.cal.state() == CalibState::kFailed);
    const UplinkCalibActions b = r.cal.tick(r.now, r.local_epoch, true);
    CHECK(!b.restore);  // single-shot
}

// The dwell gate counts EPOCHS, not milliseconds: halving the report cadence
// must lengthen the run, never let a half-observed dwell decide.
void test_gate_is_epochs_not_time() {
    UplinkCalibParams p = fast_params();
    UplinkCalibrator cal(p, 0, false);
    CHECK(cal.start(1000, 0));
    CHECK(cal.dwell_target() == p.probe_epochs);
    // A long time with no samples decides nothing.
    for (uint64_t t = 1200; t < 60000; t += 100) {
        const UplinkCalibActions a = cal.tick(t, 0, true);
        CHECK(!a.restore);
    }
    CHECK(cal.state() == CalibState::kRunning);
    CHECK(cal.dwell_progress() == 0);
}

// Between the walls, once, and only on a probe: the ambiguous extension
// lengthens the same dwell rather than deciding on a reading it cannot trust.
void test_ambiguous_extension() {
    UplinkCalibParams p = fast_params();
    p.seek.loss_ok_milli = 15;
    p.seek.loss_bad_milli = 50;
    UplinkCalibrator cal(p, 0, false);
    CHECK(cal.start(1000, 0));
    uint64_t t = 1000 + p.settle_ms + 1;
    // Deliver 4 of 5 epochs: 200permille is past the bad wall, so NOT
    // ambiguous — that one decides immediately.
    QualitySample s;
    s.accepted = true;
    s.progressed = true;
    s.epoch_delta = p.probe_epochs;
    s.reports_delta = p.probe_epochs - 1;
    s.rssi_sum_delta = -40 * static_cast<int32_t>(p.probe_epochs - 1);
    cal.on_sample(s, t);
    (void)cal.tick(t, p.probe_epochs, true);
    CHECK(cal.dwell_target() == p.probe_epochs);  // decided, new probe dwell

    // Now an ambiguous reading: 1 lost in 40 = 25permille, between the walls.
    UplinkCalibParams q = fast_params();
    q.probe_epochs = 40;
    q.ambiguous_epochs = 80;
    UplinkCalibrator cal2(q, 0, false);
    CHECK(cal2.start(1000, 0));
    t = 1000 + q.settle_ms + 1;
    QualitySample amb;
    amb.accepted = true;
    amb.progressed = true;
    amb.epoch_delta = 40;
    amb.reports_delta = 39;
    amb.rssi_sum_delta = -40 * 39;
    cal2.on_sample(amb, t);
    (void)cal2.tick(t, 40, true);
    CHECK(cal2.dwell_target() == q.ambiguous_epochs);  // extended, not decided
    CHECK(cal2.dwell_progress() == 40);                // same dwell continues
}

}  // namespace

int main() {
    test_clean_ramp();
    test_floor_start();
    test_counter_blackout_is_evidence();
    test_no_clean_point();
    test_liveness_abort();
    test_abort();
    test_gate_is_epochs_not_time();
    test_ambiguous_extension();
    if (g_fail != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    return 0;
}
