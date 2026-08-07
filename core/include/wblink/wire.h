// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: wire header codec (PROTOCOL.md §3, §11.1).
//
// Every multi-byte field is big-endian (§0). Encoders write into a caller
// buffer and return bytes written (0 = capacity/argument error). decode()
// validates structure and returns either a typed packet or a DecodeError —
// no exceptions, no allocation; DATA payload and NACK bitmap are returned as
// views into the caller's buffer, valid only as long as that buffer.
//
// Policy checks (bitmap popcount clamp §5.3/§13, the 1424 B payload budget
// §3.2/§5.1, the plausible-forward clamp §6.6) live in the consumers, not
// here — the codec answers only "is this a structurally valid v0 packet".
#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>

#include "wblink/types.h"

namespace wblink {

// §3.1 common prefix — describes the SENDER of the frame.
struct CommonPrefix {
    uint16_t originator = 0;
    uint16_t destination = 0;  // advisory; 0x0000 = broadcast
    uint32_t session_id = 0;
    friend bool operator==(const CommonPrefix&, const CommonPrefix&) = default;
};

// §3.2 DATA header (fixed fields; payload handled separately).
struct DataHeader {
    CommonPrefix prefix;
    uint8_t stream_id = 0;
    uint8_t stream_type = 0;
    uint32_t seq = 0;
    uint32_t block_id = 0;
    uint8_t data_flags = 0;
    uint8_t active_profile = 0;
    uint8_t table_version = 0;
    friend bool operator==(const DataHeader&, const DataHeader&) = default;
};

// Decoded DATA packet: header + a view into the decode buffer (no copy).
struct DataView {
    DataHeader hdr;
    const uint8_t* payload = nullptr;  // nullptr iff payload_len == 0
    uint16_t payload_len = 0;
};

// §3.3 NACK fixed part — prefix = the asking RX; target = the TX repaired.
struct NackHeader {
    CommonPrefix prefix;
    uint16_t target_originator = 0;
    uint32_t target_session = 0;
    uint8_t target_stream_id = 0;
    uint32_t base_seq = 0;
    friend bool operator==(const NackHeader&, const NackHeader&) = default;
};

// Decoded NACK: fixed part + SACK bitmap view (bit i => base_seq+i missing).
struct NackView {
    NackHeader hdr;
    const uint8_t* bitmap = nullptr;  // nullptr iff bitmap_len == 0
    uint8_t bitmap_len = 0;
};

// §3.5 LINK_REPORT (fixed 39 bytes).
struct LinkReport {
    CommonPrefix prefix;
    uint16_t target_originator = 0;
    uint32_t target_session = 0;
    uint8_t target_stream_id = 0;  // 0xFF = node-scope
    uint32_t report_epoch = 0;
    uint8_t table_version = 0;
    int8_t rssi_best = 0;   // dBm
    int8_t rssi_mean = 0;   // dBm
    uint16_t loss_postdiv_prearq = 0;  // ‰, post-diversity pre-ARQ (§3.7)
    uint32_t uniq = 0;
    uint32_t diversity = 0;
    uint8_t adapters = 0;
    uint16_t probe_per = kNoProbe;  // ‰; 0xFFFF = no probe
    uint8_t recommended_prof = 0;
    friend bool operator==(const LinkReport&, const LinkReport&) = default;
};

// §3.8 HEARTBEAT — the common prefix alone, exactly 11 bytes.
struct Heartbeat {
    CommonPrefix prefix;
    friend bool operator==(const Heartbeat&, const Heartbeat&) = default;
};

// §3.15 SELECTOR_STATE — craft-owned, ground-advisory summary.
struct SelectorState {
    CommonPrefix prefix;
    uint8_t table_version = 0;
    uint8_t active_profile = 0;
    uint8_t safe_floor_profile = 0;
    uint8_t ceiling_profile = 0;
    uint8_t lockout_profile = 0xFF;
    uint8_t state_flags = 0;
    uint8_t lockout_strikes = 0;
    uint16_t remaining_ms = 0;
    uint8_t transition_reason = 0;
    uint16_t loss_window_milli = 0;
    uint8_t lockout_active_mask = 0;
    uint8_t lockout_latched_mask = 0;
    uint16_t loss_ewma_milli = 0;
    uint32_t loss_uniq = 0;
    uint8_t loss_score = 0;
    // §3.15a — meaningful only when state_flags has kHolderPresent.
    uint16_t report_latch_holder = 0;
    // §10.6 (Pass 120) — meaningful only when state_flags has kCalibPresent:
    // bits 0-1 state {0 idle, 1 running, 2 done, 3 failed}, bits 2-4 rung,
    // bits 5-7 reserved-zero; and the CRC-8 artifact hash (0 = none).
    uint8_t calib_word = 0;
    uint8_t calib_fingerprint = 0;
    friend bool operator==(const SelectorState&, const SelectorState&) = default;
};

// §3.16 (Pass 153) CALIBRATION family, subtype 0x01 PROBE — one numbered
// frame of a counted dwell burst, zero-padded on the wire to the negotiated
// §9.3a budget so a probe is byte-equivalent on air to a full DATA packet.
// The padding is validated as a range and not surfaced: a decoded view
// carries the fixed fields plus the observed wire length.
struct CalibProbe {
    CommonPrefix prefix;
    uint32_t run_id = 0;
    uint16_t dwell_id = 0;  // starts at 1, strictly increasing within a run
    uint16_t seq = 0;       // 1..count
    uint16_t count = 0;     // the dwell's full burst size, on every probe
    uint16_t wire_len = 0;  // observed frame length incl. padding (decode-only)
    friend bool operator==(const CalibProbe&, const CalibProbe&) = default;
};

// §3.16 (Pass 153) CALIBRATION family, subtype 0x02 TALLY — the dwell's one
// receipt. Unauthenticated for the Pass 131 reasons (bounded blast radius,
// §3.16 trust paragraph); idempotent by (run_id, dwell_id).
struct CalibTally {
    CommonPrefix prefix;
    uint32_t run_id = 0;
    uint16_t dwell_id = 0;
    uint16_t received = 0;  // distinct seq observed, post-diversity
    // §3.16: two's-complement i32 WIRE IMAGE of the per-dwell RSSI sum.
    // Held unsigned so arithmetic is modulo 2^32 by construction.
    uint32_t rssi_sum_dbm = 0;
    uint8_t rx_mcs = kUplinkRxMcsUnknown;  // delivered-rung cross-check
    uint8_t adapter_fingerprint = 0;       // receiver adapter identity (Pass 146)
    friend bool operator==(const CalibTally&, const CalibTally&) = default;
};

// §3.16: an 0xF frame whose subtype this build does not know. Deliberately a
// decoded value, not a DecodeError — a mixed-version pair must degrade to
// "calibration unavailable" while the frame still counts as valid RX for
// liveness purposes (§11.4 follower).
struct CalibUnknown {
    CommonPrefix prefix;
    uint8_t subtype = 0;
    friend bool operator==(const CalibUnknown&, const CalibUnknown&) = default;
};

// §3.9 RECOVERY_REQUEST — RX asks the exact TX session to bootstrap a stream.
struct RecoveryRequest {
    CommonPrefix prefix;
    uint16_t target_originator = 0;
    uint32_t target_session = 0;
    uint8_t target_stream_id = 0;
    friend bool operator==(const RecoveryRequest&, const RecoveryRequest&) = default;
};

// §3.10 JSCC_FEEDBACK (fixed 37 bytes).
struct JsccFeedback {
    CommonPrefix prefix;
    uint16_t target_originator = 0;
    uint32_t target_session = 0;
    uint8_t target_stream_id = 0;
    uint32_t feedback_epoch = 0;
    uint16_t repair_demand_permille = 0;
    uint32_t rtt_p95_us = 0;
    uint16_t repair_samples = 0;
    uint16_t rtt_samples = 0;
    uint8_t valid_flags = 0;
    uint32_t observed_block_id = 0;
    friend bool operator==(const JsccFeedback&, const JsccFeedback&) = default;
};

// §3.11 CACHE_STATUS (fixed 29 bytes) — sender = the cache node.
struct CacheStatus {
    CommonPrefix prefix;
    uint16_t target_originator = 0;
    uint32_t target_session = 0;
    uint8_t target_stream_id = 0;
    uint32_t oldest_block = 0;
    uint32_t newest_block = 0;
    uint16_t rx_health_permille = 0;  // 0–1000
    uint8_t capability_flags = 0;     // §3.11; unknown bits are a decode error
    friend bool operator==(const CacheStatus&, const CacheStatus&) = default;
};

// §3.11 CACHE_REQUEST fixed part — sender = the aggregator.
struct CacheRequestHeader {
    CommonPrefix prefix;
    uint16_t target_originator = 0;
    uint32_t target_session = 0;
    uint8_t target_stream_id = 0;
    uint16_t target_cache = 0;  // originator of the ONE cache addressed
    uint32_t request_id = 0;
    uint32_t block_id = 0;
    uint16_t window_len = 0;  // k, 1–256
    uint8_t max_symbols = 0;  // >= 1
    friend bool operator==(const CacheRequestHeader&,
                           const CacheRequestHeader&) = default;
};

// Decoded CACHE_REQUEST: fixed part + two bitmap views (§3.11).
// missing_sources is ceil(k/8) bytes (bit i => source i absent);
// repair_have bit r => repair_idx r already held by the requester.
struct CacheRequestView {
    CacheRequestHeader hdr;
    const uint8_t* missing_sources = nullptr;
    const uint8_t* repair_have = nullptr;  // nullptr iff repair_have_len == 0
    uint8_t repair_have_len = 0;
};

// Decoded CACHE_REPLY: envelope + ONE verbatim §3.2 DATA packet as a view.
// The wrapped bytes are revalidated through decode() by the consumer (§14.3).
struct CacheReplyView {
    CommonPrefix prefix;
    uint32_t request_id = 0;
    const uint8_t* wrapped = nullptr;
    uint16_t wrapped_len = 0;
};

// §3.12 ANNOUNCE (type 0xB, fixed 30 bytes) — a craft's pairing beacon.
// Unauthenticated advertisement (no MAC); the psk is a rendezvous token, not a
// secret (§11.4a). Node-scoped like HEARTBEAT: never creates per-stream state.
struct Announce {
    CommonPrefix prefix;
    uint8_t flags = 0;         // §3.12: kClaimed, kPskPresent
    uint16_t claimed_by = 0;   // originator of the binding ground, else 0 (advisory)
    uint8_t psk[kAnnouncePskSize] = {};  // token when kPskPresent, else all-zero
    friend bool operator==(const Announce&, const Announce&) = default;
};

// §3.13 CACHE_ASSIGN (fixed 23 bytes) — Ethernet-only receiver→cache
// selection command. target_bw uses the §11 encoding (0/1/2).
struct CacheAssign {
    CommonPrefix prefix;
    uint16_t target_cache = 0;
    uint16_t target_originator = 0;
    uint32_t assignment_epoch = 0;
    uint16_t target_chan = 0;
    uint8_t target_bw = 0;
    uint8_t target_net_id = 0;
    friend bool operator==(const CacheAssign&, const CacheAssign&) = default;
};

// §3.14 VEHICLE_CMD (fixed 23 bytes) — ground→craft runtime command riding
// the §11 machinery; the craft's ACK is the same packet echoed back with
// vcmd_flags::kAck, re-MAC'd under its own prefix. cmd_mac is carried opaque
// here; HMAC computation/verification is the §11.7 engine's job.
struct VehicleCmd {
    CommonPrefix prefix;
    uint32_t cmd_nonce = 0;
    uint8_t cmd_seq = 0;    // copy counter N..1 (diagnostics only)
    uint8_t cmd_flags = 0;  // vcmd_flags
    uint8_t cmd_id = 0;
    uint8_t cmd_arg = 0;    // 0..kVcmdMaxArg (§3.14 Pass 68); full u8 for
                            // cmd_id kMode (§15.5 catalog index, Pass 105)
    uint32_t cmd_mac = 0;
    friend bool operator==(const VehicleCmd&, const VehicleCmd&) = default;
};

// §11.1 CSA (fixed 32 bytes). csa_mac is carried opaque here; HMAC
// computation/verification is the CSA engine's job (§11.4, build step 10).
struct CsaPacket {
    CommonPrefix prefix;
    uint32_t csa_nonce = 0;
    uint8_t csa_seq = 0;         // copy counter N..1
    uint16_t target_chan = 0;    // center freq MHz
    uint8_t target_bw = 0;       // 0=20, 1=40, 2=80
    uint8_t retune_class = 0;    // 0=fast intra-band, 1=cross-band
    uint16_t dt_to_switch_ms = 0;
    uint16_t t_revert_ms = 0;
    uint16_t prev_chan = 0;
    uint8_t prev_bw = 0;
    uint8_t power_intent = 0;
    uint32_t csa_mac = 0;
    friend bool operator==(const CsaPacket&, const CsaPacket&) = default;
};

enum class DecodeError : uint8_t {
    kTooShort,        // shorter than the common prefix
    kBadMagic,        // first two bytes are not 0x57 0x42
    kBadVersion,      // version nibble != 0
    kUnknownType,     // type nibble is not registered in §3.1
    kTruncated,       // shorter than the type's fixed header
    kLengthMismatch,  // buffer length disagrees with the declared/fixed size
    kInvalidField,    // reserved bits or structurally invalid fixed field
};

// index 0 = error; otherwise one decoded packet.
using Decoded = std::variant<DecodeError, DataView, NackView, LinkReport,
                             Heartbeat, CsaPacket, RecoveryRequest,
                             JsccFeedback, CacheStatus, CacheRequestView,
                             CacheReplyView, Announce, CacheAssign, VehicleCmd,
                             SelectorState, CalibProbe, CalibTally,
                             CalibUnknown>;

// Strict-length decode of one frame (devourer hands us the exact 802.11 MAC
// payload boundary — trailing bytes are an error, not padding).
Decoded decode(const uint8_t* buf, size_t len);

// Encoders. Return total bytes written, or 0 if cap is too small or an
// argument is inconsistent (e.g. payload == nullptr with payload_len > 0).
size_t encode_data(const DataHeader& hdr, const uint8_t* payload,
                   uint16_t payload_len, uint8_t* out, size_t cap);
size_t encode_nack(const NackHeader& hdr, const uint8_t* bitmap,
                   uint8_t bitmap_len, uint8_t* out, size_t cap);
size_t encode_link_report(const LinkReport& pkt, uint8_t* out, size_t cap);

// Rewrite `report_epoch` in an already-encoded LINK_REPORT frame. §3.5 says
// the epoch advances once per EMITTED report, and §10.7 divides by the craft's
// delta of exactly this field — so an epoch burned on a frame the radio never
// took is phantom loss on the ground's seek. Reports can be built well before
// they are injected (§7.2 holds a batch for the craft's quiet gap) and can be
// dropped in between (no uplink adapter, TX queue full), so the number is
// stamped at the radio call rather than at build. LINK_REPORT carries no MAC,
// so this is a plain field write. Returns false on a short/absent buffer.
bool link_report_stamp_epoch(uint8_t* frame, size_t len, uint32_t epoch);
size_t encode_heartbeat(const Heartbeat& pkt, uint8_t* out, size_t cap);
size_t encode_selector_state(const SelectorState& pkt, uint8_t* out,
                             size_t cap);
size_t encode_csa(const CsaPacket& pkt, uint8_t* out, size_t cap);
size_t encode_vehicle_cmd(const VehicleCmd& pkt, uint8_t* out, size_t cap);
// §3.16 PROBE: writes the 22 fixed bytes and zero-pads to pad_to (clamped to
// [kCalibProbeFixedSize, mtu_tier::kHighBudget]); pkt.wire_len is ignored.
size_t encode_calib_probe(const CalibProbe& pkt, uint16_t pad_to, uint8_t* out,
                          size_t cap);
size_t encode_calib_tally(const CalibTally& pkt, uint8_t* out, size_t cap);
size_t encode_announce(const Announce& pkt, uint8_t* out, size_t cap);
size_t encode_cache_assign(const CacheAssign& pkt, uint8_t* out, size_t cap);
size_t encode_recovery_request(const RecoveryRequest& pkt, uint8_t* out,
                               size_t cap);
size_t encode_jscc_feedback(const JsccFeedback& pkt, uint8_t* out, size_t cap);
size_t encode_cache_status(const CacheStatus& pkt, uint8_t* out, size_t cap);
// missing_sources must be ceil(hdr.window_len / 8) bytes.
size_t encode_cache_request(const CacheRequestHeader& hdr,
                            const uint8_t* missing_sources,
                            const uint8_t* repair_have,
                            uint8_t repair_have_len, uint8_t* out, size_t cap);
size_t encode_cache_reply(const CommonPrefix& prefix, uint32_t request_id,
                          const uint8_t* wrapped, uint16_t wrapped_len,
                          uint8_t* out, size_t cap);

}  // namespace wblink
