// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/wire.h"

#include <cstring>

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

size_t encode_heartbeat(const Heartbeat& pkt, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kHeartbeatSize) {
        return 0;
    }
    encode_prefix(pkt.prefix, PacketType::kHeartbeat, out);
    return kHeartbeatSize;
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
