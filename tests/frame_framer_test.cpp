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
        for (uint16_t j = 0; j < r; ++j) {
            const uint8_t* sub = rep[j]->payload.data();
            CHECK_EQ_U(sub[kFecOffRepairIdx], j);
            CHECK_EQ_U(be16_read(sub + kFecOffWindowLen), k);
            CHECK_EQ_U(be32_read(sub + kFecOffWindowBaseSeq), 0u);
            CHECK_EQ_U(be32_read(sub + kFecOffFrameLen), blob.size());
            CHECK_EQ_U(rep[j]->payload.size(), kFecRepairSubheaderSize + s);
        }

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

    return wbtest_finish("frame_framer_test");
}
