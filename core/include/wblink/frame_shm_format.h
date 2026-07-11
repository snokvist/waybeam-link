// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: venc_frame_ring shared-memory format (PROTOCOL.md §15.4).
//
// Byte-for-byte mirror of the producer's canonical header
// waybeam_venc/include/venc_frame_ring.h. This file carries ONLY the pure
// format (layout constants + the 8-byte metadata prefix and helpers to read
// it) — no mmap, no atomics, no futex. The live ring (io/frame_shm.cpp) does
// the I/O and memory-ordered index access; it reuses these constants so the
// two sides cannot drift.
//
// Same-host only: all fields are native-endian (the ring is a same-SoC SPSC
// handoff between waybeam_venc and waybeam-link).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wblink {

// §15.4 ring header.
inline constexpr uint32_t kFrameRingMagic = 0x5646524D;  // "VFRM"
inline constexpr uint32_t kFrameRingVersion = 1;
inline constexpr size_t kFrameRingHeaderSize = 192;  // 3x 64-byte cache lines

// Byte offsets within the 192-byte ring header (line 0 = immutable config,
// line 1 = producer-owned, line 2 = consumer-owned). io/ reads/writes the
// index words with acquire/release; the config words are set once at create.
inline constexpr size_t kFrHdrMagic = 0;           // u32
inline constexpr size_t kFrHdrVersion = 4;         // u32
inline constexpr size_t kFrHdrSlotCount = 8;       // u32 (power of two)
inline constexpr size_t kFrHdrSlotDataSize = 12;   // u32 (meta + frame bytes)
inline constexpr size_t kFrHdrTotalSize = 16;      // u32
inline constexpr size_t kFrHdrEpoch = 20;          // u32
inline constexpr size_t kFrHdrInitComplete = 24;   // u32 (1 once published)
inline constexpr size_t kFrHdrWriteIdx = 64;       // u64 (producer, line 1)
inline constexpr size_t kFrHdrFutexSeq = 72;       // u32
inline constexpr size_t kFrHdrReadIdx = 128;       // u64 (consumer, line 2)
inline constexpr size_t kFrHdrConsumerWaiting = 136;  // u32

// Per-slot layout: u32 length prefix + data[], stride aligned to 8 bytes.
// data[] = [VencFrameMeta (8 B)][Annex-B frame bytes].
inline constexpr size_t kFrameSlotLenPrefix = 4;

// Default geometry (matches the producer's defaults): 16 x 512 KB ~= 8 MB.
inline constexpr uint32_t kFrameRingDefaultSlots = 16;
inline constexpr uint32_t kFrameRingDefaultSlotSize = 512 * 1024;

// §15.4 VencFrameMeta — 8-byte prefix on every frame blob.
inline constexpr size_t kVencFrameMetaSize = 8;
inline constexpr uint8_t kFrameCodecH265 = 0x01;
inline constexpr uint8_t kFrameFlagIdr = 0x01;  // VencFrameMeta.flags bit 0

struct VencFrameMeta {
    uint32_t pts = 0;       // encoder capture timestamp (SDK units), truncated
    uint8_t codec = 0;      // kFrameCodecH265
    uint8_t flags = 0;      // bit 0 = IDR
    uint16_t reserved = 0;  // must be 0
};

// Read the 8-byte metadata prefix from a frame blob (native-endian, same-host).
// Returns false if the blob is too short to hold the prefix.
inline bool read_frame_meta(const uint8_t* blob, size_t len, VencFrameMeta* out) {
    if (blob == nullptr || out == nullptr || len < kVencFrameMetaSize) {
        return false;
    }
    std::memcpy(&out->pts, blob + 0, 4);
    out->codec = blob[4];
    out->flags = blob[5];
    std::memcpy(&out->reserved, blob + 6, 2);
    return true;
}

// True if the blob's metadata prefix marks an IDR frame (§4.1 ARQ input).
inline bool frame_blob_is_idr(const uint8_t* blob, size_t len) {
    VencFrameMeta m;
    return read_frame_meta(blob, len, &m) && (m.flags & kFrameFlagIdr) != 0;
}

}  // namespace wblink
