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

// The bench-measured channel: rssi ≈ -41 + 0.85*dBm, and each rung has an
// overload ceiling RSSI above which loss goes to ~1000‰.
struct Channel {
    int32_t qdb = 0;
    int8_t rssi() const {
        return static_cast<int8_t>(-41 + 0.85 * (qdb / 4.0));
    }
    uint16_t loss(uint8_t rung) const {
        // Reachable within the model's power range (cap ≈ -18 dBm RSSI).
        static constexpr int8_t kCeil[8] = {127, 127, 127, -19, -19,
                                            -22, -26, -26};
        return rssi() >= kCeil[rung] ? 900 : 5;
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

    explicit Rig(const CalibrateParams& p) : cal(p) {}

    void run(uint64_t until_ms, bool feed_reports = true) {
        while (now < until_ms) {
            now += 100;
            if (feed_reports && now % 100 == 0) {
                cal.on_report(ch.rssi(), ch.loss(pinned == 255 ? 0 : pinned),
                              300, now);
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
    for (int m = 0; m < 8; ++m) {
        // Placement steered into the band around -32.
        CHECK(a.placement_rssi[m] >= -36 && a.placement_rssi[m] <= -28);
        CHECK(a.placement_loss_milli[m] <= 15);
        // §10.2 level compensation baked into the curve.
        CHECK(a.curve_qdb[m] ==
              a.placement_qdb[m] -
                  (int32_t(CalibrateParams{}.levels[m]) - 4) * 8);
    }
    // Rungs with a model ceiling found it; MCS0-2 are cap-clean.
    CHECK(!a.ceilings[0].has_bad);
    CHECK(!a.ceilings[2].has_bad);
    for (int m = 3; m < 8; ++m) {
        CHECK(a.ceilings[m].has_bad);
        CHECK(a.ceilings[m].first_bad_rssi >= a.ceilings[m].last_clean_rssi);
    }
    // §3.15 word: done state, artifact hash is the app's business.
    CHECK((r.cal.word() & 0x03) == 2);
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
    test_report_loss_abort();
    test_hard_cap();
    test_abort_cmd();
    if (g_fail != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    return 0;
}
