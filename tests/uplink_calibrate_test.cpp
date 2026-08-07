// SPDX-License-Identifier: GPL-2.0-or-later
// §10.7 ground-uplink calibrator tests, single-rung (Pass 153): a synthetic
// uplink channel carries real DwellSender probes into a real DwellReceiver
// (the craft half), whose tallies feed back. Covers the clean placement run
// with the R4 rate-then-power action order, verify_failed and no_wall_found
// refusals, evidence_lost, abort/fail_persist restore edges, and the
// set_max_qdb running guard.
#include "wblink/uplink_calibrate.h"

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

UplinkCalibParams fast_params() {
    UplinkCalibParams p;
    p.settle_ms = 5;
    p.dwell_probe_frames = 20;
    p.dwell_verify_frames = 40;
    p.dwell.probe_pace_us = 100;
    p.dwell.tally_wait_ms = 20;
    p.dwell.tally_retries = 2;
    p.dwell.max_probes_per_tick = 64;
    p.rate = UplinkRate{2, true};
    return p;
}

struct Channel {
    int32_t wall_qdb = 60;
    int32_t knee_qdb = 40;
    uint16_t base_loss = 2;
    int32_t qdb = 0;
    uint16_t loss_milli() const {
        if (qdb >= wall_qdb) return 1000;
        const int32_t over = qdb > knee_qdb ? qdb - knee_qdb : 0;
        return static_cast<uint16_t>(
            std::min<int32_t>(999, base_loss + over * 8));
    }
    int8_t rssi() const { return static_cast<int8_t>(-70 + qdb / 4); }
};

struct Bench {
    UplinkCalibrator cal;
    DwellReceiver rx;  // the craft half
    Channel ch;
    uint64_t now = 1000;
    int restores = 0;
    int artifacts = 0;
    int rate_sets = 0;
    UplinkRate last_rate{};
    bool rate_before_power = false;

    explicit Bench(const UplinkCalibParams& p) : cal(p) {}

    void step() {
        const UplinkCalibActions a = cal.tick(now);
        if (a.set_rate) {
            ++rate_sets;
            last_rate = *a.set_rate;
            // R4: the first tick carrying both must order rate before power
            // — encoded structurally: both present on one action set is the
            // start tick, and the caller applies set_rate first. Assert both
            // arrive together at start.
            if (a.set_qdb) rate_before_power = true;
        }
        if (a.set_qdb) ch.qdb = *a.set_qdb;
        if (a.restore) ++restores;
        if (a.artifact_ready) ++artifacts;
        cal.new_tick();
        for (;;) {
            const DwellProbeOut po = cal.next_probe(now);
            if (!po.send) break;
            const uint16_t loss = ch.loss_milli();
            const uint32_t n = cal.probe_dwell_count();
            const uint32_t dropped = n * loss / 1000;
            if (po.seq <= dropped) continue;
            const DwellTallyOut t =
                rx.on_probe(cal.probe_run_id(), cal.probe_dwell_id(), po.seq,
                            static_cast<uint16_t>(n), ch.rssi(),
                            cal.rate().mcs, now);
            if (t.send) {
                cal.on_tally(t.run_id, t.dwell_id, t.received,
                             t.rssi_sum_dbm, t.rx_mcs, 0xC3);
            }
        }
        ++now;
    }
    bool run(uint64_t budget_ms = 600000) {
        const uint64_t end = now + budget_ms;
        while (now < end && cal.state() == CalibState::kRunning) step();
        step();
        return cal.state() != CalibState::kRunning;
    }
};

void test_clean_run_places_below_wall() {
    Bench b(fast_params());
    CHECK(b.cal.start(b.now));
    CHECK(b.run());
    CHECK(b.cal.state() == CalibState::kDone);
    CHECK(b.restores == 1);
    CHECK(b.artifacts == 1);
    CHECK(b.rate_sets == 1);
    CHECK(b.rate_before_power);
    CHECK(b.last_rate.mcs == 2 && b.last_rate.short_gi);
    CHECK(b.cal.placements().size() == 1);
    const UplinkPlacement& pl = b.cal.placements()[0];
    CHECK(pl.mcs == 2 && pl.short_gi);
    CHECK(pl.placement_qdb < b.ch.wall_qdb);
    CHECK(pl.has_first_bad);
    CHECK(pl.placement_loss_milli <= 15);
    // D-A evidence carried through: the last dwell's cross-check fields.
    CHECK(b.cal.last_dwell().rung == 0);
}

void test_no_wall_found_refused() {
    UplinkCalibParams p = fast_params();
    p.seek.max_qdb = 40;
    p.taper_rung_ceiling = false;
    Bench b(p);
    b.ch.knee_qdb = 200;  // clean all the way to the ceiling
    CHECK(b.cal.start(b.now));
    CHECK(b.run());
    CHECK(b.cal.state() == CalibState::kFailed);
    CHECK(b.cal.fail_reason() != nullptr &&
          std::strcmp(b.cal.fail_reason(), "no_wall_found") == 0);
    CHECK(b.artifacts == 0);
    CHECK(b.restores == 1);
}

void test_verify_failed_when_nothing_acceptable() {
    UplinkCalibParams p = fast_params();
    Bench b(p);
    // Loss everywhere between the walls: nothing beats loss_ok (15) but the
    // seek never sees loss_bad either — the verify walk exhausts its budget
    // and the placed best is still unacceptable.
    b.ch.base_loss = 30;
    b.ch.knee_qdb = 0;
    b.ch.wall_qdb = 200;  // never total
    CHECK(b.cal.start(b.now));
    CHECK(b.run());
    CHECK(b.cal.state() == CalibState::kFailed);
    CHECK(b.cal.fail_reason() != nullptr &&
          (std::strcmp(b.cal.fail_reason(), "verify_failed") == 0 ||
           std::strcmp(b.cal.fail_reason(), "no_clean_point") == 0));
    CHECK(b.artifacts == 0);
}

void test_evidence_lost() {
    Bench b(fast_params());
    b.ch.wall_qdb = 0;  // nothing ever delivers, no tally ever forms
    CHECK(b.cal.start(b.now));
    CHECK(b.run());
    CHECK(b.cal.state() == CalibState::kFailed);
    CHECK(b.cal.fail_reason() != nullptr &&
          std::strcmp(b.cal.fail_reason(), "evidence_lost") == 0);
}

void test_abort_and_fail_persist_restore() {
    Bench b(fast_params());
    CHECK(b.cal.start(b.now));
    for (int i = 0; i < 30; ++i) b.step();
    CHECK(b.cal.abort(b.now));
    b.step();
    CHECK(b.restores == 1);
    b.step();
    CHECK(b.restores == 1);  // single-shot

    // fail_persist flips a done run to failed and re-arms the restore.
    Bench c(fast_params());
    CHECK(c.cal.start(c.now));
    CHECK(c.run());
    CHECK(c.cal.state() == CalibState::kDone);
    CHECK(c.restores == 1);
    c.cal.fail_persist();
    CHECK(c.cal.state() == CalibState::kFailed);
    CHECK(c.cal.fail_reason() != nullptr &&
          std::strcmp(c.cal.fail_reason(), "artifact_write_failed") == 0);
    c.step();
    CHECK(c.restores == 2);
}

void test_set_max_qdb_guard() {
    Bench b(fast_params());
    CHECK(b.cal.set_max_qdb(80));
    CHECK(b.cal.start(b.now));
    CHECK(!b.cal.set_max_qdb(60));  // refused while running
    CHECK(b.cal.abort(b.now));
    CHECK(b.cal.set_max_qdb(60));
}

}  // namespace

int main() {
    test_clean_run_places_below_wall();
    test_no_wall_found_refused();
    test_verify_failed_when_nothing_acceptable();
    test_evidence_lost();
    test_abort_and_fail_persist_restore();
    test_set_max_qdb_guard();
    if (g_fail == 0) std::printf("uplink_calibrate_test: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
