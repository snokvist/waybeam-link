// SPDX-License-Identifier: GPL-2.0-or-later
// RxCore, reached DIRECTLY — the first test of the node/ layer (#109 Phase 2a).
//
// Why this file matters more than its assertions
// ----------------------------------------------
// Before the move, RxCore lived in app/main.cpp's anonymous namespace, so the
// only way to instantiate it was tests/app_test.cpp's `#include "app/main.cpp"`
// with main() suppressed and -Wno-unused-function. That include costs the whole
// 8.2k-line translation unit, drags in every app-layer type, and cannot be
// linked against by anything outside this repo. It is also why waybeam-link
// Passes 165-167 had defects that could only be proven on hardware: the code
// was not reachable from a unit test at all.
//
// This file includes ONE header and links wblink::node. If that ever stops
// being true — if a change to RxCore forces this file to reach for app-layer
// state — the layering has regressed and the compiler says so here first.
#include "wblink/node/rx_core.h"

#include "wbtest.h"

namespace {

using wblink::Config;
using wblink::node::RxCore;

// The smallest config that yields a running RX node: an originator and a
// preferred peer. No adapters, no sockets, no files, no clock.
Config rx_config() {
    Config c;
    c.node.originator = 9;
    c.node.preferred_originator = 17;
    return c;
}

// ...plus one dir:"out" stream, which is what RxCore::wants() turns into a
// WantSpec. A node with no out stream wants nothing, so nothing can pin to it.
Config rx_config_with_stream(uint16_t from_originator) {
    Config c = rx_config();
    wblink::StreamCfg s;
    s.stream_id = 0;
    s.stream_type = wblink::stream_type::kRtp;
    s.dir = wblink::Dir::kOut;
    s.originator = from_originator;
    c.streams.push_back(s);
    return c;
}

// A node boots with nothing selected and nothing latched. Both are
// std::optional and both are read by §15.3 and the §3.9 recovery gate, so
// "unset" has to be distinguishable from "0" — originator 0 is a legal value
// on the wire (§3.0 reserves it as unspecified, not as absent).
void test_boots_with_no_selection_and_no_latch() {
    RxCore rx(rx_config(), 12345, nullptr, std::nullopt);
    CHECK(!rx.selected_originator().has_value());
    CHECK(!rx.latched_originator().has_value());
    CHECK(rx.stream_keys().empty());
}

// §3.5: selection is the node's own choice and is set locally; the latch is
// evidence-driven and must NOT follow it. Conflating the two is what Pass 115
// (claim-follows-authority) had to unpick on the report-gate side, so it is
// worth a pin on the accessor pair that reports them.
//
// The subtlety, and the reason the first draft of this test was wrong:
// selected_originator() reports the ENGINE's output-want pin, not the argument
// last handed to select_originator(). A node with no dir:"out" stream wants
// nothing, so there is nothing to pin and the accessor stays nullopt however
// often it is called — which is exactly the "common ground that names no
// preferred_originator" case its own comment describes. Both shapes are
// pinned here so a future reader does not have to rediscover that.
void test_select_originator_pins_a_want_but_never_the_latch() {
    RxCore bare(rx_config(), 12345, nullptr, std::nullopt);
    bare.select_originator(17);
    CHECK(!bare.selected_originator().has_value());   // no want to pin
    CHECK(!bare.latched_originator().has_value());

    RxCore rx(rx_config_with_stream(17), 12345, nullptr, std::nullopt);
    rx.select_originator(17);
    CHECK(rx.selected_originator().has_value());
    CHECK(rx.selected_originator().value_or(0) == 17);
    // Selection is local intent; the latch needs evidence off the air.
    CHECK(!rx.latched_originator().has_value());
}

// A spectator (§2 Pass 74, §15.2 node.spectator) has no uplink, so §3.9
// recovery must reach no inject even when asked. Pass 106 gated this in the
// constructor rather than relying on the inject to no-op, so that it also
// stays quiet in the log — which means the gate is only observable from a
// constructed RxCore, i.e. only from a test shaped like this one.
void test_spectator_emits_no_recovery() {
    Config c = rx_config();
    c.node.spectator = true;
    c.node.recovery_on_latch = true;   // asked for, and still refused
    RxCore rx(c, 12345, nullptr, std::nullopt);

    int injected = 0;
    const RxCore::Inject inject = [&](const uint8_t*, size_t, uint16_t) {
        ++injected;
    };
    rx.emit_latch_recovery(1000, inject);
    CHECK_EQ_U(injected, 0);
}

// §15.3 fill on a node that has received nothing: it must produce a snapshot
// rather than refuse, because the stats line is emitted on a timer from boot
// and a receiver with no traffic yet is the normal startup state.
void test_fill_stats_on_an_idle_node() {
    RxCore rx(rx_config(), 0xDEADBEEF, nullptr, std::nullopt);
    wblink::StatsSnapshot snap;
    rx.fill_stats(snap, 1000);
    CHECK_EQ_U(snap.link.target_originator, 0);
    // reset_stats() is the §15.5 counter reset; it must be safe with no
    // streams open, which is exactly when an operator hits it after a restart.
    rx.reset_stats();
    rx.fill_stats(snap, 2000);
}

// §3.4: a block with no NACK against it has none recorded. The accessor is
// const and takes (stream, block), and an unknown stream must answer false
// rather than fabricate an entry — the ARQ scheduler reads this to decide
// whether a repair is already in flight.
void test_unknown_block_has_no_nack() {
    RxCore rx(rx_config(), 12345, nullptr, std::nullopt);
    CHECK(!rx.block_had_nack(0, 0));
    CHECK(!rx.block_had_nack(7, 4242));
}

// §15.3 Pass 186: the six probe_* fields describe THIS receiver's own §9.4
// window, which is fed by our on_data() and owes nothing to the craft's §3.15
// selector word. They were briefly filled inside the `selector_source_current
// && selector_state_fresh` gate, where an absent or stale word left them
// reading "nothing is probing" while the window held evidence — the same
// unreadable silence Pass 186 exists to remove, reintroduced one level down.
//
// The snapshot is POISONED first on purpose. An idle window's honest answer is
// byte-identical to the struct's defaults, so asserting the defaults would pass
// whether the fill ran or not — a test of the stimulus, not the effect. With
// the poison in place this fails on the gated version and passes on the
// unconditional one.
void test_probe_fields_do_not_depend_on_the_craft_selector_word() {
    RxCore rx(rx_config(), 12345, nullptr, std::nullopt);
    wblink::StatsSnapshot snap;
    snap.link.probe_per = 1234;
    snap.link.probe_per_age_ms = 5678;
    snap.link.probe_candidate_mcs = 3;
    snap.link.probe_successes = 11;
    snap.link.probe_failures = 22;
    snap.link.probe_observed = 33;
    // An idle node has no remote selector state at all, which is precisely the
    // condition the gated version could not report through.
    rx.fill_stats(snap, 1000);
    CHECK_EQ_U(snap.link.probe_per, wblink::kNoProbe);
    CHECK_EQ_U(snap.link.probe_per_age_ms, 0);
    CHECK_EQ_U(snap.link.probe_candidate_mcs, wblink::kProbeMcsNone);
    CHECK_EQ_U(snap.link.probe_successes, 0);
    CHECK_EQ_U(snap.link.probe_failures, 0);
    CHECK_EQ_U(snap.link.probe_observed, 0);
}

}  // namespace

int main() {
    test_boots_with_no_selection_and_no_latch();
    test_select_originator_pins_a_want_but_never_the_latch();
    test_spectator_emits_no_recovery();
    test_fill_stats_on_an_idle_node();
    test_unknown_block_has_no_nack();
    test_probe_fields_do_not_depend_on_the_craft_selector_word();
    return wbtest_finish("node_rx_core_test");
}
