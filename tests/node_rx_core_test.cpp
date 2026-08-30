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

// Two dir:"out" streams on ONE RxCore — which is what every deployed ground
// runs (RTP + AUDIO, and .242 adds TELEMETRY). A stream with no traffic of its
// own must report 0 best-ear loss, NOT whatever the previous stream in
// fill_stats' loop just computed. The hold was a single scalar until review,
// and an idle AUDIO stream showed the VIDEO stream's number.
void test_best_ear_hold_does_not_leak_across_streams() {
    Config c = rx_config();
    for (uint8_t id : {uint8_t{0}, uint8_t{1}}) {
        wblink::StreamCfg s;
        s.stream_id = id;
        s.stream_type = id == 0 ? wblink::stream_type::kRtp
                                : wblink::stream_type::kAudio;
        s.dir = wblink::Dir::kOut;
        c.streams.push_back(s);
    }
    RxCore rx(c, /*session=*/1, nullptr, std::nullopt);

    // Latch BOTH streams — the leak needs two entries in fill_stats' loop —
    // then keep feeding only stream 0, lossily, while stream 1 goes silent.
    std::vector<uint8_t> pkt;
    auto send = [&](uint8_t sid, uint8_t stype, uint32_t seq, uint64_t now) {
        wblink::DataHeader h{};
        h.prefix.originator = 17;
        h.prefix.destination = 0;
        h.prefix.session_id = 4242;
        h.stream_id = sid;
        h.stream_type = stype;
        h.seq = seq;
        h.block_id = 0;
        const uint8_t payload[4] = {1, 2, 3, 4};
        pkt.assign(256, 0);
        const size_t n = wblink::encode_data(h, payload, sizeof payload,
                                             pkt.data(), pkt.size());
        pkt.resize(n);
        rx.on_air(/*adapter=*/0, pkt.data(), pkt.size(), now,
                  [](uint8_t, uint32_t, uint8_t, const uint8_t*, size_t) {});
    };

    uint32_t v_seq = 0, a_seq = 0;
    uint64_t t = 1000;
    wblink::StatsSnapshot snap;
    for (int i = 0; i < 4; ++i) {                       // latch both, cleanly
        send(0, wblink::stream_type::kRtp, v_seq++, t += 10);
        send(1, wblink::stream_type::kAudio, a_seq++, t += 10);
    }
    // Stream 1 now goes silent; stream 0 keeps losing 4 of every 5.
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < 4; ++i) {
            v_seq += 5;
            send(0, wblink::stream_type::kRtp, v_seq, t += 10);
        }
        snap = wblink::StatsSnapshot{};
        rx.fill_stats(snap, t);
    }

    const wblink::StreamStats* video = nullptr;
    const wblink::StreamStats* audio = nullptr;
    for (const wblink::StreamStats& st : snap.streams) {
        if (st.stream_id == 0) video = &st;
        if (st.stream_id == 1) audio = &st;
    }
    // Both must be present, or the leak path is not exercised at all.
    CHECK(video != nullptr);
    CHECK(audio != nullptr);
    if (video == nullptr || audio == nullptr) return;
    // Stream 0 really is losing; without this the next assertion is vacuous.
    CHECK(video->loss_best_ear_window_milli > 0);
    // Stream 1 has no evidence of its own and must say 0 — NOT stream 0's
    // number, which is what a shared hold produced.
    CHECK_EQ_U(audio->loss_best_ear_window_milli, 0u);
}

}  // namespace

int main() {
    test_boots_with_no_selection_and_no_latch();
    test_select_originator_pins_a_want_but_never_the_latch();
    test_spectator_emits_no_recovery();
    test_fill_stats_on_an_idle_node();
    test_unknown_block_has_no_nack();
    test_probe_fields_do_not_depend_on_the_craft_selector_word();
    test_best_ear_hold_does_not_leak_across_streams();
    return wbtest_finish("node_rx_core_test");
}
