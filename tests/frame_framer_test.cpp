// SPDX-License-Identifier: GPL-2.0-or-later
// FrameFramer (§5.1a) + §14.1 FEC: fragmentation, block/seq/EOB stamping,
// repair emission + subheader, the all-k repair rule, the oversize-k gate, and
// an end-to-end RLC decode round-trip proving the emitted symbols are
// recoverable. Pass 205 removed the ARQ class; every referenced frame now gets
// r = max(ceil(k·rate), min_r) and none is stamped ARQ.
#include "wblink/frame_framer.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <vector>

#include "wblink/endian.h"
#include "wblink/frame_shm_format.h"
#include "wblink/rlc.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

struct Sym {
    DataHeader hdr;
    std::vector<uint8_t> payload;
    bool is_repair() const { return (hdr.data_flags & data_flags::kFecRepair) != 0; }
    bool eob() const { return (hdr.data_flags & data_flags::kEndOfBlock) != 0; }
};

struct Harness {
    FrameFramer framer;
    std::vector<Sym> sent;

    explicit Harness(FrameFecConfig fec = {})
        : framer(make_cfg(fec)) {
        framer.set_operating_point(4, 0x41, 1424);
    }
    static FrameFramerConfig make_cfg(FrameFecConfig fec) {
        FrameFramerConfig c;
        c.originator = 7;
        c.session_id = 99;
        c.stream_id = 0;
        c.stream_type = stream_type::kRtp;
        c.fec = fec;
        return c;
    }
    void feed(const std::vector<uint8_t>& blob) {
        framer.on_frame(blob.data(), blob.size(), 1000, [&](const uint8_t* f, size_t n,
                                                             const DataHeader&, uint64_t) {
            Decoded d = decode(f, n);
            Sym s;
            const DataView& v = std::get<DataView>(d);
            s.hdr = v.hdr;
            s.payload.assign(v.payload, v.payload + v.payload_len);
            sent.push_back(std::move(s));
        });
    }
};

// A frame blob = 8-byte VencFrameMeta prefix + deterministic body of `body` B.
std::vector<uint8_t> make_frame(size_t body, bool idr, uint8_t seed,
                               uint8_t extra_flags = 0) {
    std::vector<uint8_t> b(kVencFrameMetaSize + body, 0);
    b[4] = kFrameCodecH265;
    b[5] = static_cast<uint8_t>((idr ? kFrameFlagIdr : 0) | extra_flags);
    for (size_t i = 0; i < body; ++i) {
        b[kVencFrameMetaSize + i] = static_cast<uint8_t>((i * 31u + seed) & 0xFF);
    }
    return b;
}

size_t repair_count_of(const std::vector<Sym>& sent) {
    size_t count = 0;
    for (const Sym& sym : sent) {
        count += sym.is_repair();
    }
    return count;
}

// §14.1a rlc256 policy at the seed rates, so every enhance case below shares
// one construction and differs only in the knob under test.
FrameFecConfig rlc(std::optional<uint16_t> e = std::nullopt) {
    FrameFecConfig f;
    f.scheme = FecScheme::kRlc256;
    f.i_rate_permille = 250;
    f.p_rate_permille = 100;
    f.e_rate_permille = e;
    return f;
}

size_t source_count(const std::vector<Sym>& sent) {
    size_t count = 0;
    for (const Sym& sym : sent) {
        count += !sym.is_repair();
    }
    return count;
}

}  // namespace

int main() {
    // --- no ARQ bit is ever stamped (Pass 205) -----------------------------
    {
        Harness h({});
        h.feed(make_frame(3000, /*idr=*/false, 0));
        CHECK(!h.sent.empty());
        for (const Sym& sy : h.sent) {
            CHECK((sy.hdr.data_flags & 0x22) == 0);  // ARQ|PFRAME_ARQ reserved
        }
        CHECK_EQ_U(h.framer.stats().idr_frames, 0u);
        h.feed(make_frame(3000, /*idr=*/true, 1));
        CHECK_EQ_U(h.framer.stats().idr_frames, 1u);
    }

    // --- basic fragmentation, no FEC ----------------------------------------
    {
        Harness h;  // scheme none
        const uint16_t s = h.framer.symbol_size();
        CHECK_EQ_U(s, 1424u - 26u - 11u);  // 1387
        auto blob = make_frame(3000, /*idr=*/true, 1);
        const uint16_t k = static_cast<uint16_t>((blob.size() + s - 1) / s);
        h.feed(blob);
        CHECK_EQ_U(h.sent.size(), k);
        // seqs 0..k-1, one block, EOB on last only, no repair.
        std::vector<uint8_t> reasm;
        for (uint16_t i = 0; i < k; ++i) {
            CHECK_EQ_U(h.sent[i].hdr.seq, i);
            CHECK_EQ_U(h.sent[i].hdr.block_id, 0u);
            CHECK(!h.sent[i].is_repair());
            CHECK_EQ_U(h.sent[i].eob(), (i == k - 1));
            // §5.1a 4-byte source subheader: k, index.
            const uint8_t* p = h.sent[i].payload.data();
            CHECK(h.sent[i].payload.size() >= kFecSourceSubheaderSize);
            CHECK_EQ_U(be16_read(p + kFecSrcOffWindowLen), k);
            CHECK_EQ_U(be16_read(p + kFecSrcOffSymIndex), i);
            reasm.insert(reasm.end(),
                         h.sent[i].payload.begin() + kFecSourceSubheaderSize,
                         h.sent[i].payload.end());
        }
        CHECK_EQ_U(reasm.size(), blob.size());
        CHECK(reasm == blob);
        // second frame: block_id advances, seq monotonic.
        h.feed(make_frame(10, false, 2));
        CHECK_EQ_U(h.sent[k].hdr.block_id, 1u);
        CHECK_EQ_U(h.sent[k].hdr.seq, k);
    }

    // --- §9.3a negotiated ceiling intersects the profile ceiling ------------
    {
        Harness h;
        h.framer.set_operating_point(7, 0x80, mtu_tier::kHighBudget);
        // Boot/default remains compatibility-sized despite a High profile.
        CHECK_EQ_U(h.framer.effective_packet_budget(), kDefaultMaxPayload);
        CHECK_EQ_U(h.framer.symbol_size(), 1424u - 26u - 11u);
        h.framer.set_negotiated_packet_budget(mtu_tier::kMediumBudget);
        CHECK_EQ_U(h.framer.effective_packet_budget(), 2048u);
        CHECK_EQ_U(h.framer.symbol_size(), 2048u - 26u - 11u);
        h.feed(make_frame(6000, true, 33));
        for (const Sym& sy : h.sent) {
            CHECK(sy.payload.size() + kDataHeaderSize <= 2048u);
        }
        h.sent.clear();
        h.framer.set_negotiated_packet_budget(mtu_tier::kHighBudget);
        CHECK_EQ_U(h.framer.effective_packet_budget(), 3072u);
        h.feed(make_frame(6000, true, 34));
        for (const Sym& sy : h.sent) {
            CHECK(sy.payload.size() + kDataHeaderSize <= 3072u);
        }
        // A lower profile remains authoritative even after High is accepted.
        h.framer.set_operating_point(2, 0x80, kDefaultMaxPayload);
        CHECK_EQ_U(h.framer.effective_packet_budget(), kDefaultMaxPayload);
    }

    // --- §9.3a Pass 124: 12-case jumbo repair-depth guard matrix -----------
    {
        struct GuardCase {
            uint16_t profile_budget;
            uint16_t negotiated_budget;
            size_t frame_len;
            uint16_t expected_symbol;
            uint16_t expected_k;
            uint16_t expected_r;
            bool guarded;
        };
        static constexpr GuardCase cases[] = {
            {1424, 1424, 30000, 1387, 22, 5, false},
            {3072, 3072, 6000, 3035, 2, 2, false},
            {2048, 2048, 20805, 2011, 11, 3, false},
            {2048, 2048, 20806, 2011, 11, 4, true},
            {2048, 2048, 30165, 2011, 15, 4, true},
            {2048, 2048, 30166, 2011, 16, 4, false},
            {3072, 3072, 30165, 3035, 10, 4, true},
            {3072, 3072, 45525, 3035, 15, 4, true},
            {3072, 3072, 45526, 3035, 16, 4, false},
            {3072, 3072, 100000, 3035, 33, 7, false},
            {2048, 3072, 45525, 2011, 23, 5, false},
            {3072, 2048, 30000, 2011, 15, 4, true},
        };
        for (size_t i = 0; i < std::size(cases); ++i) {
            const GuardCase& tc = cases[i];
            FrameFecConfig fec;
            fec.scheme = FecScheme::kRlc256;
            fec.p_rate_permille = 200;
            fec.min_r = 2;
            Harness h(fec);
            h.framer.set_operating_point(7, 0x80, tc.profile_budget);
            h.framer.set_negotiated_packet_budget(tc.negotiated_budget);
            CHECK_EQ_U(h.framer.symbol_size(), tc.expected_symbol);
            h.feed(make_frame(tc.frame_len - kVencFrameMetaSize,
                              /*idr=*/false,
                              static_cast<uint8_t>(40 + i)));
            CHECK_EQ_U(source_count(h.sent), tc.expected_k);
            CHECK_EQ_U(h.framer.stats().repair_symbols, tc.expected_r);
            CHECK_EQ_U(h.framer.stats().mtu_fec_guard_frames,
                       tc.guarded ? 1u : 0u);
            const size_t packet_budget =
                tc.expected_symbol + kDataHeaderSize +
                kFecRepairSubheaderSize;
            for (const Sym& sym : h.sent) {
                CHECK(sym.payload.size() + kDataHeaderSize <= packet_budget);
                CHECK_EQ_U(be16_read(sym.payload.data() +
                                     (sym.is_repair()
                                          ? kFecOffWindowLen
                                          : kFecSrcOffWindowLen)),
                           tc.expected_k);
            }
        }
    }

    // A FEC-disabled stream keeps the full negotiated ceiling: reducing k has
    // no recovery benefit there and would only spend packet-rate/CPU budget.
    {
        Harness h;
        h.framer.set_operating_point(7, 0x80, mtu_tier::kHighBudget);
        h.framer.set_negotiated_packet_budget(mtu_tier::kHighBudget);
        CHECK_EQ_U(h.framer.symbol_size(), 3035u);
        h.feed(make_frame(30000 - kVencFrameMetaSize, false, 59));
        CHECK_EQ_U(source_count(h.sent), 10u);
        CHECK_EQ_U(h.framer.stats().repair_symbols, 0u);
        CHECK_EQ_U(h.framer.stats().mtu_fec_guard_frames, 0u);
    }

    // At the measured high-rate size, keep k=10 but enforce the k=16-equivalent
    // four-repair depth, even after a lower §14.2 override. Four arbitrary
    // source erasures must remain deterministically recoverable.
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.p_rate_permille = 200;
        Harness h(fec);
        h.framer.set_operating_point(7, 0x80, mtu_tier::kHighBudget);
        h.framer.set_negotiated_packet_budget(mtu_tier::kHighBudget);
        h.framer.set_next_frame_override(1);
        const auto blob = make_frame(30000 - kVencFrameMetaSize, false, 60);
        h.feed(blob);
        CHECK_EQ_U(h.framer.stats().source_symbols, 10u);
        CHECK_EQ_U(h.framer.stats().repair_symbols, 4u);
        CHECK_EQ_U(h.framer.stats().mtu_fec_guard_frames, 1u);
        CHECK_EQ_U(h.sent.size(), 14u);
        for (const Sym& sym : h.sent) {
            CHECK_EQ_U(be16_read(sym.payload.data() +
                                 (sym.is_repair() ? kFecOffWindowLen
                                                  : kFecSrcOffWindowLen)),
                       10u);
        }

        const uint16_t symbol = h.framer.symbol_size();
        RlcDecoder dec(10, symbol);
        std::vector<uint8_t> padded(symbol, 0);
        uint16_t source_index = 0;
        for (const Sym& sym : h.sent) {
            if (sym.is_repair()) {
                dec.add_repair(
                    sym.payload[kFecOffRepairIdx],
                    sym.payload.data() + kFecRepairSubheaderSize);
                continue;
            }
            if (source_index != 0 && source_index != 3 &&
                source_index != 6 && source_index != 9) {
                std::memset(padded.data(), 0, padded.size());
                const size_t chunk =
                    sym.payload.size() - kFecSourceSubheaderSize;
                std::memcpy(padded.data(),
                            sym.payload.data() + kFecSourceSubheaderSize,
                            chunk);
                dec.add_source(source_index, padded.data());
            }
            ++source_index;
        }
        CHECK(dec.can_decode());
        std::vector<uint8_t> recovered(10u * symbol, 0);
        CHECK(dec.decode(recovered.data()));
        recovered.resize(blob.size());
        CHECK(recovered == blob);

        h.framer.reset_stats();
        CHECK_EQ_U(h.framer.stats().mtu_fec_guard_frames, 0u);
    }

    // An impossible min_r remains the existing §14.1 source-only fallback;
    // Pass 124 must not recreate an invalid k+r > 256 block after rejection.
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.p_rate_permille = 200;
        fec.min_r = 255;
        Harness h(fec);
        h.framer.set_operating_point(7, 0x80, mtu_tier::kHighBudget);
        h.framer.set_negotiated_packet_budget(mtu_tier::kHighBudget);
        h.feed(make_frame(30000 - kVencFrameMetaSize, false, 62));
        CHECK_EQ_U(source_count(h.sent), 10u);
        CHECK_EQ_U(h.framer.stats().repair_symbols, 0u);
        CHECK_EQ_U(h.framer.stats().fec_oversize_k, 1u);
        CHECK_EQ_U(h.framer.stats().mtu_fec_guard_frames, 0u);
        CHECK_EQ_U(h.sent.size(), 10u);
    }

    // --- Pass 205/O1: k <= min_k no longer ships bare ----------------------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;  // i_rate 250, min_r 2
        Harness h(fec);
        h.feed(make_frame(1000, true, 3));  // k = 1
        CHECK_EQ_U(h.framer.stats().repair_symbols, 2u);  // floored to min_r
    }

    // --- FEC on IDR (i_rate) + end-to-end decode round-trip -----------------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.i_rate_permille = 250;
        fec.p_rate_permille = 100;
        Harness h(fec);
        const uint16_t s = h.framer.symbol_size();
        auto blob = make_frame(10000, /*idr=*/true, 5);
        const uint16_t k = static_cast<uint16_t>((blob.size() + s - 1) / s);
        const uint16_t r = static_cast<uint16_t>((static_cast<uint32_t>(k) * 250 + 999) / 1000);
        h.feed(blob);
        CHECK_EQ_U(h.sent.size(), static_cast<size_t>(k) + r);
        CHECK_EQ_U(h.framer.stats().source_symbols, k);
        CHECK_EQ_U(h.framer.stats().repair_symbols, r);

        // Split sources vs repairs; validate repair subheaders.
        std::vector<Sym*> src, rep;
        for (Sym& sy : h.sent) (sy.is_repair() ? rep : src).push_back(&sy);
        CHECK_EQ_U(src.size(), k);
        CHECK_EQ_U(rep.size(), r);
        size_t eob_count = 0;
        for (const Sym* sy : src) {
            eob_count += sy->eob();
        }
        for (uint16_t j = 0; j < r; ++j) {
            const uint8_t* sub = rep[j]->payload.data();
            CHECK_EQ_U(sub[kFecOffRepairIdx], j);
            CHECK_EQ_U(be16_read(sub + kFecOffWindowLen), k);
            CHECK_EQ_U(be32_read(sub + kFecOffWindowBaseSeq), 0u);
            CHECK_EQ_U(be32_read(sub + kFecOffFrameLen), blob.size());
            CHECK_EQ_U(rep[j]->payload.size(), kFecRepairSubheaderSize + s);
            CHECK_EQ_U(rep[j]->eob(), j == r - 1);
            eob_count += rep[j]->eob();
        }
        // Quiet-gap close follows the repair tail, not the last source.
        CHECK_EQ_U(eob_count, 1);

        // Drop 2 source symbols; recover from surviving sources + 2 repairs.
        RlcDecoder dec(k, s);
        std::vector<uint8_t> pad(s, 0);
        for (uint16_t i = 0; i < k; ++i) {
            if (i == 1 || i == k - 1) continue;  // erase symbol 1 and the last
            std::memset(pad.data(), 0, s);
            // chunk = source payload minus the 4-byte subheader; pad to s.
            const uint8_t* chunk = src[i]->payload.data() + kFecSourceSubheaderSize;
            const size_t clen = src[i]->payload.size() - kFecSourceSubheaderSize;
            std::memcpy(pad.data(), chunk, clen);
            dec.add_source(i, pad.data());
        }
        for (uint16_t j = 0; j < r && j < 2; ++j) {
            dec.add_repair(static_cast<uint8_t>(j),
                           rep[j]->payload.data() + kFecRepairSubheaderSize);
        }
        CHECK(dec.can_decode());
        std::vector<uint8_t> out(static_cast<size_t>(k) * s, 0);
        CHECK(dec.decode(out.data()));
        // Trim to frame_len and compare to the original blob.
        out.resize(blob.size());
        CHECK(out == blob);
    }

    // --- P-frame uses p_rate ------------------------------------------------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.p_rate_permille = 100;
        fec.min_r = 0;  // isolate the rate formula from the Pass 98 floor
        Harness h(fec);
        const uint16_t s = h.framer.symbol_size();
        auto blob = make_frame(10000, /*idr=*/false, 6);
        const uint16_t k = static_cast<uint16_t>((blob.size() + s - 1) / s);
        const uint16_t r = static_cast<uint16_t>((static_cast<uint32_t>(k) * 100 + 999) / 1000);
        h.feed(blob);
        CHECK_EQ_U(h.framer.stats().repair_symbols, r);
    }

    // --- oversize-k cap: k + r > 256 => FEC off + stat ----------------------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.i_rate_permille = 250;
        FrameFramerConfig c;
        c.stream_type = stream_type::kRtp;
        c.fec = fec;
        FrameFramer ff(c);
        ff.set_operating_point(0, 0, 58);  // s = 58 - 37 = 21
        const uint16_t s = ff.symbol_size();
        CHECK_EQ_U(s, 21u);
        auto blob = make_frame(static_cast<size_t>(21) * 260, true, 7);  // k = 260
        size_t emitted = 0;
        ff.on_frame(blob.data(), blob.size(), 1, [&](const uint8_t*, size_t,
                                                     const DataHeader&, uint64_t) { ++emitted; });
        CHECK_EQ_U(ff.stats().fec_oversize_k, 1u);
        CHECK_EQ_U(ff.stats().repair_symbols, 0u);
        CHECK_EQ_U(emitted, ff.stats().source_symbols);  // sources only
    }

    // --- malformed (< 8 B): dropped -----------------------------------------
    {
        Harness h;
        std::vector<uint8_t> tiny(4, 0);
        h.feed(tiny);
        CHECK_EQ_U(h.sent.size(), 0u);
        CHECK_EQ_U(h.framer.stats().malformed_frame, 1u);
    }

    // --- §14.2 enforcement override (Pass 38): one-shot, clamped -----------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.i_rate_permille = 250;
        fec.p_rate_permille = 100;
        fec.min_r = 0;  // isolate override / fixed-rate r from the Pass 98 floor
        Harness h(fec);
        const uint16_t s = h.framer.symbol_size();
        auto blob = make_frame(4 * s, /*idr=*/false, 9);  // k = 5
        // Override: zero parity for this frame only.
        h.framer.set_next_frame_override(0);
        h.feed(blob);
        size_t repairs = 0;
        for (const Sym& sy : h.sent) repairs += sy.is_repair() ? 1 : 0;
        CHECK_EQ_U(repairs, 0u);
        // The override was consumed: the next frame is back on §14.1 fixed
        // rates (ceil(5*0.1) = 1 repair).
        h.sent.clear();
        h.feed(blob);
        repairs = 0;
        for (const Sym& sy : h.sent) repairs += sy.is_repair() ? 1 : 0;
        CHECK_EQ_U(repairs, 1u);
        // A huge override clamps to the GF(256) capacity (256 - k).
        h.sent.clear();
        h.framer.set_next_frame_override(1000);
        h.feed(blob);
        repairs = 0;
        for (const Sym& sy : h.sent) repairs += sy.is_repair() ? 1 : 0;
        const uint16_t k = static_cast<uint16_t>((blob.size() + s - 1) / s);
        CHECK_EQ_U(repairs, 256u - k);
        // Pass 205/O1: the min_k gate is gone, so the override applies at any
        // k. A small IDR takes the enforced parity; so does a small P frame.
        Harness small(fec);
        small.framer.set_next_frame_override(8);
        small.feed(make_frame(100, /*idr=*/true, 10));  // k = 1
        CHECK_EQ_U(repair_count_of(small.sent), 8u);
        Harness bare(fec);
        bare.framer.set_next_frame_override(2);
        bare.feed(make_frame(100, /*idr=*/false, 10));  // k = 1
        CHECK_EQ_U(repair_count_of(bare.sent), 2u);
    }

    // --- §14.1 Pass 205/O1: all-k parity, no class gate --------------------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.i_rate_permille = 300;
        fec.p_rate_permille = 200;
        fec.min_r = 0;  // isolate the rate-derived r from the Pass 98 floor

        const uint16_t s = Harness(fec).framer.symbol_size();

        // A small P frame: r = ceil(3 * 0.2) = 1.
        Harness p(fec);
        p.feed(make_frame(3 * s - 8, /*idr=*/false, 20));  // k = 3
        CHECK_EQ_U(repair_count_of(p.sent), 1u);

        // A small IDR: r = ceil(3 * 0.3) = 1.
        Harness i(fec);
        i.feed(make_frame(3 * s - 8, /*idr=*/true, 22));
        CHECK_EQ_U(repair_count_of(i.sent), 1u);

        // Above the old min_k the rate is the only term.
        Harness big(fec);
        big.feed(make_frame(5 * s - 8, /*idr=*/false, 24));  // k = 5
        CHECK_EQ_U(repair_count_of(big.sent), 1u);  // ceil(5 * 0.2)
    }

    // --- §14.1 Pass 98: minimum repair floor -------------------------------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.i_rate_permille = 300;
        fec.p_rate_permille = 200;
        fec.min_r = 2;
        auto rcount = [&](size_t body, bool idr) {
            Harness h(fec);
            h.feed(make_frame(body, idr, 30));
            return repair_count_of(h.sent);
        };
        const uint16_t s = Harness(fec).framer.symbol_size();
        // k=1 P-frame: ceil(1*0.2)=1, floored to min_r=2 — the only lever for
        // k=1, since ceil(1*rate)=1 for any rate <= 1000.
        CHECK_EQ_U(rcount(1 * s - 8, false), 2u);
        // k=3: ceil(3*0.2)=1 -> floored to 2.
        CHECK_EQ_U(rcount(3 * s - 8, false), 2u);
        // Large frame: ceil(20*0.2)=4 already exceeds the floor, so unchanged.
        CHECK_EQ_U(rcount(20 * s - 8, false), 4u);
        // A P_rate=0 stream stays deliberately bare: the floor must not force
        // FEC on where the rate says none.
        FrameFecConfig off = fec;
        off.p_rate_permille = 0;
        Harness disabled(off);
        disabled.feed(make_frame(3 * s - 8, false, 32));
        CHECK_EQ_U(repair_count_of(disabled.sent), 0u);
    }

    // === §14.1a non-referenced (SVC-T droppable) class — Pass 149 ==========

    // --- classification is disjoint and IDR-first -------------------------
    {
        Harness h(rlc());
        h.feed(make_frame(3000, /*idr=*/false, 0, kFrameFlagEnhance));
        CHECK_EQ_U(h.framer.stats().fec_enhance_frames, 1u);
        CHECK_EQ_U(h.framer.stats().idr_frames, 0u);
        // A producer marking BOTH must resolve to IDR: protect more, never
        // less. Nothing does this today; the framer must not rely on that.
        h.feed(make_frame(3000, /*idr=*/true, 1, kFrameFlagEnhance));
        CHECK_EQ_U(h.framer.stats().idr_frames, 1u);
        CHECK_EQ_U(h.framer.stats().fec_enhance_frames, 1u);  // unchanged
        // The counter is the drift detector, so it must count regardless of
        // whether e_rate is configured (it is unset here).
        CHECK(!h.framer.fec().e_rate_permille.has_value());
    }

    // --- e_rate UNSET: parity rate identical to the P class ---------------
    {
        for (size_t body : {3000u, 9000u, 40000u}) {
            Harness e(rlc());
            Harness p(rlc());
            e.feed(make_frame(body, false, 2, kFrameFlagEnhance));
            p.feed(make_frame(body, false, 2));
            CHECK_EQ_U(repair_count_of(e.sent), repair_count_of(p.sent));
            CHECK_EQ_U(e.sent.size(), p.sent.size());
        }
    }

    // --- e_rate = 0: genuinely zero parity, NOT resurrected by min_r ------
    {
        Harness h(rlc(/*e=*/0));
        h.feed(make_frame(9000, false, 3, kFrameFlagEnhance));
        CHECK_EQ_U(repair_count_of(h.sent), 0u);
        CHECK(!h.sent.empty());  // source symbols still ship
        // ...while the P class at the same size keeps its parity.
        Harness p(rlc(/*e=*/0));
        p.feed(make_frame(9000, false, 3));
        CHECK(repair_count_of(p.sent) > 0u);
    }

    // --- the min_r trap: a small non-zero e_rate is dominated by the floor -
    {
        // e_rate 10 permille on a k~7 frame gives ceil(0.07)=1, floored to
        // min_r=2 — the same r the 100 permille P rate produces. Documented
        // in §14.1a so nobody reads a "light" setting as light.
        Harness lo(rlc(/*e=*/10));
        Harness p(rlc());
        lo.feed(make_frame(9000, false, 4, kFrameFlagEnhance));
        p.feed(make_frame(9000, false, 4));
        CHECK_EQ_U(repair_count_of(lo.sent), repair_count_of(p.sent));
    }

    // --- §14.2 exemption: an override never touches this class ------------
    {
        // The override would otherwise repaint parity onto exactly the frames
        // e_rate exists to leave bare. Pass 205 removed the ARQ term from the
        // override gate; the class exemption is now the only one.
        Harness h(rlc(/*e=*/0));
        h.framer.set_next_frame_override(/*parity_symbols=*/9);
        h.feed(make_frame(9000, false, 9, kFrameFlagEnhance));
        CHECK_EQ_U(repair_count_of(h.sent), 0u);  // still bare
        // Same override on a P frame DOES actuate — the exemption is scoped
        // to the class, not a disabling of §14.2.
        Harness p(rlc(/*e=*/0));
        p.framer.set_next_frame_override(9);
        p.feed(make_frame(9000, false, 9));
        CHECK_EQ_U(repair_count_of(p.sent), 9u);
        // ...and still does at small k, where the removed min_k gate used to
        // block it.
        Harness sm(rlc(/*e=*/0));
        sm.framer.set_next_frame_override(5);
        sm.feed(make_frame(sm.framer.symbol_size(), false, 10));
        CHECK_EQ_U(repair_count_of(sm.sent), 5u);
    }

    // --- live retune (§15.5) is a full replacement ------------------------
    {
        Harness h(rlc(/*e=*/0));
        CHECK(h.framer.fec().e_rate_permille.has_value());
        h.framer.set_fec_rates(250, 100, 2);  // e omitted => cleared
        CHECK(!h.framer.fec().e_rate_permille.has_value());
        h.feed(make_frame(9000, false, 11, kFrameFlagEnhance));
        CHECK(repair_count_of(h.sent) > 0u);  // back to inheriting p_rate
    }

    return wbtest_finish("frame_framer_test");
}
