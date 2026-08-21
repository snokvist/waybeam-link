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
    kRecoveryRequest = 0x6,
    kJsccFeedback = 0x7,
    kCacheStatus = 0x8,
    kCacheRequest = 0x9,
    kCacheReply = 0xA,
    kAnnounce = 0xB,  // §3.12 craft pairing beacon
    kCacheAssign = 0xC,  // §3.13 receiver-owned cache following
    kVehicleCmd = 0xD,  // §3.14 remote vehicle command (rides §11 machinery)
    kSelectorState = 0xE,  // §3.15 craft-owned adaptive state summary
    kExtended = 0xF,  // §3.16 EXTENDED type — first payload byte is the
                      // extended type ID registry (Pass 153)
};

// §3.4 stream-type registry. Values 0x10–0xEF are user/build-defined,
// 0xF0–0xFF reserved — both handled as raw u8, no enum entries.
namespace stream_type {
inline constexpr uint8_t kUnknown = 0x00;
inline constexpr uint8_t kRtp = 0x01;
inline constexpr uint8_t kTelemetry = 0x02;
inline constexpr uint8_t kControl = 0x03;
inline constexpr uint8_t kAudio = 0x04;  // §3.4 Opus/RTP audio; best-effort, non-ARQ
}  // namespace stream_type

// §3.2 data_flags bits.
namespace data_flags {
inline constexpr uint8_t kEndOfBlock = 0x01;
inline constexpr uint8_t kArq = 0x02;
inline constexpr uint8_t kRetransmit = 0x04;
inline constexpr uint8_t kFecRepair = 0x08;
inline constexpr uint8_t kCsaArmed = 0x10;
inline constexpr uint8_t kPframeArq = 0x20;
}  // namespace data_flags

// §3.12 ANNOUNCE flags. Unknown bits are a decode error (like cache flags).
namespace announce_flags {
inline constexpr uint8_t kClaimed = 0x01;      // craft is bound to a command source
inline constexpr uint8_t kPskPresent = 0x02;   // the 16-byte token is populated
inline constexpr uint8_t kKnownMask = kClaimed | kPskPresent;
}  // namespace announce_flags

// §3.12/§11.4a — session pairing token width (also the CSA HMAC key when auto).
inline constexpr size_t kAnnouncePskSize = 16;

// §3.14 VEHICLE_CMD cmd_flags bits. Unknown bits are a decode error.
namespace vcmd_flags {
inline constexpr uint8_t kAck = 0x01;       // craft→ground echo
inline constexpr uint8_t kRejected = 0x02;  // echo only: understood, won't do
inline constexpr uint8_t kKnownMask = kAck | kRejected;
}  // namespace vcmd_flags

// §11.7 command registry. 0x0B–0x1F reserved.
namespace vcmd_id {
inline constexpr uint8_t kArq = 0x01;        // arg 0=off 1=on
inline constexpr uint8_t kSelector = 0x02;   // arg 0=run 1=freeze (§9.7 pin)
inline constexpr uint8_t kFpsLadder = 0x03;  // arg 0=off 1=on (§9.11)
inline constexpr uint8_t kFpsSelect = 0x04;   // arg = preset index (Pass 71)
inline constexpr uint8_t kResolution = 0x05;  // arg = preset index (staged)
inline constexpr uint8_t kFraming = 0x06;     // arg = preset index (staged)
inline constexpr uint8_t kMode = 0x07;        // arg = §15.5 catalog index (P105)
inline constexpr uint8_t kCalibrate = 0x08;   // arg 0=abort 1=start (§10.6 P120)
inline constexpr uint8_t kMtuTier = 0x09;     // arg 0/1/2 (§9.3a Pass 122)
inline constexpr uint8_t kTxPower = 0x0A;     // arg = §10.3 preset idx (P135)

// Typed-only commands have validation/state bookkeeping that the generic
// §15.5 vehicle-command endpoint cannot safely reproduce.
inline constexpr bool typed_endpoint_only(uint8_t id) {
    return id == kMtuTier;
}
}  // namespace vcmd_id

// §3.14 — every command is enable/disable or a ≤5-choice enum (Pass 68), EXCEPT
// 0x07 MODE, whose arg indexes the open-ended §15.5 catalog and rides the full
// u8 (Pass 105). The structural cap below is thus cmd_id-dependent: it gates
// every command but MODE. Helper keeps the exception in one place.
inline constexpr uint8_t kVcmdMaxArg = 4;
inline constexpr bool vcmd_arg_in_wire_range(uint8_t cmd_id, uint8_t cmd_arg) {
    return cmd_id == vcmd_id::kMode || cmd_arg <= kVcmdMaxArg;
}

enum class FrameArqMode : uint8_t { kIdrOnly, kAllFrames };

// Exact wire sizes (§3.1–3.8, §11.1).
inline constexpr size_t kCommonPrefixSize = 11;
inline constexpr size_t kDataHeaderSize = 26;
inline constexpr size_t kNackFixedSize = 23;
inline constexpr size_t kLinkReportSize = 39;
inline constexpr size_t kHeartbeatSize = 11;
inline constexpr size_t kCsaSize = 32;
inline constexpr size_t kRecoveryRequestSize = 18;
inline constexpr size_t kJsccFeedbackSize = 37;
inline constexpr size_t kCacheStatusSize = 29;
inline constexpr size_t kCacheRequestFixedSize = 32;
inline constexpr size_t kCacheReplyFixedSize = 17;
inline constexpr size_t kAnnounceSize = 30;  // §3.12: 11 prefix + 1 + 2 + 16
inline constexpr size_t kCacheAssignSize = 23;
inline constexpr size_t kVehicleCmdSize = 23;  // §3.14: MAC covers bytes 0..18
// §3.15: 36 with the §10.6 calibration word (bit4, implies bit3), 34 with
// the §3.15a holder only (bit3), 32 without either.
inline constexpr size_t kSelectorStateCalibSize = 36;
inline constexpr size_t kSelectorStateSize = 34;
inline constexpr size_t kSelectorStateLegacySize = 32;
// §3.16 (Pass 153) EXTENDED type-ID registry: the first payload byte of
// every 0xF frame. 0x00 is reserved-invalid, unknown IDs are ignored, and
// additive growth allocates new IDs here — the version nibble is reserved
// for breaking changes only. CAL_PROBE is fixed 22 bytes zero-padded to the
// negotiated §9.3a budget — the one variable-length non-DATA layout, so its
// decoder range-checks [kCalibProbeFixedSize, mtu_tier::kHighBudget].
// CAL_TALLY is exact-length.
namespace ext_type {
inline constexpr uint8_t kReservedInvalid = 0x00;
inline constexpr uint8_t kCalProbe = 0x01;
inline constexpr uint8_t kCalTally = 0x02;
inline constexpr uint8_t kLinkVerdict = 0x03;  // §3.16 Pass 159
}  // namespace ext_type
inline constexpr size_t kCalibProbeFixedSize = 22;
inline constexpr size_t kCalibTallySize = 26;
inline constexpr size_t kLinkVerdictSize = 23;  // §3.16 Pass 159, exact-length
// §3.16 (Pass 159) LINK_VERDICT values — the CAUSE, never a vendor scalar.
// 0/stale = absence of evidence, gates nothing (§9.4 Pass 160). >6 = decode
// error.
namespace link_verdict {
inline constexpr uint8_t kUnknown = 0;
inline constexpr uint8_t kNoSignal = 1;
inline constexpr uint8_t kSaturated = 2;
inline constexpr uint8_t kInterference = 3;
inline constexpr uint8_t kWeak = 4;
inline constexpr uint8_t kMarginal = 5;
inline constexpr uint8_t kHealthy = 6;
inline constexpr uint8_t kMax = 6;
}  // namespace link_verdict
// §3.16 rx_mcs sentinel: no radiotap MCS field, a non-HT rate, or no
// accepted probe yet. Mirrors io's kRxMcsUnknown, restated here because
// core/ stays dependency-free (it is vendored standalone).
inline constexpr uint8_t kUplinkRxMcsUnknown = 0xFF;

// §3.11 CACHE_STATUS capability_flags bits.
namespace cache_capability {
inline constexpr uint8_t kIpTransport = 0x01;
inline constexpr uint8_t kKnownMask = kIpTransport;
}  // namespace cache_capability

namespace jscc_feedback_flags {
inline constexpr uint8_t kRepairReady = 0x01;
inline constexpr uint8_t kRttReady = 0x02;
inline constexpr uint8_t kKnownMask = kRepairReady | kRttReady;
}  // namespace jscc_feedback_flags

namespace selector_state_flags {
inline constexpr uint8_t kActive = 0x01;
inline constexpr uint8_t kLatched = 0x02;
inline constexpr uint8_t kConflict = 0x04;
// §3.15a: the report_latch_holder field is present; also selects the wire
// length (set = kSelectorStateSize, clear = kSelectorStateLegacySize).
inline constexpr uint8_t kHolderPresent = 0x08;
// §10.6 (Pass 120): the 2-byte calibration word is present (bytes 34-35);
// set implies kHolderPresent (fields append in order).
inline constexpr uint8_t kCalibPresent = 0x10;
inline constexpr uint8_t kKnownMask =
    kActive | kLatched | kConflict | kHolderPresent | kCalibPresent;
}  // namespace selector_state_flags

// §3.2 — absolute DATA payload ceiling for buffer sizing. The v1 negotiated
// tiers stop at 3072 B, while 4096 leaves decode/storage headroom. The effective
// per-frame budget is min(Profile.max_payload, negotiated tier), §9.3a.
inline constexpr uint16_t kMaxDataPayload = 4096;
inline constexpr uint16_t kDefaultMaxPayload = 1424;  // standard-rung default

// §9.3a negotiated complete-DATA-packet budgets. Automatic is ground-local
// and resolves to one of these concrete wire tiers before transmission.
namespace mtu_tier {
inline constexpr uint8_t kDefault = 0;
inline constexpr uint8_t kMedium = 1;
inline constexpr uint8_t kHigh = 2;
inline constexpr uint16_t kMediumBudget = 2048;
inline constexpr uint16_t kHighBudget = 3072;
inline constexpr uint16_t kFecProtectionK = 16;  // §9.3a Pass 124

inline constexpr uint16_t budget(uint8_t tier) {
    return tier == kHigh ? kHighBudget
         : tier == kMedium ? kMediumBudget
                           : kDefaultMaxPayload;
}

inline constexpr bool valid(uint8_t tier) { return tier <= kHigh; }
}  // namespace mtu_tier

// §3.5 — target_stream_id value meaning node-scope (RF health is per-link).
inline constexpr uint8_t kNodeScopeStreamId = 0xFF;

// §14.1 — FEC repair subheader (precedes the coded payload when FEC_REPAIR is
// set): repair_idx u8 @0, window_len u16 @1, window_base_seq u32 @3,
// frame_len u32 @7. 11 bytes; deducted (with the 26 B header) from a rung's
// max_payload to size source/repair symbols identically (§5.1a).
inline constexpr size_t kFecRepairSubheaderSize = 11;
inline constexpr size_t kFecOffRepairIdx = 0;
inline constexpr size_t kFecOffWindowLen = 1;
inline constexpr size_t kFecOffWindowBaseSeq = 3;
inline constexpr size_t kFecOffFrameLen = 7;
// Profile validation requires max_payload >= DATA header + 32 bytes. After
// the repair subheader this leaves at least 21 coded/source bytes.
inline constexpr uint16_t kMinFrameSymbolSize =
    32 - kFecRepairSubheaderSize;

// §5.1a — frame-shm SOURCE symbols carry a 4-byte self-describing subheader
// (window_len u16 @0 = k, sym_index u16 @2 = i) before the chunk, so RX
// reassembly knows each symbol's index + the block's k without inferring from
// seq gaps (a leading-loss run can never look like a complete frame).
inline constexpr size_t kFecSourceSubheaderSize = 4;
inline constexpr size_t kFecSrcOffWindowLen = 0;
inline constexpr size_t kFecSrcOffSymIndex = 2;

// §14.1 — GF(256) capacity: a Cauchy-RS block holds at most 256 symbols.
inline constexpr uint16_t kFecMaxSymbols = 256;
// §3.5 — probe_per value meaning "no probe ran this interval".
inline constexpr uint16_t kNoProbe = 0xFFFF;
// §15.3 Pass 186 — probe_candidate_mcs value meaning "no candidate": on a TX
// the node is not probing (air.mcs_probe off, top rung, same-MCS adjacency,
// or above the §9.7 pin); on an RX the window is idle.
inline constexpr uint8_t kProbeMcsNone = 0xFF;

// §2 — all per-stream RX state is keyed by the full tuple.
struct StreamKey {
    uint16_t originator = 0;
    uint32_t session_id = 0;
    uint8_t stream_id = 0;
    friend bool operator==(const StreamKey&, const StreamKey&) = default;
};

}  // namespace wblink
