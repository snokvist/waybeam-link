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
// run_rx() is a single 2.3k-line function whose state lives in locals captured
// by lambdas, so none of it was reachable from here. Its §10.3/§10.5/§10.7
// power ownership is now UplinkPower, a struct at namespace scope with
// injected actuators — the same shape TxCore already had — and the ground-side
// rules below are covered rather than device-verified only.
//
// What this still does NOT reach: everything else in run_rx — selection, CSA,
// the cache-repair controller, the calibration sequencer. Each is its own seam.
#define WBLINK_APP_TEST 1
#include "app/main.cpp"

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

// Records what actually reached the actuator, which is the only thing that
// matters for a power control — GET reporting the right number while the radio
// sits somewhere else is exactly the class of bug this file exists for.
struct PowerSpy {
    std::vector<std::pair<size_t, int32_t>> applied;
    int autos = 0;
    void attach(TxCore& tx) {
        tx.apply_power = [this](size_t idx, int32_t qdb) {
            applied.emplace_back(idx, qdb);
            return true;
        };
        tx.apply_power_auto = [this](size_t) { ++autos; };
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
    void clear() { applied.clear(); autos = 0; }
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

// §10.5 says the §10.3 ceiling is the ONE clamp on an override. A lowered tier
// must therefore re-assert a held latch through the NEW clamp; skipping that
// left the hardware on the old, higher value — a safety control that visibly
// did nothing, which is the worst possible outcome for one.
void test_lowered_tier_reasserts_a_held_override() {
    TxCore tx(one_tx_config(108, {60, 108}), 1, nullptr, 0);
    PowerSpy spy;
    spy.attach(tx);

    tx.set_power_override(120);            // above every ceiling
    CHECK(!spy.empty());
    CHECK(spy.last_qdb() == 108);          // clamped at the boot ceiling
    spy.clear();

    CHECK(tx.set_power_tier(0));           // ceiling 60
    CHECK(!spy.empty());                   // the latch was re-applied...
    CHECK(spy.last_qdb() == 60);           // ...through the NEW clamp

    // And §15.5 `effective` counts a held latch: the ceiling plainly reaches
    // hardware through it, even on a node with no curve.
    CHECK(tx.power_tier_effective());
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

}  // namespace

int main() {
    test_tier_on_uncalibrated_node_actuates_nothing();
    test_tier_only_lowers();
    test_ceiling_and_presets_come_from_the_same_adapter();
    test_tier_is_all_or_nothing_across_adapters();
    test_tier_rejected_with_no_preset_list();
    test_lowered_tier_reasserts_a_held_override();
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
    return wbtest_finish("app_test");
}
