// SPDX-License-Identifier: GPL-2.0-or-later
// §10.6 (Pass 120) calibration artifact persistence — see calib_store.h.
#include "wblink/calib_store.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wblink/crc8.h"
#include "wblink/log.h"

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
        // close() BEFORE the check, and remove the partial file: a buffered
        // stream reports ENOSPC/EIO only when the buffer is actually flushed,
        // so testing good() at scope exit — where the destructor swallows the
        // error — promotes a truncated temp file over a valid artifact. The
        // §10.7 twin (io/src/uplink_calib_store.cpp) was fixed for exactly
        // this; §10.6 was left behind, and Pass 195's legacy fallback makes it
        // matter more: a zero-length per-identity file falls through to a
        // superseded artifact.
        f.close();
        if (!f.good()) {
            std::remove(tmp.c_str());
            return false;
        }
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

std::string calib_identity_slug(const std::string& identity) {
    std::string out;
    out.reserve(identity.size());
    for (char c : identity) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        c == '.' || c == '_' || c == '-';
        out += ok ? c : '_';
    }
    return out;
}

std::string calib_identity(const AdapterCfg& adapter, AirCfg::Kind backend,
                           const std::string& efuse_mac) {
    (void)adapter;  // tiers 2-4 retired with kernel-monitor (Pass 164)
    // §10.7 (Pass 146; radio re-based Pass 154). Per-actuator by design, and
    // stable across a re-plug, which the old bus-path form was not.
    if (backend == AirCfg::Kind::kRadio) {
        // Pass 154: the single derived tier. NOT the USB serial, though it
        // looks like the obvious stable key: every RTL88x2 dongle measured
        // reports the placeholder "123456". NOT calib_id or the bus path
        // either — no fallback tier exists here (D3): an identity-less unit
        // gets no absolute curve, it does not get a weaker key.
        if (efuse_mac.empty()) {
            return {};
        }
        return "mac/" + efuse_mac;
    }
    // No other backend has a power actuator, so no other backend has a
    // calibration identity. The Pass 146 declared/ifname/bus tiers existed
    // only for kernel-monitor and went with it (Pass 164) — which is why
    // adapters[].calib_id and adapters[].ifname are now inert keys.
    return "udp";
}

uint8_t calib_store_write(const std::string& dir, const std::string& identity,
                          const CalibArtifact& a) {
    const std::string body = artifact_json(identity, a);
    const std::string slug = calib_identity_slug(identity);
    if (!write_atomic(dir + "/artifact-" + slug + ".json", body)) {
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
    (void)write_atomic(dir + "/curve-" + slug + ".txt", curve);
    return fingerprint_of(body);
}

Result<CalibStored> calib_store_load(const std::string& dir,
                                    const std::string& identity) {
    const std::string own =
        dir + "/artifact-" + calib_identity_slug(identity) + ".json";
    std::string body = read_file(own);
    if (body.empty()) {
        // ABSENT, not merely unreadable. read_file() cannot tell "no file"
        // from "zero bytes", and a zero-byte file is what a failed write
        // leaves — so keying the fallback on emptiness alone would let a
        // truncated write resurrect a SUPERSEDED legacy artifact and install
        // it as a fresh boot auto-load. Pre-195 the same truncation gave "no
        // artifact" and the node stayed on the §10.5 safe boot offset, which
        // is the behaviour to preserve.
        std::ifstream probe(own, std::ios::binary);
        if (probe.good()) {
            return Result<CalibStored>::fail(
                "artifact-<identity>.json is empty (a failed write?) — "
                "refusing to fall back to the pre-195 artifact.json");
        }
        // Pre-Pass-195 layout. Read it, but do not privilege it: the stored
        // identity is parsed below exactly as for a per-identity file and the
        // caller's own match check decides, so a legacy artifact belonging to
        // a different unit is refused the same way it always was.
        body = read_file(dir + "/artifact.json");
    }
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
