// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: GF(256) arithmetic for the frame-aligned RLC codec
// (PROTOCOL.md §14.1). Primitive polynomial 0x11D, generator g=2; log/exp
// tables (512 B total) built once, thread-safe. Pure — no floats, no I/O — and
// 32-bit-clean for the Android-vendored core.
#pragma once

#include <cstdint>

namespace wblink {

// §14.1 — GF(256) addition is XOR (the field has characteristic 2).
inline uint8_t gf_add(uint8_t a, uint8_t b) {
    return static_cast<uint8_t>(a ^ b);
}

// §14.1 — GF(256) multiply: 0 if either operand is 0, else exp[log[a]+log[b]].
uint8_t gf_mul(uint8_t a, uint8_t b);

// §14.1 — GF(256) multiplicative inverse. Precondition: a != 0.
uint8_t gf_inv(uint8_t a);

}  // namespace wblink
