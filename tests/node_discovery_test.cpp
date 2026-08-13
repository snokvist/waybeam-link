// SPDX-License-Identifier: GPL-2.0-or-later
// DiscoveryCatalog and ScoutEngine, reached directly (#109 Phase 2a).
//
// Same purpose as node_rx_core_test.cpp: one #include, link wblink::node, no
// app/main.cpp include and no suppressed main(). If a change here forces this
// file to reach for app-layer state, the layering has regressed and the
// compiler says so first.
//
// ScoutEngine is the piece of Phase 2a that was ALREADY shaped for this —
// every side effect it has is an injected `Hooks` callback, so a sweep can be
// driven with no radio, no socket and no clock. The rest of the phase is
// working toward that shape for the pieces that do not have it yet.
#include "wblink/node/discovery.h"

#include "wbtest.h"

namespace {

using wblink::node::DiscoveryCatalog;
using wblink::node::ScoutEngine;

// Records what the engine asked the world to do. `set_filter` and `retune_all`
// are called unconditionally on the sweep paths, so they must be present —
// a half-populated Hooks is a crash, not a refusal, which is itself worth
// knowing before wiring one in a consumer.
struct Spy {
    std::vector<uint16_t> retuned;
    std::vector<uint16_t> retuned_all;
    int filter_calls = 0;
    bool last_filter_was_wildcard = false;

    ScoutEngine::Hooks hooks() {
        ScoutEngine::Hooks h;
        h.retune = [this](uint16_t mhz, uint8_t) {
            retuned.push_back(mhz);
            return true;
        };
        h.retune_all = [this](uint16_t mhz, uint8_t) {
            retuned_all.push_back(mhz);
        };
        h.set_filter = [this](std::optional<uint8_t> net_id) {
            ++filter_calls;
            last_filter_was_wildcard = !net_id.has_value();
        };
        h.psk_known = [](uint16_t) { return false; };
        h.domain = [] { return std::string("idx/0"); };
        return h;
    }
};

ScoutEngine make_scout(Spy& spy) {
    return ScoutEngine(spy.hooks(), /*bw=*/20, /*rest_chan=*/5805,
                       /*rest_filter=*/std::optional<uint8_t>(0),
                       /*scout_adapter=*/0);
}

// A scout that has never swept is idle. `start` reports failure by returning a
// REASON string, not a bool — an empty channel list is a caller error and must
// name itself, because the SPA surfaces the string verbatim.
void test_scout_boots_idle_and_refuses_an_empty_sweep() {
    Spy spy;
    ScoutEngine scout = make_scout(spy);
    CHECK(!scout.scanning());
    CHECK(scout.start({}, 300, 1000) == "no channels to scan");
    CHECK(!scout.scanning());
    CHECK(spy.retuned.empty());   // nothing touched a radio
}

// §15.5a: a sweep widens the RX filter to hear ALL net_ids, then retunes the
// scout adapter to the first channel. The wildcard filter is the part that is
// easy to lose in a refactor and impossible to see from outside — a sweep that
// keeps the narrow filter finds only its own fleet and reports the band empty.
void test_scout_start_widens_the_filter_then_retunes() {
    Spy spy;
    ScoutEngine scout = make_scout(spy);
    CHECK(scout.start({5180, 5805}, 300, 1000).empty());   // "" == accepted
    CHECK(scout.scanning());
    CHECK_EQ_U(spy.filter_calls, 1);
    CHECK(spy.last_filter_was_wildcard);
    CHECK(!spy.retuned.empty());
    if (!spy.retuned.empty()) CHECK_EQ_U(spy.retuned.front(), 5180);
}

// stop() restores: narrow filter back, and ALL ears home — not just the scout
// adapter, because a prior claim can have split them. abandon() deliberately
// does neither, for a caller that is about to retune and re-pin itself
// (Pass 144, the claim path). The difference is the whole reason both exist.
void test_stop_restores_but_abandon_does_not() {
    Spy stopped;
    ScoutEngine a = make_scout(stopped);
    CHECK(a.start({5180, 5805}, 300, 1000).empty());
    a.stop(2000);
    CHECK(!a.scanning());
    CHECK_EQ_U(stopped.filter_calls, 2);            // widened, then restored
    CHECK(!stopped.last_filter_was_wildcard);       // ...to the narrow one
    CHECK_EQ_U(stopped.retuned_all.size(), 1);      // every ear went home
    if (!stopped.retuned_all.empty()) {
        CHECK_EQ_U(stopped.retuned_all.front(), 5805);
    }

    Spy abandoned;
    ScoutEngine b = make_scout(abandoned);
    CHECK(b.start({5180, 5805}, 300, 1000).empty());
    b.abandon(2000);
    CHECK(!b.scanning());
    CHECK_EQ_U(abandoned.filter_calls, 1);          // still wide open
    CHECK(abandoned.retuned_all.empty());           // nothing sent home
}

// Frames already in the USB pipeline can arrive after a retune. The sense
// barrier is also the minimum candidate-attribution settle: pre-barrier frames
// must not create a second sighting on the new channel. Once the barrier has
// elapsed, published evidence includes its frame weight so consumers can show
// the same heard-most resolution used by candidate_for().
void test_scout_rejects_pre_settle_candidate_residue_and_publishes_weight() {
    Spy spy;
    ScoutEngine scout = make_scout(spy);
    wblink::AirRxMeta meta{};
    meta.adapter_id = 0;
    meta.net_id = 0;
    meta.rssi = -42;
    wblink::Announce announce{};
    announce.prefix.originator = 17;
    announce.prefix.session_id = 99;
    uint8_t raw[wblink::kHeartbeatSize]{};
    wblink::be16_write(raw + 3, 17);

    CHECK(scout.start({5745}, 300, 1000).empty());
    scout.on_frame(meta, announce, raw, sizeof(raw));
    scout.stop(1100);
    CHECK(scout.results_json(1100).find("\"originator\":17") ==
          std::string::npos);

    CHECK(scout.start({5805}, 300, 2000).empty());
    scout.tick(2030);  // arms the post-retune frame-acceptance barrier
    scout.on_frame(meta, announce, raw, sizeof(raw));
    scout.stop(2100);
    const std::string json = scout.results_json(2100);
    CHECK(json.find("\"originator\":17") != std::string::npos);
    CHECK(json.find("\"frames\":1") != std::string::npos);
    const auto fresh_resting = scout.candidate_for(17);
    CHECK(fresh_resting.has_value());
    if (fresh_resting) CHECK_EQ_U(fresh_resting->chan, 5805u);
    CHECK(json.find("\"candidates\":[{\"originator\":17") !=
          std::string::npos);
    CHECK(json.find("\"chan\":5805,\"frames\":1,\"resolved\":true") !=
          std::string::npos);
}

void test_scout_near_tie_prefers_proven_resting_channel() {
    Spy spy;
    ScoutEngine scout = make_scout(spy);  // resting channel is 5805
    scout.set_trusted_rest_originator(17);
    wblink::AirRxMeta meta{};
    meta.adapter_id = 0;
    wblink::Announce announce{};
    announce.prefix.originator = 17;
    announce.prefix.session_id = 99;
    uint8_t raw[wblink::kHeartbeatSize]{};
    wblink::be16_write(raw + 3, 17);

    // Clock note: a channel that heard frames extends once (base + kExtendMs),
    // so 5745 finalizes at 1000+300+1500 rather than at its base deadline.
    CHECK(scout.start({5745, 5805}, 300, 1000).empty());
    scout.tick(1030);
    for (int i = 0; i < 11; ++i) scout.on_frame(meta, announce, raw, sizeof(raw));
    scout.tick(1300);  // base deadline: extends, stays on 5745
    scout.tick(2800);  // extended deadline: finalize 5745, enter 5805
    scout.tick(2830);
    for (int i = 0; i < 10; ++i) scout.on_frame(meta, announce, raw, sizeof(raw));
    scout.stop(2900);

    const auto resolved = scout.candidate_for(17);
    CHECK(resolved.has_value());
    if (resolved) CHECK_EQ_U(resolved->chan, 5805u);
    const std::string json = scout.results_json(2900);
    CHECK(json.find("\"decoded_airtime_permille\":") != std::string::npos);
    CHECK(json.find("\"ranking_score_permille\":") != std::string::npos);
    CHECK(json.find("\"interference_score_permille\":") != std::string::npos);
    CHECK(json.find("\"duty_cycle_known\":false") != std::string::npos);
    CHECK(json.find("\"chan\":5805,\"frames\":10,\"resolved\":true") !=
          std::string::npos);
    CHECK(json.find("\"chan\":5745,\"tuned\":true,\"evidence_valid\":false") !=
          std::string::npos);
    CHECK(json.find("\"chan\":5805,\"tuned\":true,\"evidence_valid\":true") !=
          std::string::npos);

    Spy ambiguous_spy;
    ScoutEngine ambiguous = make_scout(ambiguous_spy);
    CHECK(ambiguous.start({5745, 5805}, 300, 1500).empty());
    ambiguous.tick(1530);
    for (int i = 0; i < 11; ++i) {
        ambiguous.on_frame(meta, announce, raw, sizeof(raw));
    }
    ambiguous.tick(1800);  // base deadline: extends
    ambiguous.tick(3300);  // extended deadline: finalize 5745, enter 5805
    ambiguous.tick(3330);
    for (int i = 0; i < 10; ++i) {
        ambiguous.on_frame(meta, announce, raw, sizeof(raw));
    }
    ambiguous.stop(3400);
    CHECK(!ambiguous.candidate_for(17).has_value());
    const std::string ambiguous_json = ambiguous.results_json(3400);
    CHECK(ambiguous_json.find("\"candidates\":[]") != std::string::npos);
    CHECK(ambiguous_json.find("\"candidate_sightings\":[{\"originator\":17") !=
          std::string::npos);
    CHECK(ambiguous_json.find("\"chan\":5745,\"tuned\":true,\"evidence_valid\":false") !=
          std::string::npos);
    CHECK(ambiguous_json.find("\"chan\":5805,\"tuned\":true,\"evidence_valid\":false") !=
          std::string::npos);

    Spy moved_spy;
    ScoutEngine moved = make_scout(moved_spy);
    CHECK(moved.start({5745, 5805}, 300, 2000).empty());
    moved.tick(2030);
    for (int i = 0; i < 20; ++i) moved.on_frame(meta, announce, raw, sizeof(raw));
    moved.tick(2300);
    moved.tick(2330);
    for (int i = 0; i < 10; ++i) moved.on_frame(meta, announce, raw, sizeof(raw));
    moved.stop(2400);
    const auto clearly_moved = moved.candidate_for(17);
    CHECK(clearly_moved.has_value());
    if (clearly_moved) CHECK_EQ_U(clearly_moved->chan, 5745u);
}

// Pass 17: the catalog is observational and bounded. An originator it has
// never seen yields nullopt for both lookups — never a default-constructed
// token or a zero session, either of which would read as a real value to the
// §11.4 claim path that consumes them.
void test_catalog_knows_nothing_about_an_unseen_originator() {
    DiscoveryCatalog cat;
    CHECK(!cat.token_for(17).has_value());
    CHECK(!cat.session_for(17).has_value());
    // json() runs on the event-loop tick from boot, before anything has been
    // observed, and internally ages+trims. It must be safe and must not invent
    // an entry as a side effect of being asked.
    const std::string empty = cat.json(10000, {});
    CHECK(!empty.empty());               // a document, not a crash
    CHECK(!cat.session_for(17).has_value());
}

// §11.4a: the announced token must outlive the PRESENCE view that carried it.
//
// This is the bug that made a scouted craft unclaimable, and the mechanism is
// not the one two earlier theories blamed. `observe()` runs on the sweep path,
// so a dwell DOES cache the token — but the token lived in `nodes_`, and
// `json()` ages `nodes_` at 5 s. A ground polls the discovery snapshot on a
// cadence, so the ground's own UI refresh was deleting the key its claim
// needed, a few seconds after the ear moved off that channel. On the resting
// channel the craft re-announces at 2 Hz and the entry never ages out, which
// is exactly why this read as "only resting-channel discovery caches tokens".
//
// A sweep is ~21-38 s and the operator taps claim after it ends, so anything
// keyed to continuous presence is useless to the claim path by construction.
void test_an_announced_token_survives_the_presence_view_aging_out() {
    DiscoveryCatalog cat;
    wblink::Announce an{};
    an.prefix.originator = 17;
    an.prefix.session_id = 99;
    an.flags = wblink::announce_flags::kPskPresent;
    for (size_t i = 0; i < wblink::kAnnouncePskSize; ++i) {
        an.psk[i] = static_cast<uint8_t>(0xA0 + i);
    }
    cat.observe(an, 1000, /*net_id=*/7);
    CHECK(cat.token_for(17).has_value());
    CHECK(cat.session_for(17).has_value());

    // The ground's discovery snapshot, taken 6 s later — one poll of the very
    // UI the operator is looking at while deciding which craft to claim.
    const std::string snap = cat.json(7000, {});
    CHECK(snap.find("\"originator\":17") == std::string::npos);  // aged, correct
    // Presence is gone and SHOULD be — the craft is not being heard. The token
    // is not presence. Losing it here is what stranded the claim.
    CHECK(!cat.session_for(17).has_value());
    const auto tok = cat.token_for(17);
    CHECK(tok.has_value());
    if (tok) CHECK_EQ_U((*tok)[0], 0xA0u);
    if (tok) CHECK_EQ_U((*tok)[15], 0xAFu);
}

// A craft that reboots announces a NEW token under a new session. The cache is
// keyed by originator alone, so the fresh token must overwrite — a claim keyed
// with the pre-reboot token MACs against nothing and fails at CSA_ARMED with
// no hint. Same-originator overwrite is the whole reason this is not a set.
void test_a_re_announced_token_replaces_the_cached_one() {
    DiscoveryCatalog cat;
    wblink::Announce before{};
    before.prefix.originator = 17;
    before.prefix.session_id = 99;
    before.flags = wblink::announce_flags::kPskPresent;
    before.psk[0] = 0x11;
    cat.observe(before, 1000, 7);

    wblink::Announce after{};
    after.prefix.originator = 17;
    after.prefix.session_id = 100;  // rebooted
    after.flags = wblink::announce_flags::kPskPresent;
    after.psk[0] = 0x22;
    cat.observe(after, 2000, 7);

    const auto tok = cat.token_for(17);
    CHECK(tok.has_value());
    if (tok) CHECK_EQ_U((*tok)[0], 0x22u);
}

// An ANNOUNCE without kPskPresent carries an all-zero psk field (§3.12). It
// must not register as a token: an all-zero key would be handed to set_psk as
// a real one, and the claim would fail at the craft rather than refusing here
// with "no CSA key for craft" — a diagnosable refusal turned into a silent
// campaign timeout.
void test_an_announce_without_a_token_caches_nothing() {
    DiscoveryCatalog cat;
    wblink::Announce an{};
    an.prefix.originator = 17;
    an.prefix.session_id = 99;
    an.flags = wblink::announce_flags::kClaimed;  // claimed, but no token
    cat.observe(an, 1000, 7);
    CHECK(!cat.token_for(17).has_value());
    CHECK(cat.session_for(17).has_value());  // presence still recorded
}

// Craft-finder pacing (findings.md 2026-08-12). The base dwell is a short
// presence probe; anything heard extends it once to cover the >=1 Hz announce
// cadence. An empty band is swept at dwell_ms per channel instead of holding a
// full second for an airtime denominator nothing publishes.
//
// Arm 1 is the control and the reason the rest is evidence: "the sweep got
// faster" means nothing unless a dwell that SHOULD run long still does.
void test_short_dwell_probes_but_anything_heard_extends() {
    wblink::AirRxMeta meta{};
    meta.adapter_id = 0;
    meta.net_id = 0;
    meta.rssi = -42;
    uint8_t raw[wblink::kHeartbeatSize]{};
    wblink::be16_write(raw + 3, 17);

    // Arm 1 — silent channel: ends at the base deadline, not before, not after.
    Spy quiet;
    ScoutEngine a = make_scout(quiet);
    CHECK(a.start({5745, 5805}, 250, 1000).empty());
    a.tick(1030);                                    // arm the settle barrier
    a.tick(1100);
    CHECK_EQ_U(quiet.retuned.size(), 1u);            // still probing 5745
    a.tick(1250);                                    // base deadline
    CHECK_EQ_U(quiet.retuned.size(), 2u);
    if (quiet.retuned.size() > 1) CHECK_EQ_U(quiet.retuned[1], 5805u);

    // Arm 2 — traffic but no ANNOUNCE yet: extends once. This is what makes a
    // 250 ms base dwell safe rather than lossy; a craft's video is high-rate,
    // so presence trips long before its announce lands.
    Spy heard;
    ScoutEngine b = make_scout(heard);
    CHECK(b.start({5745, 5805}, 250, 1000).empty());
    b.tick(1030);
    b.on_frame(meta, wblink::Heartbeat{}, raw, sizeof(raw));
    b.tick(1250);                                    // extends, does not advance
    CHECK_EQ_U(heard.retuned.size(), 1u);
    b.tick(2000);                                    // inside 1000+250+1500
    CHECK_EQ_U(heard.retuned.size(), 1u);
    b.tick(2750);
    CHECK_EQ_U(heard.retuned.size(), 2u);
}

// Two craft can share a channel and announce independently. An earlier draft
// of the pacing above ended the dwell on the FIRST resolved candidate, which
// swept ~3 s off a ~12 s sweep and silently dropped every co-channel peer —
// the worst failure available to a craft finder, and invisible without this
// test because the sweep still "worked" and still found *a* craft.
void test_a_second_craft_on_one_channel_is_not_truncated_away() {
    uint8_t raw17[wblink::kHeartbeatSize]{};
    wblink::be16_write(raw17 + 3, 17);
    uint8_t raw4[wblink::kHeartbeatSize]{};
    wblink::be16_write(raw4 + 3, 4);
    wblink::Announce first{};
    first.prefix.originator = 17;
    first.prefix.session_id = 99;
    wblink::Announce second{};
    second.prefix.originator = 4;
    second.prefix.session_id = 100;
    wblink::AirRxMeta meta{};
    meta.adapter_id = 0;
    meta.rssi = -30;

    Spy spy;
    ScoutEngine scout = make_scout(spy);
    CHECK(scout.start({5805}, 250, 1000).empty());
    scout.tick(1030);
    scout.on_frame(meta, first, raw17, sizeof(raw17));    // craft 17 announces
    scout.tick(1250);                                     // must NOT advance
    meta.rssi = -70;
    scout.on_frame(meta, second, raw4, sizeof(raw4));     // craft 4, same dwell
    scout.stop(1400);

    const std::string json = scout.results_json(1400);
    CHECK(json.find("\"originator\":17") != std::string::npos);
    CHECK(json.find("\"originator\":4") != std::string::npos);
    CHECK(scout.candidate_for(17).has_value());
    CHECK(scout.candidate_for(4).has_value());
    // Each keeps its own signal — one craft's rssi must not leak onto another.
    CHECK(json.find("\"rssi_dbm\":-30") != std::string::npos);
    CHECK(json.find("\"rssi_dbm\":-70") != std::string::npos);
}

// §15.5a: candidates carry the STRONGEST rssi seen for that originator, and 0
// is the no-reading sentinel rather than a legal 0 dBm. Emitting 0 as a number
// would render as the strongest possible craft on a radio that reported
// nothing — the failure mode worth a test.
void test_candidate_rssi_is_strongest_and_absent_reads_null() {
    wblink::Announce announce{};
    announce.prefix.originator = 17;
    announce.prefix.session_id = 99;
    uint8_t raw[wblink::kHeartbeatSize]{};
    wblink::be16_write(raw + 3, 17);

    Spy spy;
    ScoutEngine scout = make_scout(spy);
    CHECK(scout.start({5805}, 250, 1000).empty());
    scout.tick(1030);
    wblink::AirRxMeta meta{};
    meta.adapter_id = 0;
    for (const int8_t rssi : {int8_t{-70}, int8_t{-16}, int8_t{-55}}) {
        meta.rssi = rssi;                            // strongest is -16
        scout.on_frame(meta, announce, raw, sizeof(raw));
    }
    scout.stop(1100);
    const std::string json = scout.results_json(1100);
    CHECK(json.find("\"rssi_dbm\":-16") != std::string::npos);
    CHECK(json.find("\"rssi_dbm\":-70") == std::string::npos);
    // Occupancy stays fail-closed no matter what the sweep heard (#173).
    CHECK(json.find("\"duty_cycle_known\":false") != std::string::npos);
    CHECK(json.find("\"duty_cycle_known\":true") == std::string::npos);

    Spy silent;
    ScoutEngine no_rssi = make_scout(silent);
    CHECK(no_rssi.start({5805}, 250, 1000).empty());
    no_rssi.tick(1030);
    wblink::AirRxMeta unreported{};
    unreported.adapter_id = 0;
    unreported.rssi = 0;                             // radio reported nothing
    no_rssi.on_frame(unreported, announce, raw, sizeof(raw));
    no_rssi.stop(1100);
    const std::string quiet_json = no_rssi.results_json(1100);
    CHECK(quiet_json.find("\"originator\":17") != std::string::npos);
    CHECK(quiet_json.find("\"rssi_dbm\":null") != std::string::npos);
    CHECK(quiet_json.find("\"rssi_dbm\":0") == std::string::npos);
}

}  // namespace

int main() {
    test_scout_boots_idle_and_refuses_an_empty_sweep();
    test_scout_start_widens_the_filter_then_retunes();
    test_stop_restores_but_abandon_does_not();
    test_scout_rejects_pre_settle_candidate_residue_and_publishes_weight();
    test_scout_near_tie_prefers_proven_resting_channel();
    test_catalog_knows_nothing_about_an_unseen_originator();
    test_an_announced_token_survives_the_presence_view_aging_out();
    test_a_re_announced_token_replaces_the_cached_one();
    test_an_announce_without_a_token_caches_nothing();
    test_short_dwell_probes_but_anything_heard_extends();
    test_a_second_craft_on_one_channel_is_not_truncated_away();
    test_candidate_rssi_is_strongest_and_absent_reads_null();
    return wbtest_finish("node_discovery_test");
}
