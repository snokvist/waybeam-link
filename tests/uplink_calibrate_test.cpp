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
#include <vector>

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
    int power_before_rate = 0;
    std::vector<UplinkRate> rates;
    bool quality_live = true;

    explicit Rig(const UplinkCalibParams& p) : cal(p) {}

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
        // Rate before power, exactly as app/main.cpp actuates them (§10.7 R4).
        if (a.set_rate) rates.push_back(*a.set_rate);
        if (a.set_qdb) {
            up.qdb = *a.set_qdb;
            // Every power command must land on a rung that was commanded
            // first. Pass 131's new stranded-actuator surface is the rung, so
            // "power arrived before its rate" is the shape to catch.
            if (rates.empty()) ++power_before_rate;
        }
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

// Rung 0's placement. Pass 131 sweeps eight rungs, but this synthetic uplink
// has no per-rung behaviour — every rung sees the same channel — so rung 0 is
// representative for the seek-shape cases below. The eight-rung structure
// itself is pinned separately, in test_eight_rung_sweep().
const UplinkPlacement& first_placement(const UplinkCalibrator& c) {
    static const UplinkPlacement kNone{};
    return c.placements().empty() ? kNone : c.placements().front();
}

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
    const UplinkPlacement& pl = first_placement(r.cal);
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
    const UplinkPlacement& pl = first_placement(r.cal);
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
    UplinkCalibrator cal(p);
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
    UplinkCalibrator cal(p);
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
    UplinkCalibrator cal2(q);
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

// Case 20: the dwell denominator is anchored on the craft's own
// last_report_epoch bounds, so a report emitted after E_B belongs to the NEXT
// dwell. This is the arithmetic the 15/50permille walls sit on: at a 40-epoch
// dwell one boundary-straddling report is 25permille, which lands between the
// walls and would turn every clean dwell ambiguous.
void test_loss_denominator_boundary() {
    UplinkCalibParams p = fast_params();
    p.probe_epochs = 40;
    p.ambiguous_epochs = 80;
    p.seek.loss_ok_milli = 15;
    p.seek.loss_bad_milli = 50;
    UplinkCalibrator cal(p);
    CHECK(cal.start(1000, 0));
    const uint64_t t = 1000 + p.settle_ms + 1;

    // A perfectly clean dwell: the craft saw 40 epochs and accepted all 40.
    // The ground emitted more than that in wall-clock terms -- reports 41 and
    // 42 are still in flight -- but they are outside (E_A, E_B] and must not
    // be counted against this dwell.
    QualitySample s;
    s.accepted = true;
    s.progressed = true;
    s.epoch_delta = 40;
    s.reports_delta = 40;
    s.rssi_sum_delta = -40 * 40;
    cal.on_sample(s, t);
    // local_epoch is 42: two more emitted than the craft has acknowledged.
    // A wall-clock or raw-emission denominator would read 2/42 = 47permille
    // here and extend; the anchored one reads 0.
    (void)cal.tick(t, 42, true);
    CHECK(cal.dwell_target() == p.probe_epochs);  // decided, not extended
    CHECK(cal.state() == CalibState::kRunning);

    // And a genuinely lossy dwell still reads lossy: 4 lost in 40 = 100
    // permille, past the bad wall.
    UplinkCalibrator lossy(p);
    CHECK(lossy.start(1000, 0));
    QualitySample bad;
    bad.accepted = true;
    bad.progressed = true;
    bad.epoch_delta = 40;
    bad.reports_delta = 36;
    bad.rssi_sum_delta = -40 * 36;
    lossy.on_sample(bad, t);
    (void)lossy.tick(t, 40, true);
    CHECK(lossy.dwell_target() == p.probe_epochs);  // decided, not ambiguous
}

// Feed one complete dwell's worth of evidence to a directly-driven
// calibrator: advance past settle, hand it a single telescoped sample, tick.
// `local_epoch` stays 0 throughout so the blackout fallback never fires — the
// craft's own anchors decide these dwells.
void feed_dwell(UplinkCalibrator& cal, uint64_t& t, uint32_t settle_ms,
                uint32_t emitted, uint32_t received, int8_t rssi) {
    t += settle_ms + 1;
    QualitySample s;
    s.accepted = true;
    s.progressed = true;
    s.epoch_delta = emitted;
    s.reports_delta = received;
    s.rssi_sum_delta = static_cast<int32_t>(rssi) *
                       static_cast<int32_t>(received);
    cal.on_sample(s, t);
    (void)cal.tick(t, 0, true);
}

// C1 regression. A PARTIAL blackout — some epochs land, then the uplink dies
// mid-dwell — leaves the craft's anchors frozen SHORT of target with
// received_ > 0. Keying the fallback on `received_ == 0` wedged that dwell
// until the 600 s hard cap, which made the §10.7 floor rule unreachable in
// exactly the scenario it exists for. The dwell must end on the ground's own
// epoch count, score 1000permille, and ascend.
void test_partial_blackout_ends_dwell() {
    UplinkCalibParams p = fast_params();
    p.probe_epochs = 40;
    p.ambiguous_epochs = 80;
    UplinkCalibrator cal(p);
    CHECK(cal.start(1000, 0));
    const int32_t floor_qdb = p.seek.min_qdb;
    CHECK(cal.qdb() == floor_qdb);

    uint64_t t = 1000 + p.settle_ms + 1;
    (void)cal.tick(t, 0, true);  // arms the ground epoch anchor at 0

    // 20 of the 40 epochs land, then the craft goes silent. received_ is 20,
    // so the old `received_ == 0` guard could never fire.
    QualitySample s;
    s.accepted = true;
    s.progressed = true;
    s.epoch_delta = 20;
    s.reports_delta = 20;
    s.rssi_sum_delta = -40 * 20;
    cal.on_sample(s, t);
    (void)cal.tick(t, 20, true);
    CHECK(cal.state() == CalibState::kRunning);
    CHECK(cal.qdb() == floor_qdb);  // still deciding
    CHECK(cal.dwell_progress() == 20);

    // The ground keeps emitting under live feedback. At target the dwell must
    // decide against the ground's own count.
    t += 2000;
    (void)cal.tick(t, p.probe_epochs, true);
    CHECK(cal.state() == CalibState::kRunning);  // not a timeout, an observation
    CHECK(cal.qdb() > floor_qdb);                // scored 1000permille -> ascend
    CHECK(cal.dwell_progress() == 0);            // a fresh dwell at the new power
}

// Bench regression (P1, first live §10.7 run). The blackout fallback scored a
// flat 1000permille, but it fires whenever the craft's anchors fall short of
// the target — and the craft's `last_report_epoch` only advances for reports it
// ACCEPTED, so on any lossy link it lags the ground's local clock by exactly
// the lost count. The local clock therefore hits the target first, and a
// measured 198-of-199 verify dwell (~5permille, clean) scored 1000permille and
// failed the run. Score the local span against `received_` instead.
void test_blackout_scores_measured_loss_not_flat_1000() {
    UplinkCalibParams p = fast_params();
    p.probe_epochs = 40;
    p.ambiguous_epochs = 80;
    UplinkCalibrator cal(p);
    CHECK(cal.start(1000, 0));
    uint64_t t = 1000 + p.settle_ms + 1;
    (void)cal.tick(t, 0, true);  // arm the ground anchor at 0

    // 39 of the 40 land; the craft's anchor stops one short of target while
    // the ground's own clock reaches it. That is a CLEAN dwell.
    QualitySample s;
    s.accepted = true;
    s.progressed = true;
    s.epoch_delta = 39;
    s.reports_delta = 39;
    s.rssi_sum_delta = -40 * 39;
    cal.on_sample(s, t);
    t += 2000;
    (void)cal.tick(t, 40, true);
    const auto& d = cal.last_dwell();
    CHECK(d.blackout);                 // the fallback did end the dwell
    CHECK(d.loss_milli <= 30);         // ...but scored the MEASURED loss
    CHECK(d.rssi_mean == -40);         // and kept the real RSSI
    CHECK(cal.state() == CalibState::kRunning);
    CHECK(cal.qdb() > p.seek.min_qdb); // clean at the floor -> ramp upward

    // A TOTAL blackout is unchanged: nothing received, still 1000permille,
    // still the seek's floor evidence.
    UplinkCalibrator dead(p);
    CHECK(dead.start(1000, 0));
    uint64_t t2 = 1000 + p.settle_ms + 1;
    (void)dead.tick(t2, 0, true);
    t2 += 2000;
    (void)dead.tick(t2, 40, true);
    CHECK(dead.last_dwell().blackout);
    CHECK(dead.last_dwell().loss_milli == 1000);
    CHECK(dead.qdb() > p.seek.min_qdb);  // ascends off the dead floor
}

// Operator ruling (Pass 129): persistence is the deliverable. A run whose
// artifact never reached disk commissioned nothing — it applied a placement
// that dies at the next boot — so reporting `done` is the same false success
// C2 removed, one layer out. The Hub menu binds the state field, and
// `fingerprint: 0` was the only signal anything had gone wrong.
void test_failed_persist_fails_the_run() {
    Rig r(fast_params());
    CHECK(r.cal.start(r.now, r.local_epoch));
    r.run(r.now + 600000);
    CHECK(r.cal.state() == CalibState::kDone);
    CHECK(r.artifacts == 1);          // the caller was told to persist

    r.cal.fail_persist();             // ...and the store write failed
    CHECK(r.cal.state() == CalibState::kFailed);
    CHECK(r.cal.fail_reason() != nullptr);
    if (r.cal.fail_reason() != nullptr) {
        CHECK(std::string(r.cal.fail_reason()) == "artifact_write_failed");
    }
    // The restore edge re-arms so the actuator returns to its pre-run owner
    // rather than holding an unpersisted placement.
    const UplinkCalibActions a = r.cal.tick(r.now, r.local_epoch, true);
    CHECK(a.restore);
    CHECK(!a.artifact_ready);
    // Only a Done run can fail this way; it must not rewrite a real failure.
    Rig f(fast_params());
    f.up.floor_rssi = 0;              // never delivers -> no_clean_point
    CHECK(f.cal.start(f.now, f.local_epoch));
    f.run(f.now + 600000);
    CHECK(f.cal.state() == CalibState::kFailed);
    f.cal.fail_persist();
    if (f.cal.fail_reason() != nullptr) {
        CHECK(std::string(f.cal.fail_reason()) == "no_clean_point");
    }
}

// Pass 130: a silently-capped radio no longer stops the sweep. The §10.7
// placement is applied to the same adapter it was measured on, so commanding
// max_qdb into a capped radio radiates the cap — identical behaviour, and no
// risk of mistaking an adapter's flat bottom step for a ceiling. This is the
// bench case that placed at min_qdb and had to be failed under Pass 129; it
// now simply succeeds at the top of the range.
void test_silent_cap_sweeps_to_max() {
    Rig r(fast_params());
    r.up.cap_qdb = 4;   // delivered power never follows commanded
    CHECK(r.cal.start(r.now, r.local_epoch));
    r.run(r.now + 600000);
    CHECK(r.cal.state() == CalibState::kDone);
    CHECK(first_placement(r.cal).placement_qdb == UplinkCalibParams{}.seek.max_qdb);
    CHECK(r.artifacts == 1);
    CHECK(r.restores == 1);
}

// Bench regression: the artifact's overload bracket must describe where the
// link breaks from being too HOT. §10.7 kept a second copy of the bracket
// alongside the seek's, and only the seek applied the "bad ABOVE a clean
// probe" rule — so a dead floor booked itself as the ceiling. Measured live as
// `first_bad_qdb: 4, last_clean_qdb: 108`, i.e. the bottom of the range
// recorded as the top.
void test_bracket_never_books_the_cold_floor() {
    Rig r(fast_params());
    r.up.floor_rssi = -34;   // qdb 4 and 20 deliver nothing; 36+ is clean
    CHECK(r.cal.start(r.now, r.local_epoch));
    r.run(r.now + 600000);
    CHECK(r.cal.state() == CalibState::kDone);
    const UplinkPlacement& pl = first_placement(r.cal);
    CHECK(pl.placement_qdb > UplinkCalibParams{}.seek.min_qdb);
    // The sweep crossed a dead floor and then ran clean to the top, so there
    // is no overload evidence at all and the bracket must say so.
    CHECK(!pl.has_first_bad);
    CHECK(pl.first_bad_qdb == 0);
    CHECK(pl.last_clean_qdb == pl.placement_qdb);

    // With a real ceiling the bracket is booked, and above the clean probe.
    Rig h(fast_params());
    h.up.ceil_rssi = -30;    // overload once RSSI reaches -30
    CHECK(h.cal.start(h.now, h.local_epoch));
    h.run(h.now + 600000);
    CHECK(h.cal.state() == CalibState::kDone);
    const UplinkPlacement& hp = first_placement(h.cal);
    CHECK(hp.has_first_bad);
    CHECK(hp.first_bad_qdb > hp.last_clean_qdb);   // ceiling is ABOVE the floor
    // The placement sits AT the last clean probe, or below it when the verify
    // dwell stepped down. It can never sit above: verify starts at the last
    // clean probe and only ever descends. Asserting equality here was wrong —
    // near a real ceiling the longer verify dwell is exactly the case Pass 130
    // kept the bounded step-down for (a placement that probed clean verifying
    // at 942permille is the measured example), so the two fields legitimately
    // differ and the gap between them is what says the probe was optimistic.
    CHECK(hp.placement_qdb <= hp.last_clean_qdb);
}

// C2 regression. PowerSeek reaches kDone from verify with loss above
// loss_ok_milli only when the descent budget or the floor is exhausted. §10.6
// records that ("the artifact never lies") because a craft artifact is a
// record the operator reads. The §10.7 artifact AUTO-APPLIES and gates the
// sequencer, so the same outcome there is a false success: it would persist an
// unusable placement and start the downlink across a dead uplink, defeating
// the order law through a state no interlock inspects.
void test_verify_exhausted_is_failure() {
    UplinkCalibParams p = fast_params();
    p.seek.loss_ok_milli = 15;
    p.seek.loss_bad_milli = 50;
    UplinkCalibrator cal(p);
    CHECK(cal.start(1000, 0));
    uint64_t t = 1000;

    // The reproduced ladder: clean@4, clean@20, bad@36 -> place at 20.
    feed_dwell(cal, t, p.settle_ms, p.probe_epochs, p.probe_epochs, -40);
    CHECK(cal.qdb() == 20);
    feed_dwell(cal, t, p.settle_ms, p.probe_epochs, p.probe_epochs, -36);
    CHECK(cal.qdb() == 36);
    feed_dwell(cal, t, p.settle_ms, p.probe_epochs, 1, -33);
    CHECK(cal.qdb() == 20);  // retreated to the last clean probe, now verifying

    // Verify at 20 fails under sustained exposure -> the bounded step-down.
    feed_dwell(cal, t, p.settle_ms, p.verify_epochs, p.verify_epochs - 1, -36);
    CHECK(cal.qdb() == 4);
    CHECK(cal.state() == CalibState::kRunning);

    // Verify at the floor fails too, and there is nowhere left to descend.
    feed_dwell(cal, t, p.settle_ms, p.verify_epochs, 1, -40);
    CHECK(cal.state() == CalibState::kFailed);
    CHECK(cal.fail_reason() != nullptr);
    if (cal.fail_reason() != nullptr) {
        CHECK(std::string(cal.fail_reason()) == "verify_failed");
    }
    // Nothing is persisted, and the power is restored.
    const UplinkCalibActions a = cal.tick(t, 0, true);
    CHECK(!a.artifact_ready);
    CHECK(!a.restore);  // already drained by the terminal step

    // And the sequencer must not reach the downlink phase from it.
    CalibSequencer q;
    q.start(1000);
    const SeqActions sa = q.tick(cal.state(), -1, t);
    CHECK(!sa.start_downlink);
    CHECK(q.phase() == CalibPhase::kFailed);
}

// C3 regression. first_bad_qdb_ survived start(), so a second run that never
// saw a bad probe published the FIRST run's bracket alongside
// has_first_bad=false — a value the writer serializes as JSON null and the
// loader reads back as 0, so the artifact failed its own fingerprint on
// reload. Two runs in one process; the second must publish a zeroed bracket.
void test_second_run_clears_bracket() {
    Rig r(fast_params());
    r.up.ceil_rssi = -33;  // qdb 36 overloads: run 1 records a bad probe
    CHECK(r.cal.start(r.now, r.local_epoch));
    r.run(r.now + 600000);
    CHECK(r.cal.state() == CalibState::kDone);
    CHECK(first_placement(r.cal).has_first_bad);
    const int32_t first_bad = first_placement(r.cal).first_bad_qdb;
    CHECK(first_bad > 0);

    r.up.ceil_rssi = 127;  // run 2 is clean all the way to max
    CHECK(r.cal.start(r.now, r.local_epoch));
    r.run(r.now + 600000);
    CHECK(r.cal.state() == CalibState::kDone);
    CHECK(!first_placement(r.cal).has_first_bad);
    CHECK(first_placement(r.cal).first_bad_qdb == 0);  // not run 1's bracket
}

// Pass 131: the run sweeps all eight rungs and emits eight placements in
// ascending order, each carrying the rate identity it was measured at. This
// is the shape the artifact's `placements` list is written from, and the
// shape a future rate policy indexes.
void test_eight_rung_sweep() {
    Rig r(fast_params());
    CHECK(r.cal.start(r.now, r.local_epoch));
    r.run(r.now + 600000);
    CHECK(r.cal.state() == CalibState::kDone);
    CHECK(r.cal.placements().size() == kUplinkRungs);
    CHECK(r.restores == 1);   // ONE restore for the whole run, not one per rung
    CHECK(r.artifacts == 1);  // ...and one artifact, at the end

    // Rate identity per rung, matching the §9.3 seeds: long GI on 0/1, short
    // on 2..7. A placement measured at a GI the uplink cannot be commanded to
    // would be a number about nothing.
    const UplinkCalibParams seeds;
    for (size_t i = 0; i < kUplinkRungs; ++i) {
        const UplinkPlacement& pl = r.cal.placements()[i];
        CHECK(pl.mcs == seeds.rungs[i].mcs);
        CHECK(pl.short_gi == seeds.rungs[i].short_gi);
        CHECK(pl.placement_loss_milli <= 15);
        // Every rung sweeps from the floor, so on this flat synthetic channel
        // every rung reaches the same top. What matters is that each is a real
        // measurement rather than rung 0's result copied forward.
        CHECK(pl.placement_qdb == seeds.seek.max_qdb);
    }

    // The rate was commanded once per rung, in order, and power never arrived
    // on an uncommanded rung. This is Pass 131's new stranded-actuator
    // surface: the sweep borrows the uplink's operating rung, not just its
    // power.
    CHECK(r.power_before_rate == 0);
    CHECK(r.rates.size() == kUplinkRungs);
    for (size_t i = 0; i < r.rates.size() && i < kUplinkRungs; ++i) {
        CHECK(r.rates[i] == seeds.rungs[i]);
    }
}

// A run that dies mid-sweep persists NOTHING. A truncated artifact would
// auto-apply at the next boot looking like a finished commissioning — the
// same false-success shape Pass 126's verify rule and Pass 129's write rule
// each removed one layer further out.
void test_failure_mid_sweep_persists_nothing() {
    Rig r(fast_params());
    CHECK(r.cal.start(r.now, r.local_epoch));
    // Let a few rungs complete, then take the observer away.
    while (r.cal.state() == CalibState::kRunning && r.cal.rung() < 3) r.step();
    CHECK(r.cal.rung() == 3);
    CHECK(r.cal.placements().size() == 3);  // rungs 0..2 measured and kept
    r.quality_live = false;
    r.step();

    CHECK(r.cal.state() == CalibState::kFailed);
    CHECK(r.cal.fail_reason() != nullptr);
    if (r.cal.fail_reason() != nullptr) {
        CHECK(std::string(r.cal.fail_reason()) == "quality_lost");
    }
    // Three placements are in hand, but no artifact was ever offered: the
    // caller is never told to persist a partial sweep.
    CHECK(r.artifacts == 0);
    CHECK(r.restores == 1);  // ...and the actuators still came back, once
}

// §10.7 sequencer: one operator action, two phases, in the right order.
void test_sequencer() {
    // Happy path: uplink done -> downlink issued -> craft runs -> done.
    {
        CalibSequencer q;
        q.start(1000);
        CHECK(q.phase() == CalibPhase::kUplink);
        CHECK(q.active());
        // Uplink still running: nothing is issued.
        SeqActions a = q.tick(CalibState::kRunning, -1, 1100);
        CHECK(!a.start_downlink);
        CHECK(q.phase() == CalibPhase::kUplink);
        // Uplink done -> the downlink campaign goes out, exactly once.
        a = q.tick(CalibState::kDone, -1, 2000);
        CHECK(a.start_downlink);
        CHECK(q.phase() == CalibPhase::kDownlink);
        a = q.tick(CalibState::kDone, -1, 2100);
        CHECK(!a.start_downlink);
        // The craft picks it up, then finishes.
        (void)q.tick(CalibState::kDone, 1, 3000);
        CHECK(q.phase() == CalibPhase::kDownlink);
        (void)q.tick(CalibState::kDone, 2, 60000);
        CHECK(q.phase() == CalibPhase::kDone);
        CHECK(!q.active());
        CHECK(q.fail_reason() == nullptr);
    }

    // THE order law: a failed uplink phase must never start the downlink.
    // §10.6 depends on the uplink delivering reports, so running it across a
    // bad one measures the wrong thing.
    {
        CalibSequencer q;
        q.start(1000);
        const SeqActions a = q.tick(CalibState::kFailed, -1, 2000);
        CHECK(!a.start_downlink);
        CHECK(q.phase() == CalibPhase::kFailed);
        CHECK(q.fail_reason() != nullptr);
        if (q.fail_reason() != nullptr) {
            CHECK(std::string(q.fail_reason()) == "uplink_phase_failed");
        }
    }

    // A stale done/failed word from a PREVIOUS craft run must not be read as
    // this phase's result before the craft has even picked the command up.
    {
        CalibSequencer q;
        q.start(1000);
        CHECK(q.tick(CalibState::kDone, 2, 2000).start_downlink);
        // Craft still airing the old "done" — not ours, keep waiting.
        (void)q.tick(CalibState::kDone, 2, 2100);
        CHECK(q.phase() == CalibPhase::kDownlink);
        (void)q.tick(CalibState::kDone, 1, 3000);   // now it starts
        (void)q.tick(CalibState::kDone, 2, 40000);  // and finishes
        CHECK(q.phase() == CalibPhase::kDone);
    }

    // A lost VCMD must not hang the sequencer forever.
    {
        CalibSequencer q(5000);
        q.start(1000);
        CHECK(q.tick(CalibState::kDone, 2000, 2000).start_downlink);
        (void)q.tick(CalibState::kDone, -1, 4000);
        CHECK(q.phase() == CalibPhase::kDownlink);
        (void)q.tick(CalibState::kDone, -1, 9000);
        CHECK(q.phase() == CalibPhase::kFailed);
        if (q.fail_reason() != nullptr) {
            CHECK(std::string(q.fail_reason()) == "downlink_no_ack");
        }
    }

    // A craft-side failure is reported as the DOWNLINK phase failing, so the
    // operator knows which half to look at.
    {
        CalibSequencer q;
        q.start(1000);
        (void)q.tick(CalibState::kDone, -1, 2000);
        (void)q.tick(CalibState::kDone, 1, 3000);
        (void)q.tick(CalibState::kDone, 3, 40000);
        CHECK(q.phase() == CalibPhase::kFailed);
        if (q.fail_reason() != nullptr) {
            CHECK(std::string(q.fail_reason()) == "downlink_phase_failed");
        }
    }

    // Abort cancels whichever phase is live, and only the downlink one owes
    // an over-air cancel.
    {
        CalibSequencer q;
        q.start(1000);
        SeqActions a = q.abort(1500);
        CHECK(!a.abort_downlink);  // uplink phase: local abort only
        CHECK(q.phase() == CalibPhase::kFailed);
        a = q.abort(1600);
        CHECK(!a.abort_downlink);  // idempotent, nothing live

        CalibSequencer q2;
        q2.start(1000);
        (void)q2.tick(CalibState::kDone, -1, 2000);
        CHECK(q2.phase() == CalibPhase::kDownlink);
        a = q2.abort(2500);
        CHECK(a.abort_downlink);  // the craft is running: cancel over air
        CHECK(q2.phase() == CalibPhase::kFailed);
    }

    // W5: once saw_running_ latches, the craft simply going quiet is `-1` —
    // deliberately not a terminal state, because the §3.15 word is sticky and
    // a real result is expected to land. A craft that DIES mid-phase never
    // sends one, which latched active() forever and made every future start
    // in either direction fail an interlock guarding a sequence that ended.
    {
        CalibSequencer q(15000, 60000);
        q.start(1000);
        CHECK(q.tick(CalibState::kDone, -1, 2000).start_downlink);
        (void)q.tick(CalibState::kDone, 1, 3000);  // craft picks it up
        (void)q.tick(CalibState::kDone, -1, 50000);
        CHECK(q.phase() == CalibPhase::kDownlink);  // still within the cap
        CHECK(q.active());
        const SeqActions a = q.tick(CalibState::kDone, -1, 70000);
        CHECK(a.abort_downlink);  // tell the craft to stop, it may still be on
        CHECK(q.phase() == CalibPhase::kFailed);
        CHECK(!q.active());  // and the interlock releases
        if (q.fail_reason() != nullptr) {
            CHECK(std::string(q.fail_reason()) == "downlink_timeout");
        }
    }

    // The cap must never beat a legitimately slow craft run: the default sits
    // above §10.6's own 600 s hard cap.
    {
        CalibSequencer q;
        q.start(1000);
        (void)q.tick(CalibState::kDone, -1, 2000);
        (void)q.tick(CalibState::kDone, 1, 3000);
        (void)q.tick(CalibState::kDone, 1, 600000);
        CHECK(q.phase() == CalibPhase::kDownlink);
        (void)q.tick(CalibState::kDone, 2, 601000);
        CHECK(q.phase() == CalibPhase::kDone);
    }

    // A plain `start` never enters the sequencer, so `phase` stays idle: it
    // describes the SEQUENCER, and reporting a lone uplink run as a
    // half-finished bi-directional one would be a lie.
    {
        CalibSequencer q;
        CHECK(q.phase() == CalibPhase::kIdle);
        CHECK(!q.active());
        const SeqActions a = q.tick(CalibState::kDone, 2, 5000);
        CHECK(!a.start_downlink);
        CHECK(q.phase() == CalibPhase::kIdle);
        CHECK(std::string(CalibSequencer::phase_name(q.phase())) == "idle");
    }
}

}  // namespace

int main() {
    test_sequencer();
    test_partial_blackout_ends_dwell();
    test_blackout_scores_measured_loss_not_flat_1000();
    test_verify_exhausted_is_failure();
    test_bracket_never_books_the_cold_floor();
    test_failed_persist_fails_the_run();
    test_silent_cap_sweeps_to_max();
    test_second_run_clears_bracket();
    test_loss_denominator_boundary();
    test_clean_ramp();
    test_floor_start();
    test_counter_blackout_is_evidence();
    test_no_clean_point();
    test_liveness_abort();
    test_abort();
    test_gate_is_epochs_not_time();
    test_ambiguous_extension();
    test_eight_rung_sweep();
    test_failure_mid_sweep_persists_nothing();
    if (g_fail != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    return 0;
}
