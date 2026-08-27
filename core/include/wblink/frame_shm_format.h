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
// v2 (venc 0.69.0): offset 88 changed meaning -- it carried throttle_permille,
// the producer's self-imposed bitrate clamp, and now carries low_water_slots,
// the raw ring occupancy the clamp was reacting to. The two have OPPOSITE
// polarity (1000 was healthy; a HIGH slot count is now the unhealthy end), so
// the producer bumped the version deliberately rather than renaming in place:
// attach() refuses a mismatch, which turns a silent misread into a loud one.
inline constexpr uint32_t kFrameRingVersion = 2;
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
inline constexpr size_t kFrHdrHealthMagic = 76;    // u32 ("VHLT" when valid)
inline constexpr size_t kFrHdrFullDrops = 80;      // u64 (producer cumulative)
// u16, in SLOTS (not a fraction of slot_count). The healthy band is <= 1: the
// producer samples just after writing, so a consumer that is keeping up still
// leaves one frame queued. Whether a fraction round-trips that 1 depends on the
// geometry -- at the 8 slots venc creates it does, at 16 it does not (62.5
// truncates to 62, back to 0) -- and the header does not fix slot_count.
inline constexpr size_t kFrHdrLowWaterSlots = 88;
// u64, producer cumulative: frames the PRODUCER discarded for a reason other
// than a full ring (an access unit it could not build at all). Kept apart from
// full_drops because the two demand opposite responses -- full_drops is
// congestion this node is causing and slowing down helps, other_drops is not
// congestion and slowing down fixes nothing.
inline constexpr size_t kFrHdrOtherDrops = 96;
inline constexpr size_t kFrHdrReadIdx = 128;       // u64 (consumer, line 2)
inline constexpr size_t kFrHdrConsumerWaiting = 136;  // u32
inline constexpr uint32_t kFrameHealthMagic = 0x56484C54;  // "VHLT"

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
inline constexpr uint8_t kFrameFlagGdr = 0x02;  // rolling intra stripe active
inline constexpr uint8_t kFrameFlagEnhance = 0x04;  // SVC-T droppable layer
// Set by the RX side (§6.3b), never by an encoder: this frame was rebuilt with
// synthesized replacement slices, so it is decodable but NOT what was sent.
// Consumers that would otherwise treat it as a trustworthy picture — a
// recorder writing a seek point, anything caching parameter sets — must not.
// Absence means "not known to be salvaged", never a guarantee of integrity:
// a producer predating this bit leaves it clear.
inline constexpr uint8_t kFrameFlagSalvaged = 0x08;
inline constexpr uint8_t kFrameFlagsKnown =
    kFrameFlagIdr | kFrameFlagGdr | kFrameFlagEnhance | kFrameFlagSalvaged;

struct VencFrameMeta {
    uint32_t pts = 0;       // encoder capture timestamp (SDK units), truncated
    uint8_t codec = 0;      // kFrameCodecH265
    uint8_t flags = 0;      // kFrameFlag* bitmap
    uint8_t gdr_pos = 0;    // 0-based position in GDR cycle
    uint8_t gdr_len = 0;    // GDR cycle length; 0 while inactive
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
    out->gdr_pos = blob[6];
    out->gdr_len = blob[7];
    return true;
}

// True if the blob's metadata prefix marks an IDR frame (§4.1 ARQ input).
inline bool frame_blob_is_idr(const uint8_t* blob, size_t len) {
    VencFrameMeta m;
    return read_frame_meta(blob, len, &m) && (m.flags & kFrameFlagIdr) != 0;
}

}  // namespace wblink
