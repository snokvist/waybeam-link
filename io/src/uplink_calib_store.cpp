// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/uplink_calib_store.h"

#include <sys/stat.h>

#include <cerrno>

#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wblink/crc8.h"

namespace wblink {

namespace {

using json = nlohmann::json;

constexpr int kSchema = 1;
// §10.7 (Pass 195): one artifact per LOCAL adapter identity, the same rule
// and the same directory as §10.6's. kLegacyFile is the pre-195 fixed name,
// read as a fallback so a deployed ground keeps its measurement across the
// upgrade; the identity check in uplink_calib_matches() is unchanged and is
// still what decides whether it applies.
std::string artifact_path(const std::string& dir, const std::string& identity) {
    return dir + "/uplink-artifact-" + calib_identity_slug(identity) + ".json";
}
constexpr const char* kLegacyFile = "/uplink-artifact.json";

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
        // close() BEFORE the check: a buffered stream reports ENOSPC/EIO only
        // when the buffer is actually flushed, so testing good() at scope exit
        // — where the destructor swallows the error — promoted a truncated
        // temp file over a valid artifact. Leave no partial file behind.
        f.close();
        if (!f.good()) {
            std::remove(tmp.c_str());
            return false;
        }
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// ---- pinned binary canonicalization ---------------------------------------
// Fixed field order, big-endian (the §3.1 wire convention), length-prefixed
// strings, no padding. This is the ONLY thing the fingerprint covers, so it
// must never be reordered — a change here invalidates every stored artifact,
// which is exactly why it is not derived from the JSON text.
void put8(std::string& o, uint8_t v) { o.push_back(static_cast<char>(v)); }
void put16(std::string& o, uint16_t v) {
    put8(o, static_cast<uint8_t>(v >> 8));
    put8(o, static_cast<uint8_t>(v));
}
void put32(std::string& o, uint32_t v) {
    put16(o, static_cast<uint16_t>(v >> 16));
    put16(o, static_cast<uint16_t>(v));
}
void put_str(std::string& o, const std::string& s) {
    put16(o, static_cast<uint16_t>(s.size()));
    o += s;
}

std::string canonical_bytes(const UplinkArtifact& a) {
    std::string o;
    put8(o, static_cast<uint8_t>(kSchema));
    put_str(o, a.local_adapter_identity);
    put16(o, a.craft_originator);
    put8(o, a.craft_adapter_fingerprint);
    put16(o, a.channel_mhz);
    put8(o, a.bw_mhz);
    // t_unix is deliberately EXCLUDED: it is provenance, not identity. Two
    // measurements of the same hardware at the same operating point are the
    // same artifact, and hashing the clock would say otherwise.
    put16(o, static_cast<uint16_t>(a.placements.size()));
    for (const UplinkPlacement& p : a.placements) {
        put8(o, p.mcs);
        put8(o, p.short_gi ? 1 : 0);
        put32(o, static_cast<uint32_t>(p.placement_qdb));
        put8(o, static_cast<uint8_t>(p.placement_rssi_dbm));
        put16(o, p.placement_loss_milli);
        put32(o, static_cast<uint32_t>(p.last_clean_qdb));
        put8(o, p.has_first_bad ? 1 : 0);
        // Zeroed when absent so the hash ROUND-TRIPS: the writer serializes
        // `first_bad_qdb` as JSON null and the loader leaves the field at its
        // 0 default, so hashing a live non-zero value here made every such
        // artifact fail its own fingerprint on reload — the operator sees
        // DONE, the next boot silently discards the measurement.
        put32(o, p.has_first_bad ? static_cast<uint32_t>(p.first_bad_qdb) : 0u);
    }
    return o;
}

}  // namespace

uint8_t uplink_calib_fingerprint(const UplinkArtifact& a) {
    const std::string b = canonical_bytes(a);
    const uint8_t fp =
        crc8_dvbs2(reinterpret_cast<const uint8_t*>(b.data()), b.size());
    return fp == 0 ? uint8_t{1} : fp;  // 0 = "no artifact" sentinel
}

uint8_t uplink_calib_store_write(const std::string& dir,
                                 const UplinkArtifact& a) {
    const uint8_t fp = uplink_calib_fingerprint(a);
    json j;
    j["schema"] = kSchema;
    j["direction"] = "uplink";
    j["t_unix"] = a.t_unix;
    j["local_adapter_identity"] = a.local_adapter_identity;
    j["craft_originator"] = a.craft_originator;
    j["craft_adapter_fingerprint"] = a.craft_adapter_fingerprint;
    j["channel_mhz"] = a.channel_mhz;
    j["bw_mhz"] = a.bw_mhz;
    j["fingerprint"] = fp;
    json ps = json::array();
    for (const UplinkPlacement& p : a.placements) {
        json e;
        e["mcs"] = p.mcs;
        e["short_gi"] = p.short_gi;
        e["placement_qdb"] = p.placement_qdb;
        e["placement_rssi_dbm"] = p.placement_rssi_dbm;
        e["placement_loss_milli"] = p.placement_loss_milli;
        e["last_clean_qdb"] = p.last_clean_qdb;
        if (p.has_first_bad) {
            e["first_bad_qdb"] = p.first_bad_qdb;
        } else {
            e["first_bad_qdb"] = nullptr;
        }
        ps.push_back(std::move(e));
    }
    j["placements"] = std::move(ps);
    // The identity is IN the artifact, so the filename derives from it and
    // the write signature is unchanged.
    return write_atomic(artifact_path(dir, a.local_adapter_identity),
                        j.dump(2))
               ? fp
               : uint8_t{0};
}

Result<UplinkArtifact> uplink_calib_store_load(const std::string& dir,
                                              const std::string& identity) {
    const std::string own = artifact_path(dir, identity);
    std::string body = read_file(own);
    if (body.empty()) {
        // Same guard as the §10.6 twin, and for the same reason: read_file
        // cannot tell "absent" from "zero bytes", and zero bytes is what a
        // failed write leaves. Without this a truncated per-identity artifact
        // falls through to a SUPERSEDED legacy one whose identity matches, so
        // uplink_calib_matches() accepts it and the ground flies an old §10.7
        // placement. ENOENT specifically — any other open failure is
        // "present but unreadable", not absent.
        struct stat st;
        if (::stat(own.c_str(), &st) == 0 || errno != ENOENT) {
            return Result<UplinkArtifact>::fail(
                "uplink-artifact-<identity>.json is present but unreadable or "
                "empty (a failed write?) — refusing the pre-195 fallback");
        }
        body = read_file(dir + kLegacyFile);
    }
    if (body.empty()) return Result<UplinkArtifact>::fail("no artifact");
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return Result<UplinkArtifact>::fail("artifact parse failed");
    }
    if (j.value("schema", 0) != kSchema) {
        return Result<UplinkArtifact>::fail("artifact schema mismatch");
    }
    UplinkArtifact a;
    a.local_adapter_identity = j.value("local_adapter_identity", std::string{});
    a.craft_originator = static_cast<uint16_t>(j.value("craft_originator", 0u));
    a.craft_adapter_fingerprint =
        static_cast<uint8_t>(j.value("craft_adapter_fingerprint", 0u));
    a.channel_mhz = static_cast<uint16_t>(j.value("channel_mhz", 0u));
    a.bw_mhz = static_cast<uint8_t>(j.value("bw_mhz", 20u));
    a.t_unix = j.value("t_unix", int64_t{0});
    if (j.contains("placements") && j["placements"].is_array()) {
        for (const json& e : j["placements"]) {
            UplinkPlacement p;
            p.mcs = static_cast<uint8_t>(e.value("mcs", 0u));
            p.short_gi = e.value("short_gi", false);
            p.placement_qdb = e.value("placement_qdb", 0);
            // §10.5 wire range. The artifact auto-applies at boot with no
            // operator in the loop, and placement_qdb reaches set_power_qdb
            // directly — clamped further only when max_power_qdb happens to
            // be configured. The CRC-8 below is explicitly NOT a tamper seal,
            // so an out-of-range value is corruption, not a request: refuse
            // the whole artifact rather than actuate part of one.
            if (p.placement_qdb < -511 || p.placement_qdb > 511) {
                return Result<UplinkArtifact>::fail(
                    "uplink artifact: placement_qdb out of range "
                    "(-511..511)");
            }
            p.placement_rssi_dbm =
                static_cast<int8_t>(e.value("placement_rssi_dbm", 0));
            p.placement_loss_milli =
                static_cast<uint16_t>(e.value("placement_loss_milli", 0u));
            p.last_clean_qdb = e.value("last_clean_qdb", 0);
            p.has_first_bad = e.contains("first_bad_qdb") &&
                              !e["first_bad_qdb"].is_null();
            if (p.has_first_bad) p.first_bad_qdb = e.value("first_bad_qdb", 0);
            a.placements.push_back(p);
        }
    }
    // Integrity check, NOT a tamper seal — anyone who can edit the file can
    // edit this field. It catches truncation and accidental corruption, and
    // doubles as the short operator-visible identity in §15.3. CRC-8 gives
    // 8 bits, so it is a corruption check by design, not a guarantee.
    const uint8_t want = static_cast<uint8_t>(j.value("fingerprint", 0u));
    if (want == 0 || uplink_calib_fingerprint(a) != want) {
        return Result<UplinkArtifact>::fail("artifact fingerprint mismatch");
    }
    return Result<UplinkArtifact>::ok(std::move(a));
}

bool uplink_calib_matches(const UplinkArtifact& a,
                          const std::string& local_identity,
                          uint16_t craft_originator, uint8_t craft_fingerprint,
                          uint16_t channel_mhz, uint8_t bw_mhz) {
    return a.local_adapter_identity == local_identity &&
           a.craft_originator == craft_originator &&
           a.craft_adapter_fingerprint == craft_fingerprint &&
           a.channel_mhz == channel_mhz && a.bw_mhz == bw_mhz;
}

const UplinkPlacement* uplink_calib_placement_for(const UplinkArtifact& a,
                                                  uint8_t mcs, bool short_gi) {
    for (const UplinkPlacement& p : a.placements) {
        if (p.mcs == mcs && p.short_gi == short_gi) return &p;
    }
    return nullptr;
}

}  // namespace wblink
