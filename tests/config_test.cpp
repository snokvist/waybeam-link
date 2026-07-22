// SPDX-License-Identifier: GPL-2.0-or-later
// §15.2 config loader + §9.3 profile-table loader: the spec sample parses to
// the expected structs, absent keys keep spec-seed defaults, and every §15.1
// load-time rule rejects with a specific error. Also cross-validates the
// file-based table loader against the golden 0x2B hash pinned in
// table_hash_test (proves the frac -> per-mille scaling path).
#include "wblink/config.h"

#include <string>

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
      "max_power_qdb": 2000 },
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
    "select": { "demote_milli": 20, "rssi_floor_dbm": -85,
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
                "channel_allowlist": [5745, 5805, 5825] }
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
                  *c.adapters[0].max_power_qdb == 2000);
            CHECK(!c.adapters[1].max_power_qdb.has_value());

            CHECK_EQ_U(c.streams.size(), 2);
            CHECK_EQ_U(c.streams[0].stream_id, 0);
            CHECK_EQ_U(c.streams[0].stream_type, stream_type::kRtp);
            CHECK(c.streams[0].dir == Dir::kIn);
            CHECK(c.streams[0].bind.listen == "127.0.0.1:5600");
            CHECK(c.streams[0].classifier == RtpClassifier::kH265);
            CHECK_EQ_U(c.streams[1].stream_type, stream_type::kTelemetry);
            CHECK(c.streams[1].classifier == RtpClassifier::kSize);

            CHECK_EQ_U(c.policy.select.demote_milli, 20);
            CHECK(c.policy.select.rssi_floor_dbm == -85);
            CHECK_EQ_U(c.policy.arq.fwd_clamp_blocks, 4);
            CHECK(c.policy.fec.scheme == FecScheme::kNone);
            CHECK_EQ_U(c.policy.ret.guard_us, 300);
            CHECK_EQ_U(c.policy.ret.return_window_us, 2000);
            CHECK(c.policy.csa.psk == "hunter2");
            CHECK_EQ_U(c.policy.csa.home_chan, 5745);
            CHECK_EQ_U(c.policy.csa.bind_release_s, 90);
            CHECK(c.policy.csa.persist_channel);
            CHECK_EQ_U(c.policy.csa.channel_allowlist.size(), 3);

            CHECK(c.stats.bind.has_value());
            CHECK(c.stats.bind->send == "127.0.0.1:9110");

            // The secret must never leak through the summary.
            const std::string summary = dump_config_summary(c);
            CHECK(summary.find("hunter2") == std::string::npos);
            CHECK(summary.find("redacted") != std::string::npos);
        }
    }

    // --- defaults: a minimal config keeps every spec seed ------------------
    {
        auto r = load_config_json(R"({"node":{"originator":9,"role":"rx"}})");
        CHECK(bool(r));
        if (r) {
            const Config& c = *r.value;
            CHECK(c.node.role == Role::kRx);
            CHECK_EQ_U(c.node.preferred_originator, 0);
            CHECK_EQ_U(c.policy.report_timeout_ms, 500);
            CHECK_EQ_U(c.policy.select.demote_milli, 20);
            CHECK(c.policy.select.ewma_alpha == 0.3);
            CHECK_EQ_U(c.policy.arq.holddown_ms, 20);
            CHECK_EQ_U(c.policy.rx.renack_backoff_ms, 6);
            CHECK_EQ_U(c.policy.rx.clamp_resync_ms, 500);  // §6.6 seed
            CHECK_EQ_U(c.policy.ret.guard_us, 300);
            CHECK(!c.policy.ret.quiet_gap);   // §7.1 baseline ships default
            CHECK(!c.node.net_id.has_value());  // §3.0: accept-any when absent
            CHECK(c.policy.csa.psk.empty());  // spectator: no psk
            CHECK(c.stats.hz == 1.0);
            CHECK(!c.stats.bind.has_value());
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
                                "unicast": true}}})");
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
        }
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

    // --- air "kernel-monitor" backend + adapter ifname ---------------------
    {
        auto r = load_config_json(R"({
          "node": {"originator": 7, "role": "rx", "net_id": 2},
          "air": {"kind": "kernel-monitor", "rx_drop_permille": 30,
                  "airtime_efficiency_permille": 600},
          "adapters": [
            {"name": "uplink", "ifname": "wlan0", "role": "tx", "channel": 5805},
            {"name": "div0", "ifname": "wlx01", "role": "rx", "channel": 5805}
          ]})");
        CHECK(bool(r));
        if (r) {
            const Config& c = *r.value;
            CHECK(c.air.kind == AirCfg::Kind::kMonitor);
            CHECK_EQ_U(c.air.rx_drop_permille, 30);
            CHECK_EQ_U(c.air.airtime_efficiency_permille, 600);
            CHECK_EQ_U(c.adapters.size(), 2);
            CHECK(c.adapters[0].ifname == "wlan0");
            CHECK(c.adapters[0].role == Role::kTx);
            CHECK(c.adapters[1].ifname == "wlx01");
            CHECK(c.adapters[1].role == Role::kRx);
        }
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
    // fec.scheme only on a frame-shm binding.
    expect_error(R"({"node":{"originator":1,"role":"rx"},
      "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
        "bind":{"kind":"udp","listen":"127.0.0.1:1"},
        "fec":{"scheme":"rlc256"}}]})",
                 "frame-shm binding");
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
      "air":{"kind":"kernel-monitor",
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
        // 0x2B (cross-validates llround scaling against table_hash_test).
        auto t = load_profile_table(std::string(WBLINK_SOURCE_DIR) +
                                    "/profiles/table.example.json");
        CHECK(bool(t));
        if (t) {
            CHECK_EQ_U(t.value->profiles.size(), 8);
            CHECK_EQ_U(t.value->floor_profile, 0);
            CHECK_EQ_U(t.value->profiles[0].airtime_budget_permille, 600);
            CHECK_EQ_U(table_version(*t.value), 0x41);  // §9.3 max_payload field
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

    // --- §10.2 Pass 43: power_map on an rx node is rejected -----------------
    expect_error(R"({"node":{"originator":9,"role":"rx"},
      "adapters":[{"name":"wlan1","bus":"1-1.2","role":"tx","channel":5805,
                   "power_map":"/etc/waybeam-link/power.wlan1.txt"}]})",
        "never applied");

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

    return wbtest_finish("config_test");
}
