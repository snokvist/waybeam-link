// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/config.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace wblink {

namespace {

using json = nlohmann::json;

std::string read_file(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open '" + path + "'";
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

Result<Role> parse_role(const std::string& s, const char* where) {
    if (s == "tx") return Result<Role>::ok(Role::kTx);
    if (s == "rx") return Result<Role>::ok(Role::kRx);
    return Result<Role>::fail(std::string(where) + ": role must be \"tx\" or \"rx\", got \"" + s + "\"");
}

// §3.4 registry names (numeric values also accepted for user types).
Result<uint8_t> parse_stream_type(const json& j, const char* where) {
    if (j.is_number_unsigned()) {
        const uint64_t v = j.get<uint64_t>();
        if (v > 0xFF) {
            return Result<uint8_t>::fail(std::string(where) + ": stream_type out of u8 range");
        }
        return Result<uint8_t>::ok(static_cast<uint8_t>(v));
    }
    const std::string s = j.get<std::string>();
    if (s == "UNKNOWN") return Result<uint8_t>::ok(stream_type::kUnknown);
    if (s == "RTP") return Result<uint8_t>::ok(stream_type::kRtp);
    if (s == "TELEMETRY") return Result<uint8_t>::ok(stream_type::kTelemetry);
    if (s == "CONTROL") return Result<uint8_t>::ok(stream_type::kControl);
    return Result<uint8_t>::fail(std::string(where) +
                                 ": unknown stream_type \"" + s +
                                 "\" (use RTP/TELEMETRY/CONTROL/UNKNOWN or a number)");
}

Result<FecScheme> parse_fec_scheme(const std::string& s, const char* where) {
    if (s == "none") return Result<FecScheme>::ok(FecScheme::kNone);
    if (s == "rlc256") return Result<FecScheme>::ok(FecScheme::kRlc256);
    if (s == "rlc256_iframe") return Result<FecScheme>::ok(FecScheme::kRlc256Iframe);
    if (s == "tetrys_reactive") return Result<FecScheme>::ok(FecScheme::kTetrysReactive);
    return Result<FecScheme>::fail(std::string(where) + ": unknown fec scheme \"" + s + "\"");
}

// frac in [0,1] -> integer per-mille per §3.6 (llround(frac*1000)).
Result<uint16_t> frac_to_permille(double frac, const char* where) {
    if (!(frac >= 0.0 && frac <= 1.0)) {
        return Result<uint16_t>::fail(std::string(where) + ": fraction must be in [0,1]");
    }
    return Result<uint16_t>::ok(static_cast<uint16_t>(std::llround(frac * 1000.0)));
}

Result<BindCfg> parse_bind(const json& j, Dir dir, const char* where) {
    BindCfg b;
    const std::string kind = j.at("kind").get<std::string>();
    if (kind == "frame-shm") {
        // §15.4 SHM ring: uses "name" (not listen/send); dir selects
        // consumer (in => attach) vs producer (out => create).
        b.kind = BindKind::kFrameShm;
        if (!j.contains("name") || j.at("name").get<std::string>().empty()) {
            return Result<BindCfg>::fail(std::string(where) +
                                         ": frame-shm binding needs a non-empty \"name\" (§15.4)");
        }
        b.name = j.at("name").get<std::string>();
        return Result<BindCfg>::ok(std::move(b));
    }
    if (kind != "udp") {
        return Result<BindCfg>::fail(std::string(where) + ": bind kind \"" + kind +
                                     "\" — unix is a v1 feature; use \"udp\" or \"frame-shm\" (§15.1)");
    }
    b.kind = BindKind::kUdp;
    const bool has_listen = j.contains("listen");
    const bool has_send = j.contains("send");
    if (has_listen == has_send) {
        return Result<BindCfg>::fail(std::string(where) +
                                     ": a binding is \"listen\" XOR \"send\", exactly one (§15.1)");
    }
    if (has_listen) b.listen = j.at("listen").get<std::string>();
    if (has_send) b.send = j.at("send").get<std::string>();
    if (dir == Dir::kIn && !has_listen) {
        return Result<BindCfg>::fail(std::string(where) + ": \"dir\":\"in\" needs a \"listen\" binding");
    }
    if (dir == Dir::kOut && !has_send) {
        return Result<BindCfg>::fail(std::string(where) + ": \"dir\":\"out\" needs a \"send\" binding");
    }
    return Result<BindCfg>::ok(std::move(b));
}

}  // namespace

Result<Config> load_config_json(const std::string& json_text) {
    json j;
    try {
        j = json::parse(json_text);
    } catch (const json::exception& e) {
        return Result<Config>::fail(std::string("JSON parse error: ") + e.what());
    }

    Config cfg;
    try {
        // node
        const json& n = j.at("node");
        cfg.node.originator = n.at("originator").get<uint16_t>();
        auto role = parse_role(n.at("role").get<std::string>(), "node");
        if (!role) return Result<Config>::fail(role.error);
        cfg.node.role = *role.value;
        if (n.contains("preferred_originator")) {
            cfg.node.preferred_originator = n.at("preferred_originator").get<uint16_t>();
        }
        if (n.contains("net_id")) {
            const uint64_t nid = n.at("net_id").get<uint64_t>();
            if (nid > 255) {
                return Result<Config>::fail("node: net_id must be 0..255");
            }
            cfg.node.net_id = static_cast<uint8_t>(nid);
        }

        if (j.contains("profile_table")) {
            cfg.profile_table_path = j.at("profile_table").get<std::string>();
        }

        // adapters
        std::set<std::string> adapter_names;
        for (const json& a : j.value("adapters", json::array())) {
            AdapterCfg ac;
            ac.name = a.at("name").get<std::string>();
            if (ac.name.empty()) {
                return Result<Config>::fail("adapters: empty name");
            }
            if (!adapter_names.insert(ac.name).second) {
                return Result<Config>::fail("adapters: duplicate name \"" + ac.name + "\"");
            }
            ac.bus = a.value("bus", std::string{});
            ac.ifname = a.value("ifname", std::string{});
            auto arole = parse_role(a.at("role").get<std::string>(),
                                    ("adapter " + ac.name).c_str());
            if (!arole) return Result<Config>::fail(arole.error);
            ac.role = *arole.value;
            ac.channel_mhz = a.at("channel").get<uint16_t>();
            const uint32_t bw = a.value("bw", 20u);
            if (bw != 20 && bw != 40 && bw != 80) {
                return Result<Config>::fail("adapter " + ac.name + ": bw must be 20, 40 or 80");
            }
            ac.bw = static_cast<uint8_t>(bw);
            ac.power_map = a.value("power_map", std::string{});
            if (a.contains("max_power_qdb")) {
                ac.max_power_qdb = a.at("max_power_qdb").get<int32_t>();
            }
            cfg.adapters.push_back(std::move(ac));
        }

        // streams (§15.1: one binding each, in XOR out, unique stream_id)
        std::set<unsigned> stream_ids;
        size_t udp_bindings = 0;
        size_t shm_bindings = 0;
        for (const json& s : j.value("streams", json::array())) {
            StreamCfg sc;
            const uint64_t sid = s.at("stream_id").get<uint64_t>();
            if (sid > 0xFF) {
                return Result<Config>::fail("streams: stream_id out of u8 range");
            }
            sc.stream_id = static_cast<uint8_t>(sid);
            if (!stream_ids.insert(sc.stream_id).second) {
                return Result<Config>::fail("streams: duplicate stream_id " + std::to_string(sid));
            }
            auto st = parse_stream_type(s.at("stream_type"),
                                        ("stream " + std::to_string(sid)).c_str());
            if (!st) return Result<Config>::fail(st.error);
            sc.stream_type = *st.value;
            const std::string dir = s.at("dir").get<std::string>();
            if (dir == "in") {
                sc.dir = Dir::kIn;
            } else if (dir == "out") {
                sc.dir = Dir::kOut;
            } else {
                return Result<Config>::fail("stream " + std::to_string(sid) +
                                            ": dir must be \"in\" or \"out\"");
            }
            auto bind = parse_bind(s.at("bind"), sc.dir,
                                   ("stream " + std::to_string(sid)).c_str());
            if (!bind) return Result<Config>::fail(bind.error);
            sc.bind = std::move(*bind.value);
            if (s.contains("originator")) {
                sc.originator = s.at("originator").get<uint16_t>();
            }
            if (s.contains("classifier")) {
                if (sc.stream_type != stream_type::kRtp) {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": classifier is RTP-profile-only (§4.1)");
                }
                const std::string c = s.at("classifier").get<std::string>();
                if (c == "size") {
                    sc.classifier = RtpClassifier::kSize;
                } else if (c == "h264") {
                    sc.classifier = RtpClassifier::kH264;
                } else if (c == "h265") {
                    sc.classifier = RtpClassifier::kH265;
                } else {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": classifier must be \"size\", \"h264\" or \"h265\"");
                }
            }
            // §14.1 per-stream FEC (frame-shm only).
            if (s.contains("fec")) {
                const json& f = s.at("fec");
                auto scheme = parse_fec_scheme(f.value("scheme", std::string("none")),
                                               ("stream " + std::to_string(sid) + ".fec").c_str());
                if (!scheme) return Result<Config>::fail(scheme.error);
                sc.fec.scheme = *scheme.value;
                sc.fec.i_rate_permille = f.value("i_rate_permille", uint16_t{250});
                sc.fec.p_rate_permille = f.value("p_rate_permille", uint16_t{100});
                sc.fec.min_k = f.value("min_k", uint16_t{3});
                if (sc.fec.scheme != FecScheme::kNone && sc.bind.kind != BindKind::kFrameShm) {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": fec.scheme is only valid on a frame-shm binding (§14.1)");
                }
            }
            if (s.contains("jscc_shadow")) {
                if (sc.bind.kind != BindKind::kFrameShm || sc.dir != Dir::kIn) {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": jscc_shadow is only valid on frame-shm ingress");
                }
                const json& js = s.at("jscc_shadow");
                JsccShadowCfg jc;
                jc.fec_floor_permille = js.at("fec_floor_permille").get<uint16_t>();
                jc.fec_cap_permille = js.at("fec_cap_permille").get<uint16_t>();
                jc.arq_guard_us = js.at("arq_guard_us").get<uint32_t>();
                jc.feedback_timeout_ms =
                    js.at("feedback_timeout_ms").get<uint32_t>();
                jc.min_rtt_samples = js.at("min_rtt_samples").get<uint16_t>();
                if (jc.fec_floor_permille > jc.fec_cap_permille ||
                    jc.fec_cap_permille > 4000) {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": jscc_shadow FEC rates require floor <= cap <= 4000");
                }
                if (jc.feedback_timeout_ms == 0 || jc.min_rtt_samples == 0) {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": jscc_shadow timeout and min_rtt_samples must be positive");
                }
                sc.jscc_shadow = jc;
            }
            if (sc.bind.kind == BindKind::kFrameShm) {
                ++shm_bindings;
            } else {
                ++udp_bindings;
            }
            cfg.streams.push_back(std::move(sc));
        }
        if (shm_bindings > 1) {
            return Result<Config>::fail(
                "too many frame-shm bindings (" + std::to_string(shm_bindings) +
                "); the §15.1 shm pool is <=1 per node (in XOR out)");
        }

        // policy — every absent key keeps its spec-seed default.
        if (j.contains("policy")) {
            const json& p = j.at("policy");
            cfg.policy.report_hz = p.value("report_hz", cfg.policy.report_hz);
            cfg.policy.report_timeout_ms =
                p.value("report_timeout_ms", cfg.policy.report_timeout_ms);
            if (p.contains("select")) {
                const json& ps = p.at("select");
                SelectPolicy& sel = cfg.policy.select;
                sel.demote_milli = ps.value("demote_milli", sel.demote_milli);
                sel.rssi_floor_dbm = ps.value("rssi_floor_dbm", sel.rssi_floor_dbm);
                sel.rssi_fade_db_per_s =
                    ps.value("rssi_fade_db_per_s", sel.rssi_fade_db_per_s);
                sel.rssi_fade_arm_dbm =
                    ps.value("rssi_fade_arm_dbm", sel.rssi_fade_arm_dbm);
                sel.promote_rssi_hyst_db =
                    ps.value("promote_rssi_hyst_db", sel.promote_rssi_hyst_db);
                sel.promote_dwell_s = ps.value("promote_dwell_s", sel.promote_dwell_s);
                sel.mcs_settle_s = ps.value("mcs_settle_s", sel.mcs_settle_s);
                sel.down_cooldown_s = ps.value("down_cooldown_s", sel.down_cooldown_s);
                sel.ewma_alpha = ps.value("ewma_alpha", sel.ewma_alpha);
                sel.bitrate_lead_s =
                    ps.value("bitrate_lead_s", sel.bitrate_lead_s);
                sel.mcs_up_grace_s =
                    ps.value("mcs_up_grace_s", sel.mcs_up_grace_s);
                sel.reentry_backoff_s =
                    ps.value("reentry_backoff_s", sel.reentry_backoff_s);
                sel.reentry_dwell_s =
                    ps.value("reentry_dwell_s", sel.reentry_dwell_s);
                sel.flap_freeze_count =
                    ps.value("flap_freeze_count", sel.flap_freeze_count);
                sel.flap_freeze_window_s =
                    ps.value("flap_freeze_window_s", sel.flap_freeze_window_s);
                sel.flap_freeze_s = ps.value("flap_freeze_s", sel.flap_freeze_s);
                sel.pressure_escape_s =
                    ps.value("pressure_escape_s", sel.pressure_escape_s);
                sel.failsafe_hold_s =
                    ps.value("failsafe_hold_s", sel.failsafe_hold_s);
                sel.failsafe_step_s =
                    ps.value("failsafe_step_s", sel.failsafe_step_s);
                sel.min_profile = ps.value("min_profile", sel.min_profile);
                sel.max_profile = ps.value("max_profile", sel.max_profile);
                if (ps.contains("rung_rssi_floor_dbm")) {
                    const json& floors = ps.at("rung_rssi_floor_dbm");
                    if (!floors.is_array() ||
                        floors.size() > sel.rung_rssi_floor_dbm.size()) {
                        return Result<Config>::fail(
                            "select.rung_rssi_floor_dbm: array of up to 8 "
                            "dBm values (§9.4)");
                    }
                    for (size_t i = 0; i < floors.size(); ++i) {
                        const int64_t v = floors[i].get<int64_t>();
                        if (v < -120 || v > 0) {
                            return Result<Config>::fail(
                                "select.rung_rssi_floor_dbm[" +
                                std::to_string(i) +
                                "]: out of dBm range [-120, 0]");
                        }
                        sel.rung_rssi_floor_dbm[i] = static_cast<int8_t>(v);
                    }
                }
            }
            if (p.contains("arq")) {
                const json& pa = p.at("arq");
                ArqPolicy& arq = cfg.policy.arq;
                arq.airtime_frac = pa.value("airtime_frac", arq.airtime_frac);
                arq.attempt_cap = pa.value("attempt_cap", arq.attempt_cap);
                arq.holddown_ms = pa.value("holddown_ms", arq.holddown_ms);
                arq.fwd_clamp_blocks =
                    pa.value("fwd_clamp_blocks", arq.fwd_clamp_blocks);
                arq.ring_window_ms =
                    pa.value("ring_window_ms", arq.ring_window_ms);
                arq.ring_byte_budget =
                    pa.value("ring_byte_budget", arq.ring_byte_budget);
                arq.classifier_size_threshold =
                    pa.value("classifier_size_threshold",
                             arq.classifier_size_threshold);
                arq.release_timeout_ms =
                    pa.value("release_timeout_ms", arq.release_timeout_ms);
                arq.min_recoverable_ms =
                    pa.value("min_recoverable_ms", arq.min_recoverable_ms);
                arq.budget_interval_ms =
                    pa.value("budget_interval_ms", arq.budget_interval_ms);
                arq.budget_floor_bytes =
                    pa.value("budget_floor_bytes", arq.budget_floor_bytes);
                arq.max_block_pkts =
                    pa.value("max_block_pkts", arq.max_block_pkts);
            }
            if (p.contains("rx")) {
                const json& pr = p.at("rx");
                RxCfgPolicy& rx = cfg.policy.rx;
                rx.stall_timeout_ms =
                    pr.value("stall_timeout_ms", rx.stall_timeout_ms);
                rx.dwell_ceiling_ms =
                    pr.value("dwell_ceiling_ms", rx.dwell_ceiling_ms);
                rx.admit_n = pr.value("admit_n", rx.admit_n);
                rx.admit_window_ms =
                    pr.value("admit_window_ms", rx.admit_window_ms);
                rx.renack_attempts =
                    pr.value("renack_attempts", rx.renack_attempts);
                rx.renack_backoff_ms =
                    pr.value("renack_backoff_ms", rx.renack_backoff_ms);
                rx.idle_teardown_ms =
                    pr.value("idle_teardown_ms", rx.idle_teardown_ms);
                rx.fwd_clamp_pkts =
                    pr.value("fwd_clamp_pkts", rx.fwd_clamp_pkts);
                rx.clamp_resync_ms =
                    pr.value("clamp_resync_ms", rx.clamp_resync_ms);
            }
            if (p.contains("fec")) {
                const json& pf = p.at("fec");
                auto scheme = parse_fec_scheme(pf.value("scheme", std::string("none")),
                                               "policy.fec");
                if (!scheme) return Result<Config>::fail(scheme.error);
                cfg.policy.fec.scheme = *scheme.value;
                cfg.policy.fec.overhead_frac =
                    pf.value("overhead_frac", cfg.policy.fec.overhead_frac);
            }
            if (p.contains("return")) {
                const json& pr = p.at("return");
                cfg.policy.ret.quiet_gap =
                    pr.value("quiet_gap", cfg.policy.ret.quiet_gap);
                cfg.policy.ret.guard_us = pr.value("guard_us", cfg.policy.ret.guard_us);
                cfg.policy.ret.return_window_us =
                    pr.value("return_window_us", cfg.policy.ret.return_window_us);
                cfg.policy.ret.unicast =
                    pr.value("unicast", cfg.policy.ret.unicast);
            }
            if (p.contains("csa")) {
                const json& pc = p.at("csa");
                CsaPolicy& csa = cfg.policy.csa;
                csa.psk = pc.value("psk", csa.psk);
                csa.settle_s = pc.value("settle_s", csa.settle_s);
                csa.verify_timeout_ms =
                    pc.value("verify_timeout_ms", csa.verify_timeout_ms);
                csa.min_interval_s = pc.value("min_interval_s", csa.min_interval_s);
                csa.ack_timeout_ms = pc.value("ack_timeout_ms", csa.ack_timeout_ms);
                csa.rendezvous_timeout_s =
                    pc.value("rendezvous_timeout_s", csa.rendezvous_timeout_s);
                csa.home_chan = pc.value("home_chan", csa.home_chan);
                if (pc.contains("channel_allowlist")) {
                    for (const json& c : pc.at("channel_allowlist")) {
                        csa.channel_allowlist.push_back(c.get<uint16_t>());
                    }
                }
            }
        }

        // stats
        if (j.contains("stats")) {
            const json& st = j.at("stats");
            cfg.stats.hz = st.value("hz", cfg.stats.hz);
            if (st.contains("bind")) {
                auto bind = parse_bind(st.at("bind"), Dir::kOut, "stats");
                if (!bind) return Result<Config>::fail(bind.error);
                cfg.stats.bind = std::move(*bind.value);
                ++udp_bindings;
            }
        }

        // control (§15.5 REST control plane; off unless a bind is given)
        if (j.contains("control")) {
            const json& c = j.at("control");
            cfg.control.bind = c.value("bind", std::string());
        }

        // venc (§9.6 encoder actuation; disabled default for dev/bench)
        if (j.contains("venc")) {
            const json& v = j.at("venc");
            cfg.venc.host = v.value("host", cfg.venc.host);
            cfg.venc.enabled = v.value("enabled", cfg.venc.enabled);
            cfg.venc.recovery_enabled =
                v.value("recovery_enabled", cfg.venc.recovery_enabled);
        }

        // air ("udp" = dev backend, not §15; "radio" = devourer, §3.0 —
        // radio adapters come from the top-level adapters array)
        if (j.contains("air")) {
            const json& a = j.at("air");
            const std::string kind = a.value("kind", std::string("udp"));
            cfg.air.rx_drop_permille = static_cast<uint16_t>(
                std::min(1000, std::max(0, a.value("rx_drop_permille", 0))));
            cfg.air.wedge_window_ms =
                a.value("wedge_window_ms", cfg.air.wedge_window_ms);
            cfg.air.wedge_min_submits =
                a.value("wedge_min_submits", cfg.air.wedge_min_submits);
            cfg.air.ack_responder =
                a.value("ack_responder", cfg.air.ack_responder);
            if (kind == "radio") {
                cfg.air.kind = AirCfg::Kind::kRadio;
            } else if (kind == "kernel-monitor") {
                cfg.air.kind = AirCfg::Kind::kMonitor;
            } else if (kind == "udp" || kind == "udp-broadcast") {
                cfg.air.kind = kind == "udp" ? AirCfg::Kind::kUdp
                                               : AirCfg::Kind::kUdpBroadcast;
                for (const json& t : a.value("tx", json::array())) {
                    cfg.air.udp.tx.push_back(t.get<std::string>());
                }
                for (const json& r : a.value("rx", json::array())) {
                    cfg.air.udp.rx.push_back(r.get<std::string>());
                }
                cfg.air.udp.pace_mbps = a.value("pace_mbps", 0u);
                if (cfg.air.kind == AirCfg::Kind::kUdpBroadcast &&
                    (cfg.air.udp.tx.size() != 1 || cfg.air.udp.rx.empty())) {
                    return Result<Config>::fail(
                        "air: udp-broadcast requires exactly one tx and at least one rx endpoint");
                }
                if (cfg.air.kind == AirCfg::Kind::kUdp &&
                    cfg.air.udp.pace_mbps != 0) {
                    return Result<Config>::fail(
                        "air: pace_mbps is only valid for udp-broadcast");
                }
            } else {
                return Result<Config>::fail(
                    "air: kind \"" + kind +
                    "\" unknown (udp | udp-broadcast | radio | kernel-monitor)");
            }
        }

        // loopback (§16.2 synthetic loss)
        if (j.contains("loopback")) {
            const json& lb = j.at("loopback");
            LoopbackCfg& l = cfg.loopback;
            const uint64_t n_adapters = lb.value("adapters", 2u);
            if (n_adapters < 1 || n_adapters > 8) {
                return Result<Config>::fail("loopback: adapters must be 1..8");
            }
            l.adapters = static_cast<uint8_t>(n_adapters);
            l.seed = lb.value("seed", l.seed);
            l.correlation = lb.value("correlation", l.correlation);
            l.return_loss_p = lb.value("return_loss_p", l.return_loss_p);
            l.rssi_dbm = lb.value("rssi_dbm", l.rssi_dbm);
            if (lb.contains("rssi_fade")) {
                const json& f = lb.at("rssi_fade");
                LoopbackCfg::RssiFade fade;
                fade.start_ms = f.at("start_ms").get<uint64_t>();
                fade.end_ms = f.at("end_ms").get<uint64_t>();
                fade.dbm = f.at("dbm").get<int8_t>();
                if (fade.end_ms <= fade.start_ms) {
                    return Result<Config>::fail(
                        "loopback.rssi_fade: end_ms must be > start_ms");
                }
                l.rssi_fade = fade;
            }
            if (lb.contains("loss")) {
                const json& loss = lb.at("loss");
                l.uniform_p = loss.value("uniform_p", 0.0);
                if (loss.contains("ge")) {
                    const json& ge = loss.at("ge");
                    l.ge = std::array<double, 4>{
                        ge.at("p_gb").get<double>(),
                        ge.at("p_bg").get<double>(),
                        ge.at("loss_g").get<double>(),
                        ge.at("loss_b").get<double>()};
                }
            }
        }

        // §15.1: <=4 UDP bindings node-wide (conservatively counting the
        // stats binding against the pool).
        if (udp_bindings > 4) {
            return Result<Config>::fail(
                "too many UDP bindings (" + std::to_string(udp_bindings) +
                "); the §15.1 pool is <=4 per node including the stats binding");
        }
    } catch (const json::exception& e) {
        return Result<Config>::fail(std::string("config: ") + e.what());
    }

    return Result<Config>::ok(std::move(cfg));
}

Result<Config> load_config(const std::string& path) {
    std::string err;
    const std::string text = read_file(path, err);
    if (!err.empty()) {
        return Result<Config>::fail(err);
    }
    return load_config_json(text);
}

Result<ProfileTable> load_profile_table_json(const std::string& json_text) {
    json j;
    try {
        j = json::parse(json_text);
    } catch (const json::exception& e) {
        return Result<ProfileTable>::fail(std::string("JSON parse error: ") + e.what());
    }

    ProfileTable table;
    try {
        const json& profiles = j.at("profiles");
        if (!profiles.is_array() || profiles.empty()) {
            return Result<ProfileTable>::fail("profile table: \"profiles\" must be a non-empty array");
        }
        if (profiles.size() > 255) {
            return Result<ProfileTable>::fail("profile table: more than 255 profiles");
        }
        for (const json& pj : profiles) {
            Profile p;
            const uint64_t id = pj.at("id").get<uint64_t>();
            if (id > 0xFF) {
                return Result<ProfileTable>::fail("profile table: id out of u8 range");
            }
            p.id = static_cast<uint8_t>(id);
            const std::string where = "profile " + std::to_string(id);
            p.mcs = pj.at("mcs").get<uint8_t>();
            const std::string gi = pj.at("guard_interval").get<std::string>();
            if (gi == "long") {
                p.gi = GuardInterval::kLong;
            } else if (gi == "short") {
                p.gi = GuardInterval::kShort;
            } else {
                return Result<ProfileTable>::fail(where + ": guard_interval must be \"long\" or \"short\"");
            }
            p.tx_power_level = pj.at("tx_power_level").get<uint8_t>();
            auto airtime = frac_to_permille(pj.at("airtime_budget_frac").get<double>(),
                                            (where + ".airtime_budget_frac").c_str());
            if (!airtime) return Result<ProfileTable>::fail(airtime.error);
            p.airtime_budget_permille = *airtime.value;
            // §3.2/§9.3 air MTU budget; default standard-rung 1424, ceiling 4096.
            const uint32_t mp = pj.value("max_payload", uint32_t{kDefaultMaxPayload});
            if (mp < kDataHeaderSize + 32 || mp > kMaxDataPayload) {
                return Result<ProfileTable>::fail(
                    where + ": max_payload must be in [" +
                    std::to_string(kDataHeaderSize + 32) + ", " +
                    std::to_string(kMaxDataPayload) + "]");
            }
            p.max_payload = static_cast<uint16_t>(mp);
            auto scheme = parse_fec_scheme(pj.value("fec_scheme", std::string("none")),
                                           (where + ".fec_scheme").c_str());
            if (!scheme) return Result<ProfileTable>::fail(scheme.error);
            p.fec_scheme = *scheme.value;
            auto overhead = frac_to_permille(pj.value("fec_overhead_frac", 0.0),
                                             (where + ".fec_overhead_frac").c_str());
            if (!overhead) return Result<ProfileTable>::fail(overhead.error);
            p.fec_overhead_permille = *overhead.value;
            const json& dl = pj.at("arq_deadline_ms");
            p.arq_deadline_iframe_ms = dl.at("iframe").get<uint16_t>();
            p.arq_deadline_pframe_ms = dl.at("pframe").get<uint16_t>();
            p.bitrate_min_kbps = pj.at("bitrate_min_kbps").get<uint32_t>();
            if (p.bitrate_min_kbps < 1000) {
                return Result<ProfileTable>::fail(
                    where + ": bitrate_min_kbps < 1000 (venc hard floor, §9.6)");
            }
            const json& rb = pj.at("reserve_bps");
            p.reserve_control_bps = rb.at("control").get<uint32_t>();
            p.reserve_telemetry_bps = rb.at("telemetry").get<uint32_t>();
            table.profiles.push_back(p);
        }
        const uint64_t floor = j.at("floor_profile").get<uint64_t>();
        if (floor > 0xFF) {
            return Result<ProfileTable>::fail("profile table: floor_profile out of u8 range");
        }
        table.floor_profile = static_cast<uint8_t>(floor);
    } catch (const json::exception& e) {
        return Result<ProfileTable>::fail(std::string("profile table: ") + e.what());
    }

    if (has_duplicate_ids(table)) {
        return Result<ProfileTable>::fail("profile table: duplicate profile ids (§3.6)");
    }
    bool floor_found = false;
    for (const Profile& p : table.profiles) {
        floor_found = floor_found || p.id == table.floor_profile;
    }
    if (!floor_found) {
        return Result<ProfileTable>::fail("profile table: floor_profile does not name an existing profile id");
    }
    return Result<ProfileTable>::ok(std::move(table));
}

Result<ProfileTable> load_profile_table(const std::string& path) {
    std::string err;
    const std::string text = read_file(path, err);
    if (!err.empty()) {
        return Result<ProfileTable>::fail(err);
    }
    return load_profile_table_json(text);
}

std::string dump_config_summary(const Config& cfg) {
    std::ostringstream ss;
    ss << "node " << cfg.node.originator
       << " role=" << (cfg.node.role == Role::kTx ? "tx" : "rx")
       << " preferred=" << cfg.node.preferred_originator << "\n";
    ss << "adapters (" << cfg.adapters.size() << "):\n";
    for (const AdapterCfg& a : cfg.adapters) {
        ss << "  " << a.name << " role=" << (a.role == Role::kTx ? "tx" : "rx")
           << " ch=" << a.channel_mhz << " bw=" << unsigned(a.bw);
        if (a.max_power_qdb) {
            ss << " max_power_qdb=" << *a.max_power_qdb;
        }
        ss << "\n";
    }
    ss << "streams (" << cfg.streams.size() << "):\n";
    for (const StreamCfg& s : cfg.streams) {
        ss << "  id=" << unsigned(s.stream_id) << " type=" << unsigned(s.stream_type)
           << " " << (s.dir == Dir::kIn ? "in  <- " + s.bind.listen
                                        : "out -> " + s.bind.send);
        if (s.stream_type == stream_type::kRtp && s.dir == Dir::kIn) {
            ss << " classifier="
               << (s.classifier == RtpClassifier::kH264   ? "h264"
                   : s.classifier == RtpClassifier::kH265 ? "h265"
                                                          : "size");
        }
        ss << "\n";
    }
    ss << "policy: report_hz=" << cfg.policy.report_hz
       << " demote_milli=" << cfg.policy.select.demote_milli
       << " fwd_clamp_blocks=" << cfg.policy.arq.fwd_clamp_blocks
       << " csa_psk=" << (cfg.policy.csa.psk.empty() ? "(unset)" : "(set, redacted)")
       << "\n";
    ss << "stats: hz=" << cfg.stats.hz << " -> "
       << (cfg.stats.bind ? cfg.stats.bind->send : std::string("stdout only"))
       << "\n";
    return ss.str();
}

}  // namespace wblink
