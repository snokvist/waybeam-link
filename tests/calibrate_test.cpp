// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the §10.6 craft-resident calibration engine
// (core/include/wblink/calibrate.h) on the Pass 153 evidence primitive: a
// synthetic channel carries real DwellSender probes into a real
// DwellReceiver (the ground half), whose tallies feed back — so the whole
// §3.16 exchange is under test, not a mock of it. Covers the full 8-rung
// run, minimum-hunt placement below the wall, abort/hard-cap restore edges,
// and the no_wall_found / evidence_lost refusals.
#include "wblink/calibrate.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_fail = 0;
#define CHECK(x)                                                          \
    do {                                                                  \
        if (!(x)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #x);                                             \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

using namespace wblink;

CalibrateParams fast_params() {
    CalibrateParams p;
    p.settle_ms = 5;
    p.dwell_probe_frames = 20;
    p.dwell_verify_frames = 40;
    p.dwell.probe_pace_us = 100;  // fast bench clock
    p.dwell.tally_wait_ms = 20;
    p.dwell.tally_retries = 2;
    p.dwell.max_probes_per_tick = 64;
    return p;
}

// Synthetic channel: per-rung overload wall in qdb. Below the wall, loss has
// an interior minimum below `knee_qdb` (PA compression: loss grows above the
// knee); at/above the wall every probe — including the re-elicited tail — is
// lost.
struct Channel {
    std::array<int32_t, 8> wall_qdb{60, 60, 56, 56, 52, 52, 48, 48};
    int32_t knee_qdb = 40;
    uint16_t base_loss = 2;  // permille below the knee
    int32_t qdb = 0;
    uint8_t rung = 0;

    uint16_t loss_milli() const {
        if (qdb >= wall_qdb[rung]) return 1000;
        const int32_t over = qdb > knee_qdb ? qdb - knee_qdb : 0;
        return static_cast<uint16_t>(
            std::min<int32_t>(999, base_loss + over * 8));
    }
    int8_t rssi() const { return static_cast<int8_t>(-70 + qdb / 4); }
};

// Drive one engine tick + a full probe drain through the channel/receiver.
struct Bench {
    Calibrator cal;
    DwellReceiver rx;
    Channel ch;
    uint64_t now = 1000;
    int restores = 0;
    int artifacts = 0;

    explicit Bench(const CalibrateParams& p) : cal(p) {}

    void step() {
        const CalibActions a = cal.tick(now);
        if (a.pin_rung) ch.rung = *a.pin_rung;
        if (a.set_qdb) ch.qdb = *a.set_qdb;
        if (a.restore) ++restores;
        if (a.artifact_ready) ++artifacts;
        cal.new_tick();
        for (;;) {
            const DwellProbeOut po = cal.next_probe(now);
            if (!po.send) break;
            // Deterministic loss: the first `loss` fraction of the seq space
            // drops; the tail survives unless the wall is total.
            const uint16_t loss = ch.loss_milli();
            const uint32_t n = cal.probe_dwell_count();
            const uint32_t dropped = n * loss / 1000;
            if (po.seq <= dropped) continue;
            const DwellTallyOut t =
                rx.on_probe(cal.probe_run_id(), cal.probe_dwell_id(), po.seq,
                            static_cast<uint16_t>(n), ch.rssi(), ch.rung,
                            now);
            if (t.send) {
                cal.on_tally(t.run_id, t.dwell_id, t.received,
                             t.rssi_sum_dbm, t.rx_mcs, 0x5A);
            }
        }
        ++now;
    }
    bool run(uint64_t budget_ms = 600000) {
        const uint64_t end = now + budget_ms;
        while (now < end && cal.state() == CalibState::kRunning) step();
        step();  // drain the terminal single-shot edges
        return cal.state() != CalibState::kRunning;
    }
};

void test_full_run_places_below_walls() {
    Bench b(fast_params());
    CHECK(b.cal.start(b.now));
    CHECK(b.run());
    CHECK(b.cal.state() == CalibState::kDone);
    CHECK(b.restores == 1);
    CHECK(b.artifacts == 1);
    const CalibArtifact& art = b.cal.artifact();
    for (size_t m = 0; m < 8; ++m) {
        CHECK(art.placement_qdb[m] < b.ch.wall_qdb[m]);
        CHECK(art.ceilings[m].has_bad);
    }
    CHECK((b.cal.word() & 0x03u) == 2);  // done
}

void test_no_wall_found_refused() {
    // §10.6 (Pass 153): the flat-at-ceiling refusal is absolute-space only,
    // and taper_rung_ceiling is the space discriminator (Pass 151). With the
    // taper on, park every rung level at the baseline so all eight share the
    // same untapered ceiling the flat channel can reach.
    CalibrateParams p = fast_params();
    p.max_qdb = 40;       // stop the sweep well under every wall
    p.taper_rung_ceiling = true;
    for (auto& lv : p.levels) lv = 4;  // baseline: no per-rung taper
    Bench b(p);
    b.ch.knee_qdb = 200;  // flat clean everywhere
    CHECK(b.cal.start(b.now));
    CHECK(b.run());
    CHECK(b.cal.state() == CalibState::kFailed);
    CHECK(b.cal.fail_reason() != nullptr &&
          std::strcmp(b.cal.fail_reason(), "no_wall_found") == 0);
    CHECK(b.restores == 1);
    CHECK(b.artifacts == 0);  // persists nothing
}

void test_offset_space_flat_at_ceiling_places() {
    // §10.6 (Pass 153, operator-ruled 2026-08-07): offset space completes a
    // flat-at-ceiling run — the ceiling is the §10.5 boot-safe offset 0.
    CalibrateParams p = fast_params();
    p.max_qdb = 0;
    p.min_qdb = -24;
    p.taper_rung_ceiling = false;
    Bench b(p);
    b.ch.knee_qdb = 200;  // flat clean across the whole window
    CHECK(b.cal.start(b.now));
    CHECK(b.run());
    CHECK(b.cal.state() == CalibState::kDone);
    CHECK(b.restores == 1);
    CHECK(b.artifacts == 1);
    for (size_t m = 0; m < 8; ++m) {
        CHECK(!b.cal.artifact().ceilings[m].has_bad);
    }
}

void test_evidence_lost_when_nothing_delivers() {
    Bench b(fast_params());
    for (auto& w : b.ch.wall_qdb) w = 0;  // everything above the wall
    CHECK(b.cal.start(b.now));
    CHECK(b.run());
    CHECK(b.cal.state() == CalibState::kFailed);
    CHECK(b.cal.fail_reason() != nullptr &&
          std::strcmp(b.cal.fail_reason(), "evidence_lost") == 0);
    CHECK(b.artifacts == 0);
}

void test_abort_restores_once() {
    Bench b(fast_params());
    CHECK(b.cal.start(b.now));
    for (int i = 0; i < 50; ++i) b.step();
    CHECK(b.cal.state() == CalibState::kRunning);
    CHECK(b.cal.abort(b.now));
    b.step();
    CHECK(b.cal.state() == CalibState::kFailed);
    CHECK(b.restores == 1);
    b.step();
    CHECK(b.restores == 1);  // single-shot
    CHECK(b.cal.start(b.now));  // restart re-arms cleanly
    CHECK(b.cal.state() == CalibState::kRunning);
}

void test_hard_cap() {
    CalibrateParams p = fast_params();
    p.hard_cap_ms = 100;
    p.dwell.tally_wait_ms = 500;  // the run outlives the cap
    Bench b(p);
    for (auto& w : b.ch.wall_qdb) w = 0;  // starve evidence
    CHECK(b.cal.start(b.now));
    CHECK(b.run(5000));
    CHECK(b.cal.state() == CalibState::kFailed);
    CHECK(b.cal.fail_reason() != nullptr &&
          std::strcmp(b.cal.fail_reason(), "hard_cap") == 0);
}

void test_start_abort_reject_semantics() {
    Bench b(fast_params());
    CHECK(b.cal.start(b.now));
    CHECK(!b.cal.start(b.now));
    CHECK(b.cal.abort(b.now));
    CHECK(!b.cal.abort(b.now));
}

}  // namespace

int main() {
    test_full_run_places_below_walls();
    test_no_wall_found_refused();
    test_offset_space_flat_at_ceiling_places();
    test_evidence_lost_when_nothing_delivers();
    test_abort_restores_once();
    test_hard_cap();
    test_start_abort_reject_semantics();
    if (g_fail == 0) std::printf("calibrate_test: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
