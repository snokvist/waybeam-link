// SPDX-License-Identifier: GPL-2.0-or-later
// Pass 179: config as text, and selection as a call.
//
// Two halves, deliberately tested at different levels. `apply_selection` and
// `load_all_json` are exercised directly against a real config, because that
// is where the BEHAVIOUR is and a C-ABI-only test could not see which
// adapters moved. The C shims are exercised for their CONTRACT — the
// exactly-one-source rule and the call-before-run window — which is all they
// own.
//
// Nothing here opens a radio: every run below is refused before load, or
// pre-stopped, exactly as node_fd_source_test is.
#include "wblink/node/load.h"
#include "wblink/node/rx_node_c.h"

#include <string>

#include "wblink/log.h"
#include "wbtest.h"

namespace {

// Two adapters on purpose: the whole point of apply_selection is that EVERY
// ear moves, and a single-adapter fixture cannot fail the wrong way.
const char* kTwoEarConfig = R"({
  "node": { "originator": 9, "role": "rx", "net_id": 3, "spectator": true },
  "adapters": [
    { "name": "a0", "bus": "1-1", "role": "rx", "channel": 5180, "bw": 20 },
    { "name": "a1", "bus": "1-2", "role": "rx", "channel": 5200, "bw": 20 }
  ],
  "streams": [
    { "stream_id": 0, "stream_type": "RTP", "dir": "out",
      "bind": { "kind": "udp", "send": "127.0.0.1:5600" } }
  ],
  "air": { "kind": "udp" },
  "stats": { "hz": 0, "stdout": false }
})";

void test_load_from_text_matches_a_loaded_config() {
    wblink::node::Loaded loaded;
    CHECK(wblink::node::load_all_json(kTwoEarConfig, loaded) == 0);
    CHECK_EQ_U(loaded.cfg.node.originator, 9);
    CHECK_EQ_U(loaded.cfg.adapters.size(), 2);
    // A config that does not parse is a failure, not an empty Config: the
    // whole reason the text path exists is to report this to the embedder.
    wblink::node::Loaded bad;
    CHECK(wblink::node::load_all_json("{ not json", bad) == 1);
    CHECK(wblink::node::load_all_json("", bad) == 1);
}

void test_selection_moves_every_ear() {
    wblink::node::Loaded loaded;
    CHECK(wblink::node::load_all_json(kTwoEarConfig, loaded) == 0);
    // Control: the fixture starts on two DIFFERENT channels and a net_id
    // that is not the one we pin, so an applier that did nothing, or moved
    // only the first ear, cannot pass.
    CHECK_EQ_U(loaded.cfg.adapters[0].channel_mhz, 5180);
    CHECK_EQ_U(loaded.cfg.adapters[1].channel_mhz, 5200);

    wblink::node::Selection sel;
    sel.originator = 17;
    sel.net_id = 42;
    sel.channel_mhz = 5805;
    wblink::node::apply_selection(sel, loaded);

    CHECK_EQ_U(loaded.cfg.node.preferred_originator, 17);
    CHECK(loaded.cfg.node.net_id.has_value());
    if (loaded.cfg.node.net_id) CHECK_EQ_U(*loaded.cfg.node.net_id, 42);
    CHECK_EQ_U(loaded.cfg.adapters[0].channel_mhz, 5805);
    CHECK_EQ_U(loaded.cfg.adapters[1].channel_mhz, 5805);
}

// A selection with net_id < 0 must LEAVE the config's value alone. nullopt is
// "accept any net_id" and is a different state from 0; §15.5a serialises the
// live selection as value_or(0), so a consumer round-tripping its own readout
// would otherwise pin the filter on 0 and go deaf.
void test_selection_leaves_net_id_alone_when_unspecified() {
    wblink::node::Loaded pinned;
    CHECK(wblink::node::load_all_json(kTwoEarConfig, pinned) == 0);
    CHECK(pinned.cfg.node.net_id.has_value());   // fixture has net_id 3
    wblink::node::Selection sel;
    sel.originator = 17;
    sel.channel_mhz = 5805;
    sel.net_id = -1;                             // "do not pin one"
    wblink::node::apply_selection(sel, pinned);
    CHECK(pinned.cfg.node.net_id.has_value());
    if (pinned.cfg.node.net_id) CHECK_EQ_U(*pinned.cfg.node.net_id, 3);
    // ...while the rest of the selection still applied.
    CHECK_EQ_U(pinned.cfg.node.preferred_originator, 17);
    CHECK_EQ_U(pinned.cfg.adapters[0].channel_mhz, 5805);
}

// Captures what the library logs, so the test can see inside a run.
std::string g_log;
void log_capture(void*, const char* msg, size_t n) { g_log.append(msg, n); }

// THE C-ABI WIRING, which nothing previously covered (2026-08-15 review): the
// old test pre-stopped the handle, and the pre-stop return sits ABOVE the load
// — so replacing the whole config-source ternary with an unconditional
// load_all left the suite green.
//
// Reaching the load without opening a radio needs a config that fails INSIDE
// load_finish rather than before it: a profile_table path that does not exist
// fails after the summary is logged and long before any backend is created.
// The summary is then the evidence, and it proves both halves at once — that
// the TEXT config was loaded, and that the SELECTION was applied to it.
void test_c_abi_run_loads_the_text_config_and_applies_the_selection() {
    // Same shape as the fixture, plus a profile_table that does not exist.
    const char* cfg = R"({
  "node": { "originator": 9, "role": "rx", "net_id": 3, "spectator": true },
  "profile_table": "/nonexistent/wblink-table.json",
  "adapters": [
    { "name": "a0", "bus": "1-1", "role": "rx", "channel": 5180, "bw": 20 },
    { "name": "a1", "bus": "1-2", "role": "rx", "channel": 5200, "bw": 20 }
  ],
  "streams": [
    { "stream_id": 0, "stream_type": "RTP", "dir": "out",
      "bind": { "kind": "udp", "send": "127.0.0.1:5600" } }
  ],
  "air": { "kind": "udp" },
  "stats": { "hz": 0, "stdout": false }
})";
    wblink_rx* rx = wblink_rx_create();
    CHECK(rx != nullptr);
    CHECK(wblink_rx_set_config_json(rx, cfg) == 0);
    CHECK(wblink_rx_set_selection(rx, 17, 42, 5805) == 0);

    g_log.clear();
    const wblink::LogSink sink{&log_capture, nullptr};
    wblink::wb_log_set_sink(&sink);
    const int rc = wblink_rx_run(rx, nullptr, nullptr, nullptr);
    wblink::wb_log_set_sink(nullptr);

    // Failed at the profile table, i.e. inside load_finish — so the load ran.
    CHECK(rc == 1);
    CHECK(g_log.find("profile table error") != std::string::npos);
    // The text config is what was loaded: the summary names its originator.
    CHECK(g_log.find("node 9") != std::string::npos);
    // ...and the selection reached it BEFORE the summary was written: the
    // fixture's ears are 5180/5200 and the pin is 5805.
    CHECK(g_log.find("5805") != std::string::npos);
    CHECK(g_log.find("5180") == std::string::npos);
    wblink_rx_destroy(rx);
}

void test_c_abi_contract() {
    CHECK(wblink_rx_set_config_json(nullptr, "{}") == 2);
    CHECK(wblink_rx_set_selection(nullptr, 1, 0, 5805) == 2);

    wblink_rx* rx = wblink_rx_create();
    CHECK(rx != nullptr);
    CHECK(wblink_rx_set_config_json(rx, nullptr) == 2);
    CHECK(wblink_rx_set_config_json(rx, "") == 2);
    // §12's "no preference" sentinel and a nowhere channel are caller bugs.
    CHECK(wblink_rx_set_selection(rx, 0, 0, 5805) == 2);
    CHECK(wblink_rx_set_selection(rx, 17, 0, 0) == 2);
    CHECK(wblink_rx_set_selection(rx, 17, 256, 5805) == 2);  // >255
    CHECK(wblink_rx_set_selection(rx, 17, -1, 5805) == 0);   // unspecified
    CHECK(wblink_rx_set_selection(rx, 17, 42, 5805) == 0);

    // Exactly one config source. With nothing set, a NULL path is still the
    // refusal it always was.
    CHECK(wblink_rx_run(rx, nullptr, nullptr, nullptr) == 2);
    CHECK(wblink_rx_set_config_json(rx, kTwoEarConfig) == 0);
    // ...and now a NON-NULL path is the refusal, because two sources cannot
    // be ranked. This must not consume the handle.
    CHECK(wblink_rx_run(rx, "/nonexistent/config.json", nullptr, nullptr) == 2);
    int rc = 55;
    CHECK(wblink_rx_state(rx, &rc) == WBLINK_NODE_CREATED);
    CHECK(rc == 55);

    // The text path runs: pre-stop so it returns without opening a radio,
    // which is what makes this assertion safe to make with a valid config.
    wblink_rx_request_stop(rx);
    CHECK(wblink_rx_run(rx, nullptr, nullptr, nullptr) == 0);
    CHECK(wblink_rx_state(rx, &rc) == WBLINK_NODE_EXITED);
    CHECK(rc == 0);
    // Both setters are call-before-run and say so after it.
    CHECK(wblink_rx_set_config_json(rx, kTwoEarConfig) == 3);
    CHECK(wblink_rx_set_selection(rx, 17, 42, 5805) == 3);
    wblink_rx_destroy(rx);
}

}  // namespace

int main() {
    test_load_from_text_matches_a_loaded_config();
    test_selection_moves_every_ear();
    test_selection_leaves_net_id_alone_when_unspecified();
    test_c_abi_run_loads_the_text_config_and_applies_the_selection();
    test_c_abi_contract();
    return wbtest_finish("node_config_api_test");
}
