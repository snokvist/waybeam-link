// SPDX-License-Identifier: GPL-2.0-or-later
// FrameFramer (§5.1a) + §14.1 FEC: fragmentation, block/seq/EOB/ARQ stamping,
// repair emission + subheader, the min_k / oversize-k gates, and an end-to-end
// RLC decode round-trip proving the emitted symbols are recoverable.
#include "wblink/frame_framer.h"

#include <cstdint>
#include <cstring>
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
    bool arq() const { return (hdr.data_flags & data_flags::kArq) != 0; }
};

struct Harness {
    FrameFramer framer;
    std::vector<Sym> sent;

    explicit Harness(FrameFecConfig fec = {},
                     FrameArqMode arq_mode = FrameArqMode::kIdrOnly)
        : framer(make_cfg(fec, arq_mode)) {
        framer.set_operating_point(4, 0x41, 1424);
    }
    static FrameFramerConfig make_cfg(FrameFecConfig fec,
                                      FrameArqMode arq_mode) {
        FrameFramerConfig c;
        c.originator = 7;
        c.session_id = 99;
        c.stream_id = 0;
        c.stream_type = stream_type::kRtp;
        c.arq_mode = arq_mode;
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
std::vector<uint8_t> make_frame(size_t body, bool idr, uint8_t seed) {
    std::vector<uint8_t> b(kVencFrameMetaSize + body, 0);
    b[4] = kFrameCodecH265;
    b[5] = idr ? kFrameFlagIdr : 0;
    for (size_t i = 0; i < body; ++i) {
        b[kVencFrameMetaSize + i] = static_cast<uint8_t>((i * 31u + seed) & 0xFF);
    }
    return b;
}

}  // namespace

int main() {
    // --- opt-in P-frame ARQ retains a distinct wire class ------------------
    {
        Harness h({}, FrameArqMode::kAllFrames);
        h.feed(make_frame(3000, /*idr=*/false, 0));
        CHECK(!h.sent.empty());
        for (const Sym& sy : h.sent) {
            CHECK(!sy.arq());
            CHECK((sy.hdr.data_flags & data_flags::kPframeArq) != 0);
        }
        CHECK_EQ_U(h.framer.stats().idr_frames, 0u);
        CHECK_EQ_U(h.framer.stats().arq_frames, 1u);
        h.feed(make_frame(3000, /*idr=*/true, 1));
        CHECK_EQ_U(h.framer.stats().idr_frames, 1u);
        CHECK_EQ_U(h.framer.stats().arq_frames, 2u);
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
        // seqs 0..k-1, one block, EOB on last only, ARQ on all (IDR), no repair.
        std::vector<uint8_t> reasm;
        for (uint16_t i = 0; i < k; ++i) {
            CHECK_EQ_U(h.sent[i].hdr.seq, i);
            CHECK_EQ_U(h.sent[i].hdr.block_id, 0u);
            CHECK(!h.sent[i].is_repair());
            CHECK(h.sent[i].arq());  // IDR
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
        CHECK(!h.sent[k].arq());  // non-IDR
    }

    // --- min_k gate: k <= min_k => ARQ-only, r = 0 --------------------------
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

    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.min_k = 3;
        Harness h(fec);
        h.feed(make_frame(1000, true, 3));  // k = 1 <= 3
        for (const Sym& s : h.sent) CHECK(!s.is_repair());
        CHECK_EQ_U(h.framer.stats().repair_symbols, 0u);
    }

    // --- FEC on IDR (i_rate) + end-to-end decode round-trip -----------------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.i_rate_permille = 250;
        fec.p_rate_permille = 100;
        fec.min_k = 3;
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

    // --- P-frame uses p_rate; no ARQ ----------------------------------------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.p_rate_permille = 100;
        fec.min_k = 3;
        fec.min_r = 0;  // isolate the rate formula from the Pass 98 floor
        Harness h(fec);
        const uint16_t s = h.framer.symbol_size();
        auto blob = make_frame(10000, /*idr=*/false, 6);
        const uint16_t k = static_cast<uint16_t>((blob.size() + s - 1) / s);
        const uint16_t r = static_cast<uint16_t>((static_cast<uint32_t>(k) * 100 + 999) / 1000);
        h.feed(blob);
        CHECK_EQ_U(h.framer.stats().repair_symbols, r);
        for (const Sym& sy : h.sent) CHECK(!sy.arq());  // P-frame
    }

    // --- oversize-k cap: k + r > 256 => FEC off + stat ----------------------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.i_rate_permille = 250;
        fec.min_k = 3;
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
        Harness h(fec, FrameArqMode::kAllFrames);
        const uint16_t s = h.framer.symbol_size();
        auto blob = make_frame(4 * s, /*idr=*/false, 9);  // k = 5 > min_k
        // Override: zero parity + PFRAME_ARQ cleared for this frame only.
        h.framer.set_next_frame_override(0, /*allow_pframe_arq=*/false);
        h.feed(blob);
        size_t repairs = 0;
        for (const Sym& sy : h.sent) {
            repairs += sy.is_repair() ? 1 : 0;
            CHECK((sy.hdr.data_flags & data_flags::kPframeArq) == 0);
        }
        CHECK_EQ_U(repairs, 0u);
        // The override was consumed: the next frame is back on §14.1 fixed
        // rates (ceil(5*0.1) = 1 repair) with PFRAME_ARQ restored.
        h.sent.clear();
        h.feed(blob);
        repairs = 0;
        bool parq = false;
        for (const Sym& sy : h.sent) {
            repairs += sy.is_repair() ? 1 : 0;
            parq |= (sy.hdr.data_flags & data_flags::kPframeArq) != 0;
        }
        CHECK_EQ_U(repairs, 1u);
        CHECK(parq);
        // A huge override clamps to the GF(256) capacity (256 - k).
        h.sent.clear();
        h.framer.set_next_frame_override(1000, true);
        h.feed(blob);
        repairs = 0;
        for (const Sym& sy : h.sent) repairs += sy.is_repair() ? 1 : 0;
        const uint16_t k = static_cast<uint16_t>((blob.size() + s - 1) / s);
        CHECK_EQ_U(repairs, 256u - k);
        // The min_k ARQ-only rule survives enforcement (§14.2 rule 1) — but
        // per Pass 94 only for an ARQ-eligible frame. An IDR under idr-only is
        // eligible, so k <= min_k still ignores the parity override entirely.
        Harness small(fec, FrameArqMode::kIdrOnly);
        small.framer.set_next_frame_override(8, true);
        small.feed(make_frame(100, /*idr=*/true, 10));  // k = 1 <= min_k 3
        for (const Sym& sy : small.sent) CHECK(!sy.is_repair());
        // ...and the Pass 94 case: the same small frame as a P-frame under
        // idr-only has no ARQ to fall back on, so the gate must NOT block the
        // override. Before Pass 94 this frame shipped bare — that was B11.
        Harness bare(fec, FrameArqMode::kIdrOnly);
        bare.framer.set_next_frame_override(2, true);
        bare.feed(make_frame(100, /*idr=*/false, 10));  // k = 1 <= min_k 3
        size_t bare_repairs = 0;
        for (const Sym& sy : bare.sent) bare_repairs += sy.is_repair() ? 1 : 0;
        CHECK_EQ_U(bare_repairs, 2u);
    }

    // --- §14.1 Pass 94: the min_k gate is conditional on ARQ eligibility ----
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.i_rate_permille = 300;
        fec.p_rate_permille = 200;
        fec.min_k = 3;
        fec.min_r = 0;  // isolate the rate-derived r from the Pass 98 floor

        // A small P-frame under idr-only: NOT ARQ-eligible, so it gets parity
        // rather than nothing. r = ceil(3 * 0.2) = 1.
        Harness p(fec, FrameArqMode::kIdrOnly);
        const uint16_t s = p.framer.symbol_size();
        p.feed(make_frame(3 * s - 8, /*idr=*/false, 20));  // k = 3 == min_k
        size_t repairs = 0;
        for (const Sym& sy : p.sent) repairs += sy.is_repair() ? 1 : 0;
        CHECK_EQ_U(repairs, 1u);

        // The same frame under all-frames IS ARQ-eligible, so the gate holds
        // and the optimisation still applies: no parity, ARQ carries it.
        Harness a(fec, FrameArqMode::kAllFrames);
        a.feed(make_frame(3 * s - 8, /*idr=*/false, 21));
        for (const Sym& sy : a.sent) CHECK(!sy.is_repair());

        // A small IDR is ARQ-eligible under either mode: gate holds.
        Harness i(fec, FrameArqMode::kIdrOnly);
        i.feed(make_frame(3 * s - 8, /*idr=*/true, 22));
        for (const Sym& sy : i.sent) CHECK(!sy.is_repair());

        // §4.1 cadence cutoff removes ARQ from every class, so the gate goes
        // inert and even an IDR gets parity. r = ceil(3 * 0.3) = 1.
        Harness c(fec, FrameArqMode::kAllFrames);
        c.framer.set_arq_suppressed(true);
        c.feed(make_frame(3 * s - 8, /*idr=*/true, 23));
        repairs = 0;
        for (const Sym& sy : c.sent) repairs += sy.is_repair() ? 1 : 0;
        CHECK_EQ_U(repairs, 1u);

        // Above min_k nothing changes: the gate was never in play.
        Harness big(fec, FrameArqMode::kAllFrames);
        big.feed(make_frame(5 * s - 8, /*idr=*/false, 24));  // k = 5 > min_k
        repairs = 0;
        for (const Sym& sy : big.sent) repairs += sy.is_repair() ? 1 : 0;
        CHECK_EQ_U(repairs, 1u);  // ceil(5 * 0.2)
    }

    // --- §14.1 Pass 98: minimum repair floor -------------------------------
    {
        FrameFecConfig fec;
        fec.scheme = FecScheme::kRlc256;
        fec.i_rate_permille = 300;
        fec.p_rate_permille = 200;
        fec.min_k = 3;
        fec.min_r = 2;
        auto rcount = [&](size_t body, bool idr, FrameArqMode mode) {
            Harness h(fec, mode);
            h.feed(make_frame(body, idr, 30));
            size_t r = 0;
            for (const Sym& sy : h.sent) r += sy.is_repair() ? 1 : 0;
            return r;
        };
        const uint16_t s = Harness(fec).framer.symbol_size();
        // k=1 P-frame under idr-only: ceil(1*0.2)=1, floored to min_r=2 — the
        // only lever for k=1, since ceil(1*rate)=1 for any rate <= 1000.
        CHECK_EQ_U(rcount(1 * s - 8, false, FrameArqMode::kIdrOnly), 2u);
        // k=3: ceil(3*0.2)=1 -> floored to 2.
        CHECK_EQ_U(rcount(3 * s - 8, false, FrameArqMode::kIdrOnly), 2u);
        // Large frame: ceil(20*0.2)=4 already exceeds the floor, so unchanged.
        CHECK_EQ_U(rcount(20 * s - 8, false, FrameArqMode::kIdrOnly), 4u);
        // The floor never resurrects an ARQ-covered small frame (min_k gate
        // still returns 0 first) or a P_rate=0 stream.
        Harness gated(fec, FrameArqMode::kAllFrames);  // k<=min_k IS arq here
        gated.feed(make_frame(2 * s - 8, false, 31));
        for (const Sym& sy : gated.sent) CHECK(!sy.is_repair());
        FrameFecConfig off = fec;
        off.p_rate_permille = 0;  // P-FEC disabled: floor must not force it on
        Harness disabled(off, FrameArqMode::kIdrOnly);
        disabled.feed(make_frame(3 * s - 8, false, 32));
        for (const Sym& sy : disabled.sent) CHECK(!sy.is_repair());
    }

    // --- §4.1 Pass 40 high-cadence ARQ cutoff --------------------------------
    {
        Harness h({}, FrameArqMode::kAllFrames);
        h.framer.set_arq_suppressed(true);
        h.feed(make_frame(3000, /*idr=*/true, 11));
        h.feed(make_frame(3000, /*idr=*/false, 12));
        for (const Sym& sy : h.sent) {
            CHECK((sy.hdr.data_flags &
                   (data_flags::kArq | data_flags::kPframeArq)) == 0);
        }
        CHECK_EQ_U(h.framer.stats().arq_frames, 0u);
        CHECK_EQ_U(h.framer.stats().arq_cutoff_frames, 2u);
        CHECK_EQ_U(h.framer.stats().idr_frames, 1u);
        // Cadence drops back below the cutoff: stamping resumes (sticky off).
        h.framer.set_arq_suppressed(false);
        h.sent.clear();
        h.feed(make_frame(3000, /*idr=*/true, 13));
        bool arq = false;
        for (const Sym& sy : h.sent) {
            arq |= (sy.hdr.data_flags & data_flags::kArq) != 0;
        }
        CHECK(arq);
        CHECK_EQ_U(h.framer.stats().arq_frames, 1u);
        CHECK_EQ_U(h.framer.stats().arq_cutoff_frames, 2u);
        // idr-only mode: a suppressed P frame is not counted (not ARQ-class).
        Harness p({}, FrameArqMode::kIdrOnly);
        p.framer.set_arq_suppressed(true);
        p.feed(make_frame(3000, /*idr=*/false, 14));
        CHECK_EQ_U(p.framer.stats().arq_cutoff_frames, 0u);
    }

    return wbtest_finish("frame_framer_test");
}
