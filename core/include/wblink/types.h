// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: protocol constants and identity types (PROTOCOL.md §0–3).
#pragma once

#include <cstddef>
#include <cstdint>

namespace wblink {

// §3.1 — wire magic, transmitted big-endian as bytes 0x57 0x42 ("WB").
inline constexpr uint16_t kMagic = 0x5742;
// §3.1 — version nibble carried in the high half of ver_type.
inline constexpr uint8_t kProtocolVersion = 0x0;

// §3.1 packet types (ver_type low nibble).
enum class PacketType : uint8_t {
    kData = 0x1,
    kNack = 0x2,
    kLinkReport = 0x3,
    kHeartbeat = 0x4,
    kCsa = 0x5,
};

// §3.4 stream-type registry. Values 0x10–0xEF are user/build-defined,
// 0xF0–0xFF reserved — both handled as raw u8, no enum entries.
namespace stream_type {
inline constexpr uint8_t kUnknown = 0x00;
inline constexpr uint8_t kRtp = 0x01;
inline constexpr uint8_t kTelemetry = 0x02;
inline constexpr uint8_t kControl = 0x03;
}  // namespace stream_type

// §3.2 data_flags bits.
namespace data_flags {
inline constexpr uint8_t kEndOfBlock = 0x01;
inline constexpr uint8_t kArq = 0x02;
inline constexpr uint8_t kRetransmit = 0x04;
inline constexpr uint8_t kFecRepair = 0x08;
inline constexpr uint8_t kCsaArmed = 0x10;
}  // namespace data_flags

// Exact wire sizes (§3.1–3.8, §11.1).
inline constexpr size_t kCommonPrefixSize = 11;
inline constexpr size_t kDataHeaderSize = 26;
inline constexpr size_t kNackFixedSize = 23;
inline constexpr size_t kLinkReportSize = 39;
inline constexpr size_t kHeartbeatSize = 11;
inline constexpr size_t kCsaSize = 32;

// §3.2 — 26 B header on a ~1450 B usable MPDU.
inline constexpr uint16_t kMaxDataPayload = 1424;

// §3.5 — target_stream_id value meaning node-scope (RF health is per-link).
inline constexpr uint8_t kNodeScopeStreamId = 0xFF;
// §3.5 — probe_per value meaning "no probe ran this interval".
inline constexpr uint16_t kNoProbe = 0xFFFF;

// §2 — all per-stream RX state is keyed by the full tuple.
struct StreamKey {
    uint16_t originator = 0;
    uint32_t session_id = 0;
    uint8_t stream_id = 0;
    friend bool operator==(const StreamKey&, const StreamKey&) = default;
};

}  // namespace wblink
