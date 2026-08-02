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
#include <optional>
#include <string>

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
    // Pass 125: the model only ever had an overload CEILING, which is why
    // the floor case went unexercised through the whole Pass 121 campaign.
    // Below floor_rssi the link is simply too weak to deliver.
    int8_t floor_rssi = -128;
    uint16_t loss(uint8_t rung) const {
        const int8_t r = rssi();
        if (r >= ceil[rung]) return 900;
        if (r < floor_rssi) return 900;
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
    // Addendum 5: hysteretic blackout — tripping at bo_trip holds until
    // power drops to bo_clear (a marginal rung's overload doesn't release
    // at the power that entered it).
    int32_t bo_trip = 1 << 30;
    int32_t bo_clear = -1;
    bool blacked = false;

    explicit Rig(const CalibrateParams& p) : cal(p) {}

    void run(uint64_t until_ms, bool feed_reports = true) {
        while (now < until_ms) {
            now += 100;
            if (ch.qdb >= bo_trip) blacked = true;
            else if (ch.qdb <= bo_clear) blacked = false;
            if (feed_reports && ch.qdb <= blackout_above && !blacked &&
                now % 100 == 0) {
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
    // Pass 130 max-power sweep against the model: rungs 0-2 climb clean to
    // max_qdb (the -6 guard is out of the model's reach); rungs 3-7 stop at
    // the highest clean step below their loss wall. Every rung sweeps from
    // min_qdb, so the placements are a property of the CHANNEL alone — under
    // the old mid-range seeding rung 3 landed at 60 because the descent grid
    // stepped straight past the clean 68.
    static constexpr int32_t kWant[8] = {108, 108, 108, 68, 68, 52, 36, 36};
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

void test_silent_rf_cap_does_not_stop_the_sweep() {
    // A silently-latched RF cap: delivered power stops following commanded at
    // 13 dBm, so RSSI plateaus while loss stays clean.
    //
    // Pass 121 stopped the ramp there. Pass 130 does not, because stopping
    // buys nothing and can cost a great deal: commanding max_qdb into a capped
    // radio still radiates the cap, so the placement behaves identically —
    // while a plateau that is NOT a cap (AGC, noise, an adapter whose bottom
    // step is flat) throws away real headroom. Measured on the craft's own
    // validated run, the cap wall was the limiter on rungs 4/6/7 and discarded
    // 12 dB at MCS7 with loss at 1permille and no wall in sight.
    Rig r(fast_params());
    r.ch.cap_qdb = 52;
    r.ch.ceil = {127, 127, 127, 127, 127, 127, 127, 127};
    CHECK(r.cal.start(r.now));
    r.run(r.now + 590000);
    CHECK(r.cal.state() == CalibState::kDone);
    const CalibArtifact& a = r.cal.artifact();
    for (int m = 0; m < 8; ++m) {
        CHECK(a.placement_qdb[m] == CalibrateParams{}.max_qdb);
        CHECK(!a.ceilings[m].has_bad);   // no loss wall was ever hit
        CHECK(a.placement_loss_milli[m] <= 15);
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

void test_verify_blackout_descent() {
    // Addendum 5: the v3 rung-6 aborts — the seek blackout retreat lands
    // on a placement that itself blacks out under sustained exposure
    // (hysteresis: tripped at 52, releases only at ≤20). Verify must
    // take the bounded step-down instead of aborting.
    Rig r(fast_params());
    r.ch.ceil = {127, 127, 127, 127, 127, 127, 127, 127};
    r.bo_trip = 52;
    r.bo_clear = 20;
    CHECK(r.cal.start(r.now));
    r.run(r.now + 590000);
    CHECK(r.cal.state() == CalibState::kDone);
    CHECK(r.restores == 1);
    const CalibArtifact& a = r.cal.artifact();
    for (int m = 0; m < 8; ++m) {
        CHECK(a.placement_qdb[m] == 20);  // descended into the clear zone
        CHECK(a.placement_loss_milli[m] <= 15);
    }
}

void test_report_loss_abort() {
    Rig r(fast_params());
    CHECK(r.cal.start(r.now));
    r.run(r.now + 2000);  // let it get going
    CHECK(r.cal.state() == CalibState::kRunning);
    // Reports stop for good. Each blacked-out dwell is simply "not clean", so
    // the sweep climbs; the run ends when it runs out of range, or on the
    // report clock if it had already banked a clean probe to retreat to.
    r.run(r.now + 120000, /*feed_reports=*/false);
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
    // Abort → start with NO tick in between: the undrained restore must
    // not fire mid-new-run.
    CHECK(r.cal.abort(r.now + 400));
    CHECK(r.cal.start(r.now + 500));
    CHECK(!r.cal.tick(r.now + 600).restore);
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

// Pass 125 floor rule. Before it, a rung-0 ramp whose floor is unusable
// placed at min_qdb (loss wall + no last_clean + qdb <= min ⇒ place(min)),
// recording a placement the link cannot actually carry. Now it ascends.
void test_floor_ascend() {
    Rig r(fast_params());
    // rssi = -41 + 0.85*(qdb/4): qdb 4/20/36 read -40/-36/-33 (all below the
    // floor), qdb 52 reads -29 and delivers. Ceilings are cleared because a
    // -30 floor under the default -30 rung-6/7 ceilings leaves those rungs
    // no clean window at all — that is a model contradiction, not a seek bug.
    r.ch.floor_rssi = -30;
    r.ch.ceil = {127, 127, 127, 127, 127, 127, 127, 127};
    CHECK(r.cal.start(r.now));
    r.run(r.now + 590000);
    CHECK(r.cal.state() == CalibState::kDone);
    const CalibArtifact& a = r.cal.artifact();
    // The placement must be a power that actually worked, never the floor.
    CHECK(a.placement_qdb[0] > CalibrateParams{}.min_qdb);
    CHECK(a.placement_qdb[0] >= 52);
    CHECK(a.placement_loss_milli[0] <= 15);
    // Crossing a dead floor is NOT overload evidence, so it must not book the
    // rung's ceiling bracket: `first_bad_rssi` describes where the link breaks
    // from being too HOT, and the bottom of the range is the opposite. Only a
    // bad probe above a clean one enters it.
    CHECK(!a.ceilings[0].has_bad);
    CHECK(a.ceilings[0].last_clean_rssi <= -6);
}

// A link that never delivers at any commanded power fails with a reason
// that says so, instead of placing at the floor and calling it success.
void test_no_clean_point() {
    Rig r(fast_params());
    r.ch.floor_rssi = 0;  // unreachable: max_qdb only reaches -18
    CHECK(r.cal.start(r.now));
    r.run(r.now + 590000);
    CHECK(r.cal.state() == CalibState::kFailed);
    CHECK(r.cal.fail_reason() != nullptr);
    if (r.cal.fail_reason() != nullptr) {
        CHECK(std::string(r.cal.fail_reason()) == "no_clean_point");
    }
    CHECK(r.restores == 1);  // every exit restores, §10.6 R4
}

// PowerSeek directly (Pass 130): one monotone ascending sweep, no descent,
// no cap wall, no floor rule. Where the sweep STARTS must not affect where it
// ends, which is why every rung now begins at min_qdb.
void test_sweep_is_monotone_and_seed_free() {
    SeekParams p;  // min 4, max 108, step 16

    // A dead bottom is not a special case: those steps are simply not clean
    // and the sweep keeps climbing. No floor rule, no ascend latch.
    {
        PowerSeek s(p);
        CHECK(s.begin().qdb == p.min_qdb);
        SeekStep st = s.on_dwell(DwellVerdict::kBad, -90, 1000);
        CHECK(st.kind == SeekStep::Kind::kProbe);
        CHECK(st.qdb == p.min_qdb + p.seek_step_qdb);
        st = s.on_dwell(DwellVerdict::kBad, -88, 1000);
        CHECK(st.qdb == p.min_qdb + 2 * p.seek_step_qdb);
        // First clean probe; the sweep continues upward looking for more.
        st = s.on_dwell(DwellVerdict::kClean, -40, 5);
        CHECK(st.kind == SeekStep::Kind::kProbe);
        CHECK(st.qdb == p.min_qdb + 3 * p.seek_step_qdb);
        // The wall: bad above a clean probe places at the clean one.
        st = s.on_dwell(DwellVerdict::kBad, -20, 900);
        CHECK(st.kind == SeekStep::Kind::kVerify);
        CHECK(st.qdb == p.min_qdb + 2 * p.seek_step_qdb);
        CHECK(s.has_bad());
        CHECK(s.first_bad_rssi() == -20);       // the OVERLOAD reading...
        // ...and the cold steps below the first clean probe never entered it.
        CHECK(s.last_clean_rssi() == -40);      // bracket has real width
    }

    // A clean sweep that never finds a wall places at max_qdb. A flat RSSI
    // response no longer stops it: if the radio is capped, commanding max
    // still radiates the cap, so there is nothing to gain by stopping lower
    // and 12 dB of measured headroom to lose (Pass 130).
    {
        PowerSeek s(p);
        SeekStep st = s.begin();
        int steps = 0;
        while (st.kind == SeekStep::Kind::kProbe && steps++ < 20) {
            st = s.on_dwell(DwellVerdict::kClean, -40, 2);  // RSSI never moves
        }
        CHECK(st.kind == SeekStep::Kind::kVerify);
        CHECK(st.qdb == p.max_qdb);
        CHECK(!s.has_bad());
    }

    // An uplink that is bad everywhere fails rather than placing.
    {
        PowerSeek s(p);
        SeekStep st = s.begin();
        int steps = 0;
        while (st.kind == SeekStep::Kind::kProbe && steps++ < 20) {
            st = s.on_dwell(DwellVerdict::kBad, -90, 1000);
        }
        CHECK(st.kind == SeekStep::Kind::kFailed);
        CHECK(std::string(st.fail_reason) == "no_clean_point");
    }

    // The rssi_guard backstop still stops a ramp that is cooking the receiver
    // before loss has risen.
    {
        PowerSeek s(p);
        (void)s.begin();
        const SeekStep hot = s.on_dwell(DwellVerdict::kClean, -2, 3);
        CHECK(hot.kind == SeekStep::Kind::kVerify);
        CHECK(hot.qdb == p.min_qdb);
    }

    // kNoEvidence holds position — the craft's blank dwell, never the
    // ground's stalled counter.
    {
        PowerSeek s(p);
        (void)s.begin();
        const SeekStep hold = s.on_dwell(DwellVerdict::kNoEvidence, 0, 0);
        CHECK(hold.qdb == p.min_qdb);
        CHECK(!hold.power_changed);
    }
}

// The addendum-4 retreat books the rung's OVERLOAD bracket, and its upper
// bound is what the blacked-out dwell measured — not the last clean probe's
// reading, which would collapse the bracket to zero width and silently corrupt
// the §10.6 ceiling record. With the sweep, a blackout is just a not-clean
// dwell, so the caller passes the observed RSSI straight in.
void test_blackout_bracket_uses_observed_rssi() {
    SeekParams p;
    PowerSeek s(p);
    (void)s.begin();
    const SeekStep up = s.on_dwell(DwellVerdict::kClean, -40.0, 5);
    CHECK(up.qdb == 20);
    // The dwell at 20 sampled -26 before the feedback channel died.
    const SeekStep bo = s.on_dwell(DwellVerdict::kBad, -26.0, 1000);
    CHECK(bo.kind == SeekStep::Kind::kVerify);
    CHECK(bo.qdb == 4);                 // retreated to the last clean power
    CHECK(s.has_bad());
    CHECK(s.first_bad_rssi() == -26);   // measured
    CHECK(s.last_clean_rssi() == -40);  // bracket has real width
}

}  // namespace

int main() {
    test_selector_state_calib_word();
    test_full_run_and_artifact();
    test_silent_rf_cap_does_not_stop_the_sweep();
    test_verify_backoff();
    test_cap_confirm_rejects_noise();
    test_blackout_retreat();
    test_verify_blackout_descent();
    test_report_loss_abort();
    test_hard_cap();
    test_abort_cmd();
    test_floor_ascend();
    test_no_clean_point();
    test_sweep_is_monotone_and_seed_free();
    test_blackout_bracket_uses_observed_rssi();
    if (g_fail != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    return 0;
}
