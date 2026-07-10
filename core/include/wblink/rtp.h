// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: minimal RTP fixed-header parse (RFC 3550, 12 bytes).
//
// This is the ONLY RTP awareness in the transport (PROTOCOL.md §4.1): the
// framer reads marker/timestamp for block boundaries and (in step 7) the NAL
// type for ARQ classification. Payload stays opaque; nothing deeper is parsed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "wblink/endian.h"

namespace wblink {

inline constexpr size_t kRtpFixedHeaderSize = 12;

struct RtpHeader {
    uint8_t version = 0;   // must be 2
    bool padding = false;
    bool extension = false;
    uint8_t csrc_count = 0;
    bool marker = false;
    uint8_t payload_type = 0;
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
};

// Parses the fixed header only (enough for boundary detection). Returns
// nullopt if the buffer is too short (including declared CSRCs) or the
// version is not 2.
inline std::optional<RtpHeader> parse_rtp_header(const uint8_t* d, size_t len) {
    if (d == nullptr || len < kRtpFixedHeaderSize) {
        return std::nullopt;
    }
    RtpHeader h;
    h.version = static_cast<uint8_t>(d[0] >> 6);
    if (h.version != 2) {
        return std::nullopt;
    }
    h.padding = (d[0] & 0x20) != 0;
    h.extension = (d[0] & 0x10) != 0;
    h.csrc_count = static_cast<uint8_t>(d[0] & 0x0F);
    if (len < kRtpFixedHeaderSize + h.csrc_count * 4u) {
        return std::nullopt;
    }
    h.marker = (d[1] & 0x80) != 0;
    h.payload_type = static_cast<uint8_t>(d[1] & 0x7F);
    h.seq = be16_read(d + 2);
    h.timestamp = be32_read(d + 4);
    h.ssrc = be32_read(d + 8);
    return h;
}

struct RtpPayload {
    const uint8_t* data;
    size_t len;
};

// Locates the RTP payload for the §4.1 classifier: skips CSRCs and the
// RFC 3550 header extension, trims declared padding. Returns nullopt on any
// inconsistency (extension/padding overruns the datagram) — the classifier
// then falls back to not-important; the transport still carries the packet
// opaquely either way.
inline std::optional<RtpPayload> rtp_payload(const uint8_t* d, size_t len,
                                             const RtpHeader& h) {
    size_t off = kRtpFixedHeaderSize + h.csrc_count * 4u;
    if (h.extension) {
        if (off + 4 > len) {
            return std::nullopt;
        }
        off += 4 + 4u * be16_read(d + off + 2);
    }
    size_t end = len;
    if (h.padding) {
        const uint8_t pad = d[len - 1];  // len >= 12 guaranteed by the parse
        if (pad == 0 || pad > end) {
            return std::nullopt;
        }
        end -= pad;
    }
    if (off > end) {
        return std::nullopt;
    }
    return RtpPayload{d + off, end - off};
}

}  // namespace wblink
