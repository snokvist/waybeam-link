// SPDX-License-Identifier: GPL-2.0-or-later
// GF(256) log/exp tables and arithmetic (PROTOCOL.md §14.1).
#include "wblink/gf256.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace wblink {
namespace {

// GF(256) log/exp tables with primitive polynomial 0x11D
// (x^8 + x^4 + x^3 + x^2 + 1) and generator g=2. exp[] is doubled to 512 B so
// exp[log[a]+log[b]] — index up to 254+254=508 — needs no modulo. Built once at
// first use via a function-local static: thread-safe under C++11 and free of
// the static-initialization-order hazard a global table would carry.
struct GfTables {
    uint8_t log[256];
    uint8_t exp[512];

    GfTables() {
        uint16_t x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[static_cast<uint8_t>(x)] = static_cast<uint8_t>(i);
            x = static_cast<uint16_t>(x << 1);
            if (x & 0x100) {
                x = static_cast<uint16_t>(x ^ 0x11D);
            }
        }
        // exp has period 255; duplicate the first 255 entries so any log-sum up
        // to 509 indexes a valid, in-range element without a modulo.
        for (int i = 255; i < 512; ++i) {
            exp[i] = exp[i - 255];
        }
        // log[0] is mathematically undefined; pin it so the table is total and
        // never reads uninitialized memory (gf_mul short-circuits a==0/b==0).
        log[0] = 0;
    }
};

const GfTables& tables() {
    static const GfTables t;
    return t;
}

}  // namespace

uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    const GfTables& t = tables();
    return t.exp[static_cast<int>(t.log[a]) + static_cast<int>(t.log[b])];
}

void gf_mul_xor(uint8_t coefficient, const uint8_t* input, uint8_t* out,
                size_t len) {
    if (coefficient == 0) {
        return;
    }
    if (coefficient == 1) {
        for (size_t i = 0; i < len; ++i) {
            out[i] = static_cast<uint8_t>(out[i] ^ input[i]);
        }
        return;
    }
    size_t i = 0;
#if defined(__ARM_NEON)
    // Carry-less multiply by a scalar coefficient, 16 field elements at a
    // time. Reduction uses x^8 = x^4+x^3+x^2+1 (0x1d for polynomial 0x11d).
    const uint8x16_t high_bit = vdupq_n_u8(0x80);
    const uint8x16_t reduction = vdupq_n_u8(0x1d);
    for (; i + 16 <= len; i += 16) {
        uint8x16_t a = vld1q_u8(input + i);
        uint8x16_t product = vdupq_n_u8(0);
        uint8_t factor = coefficient;
        while (factor != 0) {
            if ((factor & 1u) != 0) {
                product = veorq_u8(product, a);
            }
            const uint8x16_t high = vceqq_u8(vandq_u8(a, high_bit), high_bit);
            a = veorq_u8(vshlq_n_u8(a, 1), vandq_u8(high, reduction));
            factor = static_cast<uint8_t>(factor >> 1);
        }
        vst1q_u8(out + i, veorq_u8(vld1q_u8(out + i), product));
    }
#endif
    const GfTables& t = tables();
    const int coefficient_log = static_cast<int>(t.log[coefficient]);
    for (; i < len; ++i) {
        const uint8_t v = input[i];
        if (v != 0) {
            out[i] = static_cast<uint8_t>(
                out[i] ^ t.exp[coefficient_log + static_cast<int>(t.log[v])]);
        }
    }
}

uint8_t gf_inv(uint8_t a) {
    // Precondition a != 0, so log[a] is in 0..254 and 255-log[a] is in 1..255 —
    // a valid exp[] index. a^-1 = g^(255 - log a).
    const GfTables& t = tables();
    return t.exp[255 - static_cast<int>(t.log[a])];
}

}  // namespace wblink
