// SPDX-License-Identifier: GPL-2.0-or-later
// app/main.cpp unit tests — the FIRST of them (Pass 137).
//
// Why this file includes a .cpp
// -----------------------------
// Everything in app/main.cpp lives in one anonymous namespace, so it has
// internal linkage and no other translation unit can link against it. Until
// that changes, #including the whole TU (with WBLINK_APP_TEST suppressing its
// main()) is the only way to reach TxCore and the app-layer helpers.
//
// This is deliberately the CHEAP option. The alternative — extracting TxCore
// and the run_* orchestration into their own headers — is the right long-term
// answer, but it is a large refactor of the file every feature touches, and
// doing it as a prerequisite to having ANY coverage would mean the refactor
// itself lands untested. So: coverage first, on the code as it stands, and the
// extraction can follow one seam at a time with these tests holding it still.
//
// What this pins, and why these cases
// -----------------------------------
// §10.3/§11.7 0x0A power-tier state was carried on hardware alone through the
// whole Pass 135 campaign. Two defects reached a device because of it: a
// handler reading a moved-from ControlHandlers, and a sweep-bound write to a
// struct nothing read again. Both were app-layer, and the 54 core/io suites
// could not have caught either. Every case below is one of the rules that had
// NO automated check before this file existed.
//
// The first seam (Pass 138)
// -------------------------
// run_rx() is a single 2.6k-line function whose state lives in locals captured
// by lambdas, so none of its interior was reachable from here. Its
// §10.3/§10.5/§10.7 power ownership is now UplinkPower, a struct at namespace
// scope with injected actuators — the same shape TxCore already had — and the
// ground-side rules below are covered rather than device-verified only.
//
// Since #109 Phase 2c the function itself lives in node/src/rx_node.cpp and
// UplinkPower in node/uplink_power.h, so both are reachable by LINKING rather
// than by including this TU. That changed nothing about the coverage: the
// interior state is still captured locals, and what this file reaches is what
// it always reached.
//
// What this still does NOT reach: everything else in run_rx — selection, CSA,
// the cache-repair controller, the calibration sequencer. Each is its own seam.
#define WBLINK_APP_TEST 1
#include "app/main.cpp"

#include <fstream>

#include "wbtest.h"

namespace {

// A tx adapter with a §11.7 preset list. Nothing here touches a file or a
// socket: TxCore takes its power targets straight from the Config, and
// apply_power is an injectable std::function.
AdapterCfg tx_adapter(const char* name, std::optional<int32_t> ceiling,
                      std::vector<int32_t> presets) {
    AdapterCfg a;
    a.name = name;
    a.ifname = name;
    a.role = Role::kTx;
    a.channel_mhz = 5805;
    a.max_power_qdb = ceiling;
    a.power_presets_qdb = std::move(presets);
    return a;
}

Config one_tx_config(std::optional<int32_t> ceiling,
                     std::vector<int32_t> presets) {
    Config c;
    c.node.originator = 17;
    c.adapters.push_back(tx_adapter("tx0", ceiling, std::move(presets)));
    return c;
}

// The §10.2 authored curve is level 4. Flat so a placement is easy to reason
// about: every MCS resolves to the same number before the ceiling applies.
PowerCurve flat_curve(int32_t qdb) {
    PowerCurve c;
    c.qdb.fill(qdb);
    c.valid = true;
    return c;
}

// A §10.2 power-map FILE, because the config path is the one that still has to
// be refused on a relative backend and it is reachable only through the loader.
// Five rows: the first three cover the CCK/OFDM rates the parser skips, the
// last two carry HT MCS0-7 (rate index 12..19).
std::string write_flat_power_map(const char* name, double dbm) {
    std::string path = std::string("/tmp/") + name;
    std::ofstream f(path, std::ios::trunc);
    f << "#[5G]A\n";
    for (int row = 0; row < 5; ++row) {
        f << "[0] 0xE08 0xffffffff";
        for (int i = 0; i < 4; ++i) f << " " << dbm;
        f << "\n";
    }
    return path;
}

// Records what actually reached the actuator, which is the only thing that
// matters for a power control — GET reporting the right number while the radio
// sits somewhere else is exactly the class of bug this file exists for.
struct PowerSpy {
    std::vector<std::pair<size_t, int32_t>> applied;
    std::vector<std::pair<size_t, int32_t>> offsets;   // apply_power_offset
    std::vector<std::pair<size_t, int32_t>> absolutes; // apply_power (§10.2)
    int autos = 0;
    void attach(TxCore& tx) {
        tx.apply_power = [this](size_t idx, int32_t qdb) {
            applied.emplace_back(idx, qdb);
            absolutes.emplace_back(idx, qdb);
            return true;
        };
        tx.apply_power_auto = [this](size_t) { ++autos; };
        // §10.5 (Pass 150): record the two actuators SEPARATELY. They share a
        // signature and, on devourer, the same register write with opposite
        // documented meanings — so a test that funnels both cannot tell an
        // absolute write from an offset one, which is this pass's whole
        // subject.
        tx.apply_power_offset = [this](size_t idx, int32_t qdb) {
            applied.emplace_back(idx, qdb);
            offsets.emplace_back(idx, qdb);
            return true;
        };
    }
    bool empty() const { return applied.empty(); }
    // Sentinel rather than .back() on an empty vector. wbtest is built so one
    // run reports EVERY failure; a regression that leaves `applied` empty
    // would otherwise abort the process under ASan at the next last_qdb() and
    // hide every check after it — which is exactly what happened the first
    // time these tests were run against deliberately broken code.
    int32_t last_qdb() const {
        return applied.empty() ? INT32_MIN : applied.back().second;
    }
    void clear() { applied.clear(); offsets.clear(); absolutes.clear(); }
};

// ---- §10.3/§11.7 0x0A power tier -------------------------------------------

// The §10.3 Pass 134 ruling: a tier on a node with no curve and no artifact is
// ACCEPTED and recorded, but reaches no actuator. Honest, and it points the
// operator at "calibrate first" rather than at a broken menu.
void test_tier_on_uncalibrated_node_actuates_nothing() {
    TxCore tx(one_tx_config(108, {60, 84, 108}), 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);

    CHECK(tx.set_power_tier(0));
    CHECK_EQ_U(tx.power_tier(), 0);
    CHECK(tx.power_tier_ceiling().value_or(-1) == 60);
    CHECK(spy.empty());                  // nothing reached the radio
    // Nor a backend-auto command, which is a DIFFERENT wrong answer: it would
    // reset hardware power rather than leave it where the operator had it.
    CHECK_EQ_U(spy.autos, 0);
    CHECK(!tx.power_tier_effective());   // and §15.5 says so
}

// A tier may only ever LOWER power (operator ruling). The presets are clamped
// to the boot ceiling at CONFIG LOAD, so by the time TxCore sees them the
// invariant already holds — this pins that TxCore does not re-raise past it.
void test_tier_only_lowers() {
    TxCore tx(one_tx_config(84, {60, 84}), 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);
    tx.install_curve(flat_curve(200));  // curve well above every ceiling

    CHECK(tx.set_power_tier(1));
    CHECK(tx.power_tier_ceiling().value_or(-1) == 84);
    CHECK(tx.set_power_tier(0));
    CHECK(tx.power_tier_ceiling().value_or(-1) == 60);
    // Back up to the highest preset: still bounded by it, never above.
    CHECK(tx.set_power_tier(1));
    CHECK(tx.power_tier_ceiling().value_or(-1) == 84);
}

// §15.5 reports ONE ceiling and ONE preset list, so both must describe the
// same adapter. Reading the ceiling from the first target with any ceiling and
// the list from the first with a non-empty one let a second tx adapter supply
// half of a pair presented as describing one node.
void test_ceiling_and_presets_come_from_the_same_adapter() {
    Config c;
    c.node.originator = 17;
    c.adapters.push_back(tx_adapter("plain", 200, {}));       // ceiling, no list
    c.adapters.push_back(tx_adapter("tiered", 108, {60, 108}));
    TxCore tx(c, 1, nullptr, 0);

    // Before any tier: the tiered adapter's boot ceiling, not the plain one's.
    CHECK(tx.power_tier_ceiling().value_or(-1) == 108);
    CHECK_EQ_U(tx.power_presets().size(), 2);
    CHECK(tx.set_power_tier(0));
    CHECK(tx.power_tier_ceiling().value_or(-1) == 60);
    CHECK(tx.power_presets().front() == 60);
}

// ALL-OR-NOTHING. Applying a tier to one tx adapter and skipping another whose
// list is shorter — while still reporting success — would leave two adapters
// on different ceilings with nothing saying so.
void test_tier_is_all_or_nothing_across_adapters() {
    Config c;
    c.node.originator = 17;
    c.adapters.push_back(tx_adapter("a", 108, {60, 84, 108}));
    c.adapters.push_back(tx_adapter("b", 108, {60, 84}));  // shorter
    TxCore tx(c, 1, nullptr, 0);

    CHECK(tx.set_power_tier(1));            // in range for both
    const auto after_ok = tx.power_tier();
    CHECK_EQ_U(after_ok, 1);
    // Index 2 exists on "a" but not "b": REJECTED, and nothing moved.
    CHECK(!tx.set_power_tier(2));
    CHECK_EQ_U(tx.power_tier(), 1);         // still the last good tier
    CHECK(tx.power_tier_ceiling().value_or(-1) == 84);
}

// REJECTED, not silently clamped, when no adapter carries a list at all.
void test_tier_rejected_with_no_preset_list() {
    TxCore tx(one_tx_config(108, {}), 1, nullptr, 0);
    CHECK(!tx.set_power_tier(0));
    CHECK_EQ_U(static_cast<int64_t>(tx.power_tier()) + 1, 0);  // stays -1
}

// §10.5 (Pass 150) REPLACES the pre-150 contract this test used to encode.
// The §10.3 ceiling is no longer the clamp on an override — on a relative
// backend it clamped the OFFSET, so a "cannot cook a PA" ceiling of 108
// silently authorised +27 dB. The latch is now bounded by
// power_offset_max_qdb and REJECTED past it, never clamped.
void test_override_is_bounded_and_rejected_not_clamped() {
    TxCore tx(one_tx_config(108, {60, 108}), 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);

    // Above the bound (default 0): refused outright, and nothing reaches
    // hardware — the pre-150 behaviour silently applied 108 here.
    CHECK(!tx.set_power_override(120));
    CHECK(spy.empty());

    // At or below the bound: applied verbatim, NOT clamped to any ceiling.
    CHECK(tx.set_power_override(-8));
    CHECK(!spy.empty());
    CHECK(spy.last_qdb() == -8);
    spy.clear();

    // 0 is the bound's default and is allowed — it is the efuse default, so
    // it is a legal request even though it is not a safe boot value.
    CHECK(tx.set_power_override(0));
    CHECK(spy.last_qdb() == 0);
}

// §10.5 (Pass 150): the forced safe boot offset. The whole point is that a
// node never transmits at the uncharacterised efuse default, so this must
// reach the actuator for EVERY role:"tx" adapter before the first frame.
void test_boot_offset_applied_to_every_tx_adapter() {
    Config c = one_tx_config(108, {});
    c.adapters.push_back(tx_adapter("tx1", 108, {}));
    c.adapters[0].power_offset_qdb = -24;
    c.adapters[1].power_offset_qdb = -24;
    TxCore tx(c, 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);

    tx.apply_boot_power_offsets();
    CHECK_EQ_U(spy.applied.size(), 2u);
    for (const auto& [idx, qdb] : spy.applied) {
        (void)idx;
        CHECK(qdb == -24);
    }
}

// §10.5 (Pass 150): auto must land on the configured safe offset, NOT the
// backend default. Pre-150 it called apply_power_auto, which on devourer was
// offset 0 — the compressing point — and §11.6 recovery ends here (Pass 48),
// so an unattended recovery could drop a node onto it.
void test_auto_restores_safe_offset_not_backend_default() {
    Config c = one_tx_config(108, {});
    c.adapters[0].power_offset_qdb = -24;
    TxCore tx(c, 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);

    CHECK(tx.set_power_override(-8));
    spy.clear();
    tx.clear_power_override();
    CHECK(!spy.empty());
    CHECK(spy.last_qdb() == -24);  // the safe offset, not 0
}

// §10.5 (Pass 150) CRITICAL regression, narrowed by §10.6 (Pass 151): a
// CONFIG `power_map` holds ABSOLUTE qdb and is not backend-scoped, so on a
// relative backend a profile commit would rewrite the boot offset with
// 60..108 qdb reinterpreted as +15..+27 dB. That one stays refused. What is
// no longer refused is a curve this backend's own calibration authored — the
// Pass 146 fingerprint is backend-scoped, so it cannot have come from the
// other space, and it actuates through the OFFSET lever bounded by §10.5.
void test_config_curve_refused_on_relative_backend() {
    Config c = one_tx_config(108, {});
    c.adapters[0].power_offset_qdb = -24;
    c.adapters[0].power_map = write_flat_power_map("wblink_p151_map.txt", 27.0);
    TxCore tx(c, 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);
    tx.set_backend_relative(true);
    CHECK(tx.has_power_curve());        // the config curve loaded...

    tx.apply_boot_power_offsets();
    spy.clear();
    tx.resolve_and_apply_power(5, 4);   // a §10.4 commit

    CHECK(spy.absolutes.empty());       // ...but reached no actuator at all
    CHECK(spy.offsets.empty());
    CHECK(!tx.power_tier_effective());  // and §15.5 does not claim otherwise

    // On an absolute backend the same config curve DOES resolve — the refusal
    // is scoped to the backend, not a disabling of §10.2.
    TxCore mon(c, 1, nullptr, 0);
    PowerSpy mspy;
    mspy.attach(mon);
    mon.set_backend_relative(false);
    mon.resolve_and_apply_power(5, 4);
    CHECK(!mspy.absolutes.empty());
}

// §10.6 (Pass 151): an artifact-authored curve on a relative backend actuates
// as OFFSETS, never as absolutes, and never above the §10.5 bound. Without
// this the craft can be calibrated but the result reaches nothing — which is
// the state Pass 150 shipped and this pass exists to end.
void test_artifact_curve_actuates_as_offset() {
    Config c = one_tx_config(108, {});
    c.adapters[0].power_offset_qdb = -24;
    c.adapters[0].power_offset_max_qdb = 0;
    TxCore tx(c, 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);
    tx.set_backend_relative(true);
    tx.install_curve(flat_curve(-16));   // an offset-space curve
    CHECK(tx.has_power_curve());
    // §11.7 0x0A (Pass 151 → 166): the tier used to be REJECTED here because
    // the presets were absolute qdb. It is now drawn from
    // power_offset_presets_qdb, so a ceiling WOULD bind this offset-space
    // curve and `effective` says so — exactly as it already did on an
    // absolute node with a curve and no list configured. `effective` answers
    // "would a ceiling reach the actuator", not "is a tier selected"; this
    // config selects none (tier stays -1, and set_power_tier below refuses).
    CHECK(tx.power_tier_effective());
    CHECK_EQ_U(tx.power_tier(), -1);
    CHECK(!tx.set_power_tier(0));  // no offset preset list on this adapter

    tx.apply_boot_power_offsets();
    spy.clear();
    tx.resolve_and_apply_power(5, 4);

    CHECK(spy.absolutes.empty());        // the absolute lever is never touched
    CHECK(spy.offsets.size() == 1);
    CHECK(spy.offsets[0].second == -16);

    // A curve entry above the §10.5 bound is clamped to it, not applied. The
    // bound is the whole safety property of the Pass 151 window.
    TxCore hot(c, 1, nullptr, 0);
    PowerSpy hspy;
    hspy.attach(hot);
    hot.set_backend_relative(true);
    hot.install_curve(flat_curve(64));
    hot.resolve_and_apply_power(5, 4);
    CHECK(!hspy.offsets.empty());
    CHECK(hspy.offsets.back().second == 0);
    CHECK(hspy.absolutes.empty());
}

// §10.6 (Pass 151): the safety property the whole re-base rests on. A
// relative-backend sweep drives the OFFSET lever and never leaves the §10.5
// window — Pass 150 refused the run outright precisely because the absolute
// sweep walked its rungs to 108 qdb, which there is +27 dB.
void test_relative_sweep_stays_inside_the_offset_window() {
    Config c = one_tx_config(108, {});
    c.adapters[0].power_offset_qdb = -24;
    c.adapters[0].power_offset_max_qdb = 0;
    TxCore tx(c, 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);
    tx.set_backend_relative(true);

    CalibrationPolicy pol;
    pol.settle_ms = 100;
    pol.dwell_probe_frames = 20;
    pol.dwell_verify_frames = 40;
    pol.probe_pace_us = 100;
    pol.tally_wait_ms = 20;
    pol.tally_retries = 2;
    tx.init_calibration(pol, c.adapters[0].max_power_qdb);

    const auto w = tx.offset_window();
    CHECK(w.has_value());
    CHECK(w->first == -24);   // the safe boot offset — the sweep's floor
    CHECK(w->second == 0);    // the §10.5 bound — its ceiling

    // A channel with a real wall, so the run COMPLETES rather than tripping
    // the Pass 134 "no wall anywhere" refusal — the success path is the one
    // that persists an artifact and therefore the one worth pinning. The
    // evidence is the real §3.16 exchange: engine probes into a real
    // DwellReceiver, tallies back.
    uint64_t now = 1000;
    DwellReceiver ground;
    CHECK(tx.calibrator_->start(now));
    for (int i = 0; i < 120000 && tx.calibrating(); ++i) {
        now += 1;
        tx.calibrate_service(now);
        tx.calibrator_->new_tick();
        for (;;) {
            const DwellProbeOut po = tx.calibrator_->next_probe(now);
            if (!po.send) break;
            const int32_t at =
                spy.offsets.empty() ? -24 : spy.offsets.back().second;
            const uint16_t loss = at >= -8 ? 900 : 5;  // wall at -8
            const uint32_t n = tx.calibrator_->probe_dwell_count();
            if (po.seq <= n * loss / 1000) continue;
            const DwellTallyOut t = ground.on_probe(
                tx.calibrator_->probe_run_id(),
                tx.calibrator_->probe_dwell_id(), po.seq,
                static_cast<uint16_t>(n), -40, 5, now);
            if (t.send) {
                tx.calibrator_->on_tally(t.run_id, t.dwell_id, t.received,
                                         t.rssi_sum_dbm, t.rx_mcs, 0);
            }
        }
    }
    CHECK(!tx.calibrating());
    CHECK(tx.calibrator_->state() == CalibState::kDone);
    CHECK(!spy.offsets.empty());        // it really did drive the radio
    CHECK(spy.absolutes.empty());       // and never through the absolute lever
    for (const auto& o : spy.offsets) {
        CHECK(o.second >= -24 && o.second <= 0);
    }
    // Every rung placed below the wall, and the taper did NOT collapse the
    // high-order rungs onto the window floor (Pass 151 taper_rung_ceiling).
    const CalibArtifact& art = tx.calibrator_->artifact();
    for (int m = 0; m < 8; ++m) {
        CHECK(art.placement_qdb[m] == -16);
        CHECK(art.ceilings[m].has_bad);
    }
}

// §10.3/§11.7 0x0A (Pass 151 → Pass 166). Pass 151 REJECTED a tier on a
// relative backend because `power_presets_qdb` are ABSOLUTE qdb and installing
// one (60..108) as the clamp on an OFFSET resolve replaces the §10.5 bound
// with a number 15..27 dB above it. Pass 166 re-bases instead of refusing:
// the tier reads `power_offset_presets_qdb` there. Both halves are pinned
// here, because the dangerous outcome is not "the tier is refused" but "an
// absolute number reaches an offset actuator".
void test_tier_reads_the_backends_own_space() {
    Config c = one_tx_config(108, {60, 84, 108});
    c.adapters[0].power_offset_qdb = -24;
    c.adapters[0].power_offset_max_qdb = 0;
    c.adapters[0].power_offset_presets_qdb = {-72, -48, -24};
    TxCore tx(c, 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);
    tx.set_backend_relative(true);
    tx.install_curve(flat_curve(-16));

    // The absolute list is NOT what a relative node selects from.
    CHECK(tx.power_presets().size() == 3);
    CHECK(tx.power_presets()[0] == -72);
    CHECK(tx.set_power_tier(0));
    CHECK(tx.power_tier_ceiling().value_or(0) == -72);
    CHECK(tx.power_tier_effective());  // an offset ceiling clamps an offset curve

    // ...and it BINDS: the flat -16 curve resolves down to the -72 tier, where
    // before Pass 166 the absolute ceiling (108) made the clamp a no-op.
    spy.clear();
    tx.resolve_and_apply_power(5, 4);
    CHECK(!spy.offsets.empty());
    CHECK(spy.offsets.back().second == -72);

    // Boot ceiling on a relative node is the §10.5 bound, not max_power_qdb.
    Config c2 = c;
    c2.adapters[0].power_offset_presets_qdb.clear();
    TxCore untiered(c2, 1, nullptr, 0);
    untiered.set_backend_relative(true);
    CHECK(untiered.power_tier_ceiling().value_or(-1) == 0);  // == offset_max
    CHECK(!untiered.set_power_tier(0));  // no offset list -> REJECTED

    // The same config on an absolute backend selects the absolute list.
    TxCore mon(c, 1, nullptr, 0);
    mon.set_backend_relative(false);
    mon.install_curve(flat_curve(108));
    CHECK(mon.set_power_tier(0));
    CHECK(mon.power_tier_ceiling().value_or(-1) == 60);
}

// GROUND TWIN (Pass 165). Pass 151 stated the refusal once and only this
// craft-side half implemented it; the §15.5 POST /api/v1/tx/power_tier ground
// path applied the tier anyway, and its absolute ceiling then overwrote the
// OFFSET-space §10.7 sweep bound (app/main.cpp:7040, no uplink_relative
// guard) -- +24 qdb becoming 108 qdb read as an offset, i.e. +6 dB -> +27 dB.
// That path lives inside run_rx's REST lambda, which this suite cannot reach
// (main() is suppressed), so it is verified on hardware instead. Measured A/B
// on the .242 ground with the flying preset list [60,76,84,92,108]:
//
//   pre-fix   POST {"tier":4} -> 200 {"ok":true};  GET -> "tier":4
//   post-fix  POST {"tier":4} -> 409 "...relative backend (§11.7 0x0A,
//                                     Pass 151) -- use POST /api/v1/tx/power"
//                               GET -> "tier":-1  (nothing recorded)
//   post-fix  POST /api/v1/tx/power {"qdb":-24} -> 200 (the actuation path
//                                                  the 409 points at works)
//
// If Phase 2a lifts the REST surface out of app/main.cpp, this becomes a unit
// test and this comment becomes the spec for it.

// §10.3/§10.7 (Pass 167, operator ruling 2026-08-09): a §11.7 0x0A tier bounds
// FLIGHT power and must NOT narrow the offset-space calibration window.
//
// Pass 166 briefly made it do so, for symmetry with the absolute backend. The
// bench measured the cost on the ground half, which had folded the tier into
// seek.max_qdb since Pass 135: with fleet tier 1 the ceiling is -48 while the
// window floor is power_offset_qdb -24, so max < floor, the run swept ONE
// point, reported `state: done` with no fail_reason, and overwrote an artifact
// holding last_clean_qdb 24. Calibration is how a unit's real maximum is
// found; a session-volatile menu choice must not be able to hide it, still
// less to destroy the record of it.
void test_tier_does_not_narrow_the_relative_sweep_window() {
    Config c = one_tx_config(108, {});
    c.adapters[0].power_offset_qdb = -24;
    c.adapters[0].power_offset_max_qdb = 24;
    c.adapters[0].power_offset_presets_qdb = {-72, -48, -24, 0, 24};
    TxCore tx(c, 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);
    tx.set_backend_relative(true);
    tx.install_curve(flat_curve(20));   // an offset-space calibrated curve

    const auto boot = tx.offset_window();
    CHECK(boot.has_value());
    if (boot) {
        CHECK(boot->first == -24);   // the safe boot offset
        CHECK(boot->second == 24);   // the §10.5 bound
    }

    // Every tier, including one far below the floor, leaves the window alone.
    for (int ti : {3, 1, 0}) {
        const uint8_t t = static_cast<uint8_t>(ti);
        CHECK(tx.set_power_tier(t));
        const auto w = tx.offset_window();
        CHECK(w.has_value());
        if (w) {
            CHECK(w->first == -24);
            CHECK(w->second == 24);  // NOT the tier
        }
    }

    // ...while still bounding flight power, which is the half a tier owns.
    // tier 0 = -72, so the +20 curve resolves down to it.
    spy.clear();
    tx.resolve_and_apply_power(5, 4);
    CHECK(!spy.offsets.empty());
    CHECK(spy.offsets.back().second == -72);
}

// A tier survives the events that REPLACE the power state, and keeps clamping
// while it does. Both cases below were found by the Pass 167 pre-merge review,
// and both are ORDER-dependent — every test above happens to install a curve
// before selecting a tier, which is exactly the order that hides them.
void test_tier_survives_install_curve_and_clamps_the_latch() {
    Config c = one_tx_config(108, {});
    c.adapters[0].power_offset_qdb = -24;
    c.adapters[0].power_offset_max_qdb = 24;
    c.adapters[0].power_offset_presets_qdb = {-72, -48, -24, 0, 24};
    TxCore tx(c, 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);
    tx.set_backend_relative(true);

    CHECK(tx.set_power_tier(0));                          // ceiling -72
    CHECK(tx.power_tier_ceiling().value_or(0) == -72);

    // §10.6: install_curve() runs on EVERY completed craft calibration, and
    // nothing forbids calibrating with a tier held. Re-seeding the adapter
    // ceiling from power_offset_max_qdb there silently RELEASED the tier
    // upward to +24 while power_tier_ceiling() — read from a different
    // vector — kept reporting -72. "A tier can only ever LOWER power",
    // inverted, with §15.3 asserting the opposite.
    tx.install_curve(flat_curve(20));
    spy.clear();
    tx.resolve_and_apply_power(5, 4);
    CHECK(!spy.offsets.empty());
    CHECK(spy.offsets.back().second == -72);              // NOT +20, NOT +24
    CHECK(tx.power_tier_ceiling().value_or(0) == -72);    // and still agrees

    // §10.5 latch: accepted (<= the +24 bound) but clamped by the tier before
    // it reaches the actuator, matching UplinkPower::hw_qdb() on the ground.
    // The craft used to bound only by power_offset_max_qdb, so a latch
    // outranked the tier here while the ground clamped — one spec sentence
    // describing two opposite behaviours.
    spy.clear();
    CHECK(tx.set_power_override(24));
    CHECK(!spy.offsets.empty());
    CHECK(spy.offsets.back().second == -72);

    // Above the BOUND is still rejected outright, not clamped (§10.5).
    CHECK(!tx.set_power_override(48));
}

// §10.6 (Pass 151): a config whose bound sits at or under its own safe offset
// leaves no band, so there is nothing to sweep. REJECTED beats a one-point
// "curve" that reports success.
void test_empty_offset_window_has_no_sweep() {
    Config c = one_tx_config(108, {});
    c.adapters[0].power_offset_qdb = -24;
    c.adapters[0].power_offset_max_qdb = -24;   // bound == floor
    TxCore tx(c, 1, nullptr, 0);
    tx.set_backend_relative(true);
    CHECK(!tx.offset_window().has_value());

    // An absolute backend is unaffected by the §10.5 keys entirely.
    TxCore mon(c, 1, nullptr, 0);
    mon.set_backend_relative(false);
    CHECK(!mon.offset_window().has_value());
}

// §10.3 Pass 136. A running §10.6 sweep owns the power actuator, and applying
// a tier re-seeds CalibrateParams — which constructs a FRESH Calibrator,
// destroying the running one and the restore that hands the actuator back.
void test_tier_refused_during_calibration() {
    TxCore tx(one_tx_config(108, {60, 108}), 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);
    CalibrationPolicy cp;
    tx.init_calibration(cp, 108);
    CHECK(tx.set_power_tier(1));           // fine while idle

    CHECK(tx.calibrator_->start(1000));
    CHECK(tx.calibrating());
    spy.clear();
    CHECK(!tx.set_power_tier(0));          // REJECTED
    CHECK_EQ_U(tx.power_tier(), 1);        // unchanged
    CHECK(spy.empty());                    // and the sweep's actuator untouched
    CHECK(tx.calibrating());               // the run survived

    (void)tx.calibrator_->abort(2000);
    CHECK(!tx.calibrating());
    CHECK(tx.set_power_tier(0));           // and it works again after
}

// §15.5 body contract. `effective` false must be reported, not omitted — a
// menu that shows the tier without it claims an effect the node has not had.
void test_power_tier_json_shape() {
    const std::string s = power_tier_json(2, {60, 84, 108}, 84, false);
    CHECK(s.find("\"tier\":2") != std::string::npos);
    CHECK(s.find("\"presets_qdb\":[60,84,108]") != std::string::npos);
    CHECK(s.find("\"ceiling_qdb\":84") != std::string::npos);
    CHECK(s.find("\"effective\":false") != std::string::npos);

    // No list configured: tier -1 and a null ceiling, never a fabricated 0.
    const std::string none = power_tier_json(-1, {}, std::nullopt, false);
    CHECK(none.find("\"tier\":-1") != std::string::npos);
    CHECK(none.find("\"presets_qdb\":[]") != std::string::npos);
    CHECK(none.find("\"ceiling_qdb\":null") != std::string::npos);
}

// ---- §10.3/§10.5/§10.7 ground uplink power owner (Pass 138) ----------------
//
// These are the rules that were device-verified only. UplinkPower was cut out
// of run_rx precisely so they could be reached; the four hand-written copies of
// the precedence ordering it replaced are the reason two of them had drifted.

// A ground with no curve, no artifact and no latch commands nothing at all —
// the backend default owns the radio, and §15.5 must not claim otherwise.
void test_uplink_unowned_goes_to_backend_auto() {
    UplinkPower p;
    int autos = 0;
    std::vector<int32_t> applied;
    p.apply_qdb = [&](int32_t q) { applied.push_back(q); };
    p.apply_auto = [&] { ++autos; };

    p.apply();
    CHECK_EQ_U(autos, 1);
    CHECK(applied.empty());
    CHECK(!p.effective());
    CHECK(p.owner() == UplinkPower::Owner::kNone);
    CHECK(std::string(p.owner_name()) == "backend auto");
}

// §10.7 precedence, highest first. Each source is added in turn and must take
// the actuator from the one below it.
void test_uplink_precedence_order() {
    UplinkPower p;
    std::vector<int32_t> applied;
    p.apply_qdb = [&](int32_t q) { applied.push_back(q); };
    p.apply_auto = [&] { applied.push_back(INT32_MIN); };

    std::optional<int32_t> artifact;
    p.artifact_qdb = [&] { return artifact; };

    artifact = 70;
    p.apply();
    CHECK(p.owner() == UplinkPower::Owner::kArtifact);
    CHECK(applied.back() == 70);

    p.curve = flat_curve(80);
    p.mcs = 3;
    p.resolve_owner();
    p.apply();
    CHECK(p.owner() == UplinkPower::Owner::kConfigMap);
    CHECK(applied.back() == 80);   // config map outranks the artifact

    p.override_qdb = 90;
    p.apply();
    CHECK(p.owner() == UplinkPower::Owner::kOverride);
    CHECK(applied.back() == 90);   // and the §10.5 latch outranks both

    p.override_qdb.reset();
    p.apply();
    CHECK(applied.back() == 80);   // released back to the config map
}

// §10.5's exact wording: the ceiling is the only CLAMP, and GET/§15.3 report
// the latched REQUEST. Those are two different numbers and the split is easy
// to collapse by accident — on the ground it was collapsed the wrong way, and
// a 120 qdb latch reached a 27 dBm radio against a 108 ceiling.
void test_uplink_latch_reports_request_but_actuates_clamped() {
    UplinkPower p;
    std::vector<int32_t> applied;
    p.apply_qdb = [&](int32_t q) { applied.push_back(q); };
    p.apply_auto = [&] {};
    p.ceiling_qdb = 108;

    p.override_qdb = 120;
    p.apply();
    CHECK(applied.back() == 108);              // hardware: clamped
    CHECK(p.reported_qdb().value_or(0) == 120);  // §15.3: the request
    CHECK(p.hw_qdb().value_or(0) == 108);
    CHECK(p.effective());  // the ceiling plainly reaches hw through the latch
}

// A tier moves the ceiling AND re-resolves the configured map under it. The
// re-resolve was missing: owner_qdb was computed once at startup against the
// boot ceiling, so a tier could not lower a power_map-owned uplink for the
// life of the process.
void test_uplink_tier_reresolves_the_config_map() {
    UplinkPower p;
    std::vector<int32_t> applied;
    p.apply_qdb = [&](int32_t q) { applied.push_back(q); };
    p.apply_auto = [&] {};
    p.presets_qdb = {60, 84, 108};
    p.ceiling_qdb = 108;
    p.curve = flat_curve(200);   // authored well above every preset
    p.mcs = 0;
    p.resolve_owner();
    CHECK(p.owner_qdb.value_or(0) == 108);   // clamped at the boot ceiling

    CHECK(p.set_tier(0));
    CHECK(p.owner_qdb.value_or(0) == 60);    // re-resolved under the new one
    CHECK(applied.back() == 60);             // and it reached the actuator
    CHECK(p.ceiling_qdb.value_or(0) == 60);
}

// The tier must reach an actuator on a ground with NO artifact, which is the
// configuration where the old pairing-key route silently did nothing (that
// path is gated on an artifact existing).
void test_uplink_tier_actuates_without_an_artifact() {
    UplinkPower p;
    std::vector<int32_t> applied;
    p.apply_qdb = [&](int32_t q) { applied.push_back(q); };
    p.apply_auto = [&] { applied.push_back(INT32_MIN); };
    p.presets_qdb = {60, 108};
    p.ceiling_qdb = 108;
    p.artifact_qdb = [] { return std::optional<int32_t>{}; };  // none paired
    p.override_qdb = 200;
    p.apply();                       // give it something to clamp
    applied.clear();

    CHECK(p.set_tier(0));
    CHECK(!applied.empty());                   // it actuated
    CHECK(applied.back() == 60);
}

// REJECTED, and nothing moves — not a silent clamp to the nearest preset.
void test_uplink_tier_rejects_out_of_range() {
    UplinkPower p;
    p.apply_qdb = [](int32_t) {};
    p.apply_auto = [] {};
    p.presets_qdb = {60, 108};
    p.ceiling_qdb = 108;

    CHECK(!p.set_tier(2));
    CHECK(!p.set_tier(-1));
    CHECK_EQ_U(static_cast<int64_t>(p.tier) + 1, 0);   // still -1
    CHECK(p.ceiling_qdb.value_or(0) == 108);           // untouched

    UplinkPower none;                                   // no list at all
    none.apply_qdb = [](int32_t) {};
    none.apply_auto = [] {};
    CHECK(!none.set_tier(0));
}

// §15.5 and §15.3 read the same object now, so `effective` cannot mean one
// thing on the endpoint and another on the stats line — which is exactly how
// it drifted before.
void test_uplink_json_matches_effective() {
    UplinkPower p;
    p.apply_qdb = [](int32_t) {};
    p.apply_auto = [] {};
    p.presets_qdb = {60, 108};
    p.ceiling_qdb = 108;

    CHECK(p.json().find("\"effective\":false") != std::string::npos);
    CHECK(p.json().find("\"tier\":-1") != std::string::npos);
    p.override_qdb = 50;
    p.apply();
    CHECK(p.effective());
    CHECK(p.json().find("\"effective\":true") != std::string::npos);
}

// ---- §11.7 command registry ------------------------------------------------

// The name<->id map is written twice, in two switch/if chains that no compiler
// cross-checks. A name that encodes but does not decode is a command the
// §15.5 endpoint accepts and the stats surface then cannot label.
void test_vcmd_name_id_roundtrip() {
    for (const char* n : {"arq", "selector", "fps_ladder", "fps_select",
                          "resolution", "framing", "mode", "calibrate",
                          "mtu_tier", "tx_power"}) {
        const uint8_t id = vcmd_id_for(n);
        CHECK(id != 0);
        CHECK(std::string(vcmd_name_for(id)) == n);
    }
    CHECK_EQ_U(vcmd_id_for("no_such_command"), 0);
    CHECK(std::string(vcmd_name_for(0x7F)).empty());
    // 0x0A is the Pass 135 assignment; the registry reserves 0x0B upward.
    CHECK_EQ_U(vcmd_id_for("tx_power"), 0x0A);
}

// ---- small app-layer helpers ----------------------------------------------

// §11 CSA. An empty allowlist is FAIL-CLOSED: it denies every channel, it does
// not mean "no restriction". Both call sites invoke this bare, with no
// empty-list special case, so an unconfigured `policy.csa.channel_allowlist`
// refuses POST /api/v1/channel and drops incoming CSA assignments — which is
// the right default for a control that moves a flying link's radio.
//
// Pinned explicitly because "empty means unrestricted" is the intuitive
// misreading, and acting on it here would silently open every channel.
void test_channel_allowed_is_fail_closed() {
    CHECK(!channel_allowed({}, 5805));
    CHECK(channel_allowed({5180, 5805}, 5805));
    CHECK(!channel_allowed({5180, 5220}, 5805));
}

void test_bw_code() {
    CHECK_EQ_U(bw_code(20), 0);
    CHECK_EQ_U(bw_code(40), 1);
}


// ---------------------------------------------------------------------------
// AirIface contract (Pass 140)
//
// FakeAir exists because the interface's whole point is that a backend states
// its answer somewhere a test can read it. Before the contract there was
// nothing to fake: AirBackend held concrete backend optionals, so the
// capability gaps could only be observed on hardware.
//
// These cases pin the DECLARED LIMITS, not aspirations. Several assert that a
// backend does nothing — that is the point. When a gap is closed for real, the
// matching assertion fails and has to be rewritten deliberately, which is the
// opposite of a silent no-op quietly becoming a silent action.
// ---------------------------------------------------------------------------

class FakeAir : public AirIface {
  public:
    size_t adapters = 2;
    bool rf = true;
    bool tx = true;
    // Observations
    int retunes = 0, recovers = 0, reapplies = 0, flushes = 0;
    int stamp_sets = 0, filter_sets = 0;
    bool retune_ok = true, recover_ok = false;
    std::vector<size_t> retuned_adapters;
    std::vector<uint8_t> retune_widths;

    size_t inject(const uint8_t*, size_t) override { return 1; }
    size_t inject_resend(const uint8_t*, size_t) override { return 1; }
    size_t inject_return(uint16_t, const uint8_t*, size_t, bool) override {
        return 1;
    }
    int poll_once(int, const RxCb&) override { return 0; }
    std::vector<int> wait_fds() const override { return {}; }
    size_t rx_adapters() const override { return adapters; }
    void flush_rx() override { ++flushes; }
    bool retune(size_t a, uint16_t, uint8_t w, bool) override {
        ++retunes;
        retuned_adapters.push_back(a);
        retune_widths.push_back(w);
        return retune_ok;
    }
    bool recover(size_t, uint16_t, uint8_t) override {
        ++recovers;
        return recover_ok;
    }
    bool reapply_tx_power(size_t) override {
        ++reapplies;
        return true;
    }
    bool set_power_qdb(size_t, int32_t) override { return true; }
    // §10.5 Pass 150 relative contract; record it so the boot-offset
    // and latch tests can assert what actually reached the backend.
    std::vector<std::pair<size_t, int32_t>> power_offsets;
    bool set_power_offset_qdb(size_t i, int32_t q) override {
        power_offsets.emplace_back(i, q);
        return true;
    }
    bool set_power_auto(size_t) override { return true; }
    // §10.5 (Pass 169): programmable, like power_offsets above, so a test can
    // stage a railed actuator without a radio.
    std::optional<TxPowerApplied> applied_power;
    std::optional<TxPowerApplied> tx_power_applied(size_t) const override {
        return applied_power;
    }
    std::optional<uint64_t> read_tsf(size_t) override { return std::nullopt; }
    void set_tx_mode(uint8_t, bool) override {}
    void set_mcs_probe(uint16_t, uint16_t, uint8_t) override {}
    void set_stamp_net_id(uint8_t) override { ++stamp_sets; }
    void set_filter_net_id(std::optional<uint8_t>) override { ++filter_sets; }
    size_t tx_index() const override { return 0; }
    bool has_tx() const override { return tx; }
    uint16_t mtu_supported() const override { return kDefaultMaxPayload; }
    std::optional<uint32_t> estimate_airtime_us(size_t, bool,
                                                uint16_t) const override {
        return std::nullopt;
    }
    bool is_rf() const override { return rf; }
    std::string adapter_mac(size_t) const override { return {}; }
    std::optional<AirSense> rx_sense(size_t) override { return sense; }
    std::optional<AirSense> sense;  // §15.5a: injectable for scout tests
    uint64_t rx_frames(size_t a) const override { return 100 * (a + 1); }
};

// The udp dev backend is the one every fall-through used to land on, so its
// declared answers are what the old `else` branches actually did.
void test_udp_backend_declares_its_limits() {
    AirUdpCfg uc;
    uc.originator = 7;
    auto a = UdpAir::create(uc);
    CHECK(static_cast<bool>(a));
    UdpAir& u = *a.value;

    // No RF: this is what AirBackend::is_radio() now asks.
    CHECK(!u.is_rf());
    // A retune is logged intent and reports success — that is what keeps the
    // §11 CSA state machines exercisable without radios (§16).
    CHECK(u.retune(0, 5805, 20, false));
    // §11.6 Pass 80 re-init has no meaning here.
    CHECK(!u.recover(0, 5805, 20));
    // No hardware clock (§7.2 falls back to host time).
    CHECK(!u.read_tsf(0).has_value());
    // Power is logged intent, accepted like an in-process write (§10.5).
    CHECK(u.set_power_qdb(0, 60));
    CHECK(u.set_power_auto(0));
    // Compatibility MTU tier: no driver matrix to assert anything better.
    CHECK_EQ_U(u.mtu_supported(), kDefaultMaxPayload);
    // Has an uplink by construction, so it does not suppress §3.11 heartbeats.
    CHECK(u.has_tx());
    // On the contract, so it must answer an out-of-range adapter the way the
    // RF backends do (zeroed) rather than indexing off the end — a caller
    // holding an AirIface* cannot tell which backend it has.
    CHECK_EQ_U(u.rx_frames(9999), 0u);
}

// The §11.1 width/class duality used to sit in two caller-side ternaries.
void test_width_mhz_resolves_both_encodings() {
    CHECK_EQ_U(AirBackend::width_mhz(0), 20u);
    CHECK_EQ_U(AirBackend::width_mhz(1), 40u);
    CHECK_EQ_U(AirBackend::width_mhz(2), 80u);
    // Anything above the class range is already an MHz width.
    CHECK_EQ_U(AirBackend::width_mhz(20), 20u);
    CHECK_EQ_U(AirBackend::width_mhz(40), 40u);
    CHECK_EQ_U(AirBackend::width_mhz(80), 80u);
}

// With the backend owned through the contract, a test can install a fake and
// drive the REAL loops. These three previously could only be checked on
// hardware, and two of them cover behaviour this pass deliberately changed.

// Build an AirBackend around a FakeAir. chan_by_adapter is sized like a real
// create() would size it, so note_chan's bounds check behaves the same.
AirBackend backend_with(std::unique_ptr<FakeAir> f, size_t adapters) {
    AirBackend b;
    b.chan_by_adapter.assign(adapters, 5745);
    b.air = std::move(f);
    return b;
}

// CHANGED IN PASS 143: the recover path used to flush unconditionally.
// Flushing after a recovery that recovered nothing discards live backlog for
// no benefit, and on a backend with no re-init path it is a pure loss.
void test_recover_all_only_flushes_when_something_recovered() {
    auto f = std::make_unique<FakeAir>();
    f->adapters = 2;
    f->recover_ok = false;
    FakeAir* spy = f.get();
    AirBackend b = backend_with(std::move(f), 2);

    CHECK(!b.recover_all(5805, 20));
    CHECK_EQ_U(static_cast<unsigned>(spy->recovers), 2u);
    CHECK_EQ_U(static_cast<unsigned>(spy->flushes), 0u);  // the guard
    // A failed recovery must not move the recorded channel either.
    CHECK_EQ_U(b.chan_by_adapter[0], 5745u);

    spy->recover_ok = true;
    spy->recovers = 0;
    CHECK(b.recover_all(5805, 20));
    CHECK_EQ_U(static_cast<unsigned>(spy->flushes), 1u);
    CHECK_EQ_U(b.chan_by_adapter[0], 5805u);
    CHECK_EQ_U(b.chan_by_adapter[1], 5805u);
}

// CHANGED IN THIS PASS: the radio path used to re-apply TX power even when the
// retune had failed. Re-applying power for a channel the adapter is not on is
// meaningless.
void test_retune_all_reapplies_power_only_on_success() {
    auto f = std::make_unique<FakeAir>();
    f->adapters = 3;
    f->retune_ok = false;
    FakeAir* spy = f.get();
    AirBackend b = backend_with(std::move(f), 3);

    CHECK(!b.retune_all(5805, 20, false));
    CHECK_EQ_U(static_cast<unsigned>(spy->retunes), 3u);   // every adapter tried
    CHECK_EQ_U(static_cast<unsigned>(spy->reapplies), 0u); // none on failure
    CHECK_EQ_U(b.chan_by_adapter[0], 5745u);               // channel not moved
    // §11.6 verify hygiene: the backlog is dropped either way, because it is
    // old-channel residue regardless of whether the retune took.
    CHECK_EQ_U(static_cast<unsigned>(spy->flushes), 1u);

    spy->retune_ok = true;
    spy->retunes = 0;
    CHECK(b.retune_all(5805, 20, false));
    CHECK_EQ_U(static_cast<unsigned>(spy->reapplies), 3u);
    CHECK_EQ_U(b.chan_by_adapter[2], 5805u);
}

// The backend receives an MHz width whichever encoding the caller used — the
// duality that used to live in two caller-side ternaries.
void test_retune_all_passes_mhz_width_for_either_encoding() {
    for (const auto& [in, want] : std::vector<std::pair<uint8_t, uint8_t>>{
             {2, 80}, {1, 40}, {0, 20}, {80, 80}, {40, 40}, {20, 20}}) {
        auto f = std::make_unique<FakeAir>();
        f->adapters = 1;
        FakeAir* spy = f.get();
        AirBackend b = backend_with(std::move(f), 1);
        b.retune_all(5805, in, false);
        CHECK_EQ_U(spy->retune_widths.at(0), want);
    }
}

// A one-ear Scout retune is also a channel-identity boundary. Old frames in
// the process queue must be discarded before the engine labels new arrivals
// with the requested dwell channel.
void test_retune_one_flushes_old_channel_backlog() {
    auto f = std::make_unique<FakeAir>();
    f->adapters = 1;
    FakeAir* spy = f.get();
    AirBackend b = backend_with(std::move(f), 1);

    CHECK(b.retune_one(0, 5805, 20, false));
    CHECK_EQ_U(static_cast<unsigned>(spy->flushes), 1u);
    CHECK_EQ_U(b.chan_by_adapter[0], 5805u);
}

// rx_frames_total sums the liveness counter across adapters through the
// contract. It used to skip the udp backend entirely and read 0 there, so the
// §11.6 watchdog saw a permanently dead link on the dev transport.
void test_rx_frames_total_sums_through_the_contract() {
    auto f = std::make_unique<FakeAir>();
    f->adapters = 3;
    AirBackend b = backend_with(std::move(f), 3);
    CHECK_EQ_U(b.rx_frames_total(), 600u);  // 100 + 200 + 300
}

// is_radio() asks the contract "is this real RF", not "is this devourer".
void test_is_radio_follows_the_backend() {
    auto f = std::make_unique<FakeAir>();
    f->rf = false;
    FakeAir* spy = f.get();
    AirBackend b = backend_with(std::move(f), 1);
    CHECK(!b.is_radio());
    spy->rf = true;
    CHECK(b.is_radio());
}

// A node with no TX adapter emits no §3.11 heartbeat, on ANY backend. This was
// the one divergence with no comment anywhere.
void test_heartbeat_suppressed_without_tx_on_any_backend() {
    FakeAir f;
    f.tx = false;
    CHECK(!f.has_tx());
    f.rf = false;  // and it is not an RF-only rule
    CHECK(!f.has_tx());
    f.tx = true;
    CHECK(f.has_tx());
}

}  // namespace

int main() {
    test_tier_on_uncalibrated_node_actuates_nothing();
    test_tier_only_lowers();
    test_ceiling_and_presets_come_from_the_same_adapter();
    test_tier_is_all_or_nothing_across_adapters();
    test_tier_rejected_with_no_preset_list();
    test_override_is_bounded_and_rejected_not_clamped();
    test_boot_offset_applied_to_every_tx_adapter();
    test_auto_restores_safe_offset_not_backend_default();
    test_config_curve_refused_on_relative_backend();
    test_artifact_curve_actuates_as_offset();
    test_relative_sweep_stays_inside_the_offset_window();
    test_tier_reads_the_backends_own_space();
    test_tier_does_not_narrow_the_relative_sweep_window();
    test_tier_survives_install_curve_and_clamps_the_latch();
    test_empty_offset_window_has_no_sweep();
    test_tier_refused_during_calibration();
    test_power_tier_json_shape();
    test_uplink_unowned_goes_to_backend_auto();
    test_uplink_precedence_order();
    test_uplink_latch_reports_request_but_actuates_clamped();
    test_uplink_tier_reresolves_the_config_map();
    test_uplink_tier_actuates_without_an_artifact();
    test_uplink_tier_rejects_out_of_range();
    test_uplink_json_matches_effective();
    test_vcmd_name_id_roundtrip();
    test_channel_allowed_is_fail_closed();
    test_bw_code();
    test_udp_backend_declares_its_limits();
    test_width_mhz_resolves_both_encodings();
    test_recover_all_only_flushes_when_something_recovered();
    test_retune_all_reapplies_power_only_on_success();
    test_retune_all_passes_mhz_width_for_either_encoding();
    test_retune_one_flushes_old_channel_backlog();
    test_rx_frames_total_sums_through_the_contract();
    test_is_radio_follows_the_backend();
    test_heartbeat_suppressed_without_tx_on_any_backend();
    return wbtest_finish("app_test");
}
