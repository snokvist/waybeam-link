// SPDX-License-Identifier: GPL-2.0-or-later
// GF(256) field-axiom property test (PROTOCOL.md §14.1): multiply
// commutativity / associativity / distributivity over a deterministic PRNG,
// the multiplicative identity and absorbing zero, and inverses for every
// nonzero element.
#include <cstdint>

#include "wblink/gf256.h"
#include "wbtest.h"

using namespace wblink;

namespace {

// xorshift64* — deterministic across platforms, no <random> variability.
struct Rng {
    uint64_t s = 0x243F6A8885A308D3ull;
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint8_t u8() { return static_cast<uint8_t>(next()); }
};

}  // namespace

int main() {
    // Identity, absorbing zero, and inverse over the whole field.
    for (int v = 0; v < 256; ++v) {
        const uint8_t a = static_cast<uint8_t>(v);
        CHECK_EQ_U(gf_mul(a, 1), a);
        CHECK_EQ_U(gf_mul(1, a), a);
        CHECK_EQ_U(gf_mul(a, 0), 0);
        CHECK_EQ_U(gf_mul(0, a), 0);
        CHECK_EQ_U(gf_add(a, a), 0);      // characteristic 2
        CHECK_EQ_U(gf_add(a, 0), a);
        if (a != 0) {
            const uint8_t inv = gf_inv(a);
            CHECK_EQ_U(gf_mul(a, inv), 1);
            CHECK_EQ_U(gf_mul(inv, a), 1);
            CHECK_EQ_U(gf_inv(inv), a);    // involution
        }
    }

    // Exhaustive commutativity plus a nonzero-product closure check: a*b == 0
    // iff a==0 or b==0 (no zero divisors in a field).
    for (int ai = 0; ai < 256; ++ai) {
        for (int bi = 0; bi < 256; ++bi) {
            const uint8_t a = static_cast<uint8_t>(ai);
            const uint8_t b = static_cast<uint8_t>(bi);
            CHECK_EQ_U(gf_mul(a, b), gf_mul(b, a));
            const bool zero = (a == 0 || b == 0);
            CHECK(zero == (gf_mul(a, b) == 0));
        }
    }

    // Associativity and distributivity over random triples.
    Rng rng;
    for (int i = 0; i < 200000; ++i) {
        const uint8_t a = rng.u8();
        const uint8_t b = rng.u8();
        const uint8_t c = rng.u8();
        // (a*b)*c == a*(b*c)
        CHECK_EQ_U(gf_mul(gf_mul(a, b), c), gf_mul(a, gf_mul(b, c)));
        // a*(b+c) == a*b + a*c
        CHECK_EQ_U(gf_mul(a, gf_add(b, c)),
                   gf_add(gf_mul(a, b), gf_mul(a, c)));
    }

    return wbtest_finish("gf256_test");
}
