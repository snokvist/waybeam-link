// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/config.h"

#include "wblink/calib_dwell.h"  // kMaxDwellFrames (§3.16)

#include "wblink/fps_ladder.h"  // §9.11 ladder-membership validation

#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
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
    if (s == "AUDIO") return Result<uint8_t>::ok(stream_type::kAudio);
    return Result<uint8_t>::fail(std::string(where) +
                                 ": unknown stream_type \"" + s +
                                 "\" (use RTP/TELEMETRY/CONTROL/AUDIO/UNKNOWN or a number)");
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
        cfg.node.spectator = n.value("spectator", false);
        if (cfg.node.spectator && cfg.node.role != Role::kRx) {
            return Result<Config>::fail(
                "node: spectator requires role \"rx\" (§2 passive RX, Pass 74)");
        }
        // §3.9 Pass 106 latch-triggered recovery (default on).
        cfg.node.recovery_on_latch = n.value("recovery_on_latch", true);
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
            // §10.7 (Pass 146): pins the calibration artifact to this
            // physical adapter regardless of ifname/serial/bus path.
            // kernel-monitor only since Pass 154 (frozen, ruling #120).
            ac.calib_id = a.value("calib_id", std::string{});
            // §15.2 (Pass 154): EFUSE-MAC stanza pin, radio backend only
            // (cross-checked against air.kind after the air block parses).
            // Normalized to lowercase so it compares against the backend's
            // formatted identity byte-for-byte.
            ac.mac = a.value("mac", std::string{});
            if (!ac.mac.empty()) {
                if (ac.mac.size() != 17) {
                    return Result<Config>::fail(
                        "adapter " + ac.name +
                        ": mac must be aa:bb:cc:dd:ee:ff");
                }
                for (size_t k = 0; k < ac.mac.size(); ++k) {
                    char& c = ac.mac[k];
                    if (k % 3 == 2) {
                        if (c != ':') {
                            return Result<Config>::fail(
                                "adapter " + ac.name +
                                ": mac must be aa:bb:cc:dd:ee:ff");
                        }
                    } else if (c >= 'A' && c <= 'F') {
                        c = static_cast<char>(c - 'A' + 'a');
                    } else if (!((c >= '0' && c <= '9') ||
                                 (c >= 'a' && c <= 'f'))) {
                        return Result<Config>::fail(
                            "adapter " + ac.name +
                            ": mac must be aa:bb:cc:dd:ee:ff");
                    }
                }
                for (const AdapterCfg& other : cfg.adapters) {
                    if (other.mac == ac.mac) {
                        return Result<Config>::fail(
                            "adapter " + ac.name + ": duplicate mac " +
                            ac.mac + " (also on \"" + other.name + "\")");
                    }
                }
            }
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
            // §10.2/§10.7 Pass 125: the rule is per-ADAPTER role, not per-node
            // role. A map on a role:"rx" diversity adapter is never actuated on
            // either node kind; a map on the single role:"tx" adapter IS —
            // the tx-node selector commit (§10.4) or the rx-node's designated
            // §6.4 uplink (§10.7). The old node-role test both permitted a
            // never-applied map on a tx-node diversity adapter and blocked the
            // one adapter that can use it on an rx node.
            if (ac.role == Role::kRx && !ac.power_map.empty()) {
                return Result<Config>::fail(
                    "adapter " + ac.name +
                    ": power_map on a role:\"rx\" adapter is never applied "
                    "(§10.2) — put it on the role:\"tx\" adapter");
            }
            if (a.contains("max_power_qdb")) {
                ac.max_power_qdb = a.at("max_power_qdb").get<int32_t>();
                // §10.3 (Pass 150): this is now kernel-monitor's absolute
                // REFERENCE, fed to `iw txpower fixed (ref + offset)`. The
                // pre-150 "disable the ceiling" idiom of a huge value (the
                // sample configs shipped 2000 = 500 dBm) would hand the driver
                // a nonsense absolute, so the range is now enforced.
                if (*ac.max_power_qdb < -40 || *ac.max_power_qdb > 120) {
                    return Result<Config>::fail(
                        "adapter " + ac.name + ": max_power_qdb " +
                        std::to_string(*ac.max_power_qdb) +
                        " out of range -40..120 qdb — since Pass 150 this is "
                        "the kernel-monitor absolute reference, not a ceiling "
                        "(§10.3/§10.5)");
                }
            }
            // §10.5 (Pass 150): relative offset + its bound. Parsed for every
            // adapter; only role:"tx" ever applies them.
            if (a.contains("power_offset_max_qdb")) {
                ac.power_offset_max_qdb =
                    a.at("power_offset_max_qdb").get<int32_t>();
            }
            if (a.contains("power_offset_qdb")) {
                ac.power_offset_qdb = a.at("power_offset_qdb").get<int32_t>();
            }
            if (ac.power_offset_qdb < -511 || ac.power_offset_qdb > 511 ||
                ac.power_offset_max_qdb < -511 ||
                ac.power_offset_max_qdb > 511) {
                return Result<Config>::fail(
                    "adapter " + ac.name +
                    ": power_offset_qdb/power_offset_max_qdb must be "
                    "-511..511 qdb (§10.5)");
            }
            // The boot point may not start above its own bound — otherwise a
            // node boots at a power the runtime latch would refuse to set.
            if (ac.power_offset_qdb > ac.power_offset_max_qdb) {
                return Result<Config>::fail(
                    "adapter " + ac.name + ": power_offset_qdb (" +
                    std::to_string(ac.power_offset_qdb) +
                    ") exceeds power_offset_max_qdb (" +
                    std::to_string(ac.power_offset_max_qdb) + ") (§10.5)");
            }
            // §11.7 0x0A TX_POWER preset list (Pass 135). Same rx rejection as
            // power_map above, and for the same reason: an rx adapter never
            // resolves power, so a list there would be silently inert.
            if (a.contains("power_presets_qdb")) {
                if (ac.role == Role::kRx) {
                    return Result<Config>::fail(
                        "adapter " + ac.name +
                        ": power_presets_qdb on a role:\"rx\" adapter is never "
                        "applied (§10.3) — put it on the role:\"tx\" adapter");
                }
                for (const json& v : a.at("power_presets_qdb")) {
                    ac.power_presets_qdb.push_back(v.get<int32_t>());
                }
                // §11.7 preset-index bound (Pass 68): at most 5 choices.
                if (ac.power_presets_qdb.size() > kVcmdMaxArg + 1u) {
                    return Result<Config>::fail(
                        "adapter " + ac.name +
                        ": power_presets_qdb holds more than 5 entries — "
                        "§11.7 cmd_arg indexes at most 5 choices");
                }
                if (ac.power_presets_qdb.empty()) {
                    return Result<Config>::fail(
                        "adapter " + ac.name +
                        ": power_presets_qdb is empty — omit the key instead");
                }
                // A tier may only LOWER power (Pass 135): the boot ceiling is
                // the operator's hard limit and no runtime path may pass it.
                // Logged when it binds — a silently lowered preset would read
                // back as a value the operator never chose.
                if (ac.max_power_qdb) {
                    for (int32_t& q : ac.power_presets_qdb) {
                        if (q > *ac.max_power_qdb) {
                            std::fprintf(stderr,
                                         "config: adapter %s: power preset %d "
                                         "qdb clamped to max_power_qdb %d "
                                         "(§10.3)\n",
                                         ac.name.c_str(), q, *ac.max_power_qdb);
                            q = *ac.max_power_qdb;
                        }
                    }
                }
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
            if (s.contains("arq_mode")) {
                if (sc.bind.kind != BindKind::kFrameShm || sc.dir != Dir::kIn) {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": arq_mode is only valid on frame-shm ingress");
                }
                const std::string mode = s.at("arq_mode").get<std::string>();
                if (mode == "idr-only") {
                    sc.arq_mode = FrameArqMode::kIdrOnly;
                } else if (mode == "all-frames") {
                    sc.arq_mode = FrameArqMode::kAllFrames;
                } else {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": arq_mode must be \"idr-only\" or \"all-frames\"");
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
                sc.fec.min_r = f.value("min_r", uint16_t{2});
                // §14.1a: optional third class. Absent (or explicit null) =>
                // inherit p_rate_permille, byte-identical to a pre-Pass-149
                // config. Never defaulted to 0 — that would silently strip
                // authored protection when a producer switched preset.
                if (f.contains("e_rate_permille") && !f.at("e_rate_permille").is_null()) {
                    const auto e = f.at("e_rate_permille").get<int64_t>();
                    if (e < 0 || e > 4000) {
                        return Result<Config>::fail(
                            "stream " + std::to_string(sid) +
                            ": fec.e_rate_permille must be 0..4000 (§14.1a)");
                    }
                    sc.fec.e_rate_permille = static_cast<uint16_t>(e);
                }
                if (sc.fec.scheme != FecScheme::kNone && sc.bind.kind != BindKind::kFrameShm) {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": fec.scheme is only valid on a frame-shm binding (§14.1)");
                }
                if (sc.fec.e_rate_permille && sc.fec.scheme == FecScheme::kNone) {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": fec.e_rate_permille needs fec.scheme \"rlc256\" (§14.1a)");
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
                jc.enforce = js.value("enforce", jc.enforce);
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
                if (jc.enforce && sc.fec.scheme != FecScheme::kRlc256) {
                    return Result<Config>::fail(
                        "stream " + std::to_string(sid) +
                        ": jscc_shadow.enforce requires fec.scheme=rlc256");
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
                const uint64_t demote_milli = ps.value(
                    "demote_milli", static_cast<uint64_t>(sel.demote_milli));
                const uint64_t emergency_loss_milli =
                    ps.value("emergency_loss_milli",
                             static_cast<uint64_t>(
                                 sel.emergency_loss_milli));
                const uint64_t loss_min_uniq =
                    ps.value("loss_min_uniq",
                             static_cast<uint64_t>(sel.loss_min_uniq));
                const uint64_t loss_persist_score =
                    ps.value("loss_persist_score",
                             static_cast<uint64_t>(sel.loss_persist_score));
                sel.rung_lockout_s =
                    ps.value("rung_lockout_s", sel.rung_lockout_s);
                const uint64_t rung_lockout_latch_count =
                    ps.value("rung_lockout_latch_count",
                             static_cast<uint64_t>(
                                 sel.rung_lockout_latch_count));
                if (demote_milli > 1000 ||
                    emergency_loss_milli > 1000 ||
                    emergency_loss_milli <= demote_milli) {
                    return Result<Config>::fail(
                        "select: require demote_milli < "
                        "emergency_loss_milli <= 1000");
                }
                if (loss_min_uniq == 0 ||
                    loss_min_uniq > std::numeric_limits<uint32_t>::max() ||
                    loss_persist_score == 0 ||
                    loss_persist_score >
                        std::numeric_limits<uint8_t>::max() ||
                    rung_lockout_latch_count == 0 ||
                    rung_lockout_latch_count >
                        std::numeric_limits<uint8_t>::max()) {
                    return Result<Config>::fail(
                        "select: loss_min_uniq, loss_persist_score, "
                        "and rung_lockout_latch_count must be positive "
                        "and fit their wire/core integer widths");
                }
                const double max_lockout_s =
                    static_cast<double>(
                        std::numeric_limits<uint32_t>::max()) /
                    1000.0;
                if (!std::isfinite(sel.rung_lockout_s) ||
                    sel.rung_lockout_s <= 0.0 ||
                    sel.rung_lockout_s > max_lockout_s) {
                    return Result<Config>::fail(
                        "select: rung_lockout_s must be a positive finite "
                        "duration representable in milliseconds");
                }
                sel.demote_milli = static_cast<uint16_t>(demote_milli);
                sel.emergency_loss_milli =
                    static_cast<uint16_t>(emergency_loss_milli);
                sel.loss_min_uniq = static_cast<uint32_t>(loss_min_uniq);
                sel.loss_persist_score =
                    static_cast<uint8_t>(loss_persist_score);
                sel.rung_lockout_latch_count =
                    static_cast<uint8_t>(rung_lockout_latch_count);
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
                arq.arq_max_fps = pa.value("arq_max_fps", arq.arq_max_fps);
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
                cfg.policy.ret.report_redundancy = pr.value(
                    "report_redundancy", cfg.policy.ret.report_redundancy);
                if (cfg.policy.ret.report_redundancy < 1) {
                    return Result<Config>::fail(
                        "policy.return.report_redundancy: must be >= 1");
                }
            }
            if (p.contains("csa")) {
                const json& pc = p.at("csa");
                CsaPolicy& csa = cfg.policy.csa;
                csa.psk = pc.value("psk", csa.psk);
                csa.settle_s = pc.value("settle_s", csa.settle_s);
                csa.verify_timeout_ms =
                    pc.value("verify_timeout_ms", csa.verify_timeout_ms);
                csa.rx_liveness_ms =
                    pc.value("rx_liveness_ms", csa.rx_liveness_ms);
                // §15.2 (Pass 92): the §11.6 RX-liveness guard exists to catch
                // a half-applied retune. A verify window that outlives it lets
                // a full monitor re-init fire in the middle of a switch that
                // is still pending — the guard would be firing on a radio that
                // is merely waiting, not deaf. Ordering is a config invariant,
                // not a runtime tie-break.
                if (csa.rx_liveness_ms != 0 &&
                    csa.verify_timeout_ms >= csa.rx_liveness_ms) {
                    return Result<Config>::fail(
                        "policy.csa.verify_timeout_ms must be < "
                        "rx_liveness_ms (§11.6 guard fires mid-switch "
                        "otherwise)");
                }
                csa.min_interval_s = pc.value("min_interval_s", csa.min_interval_s);
                csa.ack_timeout_ms = pc.value("ack_timeout_ms", csa.ack_timeout_ms);
                csa.bind_release_s =
                    pc.value("bind_release_s", csa.bind_release_s);
                csa.persist_channel =
                    pc.value("persist_channel", csa.persist_channel);
                csa.home_chan = pc.value("home_chan", csa.home_chan);
                if (pc.contains("channel_allowlist")) {
                    for (const json& c : pc.at("channel_allowlist")) {
                        csa.channel_allowlist.push_back(c.get<uint16_t>());
                    }
                }
            }
            if (p.contains("calibration")) {  // §10.6 Pass 120
                const json& pk = p.at("calibration");
                CalibrationPolicy& cal = cfg.policy.calibration;
                // Pass 121: the Pass 120 band keys (target_rssi_dbm,
                // rssi_tol_db, ceil_step_qdb) are retired and ignored.
                cal.loss_ok_milli = pk.value("loss_ok_milli", cal.loss_ok_milli);
                cal.loss_bad_milli =
                    pk.value("loss_bad_milli", cal.loss_bad_milli);
                cal.seek_step_qdb = pk.value("seek_step_qdb", cal.seek_step_qdb);
                cal.offset_seek_step_qdb =
                    pk.value("offset_seek_step_qdb", cal.offset_seek_step_qdb);
                cal.rssi_guard_dbm =
                    pk.value("rssi_guard_dbm", cal.rssi_guard_dbm);
                cal.min_qdb = pk.value("min_qdb", cal.min_qdb);
                cal.max_qdb = pk.value("max_qdb", cal.max_qdb);
                cal.settle_ms = pk.value("settle_ms", cal.settle_ms);
                cal.hard_cap_ms = pk.value("hard_cap_ms", cal.hard_cap_ms);
                cal.artifact_dir = pk.value("artifact_dir", cal.artifact_dir);
                // §3.16 (Pass 153) shared dwell knobs — probe COUNTS, never
                // milliseconds. The Pass-152-era keys (probe_dwell_ms,
                // verify_dwell_ms, report_loss_abort_ms, calib_min_report_hz,
                // uplink_probe_epochs, uplink_verify_epochs, uplink_drain_ms,
                // uplink_liveness_ms, uplink_floor_min_samples) are retired
                // and ignored.
                cal.dwell_probe_frames =
                    pk.value("dwell_probe_frames", cal.dwell_probe_frames);
                cal.dwell_verify_frames =
                    pk.value("dwell_verify_frames", cal.dwell_verify_frames);
                cal.probe_pace_us =
                    pk.value("probe_pace_us", cal.probe_pace_us);
                cal.tally_wait_ms =
                    pk.value("tally_wait_ms", cal.tally_wait_ms);
                cal.tally_retries =
                    pk.value("tally_retries", cal.tally_retries);
                cal.feed_quiet_ms =
                    pk.value("feed_quiet_ms", cal.feed_quiet_ms);
                if (cal.min_qdb > cal.max_qdb) {
                    return Result<Config>::fail(
                        "policy.calibration: min_qdb > max_qdb (§10.6)");
                }
                // The seek moves in whole steps and judges the cap wall on a
                // >= 2 dB commanded rise. A step of 0 or less never advances
                // (or walks backward into an unbounded negative qdb handed
                // straight to set_power_qdb); a step under 2 dB can never
                // satisfy the cap-wall test, silently disabling one of the
                // three walls §10.6 places against. 8 qdb IS 2 dB.
                if (cal.seek_step_qdb < 8) {
                    return Result<Config>::fail(
                        "policy.calibration: seek_step_qdb must be >= 8 (2 dB "
                        "— the cap wall's minimum commanded step, §10.6)");
                }
                // §10.6 (Pass 151): the offset window is 24 qdb by default,
                // so this step decides how many probes a relative-backend
                // sweep gets. Bounded on both sides — the devourer TXAGC
                // granularity is 2 qdb (0.5 dB) on Jaguar1/2 and 1 qdb on
                // Jaguar3, so under 2 qdb the sweep aliases on the coarser
                // families (two probes landing on one register value); over
                // 24 leaves a default window with two probes, which is the
                // condition this key exists to prevent.
                if (cal.offset_seek_step_qdb < 2 ||
                    cal.offset_seek_step_qdb > 24) {
                    return Result<Config>::fail(
                        "policy.calibration: offset_seek_step_qdb must be "
                        "2..24 qdb (0.5..6 dB, §10.6 Pass 151)");
                }
                // §3.16 (Pass 153): dwell bursts are bounded below by 1 and
                // above by the receiver's exact-dedup bitmap.
                if (cal.dwell_probe_frames < 1 ||
                    cal.dwell_probe_frames > int{kMaxDwellFrames} ||
                    cal.dwell_verify_frames < 1 ||
                    cal.dwell_verify_frames > int{kMaxDwellFrames}) {
                    return Result<Config>::fail(
                        "policy.calibration: dwell_probe_frames/"
                        "dwell_verify_frames must be 1..1024 (§3.16)");
                }
                if (cal.probe_pace_us < 1 || cal.tally_wait_ms < 1 ||
                    cal.tally_retries < 0 || cal.feed_quiet_ms < 1) {
                    return Result<Config>::fail(
                        "policy.calibration: probe pacing / tally gates must "
                        "be >= 1 (tally_retries >= 0) (§3.16)");
                }
                // Pass 132 (kept verbatim on the new primitive): a burst too
                // small to resolve the walls decides on noise. One lost probe
                // must land at or under loss_ok_milli, i.e. 1000/N <=
                // loss_ok_milli.
                if (cal.loss_ok_milli > 0 &&
                    1000 / cal.dwell_probe_frames > cal.loss_ok_milli) {
                    return Result<Config>::fail(
                        "policy.calibration: dwell_probe_frames too small to "
                        "resolve loss_ok_milli — one lost probe must be <= it "
                        "(§3.16)");
                }
            }
            if (p.contains("cmd")) {
                const json& pm = p.at("cmd");
                CmdPolicy& cmd = cfg.policy.cmd;
                cmd.copies = pm.value("copies", cmd.copies);
                cmd.copy_interval_ms =
                    pm.value("copy_interval_ms", cmd.copy_interval_ms);
                cmd.echo_copies = pm.value("echo_copies", cmd.echo_copies);
                cmd.ack_timeout_ms =
                    pm.value("ack_timeout_ms", cmd.ack_timeout_ms);
                cmd.retry_cap = pm.value("retry_cap", cmd.retry_cap);
                // Both narrow to uint8_t in vcmd_params() and are decremented
                // pre-test, so 0 wraps to 255: a 3-copy command becomes a
                // 256-copy transmit storm. Range-check where the value is
                // still wide.
                if (cmd.copies < 1 || cmd.copies > 255) {
                    return Result<Config>::fail(
                        "policy.cmd.copies must be in [1,255] (§11.7), got " +
                        std::to_string(cmd.copies));
                }
                if (cmd.retry_cap < 1 || cmd.retry_cap > 255) {
                    return Result<Config>::fail(
                        "policy.cmd.retry_cap must be in [1,255] (§11.7), got " +
                        std::to_string(cmd.retry_cap));
                }
                cmd.min_interval_ms =
                    pm.value("min_interval_ms", cmd.min_interval_ms);
            }
        }

        // stats
        if (j.contains("stats")) {
            const json& st = j.at("stats");
            cfg.stats.hz = st.value("hz", cfg.stats.hz);
            cfg.stats.to_stdout = st.value("stdout", cfg.stats.to_stdout);
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

        // scout (§15.5a ground channel searcher)
        if (j.contains("scout")) {
            const json& sc = j.at("scout");
            cfg.scout.dwell_ms = sc.value("dwell_ms", cfg.scout.dwell_ms);
            if (sc.contains("channels") && !sc.at("channels").is_null()) {
                for (const json& ch : sc.at("channels")) {
                    cfg.scout.channels.push_back(ch.get<uint16_t>());
                }
            }
        }

        // cache (§14.3 spatial cache repair; both roles default off)
        if (j.contains("cache")) {
            const json& c = j.at("cache");
            if (c.contains("repair")) {
                const json& r = c.at("repair");
                CacheRepairCfg& cr = cfg.cache.repair;
                cr.enabled = r.value("enabled", cr.enabled);
                cr.stream_id = r.value("stream_id", cr.stream_id);
                cr.listen = r.value("listen", cr.listen);
                for (const json& e : r.value("caches", json::array())) {
                    CacheEndpointCfg ep;
                    ep.originator = e.at("originator").get<uint16_t>();
                    ep.endpoint = e.at("endpoint").get<std::string>();
                    cr.caches.push_back(std::move(ep));
                }
                cr.tail_grace_ms = r.value("tail_grace_ms", cr.tail_grace_ms);
                cr.local_quiet_ms =
                    r.value("local_quiet_ms", cr.local_quiet_ms);
                cr.min_collect_ms =
                    r.value("min_collect_ms", cr.min_collect_ms);
                cr.hard_close_ms = r.value("hard_close_ms", cr.hard_close_ms);
                cr.request_timeout_ms =
                    r.value("request_timeout_ms", cr.request_timeout_ms);
                cr.nack_grace_ms =
                    r.value("nack_grace_ms", cr.nack_grace_ms);
                cr.repair_fraction_permille = r.value(
                    "repair_fraction_permille", cr.repair_fraction_permille);
                cr.absolute_symbol_limit = r.value("absolute_symbol_limit",
                                                   cr.absolute_symbol_limit);
                cr.max_cache_attempts =
                    r.value("max_cache_attempts", cr.max_cache_attempts);
                cr.reply_limit = r.value("reply_limit", cr.reply_limit);
                cr.health_floor_permille = r.value("health_floor_permille",
                                                   cr.health_floor_permille);
                cr.status_timeout_ms =
                    r.value("status_timeout_ms", cr.status_timeout_ms);
                cr.assignment_interval_ms = r.value(
                    "assignment_interval_ms", cr.assignment_interval_ms);
                if (cr.enabled) {
                    if (cr.caches.empty() || cr.listen.empty()) {
                        return Result<Config>::fail(
                            "cache.repair: enabled requires a non-empty "
                            "caches list and a listen address (§15.2)");
                    }
                    bool frame_shm_out = false;
                    for (const StreamCfg& s : cfg.streams) {
                        frame_shm_out |= s.stream_id == cr.stream_id &&
                                         s.dir == Dir::kOut &&
                                         s.bind.kind == BindKind::kFrameShm;
                    }
                    if (!frame_shm_out) {
                        return Result<Config>::fail(
                            "cache.repair: stream_id must name a frame-shm "
                            "egress stream (§15.2)");
                    }
                    if (cr.repair_fraction_permille > 1000) {
                        return Result<Config>::fail(
                            "cache.repair: repair_fraction_permille is 0..1000");
                    }
                    if (cr.nack_grace_ms > 6) {
                        return Result<Config>::fail(
                            "cache.repair: nack_grace_ms is 0..6");
                    }
                    if (cr.assignment_interval_ms == 0) {
                        return Result<Config>::fail(
                            "cache.repair: assignment_interval_ms must be >= 1");
                    }
                }
            }
            if (c.contains("store")) {
                const json& s = c.at("store");
                CacheStoreCfg& cs = cfg.cache.store;
                cs.enabled = s.value("enabled", cs.enabled);
                cs.listen = s.value("listen", cs.listen);
                for (const json& id : s.value("stream_ids", json::array())) {
                    cs.stream_ids.push_back(id.get<uint8_t>());
                }
                cs.blocks = s.value("blocks", cs.blocks);
                cs.reply_limit = s.value("reply_limit", cs.reply_limit);
                for (const json& t : s.value("status_to", json::array())) {
                    cs.status_to.push_back(t.get<std::string>());
                }
                cs.status_interval_ms =
                    s.value("status_interval_ms", cs.status_interval_ms);
                cs.max_requests_per_s =
                    s.value("max_requests_per_s", cs.max_requests_per_s);
                if (s.contains("controller")) {
                    const json& ctl = s.at("controller");
                    CacheEndpointCfg ep;
                    ep.originator = ctl.value("originator", uint16_t{0});
                    ep.endpoint = ctl.value("endpoint", std::string());
                    cs.controller = std::move(ep);
                }
                if (cs.enabled) {
                    if (cs.listen.empty() || cs.stream_ids.empty()) {
                        return Result<Config>::fail(
                            "cache.store: enabled requires a listen address "
                            "and stream_ids (§15.2)");
                    }
                    if (cs.blocks == 0) {
                        return Result<Config>::fail(
                            "cache.store: blocks must be >= 1");
                    }
                    if (cs.controller &&
                        (cs.controller->originator == 0 ||
                         cs.controller->endpoint.empty())) {
                        return Result<Config>::fail(
                            "cache.store: controller requires non-zero "
                            "originator and endpoint");
                    }
                }
            }
        }

        // venc (§9.6 encoder actuation; disabled default for dev/bench)
        if (j.contains("venc")) {
            const json& v = j.at("venc");
            cfg.venc.host = v.value("host", cfg.venc.host);
            cfg.venc.enabled = v.value("enabled", cfg.venc.enabled);
            cfg.venc.recovery_enabled =
                v.value("recovery_enabled", cfg.venc.recovery_enabled);
            cfg.venc.max_bitrate_kbps =
                v.value("max_bitrate_kbps", cfg.venc.max_bitrate_kbps);
            if (cfg.venc.max_bitrate_kbps != 0 &&
                cfg.venc.max_bitrate_kbps < 1000) {
                return Result<Config>::fail(
                    "venc: max_bitrate_kbps must be 0 (unlimited) or >= 1000 "
                    "(§9.6 venc hard floor, Pass 75)");
            }
            cfg.venc.frame_caps = v.value("frame_caps", cfg.venc.frame_caps);
            cfg.venc.fps_hint = v.value("fps_hint", cfg.venc.fps_hint);
            cfg.venc.i_headroom_permille = v.value(
                "i_headroom_permille", cfg.venc.i_headroom_permille);
            cfg.venc.p_headroom_permille = v.value(
                "p_headroom_permille", cfg.venc.p_headroom_permille);
            cfg.venc.cap_ceiling_bytes =
                v.value("cap_ceiling_bytes", cfg.venc.cap_ceiling_bytes);
            cfg.venc.settle_ms = v.value("settle_ms", cfg.venc.settle_ms);
            if (cfg.venc.fps_hint == 0 ||
                cfg.venc.i_headroom_permille > 1000 ||
                cfg.venc.p_headroom_permille > 1000) {
                return Result<Config>::fail(
                    "venc: fps_hint must be >= 1 and headrooms 0..1000");
            }
            if (v.contains("fps_ladder")) {
                const json& fl = v.at("fps_ladder");
                FpsLadderCfg& lc = cfg.venc.fps_ladder;
                lc.enabled = fl.value("enabled", lc.enabled);
                lc.min = fl.value("min", lc.min);
                lc.preferred = fl.value("preferred", lc.preferred);
                lc.max = fl.value("max", lc.max);
                lc.min_p_frame_bytes =
                    fl.value("min_p_frame_bytes", lc.min_p_frame_bytes);
                lc.restore_hysteresis_bytes = fl.value(
                    "restore_hysteresis_bytes", lc.restore_hysteresis_bytes);
                lc.sample_timeout_ms =
                    fl.value("sample_timeout_ms", lc.sample_timeout_ms);
                lc.reduce_after_ms =
                    fl.value("reduce_after_ms", lc.reduce_after_ms);
                lc.reduce_dwell_ms =
                    fl.value("reduce_dwell_ms", lc.reduce_dwell_ms);
                lc.restore_after_ms =
                    fl.value("restore_after_ms", lc.restore_after_ms);
                lc.settle_ms = fl.value("settle_ms", lc.settle_ms);
                if (lc.enabled) {
                    if (!fps_ladder_member(lc.min) ||
                        !fps_ladder_member(lc.preferred) ||
                        !fps_ladder_member(lc.max) ||
                        lc.min > lc.preferred || lc.preferred > lc.max) {
                        return Result<Config>::fail(
                            "venc.fps_ladder: min <= preferred <= max, all "
                            "ladder members (§9.11)");
                    }
                    if (lc.min_p_frame_bytes == 0 ||
                        lc.sample_timeout_ms == 0 ||
                        lc.restore_hysteresis_bytes >
                            UINT32_MAX - lc.min_p_frame_bytes) {
                        return Result<Config>::fail(
                            "venc.fps_ladder: frame-size floor/timeout invalid "
                            "(§9.11)");
                    }
                    if (!cfg.venc.enabled) {
                        return Result<Config>::fail(
                            "venc.fps_ladder: requires venc.enabled (§9.11)");
                    }
                }
            }
            // §11.7 v2 command presets (Pass 71): at most 5 entries per list
            // (the Pass 68 cmd_arg 0..4 bound); fps entries must be §9.11
            // ladder members (cap coupling assumes rungs).
            if (v.contains("command_presets")) {
                const json& cp = v.at("command_presets");
                if (cp.contains("fps")) {
                    cfg.venc.preset_fps =
                        cp.at("fps").get<std::vector<uint16_t>>();
                }
                if (cp.contains("resolution")) {
                    cfg.venc.preset_resolution =
                        cp.at("resolution").get<std::vector<std::string>>();
                }
                if (cp.contains("framing")) {
                    cfg.venc.preset_framing =
                        cp.at("framing").get<std::vector<std::string>>();
                }
                if (cfg.venc.preset_fps.size() > 5 ||
                    cfg.venc.preset_resolution.size() > 5 ||
                    cfg.venc.preset_framing.size() > 5) {
                    return Result<Config>::fail(
                        "venc.command_presets: at most 5 entries per list "
                        "(§11.7)");
                }
                for (const uint16_t f : cfg.venc.preset_fps) {
                    if (!fps_ladder_member(f)) {
                        return Result<Config>::fail(
                            "venc.command_presets.fps: entries must be §9.11 "
                            "ladder members");
                    }
                }
            }
            // §15.5 operating-mode selection (Pass 96).
            cfg.venc.active_mode = v.value("active_mode", cfg.venc.active_mode);
            cfg.venc.mode_apply_cmd =
                v.value("mode_apply_cmd", cfg.venc.mode_apply_cmd);
            // §15.5 Pass 104: modes_dir for GET /api/v1/modes. Empty here is
            // resolved to mode_apply_cmd's directory at wiring time.
            cfg.venc.modes_dir = v.value("modes_dir", cfg.venc.modes_dir);
            // The mode name is passed to a forked applier as argv (never a
            // shell), but keep it to a filesystem-safe charset so it also names
            // a modes/<name>.json without surprises.
            for (char c : cfg.venc.active_mode) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
                      c == '_' || c == '.')) {
                    return Result<Config>::fail(
                        "venc.active_mode: only [A-Za-z0-9._-] allowed");
                }
            }
        }

        // air ("udp" = dev backend, not §15; "radio" = devourer, §3.0 —
        // radio adapters come from the top-level adapters array)
        if (j.contains("air")) {
            const json& a = j.at("air");
            const std::string kind = a.value("kind", std::string("udp"));
            cfg.air.rx_drop_permille = static_cast<uint16_t>(
                std::min(1000, std::max(0, a.value("rx_drop_permille", 0))));
            const uint32_t airtime_efficiency = a.value(
                "airtime_efficiency_permille",
                uint32_t{cfg.air.airtime_efficiency_permille});
            if (airtime_efficiency > 1000) {
                return Result<Config>::fail(
                    "air: airtime_efficiency_permille must be 0..1000");
            }
            cfg.air.airtime_efficiency_permille =
                static_cast<uint16_t>(airtime_efficiency);
            cfg.air.wedge_window_ms =
                a.value("wedge_window_ms", cfg.air.wedge_window_ms);
            cfg.air.wedge_min_submits =
                a.value("wedge_min_submits", cfg.air.wedge_min_submits);
            cfg.air.wedge_exit_windows =
                a.value("wedge_exit_windows", cfg.air.wedge_exit_windows);
            cfg.air.disable_cca = a.value("disable_cca", cfg.air.disable_cca);
            cfg.air.ack_responder =
                a.value("ack_responder", cfg.air.ack_responder);
            // §15.2 (Pass 156): unicast hardware-retry limit, 0-63 (the
            // TX-descriptor field width).
            cfg.air.tx_retry_limit =
                a.value("tx_retry_limit", cfg.air.tx_retry_limit);
            if (cfg.air.tx_retry_limit < 0 || cfg.air.tx_retry_limit > 63) {
                return Result<Config>::fail(
                    "air.tx_retry_limit " +
                    std::to_string(cfg.air.tx_retry_limit) +
                    " out of range 0..63 (§15.2 Pass 156)");
            }
            // §15.2 (Pass 157) node TX coding; radio-only enforcement is
            // below, once kind resolves.
            cfg.air.ldpc = a.value("ldpc", cfg.air.ldpc);
            cfg.air.stbc = a.value("stbc", cfg.air.stbc);
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
            // §14.2: the authored calibration is a transport-efficiency
            // measurement, so it is valid on either RF backend (Pass 143) and
            // meaningless on the udp bench transports. Per transport — the
            // monitor rig's seed does not carry over to devourer.
            if (cfg.air.kind != AirCfg::Kind::kMonitor &&
                cfg.air.kind != AirCfg::Kind::kRadio &&
                cfg.air.airtime_efficiency_permille != 0) {
                return Result<Config>::fail(
                    "air: airtime_efficiency_permille is only valid for "
                    "kernel-monitor and radio");
            }
            // §10.7 uplink_rate: the rx node's committed operating point.
            if (a.contains("uplink_rate")) {
                const json& ur = a.at("uplink_rate");
                const uint32_t mcs = ur.value("mcs", 0u);
                if (mcs > 7) {
                    return Result<Config>::fail(
                        "air.uplink_rate: mcs must be 0..7 (§9.3 HT rungs)");
                }
                const uint32_t ubw = ur.value("bw", 20u);
                if (ubw != 20 && ubw != 40) {
                    return Result<Config>::fail(
                        "air.uplink_rate: bw must be 20 or 40 (HT only)");
                }
                cfg.air.uplink_mcs = static_cast<uint8_t>(mcs);
                cfg.air.uplink_sgi = ur.value("sgi", false);
                cfg.air.uplink_bw = static_cast<uint8_t>(ubw);
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

        // §3.0/§15.2 (Pass 156) coupling law: the hardware-ACK hybrid's
        // knobs are ONE decision. Enabling unicast returns or the ACK
        // responder with a zero retry limit arms an ARQ loop at one end and
        // disables it at the other — a knob that reads enabled while doing
        // nothing, refused rather than run inert. Radio backend only: the
        // kernel MAC owns its own retries on kernel-monitor (frozen, #120).
        if (cfg.air.kind == AirCfg::Kind::kRadio &&
            cfg.air.tx_retry_limit == 0 &&
            (cfg.air.ack_responder || cfg.policy.ret.unicast)) {
            return Result<Config>::fail(
                std::string("air.tx_retry_limit is 0 while ") +
                (cfg.air.ack_responder ? "air.ack_responder"
                                       : "return.unicast") +
                " is enabled — the hardware-ACK hybrid would run silently "
                "inert (§3.0 Pass 156); set a nonzero limit or disable the "
                "hybrid");
        }

        // §15.2 (Pass 157): TX coding keys are radio-backend-only — a dead
        // key on udp-air, an unverifiable leg on frozen kernel-monitor
        // (#120). Same posture as adapters[].mac below.
        if (cfg.air.kind != AirCfg::Kind::kRadio &&
            (cfg.air.ldpc || cfg.air.stbc)) {
            return Result<Config>::fail(
                std::string(cfg.air.ldpc ? "air.ldpc" : "air.stbc") +
                " is a radio-backend key (§15.2 Pass 157); remove it or set "
                "air.kind \"radio\"");
        }

        // §15.2 (Pass 154): adapters[].mac is the radio backend's EFUSE
        // identity pin — on any other backend it would be a silently dead
        // key promising a binding that never happens, so it is rejected.
        if (cfg.air.kind != AirCfg::Kind::kRadio) {
            for (const AdapterCfg& a : cfg.adapters) {
                if (!a.mac.empty()) {
                    return Result<Config>::fail(
                        "adapter " + a.name +
                        ": mac is a radio-backend key (§15.2 Pass 154); "
                        "kernel-monitor derives identity from ifname");
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
            // §3.2/§9.3a complete DATA packet budget. 4096 remains the decode
            // allocation ceiling; authored v1 TX profiles stop at High/3072.
            const uint32_t mp = pj.value("max_payload", uint32_t{kDefaultMaxPayload});
            if (mp < kDataHeaderSize + 32 || mp > mtu_tier::kHighBudget) {
                return Result<ProfileTable>::fail(
                    where + ": max_payload must be in [" +
                    std::to_string(kDataHeaderSize + 32) + ", " +
                    std::to_string(mtu_tier::kHighBudget) + "]");
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
        if (!a.mac.empty()) {
            ss << " mac=" << a.mac;  // §15.2 Pass 154 stanza pin
        }
        ss << " power_offset_qdb=" << a.power_offset_qdb
           << " power_offset_max_qdb=" << a.power_offset_max_qdb;
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
    ss << "stats: hz=" << cfg.stats.hz
       << " stdout=" << (cfg.stats.to_stdout ? "on" : "off")
       << " -> "
       << (cfg.stats.bind ? cfg.stats.bind->send
                          : std::string(cfg.stats.to_stdout ? "stdout only"
                                                            : "(no sink)"))
       << "\n";
    return ss.str();
}

}  // namespace wblink
