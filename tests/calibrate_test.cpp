// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the §10.6 craft-resident calibration engine
// (core/include/wblink/calibrate.h): a synthetic channel model drives the
// full 8-rung loop; the abort paths (report loss, hard cap, §11.7 abort)
// each verify the single-shot restore action.
#include "wblink/calibrate.h"
#include "wblink/wire.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

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

// The bench-measured channel: rssi ≈ -41 + 0.85*dBm off the DELIVERED
// power (cap_qdb models the silently-latched RF cap of Pass 121), and
// each rung has an overload ceiling RSSI above which loss goes to ~1000‰.
struct Channel {
    int32_t qdb = 0;
    int32_t cap_qdb = 10000;  // default: no cap
    std::array<int8_t, 8> ceil{127, 127, 127, -24, -24, -27, -30, -30};
    int8_t rssi() const {
        return static_cast<int8_t>(-41 +
                                   0.85 * (std::min(qdb, cap_qdb) / 4.0));
    }
    // Near-cliff instability (addendum 2): at/above flaky_above the link
    // survives short exposure but collapses under sustained exposure —
    // clean through a probe dwell, bad partway into a verify dwell.
    int8_t flaky_above = 127;
    mutable int hot = 0;
    // One-dwell RSSI noise glitch (addendum 3): the first dwell at
    // glitch_qdb reads 3 dB low — 7 samples = settle(2) + probe(5) at
    // the fast_params cadence — then reads true. A confirmation dwell
    // sees the real value.
    int32_t glitch_qdb = -1;
    mutable int glitch_left = 7;
    int8_t rssi_sample() const {
        if (qdb == glitch_qdb && glitch_left > 0) {
            --glitch_left;
            return static_cast<int8_t>(rssi() - 3);
        }
        return rssi();
    }
    uint16_t loss(uint8_t rung) const {
        const int8_t r = rssi();
        if (r >= ceil[rung]) return 900;
        if (r >= flaky_above && ++hot > 10) return 900;
        return 5;
    }
};

// Drive: apply actions to the model, feed reports at 10 Hz, tick at 100 ms.
struct Rig {
    Calibrator cal;
    Channel ch;
    uint8_t pinned = 255;
    uint64_t now = 1000;
    int restores = 0;
    int artifacts = 0;
    // Addendum 4: above this commanded power the feedback channel itself
    // collapses — no reports are delivered at all.
    int32_t blackout_above = 1 << 30;

    explicit Rig(const CalibrateParams& p) : cal(p) {}

    void run(uint64_t until_ms, bool feed_reports = true) {
        while (now < until_ms) {
            now += 100;
            if (feed_reports && ch.qdb <= blackout_above && now % 100 == 0) {
                cal.on_report(ch.rssi_sample(),
                              ch.loss(pinned == 255 ? 0 : pinned), 300, now);
            }
            const CalibActions a = cal.tick(now);
            if (a.pin_rung) pinned = *a.pin_rung;
            if (a.set_qdb) ch.qdb = *a.set_qdb;
            if (a.restore) ++restores;
            if (a.artifact_ready) ++artifacts;
            if (cal.state() != CalibState::kRunning && !a.restore &&
                !a.artifact_ready) {
                return;
            }
        }
    }
};

CalibrateParams fast_params() {
    CalibrateParams p;
    p.settle_ms = 200;
    p.probe_dwell_ms = 500;
    p.verify_dwell_ms = 800;
    p.hard_cap_ms = 600000;  // generous: the fast dwells finish way under
    return p;
}

void test_full_run_and_artifact() {
    Rig r(fast_params());
    CHECK(r.cal.start(r.now));
    CHECK(!r.cal.start(r.now));  // §11.7: start while running = REJECTED
    r.run(r.now + 590000);
    CHECK(r.cal.state() == CalibState::kDone);
    CHECK(r.restores == 1);
    CHECK(r.artifacts == 1);
    const CalibArtifact& a = r.cal.artifact();
    // Pass 121 max-power seek against the model: rungs 0-2 ramp clean to
    // max_qdb (the -6 guard is out of the model's reach); rungs 3-7 stop
    // one step below their loss wall. Rung 3's first probes (from rung
    // 2's placement - one step) are already past its wall, exercising
    // the descend path.
    static constexpr int32_t kWant[8] = {108, 108, 108, 60, 60, 60, 44, 44};
    for (int m = 0; m < 8; ++m) {
        CHECK(a.placement_qdb[m] == kWant[m]);
        CHECK(a.placement_loss_milli[m] <= 15);
        // §10.2 level compensation baked into the curve.
        CHECK(a.curve_qdb[m] ==
              a.placement_qdb[m] -
                  (int32_t(CalibrateParams{}.levels[m]) - 4) * 8);
    }
    // Rungs with a model ceiling found it during the ramp; MCS0-2 are
    // guard-stopped with no bad probe.
    CHECK(!a.ceilings[0].has_bad);
    CHECK(!a.ceilings[2].has_bad);
    for (int m = 3; m < 8; ++m) {
        CHECK(a.ceilings[m].has_bad);
        CHECK(a.ceilings[m].first_bad_rssi >= a.ceilings[m].last_clean_rssi);
    }
    // §3.15 word: done state, artifact hash is the app's business.
    CHECK((r.cal.word() & 0x03) == 2);
}

void test_cap_wall() {
    // A silently-latched RF cap (the Pass 121 trigger): delivered power
    // stops following commanded power at 13 dBm. No rung ever shows loss,
    // yet every placement must land on the cap, not the commanded cap-max.
    Rig r(fast_params());
    r.ch.cap_qdb = 52;
    r.ch.ceil = {127, 127, 127, 127, 127, 127, 127, 127};
    CHECK(r.cal.start(r.now));
    r.run(r.now + 590000);
    CHECK(r.cal.state() == CalibState::kDone);
    const CalibArtifact& a = r.cal.artifact();
    for (int m = 0; m < 8; ++m) {
        CHECK(a.placement_qdb[m] == 52);
        CHECK(!a.ceilings[m].has_bad);  // cap wall, not overload
    }
}

void test_verify_backoff() {
    // Addendum 2: a placement that probes clean but fails the verify
    // dwell must step down and re-verify — the fp 0xE3 rung-5 defect
    // (probed clean, verified 942‰, recorded and moved on).
    Rig r(fast_params());
    r.ch.cap_qdb = 52;
    r.ch.ceil = {127, 127, 127, 127, 127, 127, 127, 127};
    r.ch.flaky_above = -31;  // 52 qdb lands at -29: flaky under exposure
    CHECK(r.cal.start(r.now));
    r.run(r.now + 590000);
    CHECK(r.cal.state() == CalibState::kDone);
    const CalibArtifact& a = r.cal.artifact();
    for (int m = 0; m < 8; ++m) {
        // Every rung ends one step below the flaky zone, verified clean.
        CHECK(a.placement_qdb[m] == 36);
        CHECK(a.placement_loss_milli[m] <= 15);
    }
    // The verify failure recorded rung 0's overload bracket.
    CHECK(a.ceilings[0].has_bad);
    CHECK(a.ceilings[0].first_bad_rssi == -29);
}

void test_cap_confirm_rejects_noise() {
    // Addendum 3: one noisy dwell at 68 qdb reads 3 dB low — without the
    // confirmation re-dwell this is a false cap wall (the 10-run
    // campaign's 8 dB MCS7 flip). The confirm dwell reads true and the
    // ramp must continue to the genuine limit.
    Rig r(fast_params());
    r.ch.ceil = {127, 127, 127, 127, 127, 127, 127, 127};
    r.ch.glitch_qdb = 68;
    CHECK(r.cal.start(r.now));
    r.run(r.now + 590000);
    CHECK(r.cal.state() == CalibState::kDone);
    // Rung 0 ramps clean to max_qdb despite the glitch.
    CHECK(r.cal.artifact().placement_qdb[0] == 108);
}

void test_blackout_retreat() {
    // Addendum 4: probing above 52 qdb kills the feedback channel (total
    // overload — the v2 campaign's rung-7 report_loss aborts). The loop
    // must book the wall, retreat to the last clean power, and finish.
    Rig r(fast_params());
    r.ch.ceil = {127, 127, 127, 127, 127, 127, 127, 127};
    r.blackout_above = 52;
    CHECK(r.cal.start(r.now));
    r.run(r.now + 590000);
    CHECK(r.cal.state() == CalibState::kDone);
    CHECK(r.restores == 1);
    const CalibArtifact& a = r.cal.artifact();
    for (int m = 0; m < 8; ++m) {
        CHECK(a.placement_qdb[m] == 52);
        CHECK(a.ceilings[m].has_bad);  // blackout booked as the bracket
        CHECK(a.placement_loss_milli[m] <= 15);
    }
}

void test_report_loss_abort() {
    Rig r(fast_params());
    CHECK(r.cal.start(r.now));
    r.run(r.now + 2000);  // let it get going
    CHECK(r.cal.state() == CalibState::kRunning);
    r.run(r.now + 10000, /*feed_reports=*/false);  // reports stop
    CHECK(r.cal.state() == CalibState::kFailed);
    CHECK(r.cal.fail_reason() != nullptr);
    CHECK(r.restores == 1);
    CHECK(r.artifacts == 0);  // no artifact on failure
}

void test_hard_cap() {
    CalibrateParams p = fast_params();
    p.hard_cap_ms = 3000;
    Rig r(p);
    CHECK(r.cal.start(r.now));
    r.run(r.now + 10000);
    CHECK(r.cal.state() == CalibState::kFailed);
    CHECK(r.restores == 1);
}

void test_abort_cmd() {
    Rig r(fast_params());
    CHECK(!r.cal.abort(r.now));  // §11.7: abort while idle = REJECTED
    CHECK(r.cal.start(r.now));
    r.run(r.now + 1500);
    CHECK(r.cal.abort(r.now));
    CHECK(!r.cal.abort(r.now));  // idempotent-in-effect, second = REJECTED
    const CalibActions a = r.cal.tick(r.now + 100);
    CHECK(a.restore);  // single-shot restore on the next tick
    CHECK(!r.cal.tick(r.now + 200).restore);
    // A failed run can be restarted.
    CHECK(r.cal.start(r.now + 300));
}

void test_selector_state_calib_word() {
    // §3.15 Pass 120: bit4 word encodes at 36 bytes and round-trips; bit4
    // without bit3 is refused in both directions.
    SelectorState in;
    in.prefix = {17, 0, 123456};
    in.state_flags = selector_state_flags::kHolderPresent |
                     selector_state_flags::kCalibPresent;
    in.report_latch_holder = 9;
    in.calib_word = 0x15;  // running, rung 5
    in.calib_fingerprint = 0xBF;
    uint8_t buf[64];
    const size_t n = encode_selector_state(in, buf, sizeof buf);
    CHECK(n == kSelectorStateCalibSize);
    const auto dec = decode(buf, n);
    const SelectorState* out = std::get_if<SelectorState>(&dec);
    CHECK(out != nullptr);
    if (out) {
        CHECK(out->calib_word == 0x15);
        CHECK(out->calib_fingerprint == 0xBF);
        CHECK(out->report_latch_holder == 9);
    }
    // bit4 without bit3: encoder refuses...
    SelectorState bad = in;
    bad.state_flags = selector_state_flags::kCalibPresent;
    CHECK(encode_selector_state(bad, buf, sizeof buf) == 0);
    // ...and the decoder refuses the same shape on the wire.
    buf[16] = selector_state_flags::kCalibPresent;
    const auto dec2 = decode(buf, n);
    CHECK(std::get_if<SelectorState>(&dec2) == nullptr);
    // Legacy 34- and 32-byte shapes still decode (mixed-version pair).
    SelectorState legacy = in;
    legacy.state_flags = selector_state_flags::kHolderPresent;
    CHECK(encode_selector_state(legacy, buf, sizeof buf) ==
          kSelectorStateSize);
    const auto dec3 = decode(buf, kSelectorStateSize);
    CHECK(std::get_if<SelectorState>(&dec3) != nullptr);
}

}  // namespace

int main() {
    test_selector_state_calib_word();
    test_full_run_and_artifact();
    test_cap_wall();
    test_verify_backoff();
    test_cap_confirm_rejects_noise();
    test_blackout_retreat();
    test_report_loss_abort();
    test_hard_cap();
    test_abort_cmd();
    if (g_fail != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    return 0;
}
