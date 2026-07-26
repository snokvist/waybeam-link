// SPDX-License-Identifier: GPL-2.0-or-later
// §15.5 Pass 104: operating-mode catalog (GET /api/v1/modes). See modes.h.
#include "wblink/modes.h"

#include <dirent.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace wblink {

namespace {

// One catalog row — the user-facing facts of a modes/<name>.json (§16).
struct ModeRow {
    std::string name;
    std::string resolution;  // .venc.video0.size, e.g. "1024x576"
    std::string fps_mode;    // .link.policy.fps_mode; static|variable
    int fps = 0;             // .venc.video0.fps (latency axis)
    int mcs_min = 0;         // .link.policy.select.min_profile (range band lo)
    int mcs_max = 0;         // .link.policy.select.max_profile (range band hi)
};

bool ends_with_json(const std::string& s) {
    static const std::string ext = ".json";
    return s.size() > ext.size() &&
           s.compare(s.size() - ext.size(), ext.size(), ext) == 0;
}

// Parse one mode file into `row`. Returns false (skip this mode) when the file
// is unreadable, unparseable, or missing any required field — never throws.
bool parse_mode(const std::string& path, const std::string& name,
                ModeRow& row) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    try {
        const auto j = nlohmann::json::parse(ss.str());
        const auto& v0 = j.at("venc").at("video0");
        const auto& sel = j.at("link").at("policy").at("select");
        row.name = name;
        row.fps = v0.at("fps").get<int>();
        row.resolution = v0.at("size").get<std::string>();
        row.mcs_min = sel.at("min_profile").get<int>();
        row.mcs_max = sel.at("max_profile").get<int>();
        // fps_mode is optional; §9.11 default is static (fps pinned, recordable).
        row.fps_mode = "static";
        const auto& pol = j.at("link").at("policy");
        if (pol.contains("fps_mode") && pol["fps_mode"].is_string()) {
            row.fps_mode = pol["fps_mode"].get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
        return false;  // partial/malformed mode file: skip, not fatal
    }
    return true;
}

// Enumerate + parse + name-sort the modes under `dir`. The single ordering
// authority: both the GET /api/v1/modes catalog and the §11.7 MODE index→name
// map read from this, so a malformed file skipped here is skipped for both and
// the ordinals never drift (Pass 104/105).
std::vector<ModeRow> collect_modes(const std::string& dir) {
    std::vector<ModeRow> modes;
    if (!dir.empty()) {
        if (DIR* d = ::opendir(dir.c_str())) {
            while (const dirent* e = ::readdir(d)) {
                const std::string entry = e->d_name;
                if (!ends_with_json(entry)) continue;
                const std::string name = entry.substr(0, entry.size() - 5);
                ModeRow row;
                if (parse_mode(dir + "/" + entry, name, row)) {
                    modes.push_back(std::move(row));
                }
            }
            ::closedir(d);
        }
    }
    // Name-sorted for a stable menu (readdir order is filesystem-defined).
    std::sort(modes.begin(), modes.end(),
              [](const ModeRow& a, const ModeRow& b) { return a.name < b.name; });
    return modes;
}

// §15.5 Pass 108: "<count>-<hex32>", FNV-1a-32 over the name-sorted mode names
// joined by '\n'. Names only, in catalog order — that is exactly what the §11.7
// MODE index→name map depends on, so editing a mode file's *contents* leaves the
// fingerprint stable by design.
std::string catalog_fingerprint(const std::vector<ModeRow>& modes) {
    uint32_t h = 2166136261u;  // FNV-1a-32 offset basis
    const auto feed = [&h](char c) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    };
    for (size_t i = 0; i < modes.size(); ++i) {
        if (i != 0) feed('\n');
        for (const char c : modes[i].name) feed(c);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%zu-%08x", modes.size(), h);
    return buf;
}

}  // namespace

std::string modes_catalog_json(const std::string& dir, const std::string& active,
                               bool apply_configured) {
    const std::vector<ModeRow> modes = collect_modes(dir);
    nlohmann::json out;
    out["active"] = active;
    out["apply_configured"] = apply_configured;
    out["catalog_fingerprint"] = catalog_fingerprint(modes);
    out["modes"] = nlohmann::json::array();
    for (const auto& m : modes) {
        out["modes"].push_back({{"name", m.name},
                                {"fps", m.fps},
                                {"resolution", m.resolution},
                                {"mcs_min", m.mcs_min},
                                {"mcs_max", m.mcs_max},
                                {"fps_mode", m.fps_mode}});
    }
    return out.dump();
}

std::string mode_name_at(const std::string& dir, size_t index) {
    const std::vector<ModeRow> modes = collect_modes(dir);
    if (index >= modes.size()) return "";  // §11.7 range error → REJECTED
    return modes[index].name;
}

}  // namespace wblink
