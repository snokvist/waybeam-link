// SPDX-License-Identifier: GPL-2.0-or-later
// §6.3b SpatialRepair: learn geometry from delivered blobs, then repair
// failed blocks across the erasure matrix — middle/first/last slice, two
// slices, whole frame (freeze), meta chunk lost, and every refusal shape.
#include "wblink/spatial_repair.h"

#include <cstring>
#include <algorithm>
#include <map>
#include <vector>

#include "hevc_conceal_vector.h"
#include "wblink/frame_shm_format.h"
#include "wbtest.h"

using wblink::SpatialRepair;
using wblink::SpatialRepairConfig;

namespace {

struct Nal {
    size_t sc;
    size_t off;
    size_t end;
};

std::vector<Nal> scan(const unsigned char* d, size_t n) {
    std::vector<Nal> out;
    for (size_t i = 0; i + 3 <= n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            out.push_back({(i > 0 && d[i - 1] == 0) ? i - 1 : i, i + 3, 0});
            i += 2;
        }
    }
    for (size_t k = 0; k < out.size(); ++k) {
        out[k].end = k + 1 < out.size() ? out[k + 1].sc : n;
    }
    return out;
}

// Split the embedded stream into per-AU [VencFrameMeta][Annex-B] blobs the
// way the venc producer would publish them.
std::vector<std::vector<uint8_t>> make_blobs() {
    const unsigned char* v = kTinyHevcStream;
    const std::vector<Nal> nals = scan(v, sizeof(kTinyHevcStream));
    std::vector<std::vector<uint8_t>> blobs;
    std::vector<uint8_t> prefix;  // VPS/SPS/PPS ride in the next VCL's AU
    uint32_t pts = 1000;
    for (const Nal& n : nals) {
        const uint8_t t = (v[n.off] >> 1) & 0x3F;
        if (t > 31) {
            prefix.insert(prefix.end(), v + n.sc, v + n.end);
            continue;
        }
        if ((v[n.off + 2] >> 7) & 1) {  // first_slice: new AU
            std::vector<uint8_t> b(wblink::kVencFrameMetaSize, 0);
            pts += 20000;
            std::memcpy(b.data(), &pts, 4);
            b[4] = wblink::kFrameCodecH265;
            b[5] = (16 <= t && t <= 23) ? wblink::kFrameFlagIdr : 0;
            b.insert(b.end(), prefix.begin(), prefix.end());
            prefix.clear();
            blobs.push_back(std::move(b));
        }
        blobs.back().insert(blobs.back().end(), v + n.sc, v + n.end);
    }
    return blobs;
}

// Chunk a blob into s-byte pieces, keeping only the listed indices.
std::map<uint16_t, std::vector<uint8_t>> chunks(
    const std::vector<uint8_t>& blob, uint16_t s,
    const std::vector<int>& drop) {
    std::map<uint16_t, std::vector<uint8_t>> out;
    const uint16_t k =
        static_cast<uint16_t>((blob.size() + s - 1) / s);
    for (uint16_t i = 0; i < k; ++i) {
        bool dropped = false;
        for (const int d : drop) {
            dropped |= (d == i);
        }
        if (dropped) {
            continue;
        }
        const size_t off = static_cast<size_t>(i) * s;
        const size_t len = std::min<size_t>(s, blob.size() - off);
        out.emplace(i, std::vector<uint8_t>(blob.begin() + off,
                                            blob.begin() + off + len));
    }
    return out;
}

// Byte range [begin, end) of slice `idx` (start code to next start code)
// within a blob, so tests can erase exactly one slice's bytes.
void slice_span(const std::vector<uint8_t>& blob, int idx, size_t* begin,
                size_t* end) {
    const std::vector<Nal> nals =
        scan(blob.data() + wblink::kVencFrameMetaSize,
             blob.size() - wblink::kVencFrameMetaSize);
    *begin = nals[idx].sc + wblink::kVencFrameMetaSize;
    *end = nals[idx].end + wblink::kVencFrameMetaSize;
}

// A chunk index lying strictly inside slice `idx`'s bytes (past the start
// code, before the next NAL), so erasing it kills exactly that slice.
int interior_chunk(const std::vector<uint8_t>& blob, int idx, uint16_t s) {
    size_t b = 0;
    size_t e = 0;
    slice_span(blob, idx, &b, &e);
    for (int c = static_cast<int>(b / s) + 1;
         static_cast<size_t>(c + 1) * s <= e; ++c) {
        if (static_cast<size_t>(c) * s >= b + 3) {
            return c;
        }
    }
    return -1;
}

int count_slices(const uint8_t* blob, size_t len) {
    int n = 0;
    for (const Nal& nal :
         scan(blob + wblink::kVencFrameMetaSize,
              len - wblink::kVencFrameMetaSize)) {
        const uint8_t t = (blob[wblink::kVencFrameMetaSize + nal.off] >> 1) &
                          0x3F;
        n += (t <= 31);
    }
    return n;
}

}  // namespace

int main() {
    const auto blobs = make_blobs();
    CHECK_EQ_U(blobs.size(), 8);  // 1 IDR + 7 P

    SpatialRepairConfig cfg;
    SpatialRepair sr(cfg);
    CHECK(!sr.geometry_known());

    // Nothing learned yet: repair must refuse.
    std::vector<uint8_t> got;
    const SpatialRepair::Emit grab = [&](const uint8_t* f, size_t n) {
        got.assign(f, f + n);
    };
    // Small chunks so every slice spans several of them and an erasure can
    // target exactly one slice.
    const uint16_t s = 8;
    {
        auto src = chunks(blobs[3], s, {1});
        CHECK(!sr.repair(static_cast<uint16_t>((blobs[3].size() + s - 1) / s),
                         s, static_cast<uint32_t>(blobs[3].size()), src,
                         grab));
        CHECK_EQ_U(sr.stats().salvage_failed, 1);
    }

    // Learn from the first three delivered frames (IDR + 2 P).
    for (int i = 0; i < 3; ++i) {
        sr.learn(blobs[i].data(), blobs[i].size());
    }
    CHECK(sr.geometry_known());

    const auto repair_drop = [&](int au, const std::vector<int>& drop,
                                 uint32_t frame_len_known) {
        got.clear();
        auto src = chunks(blobs[au], s, drop);
        const uint16_t k =
            static_cast<uint16_t>((blobs[au].size() + s - 1) / s);
        return sr.repair(k, s,
                         frame_len_known
                             ? static_cast<uint32_t>(blobs[au].size())
                             : 0,
                         src, grab);
    };

    // -- middle slice erased (one interior chunk) -------------------------
    int c1 = interior_chunk(blobs[3], 1, s);
    CHECK(c1 > 0);
    CHECK(repair_drop(3, {c1}, true));
    CHECK_EQ_U(count_slices(got.data(), got.size()), 3);
    CHECK_EQ_U(sr.stats().frames_salvaged, 1);
    CHECK_EQ_U(sr.stats().slices_synthesized, 1);
    // survivor NAL body byte-identical: slice 0's bytes (start-code framing
    // aside — 3- vs 4-byte codes are equivalent) appear verbatim in the
    // output.
    {
        size_t s0b = 0;
        size_t s0e = 0;
        slice_span(blobs[3], 0, &s0b, &s0e);
        const auto nals0 = scan(blobs[3].data() + s0b, s0e - s0b);
        CHECK_EQ_U(nals0.size(), 1);
        const uint8_t* body = blobs[3].data() + s0b + nals0[0].off;
        const size_t body_len = nals0[0].end - nals0[0].off;
        CHECK(std::search(got.begin(), got.end(), body, body + body_len) !=
              got.end());
    }
    // meta rides through untouched
    CHECK(std::memcmp(got.data(), blobs[3].data(),
                      wblink::kVencFrameMetaSize) == 0);

    // -- meta chunk + first slice erased ----------------------------------
    sr.learn(blobs[3].data(), blobs[3].size());
    int c0 = interior_chunk(blobs[4], 0, s);
    CHECK(c0 > 0);
    CHECK(repair_drop(4, {0, c0}, true));  // chunk 0 holds VencFrameMeta
    CHECK_EQ_U(count_slices(got.data(), got.size()), 3);
    CHECK_EQ_U(got[4], wblink::kFrameCodecH265);  // synthesized meta
    CHECK_EQ_U(got[5] & wblink::kFrameFlagIdr, 0);

    // -- last slice erased, frame_len UNKNOWN ----------------------------
    sr.learn(blobs[4].data(), blobs[4].size());
    int c2 = interior_chunk(blobs[5], 2, s);
    CHECK(c2 > 0);
    CHECK(repair_drop(5, {c2}, false));
    CHECK_EQ_U(count_slices(got.data(), got.size()), 3);

    // -- two slices erased ------------------------------------------------
    sr.learn(blobs[5].data(), blobs[5].size());
    int d1 = interior_chunk(blobs[6], 1, s);
    int d2 = interior_chunk(blobs[6], 2, s);
    CHECK(d1 > 0 && d2 > 0);
    const uint64_t synth_before = sr.stats().slices_synthesized;
    CHECK(repair_drop(6, {d1, d2}, true));
    CHECK_EQ_U(count_slices(got.data(), got.size()), 3);
    CHECK_EQ_U(sr.stats().slices_synthesized, synth_before + 2);

    // -- whole frame erased on a CYCLING-RPS stream: freeze refuses -------
    // The HM lowdelay GOP-4 vector cycles its per-picture reference set, so
    // the donor's RPS is invalid at POC+1 and the freeze path must fall back
    // to drop (rps_stable_ guard); slice-level salvage above was unaffected.
    sr.learn(blobs[6].data(), blobs[6].size());
    {
        got.clear();
        const uint16_t k =
            static_cast<uint16_t>((blobs[7].size() + s - 1) / s);
        // Keep only the last chunk (no complete slice in it): freeze path.
        std::vector<int> all;
        for (int i = 0; i + 1 < k; ++i) {
            all.push_back(i);
        }
        auto tail_only = chunks(blobs[7], s, all);
        CHECK(!sr.repair(k, s, static_cast<uint32_t>(blobs[7].size()),
                         tail_only, grab));
        CHECK_EQ_U(sr.stats().frames_frozen, 0);
    }

    // -- whole frame erased with a STABLE RPS: freeze succeeds ------------
    // Stability = the last two donors' RPS spans agree, so feed the same P
    // picture twice (the unit-level stand-in for a constant-RPS stream).
    {
        SpatialRepair sf(cfg);
        sf.learn(blobs[0].data(), blobs[0].size());  // IDR: SPS/PPS/geometry
        sf.learn(blobs[1].data(), blobs[1].size());  // first donor: not yet
        sf.learn(blobs[1].data(), blobs[1].size());  // same RPS twice: stable
        got.clear();
        const uint16_t k =
            static_cast<uint16_t>((blobs[2].size() + s - 1) / s);
        std::vector<int> all;
        for (int i = 0; i + 1 < k; ++i) {
            all.push_back(i);
        }
        auto tail_only = chunks(blobs[2], s, all);
        CHECK(sf.repair(k, s, static_cast<uint32_t>(blobs[2].size()),
                        tail_only, grab));
        CHECK_EQ_U(count_slices(got.data(), got.size()), 3);
        CHECK_EQ_U(sf.stats().frames_frozen, 1);
        // A donor with a different RPS then disables freeze but not salvage.
        sf.learn(blobs[3].data(), blobs[3].size());
        got.clear();
        const uint16_t k4 =
            static_cast<uint16_t>((blobs[4].size() + s - 1) / s);
        std::vector<int> all4;
        for (int i = 0; i + 1 < k4; ++i) {
            all4.push_back(i);
        }
        auto tail4 = chunks(blobs[4], s, all4);
        CHECK(!sf.repair(k4, s, static_cast<uint32_t>(blobs[4].size()),
                         tail4, grab));
        CHECK_EQ_U(sf.stats().frames_frozen, 1);
        const int ci = interior_chunk(blobs[4], 1, s);
        CHECK(ci > 0);
        got.clear();
        auto src = chunks(blobs[4], s, {ci});
        CHECK(sf.repair(k4, s, static_cast<uint32_t>(blobs[4].size()), src,
                        grab));
        CHECK_EQ_U(sf.stats().frames_salvaged, 1);
    }

    // -- refusals ----------------------------------------------------------
    const uint64_t failed_before = sr.stats().salvage_failed;
    // IDR frame: never concealed (meta IDR flag + IRAP NALs both refuse).
    {
        got.clear();
        const int ci = interior_chunk(blobs[0], 1, s);
        CHECK(ci > 0);
        auto src = chunks(blobs[0], s, {ci});
        const uint16_t k =
            static_cast<uint16_t>((blobs[0].size() + s - 1) / s);
        CHECK(!sr.repair(k, s, static_cast<uint32_t>(blobs[0].size()), src,
                         grab));
    }
    // s unknown.
    {
        auto src = chunks(blobs[3], s, {1});
        CHECK(!sr.repair(5, 0, static_cast<uint32_t>(blobs[3].size()), src,
                         grab));
    }
    CHECK_EQ_U(sr.stats().salvage_failed, failed_before + 2);

    // reset_stream forgets the learned shape.
    sr.reset_stream();
    CHECK(!sr.geometry_known());

    return wbtest_finish("spatial_repair_test");
}
