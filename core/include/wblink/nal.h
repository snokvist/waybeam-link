// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §4.1 RTP-profile ARQ classifier — NAL unit type only.
//
// The shallow classifier the spec mandates: read ONLY the NAL unit type from
// the RTP payload and answer one question — is this packet part of an
// ARQ-important block? Nothing deeper is parsed; the payload stays opaque to
// the rest of the transport.
//
//  - H.264 (RFC 6184): IDR slice (5) + parameter sets SPS (7) / PPS (8)
//    => important. STAP-A/B aggregation is scanned unit-by-unit (a keyframe
//    AU commonly travels as STAP-A(SPS,PPS) + FU-A(IDR...)); FU-A/B carry the
//    real type in the FU header, present in EVERY fragment, so importance is
//    stamped from the first packet of the block. MTAP16/24 (interleaved mode)
//    is not used by any low-latency payloader we feed => not important.
//  - H.265 (RFC 7798): IDR_W_RADL (19) / IDR_N_LP (20) / CRA (21) +
//    VPS (32) / SPS (33) / PPS (34) => important, exactly the §4.1 list. AP
//    (48) scanned unit-by-unit; FU (49) reads the FU header. DONL fields are
//    assumed absent (they exist only with sprop-max-don-diff > 0 interleaved
//    sessions, which contradict the §5.1 latency posture).
//
// Malformed payloads never trap: every read is bounds-checked and a parse
// dead-end classifies as NOT important (fail-toward-best-effort — a junk
// packet must not be able to buy itself ARQ service).
#pragma once

#include <cstddef>
#include <cstdint>

#include "wblink/endian.h"

namespace wblink {

// §4.1 classifier selection, per stream. kSize is the pure-agnostic fallback
// (cumulative block size over a threshold); the NAL modes stamp importance
// from the first packet of the block.
enum class RtpClassifier : uint8_t { kSize = 0, kH264 = 1, kH265 = 2 };

namespace detail {

inline constexpr bool h264_type_important(uint8_t t) {
    return t == 5 || t == 7 || t == 8;  // IDR slice, SPS, PPS
}

inline constexpr bool h265_type_important(uint8_t t) {
    return t == 19 || t == 20 || t == 21 ||  // IDR_W_RADL, IDR_N_LP, CRA_NUT
           t == 32 || t == 33 || t == 34;    // VPS, SPS, PPS
}

// Scans RFC 6184 STAP aggregation units: [2B size][NAL]... starting at off.
inline bool h264_scan_stap(const uint8_t* p, size_t len, size_t off) {
    while (off + 2 <= len) {
        const uint16_t sz = be16_read(p + off);
        off += 2;
        if (sz == 0 || off + sz > len) {
            return false;  // malformed: stop, classify best-effort
        }
        if (h264_type_important(p[off] & 0x1F)) {
            return true;
        }
        off += sz;
    }
    return false;
}

// Scans RFC 7798 AP aggregation units: [2B size][2B NAL header + body]...
inline bool h265_scan_ap(const uint8_t* p, size_t len, size_t off) {
    while (off + 2 <= len) {
        const uint16_t sz = be16_read(p + off);
        off += 2;
        if (sz < 2 || off + sz > len) {
            return false;
        }
        if (h265_type_important(static_cast<uint8_t>((p[off] >> 1) & 0x3F))) {
            return true;
        }
        off += sz;
    }
    return false;
}

}  // namespace detail

// True iff an H.264 RTP payload (RFC 6184) contains an ARQ-important NAL.
inline bool h264_payload_important(const uint8_t* p, size_t len) {
    if (p == nullptr || len < 1) {
        return false;
    }
    const uint8_t type = p[0] & 0x1F;
    if (type >= 1 && type <= 23) {  // single NAL unit packet
        return detail::h264_type_important(type);
    }
    switch (type) {
        case 24:  // STAP-A: indicator, then units
            return detail::h264_scan_stap(p, len, 1);
        case 25:  // STAP-B: indicator, 2B DON, then units
            return detail::h264_scan_stap(p, len, 3);
        case 28:  // FU-A: indicator, FU header
        case 29:  // FU-B: indicator, FU header, 2B DON
            return len >= 2 && detail::h264_type_important(p[1] & 0x1F);
        default:  // MTAP16/24, reserved
            return false;
    }
}

// True iff an H.265 RTP payload (RFC 7798) contains an ARQ-important NAL.
inline bool h265_payload_important(const uint8_t* p, size_t len) {
    if (p == nullptr || len < 2) {
        return false;
    }
    const uint8_t type = static_cast<uint8_t>((p[0] >> 1) & 0x3F);
    if (type < 48) {  // single NAL unit packet (2B payload header = NAL header)
        return detail::h265_type_important(type);
    }
    if (type == 48) {  // AP: 2B payload header, then units (no DONL)
        return detail::h265_scan_ap(p, len, 2);
    }
    if (type == 49) {  // FU: 2B payload header, 1B FU header (no DONL)
        return len >= 3 && detail::h265_type_important(p[2] & 0x3F);
    }
    return false;  // PACI, reserved
}

}  // namespace wblink
