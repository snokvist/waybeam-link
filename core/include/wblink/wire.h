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
    kUnknownType,     // type nibble not in {1..7}
    kTruncated,       // shorter than the type's fixed header
    kLengthMismatch,  // buffer length disagrees with the declared/fixed size
    kInvalidField,    // reserved bits or structurally invalid fixed field
};

// index 0 = error; otherwise one decoded packet.
using Decoded = std::variant<DecodeError, DataView, NackView, LinkReport,
                             Heartbeat, CsaPacket, RecoveryRequest,
                             JsccFeedback>;

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
size_t encode_heartbeat(const Heartbeat& pkt, uint8_t* out, size_t cap);
size_t encode_csa(const CsaPacket& pkt, uint8_t* out, size_t cap);
size_t encode_recovery_request(const RecoveryRequest& pkt, uint8_t* out,
                               size_t cap);
size_t encode_jscc_feedback(const JsccFeedback& pkt, uint8_t* out, size_t cap);

}  // namespace wblink
