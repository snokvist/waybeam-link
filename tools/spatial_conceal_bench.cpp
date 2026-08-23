// SPDX-License-Identifier: GPL-2.0-or-later
// spatial_conceal_bench — the §6.3b milestone chain, production code only:
//
//   real multi-slice HEVC AUs -> FrameFramer (§5.1a fragmentation + §14.1
//   Cauchy-RS parity) -> deterministic symbol loss -> FrameReassembler
//   (fast/FEC/salvage) -> SpatialRepair (slice concealment / freeze)
//   -> egressed access units written to a file for real decoders.
//
// Frames whose meta carries the IDR flag are exempt from loss (production
// gives IDR i_rate parity + ARQ retransmission until delivered). Prints
// per-outcome counters and the salvage-path latency distribution.
//
// Usage: spatial_conceal_bench <in.265> <out.265> <loss_permille> <seed>
//                              [p_rate_permille=100] [fps=100]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "wblink/frame_framer.h"
#include "wblink/frame_reassembler.h"
#include "wblink/frame_shm_format.h"
#include "wblink/spatial_repair.h"
#include "wblink/types.h"

using namespace wblink;

namespace {

struct Nal {
    size_t sc;
    size_t off;
    size_t end;
};

std::vector<Nal> scan(const uint8_t* d, size_t n) {
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

// xorshift32 — deterministic, seedable, no libc rand state.
struct Rng {
    uint32_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    bool drop(uint32_t permille) { return next() % 1000 < permille; }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: %s <in.265> <out.265> <loss_permille> <seed> "
                     "[p_rate=100] [fps=100]\n",
                     argv[0]);
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::perror("open");
        return 1;
    }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        data.insert(data.end(), buf, buf + got);
    }
    std::fclose(f);
    const uint32_t loss = static_cast<uint32_t>(std::strtol(argv[3], nullptr, 10));
    Rng rng{static_cast<uint32_t>(std::strtol(argv[4], nullptr, 10)) | 1u};
    const uint16_t p_rate =
        argc > 5 ? static_cast<uint16_t>(std::strtol(argv[5], nullptr, 10)) : 100;
    const uint32_t fps =
        argc > 6 ? static_cast<uint32_t>(std::strtol(argv[6], nullptr, 10)) : 100;

    // Group into per-AU blobs the way the venc producer publishes them.
    std::vector<std::vector<uint8_t>> blobs;
    std::vector<uint8_t> prefix;
    for (const Nal& n : scan(data.data(), data.size())) {
        const uint8_t t = (data[n.off] >> 1) & 0x3F;
        if (t > 31) {
            prefix.insert(prefix.end(), data.begin() + static_cast<long>(n.sc),
                          data.begin() + static_cast<long>(n.end));
            continue;
        }
        if ((data[n.off + 2] >> 7) & 1) {
            std::vector<uint8_t> b(kVencFrameMetaSize, 0);
            const uint32_t pts = static_cast<uint32_t>(blobs.size()) * (1000000u / fps);
            std::memcpy(b.data(), &pts, 4);
            b[4] = kFrameCodecH265;
            b[5] = (16 <= t && t <= 23) ? kFrameFlagIdr : 0;
            b.insert(b.end(), prefix.begin(), prefix.end());
            prefix.clear();
            blobs.push_back(std::move(b));
        }
        if (blobs.empty()) {
            continue;  // VCL before any first-slice flag: skip
        }
        blobs.back().insert(blobs.back().end(),
                            data.begin() + static_cast<long>(n.sc),
                            data.begin() + static_cast<long>(n.end));
    }

    FrameFramerConfig fc;
    fc.stream_type = stream_type::kRtp;
    fc.fec.scheme = FecScheme::kRlc256;
    fc.fec.i_rate_permille = 250;
    fc.fec.p_rate_permille = p_rate;
    fc.fec.min_k = 3;
    fc.fec.min_r = 2;
    FrameFramer framer(fc);

    FrameReassemblerConfig rc;
    rc.deadline_ms = 50;
    FrameReassembler reasm(rc);
    SpatialRepairConfig sc;
    SpatialRepair repair(sc);

    uint64_t salvage_ns_total = 0;
    uint64_t salvage_ns_max = 0;
    uint32_t salvage_runs = 0;
    reasm.set_salvage_hook([&](const SalvageView& v,
                               const FrameReassembler::Emit& emit) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = repair.repair(v.k, v.s, v.frame_len, *v.sources, emit);
        const auto ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        salvage_ns_total += ns;
        salvage_ns_max = std::max(salvage_ns_max, ns);
        ++salvage_runs;
        return ok;
    });

    FILE* out = std::fopen(argv[2], "wb");
    if (!out) {
        std::perror("open out");
        return 1;
    }
    uint64_t frames_out = 0;
    const FrameReassembler::Emit emit = [&](const uint8_t* frame, size_t len) {
        repair.learn(frame, len);
        if (len > kVencFrameMetaSize) {
            std::fwrite(frame + kVencFrameMetaSize, 1,
                        len - kVencFrameMetaSize, out);
        }
        ++frames_out;
        return true;
    };

    const uint32_t frame_ms = 1000 / (fps ? fps : 100);
    uint64_t now = 1000;
    uint64_t src_sent = 0;
    uint64_t src_dropped = 0;
    for (const auto& blob : blobs) {
        const bool idr = (blob[5] & kFrameFlagIdr) != 0;
        framer.on_frame(
            blob.data(), blob.size(), now,
            [&](const uint8_t* pkt, size_t n, const DataHeader& hdr,
                uint64_t) {
                const bool is_src =
                    (hdr.data_flags & data_flags::kFecRepair) == 0;
                src_sent += is_src;
                if (!idr && rng.drop(loss)) {  // IDR: ARQ-protected
                    src_dropped += is_src;
                    return;
                }
                reasm.push(hdr.block_id, hdr.data_flags,
                           pkt + kDataHeaderSize, n - kDataHeaderSize, now,
                           emit);
            });
        now += frame_ms;
        reasm.tick(now, emit);
    }
    reasm.tick(now + 100, emit);
    std::fclose(out);

    const FrameReassemblerStats& rs = reasm.stats();
    const SpatialRepairStats& ss = repair.stats();
    std::printf(
        "frames_in=%zu frames_out=%llu fast=%llu fec=%llu salvaged=%llu "
        "frozen=%llu salvage_failed=%llu dropped=%llu "
        "slices_synthesized=%llu src_loss=%.1f%%\n",
        blobs.size(), static_cast<unsigned long long>(frames_out),
        static_cast<unsigned long long>(rs.frames_fast),
        static_cast<unsigned long long>(rs.frames_fec),
        static_cast<unsigned long long>(ss.frames_salvaged),
        static_cast<unsigned long long>(ss.frames_frozen),
        static_cast<unsigned long long>(ss.salvage_failed),
        static_cast<unsigned long long>(rs.frames_unrecoverable),
        static_cast<unsigned long long>(ss.slices_synthesized),
        src_sent ? 100.0 * static_cast<double>(src_dropped) /
                       static_cast<double>(src_sent)
                 : 0.0);
    if (salvage_runs > 0) {
        std::printf("salvage runs=%u avg=%.1fus max=%.1fus\n", salvage_runs,
                    static_cast<double>(salvage_ns_total) / salvage_runs /
                        1000.0,
                    static_cast<double>(salvage_ns_max) / 1000.0);
    }
    return 0;
}
