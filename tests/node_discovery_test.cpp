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

    CHECK(scout.start({5745, 5805}, 300, 1000).empty());
    scout.tick(1030);
    for (int i = 0; i < 11; ++i) scout.on_frame(meta, announce, raw, sizeof(raw));
    scout.tick(1300);  // finalize 5745, enter 5805
    scout.tick(1330);
    for (int i = 0; i < 10; ++i) scout.on_frame(meta, announce, raw, sizeof(raw));
    scout.stop(1400);

    const auto resolved = scout.candidate_for(17);
    CHECK(resolved.has_value());
    if (resolved) CHECK_EQ_U(resolved->chan, 5805u);
    const std::string json = scout.results_json(1400);
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
    ambiguous.tick(1800);
    ambiguous.tick(1830);
    for (int i = 0; i < 10; ++i) {
        ambiguous.on_frame(meta, announce, raw, sizeof(raw));
    }
    ambiguous.stop(1900);
    CHECK(!ambiguous.candidate_for(17).has_value());
    const std::string ambiguous_json = ambiguous.results_json(1900);
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

}  // namespace

int main() {
    test_scout_boots_idle_and_refuses_an_empty_sweep();
    test_scout_start_widens_the_filter_then_retunes();
    test_stop_restores_but_abandon_does_not();
    test_scout_rejects_pre_settle_candidate_residue_and_publishes_weight();
    test_scout_near_tie_prefers_proven_resting_channel();
    test_catalog_knows_nothing_about_an_unseen_originator();
    return wbtest_finish("node_discovery_test");
}
