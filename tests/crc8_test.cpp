// SPDX-License-Identifier: GPL-2.0-or-later
// CRC-8/DVB-S2 known-answer tests. The check value for "123456789" is the
// published CRC-8/DVB-S2 KAT (0xBC); the rest pin table construction and
// incremental use.
#include "wblink/crc8.h"

#include <cstring>

#include "wbtest.h"

using namespace wblink;

int main() {
    // Published KAT: CRC-8/DVB-S2("123456789") = 0xBC.
    const char* kat = "123456789";
    CHECK_EQ_U(crc8_dvbs2(reinterpret_cast<const uint8_t*>(kat), 9), 0xBC);

    // Empty input = init value.
    CHECK_EQ_U(crc8_dvbs2(nullptr, 0), 0x00);

    // Single zero byte: CRC of 0x00 through the table is table[0] == 0.
    const uint8_t zero = 0x00;
    CHECK_EQ_U(crc8_dvbs2(&zero, 1), 0x00);

    // Incremental == one-shot.
    const uint8_t buf[] = {0x57, 0x42, 0x01, 0xFF, 0x00, 0xD5};
    uint8_t inc = crc8_dvbs2(buf, 3);
    inc = crc8_dvbs2(buf + 3, 3, inc);
    CHECK_EQ_U(inc, crc8_dvbs2(buf, 6));

    // constexpr usability (compile-time evaluation).
    constexpr uint8_t kOne[] = {0x01};
    static_assert(crc8_dvbs2(kOne, 1) == 0xD5, "table[1] must be the poly");

    return wbtest_finish("crc8_test");
}
