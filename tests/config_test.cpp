// SPDX-License-Identifier: GPL-2.0-or-later
// §15.2 config loader + §9.3 profile-table loader: the spec sample parses to
// the expected structs, absent keys keep spec-seed defaults, and every §15.1
// load-time rule rejects with a specific error. Also cross-validates the
// file-based table loader against the golden hash pinned in
// table_hash_test (proves the frac -> per-mille scaling path).
#include "wblink/config.h"
#include "wblink/selector.h"

#include <string>
#include <vector>

#include "wbtest.h"

using namespace wblink;

namespace {

// The §15.2 sample (psk included; craft/ground shape).
const char* kSample = R"({
  "node":  { "originator": 17, "role": "tx", "preferred_originator": 9 },
  "profile_table": "/etc/waybeam-link/profiles.json",
  "adapters": [
    { "name": "wlan0", "bus": "1-1.2", "role": "tx",
      "channel": 5805, "bw": 20,
      "power_map": "/etc/waybeam-link/power.wlan0.txt",
      "max_power_qdb": 108,
      "power_presets_qdb": [60, 76, 84] },
    { "name": "wlan1", "bus": "1-1.3", "role": "rx", "channel": 5805, "bw": 20 }
  ],
  "streams": [
    { "stream_id": 0, "stream_type": "RTP", "dir": "in", "classifier": "h265",
      "bind": { "kind": "udp", "listen": "127.0.0.1:5600" } },
    { "stream_id": 1, "stream_type": "TELEMETRY", "dir": "in",
      "bind": { "kind": "udp", "listen": "127.0.0.1:14650" } }
  ],
  "policy": {
    "report_hz": 10, "report_timeout_ms": 500,
    "select": { "demote_milli": 20, "emergency_loss_milli": 180,
                "verdict_ttl_s": 4.5,
                "loss_min_uniq": 40, "loss_persist_score": 6,
                "rung_lockout_s": 12.5, "rung_lockout_latch_count": 5,
                "rssi_floor_dbm": -85,
                "rssi_fade_db_per_s": 10, "rssi_fade_arm_dbm": -65,
                "promote_rssi_hyst_db": 6, "promote_dwell_s": 0.5,
                "mcs_settle_s": 5.0, "down_cooldown_s": 0.2,
                "ewma_alpha": 0.3 },
    "arq":    { "airtime_frac": 0.15, "attempt_cap": 3, "holddown_ms": 20,
                "fwd_clamp_blocks": 4 },
    "fec":    { "scheme": "none", "overhead_frac": 0.0 },
    "return": { "guard_us": 300, "return_window_us": 2000 },
    "csa":    { "psk": "hunter2",
                "settle_s": 3.0, "verify_timeout_ms": 150,
                "min_interval_s": 5, "ack_timeout_ms": 1000,
                "bind_release_s": 90, "persist_channel": true,
                "home_chan": 5745,
                "channel_allowlist": [5745, 5805, 5825] },
    "cmd":    { "copies": 4, "copy_interval_ms": 25, "echo_copies": 3,
                "ack_timeout_ms": 800, "retry_cap": 2,
                "min_interval_ms": 300 }
  },
  "stats": { "hz": 1, "bind": { "kind": "udp", "send": "127.0.0.1:9110" } },
  "scout": { "dwell_ms": 250, "channels": [5745, 5825] }
})";

bool expect_error(const std::string& json, const char* needle) {
    auto r = load_config_json(json);
    ++wbtest::checks;
    if (r) {
        std::fprintf(stderr, "expected rejection containing \"%s\", got OK\n",
                     needle);
        ++wbtest::failures;
        return false;
    }
    ++wbtest::checks;
    if (r.error.find(needle) == std::string::npos) {
        std::fprintf(stderr, "error \"%s\" does not mention \"%s\"\n",
                     r.error.c_str(), needle);
        ++wbtest::failures;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    // --- the spec sample parses to the expected structs --------------------
    {
        auto r = load_config_json(kSample);
        CHECK(bool(r));
        if (r) {
            const Config& c = *r.value;
            CHECK_EQ_U(c.node.originator, 17);
            CHECK(c.node.role == Role::kTx);
            CHECK(!c.node.spectator);  // §2 Pass 74: default off
            CHECK_EQ_U(c.node.preferred_originator, 9);
            CHECK_EQ_U(c.scout.dwell_ms, 250);
            CHECK_EQ_U(c.scout.channels.size(), 2);
            CHECK(c.profile_table_path == "/etc/waybeam-link/profiles.json");

            CHECK_EQ_U(c.adapters.size(), 2);
            CHECK(c.adapters[0].name == "wlan0");
            CHECK(c.adapters[0].role == Role::kTx);
            CHECK_EQ_U(c.adapters[0].channel_mhz, 5805);
            CHECK_EQ_U(c.adapters[0].bw, 20);
            CHECK(c.adapters[0].max_power_qdb.has_value() &&
                  *c.adapters[0].max_power_qdb == 108);
            CHECK(!c.adapters[1].max_power_qdb.has_value());
            // §11.7 0x0A (Pass 135): all three sit under max_power_qdb, so
            // none is clamped and the list survives verbatim.
            CHECK_EQ_U(c.adapters[0].power_presets_qdb.size(), 3);
            CHECK(c.adapters[0].power_presets_qdb ==
                  std::vector<int32_t>({60, 76, 84}));
            CHECK(c.adapters[1].power_presets_qdb.empty());

            CHECK_EQ_U(c.streams.size(), 2);
            CHECK_EQ_U(c.streams[0].stream_id, 0);
            CHECK_EQ_U(c.streams[0].stream_type, stream_type::kRtp);
            CHECK(c.streams[0].dir == Dir::kIn);
            CHECK(c.streams[0].bind.listen == "127.0.0.1:5600");
            CHECK(c.streams[0].classifier == RtpClassifier::kH265);
            CHECK_EQ_U(c.streams[1].stream_type, stream_type::kTelemetry);
            CHECK(c.streams[1].classifier == RtpClassifier::kSize);

            CHECK_EQ_U(c.policy.select.demote_milli, 20);
            CHECK_EQ_U(c.policy.select.emergency_loss_milli, 180);
            CHECK_EQ_U(c.policy.select.loss_min_uniq, 40);
            CHECK_EQ_U(c.policy.select.loss_persist_score, 6);
            CHECK(c.policy.select.rung_lockout_s == 12.5);
            CHECK_EQ_U(c.policy.select.rung_lockout_latch_count, 5);
            CHECK(c.policy.select.rssi_floor_dbm == -85);
            // §9.4 Pass 160: value-visible parse proof (an unknown key
            // would be silently ignored — the loader never enumerates).
            CHECK(c.policy.select.verdict_ttl_s == 4.5);
            CHECK_EQ_U(c.policy.arq.fwd_clamp_blocks, 4);
            CHECK(c.policy.fec.scheme == FecScheme::kNone);
            CHECK_EQ_U(c.policy.ret.guard_us, 300);
            CHECK_EQ_U(c.policy.ret.return_window_us, 2000);
            CHECK(c.policy.csa.psk == "hunter2");
            CHECK_EQ_U(c.policy.csa.home_chan, 5745);
            CHECK_EQ_U(c.policy.csa.bind_release_s, 90);
            CHECK(c.policy.csa.persist_channel);
            CHECK_EQ_U(c.policy.csa.channel_allowlist.size(), 3);
            CHECK_EQ_U(c.policy.cmd.copies, 4);
            CHECK_EQ_U(c.policy.cmd.copy_interval_ms, 25);
            CHECK_EQ_U(c.policy.cmd.echo_copies, 3);
            CHECK_EQ_U(c.policy.cmd.ack_timeout_ms, 800);
            CHECK_EQ_U(c.policy.cmd.retry_cap, 2);
            CHECK_EQ_U(c.policy.cmd.min_interval_ms, 300);

            CHECK(c.stats.bind.has_value());
            CHECK(c.stats.bind->send == "127.0.0.1:9110");

            // The secret must never leak through the summary.
            const std::string summary = dump_config_summary(c);
            CHECK(summary.find("hunter2") == std::string::npos);
            CHECK(summary.find("redacted") != std::string::npos);
        }
    }

    // --- Pass 110 selector classifier/lockout boundaries ------------------
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"select":{"demote_milli":45,
                          "emergency_loss_milli":45}}})",
                 "demote_milli < emergency_loss_milli");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"select":{"loss_min_uniq":0}}})",
                 "loss_min_uniq");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"select":{"loss_persist_score":256}}})",
                 "integer widths");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"select":{"rung_lockout_latch_count":256}}})",
                 "integer widths");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"select":{"rung_lockout_s":5000000}}})",
                 "representable in milliseconds");

    // --- defaults: a minimal config keeps every spec seed ------------------
    {
        auto r = load_config_json(R"({"node":{"originator":9,"role":"rx"}})");
        CHECK(bool(r));
        if (r) {
            const Config& c = *r.value;
            CHECK(c.node.role == Role::kRx);
            CHECK_EQ_U(c.node.preferred_originator, 0);
            CHECK_EQ_U(c.policy.report_timeout_ms, 500);
            CHECK_EQ_U(c.policy.select.demote_milli, 45);  // §17 re-derive 2026-07-26
            CHECK_EQ_U(c.policy.select.emergency_loss_milli, 200);
            CHECK_EQ_U(c.policy.select.loss_min_uniq, 32);
            CHECK_EQ_U(c.policy.select.loss_persist_score, 5);
            CHECK(c.policy.select.rung_lockout_s == 30.0);
            CHECK_EQ_U(c.policy.select.rung_lockout_latch_count, 4);
            CHECK(c.policy.select.ewma_alpha == 0.3);
            CHECK_EQ_U(c.policy.arq.holddown_ms, 20);
            CHECK_EQ_U(c.policy.rx.renack_backoff_ms, 6);
            CHECK_EQ_U(c.policy.rx.clamp_resync_ms, 500);  // §6.6 seed
            CHECK_EQ_U(c.policy.ret.guard_us, 300);
            CHECK(!c.policy.ret.quiet_gap);   // §7.1 baseline ships default
            CHECK_EQ_U(c.policy.ret.report_redundancy, 2);  // Pass 78 seed
            CHECK_EQ_U(c.policy.csa.rx_liveness_ms, 750);   // Pass 80 seed
            CHECK(!c.node.net_id.has_value());  // §3.0: accept-any when absent
            CHECK(c.policy.csa.psk.empty());  // spectator: no psk
            CHECK(c.stats.hz == 1.0);
            CHECK(!c.stats.bind.has_value());
            CHECK(c.stats.to_stdout);  // A2: default on (bench/dev stream)
        }
    }

    // --- A2: stats.stdout gates the §15.3 stdout NDJSON (production quiet) --
    {
        auto r = load_config_json(
            R"({"node":{"originator":9,"role":"rx"},"stats":{"stdout":false}})");
        CHECK(bool(r));
        if (r) {
            CHECK(!r.value->stats.to_stdout);
        }
    }

    // --- §3.0 net_id + air "radio" + §7.2 quiet-gap knobs -------------------
    {
        auto r = load_config_json(R"({
          "node": {"originator": 3, "role": "rx", "net_id": 5},
          "air": {"kind": "radio", "wedge_window_ms": 500,
                  "wedge_min_submits": 4, "ack_responder": true},
          "policy": {"return": {"quiet_gap": true, "guard_us": 400,
                                "return_window_us": 1500,
                                "unicast": true,
                                "report_redundancy": 1}}})");
        CHECK(bool(r));
        if (r) {
            const Config& c = *r.value;
            CHECK(c.node.net_id.has_value());
            CHECK_EQ_U(*c.node.net_id, 5);
            CHECK(c.air.kind == AirCfg::Kind::kRadio);
            CHECK_EQ_U(c.air.wedge_window_ms, 500);
            CHECK_EQ_U(c.air.wedge_min_submits, 4);
            CHECK(c.air.ack_responder);
            CHECK(c.policy.ret.quiet_gap);
            CHECK_EQ_U(c.policy.ret.guard_us, 400);
            CHECK_EQ_U(c.policy.ret.return_window_us, 1500);
            CHECK(c.policy.ret.unicast);
            CHECK_EQ_U(c.policy.ret.report_redundancy, 1);  // Pass 78
        }
    }
    // Pass 78: report_redundancy 0 is invalid (1 disables).
    {
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "policy":{"return":{"report_redundancy": 0}}})",
                     "report_redundancy");
    }
    // §9.10 wedge knobs + Pass-12 hybrid halves default off / to seeds.
    {
        auto r = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"}, "air": {"kind": "radio"}})");
        CHECK(bool(r));
        if (r) {
            CHECK_EQ_U(r.value->air.wedge_window_ms, 1000);
            CHECK_EQ_U(r.value->air.wedge_min_submits, 8);
            CHECK(!r.value->air.ack_responder);
            CHECK(!r.value->policy.ret.unicast);
        }
    }

    // --- §15.2 usb_tx_agg (Pass 184) ---------------------------------------
    {
        // Default is OFF, which is what keeps every existing deployment on
        // the per-frame path byte for byte.
        auto r = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"}, "air": {"kind": "radio"}})");
        CHECK(bool(r));
        if (r) CHECK_EQ_U(r.value->air.usb_tx_agg, 0);
        auto r3 = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"},
          "air": {"kind": "radio", "usb_tx_agg": 3}})");
        CHECK(bool(r3));
        if (r3) CHECK_EQ_U(r3.value->air.usb_tx_agg, 3);
        // Above the HalMAC BLK_DESC_NUM ceiling is a config ERROR, not a
        // silent clamp: a config saying 8 would otherwise quietly mean 3.
        auto r4 = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"},
          "air": {"kind": "radio", "usb_tx_agg": 8}})");
        CHECK(!bool(r4));
        // A bulk-OUT URB is a USB-radio concept; on udp-air the key would
        // read enabled while doing nothing.
        auto r5 = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"},
          "air": {"kind": "udp", "usb_tx_agg": 3}})");
        CHECK(!bool(r5));
        // ...but omitting it on udp-air is fine — the refusal is about a
        // SET key, not about the backend carrying a default.
        auto r6 = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"}, "air": {"kind": "udp"}})");
        CHECK(bool(r6));
    }

    // --- §3.0/§15.2 tx_retry_limit coupling (Pass 156) ----------------------
    {
        // Default seeds 8 (operator-ruled); parse override works.
        auto r = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"}, "air": {"kind": "radio"}})");
        CHECK(bool(r));
        if (r) CHECK_EQ_U(r.value->air.tx_retry_limit, 8);
        auto r2 = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"},
          "air": {"kind": "radio", "tx_retry_limit": 16}})");
        CHECK(bool(r2));
        if (r2) CHECK_EQ_U(r2.value->air.tx_retry_limit, 16);
    }
    // The hybrid armed with retries disabled is refused, not run inert —
    // either half arms it.
    {
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"radio","ack_responder":true,"tx_retry_limit":0}})",
                     "silently inert");
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"radio","tx_retry_limit":0},
          "policy":{"return":{"unicast":true}}})",
                     "silently inert");
    }
    // Limit 0 alone is legal (the WFB posture, hybrid off) — and the
    // non-radio control: the coupling law is radio-only, so a udp node may
    // pair a zero limit with unicast returns without tripping it.
    {
        auto r = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"},
          "air": {"kind": "radio", "tx_retry_limit": 0}})");
        CHECK(bool(r));
        auto r2 = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"},
          "air": {"kind": "udp", "tx_retry_limit": 0},
          "policy": {"return": {"unicast": true}}})");
        CHECK(bool(r2));
    }
    // Descriptor field width is enforced — both arms, and the boundary
    // value itself is accepted.
    {
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"radio","tx_retry_limit":64}})",
                     "out of range 0..63");
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"radio","tx_retry_limit":-1}})",
                     "out of range 0..63");
        auto r = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"},
          "air": {"kind": "radio", "tx_retry_limit": 63}})");
        CHECK(bool(r));
        if (r) CHECK_EQ_U(r.value->air.tx_retry_limit, 63);
    }
    // --- §15.2 air.ldpc / air.stbc (Pass 157) -------------------------------
    // Defaults off; radio accepts both; any other backend refuses each key
    // (Pass 154 mac posture — a dead key off the radio backend).
    {
        auto r = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"}, "air": {"kind": "radio"}})");
        CHECK(bool(r));
        if (r) {
            CHECK(!r.value->air.ldpc);
            CHECK(!r.value->air.stbc);
        }
        auto r2 = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"},
          "air": {"kind": "radio", "ldpc": true, "stbc": true}})");
        CHECK(bool(r2));
        if (r2) {
            CHECK(r2.value->air.ldpc);
            CHECK(r2.value->air.stbc);
        }
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"udp","ldpc":true}})",
                     "air.ldpc is a radio-backend key");
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"udp-broadcast","stbc":true,
                 "tx":["127.0.0.1:5801"],"rx":["127.0.0.1:5810"]}})",
                     "air.stbc is a radio-backend key");
    }
    // --- §15.2 air.mcs_probe (Pass 163) -------------------------------------
    // Default off; radio TX node accepts; any other backend refuses (same
    // posture), and so does an rx-role node (silently dead knob otherwise).
    {
        auto r = load_config_json(R"({
          "node": {"originator": 3, "role": "tx"},
          "air": {"kind": "radio", "mcs_probe": true}})");
        CHECK(bool(r));
        if (r) CHECK(r.value->air.mcs_probe);
        expect_error(R"({"node":{"originator":3,"role":"tx"},
          "air":{"kind":"udp","mcs_probe":true}})",
                     "air.mcs_probe is a radio-backend key");
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"radio","mcs_probe":true}})",
                     "air.mcs_probe is a TX-node key");
        expect_error(R"({"node":{"originator":3,"role":"tx"},
          "air":{"kind":"radio"},
          "policy":{"select":{"probe_veto_permille":1500}}})",
                     "probe_veto_permille");
    }
    // Both hybrid halves on at once: the refusal names ack_responder (the
    // message ternary's first branch).
    {
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"radio","ack_responder":true,"tx_retry_limit":0},
          "policy":{"return":{"unicast":true}}})",
                     "air.ack_responder");
        auto r = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"},
          "air": {"kind": "udp", "tx_retry_limit": 0},
          "policy": {"return": {"unicast": true}}})");
        CHECK(bool(r));
    }

    // --- §15.2 adapters[].mac (Pass 154) ------------------------------------
    {
        // Parsed and normalized to lowercase on the radio backend.
        auto r = load_config_json(R"({
          "node": {"originator": 3, "role": "rx"},
          "air": {"kind": "radio"},
          "adapters": [
            { "name": "up", "role": "tx", "channel": 5805,
              "mac": "84:FC:14:50:BC:DE" },
            { "name": "ear", "role": "rx", "channel": 5805 }]})");
        CHECK(bool(r));
        if (r) {
            CHECK(r.value->adapters[0].mac == "84:fc:14:50:bc:de");
            CHECK(r.value->adapters[1].mac.empty());
        }
    }
    // Malformed MACs are a config error, not a silently dead pin.
    {
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"radio"},
          "adapters":[{"name":"up","role":"tx","channel":5805,
                       "mac":"84fc1450bcde"}]})",
                     "mac must be");
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"radio"},
          "adapters":[{"name":"up","role":"tx","channel":5805,
                       "mac":"84:fc:14:50:bc:zz"}]})",
                     "mac must be");
    }
    // Two stanzas pinned to one unit cannot both bind it.
    {
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"radio"},
          "adapters":[
            {"name":"a","role":"tx","channel":5805,"mac":"84:fc:14:50:bc:de"},
            {"name":"b","role":"rx","channel":5805,"mac":"84:FC:14:50:BC:DE"}]})",
                     "duplicate mac");
    }
    // The key is radio-only: on any other backend it would promise a binding
    // that never happens.
    {
        expect_error(R"({"node":{"originator":3,"role":"rx"},
          "air":{"kind":"udp"},
          "adapters":[{"name":"up","role":"tx",
                       "channel":5805,"mac":"84:fc:14:50:bc:de"}]})",
                     "radio-backend key");
    }

    // --- frame-shm binding + fec block (§15.4/§14.1) ------------------------
    {
        auto r = load_config_json(R"({
          "node": {"originator": 7, "role": "tx"},
          "streams": [{ "stream_id": 0, "stream_type": "RTP", "dir": "in",
            "bind": { "kind": "frame-shm", "name": "venc_frame" },
            "arq_mode": "all-frames",
            "fec": { "scheme": "rlc256", "i_rate_permille": 300,
                     "p_rate_permille": 120, "min_k": 4 } }]})");
        CHECK(bool(r));
        if (r) {
            CHECK_EQ_U(r.value->streams.size(), 1u);
            const StreamCfg& s = r.value->streams[0];
            CHECK(s.bind.kind == BindKind::kFrameShm);
            CHECK(s.bind.name == "venc_frame");
            CHECK(s.arq_mode == FrameArqMode::kAllFrames);
            CHECK(s.fec.scheme == FecScheme::kRlc256);
            CHECK_EQ_U(s.fec.i_rate_permille, 300u);
            CHECK_EQ_U(s.fec.p_rate_permille, 120u);
            CHECK_EQ_U(s.fec.min_k, 4u);
            // §14.1a: absent e_rate_permille => unset (inherit p_rate).
            CHECK(!s.fec.e_rate_permille.has_value());
        }
        expect_error(R"({"node":{"originator":7,"role":"tx"},
          "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
            "bind":{"kind":"udp","listen":"127.0.0.1:5600"},
            "arq_mode":"all-frames"}]})", "frame-shm ingress");
        expect_error(R"({"node":{"originator":7,"role":"tx"},
          "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
            "bind":{"kind":"frame-shm","name":"venc_frame"},
            "arq_mode":"sometimes"}]})", "idr-only");
    }

    // §14.2 Pass 143: the authored calibration is a transport-efficiency
    // measurement, so it is valid on the radio backend too — and still
    // rejected on the udp bench transports (below).
    {
        auto r = load_config_json(R"({
          "node": {"originator": 7, "role": "rx"},
          "air": {"kind": "radio", "airtime_efficiency_permille": 550},
          "adapters": [
            {"name": "uplink", "bus": "1-1", "role": "tx", "channel": 5805}
          ]})");
        CHECK(bool(r));
        if (r) {
            CHECK(r.value->air.kind == AirCfg::Kind::kRadio);
            CHECK_EQ_U(r.value->air.airtime_efficiency_permille, 550);
        }
    }

    // --- air "radio" backend + the retired kernel-monitor value -------------
    {
        auto r = load_config_json(R"({
          "node": {"originator": 7, "role": "rx", "net_id": 2},
          "air": {"kind": "radio", "rx_drop_permille": 30,
                  "airtime_efficiency_permille": 600},
          "adapters": [
            {"name": "uplink", "bus": "1-1", "role": "tx", "channel": 5805},
            {"name": "div0", "bus": "5-1", "ifname": "legacy0",
             "role": "rx", "channel": 5805}
          ]})");
        CHECK(bool(r));
        if (r) {
            const Config& c = *r.value;
            CHECK(c.air.kind == AirCfg::Kind::kRadio);
            CHECK_EQ_U(c.air.rx_drop_permille, 30);
            CHECK_EQ_U(c.air.airtime_efficiency_permille, 600);
            CHECK_EQ_U(c.adapters.size(), 2);
            CHECK(c.adapters[0].bus == "1-1");
            CHECK(c.adapters[0].role == Role::kTx);
            CHECK(c.adapters[1].bus == "5-1");
            CHECK(c.adapters[1].role == Role::kRx);
            // adapters[].ifname is INERT since Pass 164 but still PARSED, and
            // config_registry_test.py ties the registry entry to the accessor
            // site. Pin the parse: dropping the accessor would silently drop
            // the registry entry with it.
            CHECK(c.adapters[1].ifname == "legacy0");
        }
        // Pass 164: the value is REJECTED, and the message names the
        // retirement rather than reporting an unknown kind — every pre-164
        // RX/spectator config on the shelf carries it. This is the negative
        // control for the deletion: without it, a stale config would fail
        // with a generic "unknown" and read as a typo.
        expect_error(R"({
          "node": {"originator": 7, "role": "rx"},
          "air": {"kind": "kernel-monitor"},
          "adapters": [
            {"name": "uplink", "ifname": "wlan0", "role": "tx",
             "channel": 5805}
          ]})",
                     "retired in Pass 164");
        // ...while a genuinely unknown kind still reports the live value set,
        // which no longer offers kernel-monitor.
        expect_error(R"({"node":{"originator":7,"role":"rx"},
          "air":{"kind":"nonsense"}})",
                     "udp | udp-broadcast | radio)");
    }

    // --- §15.1 rejection paths ---------------------------------------------
    // in-stream with a "send" binding (in XOR out).
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
        "bind":{"kind":"udp","send":"127.0.0.1:1"}}]})",
                 "\"listen\"");
    // binding with both listen and send.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
        "bind":{"kind":"udp","listen":"127.0.0.1:1","send":"127.0.0.1:2"}}]})",
                 "XOR");
    // unix "shm" kind is still v1-reserved; udp + frame-shm are the live kinds.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
        "bind":{"kind":"shm","listen":"x"}}]})",
                 "v1 feature");
    // frame-shm needs a name.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
        "bind":{"kind":"frame-shm"}}]})",
                 "name");
    // §14.1a fec.e_rate_permille: present, explicit null, and the bounds.
    {
        auto r = load_config_json(R"({
          "node": {"originator": 7, "role": "tx"},
          "streams": [{ "stream_id": 0, "stream_type": "RTP", "dir": "in",
            "bind": { "kind": "frame-shm", "name": "venc_frame" },
            "fec": { "scheme": "rlc256", "e_rate_permille": 0 } }]})");
        CHECK(bool(r));
        if (r) {
            const StreamCfg& s = r.value->streams[0];
            CHECK(s.fec.e_rate_permille.has_value());
            CHECK_EQ_U(*s.fec.e_rate_permille, 0u);  // 0 != unset
        }
        // Explicit null is the same as absent — "inherit p_rate".
        auto n = load_config_json(R"({
          "node": {"originator": 7, "role": "tx"},
          "streams": [{ "stream_id": 0, "stream_type": "RTP", "dir": "in",
            "bind": { "kind": "frame-shm", "name": "venc_frame" },
            "fec": { "scheme": "rlc256", "e_rate_permille": null } }]})");
        CHECK(bool(n));
        if (n) CHECK(!n.value->streams[0].fec.e_rate_permille.has_value());
    }
    expect_error(R"({"node":{"originator":7,"role":"tx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
        "bind":{"kind":"frame-shm","name":"venc_frame"},
        "fec":{"scheme":"rlc256","e_rate_permille":4001}}]})",
                 "0..4000");
    // A rate with no scheme to apply it is a config error, not a silent no-op.
    expect_error(R"({"node":{"originator":7,"role":"tx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
        "bind":{"kind":"frame-shm","name":"venc_frame"},
        "fec":{"scheme":"none","e_rate_permille":0}}]})",
                 "rlc256");
    // §10.5 (Pass 150): the boot offset may not start above its own bound —
    // a node must never boot at a power the runtime latch would refuse.
    expect_error(R"({"node":{"originator":7,"role":"tx"},
      "adapters":[{"name":"wlan0","role":"tx","channel":5805,
        "power_offset_qdb":8,"power_offset_max_qdb":0}]})",
                 "exceeds power_offset_max_qdb");
    expect_error(R"({"node":{"originator":7,"role":"tx"},
      "adapters":[{"name":"wlan0","role":"tx","channel":5805,
        "power_offset_qdb":-9999}]})",
                 "-511..511");
    {
        // Defaults: safe-by-default seed, bound at 0. Absent keys must give
        // the seed, not 0 — 0 is the uncharacterised efuse default.
        auto r = load_config_json(R"({"node":{"originator":7,"role":"tx"},
          "adapters":[{"name":"wlan0","role":"tx","channel":5805}]})");
        CHECK(bool(r));
        if (r) {
            CHECK_EQ_U(static_cast<int64_t>(r.value->adapters[0].power_offset_qdb) + 24, 0);
            CHECK_EQ_U(r.value->adapters[0].power_offset_max_qdb, 0);
        }
        // Positive headroom is supported when explicitly opted into — efuse
        // tables are per-module and some ship conservative.
        auto p = load_config_json(R"({"node":{"originator":7,"role":"tx"},
          "adapters":[{"name":"wlan0","role":"tx","channel":5805,
            "power_offset_qdb":16,"power_offset_max_qdb":40}]})");
        CHECK(bool(p));
        if (p) CHECK_EQ_U(p.value->adapters[0].power_offset_qdb, 16);
    }
    // fec.scheme only on a frame-shm binding.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
        "bind":{"kind":"udp","listen":"127.0.0.1:1"},
        "fec":{"scheme":"rlc256"}}]})",
                 "frame-shm binding");
    // §2 Pass 74: node.spectator requires role "rx" (a tx node has an uplink).
    expect_error(R"({"node":{"originator":1,"role":"tx","spectator":true}})",
                 "spectator requires role");
    // §2 Pass 74: a passive spectator (rx, spectator, frame-shm delivery, no tx
    // adapter) parses — the backend's allow_rx_only is what the flag gates.
    {
        auto r = load_config_json(R"({
          "node":{"originator":9,"role":"rx","spectator":true,
                  "preferred_originator":17},
          "adapters":[{"name":"rx0","bus":"5-1","role":"rx",
                       "channel":5805,"bw":20}],
          "streams":[{"stream_id":0,"stream_type":"RTP","dir":"out",
                      "bind":{"kind":"frame-shm","name":"venc_frame"}}],
          "air":{"kind":"radio"}})");
        CHECK(bool(r));
        if (r) CHECK(r.value->node.spectator);
    }
    // Enforcing JSCC parity is not a partial mode: it requires the RLC
    // encoder that can apply the per-frame repair override.
    expect_error(R"({"node":{"originator":1,"role":"tx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
        "bind":{"kind":"frame-shm","name":"venc_frame"},
        "fec":{"scheme":"none"},
        "jscc_shadow":{"fec_floor_permille":20,"fec_cap_permille":400,
          "arq_guard_us":500,"feedback_timeout_ms":500,
          "min_rtt_samples":20,"enforce":true}}]})",
                 "requires fec.scheme=rlc256");
    // §11.5/§15.2 (Pass 92): the shipped verify window is DERIVED from the
    // engine seed, never restated here. Pass 89 raised the engine default to
    // 500 ms and this default kept saying 150 — and csa_params() copies this
    // one over the engine's, so no binary ever ran the ruled value.
    {
        auto r = load_config_json(R"({"node":{"originator":1,"role":"rx"}})");
        CHECK(bool(r));
        if (r) {
            CHECK_EQ_U(r.value->policy.csa.verify_timeout_ms,
                       kCsaVerifyTimeoutMsDefault);
            CHECK_EQ_U(r.value->policy.csa.verify_timeout_ms, 500u);
        }
    }
    // §15.2 (Pass 92): the §11.6 RX-liveness guard must outlast the verify
    // window, or a monitor re-init fires in the middle of a pending switch.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "policy":{"csa":{"verify_timeout_ms":800,"rx_liveness_ms":750}}})",
                 "verify_timeout_ms");
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "policy":{"csa":{"verify_timeout_ms":750,"rx_liveness_ms":750}}})",
                 "verify_timeout_ms");
    // rx_liveness_ms = 0 disables the guard, so the ordering rule is moot.
    {
        auto r = load_config_json(R"({"node":{"originator":1,"role":"rx"},
          "policy":{"csa":{"verify_timeout_ms":3000,"rx_liveness_ms":0}}})");
        CHECK(bool(r));
        if (r) CHECK_EQ_U(r.value->policy.csa.verify_timeout_ms, 3000u);
    }
    // §3.0 net_id is one byte.
    expect_error(R"({"node":{"originator":1,"role":"rx","net_id":256}})",
                 "net_id");
    // air backend kinds are udp | radio.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "air":{"kind":"carrier-pigeon"}})",
                 "unknown");
    {
        auto r = load_config_json(R"({
          "node":{"originator":1,"role":"rx"},
          "air":{"kind":"udp-broadcast","tx":["127.255.255.255:5801"],
                 "rx":["0.0.0.0:5801","0.0.0.0:5801"],
                 "pace_mbps":20}})");
        CHECK(bool(r));
        if (r) {
            CHECK(r.value->air.kind == AirCfg::Kind::kUdpBroadcast);
            CHECK_EQ_U(r.value->air.udp.pace_mbps, 20);
            CHECK_EQ_U(r.value->air.udp.rx.size(), 2);
        }
    }
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "air":{"kind":"udp-broadcast","tx":[],"rx":["0.0.0.0:5801"]}})",
                 "exactly one");
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "air":{"kind":"udp","pace_mbps":20}})", "only valid");
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "air":{"kind":"radio",
             "airtime_efficiency_permille":1001}})", "0..1000");
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "air":{"kind":"udp","airtime_efficiency_permille":600}})",
                 "only valid");
    // duplicate stream_id.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[
        {"stream_id":0,"stream_type":"RTP","dir":"in",
         "bind":{"kind":"udp","listen":"127.0.0.1:1"}},
        {"stream_id":0,"stream_type":"TELEMETRY","dir":"in",
         "bind":{"kind":"udp","listen":"127.0.0.1:2"}}]})",
                 "duplicate stream_id");
    // >4 UDP bindings (4 streams + stats).
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[
        {"stream_id":0,"stream_type":"RTP","dir":"in","bind":{"kind":"udp","listen":"127.0.0.1:1"}},
        {"stream_id":1,"stream_type":"RTP","dir":"in","bind":{"kind":"udp","listen":"127.0.0.1:2"}},
        {"stream_id":2,"stream_type":"RTP","dir":"in","bind":{"kind":"udp","listen":"127.0.0.1:3"}},
        {"stream_id":3,"stream_type":"RTP","dir":"in","bind":{"kind":"udp","listen":"127.0.0.1:4"}}],
      "stats":{"hz":1,"bind":{"kind":"udp","send":"127.0.0.1:9"}}})",
                 "too many UDP bindings");
    // bad role.
    expect_error(R"({"node":{"originator":1,"role":"master"}})", "role");
    // bad bw.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "adapters":[{"name":"wlan0","role":"rx","channel":5805,"bw":30}]})",
                 "bw");
    // duplicate adapter name.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "adapters":[
        {"name":"wlan0","role":"rx","channel":5805},
        {"name":"wlan0","role":"rx","channel":5805}]})",
                 "duplicate name");
    // unknown stream_type string.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"VIDEO","dir":"in",
        "bind":{"kind":"udp","listen":"127.0.0.1:1"}}]})",
                 "stream_type");
    // §3.4 AUDIO stream type parses (Pass 77).
    {
      auto r = load_config_json(R"({"node":{"originator":1,"role":"tx"},
        "streams":[{"stream_id":1,"stream_type":"AUDIO","dir":"in",
          "bind":{"kind":"udp","listen":"127.0.0.1:5601"}}]})");
      CHECK(bool(r));
      if (r) CHECK_EQ_U(r.value->streams[0].stream_type, stream_type::kAudio);
    }
    // classifier on a non-RTP stream (§4.1: RTP-profile-only).
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"TELEMETRY","dir":"in",
        "classifier":"h265",
        "bind":{"kind":"udp","listen":"127.0.0.1:1"}}]})",
                 "RTP-profile-only");
    // unknown classifier value.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
        "classifier":"av1",
        "bind":{"kind":"udp","listen":"127.0.0.1:1"}}]})",
                 "classifier");

    // --- §6.6 clamp_resync_ms is config-overridable (§17) -------------------
    {
        auto r = load_config_json(
            R"({"node":{"originator":1,"role":"rx"},
                "policy":{"rx":{"clamp_resync_ms":100}}})");
        CHECK(bool(r));
        if (r) {
            CHECK_EQ_U(r.value->policy.rx.clamp_resync_ms, 100);
        }
    }

    // --- step-8 knobs: selector, venc, loopback RSSI -------------------------
    {
        auto r = load_config_json(R"({"node":{"originator":1,"role":"tx"},
          "policy":{"select":{
            "bitrate_lead_s": 0.3, "mcs_up_grace_s": 0.1,
            "reentry_backoff_s": 4, "reentry_dwell_s": 1.5,
            "flap_freeze_count": 2, "flap_freeze_window_s": 8,
            "flap_freeze_s": 12, "pressure_escape_s": 1.0,
            "failsafe_hold_s": 0.5, "failsafe_step_s": 0.5,
            "min_profile": 1, "max_profile": 5,
            "rung_rssi_floor_dbm": [-90, -87, -84]}},
          "venc": {"host": "127.0.0.1:8085", "enabled": true,
                   "recovery_enabled": true},
          "loopback": {"rssi_dbm": -55,
            "rssi_fade": {"start_ms": 1000, "end_ms": 2000, "dbm": -92}}})");
        CHECK(bool(r));
        if (r) {
            const Config& c = *r.value;
            CHECK(c.policy.select.bitrate_lead_s == 0.3);
            CHECK_EQ_U(c.policy.select.flap_freeze_count, 2);
            CHECK_EQ_U(c.policy.select.min_profile, 1);
            CHECK_EQ_U(c.policy.select.max_profile, 5);
            // Partial floors array overrides the prefix, keeps seed tail.
            CHECK(c.policy.select.rung_rssi_floor_dbm[0] == -90);
            CHECK(c.policy.select.rung_rssi_floor_dbm[2] == -84);
            CHECK(c.policy.select.rung_rssi_floor_dbm[3] == -80);  // seed
            CHECK(c.venc.enabled);
            CHECK(c.venc.recovery_enabled);
            CHECK(c.venc.host == "127.0.0.1:8085");
            CHECK(c.loopback.rssi_dbm == -55);
            CHECK(c.loopback.rssi_fade.has_value());
            CHECK_EQ_U(c.loopback.rssi_fade->end_ms, 2000);
            CHECK(c.loopback.rssi_fade->dbm == -92);
        }
        // Defaults hold when absent.
        auto d = load_config_json(R"({"node":{"originator":1,"role":"rx"}})");
        CHECK(bool(d));
        if (d) {
            CHECK(!d.value->venc.enabled);
            CHECK(!d.value->venc.recovery_enabled);
            CHECK(d.value->policy.select.pressure_escape_s == 2.0);
            CHECK(d.value->policy.select.rung_rssi_floor_dbm[0] == -88);
            CHECK_EQ_U(d.value->policy.select.max_profile, 255);
            CHECK(d.value->loopback.rssi_dbm == -60);
        }
        // Rejections: oversize floors array; out-of-range dBm; bad fade.
        expect_error(R"({"node":{"originator":1,"role":"rx"},
          "policy":{"select":{"rung_rssi_floor_dbm":
            [-1,-2,-3,-4,-5,-6,-7,-8,-9]}}})",
                     "rung_rssi_floor_dbm");
        expect_error(R"({"node":{"originator":1,"role":"rx"},
          "policy":{"select":{"rung_rssi_floor_dbm":[-121]}}})",
                     "dBm range");
        expect_error(R"({"node":{"originator":1,"role":"rx"},
          "loopback":{"rssi_fade":{"start_ms":5,"end_ms":5,"dbm":-90}}})",
                     "end_ms");
    }

    // --- optional §14.2 JSCC TX shadow has no implicit inputs ---------------
    {
        auto r = load_config_json(R"({
          "node":{"originator":17,"role":"tx"},
          "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
            "bind":{"kind":"frame-shm","name":"venc_frame"},
            "jscc_shadow":{"fec_floor_permille":20,"fec_cap_permille":400,
              "arq_guard_us":500,"feedback_timeout_ms":500,
              "min_rtt_samples":20}}]})");
        CHECK(bool(r));
        if (r) {
            CHECK(r.value->streams[0].jscc_shadow.has_value());
            const auto& js = *r.value->streams[0].jscc_shadow;
            CHECK_EQ_U(js.fec_floor_permille, 20);
            CHECK_EQ_U(js.fec_cap_permille, 400);
            CHECK_EQ_U(js.arq_guard_us, 500);
            CHECK_EQ_U(js.feedback_timeout_ms, 500);
            CHECK_EQ_U(js.min_rtt_samples, 20);
        }
        expect_error(R"({"node":{"originator":17,"role":"tx"},
          "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
            "bind":{"kind":"udp","listen":"127.0.0.1:5600"},
            "jscc_shadow":{"fec_floor_permille":20,"fec_cap_permille":400,
              "arq_guard_us":500,"feedback_timeout_ms":500,
              "min_rtt_samples":20}}]})", "frame-shm ingress");
        expect_error(R"({"node":{"originator":17,"role":"tx"},
          "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
            "bind":{"kind":"frame-shm","name":"venc_frame"},
            "jscc_shadow":{"fec_floor_permille":500,"fec_cap_permille":400,
              "arq_guard_us":500,"feedback_timeout_ms":500,
              "min_rtt_samples":20}}]})", "floor <= cap");
    }

    // --- profile table -------------------------------------------------------
    {
        // The repo's example table must load and reproduce the golden hash
        // (cross-validates llround scaling against table_hash_test). Moved
        // Pass 111's calibrated per-rung airtime ceilings are inside the §3.6
        // CRC-8 content hash, so both ends must redeploy together or they do
        // not agree on the table.
        auto t = load_profile_table(std::string(WBLINK_SOURCE_DIR) +
                                    "/profiles/table.example.json");
        CHECK(bool(t));
        if (t) {
            CHECK_EQ_U(t.value->profiles.size(), 8);
            CHECK_EQ_U(t.value->floor_profile, 0);
            CHECK_EQ_U(t.value->profiles[0].airtime_budget_permille, 600);
            CHECK_EQ_U(t.value->profiles[4].airtime_budget_permille, 510);
            CHECK_EQ_U(t.value->profiles[5].airtime_budget_permille, 463);
            CHECK_EQ_U(t.value->profiles[6].airtime_budget_permille, 438);
            CHECK_EQ_U(t.value->profiles[7].airtime_budget_permille, 418);
            CHECK_EQ_U(t.value->profiles[0].max_payload, 1424);
            CHECK_EQ_U(t.value->profiles[3].max_payload, 2048);
            CHECK_EQ_U(t.value->profiles[7].max_payload, 3072);
            CHECK_EQ_U(t.value->profiles[0].fec_overhead_permille, 250);
            CHECK_EQ_U(t.value->profiles[5].fec_overhead_permille, 180);
            static constexpr uint32_t kPass111Bitrates[] = {
                2829, 5754, 10303, 13769, 18025, 21839, 23249, 24658};
            for (size_t i = 0; i < t.value->profiles.size(); ++i) {
                CHECK_EQ_U(derive_bitrate_kbps(t.value->profiles[i]),
                           kPass111Bitrates[i]);
            }
            CHECK_EQ_U(table_version(*t.value), 0xC1);  // Pass 163 (was 0x80)
            CHECK_EQ_U(t.value->probe_period, 64);
            CHECK_EQ_U(t.value->probe_slot, 4);
        }
    }
    {
        // Duplicate ids rejected.
        auto t = load_profile_table_json(R"({"profiles":[
          {"id":0,"mcs":0,"guard_interval":"long","tx_power_level":4,
           "airtime_budget_frac":0.6,"arq_deadline_ms":{"iframe":80,"pframe":25},
           "bitrate_min_kbps":2200,"reserve_bps":{"control":64000,"telemetry":32000}},
          {"id":0,"mcs":1,"guard_interval":"long","tx_power_level":4,
           "airtime_budget_frac":0.6,"arq_deadline_ms":{"iframe":80,"pframe":25},
           "bitrate_min_kbps":2200,"reserve_bps":{"control":64000,"telemetry":32000}}],
          "floor_profile":0})");
        CHECK(!t);
        CHECK(t.error.find("duplicate") != std::string::npos);
    }
    {
        // venc hard floor (§9.6).
        auto t = load_profile_table_json(R"({"profiles":[
          {"id":0,"mcs":0,"guard_interval":"long","tx_power_level":4,
           "airtime_budget_frac":0.6,"arq_deadline_ms":{"iframe":80,"pframe":25},
           "bitrate_min_kbps":512,"reserve_bps":{"control":64000,"telemetry":32000}}],
          "floor_profile":0})");
        CHECK(!t);
        CHECK(t.error.find("1000") != std::string::npos);
    }
    {
        // floor_profile must name an existing id.
        auto t = load_profile_table_json(R"({"profiles":[
          {"id":0,"mcs":0,"guard_interval":"long","tx_power_level":4,
           "airtime_budget_frac":0.6,"arq_deadline_ms":{"iframe":80,"pframe":25},
           "bitrate_min_kbps":2200,"reserve_bps":{"control":64000,"telemetry":32000}}],
          "floor_profile":5})");
        CHECK(!t);
        CHECK(t.error.find("floor_profile") != std::string::npos);
    }
    {
        // Fractions outside [0,1] rejected.
        auto t = load_profile_table_json(R"({"profiles":[
          {"id":0,"mcs":0,"guard_interval":"long","tx_power_level":4,
           "airtime_budget_frac":1.5,"arq_deadline_ms":{"iframe":80,"pframe":25},
           "bitrate_min_kbps":2200,"reserve_bps":{"control":64000,"telemetry":32000}}],
          "floor_profile":0})");
        CHECK(!t);
    }
    {
        // §3.6 Pass 163 probe schedule: parsed, hashed, and slot < period
        // enforced. Absence keeps 0/0 (probing structurally off).
        const char* one = R"({"profiles":[
          {"id":0,"mcs":0,"guard_interval":"long","tx_power_level":4,
           "airtime_budget_frac":0.6,"arq_deadline_ms":{"iframe":80,"pframe":25},
           "bitrate_min_kbps":2200,"reserve_bps":{"control":64000,"telemetry":32000}}],
          "floor_profile":0)";
        auto plain = load_profile_table_json(std::string(one) + "}");
        CHECK(bool(plain));
        auto probed = load_profile_table_json(
            std::string(one) + R"(,"probe":{"period":64,"slot":4}})");
        CHECK(bool(probed));
        if (plain && probed) {
            CHECK_EQ_U(plain.value->probe_period, 0);
            CHECK_EQ_U(probed.value->probe_period, 64);
            CHECK_EQ_U(probed.value->probe_slot, 4);
            // The schedule is hashed content (§3.6): same profiles, different
            // schedule => different table_version.
            CHECK(table_version(*plain.value) != table_version(*probed.value));
        }
        auto bad = load_profile_table_json(
            std::string(one) + R"(,"probe":{"period":16,"slot":16}})");
        CHECK(!bad);
        CHECK(bad.error.find("probe.slot") != std::string::npos);
    }

    // --- §14.2 enforce flag (Pass 38): parse + default off -----------------
    {
        auto r = load_config_json(R"({"node":{"originator":9,"role":"tx"},
          "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
            "bind":{"kind":"frame-shm","name":"venc_frame"},
            "fec":{"scheme":"rlc256"},
            "jscc_shadow":{"fec_floor_permille":20,"fec_cap_permille":400,
              "arq_guard_us":500,"feedback_timeout_ms":500,
              "min_rtt_samples":5,"enforce":true}}]})");
        CHECK(bool(r));
        if (r) {
            CHECK(r.value->streams[0].jscc_shadow.has_value());
            CHECK(r.value->streams[0].jscc_shadow->enforce);
        }
        auto d = load_config_json(R"({"node":{"originator":9,"role":"tx"},
          "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
            "bind":{"kind":"frame-shm","name":"venc_frame"},
            "jscc_shadow":{"fec_floor_permille":20,"fec_cap_permille":400,
              "arq_guard_us":500,"feedback_timeout_ms":500,
              "min_rtt_samples":5}}]})");
        CHECK(bool(d) && !d.value->streams[0].jscc_shadow->enforce);
    }

    // --- §9.6 venc frame-cap knobs (Pass 37): defaults + validation --------
    {
        auto r = load_config_json(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"fps_hint":90,
                  "cap_ceiling_bytes":150000,"settle_ms":500}})");
        CHECK(bool(r));
        if (r) {
            CHECK(r.value->venc.frame_caps);  // default on
            CHECK_EQ_U(r.value->venc.fps_hint, 90);
            CHECK_EQ_U(r.value->venc.i_headroom_permille, 1000);
            CHECK_EQ_U(r.value->venc.p_headroom_permille, 1000);
            CHECK_EQ_U(r.value->venc.cap_ceiling_bytes, 150000);
            CHECK_EQ_U(r.value->venc.settle_ms, 500);
        }
        expect_error(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"fps_hint":0}})", "fps_hint");
        expect_error(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"i_headroom_permille":1200}})", "headrooms");
    }

    // --- §10.2/§10.7 Pass 125: power_map is gated by ADAPTER role -----------
    // Pass 43 keyed this on NODE role, which got both cases wrong. The rule
    // is now: a map is legal exactly where it is actuated.
    {
        // rx node + role:"tx" uplink adapter: LEGAL as of Pass 125 — this is
        // the §10.7 ground-uplink actuator. Pass 43 rejected it.
        auto d = load_config_json(R"({"node":{"originator":9,"role":"rx"},
          "adapters":[{"name":"wlan1","bus":"1-1.2","role":"tx","channel":5805,
                       "power_map":"/etc/waybeam-link/power.wlan1.txt"}]})");
        CHECK(bool(d));
        if (d) {
            CHECK(d.value->adapters.size() == 1);
            CHECK(!d.value->adapters[0].power_map.empty());
        }
    }
    // rx node + role:"rx" diversity adapter: still rejected, never actuated.
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "adapters":[{"name":"wlan0","bus":"1-1.1","role":"tx","channel":5805},
                  {"name":"wlan1","bus":"1-1.2","role":"rx","channel":5805,
                   "power_map":"/etc/waybeam-link/power.wlan1.txt"}]})",
        "never applied");
    // tx node + role:"rx" diversity adapter: NOW rejected too. Pass 43 let
    // this through on a node-role test, silently loading a map that the
    // §10.4 selector commit only ever resolves for the tx adapter.
    expect_error(R"({"node":{"originator":9,"role":"tx"},
      "adapters":[{"name":"wlan0","bus":"1-1.1","role":"tx","channel":5805},
                  {"name":"wlan1","bus":"1-1.2","role":"rx","channel":5805,
                   "power_map":"/etc/waybeam-link/power.wlan1.txt"}]})",
        "never applied");
    // tx node + role:"tx": unchanged, and already covered by the golden
    // config at the top of this file. More than one role:"tx" adapter is
    // rejected at backend open (air_radio.cpp), not here.

    // --- §10.7 Pass 125: air.uplink_rate ------------------------------------
    {
        // Seeds equal the pre-Pass-125 TxRate struct default, so an absent
        // block must produce byte-identical on-air behaviour.
        auto d = load_config_json(R"({"node":{"originator":9,"role":"rx"}})");
        CHECK(bool(d));
        if (d) {
            CHECK(d.value->air.uplink_mcs == 0);
            CHECK(!d.value->air.uplink_sgi);
            CHECK(d.value->air.uplink_bw == 20);
        }
        auto e = load_config_json(R"({"node":{"originator":9,"role":"rx"},
          "air":{"kind":"radio","uplink_rate":{"mcs":3,"sgi":true,"bw":40}}})");
        CHECK(bool(e));
        if (e) {
            CHECK(e.value->air.uplink_mcs == 3);
            CHECK(e.value->air.uplink_sgi);
            CHECK(e.value->air.uplink_bw == 40);
        }
    }
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "air":{"kind":"radio","uplink_rate":{"mcs":8}}})", "mcs must be 0..7");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "air":{"kind":"radio","uplink_rate":{"bw":80}}})", "bw must be 20 or 40");

    // --- §10.7 Pass 125: uplink calibration gates ---------------------------
    {
        auto d = load_config_json(R"({"node":{"originator":9,"role":"rx"}})");
        CHECK(bool(d));
        if (d) {
            const CalibrationPolicy& c = d.value->policy.calibration;
            // §3.16 (Pass 153) dwell seeds: at 500 probes one loss is
            // 2permille — decidable first time, and the verify dwell finally
            // has the n the estimator arithmetic demands.
            CHECK(c.dwell_probe_frames == 500);
            CHECK(c.dwell_verify_frames == 1000);
            CHECK(c.probe_pace_us == 2000);
            CHECK(c.tally_wait_ms == 500);
            CHECK(c.tally_retries == 3);
            CHECK(c.feed_quiet_ms == 2000);
            CHECK(c.settle_ms == 300);
        }
    }
    // Pass 132 (kept on the new primitive): a burst too small to resolve the
    // walls decides on noise — one lost probe must be <= loss_ok_milli, so
    // with the 15permille seed a 40-probe burst (25permille) is refused.
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"calibration":{"dwell_probe_frames":40}}})",
        "too small to resolve loss_ok_milli");
    // Configs still carrying Pass-152-era keys load, with them ignored.
    {
        auto d = load_config_json(R"({"node":{"originator":9,"role":"rx"},
          "policy":{"calibration":{"uplink_ambiguous_epochs":80,
            "uplink_probe_epochs":100, "uplink_drain_ms":600,
            "uplink_floor_min_samples":300, "calib_min_report_hz":6,
            "probe_dwell_ms":1200, "report_loss_abort_ms":3000}}})");
        CHECK(bool(d));
    }
    // §3.16: dwell bursts bounded by the receiver's exact-dedup bitmap.
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"calibration":{"dwell_verify_frames":0}}})",
        "must be 1..1024");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"calibration":{"dwell_verify_frames":2048}}})",
        "must be 1..1024");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"calibration":{"tally_wait_ms":0}}})",
        "must be >= 1");
    {
        auto ok = load_config_json(R"({"node":{"originator":9,"role":"rx"},
          "policy":{"calibration":{"tally_retries":0,
            "dwell_probe_frames":100}}})");
        CHECK(bool(ok) && ok.value->policy.calibration.tally_retries == 0 &&
              ok.value->policy.calibration.dwell_probe_frames == 100);
    }
    // W4/W8: the seek moves in whole steps and judges the cap wall on a >= 2 dB
    // (8 qdb) commanded rise. Zero or negative never terminates and walks an
    // unbounded negative qdb into set_power_qdb; under 8 the cap wall can never
    // fire, silently disabling one of §10.6's three walls.
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"calibration":{"seek_step_qdb":0}}})",
        "seek_step_qdb must be >= 8");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"calibration":{"seek_step_qdb":-16}}})",
        "seek_step_qdb must be >= 8");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"calibration":{"seek_step_qdb":4}}})",
        "seek_step_qdb must be >= 8");
    {
        auto ok = load_config_json(R"({"node":{"originator":9,"role":"rx"},
          "policy":{"calibration":{"seek_step_qdb":8}}})");
        CHECK(bool(ok) && ok.value->policy.calibration.seek_step_qdb == 8);
    }
    // §10.6 (Pass 151): the relative-backend step. Bounded on BOTH sides —
    // under 2 qdb aliases on the 0.5 dB TXAGC families (devourer Jaguar1/2;
    // Jaguar3 resolves 1 qdb), and over 6 dB leaves the default 24 qdb
    // window with two probes, which is the condition the key exists to
    // prevent.
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"calibration":{"offset_seek_step_qdb":1}}})",
        "offset_seek_step_qdb must be 2..24");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "policy":{"calibration":{"offset_seek_step_qdb":32}}})",
        "offset_seek_step_qdb must be 2..24");
    {
        auto d = load_config_json(R"({"node":{"originator":9,"role":"rx"}})");
        CHECK(bool(d) &&
              d.value->policy.calibration.offset_seek_step_qdb == 8);
        auto ok = load_config_json(R"({"node":{"originator":9,"role":"rx"},
          "policy":{"calibration":{"offset_seek_step_qdb":2}}})");
        CHECK(bool(ok) &&
              ok.value->policy.calibration.offset_seek_step_qdb == 2);
    }

    // --- §4.1 Pass 40 ARQ cadence cutoff: seed + parse ----------------------
    {
        auto d = load_config_json(R"({"node":{"originator":9,"role":"tx"}})");
        CHECK(bool(d) && d.value->policy.arq.arq_max_fps == 100);
        auto r = load_config_json(R"({"node":{"originator":9,"role":"tx"},
          "policy":{"arq":{"arq_max_fps":0}}})");
        CHECK(bool(r) && r.value->policy.arq.arq_max_fps == 0);
    }

    // --- §9.11 fps ladder (Pass 39, corrected Pass 53) ----------------------
    {
        auto r = load_config_json(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"fps_ladder":{"enabled":true,
            "min":60,"preferred":100,"max":144,
            "min_p_frame_bytes":12000,"restore_hysteresis_bytes":1500,
            "sample_timeout_ms":750,"reduce_after_ms":1500}}})");
        CHECK(bool(r));
        if (r) {
            CHECK(r.value->venc.fps_ladder.enabled);
            CHECK_EQ_U(r.value->venc.fps_ladder.preferred, 100);
            CHECK_EQ_U(r.value->venc.fps_ladder.min_p_frame_bytes, 12000);
            CHECK_EQ_U(r.value->venc.fps_ladder.restore_hysteresis_bytes,
                       1500);
            CHECK_EQ_U(r.value->venc.fps_ladder.sample_timeout_ms, 750);
            CHECK_EQ_U(r.value->venc.fps_ladder.reduce_after_ms, 1500);
            CHECK_EQ_U(r.value->venc.fps_ladder.restore_after_ms, 8000);
        }
        expect_error(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"fps_ladder":{"enabled":true,
            "min":50,"preferred":90,"max":144}}})", "ladder members");
        expect_error(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"fps_ladder":{"enabled":true,
            "min":90,"preferred":60,"max":144}}})", "ladder members");
        expect_error(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"fps_ladder":{"enabled":true}}})", "requires venc.enabled");
        expect_error(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"fps_ladder":{"enabled":true,
            "min_p_frame_bytes":0}}})", "frame-size floor");
        expect_error(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"fps_ladder":{"enabled":true,
            "sample_timeout_ms":0}}})", "frame-size floor");
    }

    // --- §11.7 v2 command presets (Pass 71) --------------------------------
    {
        auto r = load_config_json(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"command_presets":{
            "fps":[30,60,90],"resolution":["1280x720","1920x1080"],
            "framing":["off","crop"]}}})");
        CHECK(bool(r));
        if (r) {
            CHECK_EQ_U(r.value->venc.preset_fps.size(), 3);
            CHECK_EQ_U(r.value->venc.preset_fps[1], 60);
            CHECK_EQ_U(r.value->venc.preset_resolution.size(), 2);
            CHECK(r.value->venc.preset_resolution[0] == "1280x720");
            CHECK_EQ_U(r.value->venc.preset_framing.size(), 2);
        }
        // Absent object leaves every list empty (command REJECTED, §11.7).
        auto d = load_config_json(
            R"({"node":{"originator":9,"role":"tx"},"venc":{"enabled":true}})");
        CHECK(bool(d) && d.value->venc.preset_fps.empty());
        expect_error(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"command_presets":{
            "fps":[30,45,60,75,90,100]}}})", "at most 5");
        expect_error(R"({"node":{"originator":9,"role":"tx"},
          "venc":{"enabled":true,"command_presets":{"fps":[50]}}})",
          "ladder members");
    }

    // --- §14.3 cache config: parse, defaults, and validation ---------------
    {
        auto r = load_config_json(R"({
          "node": {"originator": 9, "role": "rx"},
          "streams": [
            {"stream_id": 0, "stream_type": "RTP", "dir": "out",
             "bind": {"kind": "frame-shm", "name": "venc_out"}}],
          "cache": {
            "repair": {"enabled": true, "stream_id": 0,
                       "listen": "127.0.0.1:5802",
                       "caches": [{"originator": 33,
                                   "endpoint": "127.0.0.1:5801"}]},
            "store": {"enabled": true, "listen": "127.0.0.1:5801",
                      "controller": {"originator": 9,
                                     "endpoint": "127.0.0.1:5802"},
                      "stream_ids": [0],
                      "status_to": ["127.0.0.1:5802"]}}})");
        CHECK(bool(r));
        if (r) {
            const Config& c = *r.value;
            CHECK(c.cache.repair.enabled);
            CHECK_EQ_U(c.cache.repair.caches.size(), 1);
            CHECK_EQ_U(c.cache.repair.caches[0].originator, 33);
            // §14.3 seeds survive an unconfigured field.
            CHECK_EQ_U(c.cache.repair.local_quiet_ms, 2);
            CHECK_EQ_U(c.cache.repair.hard_close_ms, 8);
            CHECK_EQ_U(c.cache.repair.repair_fraction_permille, 200);
            CHECK_EQ_U(c.cache.repair.max_cache_attempts, 2);
            CHECK_EQ_U(c.cache.repair.health_floor_permille, 800);
            CHECK_EQ_U(c.cache.repair.nack_grace_ms, 3);
            CHECK_EQ_U(c.cache.repair.assignment_interval_ms, 500);
            CHECK(c.cache.store.enabled);
            CHECK_EQ_U(c.cache.store.blocks, 96);
            CHECK_EQ_U(c.cache.store.max_requests_per_s, 400);
            CHECK_EQ_U(c.cache.store.status_interval_ms, 500);
            CHECK(c.cache.store.controller.has_value());
            CHECK_EQ_U(c.cache.store.controller->originator, 9);
        }
        // Both roles default off.
        auto d = load_config_json(R"({"node":{"originator":9,"role":"rx"}})");
        CHECK(bool(d) && !d.value->cache.repair.enabled &&
              !d.value->cache.store.enabled);
    }
    // repair.enabled requires caches + listen, and a frame-shm egress stream.
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "cache":{"repair":{"enabled":true,"listen":"127.0.0.1:5802"}}})",
        "caches");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"out",
                  "bind":{"kind":"udp","send":"127.0.0.1:5700"}}],
      "cache":{"repair":{"enabled":true,"listen":"127.0.0.1:5802",
        "caches":[{"originator":33,"endpoint":"127.0.0.1:5801"}]}}})",
        "frame-shm");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"out",
                  "bind":{"kind":"frame-shm","name":"venc_out"}}],
      "cache":{"repair":{"enabled":true,"stream_id":0,
        "listen":"127.0.0.1:5802","nack_grace_ms":7,
        "caches":[{"originator":33,"endpoint":"127.0.0.1:5801"}]}}})",
        "nack_grace_ms");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"out",
                  "bind":{"kind":"frame-shm","name":"venc_out"}}],
      "cache":{"repair":{"enabled":true,"stream_id":0,
        "listen":"127.0.0.1:5802","assignment_interval_ms":0,
        "caches":[{"originator":33,"endpoint":"127.0.0.1:5801"}]}}})",
        "assignment_interval_ms");
    // store.enabled requires listen + stream_ids.
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "cache":{"store":{"enabled":true,"listen":"127.0.0.1:5801"}}})",
        "stream_ids");
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "cache":{"store":{"enabled":true,"listen":"127.0.0.1:5801",
        "stream_ids":[0],"controller":{"originator":0,
        "endpoint":"127.0.0.1:5802"}}}})", "controller");

    // --- §11.7 0x0A power presets (Pass 135) -------------------------------
    // Content-based substitution: no offset arithmetic against kSample, which
    // silently walks off the end when the sample text moves.
    const auto subst = [](std::string src, const std::string& from,
                          const std::string& to) {
        const size_t at = src.find(from);
        return at == std::string::npos ? src : src.replace(at, from.size(), to);
    };
    {
        // A tier may only ever LOWER power: a preset above the boot ceiling is
        // clamped to it, not honoured. Without this the runtime menu would be
        // a way past the one sanity limit §10.3 exists to provide.
        auto r = load_config_json(
            subst(kSample, "[60, 76, 84]", "[60, 76, 9000]"));
        CHECK(bool(r));
        if (r) {
            CHECK_EQ_U(r.value->adapters[0].power_presets_qdb.size(), 3);
            CHECK(r.value->adapters[0].power_presets_qdb[2] == 108);
        }
    }
    {
        // No ceiling configured: nothing to clamp against, list kept as-is.
        auto r = load_config_json(
            subst(kSample, "\"max_power_qdb\": 108,", ""));
        CHECK(bool(r));
        if (r) {
            CHECK(!r.value->adapters[0].max_power_qdb.has_value());
            CHECK(r.value->adapters[0].power_presets_qdb[2] == 84);
        }
    }
    // An rx adapter never resolves power, so a list there is inert — the same
    // reason power_map is rejected on one.
    expect_error(subst(kSample, "\"1-1.3\", \"role\": \"rx\",",
                       "\"1-1.3\", \"role\": \"rx\", "
                       "\"power_presets_qdb\": [60],"),
                 "power_presets_qdb on a role:\"rx\" adapter");
    // §11.7 cmd_arg indexes at most 5 choices (Pass 68).
    expect_error(subst(kSample, "[60, 76, 84]", "[4,20,36,52,68,84]"),
                 "more than 5 entries");
    expect_error(subst(kSample, "[60, 76, 84]", "[]"),
                 "power_presets_qdb is empty");

    // §10.3/§11.7 0x0A (Pass 166): the offset-space list, term for term with
    // the absolute one above but clamped to power_offset_max_qdb. Each case
    // below is the offset twin of a case above; the FIRST one is the whole
    // point of the key, so it is asserted on both the value and the count.
    {
        const std::string kOff =
            "\"power_offset_max_qdb\": 24, "
            "\"power_offset_presets_qdb\": [-72, -48, -24, 0, 24], ";
        const std::string with = subst(kSample, "\"power_presets_qdb\"",
                                       kOff + "\"power_presets_qdb\"");
        auto r = load_config_json(with);
        CHECK(bool(r));
        if (r) {
            const auto& p = r.value->adapters[0].power_offset_presets_qdb;
            CHECK_EQ_U(p.size(), 5);
            CHECK(p[0] == -72);
            CHECK(p[4] == 24);   // == the bound: allowed, not clamped away
            // The absolute list is still parsed alongside; the two coexist and
            // the SPACE picks which governs, not the loader.
            CHECK_EQ_U(r.value->adapters[0].power_presets_qdb.size(), 3);
        }
        // Above the bound is clamped to it, exactly as the absolute list is
        // clamped to max_power_qdb. This is the "a tier only lowers" rule in
        // the space where it now matters — the only RF backend is relative.
        auto hot = load_config_json(
            subst(with, "[-72, -48, -24, 0, 24]", "[-72, -48, 200]"));
        CHECK(bool(hot));
        if (hot) {
            CHECK(hot.value->adapters[0].power_offset_presets_qdb[2] == 24);
        }
        // Unlike max_power_qdb, the bound is not optional — it defaults to 0,
        // so omitting it clamps to 0 rather than leaving the list untouched.
        auto dflt = load_config_json(
            subst(with, "\"power_offset_max_qdb\": 24, ", ""));
        CHECK(bool(dflt));
        if (dflt) {
            CHECK(dflt.value->adapters[0].power_offset_presets_qdb[4] == 0);
        }
        expect_error(subst(with, "[-72, -48, -24, 0, 24]",
                           "[-96, -84, -72, -48, -24, 0]"),
                     "more than 5 entries");
        expect_error(subst(with, "[-72, -48, -24, 0, 24]", "[]"),
                     "power_offset_presets_qdb is empty");
        expect_error(subst(kSample, "\"1-1.3\", \"role\": \"rx\",",
                           "\"1-1.3\", \"role\": \"rx\", "
                           "\"power_offset_presets_qdb\": [-24],"),
                     "power_offset_presets_qdb on a role:\"rx\" adapter");
    }

    return wbtest_finish("config_test");
}
