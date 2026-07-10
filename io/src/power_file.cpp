// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/power_file.h"

#include <cmath>
#include <fstream>
#include <sstream>

namespace wblink {

namespace {

// Driver rate-index enum (hal_com_phycfg.c): MGN_MCS0 = 12.
constexpr size_t kHtMcs0Index = 12;

bool is_terminator(const std::string& tok) { return tok == "0xffff"; }

}  // namespace

Result<PowerCurve> parse_power_curve(const std::string& text, bool band_5g) {
    const std::string want =
        band_5g ? std::string("#[5G]A") : std::string("#[2.4G]A");

    std::istringstream in(text);
    std::string line;
    bool in_section = false;
    bool section_seen = false;
    size_t rate_index = 0;
    size_t filled = 0;
    PowerCurve curve;

    while (std::getline(in, line)) {
        // Trim trailing CR (files often come from Windows-authored trees).
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream ls(line);
        std::string tok;
        if (!(ls >> tok)) {
            continue;  // blank
        }
        if (tok.rfind("#[", 0) == 0) {
            if (tok == want) {
                in_section = true;
                section_seen = true;
                rate_index = 0;
            } else if (tok.rfind("#[v", 0) == 0) {
                // format/version header — ignore
            } else {
                in_section = false;  // some other band/path section
            }
            continue;
        }
        if (!in_section) {
            continue;
        }
        if (is_terminator(tok)) {
            in_section = false;
            continue;
        }
        // Row: [TxNum] reg mask v1 v2 v3 v4 — 4 sequential rate values.
        if (tok.front() != '[') {
            return Result<PowerCurve>::fail(
                "power map: expected \"[TxNum]\" row start, got \"" + tok +
                "\"");
        }
        std::string reg, mask;
        if (!(ls >> reg >> mask)) {
            return Result<PowerCurve>::fail(
                "power map: truncated row (need reg + mask + 4 values)");
        }
        for (int i = 0; i < 4; ++i, ++rate_index) {
            double dbm = 0.0;
            if (!(ls >> dbm)) {
                return Result<PowerCurve>::fail(
                    "power map: row carries fewer than 4 values");
            }
            if (dbm < 0.0 || dbm > 63.0) {
                return Result<PowerCurve>::fail(
                    "power map: value out of dBm range [0, 63]");
            }
            if (rate_index >= kHtMcs0Index &&
                rate_index < kHtMcs0Index + curve.qdb.size()) {
                curve.qdb[rate_index - kHtMcs0Index] =
                    static_cast<int32_t>(std::lround(dbm * 4.0));  // qdb
                ++filled;
            }
        }
    }
    if (!section_seen) {
        return Result<PowerCurve>::fail("power map: section \"" + want +
                                        "\" not found");
    }
    if (filled != curve.qdb.size()) {
        return Result<PowerCurve>::fail(
            "power map: section \"" + want + "\" covers only " +
            std::to_string(filled) + "/8 HT MCS values (rate indices 12-19)");
    }
    curve.valid = true;
    return Result<PowerCurve>::ok(std::move(curve));
}

Result<PowerCurve> load_power_curve(const std::string& path, bool band_5g) {
    std::ifstream f(path);
    if (!f) {
        return Result<PowerCurve>::fail("power map: cannot open '" + path +
                                        "'");
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_power_curve(ss.str(), band_5g);
}

}  // namespace wblink
