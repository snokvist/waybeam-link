// SPDX-License-Identifier: GPL-2.0-or-later
// Systematic Cauchy Reed–Solomon codec test (PROTOCOL.md §14.1): the
// systematic property (source-only decode is byte-exact), exhaustive
// erasure-pattern recovery for a small block, burst erasure, random large
// blocks with bounded loss, k=1, the k+r=256 capacity boundary, source/repair
// mixes, and rlc_repair_row determinism. Deterministic seeded PRNG throughout.
#include <cstdint>
#include <cstring>
#include <vector>

#include "wblink/rlc.h"
#include "wbtest.h"

using namespace wblink;

namespace {

// xorshift64* — deterministic across platforms, no <random> variability.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x1234567811223344ull) {}
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint8_t u8() { return static_cast<uint8_t>(next()); }
    uint32_t range(uint32_t lo, uint32_t hi) {
        return lo + static_cast<uint32_t>(next() % (hi - lo + 1));
    }
};

// A fully-formed block: k random source symbols and r encoded repair symbols,
// each s bytes.
struct Block {
    uint16_t k;
    uint8_t r;
    size_t s;
    std::vector<std::vector<uint8_t>> sources;  // k symbols
    std::vector<std::vector<uint8_t>> repairs;  // r symbols
};

Block make_block(uint16_t k, uint8_t r, size_t s, Rng& rng) {
    Block blk;
    blk.k = k;
    blk.r = r;
    blk.s = s;
    blk.sources.resize(k);
    for (uint16_t i = 0; i < k; ++i) {
        blk.sources[i].resize(s);
        for (size_t b = 0; b < s; ++b) {
            blk.sources[i][b] = rng.u8();
        }
    }
    std::vector<const uint8_t*> ptrs(k);
    for (uint16_t i = 0; i < k; ++i) {
        ptrs[i] = blk.sources[i].data();
    }
    blk.repairs.resize(r);
    for (uint8_t j = 0; j < r; ++j) {
        blk.repairs[j].resize(s);
        rlc_encode_repair(k, j, ptrs.data(), s, blk.repairs[j].data());
    }
    return blk;
}

// Feed the symbols selected by `received` (symbol id < k => source id, else
// repair id-k) to a decoder, decode, and verify every source is recovered
// byte-exact. `received` must hold at least k ids.
bool recover_ok(const Block& blk, const std::vector<uint32_t>& received) {
    RlcDecoder dec(blk.k, blk.s);
    for (uint32_t id : received) {
        if (id < blk.k) {
            dec.add_source(static_cast<uint16_t>(id), blk.sources[id].data());
        } else {
            const uint32_t rj = id - blk.k;
            dec.add_repair(static_cast<uint8_t>(rj), blk.repairs[rj].data());
        }
    }
    if (!dec.can_decode()) {
        return false;
    }
    std::vector<uint8_t> out(static_cast<size_t>(blk.k) * blk.s);
    if (!dec.decode(out.data())) {
        return false;
    }
    for (uint16_t i = 0; i < blk.k; ++i) {
        if (std::memcmp(&out[static_cast<size_t>(i) * blk.s],
                        blk.sources[i].data(), blk.s) != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    // --- Systematic property: all k sources → exact recovery, no repairs used.
    {
        Rng rng(0xA1);
        Block blk = make_block(8, 0, 37, rng);
        std::vector<uint32_t> all;
        for (uint32_t i = 0; i < 8; ++i) {
            all.push_back(i);
        }
        CHECK(recover_ok(blk, all));
        // Idempotent: a second decode on the same decoder must still be exact.
        RlcDecoder dec(blk.k, blk.s);
        for (uint16_t i = 0; i < 8; ++i) {
            dec.add_source(i, blk.sources[i].data());
        }
        std::vector<uint8_t> o1(8u * blk.s), o2(8u * blk.s);
        CHECK(dec.decode(o1.data()));
        CHECK(dec.decode(o2.data()));
        CHECK(std::memcmp(o1.data(), o2.data(), o1.size()) == 0);
    }

    // --- Exhaustive erasure patterns: k=4, r=4 (8 transmitted symbols). Every
    // subset of size >= k must recover (covers single, double, and heavier
    // erasure, and every source/repair mix).
    {
        Rng rng(0xBEEF);
        Block blk = make_block(4, 4, 29, rng);
        const uint32_t total = blk.k + blk.r;  // 8
        for (uint32_t mask = 0; mask < (1u << total); ++mask) {
            std::vector<uint32_t> recv;
            for (uint32_t id = 0; id < total; ++id) {
                if (mask & (1u << id)) {
                    recv.push_back(id);
                }
            }
            if (recv.size() >= blk.k) {
                CHECK(recover_ok(blk, recv));
            }
        }
    }

    // --- Burst erasure: contiguous run of sources lost, repairs fill the gap.
    {
        Rng rng(0xC0FFEE);
        const uint16_t k = 24;
        const uint8_t r = 16;
        Block blk = make_block(k, r, 64, rng);
        // Lose sources [5, 5+burst) with burst == r; survivors = other sources
        // + all repairs. Exactly (k - r) + r = k symbols survive at burst==r.
        for (uint8_t burst = 1; burst <= r; ++burst) {
            std::vector<uint32_t> recv;
            for (uint32_t i = 0; i < k; ++i) {
                if (i < 5 || i >= 5u + burst) {
                    recv.push_back(i);
                }
            }
            for (uint32_t j = 0; j < r; ++j) {
                recv.push_back(k + j);
            }
            CHECK(recover_ok(blk, recv));
        }
    }

    // --- Random large block: k=132, r=40. Many trials, each a distinct random
    // loss of up to r symbols (so >= k survive), pattern varied by trial index.
    {
        for (uint32_t trial = 0; trial < 300; ++trial) {
            Rng rng(0x9000ull + trial * 0x1000193ull);
            const uint16_t k = 132;
            const uint8_t r = 40;
            const size_t s = 48;
            Block blk = make_block(k, r, s, rng);
            const uint32_t total = k + r;  // 172
            const uint32_t lose = rng.range(0, r);  // 0..40, survivors >= k
            std::vector<uint8_t> lost(total, 0);
            for (uint32_t n = 0; n < lose;) {
                const uint32_t id = rng.range(0, total - 1);
                if (!lost[id]) {
                    lost[id] = 1;
                    ++n;
                }
            }
            std::vector<uint32_t> recv;
            for (uint32_t id = 0; id < total; ++id) {
                if (!lost[id]) {
                    recv.push_back(id);
                }
            }
            CHECK(recover_ok(blk, recv));
        }
    }

    // --- k=1: recover from the lone source, and from a repair alone.
    {
        Rng rng(0x1);
        Block blk = make_block(1, 3, 41, rng);
        CHECK(recover_ok(blk, {0}));               // source only
        CHECK(recover_ok(blk, {1}));               // repair 0 only
        CHECK(recover_ok(blk, {3}));               // repair 2 only
    }

    // --- Capacity boundary k + r = 256 (k=200, r=56). A few random loss trials.
    {
        for (uint32_t trial = 0; trial < 8; ++trial) {
            Rng rng(0x5600ull + trial);
            const uint16_t k = 200;
            const uint8_t r = 56;
            const size_t s = 16;
            Block blk = make_block(k, r, s, rng);
            const uint32_t total = k + r;  // 256
            const uint32_t lose = rng.range(1, r);
            std::vector<uint8_t> lost(total, 0);
            for (uint32_t n = 0; n < lose;) {
                const uint32_t id = rng.range(0, total - 1);
                if (!lost[id]) {
                    lost[id] = 1;
                    ++n;
                }
            }
            std::vector<uint32_t> recv;
            for (uint32_t id = 0; id < total; ++id) {
                if (!lost[id]) {
                    recv.push_back(id);
                }
            }
            CHECK(recover_ok(blk, recv));
        }
    }

    // --- Explicit source/repair mix: half the sources dropped, made up by an
    // equal count of repairs.
    {
        Rng rng(0x51ED);  // deterministic seed for the source/repair mix
        const uint16_t k = 12;
        const uint8_t r = 8;
        Block blk = make_block(k, r, 55, rng);
        std::vector<uint32_t> recv;
        for (uint32_t i = 0; i < k; i += 2) {  // sources 0,2,4,...,10 (6 of 12)
            recv.push_back(i);
        }
        for (uint32_t j = 0; j < 6; ++j) {  // 6 repairs to reach k=12
            recv.push_back(k + j);
        }
        CHECK_EQ_U(recv.size(), k);
        CHECK(recover_ok(blk, recv));
    }

    // --- Determinism: rlc_repair_row yields identical bytes for the same
    // (k, repair_idx), independent of call order.
    {
        const uint16_t ks[] = {1, 4, 132, 200};
        for (uint16_t k : ks) {
            for (uint8_t idx = 0; idx < 5; ++idx) {
                std::vector<uint8_t> a(k), b(k);
                rlc_repair_row(k, idx, a.data());
                rlc_repair_row(k, idx, b.data());
                CHECK(std::memcmp(a.data(), b.data(), k) == 0);
                // First coefficient is 1/(k+idx) — nonzero and stable.
                CHECK(a[0] != 0);
            }
        }
    }

    return wbtest_finish("rlc_test");
}
