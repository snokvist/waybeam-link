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

}  // namespace

// §9.4 Pass 186: the probe candidate is clamped to the LIVE §9.7 pin, and a
// pin change re-derives it. Before this, the arming happened only inside the
// commit branch, so a pin that forbids every climb — which a §11.7 SELECTOR
// freeze installs by pinning min == max == current, after which nothing can
// commit — left the previous arming on the radio, spending a 1/period duty on
// evidence whose only consumer (a veto) had nothing left to veto.
void test_probe_arming_follows_the_live_pin() {
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

    // The hook is what §9.4 fail-closed enablement installs; without it the
    // node is not probing and must keep saying so.
    struct Armed {
        uint16_t period = 0xFFFF;
        uint16_t slot = 0xFFFF;
        uint8_t mcs = 0xFF;
        int calls = 0;
    } armed;
    CHECK_EQ_U(tx.probe_candidate_mcs_, wblink::kProbeMcsNone);
    tx.set_profile_pin(2, 6);
    CHECK_EQ_U(tx.probe_candidate_mcs_, wblink::kProbeMcsNone);  // no hook

    tx.apply_probe = [&armed](uint16_t period, uint16_t slot, uint8_t mcs) {
        armed = {period, slot, mcs, armed.calls + 1};
    };

    // Unpinned at the ladder floor (profile 2): candidate is id 4's mcs 3.
    tx.set_profile_pin(2, 6);
    CHECK_EQ_U(armed.calls, 1);
    CHECK_EQ_U(armed.period, 64);
    CHECK_EQ_U(armed.slot, 4);
    CHECK_EQ_U(armed.mcs, 3);
    CHECK_EQ_U(tx.probe_candidate_mcs_, 3);

    // Pinned AT the current rung — the SELECTOR-freeze shape. Nothing can
    // climb, so the probe disarms: period 0, and §15.3 reads "not probing".
    tx.set_profile_pin(2, 2);
    CHECK_EQ_U(armed.calls, 2);
    CHECK_EQ_U(armed.period, 0);
    CHECK_EQ_U(armed.mcs, 0);
    CHECK_EQ_U(tx.probe_candidate_mcs_, wblink::kProbeMcsNone);

    // Lifting the pin re-arms without waiting for a commit.
    tx.set_profile_pin(2, 6);
    CHECK_EQ_U(armed.calls, 3);
    CHECK_EQ_U(armed.period, 64);
    CHECK_EQ_U(armed.mcs, 3);
    CHECK_EQ_U(tx.probe_candidate_mcs_, 3);
}

// A table with no probe schedule disarms on every path, so a fleet that has
// not adopted the §3.6 probe block never flies one.
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
    uint16_t period = 0xFFFF;
    tx.apply_probe = [&period](uint16_t p, uint16_t, uint8_t) { period = p; };
    tx.set_profile_pin(2, 4);
    CHECK_EQ_U(period, 0);
    CHECK_EQ_U(tx.probe_candidate_mcs_, wblink::kProbeMcsNone);
}

int main() {
    test_constructs_without_a_profile_table();
    test_s_to_ms_rounds_and_floors();
    test_selector_policy_carries_the_config();
    test_bw_code_maps_widths();
    test_probe_arming_follows_the_live_pin();
    test_no_probe_schedule_never_arms();
    return wbtest_finish("node_tx_core_test");
}
