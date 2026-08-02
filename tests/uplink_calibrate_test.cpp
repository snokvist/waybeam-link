// SPDX-License-Identifier: GPL-2.0-or-later
// §10.7 ground-uplink calibrator tests. A synthetic uplink drives the burst
// loop end to end: the clean ramp, the floor start that every real run begins
// in, the eight-rung sweep, liveness abort, and the restore-on-every-exit law.
//
// What differs from §10.6 and so is what is pinned here: the ground SENDS a
// counted burst of probes and divides by its own count (Pass 132), rather
// than measuring a stream whose rate it does not control. A burst that
// delivers nothing is a 1000permille observation, never a timeout.
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
    int restores = 0;
    int artifacts = 0;
    int power_before_rate = 0;
    int bursts = 0;
    uint32_t budget = 0;
    uint32_t inflight = 0;   // built, not yet committed (§7.2 batching)
    std::vector<UplinkRate> rates;
    bool quality_live = true;

    explicit Rig(const UplinkCalibParams& p) : cal(p) {}

    // One 10 ms tick. The ground emits whatever the calibrator's probe
    // budget allows -- capped per tick to model the §7.2 quiet gap, which
    // fits roughly a dozen small frames -- and the craft counts what it
    // receives. §3.16 feedback still arrives at 2 Hz.
    static constexpr uint32_t kProbesPerGap = 12;

    void step() {
        now += 10;
        // Reports are BUILT (spending budget) and injected later, at the §7.2
        // return window -- only then is the epoch committed. The lag is what
        // made a top-up loop run away on hardware, so the rig models it.
        uint32_t build = budget < kProbesPerGap ? budget : kProbesPerGap;
        budget -= build;
        inflight += build;
        if (now % 20 == 0) {             // return window: commit the batch
            while (inflight-- > 0) {
                ++local_epoch;
                craft_reports += up.delivered(1);
            }
            inflight = 0;
        }
        if (now - last_fb_ms_ >= 500) {  // 2 Hz §3.16
            last_fb_ms_ = now;
            QualitySample s;
            s.accepted = true;
            if (craft_reports != last_reports_) {
                s.progressed = true;
                s.reports_delta = craft_reports - last_reports_;
                s.rssi_sum_delta =
                    static_cast<int32_t>(up.rssi()) *
                    static_cast<int32_t>(s.reports_delta);
                last_reports_ = craft_reports;
            }
            cal.on_sample(s, now);
        }
        const UplinkCalibActions a =
            cal.tick(now, local_epoch, quality_live, budget == 0);
        // Rate before power, exactly as app/main.cpp actuates them (§10.7 R4).
        if (a.set_rate) rates.push_back(*a.set_rate);
        if (a.set_qdb) {
            up.qdb = *a.set_qdb;
            // Every power command must land on a rung that was commanded
            // first. The rung is Pass 131's new stranded-actuator surface, so
            // "power arrived before its rate" is the shape to catch.
            if (rates.empty()) ++power_before_rate;
        }
        if (a.probe_budget) {
            budget = *a.probe_budget;
            ++bursts;
        }
        if (a.restore) ++restores;
        if (a.artifact_ready) ++artifacts;
    }

    void run(uint64_t until_ms) {
        while (now < until_ms && cal.state() == CalibState::kRunning) step();
        // Drain the single-shot restore/artifact edge after the terminal step.
        const UplinkCalibActions a =
            cal.tick(now, local_epoch, quality_live, budget == 0);
        if (a.restore) ++restores;
        if (a.artifact_ready) ++artifacts;
    }

    uint32_t last_reports_ = 0;
    uint64_t last_fb_ms_ = 0;
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
    p.probe_epochs = 20;
    p.verify_epochs = 40;
    p.drain_ms = 600;
    p.liveness_ms = 2000;
    return p;
}

// Feed one complete burst: settle, arm, let the ground send the whole burst,
// deliver `received` of it, drain, score. That is the entire measurement —
// compare it with the anchoring dance Pass 125 needed to do the same job.
void feed_dwell(UplinkCalibrator& cal, uint64_t& t, uint32_t& epoch,
                const UplinkCalibParams& p, uint32_t received, int8_t rssi) {
    const uint32_t target = cal.dwell_target();
    t += p.settle_ms + 1;
    (void)cal.tick(t, epoch, true, false);  // arms the anchor
    (void)cal.tick(t, epoch, true, false);  // issues the burst
    epoch += target;                        // ...the ground sends it
    if (received > 0) {
        QualitySample s;
        s.accepted = true;
        s.progressed = true;
        s.reports_delta = received;
        s.rssi_sum_delta =
            static_cast<int32_t>(rssi) * static_cast<int32_t>(received);
        cal.on_sample(s, t);
    }
    (void)cal.tick(t, epoch, true, true);   // burst spent -> drain opens
    t += p.drain_ms + 1;
    (void)cal.tick(t, epoch, true, true);   // drain elapsed -> score
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
    const UplinkCalibActions a = r.cal.tick(r.now, r.local_epoch, true, true);
    CHECK(a.restore);
    CHECK(!a.artifact_ready);
    CHECK(r.cal.state() == CalibState::kFailed);
    const UplinkCalibActions b = r.cal.tick(r.now, r.local_epoch, true, true);
    CHECK(!b.restore);  // single-shot
}

// The dwell gate is the BURST, not the clock: a dwell decides when its probes
// have gone out and drained, never on elapsed time. Wall time alone must not
// end one, however long it runs.
void test_gate_is_the_burst_not_time() {
    UplinkCalibParams p = fast_params();
    UplinkCalibrator cal(p);
    CHECK(cal.start(1000, 0));
    CHECK(cal.dwell_target() == p.probe_epochs);

    // The burst is issued exactly once, and while it is still being emitted
    // (`burst_spent` false) no amount of time decides anything. This is also
    // the regression for the bench runaway: the budget is handed out on ONE
    // tick, so a top-up loop cannot re-arm it faster than the §7.2 batch
    // drains and flood the return path (measured: 3480 probes for a 100
    // burst, which starved the craft's §3.16 into `quality_lost`).
    int budgets_issued = 0;
    for (uint64_t t = 1200; t < 60000; t += 100) {
        const UplinkCalibActions a = cal.tick(t, 0, true, false);
        if (a.probe_budget) ++budgets_issued;
        CHECK(!a.restore);
    }
    CHECK(budgets_issued == 1);
    CHECK(cal.state() == CalibState::kRunning);
    CHECK(cal.dwell_progress() == 0);
}

// Pass 132 deletes the ambiguous extension. It existed because a 40-probe
// gate put ONE lost report at 25permille -- between loss_ok_milli (15) and
// loss_bad_milli (50) -- so the reading could not be made and the dwell had
// to be re-run longer. With the ground choosing the burst size, the fix is to
// choose one big enough: at 100 probes one loss is 10permille (clean) and
// five are 50permille (the wall), so every reading is decidable first time.
// config.cpp enforces exactly this (1000/N <= loss_ok_milli).
void test_burst_resolves_the_walls_first_time() {
    UplinkCalibParams p = fast_params();
    p.probe_epochs = 100;
    p.seek.loss_ok_milli = 15;
    p.seek.loss_bad_milli = 50;
    UplinkCalibrator cal(p);
    CHECK(cal.start(1000, 0));
    uint64_t t = 1000;
    uint32_t epoch = 0;

    // 99 of 100 = 10permille: clean, decided, and the seek steps UP.
    const int32_t floor_qdb = cal.qdb();
    feed_dwell(cal, t, epoch, p, 99, -40);   // clean -> Pass 133 confirm burst
    feed_dwell(cal, t, epoch, p, 99, -40);   // confirmed; this one scores
    CHECK(cal.last_dwell().loss_milli == 10);
    CHECK(cal.qdb() > floor_qdb);            // clean -> climb
    CHECK(cal.dwell_target() == p.probe_epochs);  // a NEW dwell, not extended

    // 94 of 100 = 60permille: past the bad wall, decided the other way.
    UplinkCalibrator bad(p);
    CHECK(bad.start(1000, 0));
    uint64_t t2 = 1000;
    uint32_t e2 = 0;
    feed_dwell(bad, t2, e2, p, 94, -40);
    CHECK(bad.last_dwell().loss_milli == 60);
    CHECK(bad.dwell_target() == p.probe_epochs);
}

// THE Pass 132 simplification, pinned. The denominator is the ground's own
// count of probes the radio took -- not the craft's `last_report_epoch`
// delta, and not wall time. Reports still in flight cannot skew it, because
// the burst is finished and drained before anything is scored.
//
// This one property retires four mechanisms: the `emitted = E_B - E_A`
// anchoring identity (Pass 125), the local-epoch blackout fallback (Pass 126
// C1), that fallback's measured-loss scoring rule (Pass 128), and the
// ambiguous extension. Passes 126 and 128 were both defects INSIDE that
// machinery.
void test_denominator_is_the_grounds_own_count() {
    UplinkCalibParams p = fast_params();
    p.probe_epochs = 100;
    UplinkCalibrator cal(p);
    CHECK(cal.start(1000, 0));
    uint64_t t = 1000;
    uint32_t epoch = 0;

    // A perfectly clean burst: 100 sent, 100 counted. Two of them, because
    // Pass 133 re-runs a rung's first clean probe before accepting it.
    feed_dwell(cal, t, epoch, p, 100, -40);
    feed_dwell(cal, t, epoch, p, 100, -40);
    CHECK(cal.last_dwell().sent == 100);
    CHECK(cal.last_dwell().received == 100);
    CHECK(cal.last_dwell().loss_milli == 0);
    CHECK(cal.last_dwell().rssi_mean == -40);

    // The craft's own epoch counter is NOT consulted: QualitySample carries an
    // epoch_delta field and the calibrator ignores it entirely. Feed a wildly
    // wrong one and the score is unchanged.
    UplinkCalibrator cal2(p);
    CHECK(cal2.start(1000, 0));
    uint64_t t2 = 1000;
    uint32_t e2 = 0;
    const uint32_t target = cal2.dwell_target();
    t2 += p.settle_ms + 1;
    (void)cal2.tick(t2, e2, true, true);
    e2 += target;
    QualitySample s;
    s.accepted = true;
    s.progressed = true;
    s.reports_delta = 100;
    s.epoch_delta = 9999;               // nonsense; must not be used
    s.rssi_sum_delta = -40 * 100;
    cal2.on_sample(s, t2);
    (void)cal2.tick(t2, e2, true, true);
    t2 += p.drain_ms + 1;
    (void)cal2.tick(t2, e2, true, true);
    feed_dwell(cal2, t2, e2, p, 100, -40);   // the Pass 133 confirm burst
    CHECK(cal2.last_dwell().sent == 100);
    CHECK(cal2.last_dwell().loss_milli == 0);
}

// The C1 shape, which the burst model makes structurally impossible. A
// PARTIAL blackout -- some probes land, then the uplink dies mid-burst --
// used to leave the craft's anchors frozen short of target with received > 0,
// so the dwell never ended and the run wedged at min_qdb until the 600 s hard
// cap. Now the dwell ends when the GROUND has finished sending, which it
// always does, and the partial delivery is simply the measured loss.
void test_partial_blackout_ends_the_burst() {
    UplinkCalibParams p = fast_params();
    p.probe_epochs = 100;
    UplinkCalibrator cal(p);
    CHECK(cal.start(1000, 0));
    const int32_t floor_qdb = cal.qdb();
    uint64_t t = 1000;
    uint32_t epoch = 0;

    // Half the burst lands, then silence. received > 0, so the old
    // `received == 0` guard could never have fired.
    feed_dwell(cal, t, epoch, p, 50, -40);
    CHECK(cal.state() == CalibState::kRunning);
    CHECK(cal.last_dwell().sent == 100);
    CHECK(cal.last_dwell().received == 50);
    CHECK(cal.last_dwell().loss_milli == 500);  // measured, not a flat 1000
    CHECK(cal.qdb() > floor_qdb);               // bad at the floor -> climb
}

// A totally dead burst is the seek's floor evidence and still scores
// 1000permille with the synthetic guard RSSI. The Pass 128 bench failure --
// a 198-of-199 verify dwell scored as a total blackout because the craft's
// accepted-epoch anchor lagged the ground's clock by the lost count -- cannot
// recur: there is no anchor to lag, only the ground's own count.
void test_dead_burst_is_1000_permille() {
    UplinkCalibParams p = fast_params();
    p.probe_epochs = 100;
    UplinkCalibrator dead(p);
    CHECK(dead.start(1000, 0));
    uint64_t t = 1000;
    uint32_t epoch = 0;
    const int32_t floor_qdb = dead.qdb();
    feed_dwell(dead, t, epoch, p, 0, -40);
    CHECK(dead.last_dwell().received == 0);
    CHECK(dead.last_dwell().loss_milli == 1000);
    CHECK(dead.qdb() > floor_qdb);   // ascends off the dead floor

    // ...and the near-clean case that Pass 128 mis-scored reads clean. Two
    // bursts: Pass 133 re-runs the FIRST clean probe of a rung once before it
    // may establish `last_clean`, so the second is the one that scores.
    UplinkCalibrator ok(p);
    CHECK(ok.start(1000, 0));
    uint64_t t2 = 1000;
    uint32_t e2 = 0;
    feed_dwell(ok, t2, e2, p, 99, -40);
    feed_dwell(ok, t2, e2, p, 99, -40);
    CHECK(ok.last_dwell().loss_milli == 10);
    CHECK(ok.last_dwell().rssi_mean == -40);
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
    const UplinkCalibActions a = r.cal.tick(r.now, r.local_epoch, true, true);
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
    uint32_t epoch = 0;

    // Pass 133 ladder: clean@4 (confirmed), clean@20, then bad all the way to
    // max_qdb. The bracket is booked at 36 but the sweep runs the FULL range
    // before placing at the highest clean probe, 20.
    feed_dwell(cal, t, epoch, p, p.probe_epochs, -40);   // clean, triggers confirm
    CHECK(cal.qdb() == 4);                                // same power re-run
    feed_dwell(cal, t, epoch, p, p.probe_epochs, -40);   // confirmed clean
    CHECK(cal.qdb() == 20);
    feed_dwell(cal, t, epoch, p, p.probe_epochs, -36);
    CHECK(cal.qdb() == 36);
    // Every remaining step is bad; none of them ends the sweep.
    for (int32_t q = 36; q < p.seek.max_qdb; q += p.seek.seek_step_qdb) {
        feed_dwell(cal, t, epoch, p, 1, -33);
    }
    feed_dwell(cal, t, epoch, p, 1, -33);                 // the max_qdb probe
    CHECK(cal.qdb() == 20);  // placed at the highest clean probe, now verifying

    // Verify at 20 fails under sustained exposure -> the bounded step-down.
    feed_dwell(cal, t, epoch, p, p.verify_epochs - 1, -36);
    CHECK(cal.qdb() == 4);
    CHECK(cal.state() == CalibState::kRunning);

    // Verify at the floor fails too, and there is nowhere left to descend.
    feed_dwell(cal, t, epoch, p, 1, -40);
    CHECK(cal.state() == CalibState::kFailed);
    CHECK(cal.fail_reason() != nullptr);
    if (cal.fail_reason() != nullptr) {
        CHECK(std::string(cal.fail_reason()) == "verify_failed");
    }
    // Nothing is persisted, and the power is restored.
    const UplinkCalibActions a = cal.tick(t, 0, true, true);
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
    test_partial_blackout_ends_the_burst();
    test_dead_burst_is_1000_permille();
    test_verify_exhausted_is_failure();
    test_bracket_never_books_the_cold_floor();
    test_failed_persist_fails_the_run();
    test_silent_cap_sweeps_to_max();
    test_second_run_clears_bracket();
    test_denominator_is_the_grounds_own_count();
    test_clean_ramp();
    test_floor_start();
    test_counter_blackout_is_evidence();
    test_no_clean_point();
    test_liveness_abort();
    test_abort();
    test_gate_is_the_burst_not_time();
    test_burst_resolves_the_walls_first_time();
    test_eight_rung_sweep();
    test_failure_mid_sweep_persists_nothing();
    if (g_fail != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    return 0;
}
