// SPDX-License-Identifier: GPL-2.0-or-later
// --check --strict: unknown keys, inert keys, and the predicates behind them.
//
// "Inert" means registered but not read on this node's mode — the failure the
// operator actually cares about, where a knob is set, believed, and does
// nothing. It cannot be inferred from the parse (the loader reads
// policy.return unconditionally at config.cpp:649; the gate is downstream at
// app/main.cpp:3846), so each predicate is declared and cited at its
// definition in io/src/config_registry.cpp.
#include "wblink/config.h"
#include "wblink/config_registry.h"

#include <cstdio>
#include <string>
#include <vector>

#include "wbtest.h"

using namespace wblink;

namespace {

// Minimal loadable node config. Callers splice extra stanzas in via `extra`.
// `air.kind` matters: has_uplink() is per backend, and only the RF ones follow
// the adapters (UdpAir::has_tx is hardcoded true). kernel-monitor is what
// deploy/ground-192.168.2.199.json actually flies.
const char* kUdpInStream =
    R"({ "stream_id": 0, "stream_type": "RTP", "dir": "in",
         "bind": { "kind": "udp", "listen": "127.0.0.1:5600" } })";
// cache.repair.enabled requires its stream_id to name a frame-shm EGRESS
// stream (§15.2, config.cpp), so the enabled fixture needs this shape.
const char* kShmOutStream =
    R"({ "stream_id": 0, "stream_type": "RTP", "dir": "out",
         "bind": { "kind": "frame-shm", "name": "wbtest_ring" } })";

std::string cfg_json(const std::string& adapters, const std::string& extra,
                     const std::string& air = R"({ "kind": "kernel-monitor" })",
                     const std::string& streams = kUdpInStream) {
    return std::string(R"({
  "node": { "originator": 7, "role": "rx" },
  "air": )" + air + R"(,
  "adapters": [)") + adapters + R"(],
  "streams": [)" + streams + R"(])" + extra + "\n}\n";
}

const char* kRxOnly =
    R"({ "name": "wlan0", "ifname": "wlan0", "role": "rx", "channel": 5805, "bw": 20 })";
const char* kTxAndRx =
    R"({ "name": "wlan0", "ifname": "wlan0", "role": "tx", "channel": 5805, "bw": 20 },)"
    R"({ "name": "wlan1", "ifname": "wlan1", "role": "rx", "channel": 5805, "bw": 20 })";

// Load, then classify. Fails the test rather than returning junk if the
// fixture itself does not load — a fixture that stopped parsing would
// otherwise read as "no findings".
std::vector<KeyFinding> findings_for(const std::string& text) {
    auto cfg = load_config_json(text);
    CHECK(static_cast<bool>(cfg));
    if (!cfg) {
        std::fprintf(stderr, "  fixture did not load: %s\n", cfg.error.c_str());
        return {};
    }
    return check_config_keys(text, *cfg.value);
}

bool has(const std::vector<KeyFinding>& v, const std::string& path,
         KeyVerdict want) {
    for (const KeyFinding& f : v) {
        if (f.path == path && f.verdict == want) return true;
    }
    return false;
}

const KeyEntry* entry(const std::string& path) {
    std::size_t n = 0;
    const KeyEntry* keys = config_registry(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (path == keys[i].path) return &keys[i];
    }
    return nullptr;
}

}  // namespace

int main() {
    // --- registry shape -----------------------------------------------------
    std::size_t n = 0;
    const KeyEntry* keys = config_registry(&n);
    for (std::size_t i = 0; i < n; ++i) {
        // Both or neither: a predicate with no reason prints an empty
        // explanation, and a reason with no predicate never fires.
        const bool paired = (keys[i].live == nullptr) ==
                            (keys[i].inert_reason == nullptr);
        CHECK(paired);
        if (!paired) std::fprintf(stderr, "  unpaired: %s\n", keys[i].path);
    }

    // The keys that ARE the gate must stay live. Predicating venc.enabled on
    // venc_enabled() would report the switch as inert exactly when it is off,
    // which is when an operator most needs to see it.
    for (const char* p : {"venc.enabled", "cache.repair.enabled",
                          "cache.store.enabled", "venc", "cache.repair",
                          "cache.store"}) {
        const KeyEntry* e = entry(p);
        CHECK(e != nullptr);
        if (e != nullptr) CHECK(e->live == nullptr);
    }

    // Regression guard: air.tx / air.rx / air.pace_mbps are udp-only, but
    // air.tx_retry_limit and air.rx_drop_permille merely share a prefix and
    // are radio-backend keys. A prefix rule marked them inert during
    // development, which is the "predicate reports a live key as dead" failure
    // the design is meant to avoid.
    for (const char* p : {"air.tx_retry_limit", "air.rx_drop_permille",
                          "air.ldpc", "air.stbc", "air.mcs_probe"}) {
        const KeyEntry* e = entry(p);
        CHECK(e != nullptr);
        if (e != nullptr && e->live != nullptr) {
            std::fprintf(stderr, "  %s must not be udp-gated\n", p);
            CHECK(false);
        }
    }
    for (const char* p : {"air.tx", "air.rx", "air.pace_mbps"}) {
        const KeyEntry* e = entry(p);
        CHECK(e != nullptr);
        if (e != nullptr) CHECK(e->live != nullptr);
    }

    // --- unknown keys -------------------------------------------------------
    {
        const auto f = findings_for(cfg_json(kRxOnly,
            R"(, "policy": { "report_hz": 10, "report_hzz": 11 })"));
        CHECK(has(f, "policy.report_hzz", KeyVerdict::kUnknown));
        CHECK(!has(f, "policy.report_hz", KeyVerdict::kUnknown));
    }

    // `_`-prefixed keys are comments by convention and the only keys allowed
    // to be unknown — at any depth.
    {
        const auto f = findings_for(cfg_json(kRxOnly,
            R"(, "_note": "x", "policy": { "_scratch": 1, "report_hz": 10 })"));
        CHECK(!has(f, "_note", KeyVerdict::kUnknown));
        CHECK(!has(f, "policy._scratch", KeyVerdict::kUnknown));
    }

    // --- inert: the uplink-free case, which is the whole point --------------
    // deploy/ground-192.168.2.199.json is this shape: a real spectator whose
    // policy.return block is dead text (§15.2, PROTOCOL.md:4510).
    {
        const std::string ret =
            R"(, "policy": { "return": { "guard_us": 300, "quiet_gap": true } })";
        const auto no_tx = findings_for(cfg_json(kRxOnly, ret));
        CHECK(has(no_tx, "policy.return.guard_us", KeyVerdict::kInert));
        CHECK(has(no_tx, "policy.return.quiet_gap", KeyVerdict::kInert));

        // One role:"tx" adapter is the only difference, and it must silence it.
        const auto with_tx = findings_for(cfg_json(kTxAndRx, ret));
        CHECK(!has(with_tx, "policy.return.guard_us", KeyVerdict::kInert));
        CHECK(!has(with_tx, "policy.return.quiet_gap", KeyVerdict::kInert));
    }

    // --- inert: subsystem switches -----------------------------------------
    {
        const std::string on =
            R"(, "venc": { "enabled": true, "host": "127.0.0.1:80" })";
        const std::string off =
            R"(, "venc": { "enabled": false, "host": "127.0.0.1:80" })";
        CHECK(has(findings_for(cfg_json(kTxAndRx, off)), "venc.host",
                  KeyVerdict::kInert));
        CHECK(!has(findings_for(cfg_json(kTxAndRx, on)), "venc.host",
                   KeyVerdict::kInert));
        // The switch itself is never reported.
        CHECK(!has(findings_for(cfg_json(kTxAndRx, off)), "venc.enabled",
                   KeyVerdict::kInert));
    }

    // --- inert: every remaining predicate group -----------------------------
    // Review found that inverting air_is_udp, cache_repair_enabled or
    // cache_store_enabled left the suite at 312 checks / 0 failures. A
    // predicate with no behavioural test is a predicate nobody is checking.
    {
        // pace_mbps is live on udp-broadcast and inert on an RF backend.
        const auto bcast = findings_for(cfg_json(kTxAndRx, "",
            R"({ "kind": "udp-broadcast", "tx": ["127.0.0.1:1"],
                 "rx": ["0.0.0.0:1"], "pace_mbps": 10 })"));
        CHECK(!has(bcast, "air.pace_mbps", KeyVerdict::kInert));
        const auto rf = findings_for(cfg_json(kTxAndRx,
            R"(, "policy": { "report_hz": 10 })"));
        CHECK(rf.empty());
    }
    {
        const std::string caches =
            R"("caches": [ { "originator": 9, "endpoint": "127.0.0.1:9201" } ],)"
            R"( "listen": "0.0.0.0:9200", "stream_id": 0)";
        const auto off = findings_for(cfg_json(kTxAndRx,
            R"(, "cache": { "repair": { "enabled": false, )" + caches + " } }"));
        CHECK(has(off, "cache.repair.listen", KeyVerdict::kInert));
        CHECK(!has(off, "cache.repair.enabled", KeyVerdict::kInert));
        const auto on = findings_for(cfg_json(kTxAndRx,
            R"(, "cache": { "repair": { "enabled": true, )" + caches + " } }",
            R"({ "kind": "kernel-monitor" })", kShmOutStream));
        CHECK(!has(on, "cache.repair.listen", KeyVerdict::kInert));
    }
    {
        const std::string store =
            R"("blocks": 8, "listen": "0.0.0.0:9300", "stream_ids": [0])";
        const auto off = findings_for(cfg_json(kTxAndRx,
            R"(, "cache": { "store": { "enabled": false, )" + store + " } }"));
        CHECK(has(off, "cache.store.blocks", KeyVerdict::kInert));
        CHECK(!has(off, "cache.store.enabled", KeyVerdict::kInert));
        const auto on = findings_for(cfg_json(kTxAndRx,
            R"(, "cache": { "store": { "enabled": true, )" + store + " } }"));
        CHECK(!has(on, "cache.store.blocks", KeyVerdict::kInert));
    }
    {
        // The §15.5 mode catalog is gated on venc.mode_apply_cmd, NOT on
        // venc.enabled (app/main.cpp:5083, :8508). Predicating it on
        // venc.enabled reported live keys dead on the §16 craft layout, which
        // is the one deployment that sets mode_apply_cmd.
        const auto no_cmd = findings_for(cfg_json(kTxAndRx,
            R"(, "venc": { "enabled": false, "modes_dir": "/etc/modes" })"));
        CHECK(has(no_cmd, "venc.modes_dir", KeyVerdict::kInert));
        const auto with_cmd = findings_for(cfg_json(kTxAndRx,
            R"(, "venc": { "enabled": false, "modes_dir": "/etc/modes",
                           "mode_apply_cmd": "/usr/bin/apply" })"));
        CHECK(!has(with_cmd, "venc.modes_dir", KeyVerdict::kInert));
        CHECK(!has(with_cmd, "venc.active_mode", KeyVerdict::kInert));
        // ...while the bitrate actuator keys stay gated on venc.enabled.
        CHECK(has(with_cmd, "venc.enabled", KeyVerdict::kInert) == false);
    }
    {
        // UdpAir::has_tx() is hardcoded true (air_udp.h:119) and udp configs
        // carry no adapters, so an adapters-only uplink test called every
        // policy.return key inert on a live udp node.
        const auto udp = findings_for(cfg_json("",
            R"(, "policy": { "return": { "guard_us": 300 } })",
            R"({ "kind": "udp", "tx": ["127.0.0.1:1"], "rx": ["0.0.0.0:1"] })"));
        CHECK(!has(udp, "policy.return.guard_us", KeyVerdict::kInert));
    }

    // Deep nesting must not recurse without bound: --check has no such
    // surface, so --strict must not add one.
    {
        std::string deep = R"(, "policy": )";
        const int kDepth = 5000;
        for (int i = 0; i < kDepth; ++i) deep += "{\"a\": ";
        deep += "1";
        for (int i = 0; i < kDepth; ++i) deep += "}";
        const std::string text = cfg_json(kTxAndRx, deep);
        auto cfg = load_config_json(text);
        // Whether it loads is not the point; classifying must not crash.
        if (cfg) {
            const auto f = check_config_keys(text, *cfg.value);
            CHECK(f.size() < 100);
        }
    }

    // A well-formed config with none of the above must be silent, or --strict
    // is noise and will be ignored.
    {
        const auto f = findings_for(cfg_json(kTxAndRx, ""));
        CHECK(f.empty());
        for (const KeyFinding& x : f) {
            std::fprintf(stderr, "  unexpected: %s\n", x.path.c_str());
        }
    }

    // --- report ------------------------------------------------------------
    {
        const auto f = findings_for(cfg_json(kRxOnly,
            R"(, "policy": { "return": { "guard_us": 300 } })"));
        const std::string js = check_report_json(f);
        CHECK(js.find("\"ok\": false") != std::string::npos);
        CHECK(js.find("\"verdict\": \"inert\"") != std::string::npos);
        // The reasons are prose containing quotes (role:"tx"), so the report
        // must escape them — this is why it uses a real serialiser.
        CHECK(js.find("role:\\\"tx\\\"") != std::string::npos);
        // No config VALUE ever reaches the report; only paths and verdicts.
        CHECK(js.find("300") == std::string::npos);

        const std::string clean = check_report_json({});
        CHECK(clean.find("\"ok\": true") != std::string::npos);
    }

    return wbtest_finish("config_strict");
}
