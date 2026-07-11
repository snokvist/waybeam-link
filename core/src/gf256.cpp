// SPDX-License-Identifier: GPL-2.0-or-later
// GF(256) log/exp tables and arithmetic (PROTOCOL.md §14.1).
#include "wblink/gf256.h"

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

uint8_t gf_inv(uint8_t a) {
    // Precondition a != 0, so log[a] is in 0..254 and 255-log[a] is in 1..255 —
    // a valid exp[] index. a^-1 = g^(255 - log a).
    const GfTables& t = tables();
    return t.exp[255 - static_cast<int>(t.log[a])];
}

}  // namespace wblink
