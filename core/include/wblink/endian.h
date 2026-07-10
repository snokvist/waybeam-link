// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: big-endian (network order) field access (PROTOCOL.md §0).
// Byte-oriented on purpose — free of host-endianness and alignment assumptions,
// identical codegen question on x86-64 and ARMv7.
#pragma once

#include <cstdint>

namespace wblink {

constexpr void be16_write(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

constexpr void be32_write(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

constexpr uint16_t be16_read(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                 static_cast<uint16_t>(p[1]));
}

constexpr uint32_t be32_read(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

}  // namespace wblink
