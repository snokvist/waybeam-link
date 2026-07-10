// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: CRC-8/DVB-S2 — poly 0xD5, init 0x00, no reflection, no
// final XOR. The table_version content hash (PROTOCOL.md §3.6) and the same
// CRC-8 the ecosystem's CRSF stack uses.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace wblink {

namespace detail {
constexpr std::array<uint8_t, 256> make_crc8_dvbs2_table() {
    std::array<uint8_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t crc = static_cast<uint8_t>(i);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0xD5)
                               : static_cast<uint8_t>(crc << 1);
        }
        table[i] = crc;
    }
    return table;
}
inline constexpr std::array<uint8_t, 256> kCrc8Dvbs2Table =
    make_crc8_dvbs2_table();
}  // namespace detail

constexpr uint8_t crc8_dvbs2(const uint8_t* data, size_t len,
                             uint8_t crc = 0x00) {
    for (size_t i = 0; i < len; ++i) {
        crc = detail::kCrc8Dvbs2Table[static_cast<uint8_t>(crc ^ data[i])];
    }
    return crc;
}

}  // namespace wblink
