// SPDX-License-Identifier: GPL-2.0-or-later
// UplinkPower, reached from ONE #include (#109 Phase 2c).
//
// This struct is the §10.3/§10.5/§10.7/§11.7 0x0A precedence chain for the
// GROUND's uplink. Until this move the only way to reach it was app_test's
// whole-TU `#include "app/main.cpp"`.
//
// It is the ground-side twin of a chain that has already bitten twice. The
// two order-dependent tier defects found during Pass 166/167 verification —
// `install_curve()` re-seeding the adapter ceiling, and a craft latch
// outranking the tier — are `TxCore`'s (`node/tx_core.h`), not this struct's,
// and stay pinned where they were found, in `tests/app_test.cpp`. What
// carries over is the SHAPE of the mistake: both were invisible because every
// existing case installs a curve BEFORE selecting a tier, and this struct has
// the same two operations and the same never-exercised ordering.
//
// So `test_tier_relowers_a_configured_map` runs them both ways round.
#include "wblink/node/uplink_power.h"

#include <string>

#include "wblink/node/stats_fill.h"  // §15.5 build_tx_power_json (Pass 169)
#include "wbtest.h"

namespace {

using wblink::node::UplinkPower;
using wblink::node::build_tx_power_json;

// A curve with one authored level-4 point per MCS, so resolve_power_qdb has
// something to return and the ceiling has something to clamp.
wblink::PowerCurve flat_curve(int32_t qdb) {
    wblink::PowerCurve c;
    for (size_t mcs = 0; mcs < c.qdb.size(); ++mcs) {
        c.qdb[mcs] = qdb;
    }
    c.valid = true;  // resolve_power_qdb returns nullopt without this
    return c;
}

// §10.5 is explicit that a latch REPORTS the request and ACTUATES the clamped
// value. Reporting the clamp would tell an operator their command was obeyed
// when the ceiling ate it; actuating the request would drive past the ceiling.
// The two accessors exist to keep those apart, so they are pinned together.
void test_latch_reports_request_and_actuates_clamped() {
    UplinkPower p;
    p.ceiling_qdb = 40;
    p.override_qdb = 96;
    CHECK(p.owner() == UplinkPower::Owner::kOverride);
    CHECK(p.reported_qdb().has_value() && *p.reported_qdb() == 96);
    CHECK(p.hw_qdb().has_value() && *p.hw_qdb() == 40);
    // No ceiling: the request IS the actuation. A missing bound must not read
    // as a bound of zero.
    p.ceiling_qdb.reset();
    CHECK(*p.hw_qdb() == 96);
}

// §11.7 0x0A rejects rather than clamps. An out-of-range index is an operator
// error, and silently landing on the nearest tier would move the ceiling to a
// value nobody asked for.
void test_set_tier_rejects_out_of_range() {
    UplinkPower p;
    CHECK(!p.set_tier(0));      // no list at all
    CHECK_EQ_U(p.tier, -1);
    p.presets_qdb = {-72, -48, -24, 0, 24};
    // A negative index is already caught by the size comparison — the cast to
    // size_t wraps it past the end — so this pins the OUTCOME, not the `t < 0`
    // guard, which is belt-and-braces and survives being deleted.
    CHECK(!p.set_tier(-1));
    CHECK(!p.set_tier(5));      // one past the end
    CHECK_EQ_U(p.tier, -1);
    CHECK(p.set_tier(4));       // the top IS selectable — it releases the clamp
    CHECK_EQ_U(p.tier, 4);
    CHECK(*p.ceiling_qdb == 24);
}

// Pass 136: a tier must re-resolve the configured map under the NEW ceiling.
// Resolving once at boot pinned the boot ceiling for the life of the process,
// so a tier could not lower a power_map-owned uplink — it moved the number the
// clamp read while the clamped value stayed where it was.
//
// Deliberately in tier-then-curve order as well as curve-then-tier, because
// only one of those two orders was ever exercised before.
void test_tier_relowers_a_configured_map() {
    UplinkPower p;
    p.mcs = 1;
    p.presets_qdb = {-72, -24, 24};
    p.curve = flat_curve(24);
    p.resolve_owner();                       // boot resolve, no ceiling yet
    CHECK(p.owner() == UplinkPower::Owner::kConfigMap);
    const int32_t boot = *p.reported_qdb();
    CHECK(p.set_tier(0));                    // ceiling drops to -72
    CHECK(*p.reported_qdb() < boot);         // the map followed it down
    CHECK(*p.reported_qdb() <= -72);

    // Same two operations, opposite order: select first, then install.
    UplinkPower q;
    q.mcs = 1;
    q.presets_qdb = {-72, -24, 24};
    CHECK(q.set_tier(0));                    // tier with no curve yet
    q.curve = flat_curve(24);
    q.resolve_owner();
    CHECK(*q.reported_qdb() == *p.reported_qdb());
}

// §10.3 Pass 134/136: `effective` says whether a number of OURS reaches the
// actuator. A tier on a node with no curve, no artifact and no latch is
// recorded and moves nothing — reporting it as effective is the exact claim
// Pass 136 had to withdraw.
void test_effective_follows_the_owner_not_the_tier() {
    UplinkPower p;
    p.presets_qdb = {-24, 0};
    CHECK(p.set_tier(1));
    CHECK_EQ_U(p.tier, 1);
    CHECK(!p.effective());                   // tier set, nothing actuated
    CHECK(p.owner() == UplinkPower::Owner::kNone);
    p.override_qdb = 12;
    CHECK(p.effective());
}

// Precedence is latch > configured map > artifact > backend auto, and the
// artifact callback must not run while a higher source holds the uplink: it
// re-runs the §10.7 pairing check and updates the stale flag as a side effect,
// so a spurious call is a spurious state change, not just wasted work.
void test_artifact_is_not_consulted_above_its_rank() {
    int artifact_calls = 0;
    UplinkPower p;
    p.artifact_qdb = [&]() -> std::optional<int32_t> {
        ++artifact_calls;
        return 8;
    };
    CHECK(p.owner() == UplinkPower::Owner::kArtifact);
    CHECK(*p.reported_qdb() == 8);
    const int after_artifact = artifact_calls;
    CHECK(after_artifact > 0);

    p.override_qdb = 20;                     // §10.5 outranks it
    CHECK(p.owner() == UplinkPower::Owner::kOverride);
    CHECK_EQ_U(artifact_calls, after_artifact);
}

// The middle rank of the chain, which the cases above skip past: a configured
// map outranks an artifact. Both are "a number we computed" rather than a
// live operator command, so nothing about the two makes the order obvious —
// it has to be asserted or a swap of the two branches goes unnoticed.
void test_config_map_outranks_the_artifact() {
    int artifact_calls = 0;
    UplinkPower p;
    p.mcs = 1;
    p.artifact_qdb = [&]() -> std::optional<int32_t> {
        ++artifact_calls;
        return -40;
    };
    CHECK(p.owner() == UplinkPower::Owner::kArtifact);

    p.curve = flat_curve(12);
    p.resolve_owner();
    CHECK(p.owner() == UplinkPower::Owner::kConfigMap);
    CHECK(*p.reported_qdb() == 12);
    const int settled = artifact_calls;
    CHECK(p.reported_qdb().has_value());
    CHECK_EQ_U(artifact_calls, settled);     // and the artifact stays unread
}

// apply() is the single convergence point: a resolved value reaches apply_qdb,
// and NOTHING resolved falls through to apply_auto rather than to silence.
// Backend auto is a real state, not the absence of one.
void test_apply_converges_on_one_actuator() {
    std::optional<int32_t> applied;
    int autos = 0;
    UplinkPower p;
    p.apply_qdb = [&](int32_t q) { applied = q; };
    p.apply_auto = [&]() { ++autos; };

    p.apply();                               // nothing owns it
    CHECK(!applied.has_value());
    CHECK_EQ_U(autos, 1);

    p.ceiling_qdb = 16;
    p.override_qdb = 64;
    p.apply();
    CHECK(applied.has_value() && *applied == 16);   // clamped, per §10.5
    CHECK_EQ_U(autos, 1);                            // and auto NOT re-run
}

// §15.5 GET /api/v1/tx/power (Pass 169). The latch says what was ASKED for;
// `applied_qdb` says what the chip took. They diverge whenever the backend's
// TXAGC index rails — measured 2026-08-14, where a craft swallowed 18 dB of
// commanded range behind a 200 OK and nothing in the response disagreed.
void test_tx_power_json_reports_the_applied_value_and_the_rail() {
    // Nothing written yet: the applied fields are ABSENT, not zero, so a
    // reader can tell "never applied" from "applied 0".
    CHECK(build_tx_power_json(std::nullopt, std::nullopt, true) ==
          "{\"override_active\":false,\"backend\":\"radio\"}");

    // Applied == requested: still reported, and explicitly not railed — a
    // consumer must not have to infer health from an absent field.
    const std::string ok = build_tx_power_json(
        -24, wblink::AirIface::TxPowerApplied{-24, false, false}, true);
    CHECK(ok.find("\"applied_qdb\":-24") != std::string::npos);
    CHECK(ok.find("\"saturated_low\":false") != std::string::npos);

    // The measured failure: -120 qdb commanded, the chip took -48 and railed.
    // Both numbers must survive, because the pair is the whole finding.
    const std::string railed = build_tx_power_json(
        -120, wblink::AirIface::TxPowerApplied{-48, true, false}, true);
    CHECK(railed.find("\"qdb\":-120") != std::string::npos);
    CHECK(railed.find("\"applied_qdb\":-48") != std::string::npos);
    CHECK(railed.find("\"saturated_low\":true") != std::string::npos);
}

// A backend with no actuator reports no applied value rather than echoing the
// request back as if the chip had confirmed it.
void test_tx_power_json_omits_applied_where_there_is_no_actuator() {
    const std::string udp = build_tx_power_json(20, std::nullopt, false);
    CHECK(udp.find("applied_qdb") == std::string::npos);
    CHECK(udp.find("\"saturated_low\"") == std::string::npos);
    CHECK(udp.find("\"backend\":\"udp\"") != std::string::npos);
    // ...and it names no actuator either: `actuator` describes a radio
    // adapter's chip, and the udp bench has no chip to describe.
    CHECK(udp.find("\"actuator\"") == std::string::npos);
}

// §10.5 (Pass 171). A RADIO adapter whose chip has no lever is the case the
// three fields above cannot express: devourer answers an unsupported knob with
// 0, so `applied_qdb:0, saturated_low:false` is byte-identical to a successful
// zero-offset apply — the exact surface the RTL8733BU produced on 2026-08-14
// while 18 dB of commanded offset aired nothing. `actuator` has to be a stated
// value, because omission is already taken: it means "no write yet".
void test_tx_power_json_states_whether_an_actuator_exists_at_all() {
    // No actuator: said out loud, and the three that would lie are gone.
    const std::string none = build_tx_power_json(
        -24, wblink::AirIface::TxPowerApplied{0, false, false, false}, true);
    CHECK(none.find("\"actuator\":\"none\"") != std::string::npos);
    CHECK(none.find("applied_qdb") == std::string::npos);
    CHECK(none.find("\"saturated_low\"") == std::string::npos);
    CHECK(none.find("\"saturated_high\"") == std::string::npos);
    // The latched request still reports — the operator asked for it, and the
    // point of the refusal is that they learn it did not land.
    CHECK(none.find("\"qdb\":-24") != std::string::npos);

    // A working actuator says so, so a consumer never has to read "actuator
    // absent" as "actuator fine". This is the half that would make deleting
    // the field look harmless if only the `none` case were pinned.
    const std::string offset = build_tx_power_json(
        -24, wblink::AirIface::TxPowerApplied{-24, false, false, true}, true);
    CHECK(offset.find("\"actuator\":\"offset\"") != std::string::npos);
    CHECK(offset.find("\"applied_qdb\":-24") != std::string::npos);

    // And "no write yet" stays distinguishable from both: nullopt names no
    // actuator at all, so the three-way distinction the field exists for
    // (unknown / none / working) survives.
    const std::string unwritten = build_tx_power_json(-24, std::nullopt, true);
    CHECK(unwritten.find("\"actuator\"") == std::string::npos);
    CHECK(unwritten.find("applied_qdb") == std::string::npos);
}

}  // namespace

// §15.5 (Pass 172). The adapters[] array states the per-die capability
// answers on every entry — and with NO backend yet (an /info served before
// bring-up, or the C snapshot's not-ready window) the fields still appear,
// carrying the stated defaults rather than vanishing: absence is already
// taken by pre-Pass-172 payloads, so a reader must never have to infer a
// capability from a missing key.
void test_adapters_array_states_caps_on_every_entry() {
    wblink::node::Loaded l;
    wblink::AdapterCfg a;
    a.name = "gnd0";
    a.role = wblink::Role::kTx;
    a.channel_mhz = 5805;
    l.cfg.adapters.push_back(a);

    const std::string no_air = wblink::node::build_adapters_array(l, nullptr);
    CHECK(no_air.find("\"name\":\"gnd0\"") != std::string::npos);
    CHECK(no_air.find("\"role\":\"tx\"") != std::string::npos);
    CHECK(no_air.find("\"channel\":5805") != std::string::npos);
    CHECK(no_air.find("\"mac\":null") != std::string::npos);
    CHECK(no_air.find("\"chip\":\"unknown\"") != std::string::npos);
    CHECK(no_air.find("\"power_actuator\":false") != std::string::npos);
    CHECK(no_air.find("\"ldpc_rx_flag\":false") != std::string::npos);
    CHECK(no_air.find("\"fastretune\":false") != std::string::npos);
}

int main() {
    test_latch_reports_request_and_actuates_clamped();
    test_adapters_array_states_caps_on_every_entry();
    test_set_tier_rejects_out_of_range();
    test_tier_relowers_a_configured_map();
    test_effective_follows_the_owner_not_the_tier();
    test_artifact_is_not_consulted_above_its_rank();
    test_config_map_outranks_the_artifact();
    test_apply_converges_on_one_actuator();
    test_tx_power_json_reports_the_applied_value_and_the_rail();
    test_tx_power_json_omits_applied_where_there_is_no_actuator();
    test_tx_power_json_states_whether_an_actuator_exists_at_all();
    return wbtest_finish("node_uplink_power_test");
}
