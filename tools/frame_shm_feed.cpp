// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic frame-SHM bench endpoint (no GStreamer): deterministic
// [VencFrameMeta][pattern] blobs into a producer ring, and a verifying
// consumer for the RX egress ring. Payload byte j of frame i is
// (i*31 + j) & 0xff, so the consumer proves byte-exact §6.3a reassembly,
// not just delivery counts.
//
//   frame_shm_feed produce <ring> <frames> <fps> <p_bytes> <idr_every>
//   frame_shm_feed consume <ring> <min_frames> <idle_ms> <total_ms>
//
// produce: creates the ring (the waybeam-link TX attaches as consumer). IDR
// frames (every idr_every, starting at 0) are 4x p_bytes. Prints the
// gst-bench-compatible "producer frames=N ... full_drop=X oversize_drop=Y".
// consume: attaches to the RX-created ring, reads until no frame arrives for
// idle_ms (or total_ms elapses), verifies every byte, prints
// "consumer frames=N bad=B" and exits 0 iff N >= min_frames and B == 0.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wblink/frame_shm.h"
#include "wblink/frame_shm_format.h"

using namespace wblink;

namespace {

void fill_frame(std::vector<uint8_t>& blob, uint32_t index, size_t bytes,
                bool idr) {
    blob.assign(bytes, 0);
    VencFrameMeta meta;
    meta.pts = index;
    meta.codec = kFrameCodecH265;
    meta.flags = idr ? 0x01 : 0x00;
    std::memcpy(blob.data(), &meta, kVencFrameMetaSize);
    for (size_t j = kVencFrameMetaSize; j < bytes; ++j) {
        blob[j] = static_cast<uint8_t>((index * 31 + j) & 0xff);
    }
}

// Returns the frame index (from pts) or -1 on a pattern/meta mismatch.
long verify_frame(const uint8_t* blob, size_t len) {
    VencFrameMeta meta;
    if (!read_frame_meta(blob, len, &meta)) {
        return -1;
    }
    for (size_t j = kVencFrameMetaSize; j < len; ++j) {
        if (blob[j] != static_cast<uint8_t>((meta.pts * 31 + j) & 0xff)) {
            return -1;
        }
    }
    return static_cast<long>(meta.pts);
}

int run_produce(const std::string& ring_name, uint32_t frames, uint32_t fps,
                size_t p_bytes, uint32_t idr_every, uint32_t settle_ms) {
    auto ring = FrameShmRing::create(ring_name);
    if (!ring) {
        std::fprintf(stderr, "create '%s': %s\n", ring_name.c_str(),
                     ring.error.c_str());
        return 1;
    }
    // The waybeam-link TX lazy-attaches at a 500 ms cadence; give it time to
    // find the ring so the head of the stream isn't dropped ring-full.
    std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
    const auto period =
        std::chrono::microseconds(fps > 0 ? 1000000 / fps : 0);
    std::vector<uint8_t> blob;
    uint32_t written = 0;
    auto next = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < frames; ++i) {
        const bool idr = idr_every > 0 && (i % idr_every) == 0;
        fill_frame(blob, i, idr ? p_bytes * 4 : p_bytes, idr);
        if ((*ring.value)->write_frame(blob.data(), blob.size())) {
            ++written;
        }
        next += period;
        std::this_thread::sleep_until(next);
    }
    const FrameShmRing::Stats& st = (*ring.value)->stats();
    std::printf("producer frames=%u bytes=%llu full_drop=%llu "
                "oversize_drop=%llu\n",
                written,
                static_cast<unsigned long long>(st.frame_bytes),
                static_cast<unsigned long long>(st.full_drops),
                static_cast<unsigned long long>(st.oversize_drops));
    // Give the TX consumer time to drain the tail before the ring unlinks.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // Ring-full drops are venc never-block semantics (§15.4), not a bench
    // failure; oversize means the tool was misconfigured.
    return st.oversize_drops == 0 ? 0 : 1;
}

int run_consume(const std::string& ring_name, uint32_t min_frames,
                uint32_t idle_ms, uint32_t total_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(total_ms);
    std::unique_ptr<FrameShmRing> ring;
    while (!ring && std::chrono::steady_clock::now() < deadline) {
        auto r = FrameShmRing::attach(ring_name);
        if (r) {
            ring = std::move(*r.value);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (!ring) {
        std::fprintf(stderr, "attach '%s': timed out\n", ring_name.c_str());
        return 1;
    }
    std::vector<uint8_t> buf(kFrameRingDefaultSlotSize);
    uint64_t frames = 0;
    uint64_t bad = 0;
    auto last_frame = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        const long n = ring->read_frame(buf.data(), buf.size());
        if (n > 0) {
            if (verify_frame(buf.data(), static_cast<size_t>(n)) < 0) {
                ++bad;
            } else {
                ++frames;
            }
            last_frame = std::chrono::steady_clock::now();
            continue;
        }
        if (n < 0) {
            ++bad;
            continue;
        }
        if (frames > 0 && std::chrono::steady_clock::now() - last_frame >
                              std::chrono::milliseconds(idle_ms)) {
            break;  // stream went quiet — the producer is done
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("consumer frames=%llu bad=%llu\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(bad));
    return (frames >= min_frames && bad == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if ((argc == 7 || argc == 8) && std::strcmp(argv[1], "produce") == 0) {
        const uint32_t settle_ms =
            argc == 8 ? static_cast<uint32_t>(std::atol(argv[7])) : 1500;
        return run_produce(argv[2],
                           static_cast<uint32_t>(std::atol(argv[3])),
                           static_cast<uint32_t>(std::atol(argv[4])),
                           static_cast<size_t>(std::atol(argv[5])),
                           static_cast<uint32_t>(std::atol(argv[6])),
                           settle_ms);
    }
    if (argc == 6 && std::strcmp(argv[1], "consume") == 0) {
        return run_consume(argv[2],
                           static_cast<uint32_t>(std::atol(argv[3])),
                           static_cast<uint32_t>(std::atol(argv[4])),
                           static_cast<uint32_t>(std::atol(argv[5])));
    }
    std::fprintf(stderr,
                 "usage: %s produce <ring> <frames> <fps> <p_bytes> "
                 "<idr_every> [settle_ms]\n"
                 "       %s consume <ring> <min_frames> <idle_ms> <total_ms>\n",
                 argv[0], argv[0]);
    return 2;
}
