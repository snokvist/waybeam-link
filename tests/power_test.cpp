// SPDX-License-Identifier: GPL-2.0-or-later
// §10.2 power-map loader: the PHY_REG_PG.txt row-format subset — section
// selection by band, sequential rate-index assignment (HT MCS0–7 = indices
// 12–19), dBm→qdb scaling, and specific rejections for malformed input.
// resolve_power_qdb() itself is covered in selector_test.
#include "wblink/power_file.h"

#include <string>

#include "wbtest.h"

using namespace wblink;

namespace {

// A minimal well-formed map: CCK+OFDM rows (indices 0–11) then two HT rows
// (12–19). Values chosen so curve[mcs] is recognizable: 20.0 − mcs×0.5 dBm.
const char* kGood = R"(#[v2][Exact]#
#[2.4G]A
[1]  0xc20  0xffffffff  18.0  18.0  18.0  18.0
[1]  0xc24  0xffffffff  17.0  17.0  17.0  17.0
[1]  0xc28  0xffffffff  16.0  16.0  16.0  16.0
[1]  0xc2c  0xffffffff  20.0  19.5  19.0  18.5
[1]  0xc30  0xffffffff  18.0  17.5  17.0  16.5
0xffff
#[5G]A
[1]  0xc20  0xffffffff  10.0  10.0  10.0  10.0
[1]  0xc24  0xffffffff  10.0  10.0  10.0  10.0
[1]  0xc28  0xffffffff  10.0  10.0  10.0  10.0
[1]  0xc2c  0xffffffff  14.0  14.0  14.0  14.0
[1]  0xc30  0xffffffff  13.0  13.0  13.0  13.0
0xffff
)";

}  // namespace

int main() {
    // --- happy path: 2.4G section, dBm×4 = qdb -------------------------------
    {
        auto r = parse_power_curve(kGood, /*band_5g=*/false);
        CHECK(bool(r));
        if (r) {
            const PowerCurve& c = *r.value;
            CHECK(c.valid);
            CHECK_EQ_U(static_cast<uint32_t>(c.qdb[0]), 80);  // 20.0 dBm
            CHECK_EQ_U(static_cast<uint32_t>(c.qdb[1]), 78);  // 19.5
            CHECK_EQ_U(static_cast<uint32_t>(c.qdb[3]), 74);  // 18.5
            CHECK_EQ_U(static_cast<uint32_t>(c.qdb[4]), 72);  // 18.0
            CHECK_EQ_U(static_cast<uint32_t>(c.qdb[7]), 66);  // 16.5
        }
    }
    // --- band selection: 5G section is independent ---------------------------
    {
        auto r = parse_power_curve(kGood, /*band_5g=*/true);
        CHECK(bool(r));
        if (r) {
            CHECK_EQ_U(static_cast<uint32_t>(r.value->qdb[0]), 56);  // 14.0
            CHECK_EQ_U(static_cast<uint32_t>(r.value->qdb[4]), 52);  // 13.0
        }
    }
    // --- rejections -----------------------------------------------------------
    {
        // Missing band section.
        auto r = parse_power_curve("#[2.4G]A\n[1] 0xc20 0xffffffff 1 1 1 1\n",
                                   true);
        CHECK(!r);
        CHECK(r.error.find("#[5G]A") != std::string::npos);
        // Section present but doesn't reach the HT indices.
        auto short_r = parse_power_curve(
            "#[2.4G]A\n[1] 0xc20 0xffffffff 18 18 18 18\n0xffff\n", false);
        CHECK(!short_r);
        CHECK(short_r.error.find("covers only") != std::string::npos);
        // Garbage row start.
        auto bad = parse_power_curve("#[2.4G]A\nnonsense row here\n", false);
        CHECK(!bad);
        // Row with fewer than 4 values.
        auto trunc = parse_power_curve(
            "#[2.4G]A\n[1] 0xc20 0xffffffff 18 18\n", false);
        CHECK(!trunc);
        // Value out of the plausible dBm range.
        auto hot = parse_power_curve(
            "#[2.4G]A\n[1] 0xc20 0xffffffff 90 18 18 18\n", false);
        CHECK(!hot);
        // Empty input.
        CHECK(!parse_power_curve("", false));
    }

    return wbtest_finish("power_test");
}
