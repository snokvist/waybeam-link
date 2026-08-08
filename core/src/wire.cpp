// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/wire.h"

#include <cstring>
#include <algorithm>

#include "wblink/endian.h"

namespace wblink {

namespace {

void encode_prefix(const CommonPrefix& p, PacketType type, uint8_t* out) {
    be16_write(out + 0, kMagic);
    out[2] = static_cast<uint8_t>((kProtocolVersion << 4) |
                                  static_cast<uint8_t>(type));
    be16_write(out + 3, p.originator);
    be16_write(out + 5, p.destination);
    be32_write(out + 7, p.session_id);
}

CommonPrefix decode_prefix(const uint8_t* buf) {
    CommonPrefix p;
    p.originator = be16_read(buf + 3);
    p.destination = be16_read(buf + 5);
    p.session_id = be32_read(buf + 7);
    return p;
}

Decoded decode_data(const uint8_t* buf, size_t len) {
    if (len < kDataHeaderSize) {
        return DecodeError::kTruncated;
    }
    const uint16_t payload_len = be16_read(buf + 24);
    if (len != kDataHeaderSize + payload_len) {
        return DecodeError::kLengthMismatch;
    }
    // §3.2 absolute ceiling, enforced on the RX side too: this is the only
    // check between an off-air datagram and an RxEngine held-packet copy, and
    // the udp-broadcast backend (§16.3) accepts datagrams far above the air
    // MTU. Without it a forger holds fwd_clamp_pkts × 60 KB of RX heap.
    if (payload_len > kMaxDataPayload) {
        return DecodeError::kInvalidField;
    }
    DataView v;
    v.hdr.prefix = decode_prefix(buf);
    v.hdr.stream_id = buf[11];
    v.hdr.stream_type = buf[12];
    v.hdr.seq = be32_read(buf + 13);
    v.hdr.block_id = be32_read(buf + 17);
    v.hdr.data_flags = buf[21];
    v.hdr.active_profile = buf[22];
    v.hdr.table_version = buf[23];
    v.payload_len = payload_len;
    v.payload = payload_len > 0 ? buf + kDataHeaderSize : nullptr;
    return v;
}

Decoded decode_nack(const uint8_t* buf, size_t len) {
    if (len < kNackFixedSize) {
        return DecodeError::kTruncated;
    }
    const uint8_t bitmap_len = buf[22];
    if (len != kNackFixedSize + bitmap_len) {
        return DecodeError::kLengthMismatch;
    }
    NackView v;
    v.hdr.prefix = decode_prefix(buf);
    v.hdr.target_originator = be16_read(buf + 11);
    v.hdr.target_session = be32_read(buf + 13);
    v.hdr.target_stream_id = buf[17];
    v.hdr.base_seq = be32_read(buf + 18);
    v.bitmap_len = bitmap_len;
    v.bitmap = bitmap_len > 0 ? buf + kNackFixedSize : nullptr;
    return v;
}

Decoded decode_link_report(const uint8_t* buf, size_t len) {
    if (len < kLinkReportSize) {
        return DecodeError::kTruncated;
    }
    if (len != kLinkReportSize) {
        return DecodeError::kLengthMismatch;
    }
    LinkReport r;
    r.prefix = decode_prefix(buf);
    r.target_originator = be16_read(buf + 11);
    r.target_session = be32_read(buf + 13);
    r.target_stream_id = buf[17];
    r.report_epoch = be32_read(buf + 18);
    r.table_version = buf[22];
    r.rssi_best = static_cast<int8_t>(buf[23]);
    r.rssi_mean = static_cast<int8_t>(buf[24]);
    r.loss_postdiv_prearq = be16_read(buf + 25);
    r.uniq = be32_read(buf + 27);
    r.diversity = be32_read(buf + 31);
    r.adapters = buf[35];
    r.probe_per = be16_read(buf + 36);
    r.recommended_prof = buf[38];
    return r;
}

Decoded decode_heartbeat(const uint8_t* buf, size_t len) {
    if (len != kHeartbeatSize) {
        return DecodeError::kLengthMismatch;  // §3.8: exactly 11 bytes
    }
    Heartbeat h;
    h.prefix = decode_prefix(buf);
    return h;
}

bool selector_state_fields_valid(const SelectorState& s) {
    const bool active =
        (s.state_flags & selector_state_flags::kActive) != 0;
    const bool latched =
        (s.state_flags & selector_state_flags::kLatched) != 0;
    const bool conflict =
        (s.state_flags & selector_state_flags::kConflict) != 0;
    if ((s.state_flags & ~selector_state_flags::kKnownMask) != 0 ||
        ((latched || conflict) && !active) || s.transition_reason > 10 ||
        s.loss_window_milli > 1000 || s.loss_ewma_milli > 1000 ||
        (s.lockout_latched_mask & ~s.lockout_active_mask) != 0) {
        return false;
    }
    if (!active) {
        return s.lockout_profile == 0xFF && s.lockout_strikes == 0 &&
               s.remaining_ms == 0 && s.lockout_active_mask == 0 &&
               s.lockout_latched_mask == 0;
    }
    if (s.lockout_profile == 0xFF || s.lockout_strikes == 0 ||
        s.lockout_active_mask == 0 ||
        (latched && s.lockout_latched_mask == 0)) {
        return false;
    }
    return latched ? s.remaining_ms == 0 : s.remaining_ms > 0;
}

Decoded decode_selector_state(const uint8_t* buf, size_t len) {
    if (len < kSelectorStateLegacySize) {
        return DecodeError::kTruncated;
    }
    // §3.15a: bit3 and the length must agree. Accepting both shapes is what
    // keeps a legacy 32-byte summary decodable instead of dropping the whole
    // lockout display on a mixed-version pair.
    if (len != kSelectorStateCalibSize && len != kSelectorStateSize &&
        len != kSelectorStateLegacySize) {
        return DecodeError::kLengthMismatch;
    }
    const bool holder_present =
        (buf[16] & selector_state_flags::kHolderPresent) != 0;
    const bool calib_present =
        (buf[16] & selector_state_flags::kCalibPresent) != 0;
    // §10.6: bit4 implies bit3 (fields append in order), and each flag must
    // agree with the wire length.
    if (calib_present && !holder_present) {
        return DecodeError::kInvalidField;
    }
    const size_t expect = calib_present ? kSelectorStateCalibSize
                          : holder_present ? kSelectorStateSize
                                           : kSelectorStateLegacySize;
    if (len != expect) {
        return DecodeError::kLengthMismatch;
    }
    SelectorState s;
    s.prefix = decode_prefix(buf);
    s.table_version = buf[11];
    s.active_profile = buf[12];
    s.safe_floor_profile = buf[13];
    s.ceiling_profile = buf[14];
    s.lockout_profile = buf[15];
    s.state_flags = buf[16];
    s.lockout_strikes = buf[17];
    s.remaining_ms = be16_read(buf + 18);
    s.transition_reason = buf[20];
    s.loss_window_milli = be16_read(buf + 21);
    s.lockout_active_mask = buf[23];
    s.lockout_latched_mask = buf[24];
    s.loss_ewma_milli = be16_read(buf + 25);
    s.loss_uniq = be32_read(buf + 27);
    s.loss_score = buf[31];
    if (holder_present) {
        s.report_latch_holder = be16_read(buf + 32);
    }
    if (calib_present) {
        s.calib_word = buf[34];
        s.calib_fingerprint = buf[35];
    }
    return selector_state_fields_valid(s) ? Decoded{s}
                                          : Decoded{DecodeError::kInvalidField};
}

Decoded decode_csa(const uint8_t* buf, size_t len) {
    if (len < kCsaSize) {
        return DecodeError::kTruncated;
    }
    if (len != kCsaSize) {
        return DecodeError::kLengthMismatch;
    }
    CsaPacket c;
    c.prefix = decode_prefix(buf);
    c.csa_nonce = be32_read(buf + 11);
    c.csa_seq = buf[15];
    c.target_chan = be16_read(buf + 16);
    c.target_bw = buf[18];
    c.retune_class = buf[19];
    c.dt_to_switch_ms = be16_read(buf + 20);
    c.t_revert_ms = be16_read(buf + 22);
    c.prev_chan = be16_read(buf + 24);
    c.prev_bw = buf[26];
    c.power_intent = buf[27];
    c.csa_mac = be32_read(buf + 28);
    return c;
}

Decoded decode_recovery_request(const uint8_t* buf, size_t len) {
    if (len != kRecoveryRequestSize) {
        return len < kRecoveryRequestSize ? DecodeError::kTruncated
                                          : DecodeError::kLengthMismatch;
    }
    RecoveryRequest r;
    r.prefix = decode_prefix(buf);
    r.target_originator = be16_read(buf + 11);
    r.target_session = be32_read(buf + 13);
    r.target_stream_id = buf[17];
    return r;
}

Decoded decode_jscc_feedback(const uint8_t* buf, size_t len) {
    if (len != kJsccFeedbackSize) {
        return len < kJsccFeedbackSize ? DecodeError::kTruncated
                                      : DecodeError::kLengthMismatch;
    }
    JsccFeedback f;
    f.prefix = decode_prefix(buf);
    f.target_originator = be16_read(buf + 11);
    f.target_session = be32_read(buf + 13);
    f.target_stream_id = buf[17];
    f.feedback_epoch = be32_read(buf + 18);
    f.repair_demand_permille = be16_read(buf + 22);
    f.rtt_p95_us = be32_read(buf + 24);
    f.repair_samples = be16_read(buf + 28);
    f.rtt_samples = be16_read(buf + 30);
    f.valid_flags = buf[32];
    f.observed_block_id = be32_read(buf + 33);
    if ((f.valid_flags & ~jscc_feedback_flags::kKnownMask) != 0) {
        return DecodeError::kInvalidField;
    }
    return f;
}

Decoded decode_cache_status(const uint8_t* buf, size_t len) {
    if (len != kCacheStatusSize) {
        return len < kCacheStatusSize ? DecodeError::kTruncated
                                      : DecodeError::kLengthMismatch;
    }
    CacheStatus s;
    s.prefix = decode_prefix(buf);
    s.target_originator = be16_read(buf + 11);
    s.target_session = be32_read(buf + 13);
    s.target_stream_id = buf[17];
    s.oldest_block = be32_read(buf + 18);
    s.newest_block = be32_read(buf + 22);
    s.rx_health_permille = be16_read(buf + 26);
    s.capability_flags = buf[28];
    if (s.rx_health_permille > 1000 ||
        (s.capability_flags & ~cache_capability::kKnownMask) != 0) {
        return DecodeError::kInvalidField;
    }
    return s;
}

Decoded decode_cache_request(const uint8_t* buf, size_t len) {
    if (len < kCacheRequestFixedSize) {
        return DecodeError::kTruncated;
    }
    const uint16_t k = be16_read(buf + 28);
    const uint8_t max_symbols = buf[30];
    const uint8_t repair_have_len = buf[31];
    if (k == 0 || k > kFecMaxSymbols || max_symbols == 0 ||
        repair_have_len > 32) {
        return DecodeError::kInvalidField;
    }
    const size_t src_len = (static_cast<size_t>(k) + 7) / 8;
    if (len != kCacheRequestFixedSize + src_len + repair_have_len) {
        return DecodeError::kLengthMismatch;
    }
    CacheRequestView v;
    v.hdr.prefix = decode_prefix(buf);
    v.hdr.target_originator = be16_read(buf + 11);
    v.hdr.target_session = be32_read(buf + 13);
    v.hdr.target_stream_id = buf[17];
    v.hdr.target_cache = be16_read(buf + 18);
    v.hdr.request_id = be32_read(buf + 20);
    v.hdr.block_id = be32_read(buf + 24);
    v.hdr.window_len = k;
    v.hdr.max_symbols = max_symbols;
    v.missing_sources = buf + kCacheRequestFixedSize;
    v.repair_have_len = repair_have_len;
    v.repair_have =
        repair_have_len > 0 ? buf + kCacheRequestFixedSize + src_len : nullptr;
    return v;
}

Decoded decode_cache_reply(const uint8_t* buf, size_t len) {
    if (len < kCacheReplyFixedSize) {
        return DecodeError::kTruncated;
    }
    const uint16_t wrapped_len = be16_read(buf + 15);
    if (wrapped_len < kDataHeaderSize) {
        return DecodeError::kInvalidField;
    }
    if (len != kCacheReplyFixedSize + wrapped_len) {
        return DecodeError::kLengthMismatch;
    }
    CacheReplyView v;
    v.prefix = decode_prefix(buf);
    v.request_id = be32_read(buf + 11);
    v.wrapped = buf + kCacheReplyFixedSize;
    v.wrapped_len = wrapped_len;
    return v;
}

Decoded decode_announce(const uint8_t* buf, size_t len) {
    if (len != kAnnounceSize) {  // §3.12: exactly 30 bytes
        return len < kAnnounceSize ? DecodeError::kTruncated
                                   : DecodeError::kLengthMismatch;
    }
    const uint8_t flags = buf[11];
    if ((flags & ~announce_flags::kKnownMask) != 0) {
        return DecodeError::kInvalidField;  // reserved bits must be 0
    }
    Announce a;
    a.prefix = decode_prefix(buf);
    a.flags = flags;
    a.claimed_by = be16_read(buf + 12);
    std::memcpy(a.psk, buf + 14, kAnnouncePskSize);
    return a;
}

Decoded decode_cache_assign(const uint8_t* buf, size_t len) {
    if (len != kCacheAssignSize) {
        return len < kCacheAssignSize ? DecodeError::kTruncated
                                      : DecodeError::kLengthMismatch;
    }
    CacheAssign a;
    a.prefix = decode_prefix(buf);
    a.target_cache = be16_read(buf + 11);
    a.target_originator = be16_read(buf + 13);
    a.assignment_epoch = be32_read(buf + 15);
    a.target_chan = be16_read(buf + 19);
    a.target_bw = buf[21];
    a.target_net_id = buf[22];
    if (a.target_cache == 0 || a.target_originator == 0 ||
        a.target_chan == 0 || a.target_bw > 2) {
        return DecodeError::kInvalidField;
    }
    return a;
}

Decoded decode_extended(const uint8_t* buf, size_t len) {
    // §3.16 (Pass 153): the first payload byte is the extended type ID —
    // ONE shared dispatch point for every 0xF frame. It needs to exist
    // before it can be read; below that even ExtUnknown is unreachable.
    if (len < kCommonPrefixSize + 1) return DecodeError::kTruncated;
    const uint8_t ext_id = buf[11];
    switch (ext_id) {
        case ext_type::kReservedInvalid:
            return DecodeError::kInvalidField;  // 0x00: reserved-invalid
        case ext_type::kCalProbe: {
            // The one variable-length non-DATA type: fixed fields plus zero
            // padding to the sender's negotiated §9.3a budget, so length is a
            // RANGE check against the largest tier, not an exact size.
            if (len < kCalibProbeFixedSize) return DecodeError::kTruncated;
            if (len > mtu_tier::kHighBudget) {
                return DecodeError::kLengthMismatch;
            }
            CalibProbe pr;
            pr.prefix = decode_prefix(buf);
            pr.run_id = be32_read(buf + 12);
            pr.dwell_id = be16_read(buf + 16);
            pr.seq = be16_read(buf + 18);
            pr.count = be16_read(buf + 20);
            pr.wire_len = static_cast<uint16_t>(len);
            // §3.16: dwell_id starts at 1, seq is 1..count — zero in any of
            // them is structurally invalid, as is seq past the burst size.
            // destination MUST be non-zero: calibration is addressed, never
            // broadcast.
            if (pr.dwell_id == 0 || pr.seq == 0 || pr.count == 0 ||
                pr.seq > pr.count || pr.prefix.destination == 0) {
                return DecodeError::kInvalidField;
            }
            return pr;
        }
        case ext_type::kCalTally: {
            if (len != kCalibTallySize) {
                return len < kCalibTallySize ? DecodeError::kTruncated
                                             : DecodeError::kLengthMismatch;
            }
            CalibTally t;
            t.prefix = decode_prefix(buf);
            t.run_id = be32_read(buf + 12);
            t.dwell_id = be16_read(buf + 16);
            t.received = be16_read(buf + 18);
            t.rssi_sum_dbm = be32_read(buf + 20);
            t.rx_mcs = buf[24];
            t.adapter_fingerprint = buf[25];
            if (t.dwell_id == 0 || t.prefix.destination == 0) {
                return DecodeError::kInvalidField;
            }
            return t;
        }
        case ext_type::kLinkVerdict: {
            // §3.16 (Pass 159): exact-length, addressed, verdict 0..6.
            if (len != kLinkVerdictSize) {
                return len < kLinkVerdictSize ? DecodeError::kTruncated
                                              : DecodeError::kLengthMismatch;
            }
            LinkVerdictPkt v;
            v.prefix = decode_prefix(buf);
            v.target_originator = be16_read(buf + 12);
            v.target_session = be32_read(buf + 14);
            v.report_epoch = be32_read(buf + 18);
            v.verdict = buf[22];
            if (v.verdict > link_verdict::kMax ||
                v.prefix.destination == 0) {
                return DecodeError::kInvalidField;
            }
            return v;
        }
        default: {
            // §3.16: an unknown extended type ID is a DECODED value, not an
            // error — additive growth must degrade to "feature unavailable"
            // while the frame still counts as valid RX for liveness (§11.4).
            ExtUnknown u;
            u.prefix = decode_prefix(buf);
            u.ext_id = ext_id;
            return u;
        }
    }
}

Decoded decode_vehicle_cmd(const uint8_t* buf, size_t len) {
    if (len != kVehicleCmdSize) {
        return len < kVehicleCmdSize ? DecodeError::kTruncated
                                     : DecodeError::kLengthMismatch;
    }
    const uint8_t flags = buf[16];
    if ((flags & ~vcmd_flags::kKnownMask) != 0) {
        return DecodeError::kInvalidField;  // reserved bits must be 0
    }
    VehicleCmd c;
    c.prefix = decode_prefix(buf);
    c.cmd_nonce = be32_read(buf + 11);
    c.cmd_seq = buf[15];
    c.cmd_flags = flags;
    c.cmd_id = buf[17];
    c.cmd_arg = buf[18];
    c.cmd_mac = be32_read(buf + 19);
    // §3.14: cmd_arg is structurally 0..4 (Pass 68) for every command but 0x07
    // MODE, whose arg indexes the open-ended §15.5 catalog and rides the full
    // u8 (Pass 105). An unknown cmd_id is NOT a decode error — the §11.7 craft
    // engine answers it with REJECTED.
    if (!vcmd_arg_in_wire_range(c.cmd_id, c.cmd_arg)) {
        return DecodeError::kInvalidField;
    }
    return c;
}

}  // namespace

Decoded decode(const uint8_t* buf, size_t len) {
    if (buf == nullptr || len < 2) {
        return DecodeError::kTooShort;
    }
    if (be16_read(buf) != kMagic) {
        return DecodeError::kBadMagic;
    }
    if (len < kCommonPrefixSize) {
        return DecodeError::kTooShort;
    }
    const uint8_t version = static_cast<uint8_t>(buf[2] >> 4);
    if (version != kProtocolVersion) {
        return DecodeError::kBadVersion;
    }
    switch (static_cast<PacketType>(buf[2] & 0x0F)) {
        case PacketType::kData:
            return decode_data(buf, len);
        case PacketType::kNack:
            return decode_nack(buf, len);
        case PacketType::kLinkReport:
            return decode_link_report(buf, len);
        case PacketType::kHeartbeat:
            return decode_heartbeat(buf, len);
        case PacketType::kCsa:
            return decode_csa(buf, len);
        case PacketType::kRecoveryRequest:
            return decode_recovery_request(buf, len);
        case PacketType::kJsccFeedback:
            return decode_jscc_feedback(buf, len);
        case PacketType::kCacheStatus:
            return decode_cache_status(buf, len);
        case PacketType::kCacheRequest:
            return decode_cache_request(buf, len);
        case PacketType::kCacheReply:
            return decode_cache_reply(buf, len);
        case PacketType::kAnnounce:
            return decode_announce(buf, len);
        case PacketType::kCacheAssign:
            return decode_cache_assign(buf, len);
        case PacketType::kExtended:
            return decode_extended(buf, len);
        case PacketType::kVehicleCmd:
            return decode_vehicle_cmd(buf, len);
        case PacketType::kSelectorState:
            return decode_selector_state(buf, len);
        default:
            return DecodeError::kUnknownType;
    }
}

size_t encode_data(const DataHeader& hdr, const uint8_t* payload,
                   uint16_t payload_len, uint8_t* out, size_t cap) {
    const size_t total = kDataHeaderSize + payload_len;
    if (out == nullptr || cap < total ||
        (payload == nullptr && payload_len > 0)) {
        return 0;
    }
    encode_prefix(hdr.prefix, PacketType::kData, out);
    out[11] = hdr.stream_id;
    out[12] = hdr.stream_type;
    be32_write(out + 13, hdr.seq);
    be32_write(out + 17, hdr.block_id);
    out[21] = hdr.data_flags;
    out[22] = hdr.active_profile;
    out[23] = hdr.table_version;
    be16_write(out + 24, payload_len);
    if (payload_len > 0) {
        std::memcpy(out + kDataHeaderSize, payload, payload_len);
    }
    return total;
}

size_t encode_nack(const NackHeader& hdr, const uint8_t* bitmap,
                   uint8_t bitmap_len, uint8_t* out, size_t cap) {
    const size_t total = kNackFixedSize + bitmap_len;
    if (out == nullptr || cap < total ||
        (bitmap == nullptr && bitmap_len > 0)) {
        return 0;
    }
    encode_prefix(hdr.prefix, PacketType::kNack, out);
    be16_write(out + 11, hdr.target_originator);
    be32_write(out + 13, hdr.target_session);
    out[17] = hdr.target_stream_id;
    be32_write(out + 18, hdr.base_seq);
    out[22] = bitmap_len;
    if (bitmap_len > 0) {
        std::memcpy(out + kNackFixedSize, bitmap, bitmap_len);
    }
    return total;
}

size_t encode_link_report(const LinkReport& pkt, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kLinkReportSize) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kLinkReport, out);
    be16_write(out + 11, pkt.target_originator);
    be32_write(out + 13, pkt.target_session);
    out[17] = pkt.target_stream_id;
    be32_write(out + 18, pkt.report_epoch);
    out[22] = pkt.table_version;
    out[23] = static_cast<uint8_t>(pkt.rssi_best);
    out[24] = static_cast<uint8_t>(pkt.rssi_mean);
    be16_write(out + 25, pkt.loss_postdiv_prearq);
    be32_write(out + 27, pkt.uniq);
    be32_write(out + 31, pkt.diversity);
    out[35] = pkt.adapters;
    be16_write(out + 36, pkt.probe_per);
    out[38] = pkt.recommended_prof;
    return kLinkReportSize;
}

bool link_report_stamp_epoch(uint8_t* frame, size_t len, uint32_t epoch) {
    if (frame == nullptr || len < kLinkReportSize) return false;
    be32_write(frame + 18, epoch);  // same offset as encode_link_report
    return true;
}

size_t encode_heartbeat(const Heartbeat& pkt, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kHeartbeatSize) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kHeartbeat, out);
    return kHeartbeatSize;
}

size_t encode_selector_state(const SelectorState& pkt, uint8_t* out,
                             size_t cap) {
    const bool holder_present =
        (pkt.state_flags & selector_state_flags::kHolderPresent) != 0;
    const bool calib_present =
        (pkt.state_flags & selector_state_flags::kCalibPresent) != 0;
    if (calib_present && !holder_present) {
        return 0;  // §10.6: bit4 implies bit3
    }
    const size_t size = calib_present ? kSelectorStateCalibSize
                        : holder_present ? kSelectorStateSize
                                         : kSelectorStateLegacySize;
    if (out == nullptr || cap < size || !selector_state_fields_valid(pkt)) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kSelectorState, out);
    out[11] = pkt.table_version;
    out[12] = pkt.active_profile;
    out[13] = pkt.safe_floor_profile;
    out[14] = pkt.ceiling_profile;
    out[15] = pkt.lockout_profile;
    out[16] = pkt.state_flags;
    out[17] = pkt.lockout_strikes;
    be16_write(out + 18, pkt.remaining_ms);
    out[20] = pkt.transition_reason;
    be16_write(out + 21, pkt.loss_window_milli);
    out[23] = pkt.lockout_active_mask;
    out[24] = pkt.lockout_latched_mask;
    be16_write(out + 25, pkt.loss_ewma_milli);
    be32_write(out + 27, pkt.loss_uniq);
    out[31] = pkt.loss_score;
    if (holder_present) {
        be16_write(out + 32, pkt.report_latch_holder);
    }
    if (calib_present) {
        out[34] = pkt.calib_word;
        out[35] = pkt.calib_fingerprint;
    }
    return size;
}

size_t encode_announce(const Announce& pkt, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kAnnounceSize) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kAnnounce, out);
    out[11] = pkt.flags;
    be16_write(out + 12, pkt.claimed_by);
    std::memcpy(out + 14, pkt.psk, kAnnouncePskSize);
    return kAnnounceSize;
}

size_t encode_cache_assign(const CacheAssign& pkt, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kCacheAssignSize || pkt.target_cache == 0 ||
        pkt.target_originator == 0 || pkt.target_chan == 0 ||
        pkt.target_bw > 2) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kCacheAssign, out);
    be16_write(out + 11, pkt.target_cache);
    be16_write(out + 13, pkt.target_originator);
    be32_write(out + 15, pkt.assignment_epoch);
    be16_write(out + 19, pkt.target_chan);
    out[21] = pkt.target_bw;
    out[22] = pkt.target_net_id;
    return kCacheAssignSize;
}

size_t encode_vehicle_cmd(const VehicleCmd& pkt, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kVehicleCmdSize ||
        (pkt.cmd_flags & ~vcmd_flags::kKnownMask) != 0 ||
        !vcmd_arg_in_wire_range(pkt.cmd_id, pkt.cmd_arg)) {  // §3.14/P105
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kVehicleCmd, out);
    be32_write(out + 11, pkt.cmd_nonce);
    out[15] = pkt.cmd_seq;
    out[16] = pkt.cmd_flags;
    out[17] = pkt.cmd_id;
    out[18] = pkt.cmd_arg;
    be32_write(out + 19, pkt.cmd_mac);
    return kVehicleCmdSize;
}

size_t encode_calib_probe(const CalibProbe& pkt, uint16_t pad_to, uint8_t* out,
                          size_t cap) {
    // §3.16: the same structural invariants decode_calibration enforces —
    // refuse to mint a frame an honest receiver would reject. pad_to is the
    // negotiated §9.3a budget, clamped into the decoder's accepted range.
    const size_t total =
        std::min<size_t>(std::max<size_t>(pad_to, kCalibProbeFixedSize),
                         mtu_tier::kHighBudget);
    if (out == nullptr || cap < total || pkt.dwell_id == 0 || pkt.seq == 0 ||
        pkt.count == 0 || pkt.seq > pkt.count ||
        pkt.prefix.destination == 0) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kExtended, out);
    out[11] = ext_type::kCalProbe;
    be32_write(out + 12, pkt.run_id);
    be16_write(out + 16, pkt.dwell_id);
    be16_write(out + 18, pkt.seq);
    be16_write(out + 20, pkt.count);
    std::memset(out + kCalibProbeFixedSize, 0,
                total - kCalibProbeFixedSize);
    return total;
}

size_t encode_calib_tally(const CalibTally& pkt, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kCalibTallySize || pkt.dwell_id == 0 ||
        pkt.prefix.destination == 0) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kExtended, out);
    out[11] = ext_type::kCalTally;
    be32_write(out + 12, pkt.run_id);
    be16_write(out + 16, pkt.dwell_id);
    be16_write(out + 18, pkt.received);
    be32_write(out + 20, pkt.rssi_sum_dbm);
    out[24] = pkt.rx_mcs;
    out[25] = pkt.adapter_fingerprint;
    return kCalibTallySize;
}

size_t encode_link_verdict(const LinkVerdictPkt& pkt, uint8_t* out,
                           size_t cap) {
    // §3.16 (Pass 159): addressed, never broadcast; verdict 0..6.
    if (out == nullptr || cap < kLinkVerdictSize ||
        pkt.prefix.destination == 0 || pkt.verdict > link_verdict::kMax) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kExtended, out);
    out[11] = ext_type::kLinkVerdict;
    be16_write(out + 12, pkt.target_originator);
    be32_write(out + 14, pkt.target_session);
    be32_write(out + 18, pkt.report_epoch);
    out[22] = pkt.verdict;
    return kLinkVerdictSize;
}

size_t encode_csa(const CsaPacket& pkt, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kCsaSize) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kCsa, out);
    be32_write(out + 11, pkt.csa_nonce);
    out[15] = pkt.csa_seq;
    be16_write(out + 16, pkt.target_chan);
    out[18] = pkt.target_bw;
    out[19] = pkt.retune_class;
    be16_write(out + 20, pkt.dt_to_switch_ms);
    be16_write(out + 22, pkt.t_revert_ms);
    be16_write(out + 24, pkt.prev_chan);
    out[26] = pkt.prev_bw;
    out[27] = pkt.power_intent;
    be32_write(out + 28, pkt.csa_mac);
    return kCsaSize;
}

size_t encode_recovery_request(const RecoveryRequest& pkt, uint8_t* out,
                               size_t cap) {
    if (out == nullptr || cap < kRecoveryRequestSize) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kRecoveryRequest, out);
    be16_write(out + 11, pkt.target_originator);
    be32_write(out + 13, pkt.target_session);
    out[17] = pkt.target_stream_id;
    return kRecoveryRequestSize;
}

size_t encode_jscc_feedback(const JsccFeedback& pkt, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kJsccFeedbackSize ||
        (pkt.valid_flags & ~jscc_feedback_flags::kKnownMask) != 0) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kJsccFeedback, out);
    be16_write(out + 11, pkt.target_originator);
    be32_write(out + 13, pkt.target_session);
    out[17] = pkt.target_stream_id;
    be32_write(out + 18, pkt.feedback_epoch);
    be16_write(out + 22, pkt.repair_demand_permille);
    be32_write(out + 24, pkt.rtt_p95_us);
    be16_write(out + 28, pkt.repair_samples);
    be16_write(out + 30, pkt.rtt_samples);
    out[32] = pkt.valid_flags;
    be32_write(out + 33, pkt.observed_block_id);
    return kJsccFeedbackSize;
}

size_t encode_cache_status(const CacheStatus& pkt, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kCacheStatusSize ||
        pkt.rx_health_permille > 1000 ||
        (pkt.capability_flags & ~cache_capability::kKnownMask) != 0) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kCacheStatus, out);
    be16_write(out + 11, pkt.target_originator);
    be32_write(out + 13, pkt.target_session);
    out[17] = pkt.target_stream_id;
    be32_write(out + 18, pkt.oldest_block);
    be32_write(out + 22, pkt.newest_block);
    be16_write(out + 26, pkt.rx_health_permille);
    out[28] = pkt.capability_flags;
    return kCacheStatusSize;
}

size_t encode_cache_request(const CacheRequestHeader& hdr,
                            const uint8_t* missing_sources,
                            const uint8_t* repair_have,
                            uint8_t repair_have_len, uint8_t* out, size_t cap) {
    if (hdr.window_len == 0 || hdr.window_len > kFecMaxSymbols ||
        hdr.max_symbols == 0 || repair_have_len > 32) {
        return 0;
    }
    const size_t src_len = (static_cast<size_t>(hdr.window_len) + 7) / 8;
    const size_t total = kCacheRequestFixedSize + src_len + repair_have_len;
    if (out == nullptr || cap < total || missing_sources == nullptr ||
        (repair_have == nullptr && repair_have_len > 0)) {
        return 0;
    }
    encode_prefix(hdr.prefix, PacketType::kCacheRequest, out);
    be16_write(out + 11, hdr.target_originator);
    be32_write(out + 13, hdr.target_session);
    out[17] = hdr.target_stream_id;
    be16_write(out + 18, hdr.target_cache);
    be32_write(out + 20, hdr.request_id);
    be32_write(out + 24, hdr.block_id);
    be16_write(out + 28, hdr.window_len);
    out[30] = hdr.max_symbols;
    out[31] = repair_have_len;
    std::memcpy(out + kCacheRequestFixedSize, missing_sources, src_len);
    if (repair_have_len > 0) {
        std::memcpy(out + kCacheRequestFixedSize + src_len, repair_have,
                    repair_have_len);
    }
    return total;
}

size_t encode_cache_reply(const CommonPrefix& prefix, uint32_t request_id,
                          const uint8_t* wrapped, uint16_t wrapped_len,
                          uint8_t* out, size_t cap) {
    const size_t total = kCacheReplyFixedSize + wrapped_len;
    if (out == nullptr || cap < total || wrapped == nullptr ||
        wrapped_len < kDataHeaderSize) {
        return 0;
    }
    encode_prefix(prefix, PacketType::kCacheReply, out);
    be32_write(out + 11, request_id);
    be16_write(out + 15, wrapped_len);
    std::memcpy(out + kCacheReplyFixedSize, wrapped, wrapped_len);
    return total;
}

}  // namespace wblink
