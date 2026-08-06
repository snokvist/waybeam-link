// SPDX-License-Identifier: GPL-2.0-or-later
// §10.6 (Pass 120) calibration artifact persistence — see calib_store.h.
#include "wblink/calib_store.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wblink/crc8.h"

namespace wblink {

using json = nlohmann::json;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_atomic(const std::string& path, const std::string& body) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << body;
        if (!f.good()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// Canonical serialization: nlohmann dump() with sorted keys (its default
// object ordering) — the fingerprint is the CRC-8 of these exact bytes.
std::string artifact_json(const std::string& identity,
                          const CalibArtifact& a) {
    json j;
    j["identity"] = identity;
    j["curve_qdb"] = a.curve_qdb;
    j["placement_qdb"] = a.placement_qdb;
    j["placement_rssi"] = a.placement_rssi;
    j["placement_loss_milli"] = a.placement_loss_milli;
    json ceils = json::array();
    for (size_t m = 0; m < a.ceilings.size(); ++m) {
        json c;
        c["rung"] = m;
        c["last_clean_rssi"] = a.ceilings[m].last_clean_rssi;
        if (a.ceilings[m].has_bad) {
            c["first_bad_rssi"] = a.ceilings[m].first_bad_rssi;
        } else {
            c["first_bad_rssi"] = nullptr;
        }
        ceils.push_back(std::move(c));
    }
    j["ceilings"] = std::move(ceils);
    return j.dump();
}

uint8_t fingerprint_of(const std::string& body) {
    const uint8_t fp = crc8_dvbs2(
        reinterpret_cast<const uint8_t*>(body.data()), body.size());
    return fp == 0 ? uint8_t{1} : fp;  // 0 = §3.15 "no artifact" sentinel
}

}  // namespace

const char* calib_backend_tag(AirCfg::Kind kind) {
    switch (kind) {
        case AirCfg::Kind::kRadio:
            return "radio";
        case AirCfg::Kind::kMonitor:
            return "monitor";
        default:
            return "udp";
    }
}

std::string calib_identity(const AdapterCfg& adapter, AirCfg::Kind backend) {
    // §10.7 (Pass 146). Per-actuator by design — a monitor-measured curve must
    // read STALE on devourer, not be applied — and stable across a re-plug,
    // which the old bus-path form was not.
    if (!adapter.calib_id.empty()) {
        // Scoped by backend: the derived tiers below distinguish backends for
        // free (an ifname only exists on monitor, a bus path only on devourer),
        // but calib_id is operator-chosen and would otherwise be identical
        // under both — silently applying a monitor curve to devourer's offset
        // actuator. Live case, not hypothetical: this fleet's craft ran
        // kernel-monitor before Pass 145 and now runs devourer on the same
        // physical adapter.
        return std::string("id/") + calib_backend_tag(backend) + "/" +
               adapter.calib_id;
    }
    if (!adapter.ifname.empty()) {
        const std::string mac = read_file("/sys/class/net/" + adapter.ifname +
                                          "/address");
        std::string m = mac;
        while (!m.empty() && (m.back() == '\n' || m.back() == ' ')) {
            m.pop_back();
        }
        return adapter.ifname + "/" + (m.empty() ? "?" : m);
    }
    if (!adapter.bus.empty()) {
        // NOT the USB serial, though it looks like the obvious stable key:
        // every RTL88x2 dongle measured — 8822EU and 8812CU on the bench, the
        // craft's 8822EU — reports the placeholder "123456". Keying on that
        // would let two adapters in one host share an artifact and apply each
        // other's curve without ever reading STALE, which is worse than the
        // instability below. On devourer the identity must be DECLARED.
        //
        // Last resort. Bus paths shuffle on any re-plug (CLAUDE.md), so an
        // artifact keyed this way goes stale the next time the dongle moves —
        // and the node then boots with no curve. Say so once, loudly.
        std::fprintf(stderr,
                     "calibrate: adapter \"%s\" has no calib_id — keying the "
                     "artifact on bus path \"%s\", which CHANGES on re-plug. "
                     "Set adapters[].calib_id to pin it (§10.7).\n",
                     adapter.name.c_str(), adapter.bus.c_str());
        return "bus/" + adapter.bus;
    }
    return "udp";
}

uint8_t calib_store_write(const std::string& dir, const std::string& identity,
                          const CalibArtifact& a) {
    const std::string body = artifact_json(identity, a);
    if (!write_atomic(dir + "/artifact.json", body)) {
        return 0;
    }
    // Operator-readable §10.2 curve twin (advisory; artifact.json is the
    // boot-load source of truth).
    std::string curve = "#[v2][Exact]#\n#[5G]A\n";
    char row[128];
    for (int i = 0; i < 20; i += 4) {
        double v[4];
        for (int k = 0; k < 4; ++k) {
            const int idx = i + k;
            const int m = idx < 12 ? 0 : idx - 12;  // legacy rows carry MCS0
            v[k] = a.curve_qdb[static_cast<size_t>(m)] / 4.0;
        }
        std::snprintf(row, sizeof row,
                      "[1]  0xc20  0xffffffff  %.1f %.1f %.1f %.1f\n", v[0],
                      v[1], v[2], v[3]);
        curve += row;
    }
    curve += "0xffff\n";
    (void)write_atomic(dir + "/curve.txt", curve);
    return fingerprint_of(body);
}

Result<CalibStored> calib_store_load(const std::string& dir) {
    const std::string body = read_file(dir + "/artifact.json");
    if (body.empty()) {
        return Result<CalibStored>::fail("no artifact");
    }
    json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        return Result<CalibStored>::fail("artifact.json: parse error");
    }
    CalibStored out;
    try {
        out.identity = j.at("identity").get<std::string>();
        const auto curve = j.at("curve_qdb");
        for (size_t m = 0; m < 8; ++m) {
            out.artifact.curve_qdb[m] = curve.at(m).get<int32_t>();
            out.curve.qdb[m] = out.artifact.curve_qdb[m];
            out.artifact.placement_qdb[m] =
                j.at("placement_qdb").at(m).get<int32_t>();
            out.artifact.placement_rssi[m] =
                j.at("placement_rssi").at(m).get<int8_t>();
            out.artifact.placement_loss_milli[m] =
                j.at("placement_loss_milli").at(m).get<uint16_t>();
            const auto& c = j.at("ceilings").at(m);
            out.artifact.ceilings[m].last_clean_rssi =
                c.at("last_clean_rssi").get<int8_t>();
            if (!c.at("first_bad_rssi").is_null()) {
                out.artifact.ceilings[m].has_bad = true;
                out.artifact.ceilings[m].first_bad_rssi =
                    c.at("first_bad_rssi").get<int8_t>();
            }
        }
    } catch (const json::exception& e) {
        return Result<CalibStored>::fail(std::string("artifact.json: ") +
                                         e.what());
    }
    out.curve.valid = true;
    out.fingerprint = fingerprint_of(body);
    return Result<CalibStored>::ok(std::move(out));
}

std::string calib_store_json(const std::string& state, uint8_t rung,
                             uint8_t fingerprint, bool stale,
                             const char* fail_reason,
                             const CalibArtifact* artifact) {
    json j;
    // §10.7 Pass 125: both directions answer at /api/v1/calibration, so the
    // response has to say which one it is. The craft is always downlink.
    j["direction"] = "downlink";
    j["state"] = state;
    j["rung"] = rung;
    j["fingerprint"] = fingerprint;
    j["stale"] = stale;
    j["fail_reason"] = fail_reason ? json(fail_reason) : json(nullptr);
    if (artifact != nullptr) {
        j["artifact"] = json::parse(artifact_json("", *artifact));
    } else {
        j["artifact"] = nullptr;
    }
    return j.dump();
}

}  // namespace wblink
