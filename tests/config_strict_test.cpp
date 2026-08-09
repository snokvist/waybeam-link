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
// `air.kind` matters: has_uplink() is per backend, and only the RF one follows
// the adapters (UdpAir::has_tx is hardcoded true). radio is what every flying
// node runs since Pass 164 deleted kernel-monitor.
const char* kUdpInStream =
    R"({ "stream_id": 0, "stream_type": "RTP", "dir": "in",
         "bind": { "kind": "udp", "listen": "127.0.0.1:5600" } })";
// cache.repair.enabled requires its stream_id to name a frame-shm EGRESS
// stream (§15.2, config.cpp), so the enabled fixture needs this shape.
const char* kShmOutStream =
    R"({ "stream_id": 0, "stream_type": "RTP", "dir": "out",
         "bind": { "kind": "frame-shm", "name": "wbtest_ring" } })";

std::string cfg_json(const std::string& adapters, const std::string& extra,
                     const std::string& air = R"({ "kind": "radio" })",
                     const std::string& streams = kUdpInStream) {
    return std::string(R"({
  "node": { "originator": 7, "role": "rx" },
  "air": )" + air + R"(,
  "adapters": [)") + adapters + R"(],
  "streams": [)" + streams + R"(])" + extra + "\n}\n";
}

const char* kRxOnly =
    R"({ "name": "wlan0", "bus": "1-1", "role": "rx", "channel": 5805, "bw": 20 })";
const char* kTxAndRx =
    R"({ "name": "wlan0", "bus": "1-1", "role": "tx", "channel": 5805, "bw": 20 },)"
    R"({ "name": "wlan1", "bus": "5-1", "role": "rx", "channel": 5805, "bw": 20 })";

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
    // The RK3566 spectator archetype: a real spectator whose policy.return
    // block is dead text (§15.2, PROTOCOL.md:4510). Its config,
    // deploy/ground-192.168.2.199.json, was deleted in Pass 164 (node offline,
    // could not be mirrored) -- this fixture is now the only copy of the shape.
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
            R"({ "kind": "radio" })", kShmOutStream));
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

    // --- Pass 164 stranded keys --------------------------------------------
    // adapters[].ifname and adapters[].calib_id lost their only reader when
    // the kernel-monitor backend was deleted. They are inert on EVERY
    // backend, so both arms of every fixture must report them.
    {
        const char* kIfnameAdapter =
            R"({ "name": "wlan0", "bus": "1-1", "ifname": "wlan0",
                 "calib_id": "craft-eu-1",
                 "role": "tx", "channel": 5805, "bw": 20 })";
        const auto radio = findings_for(cfg_json(kIfnameAdapter, ""));
        CHECK(has(radio, "adapters[].ifname", KeyVerdict::kInert));
        CHECK(has(radio, "adapters[].calib_id", KeyVerdict::kInert));
        // ...and on udp too — these are not backend-conditional.
        const auto udp = findings_for(cfg_json(kIfnameAdapter, "",
            R"({ "kind": "udp", "tx": ["127.0.0.1:1"], "rx": ["0.0.0.0:1"] })"));
        CHECK(has(udp, "adapters[].ifname", KeyVerdict::kInert));
        CHECK(has(udp, "adapters[].calib_id", KeyVerdict::kInert));
        // A config that omits them says nothing — inert must not be noise.
        const auto clean = findings_for(cfg_json(kTxAndRx, ""));
        CHECK(!has(clean, "adapters[].ifname", KeyVerdict::kInert));
        CHECK(!has(clean, "adapters[].calib_id", KeyVerdict::kInert));
    }
    // REGRESSION PIN (pre-merge review, Pass 164), NARROWED by Pass 166.
    //
    // max_power_qdb was briefly declared inert on radio and is NOT: §15.3
    // tx_power_ceiling_qdb, §15.5 GET /api/v1/tx/power_tier, the ground
    // UplinkPower::hw_qdb() override clamp that reaches the actuator, and the
    // load-time clamp of the presets all read it, and BOTH flying configs set
    // it. A predicate there tells the operator to delete a key that clamps TX
    // power. It stays unpredicated — see the DO NOT PREDICATE block in
    // io/src/config_registry.cpp.
    //
    // power_presets_qdb IS predicated as of Pass 166, and on `air.kind`
    // alone: a relative node's tier reads power_offset_presets_qdb instead,
    // selected at one site (TxCore::set_backend_relative / the run_rx
    // UplinkPower seed), so the readers listed above follow the space rather
    // than contradicting it. The two keys are pinned in OPPOSITE directions
    // here on purpose — that is the whole distinction Pass 164's review had
    // to draw after the fact.
    {
        const char* kAbsAdapter =
            R"({ "name": "wlan0", "bus": "1-1", "role": "tx",
                 "channel": 5805, "bw": 20, "max_power_qdb": 84,
                 "power_presets_qdb": [60, 76, 84] })";
        const auto radio = findings_for(cfg_json(kAbsAdapter, ""));
        CHECK(!has(radio, "adapters[].max_power_qdb", KeyVerdict::kInert));
        CHECK(has(radio, "adapters[].power_presets_qdb", KeyVerdict::kInert));
        const auto udp = findings_for(cfg_json(kAbsAdapter, "",
            R"({ "kind": "udp", "tx": ["127.0.0.1:1"], "rx": ["0.0.0.0:1"] })"));
        CHECK(!has(udp, "adapters[].max_power_qdb", KeyVerdict::kInert));
        CHECK(!has(udp, "adapters[].power_presets_qdb", KeyVerdict::kInert));

        // §10.3 (Pass 166) the other way round: the offset list is live on
        // radio and inert on udp. Without this the predicate could be
        // one-sided and still pass — the failure #148's predicates had.
        const char* kOffAdapter =
            R"({ "name": "wlan0", "bus": "1-1", "role": "tx",
                 "channel": 5805, "bw": 20, "power_offset_max_qdb": 24,
                 "power_offset_presets_qdb": [-72, -48, -24, 0, 24] })";
        const auto radio_off = findings_for(cfg_json(kOffAdapter, ""));
        CHECK(!has(radio_off, "adapters[].power_offset_presets_qdb",
                   KeyVerdict::kInert));
        const auto udp_off = findings_for(cfg_json(kOffAdapter, "",
            R"({ "kind": "udp", "tx": ["127.0.0.1:1"], "rx": ["0.0.0.0:1"] })"));
        CHECK(has(udp_off, "adapters[].power_offset_presets_qdb",
                  KeyVerdict::kInert));
        // Neither list is reported on a config that omits it.
        const auto clean = findings_for(cfg_json(kTxAndRx, ""));
        CHECK(!has(clean, "adapters[].power_presets_qdb", KeyVerdict::kInert));
        CHECK(!has(clean, "adapters[].power_offset_presets_qdb",
                   KeyVerdict::kInert));
    }
    // Prefix-bleed guard, the trap that bit air.tx_retry_limit in #148. These
    // are the RELATIVE §10.5 contract plus the absolute ceiling: every one is
    // live on every backend and must stay unpredicated. The two preset lists
    // are deliberately absent — Pass 166 predicates both, each on air.kind.
    for (const char* p : {"adapters[].power_offset_qdb",
                          "adapters[].power_offset_max_qdb",
                          "adapters[].max_power_qdb",
                          "adapters[].power_map", "adapters[].bus",
                          "adapters[].mac"}) {
        const KeyEntry* e = entry(p);
        CHECK(e != nullptr);
        if (e != nullptr && e->live != nullptr) {
            std::fprintf(stderr, "  %s must not be Pass-164-gated\n", p);
            CHECK(false);
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
