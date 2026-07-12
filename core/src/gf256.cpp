// SPDX-License-Identifier: GPL-2.0-or-later
// GF(256) log/exp tables and arithmetic (PROTOCOL.md §14.1).
#include "wblink/gf256.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace wblink {
namespace {

// GF(256) log/exp tables with primitive polynomial 0x11D
// (x^8 + x^4 + x^3 + x^2 + 1) and generator g=2. exp[] is doubled so a log-sum
// needs no modulo. The nibble product tables let ARMv7 NEON multiply 16 field
// elements using two 16-entry vtbl lookups. Built once at first use.
struct GfTables {
    uint8_t log[256];
    uint8_t exp[512];
    uint8_t mul_lo[256][16];
    uint8_t mul_hi[256][16];

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
        for (int coefficient = 0; coefficient < 256; ++coefficient) {
            for (int nibble = 0; nibble < 16; ++nibble) {
                const auto product = [&](int value) -> uint8_t {
                    if (coefficient == 0 || value == 0) {
                        return 0;
                    }
                    return exp[static_cast<int>(log[coefficient]) +
                               static_cast<int>(log[value])];
                };
                mul_lo[coefficient][nibble] = product(nibble);
                mul_hi[coefficient][nibble] = product(nibble << 4);
            }
        }
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
    const GfTables& t = tables();
#if defined(__ARM_NEON)
    uint8x8x2_t low_table;
    low_table.val[0] = vld1_u8(&t.mul_lo[coefficient][0]);
    low_table.val[1] = vld1_u8(&t.mul_lo[coefficient][8]);
    uint8x8x2_t high_table;
    high_table.val[0] = vld1_u8(&t.mul_hi[coefficient][0]);
    high_table.val[1] = vld1_u8(&t.mul_hi[coefficient][8]);
    const uint8x8_t low_mask = vdup_n_u8(0x0f);
    for (; i + 16 <= len; i += 16) {
        const uint8x16_t input_vec = vld1q_u8(input + i);
        const uint8x8_t input_low = vget_low_u8(input_vec);
        const uint8x8_t input_high = vget_high_u8(input_vec);
        const auto multiply = [&](uint8x8_t values) {
            return veor_u8(vtbl2_u8(low_table, vand_u8(values, low_mask)),
                           vtbl2_u8(high_table, vshr_n_u8(values, 4)));
        };
        const uint8x16_t product =
            vcombine_u8(multiply(input_low), multiply(input_high));
        vst1q_u8(out + i, veorq_u8(vld1q_u8(out + i), product));
    }
#endif
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
