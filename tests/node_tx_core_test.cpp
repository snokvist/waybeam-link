// SPDX-License-Identifier: GPL-2.0-or-later
// TxCore, reached directly (#109 Phase 2a).
//
// The §10.3/§11.7 0x0A power-tier rules already have thorough coverage in
// tests/app_test.cpp, which reaches TxCore by `#include`-ing the whole of
// app/main.cpp. Those cases stay where they are: they are the pins for the two
// order-dependent tier defects Pass 166/167 verification found, and moving a
// regression case away from the fixtures it was written against buys nothing.
// (UplinkPower — the GROUND-side twin of that chain, and a different struct —
// moved out in Phase 2c and has its own suite, node_uplink_power_test.cpp.)
//
// What this file adds is the thing app_test cannot assert: that TxCore is
// reachable from ONE header with no app layer at all. It pins the §15.2 ->
// core policy adapters that moved with it, because those are pure functions
// that nothing had ever tested directly, and a constructed TxCore to prove the
// layering holds.
#include "wblink/node/tx_core.h"

#include <string>

#include "wbtest.h"

namespace {

using wblink::Config;
using wblink::node::bw_code;
using wblink::node::s_to_ms;
using wblink::node::selector_policy;
using wblink::node::TxCore;

Config tx_config() {
    Config c;
    c.node.originator = 17;
    return c;
}

// A TxCore builds from a Config alone — no radio, no socket, no profile table.
// `table` is a nullable pointer by design: a node with no §9.3 table still
// runs, it just cannot resolve an operating point.
void test_constructs_without_a_profile_table() {
    TxCore tx(tx_config(), 12345, nullptr, 0);
    CHECK_EQ_U(tx.power_tier(), -1);        // §11.7 0x0A unset
    CHECK(!tx.backend_relative());          // set later, by run_*
    CHECK(!tx.has_power_curve());
    CHECK(!tx.calibrating());
}

// §15.2 seconds -> milliseconds. Every `*_s` knob in policy.select goes
// through this, so its rounding IS the config contract for a dozen keys.
// Negative and zero must both floor to 0 rather than wrap: these feed
// unsigned timers, and a wrapped value is a lockout that never expires.
void test_s_to_ms_rounds_and_floors() {
    CHECK_EQ_U(s_to_ms(0.0), 0u);
    CHECK_EQ_U(s_to_ms(-1.0), 0u);          // not 4294966296
    CHECK_EQ_U(s_to_ms(1.0), 1000u);
    CHECK_EQ_U(s_to_ms(0.25), 250u);
    CHECK_EQ_U(s_to_ms(0.0004), 0u);        // rounds to nearest, not up
    CHECK_EQ_U(s_to_ms(0.0006), 1u);
}

// §15.2 -> §9.4: the selector policy adapter carries the seconds knobs through
// s_to_ms and the rest straight across. Pinned on one of each, because the
// failure mode is a silently dropped field — the struct is 30-odd members and
// a missed assignment leaves a default that looks plausible.
void test_selector_policy_carries_the_config() {
    Config c = tx_config();
    c.policy.select.min_profile = 2;
    c.policy.select.max_profile = 5;
    c.policy.select.demote_milli = 123;
    c.policy.select.rung_lockout_s = 1.5;      // seconds -> ms
    c.venc.max_bitrate_kbps = 9000;            // §9.6 Pass 75 ceiling
    const wblink::SelectorPolicy p = selector_policy(c);
    CHECK_EQ_U(p.min_profile, 2);
    CHECK_EQ_U(p.max_profile, 5);
    CHECK_EQ_U(p.demote_milli, 123);
    CHECK_EQ_U(p.rung_lockout_ms, 1500u);
    CHECK_EQ_U(p.max_bitrate_kbps, 9000);
}

// The dot11 bandwidth code is not the width in MHz. Getting this wrong sends
// a correct-looking channel spec that the chip reads as a different width.
void test_bw_code_maps_widths() {
    CHECK_EQ_U(bw_code(20), 0);
    CHECK_EQ_U(bw_code(40), 1);
    CHECK_EQ_U(bw_code(80), 2);
}

// §9.4 Pass 186/187: the probe candidate follows the EFFECTIVE ceiling — the
// live §9.7 pin narrowed by the §9.2 lockout — and is re-derived every
// selector tick, because the lockout half of that moves with time and expiry
// and produces no commit to hang a refresh on. refresh_probe() is called
// directly here: driving a real §9.2 lockout needs a whole loss campaign, and
// this is a test of the arming seam, not of the lockout.
void test_probe_arming_follows_the_effective_ceiling() {
    wblink::ProfileTable t;
    for (auto [id, mcs] : {std::pair<uint8_t, uint8_t>{2, 1},
                           {4, 3},
                           {6, 5}}) {
        wblink::Profile p;
        p.id = id;
        p.mcs = mcs;
        p.max_payload = 1424;
        t.profiles.push_back(p);
    }
    t.floor_profile = 2;
    t.probe_period = 64;
    t.probe_slot = 4;

    Config c = tx_config();
    c.policy.select.min_profile = 2;
    c.policy.select.max_profile = 6;  // unpinned within this ladder
    TxCore tx(c, 12345, &t, 0xF2);

    struct Armed {
        uint16_t period = 0xFFFF;
        uint16_t slot = 0xFFFF;
        uint8_t mcs = 0xFF;
        int calls = 0;
    } armed;

    // §9.4 fail-closed: with no hook installed the node is not probing, and
    // §15.3 must keep saying so however often the seam runs.
    CHECK_EQ_U(tx.probe_candidate_mcs_, wblink::kProbeMcsNone);
    tx.refresh_probe(2);
    CHECK_EQ_U(tx.probe_candidate_mcs_, wblink::kProbeMcsNone);
    CHECK_EQ_U(armed.calls, 0);

    tx.apply_probe = [&armed](uint16_t period, uint16_t slot, uint8_t mcs) {
        armed = {period, slot, mcs, armed.calls + 1};
    };

    // Unpinned at the ladder floor (profile 2): candidate is id 4's mcs 3.
    tx.refresh_probe(2);
    CHECK_EQ_U(armed.calls, 1);
    CHECK_EQ_U(armed.period, 64);
    CHECK_EQ_U(armed.slot, 4);
    CHECK_EQ_U(armed.mcs, 3);
    CHECK_EQ_U(tx.probe_candidate_mcs_, 3);

    // THE CHANGE-GUARD, and it is what makes a per-tick call affordable: the
    // seam now runs at tick cadence, so re-deriving the same answer must not
    // write the radio. Without this the arming would be re-issued ~10x/s
    // forever on a steady link.
    for (int i = 0; i < 25; ++i) tx.refresh_probe(2);
    CHECK_EQ_U(armed.calls, 1);
    CHECK_EQ_U(tx.probe_candidate_mcs_, 3);

    // Pinned AT the current rung — the §11.7 SELECTOR-freeze shape. Nothing
    // can climb, so the probe disarms: period 0, and §15.3 reads "not
    // probing". No commit happens here; the tick seam is what catches it.
    tx.set_profile_pin(2, 2);
    tx.refresh_probe(2);
    CHECK_EQ_U(armed.calls, 2);
    CHECK_EQ_U(armed.period, 0);
    CHECK_EQ_U(armed.mcs, 0);
    CHECK_EQ_U(tx.probe_candidate_mcs_, wblink::kProbeMcsNone);

    // Lifting the pin re-arms, again without a commit.
    tx.set_profile_pin(2, 6);
    tx.refresh_probe(2);
    CHECK_EQ_U(armed.calls, 3);
    CHECK_EQ_U(armed.period, 64);
    CHECK_EQ_U(armed.mcs, 3);
    CHECK_EQ_U(tx.probe_candidate_mcs_, 3);
}

// §9.4 Pass 187: the ceiling the clamp reads is Selector::effective_ceiling_
// profile(), which is evaluate()'s adaptive_hi. With no lockout it is the
// §9.7 pin, so a fresh selector must answer the pin exactly — that equality is
// what lets the probe clamp be stated in terms of one ceiling rather than two.
void test_effective_ceiling_is_the_pin_when_nothing_is_locked_out() {
    wblink::ProfileTable t;
    for (auto [id, mcs] : {std::pair<uint8_t, uint8_t>{2, 1},
                           {4, 3},
                           {6, 5}}) {
        wblink::Profile p;
        p.id = id;
        p.mcs = mcs;
        p.max_payload = 1424;
        t.profiles.push_back(p);
    }
    t.floor_profile = 2;

    wblink::SelectorPolicy pol;
    pol.min_profile = 2;
    pol.max_profile = 6;
    wblink::Selector sel(pol, &t);
    CHECK_EQ_U(sel.effective_ceiling_profile(1000), 6);
    sel.set_profile_pin(2, 4);
    CHECK_EQ_U(sel.effective_ceiling_profile(1000), 4);
    sel.set_profile_pin(2, 2);
    CHECK_EQ_U(sel.effective_ceiling_profile(1000), 2);
    // §9.7 saturation: an id absent from the ladder resolves to the top rung,
    // so 255 still means unpinned rather than clamping everything away.
    sel.set_profile_pin(2, 255);
    CHECK_EQ_U(sel.effective_ceiling_profile(1000), 6);
}

// A table with no §3.6 probe block never flies a probe, and — with the Pass
// 187 change-guard — never writes the radio at all to say so. That is the
// right answer rather than a weaker one: RadioAir's own probe_period starts at
// 0, so an explicit disarm here would be a redundant write repeated at tick
// cadence, forever, on every node in a fleet that has not adopted the block.
// The assertion is therefore "zero calls AND not probing", not "disarmed".
void test_no_probe_schedule_never_arms() {
    wblink::ProfileTable t;
    for (auto [id, mcs] : {std::pair<uint8_t, uint8_t>{2, 1}, {4, 3}}) {
        wblink::Profile p;
        p.id = id;
        p.mcs = mcs;
        p.max_payload = 1424;
        t.profiles.push_back(p);
    }
    t.floor_profile = 2;  // probe_period stays 0

    TxCore tx(tx_config(), 12345, &t, 0x68);
    int calls = 0;
    tx.apply_probe = [&calls](uint16_t, uint16_t, uint8_t) { ++calls; };
    for (int i = 0; i < 20; ++i) tx.refresh_probe(2);
    CHECK_EQ_U(calls, 0);
    CHECK_EQ_U(tx.probe_candidate_mcs_, wblink::kProbeMcsNone);
}

// §10.6 Pass 187: CalibActions::pin_rung is an MCS and §9.7 pins take profile
// IDs. On a ladder whose ids differ from its MCS values the old code pinned
// the MCS directly, §9.7 saturated the unmatched id to the top rung, and the
// sweep ran UNPINNED — every dwell measuring whatever rung the selector had
// drifted to, silently. The mapping is asserted here through the same lookup
// the calibration seam uses.
void test_calibration_pin_maps_mcs_to_a_profile_id() {
    wblink::ProfileTable t;
    // ids and MCS deliberately disjoint: id 2 carries mcs 1, id 4 carries
    // mcs 3, id 6 carries mcs 5. Pinning "mcs 3" as an id would name profile
    // 3, which does not exist.
    for (auto [id, mcs] : {std::pair<uint8_t, uint8_t>{2, 1},
                           {4, 3},
                           {6, 5}}) {
        wblink::Profile p;
        p.id = id;
        p.mcs = mcs;
        p.max_payload = 1424;
        t.profiles.push_back(p);
    }
    t.floor_profile = 2;

    TxCore tx(tx_config(), 12345, &t, 0xF2);
    CHECK_EQ_U(tx.profile_id_for_mcs(1).value(), 2);
    CHECK_EQ_U(tx.profile_id_for_mcs(3).value(), 4);
    CHECK_EQ_U(tx.profile_id_for_mcs(5).value(), 6);
    // The MCS values the calibrator sweeps that this ladder cannot select —
    // it walks 0..7 regardless — must be reported as unmappable, not
    // silently pinned to something else.
    CHECK(!tx.profile_id_for_mcs(0).has_value());
    CHECK(!tx.profile_id_for_mcs(2).has_value());
    CHECK(!tx.profile_id_for_mcs(4).has_value());
    CHECK(!tx.profile_id_for_mcs(7).has_value());
    // Two profiles at one MCS: the lower id wins, matching the ascending-id
    // order §9.4's up-candidate walk uses.
    wblink::Profile dup;
    dup.id = 9;
    dup.mcs = 3;
    dup.max_payload = 1424;
    t.profiles.push_back(dup);
    TxCore tx2(tx_config(), 12345, &t, 0xF2);
    CHECK_EQ_U(tx2.profile_id_for_mcs(3).value(), 4);
}

// §9.4 Pass 188: a §9.2 lockout puts the effective ceiling BELOW max_profile,
// and the probe must KEEP MEASURING across that gap while the veto stays
// clamped by the climb gate. Builds the one state the pin-only tests cannot
// reach — the two ceilings disagreeing — which is where both the Pass 187
// behaviour and its Pass 188 reversal are visible.
void test_probe_keeps_measuring_through_a_lockout() {
    wblink::ProfileTable t;
    for (uint8_t i = 0; i < 8; ++i) {
        wblink::Profile p;
        p.id = i;
        p.mcs = i;
        p.gi = wblink::GuardInterval::kLong;
        p.tx_power_level = 4;
        p.airtime_budget_permille = 600;
        p.bitrate_min_kbps = 2200;
        p.max_payload = 1424;
        t.profiles.push_back(p);
    }
    t.floor_profile = 0;
    t.probe_period = 64;
    t.probe_slot = 4;

    Config c = tx_config();
    c.policy.select.max_profile = 7;   // unpinned: the pin can never explain
    c.policy.select.rung_lockout_s = 100.0;  // long enough to stay latched
    TxCore tx(c, 12345, &t, 0xF2);

    uint8_t armed_mcs = 0xFE;
    uint16_t armed_period = 0xFFFF;
    tx.apply_probe = [&](uint16_t period, uint16_t, uint8_t mcs) {
        armed_period = period;
        armed_mcs = mcs;
    };

    uint32_t epoch = 0;
    auto report = [&](uint64_t now, int8_t rssi, uint16_t loss) {
        wblink::LinkReport r;
        r.prefix.originator = 9;
        r.prefix.session_id = 1;
        r.report_epoch = ++epoch;
        r.rssi_best = rssi;
        r.rssi_mean = rssi;
        r.loss_postdiv_prearq = loss;
        r.uniq = 100;
        r.adapters = 2;
        tx.selector_.on_report(r, now);
    };
    auto run = [&](uint64_t from, uint64_t to, int8_t rssi, uint16_t loss) {
        uint64_t next = from;
        for (uint64_t now = from; now <= to; now += 10) {
            if (now >= next) {
                report(now, rssi, loss);
                next = now + 100;
            }
            (void)tx.selector_.tick(now);
        }
    };

    (void)tx.selector_.tick(0);          // boot
    run(0, 6000, -40, 0);                // clean + strong: climb to the top
    CHECK_EQ_U(tx.selector_.profile_id(), 7);
    run(6000, 6900, -40, 100);           // loss at the top: demote + lock it
    CHECK_EQ_U(tx.selector_.profile_id(), 6);
    CHECK(tx.selector_.lockout(6900).active);

    // The two ceilings now DISAGREE, which is the whole point of the case.
    CHECK_EQ_U(tx.selector_.max_profile(), 7);
    CHECK_EQ_U(tx.selector_.effective_ceiling_profile(6900), 6);

    // Below the locked rung the probe still flies: profile 5's up-candidate is
    // profile 6, which is AT the effective ceiling, and the clamp is "not
    // above" rather than "strictly below". Arming here first is what makes the
    // disarm below a visible TRANSITION rather than a state that was never
    // entered.
    tx.refresh_probe(5);
    CHECK_EQ_U(tx.probe_candidate_mcs_, 6);
    CHECK_EQ_U(armed_period, 64);
    CHECK_EQ_U(armed_mcs, 6);

    // §9.4 Pass 188: one rung up, with 7 locked out, the probe KEEPS MEASURING.
    // Pass 187 disarmed here; a range walk then measured that the probe was off
    // for 46% of a degrading link and the veto never fired once, because loss
    // locks the rung above and nothing is left measuring the candidate rate by
    // the time it matters. Arming follows the §9.7 pin alone now.
    tx.refresh_probe(6);
    CHECK_EQ_U(tx.probe_candidate_mcs_, 7);
    CHECK_EQ_U(armed_period, 64);
    CHECK_EQ_U(armed_mcs, 7);

    // ...and the VETO is still clamped, with no code in the arming path: the
    // climb rules gate on `rung_ < adaptive_hi` before probe_veto_fresh() is
    // consulted, so while rung 7 is locked the effective ceiling sits at 6 and
    // a climb into 7 is unreachable however bad the evidence is. That gap
    // between the two ceilings is the whole Pass 188 argument, so pin it.
    CHECK_EQ_U(tx.selector_.max_profile(), 7);
    CHECK_EQ_U(tx.selector_.effective_ceiling_profile(6900), 6);

    // After the lockout expires the two ceilings agree again, and the evidence
    // gathered DURING the lockout is what §9.2 re-entry now has to judge on —
    // the reason Pass 188 keeps the probe running through it.
    run(110000, 111000, -30, 0);
    CHECK_EQ_U(tx.selector_.effective_ceiling_profile(111000), 7);
    tx.refresh_probe(6);
    CHECK_EQ_U(tx.probe_candidate_mcs_, 7);
    CHECK_EQ_U(armed_period, 64);
    CHECK_EQ_U(armed_mcs, 7);
}

// §10.6 Pass 187 at the SEAM. test_calibration_pin_maps_mcs_to_a_profile_id
// above checks the lookup in isolation, and mutation-testing showed that is
// not enough: bypassing the lookup at the call site left it green. This drives
// a real calibration through calibrate_service() and reads what actually
// reached the §9.7 pin.
void test_calibration_seam_pins_by_id_not_by_mcs() {
    wblink::ProfileTable t;
    // ids and MCS deliberately offset by two, so pinning the MCS directly
    // names a profile that does not exist and §9.7 silently saturates.
    for (auto [id, mcs] : {std::pair<uint8_t, uint8_t>{2, 0},
                           {3, 1},
                           {4, 2}}) {
        wblink::Profile p;
        p.id = id;
        p.mcs = mcs;
        p.gi = wblink::GuardInterval::kLong;
        p.tx_power_level = 4;
        p.airtime_budget_permille = 600;
        p.bitrate_min_kbps = 2200;
        p.max_payload = 1424;
        t.profiles.push_back(p);
    }
    t.floor_profile = 2;

    Config c = tx_config();
    c.policy.select.min_profile = 2;
    c.policy.select.max_profile = 4;
    TxCore tx(c, 12345, &t, 0xF2);
    tx.init_calibration(c.policy.calibration, std::nullopt);
    CHECK(tx.calibrator_.has_value());
    CHECK(tx.calibrator_->start(1000));

    // The first action of a run enters rung 0 — i.e. MCS 0, which this ladder
    // carries on profile id 2.
    tx.calibrate_service(1000);
    // min==max is the §10.6 pin shape; max_profile() is the readable half.
    CHECK_EQ_U(tx.selector_.max_profile(), 2);   // the ID, not the MCS 0
    CHECK_EQ_U(tx.selector_.effective_ceiling_profile(1000), 2);
    // Still running: a mappable MCS must not abort the sweep.
    CHECK(tx.calibrating());
}

// ...and the refusal. The calibrator walks MCS 0..7 whatever the ladder holds,
// so a ladder that cannot select one of them makes that rung unmeasurable. An
// unpinned dwell would place a power for a rate the radio never flew, so the
// run FAILS with its own reason rather than reporting a curve it did not
// measure (§10.6 refuses false success).
void test_calibration_refuses_an_mcs_the_ladder_cannot_select() {
    wblink::ProfileTable t;
    // No profile at MCS 0 — the very first rung the calibrator enters.
    for (auto [id, mcs] : {std::pair<uint8_t, uint8_t>{2, 3},
                           {4, 5}}) {
        wblink::Profile p;
        p.id = id;
        p.mcs = mcs;
        p.gi = wblink::GuardInterval::kLong;
        p.tx_power_level = 4;
        p.airtime_budget_permille = 600;
        p.bitrate_min_kbps = 2200;
        p.max_payload = 1424;
        t.profiles.push_back(p);
    }
    t.floor_profile = 2;

    Config c = tx_config();
    c.policy.select.min_profile = 2;
    c.policy.select.max_profile = 4;
    TxCore tx(c, 12345, &t, 0xF2);
    tx.init_calibration(c.policy.calibration, std::nullopt);
    CHECK(tx.calibrator_->start(1000));
    tx.calibrate_service(1000);
    CHECK(!tx.calibrating());
    CHECK(tx.calibrator_->state() == wblink::CalibState::kFailed);
    // A distinct reason: a structural config defect must not read as an
    // operator cancellation.
    const char* why = tx.calibrator_->fail_reason();
    CHECK(why != nullptr);
    // Guarded rather than constructed straight into std::string: when this
    // case regresses the run does not fail at all, why is null, and an
    // unguarded construction throws instead of reporting.
    CHECK(why != nullptr && std::string(why) == "mcs_not_in_ladder");
}

}  // namespace

int main() {
    test_constructs_without_a_profile_table();
    test_s_to_ms_rounds_and_floors();
    test_selector_policy_carries_the_config();
    test_bw_code_maps_widths();
    test_probe_arming_follows_the_effective_ceiling();
    test_effective_ceiling_is_the_pin_when_nothing_is_locked_out();
    test_no_probe_schedule_never_arms();
    test_calibration_pin_maps_mcs_to_a_profile_id();
    test_probe_keeps_measuring_through_a_lockout();
    test_calibration_seam_pins_by_id_not_by_mcs();
    test_calibration_refuses_an_mcs_the_ladder_cannot_select();
    return wbtest_finish("node_tx_core_test");
}
