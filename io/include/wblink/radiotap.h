// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: radiotap for BOTH air backends (§3.0 Pass 118). Two pure,
// dependency-free pieces so both unit-test dry:
//   radiotap_tx_ht()   — build the 13-byte HT radiotap header that carries the
//                        per-packet MCS. Pass 118 retired the Pass-13 split:
//                        devourer honours the radiotap rate fields on every
//                        generation and falls back to its committed SetTxMode
//                        only for frames whose radiotap carries no rate, so
//                        this one header now prefixes every injected frame.
//   radiotap_parse()   — extract RSSI (DBM_ANTSIGNAL), TSFT and the received
//                        MCS from a monitor frame's leading radiotap header,
//                        and report the header length to strip before
//                        dot11_parse.
// Format reference (not vendored): third_party/devourer/src/ieee80211_radiotap.h
// enum + Radiotap.c align/size table + RadiotapBuilder.cpp build_ht.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace wblink {

// ---- TX: HT (11n) radiotap header carrying the per-packet MCS ---------------
// version(0) pad(0) it_len(13) it_present(TX_FLAGS|MCS) tx_flags
// mcs_known mcs_flags mcs_index. Length stays 13 so mac80211's injection path
// reads it as HT (not VHT). bw: 20 or 40 (80 is not HT).
inline constexpr size_t kRadiotapTxHtLen = 13;

// §3.0 TX_FLAGS values. NOACK is the default for the broadcast frame; the
// Pass-12 unicast return clears it so the frame solicits a MAC ACK and gets
// devourer's descriptor-driven retries.
inline constexpr uint16_t kTxFlagsNoAck = 0x0008;
inline constexpr uint16_t kTxFlagsAck = 0x0000;

inline size_t radiotap_tx_ht(uint8_t* out, uint8_t mcs, bool sgi, uint8_t bw,
                             uint16_t tx_flags = kTxFlagsNoAck,
                             bool ldpc = false, uint8_t stbc = 0) {
    // it_present = TX_FLAGS (1<<15) | MCS (1<<19) = 0x00088000.
    constexpr uint32_t kPresent = (1u << 15) | (1u << 19);
    // MCS "known" mask: BW | MCS | GI | FEC | STBC.
    constexpr uint8_t kKnown = 0x01u | 0x02u | 0x04u | 0x10u | 0x20u;
    out[0] = 0x00;  // version
    out[1] = 0x00;  // pad
    out[2] = static_cast<uint8_t>(kRadiotapTxHtLen & 0xff);
    out[3] = static_cast<uint8_t>(kRadiotapTxHtLen >> 8);
    out[4] = static_cast<uint8_t>(kPresent & 0xff);
    out[5] = static_cast<uint8_t>((kPresent >> 8) & 0xff);
    out[6] = static_cast<uint8_t>((kPresent >> 16) & 0xff);
    out[7] = static_cast<uint8_t>((kPresent >> 24) & 0xff);
    out[8] = static_cast<uint8_t>(tx_flags & 0xff);
    out[9] = static_cast<uint8_t>(tx_flags >> 8);
    out[10] = kKnown;
    uint8_t flags = 0;
    if (bw >= 40) flags = static_cast<uint8_t>(flags | 0x01u);  // 40 MHz
    if (sgi) flags = static_cast<uint8_t>(flags | 0x04u);       // short GI
    // §3.0 Pass 157: the known mask above claims FEC and STBC known, so a
    // clear bit is an affirmative BCC / zero-stream command — these carry
    // the node coding, not an omission.
    if (ldpc) flags = static_cast<uint8_t>(flags | 0x10u);  // FEC: LDPC
    flags = static_cast<uint8_t>(flags | ((stbc & 0x03u) << 5));  // streams
    out[11] = flags;
    out[12] = mcs <= 31 ? mcs : 0;
    return kRadiotapTxHtLen;
}

// ---- RX: minimal standalone radiotap parser --------------------------------
// §15.3 Pass 118: the HT MCS a frame was received at, or kRxMcsUnknown when
// the backend could not resolve one (no radiotap MCS field, a non-HT rate
// code, or an index past the §9.3 ladder). Advisory observation — no control
// path reads it. Lives here, not with AirRxMeta, so the §15.3 writer can
// size its histogram without depending on an air backend.
inline constexpr uint8_t kRxMcsUnknown = 0xff;
inline constexpr size_t kRxMcsBuckets = 8;  // the §9.3 ladder, MCS0-7

struct RadiotapRx {
    size_t hdr_len = 0;              // bytes to strip before the 802.11 MPDU
    std::optional<int8_t> rssi_dbm;  // IEEE80211_RADIOTAP_DBM_ANTSIGNAL
    std::optional<uint64_t> tsf_us;  // IEEE80211_RADIOTAP_TSFT (MAC time, µs)
    // §3.0 Pass 118: HT MCS index the frame was received at, when the MCS
    // field is present AND its "known" mask claims the index. Absent for a
    // legacy/VHT/HE rate — the caller counts those as rx_mcs_unknown (§15.3).
    std::optional<uint8_t> mcs;
    bool fcs_at_end = false;          // FLAGS bit 0x10: MPDU includes 4-byte FCS
};

// Parse the leading radiotap header. nullopt only when the framing is
// malformed (buffer too short / it_len out of range). rssi/tsf are filled when
// present. Handles the extended it_present chain (bit 31) and the standard
// field alignment/size table; interprets the first present word's standard
// fields (indices 0..22) — extension namespaces (bits 29/30) are not needed
// for TSFT/RSSI on the mac80211/Realtek monitor drivers we use.
inline std::optional<RadiotapRx> radiotap_parse(const uint8_t* buf,
                                                size_t len) {
    if (buf == nullptr || len < 8) {
        return std::nullopt;
    }
    const uint16_t it_len =
        static_cast<uint16_t>(buf[2] | (static_cast<uint16_t>(buf[3]) << 8));
    if (it_len < 8 || static_cast<size_t>(it_len) > len) {
        return std::nullopt;
    }
    // Standard radiotap namespace {align, size}, indices 0..22.
    static constexpr uint8_t kAlign[23] = {8, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1, 1,
                                           1, 1, 2, 2, 1, 1, 0, 1, 4, 2, 8};
    static constexpr uint8_t kSize[23] = {8, 1, 1, 4, 2, 1, 1, 2, 2, 2, 1, 1,
                                          1, 1, 2, 2, 1, 1, 0, 3, 8, 12, 12};
    auto le32 = [buf](size_t o) -> uint32_t {
        return static_cast<uint32_t>(buf[o]) |
               (static_cast<uint32_t>(buf[o + 1]) << 8) |
               (static_cast<uint32_t>(buf[o + 2]) << 16) |
               (static_cast<uint32_t>(buf[o + 3]) << 24);
    };
    // Walk the present-word chain (bit 31 = another word follows) to find
    // where the field data begins.
    const uint32_t present0 = le32(4);
    uint32_t w = present0;
    size_t words = 1;
    while (w & 0x80000000u) {
        const size_t o = 4 + words * 4;
        if (o + 4 > static_cast<size_t>(it_len)) {
            return std::nullopt;
        }
        w = le32(o);
        ++words;
    }
    size_t off = 4 + words * 4;  // start of field data (bytes from buf)

    RadiotapRx r;
    r.hdr_len = it_len;
    for (int bit = 0; bit <= 22; ++bit) {
        if (!(present0 & (1u << bit))) {
            continue;
        }
        const size_t al = kAlign[bit];
        const size_t sz = kSize[bit];
        if (al == 0 || sz == 0) {
            continue;  // undefined index (18)
        }
        const size_t rem = off % al;
        if (rem != 0) {
            off += al - rem;  // align (rem < al ⇒ positive)
        }
        if (off + sz > static_cast<size_t>(it_len)) {
            break;  // field runs past the header — truncated
        }
        if (bit == 0) {  // TSFT: u64 LE µs
            uint64_t t = 0;
            for (int i = 0; i < 8; ++i) {
                t |= static_cast<uint64_t>(buf[off + static_cast<size_t>(i)])
                     << (8 * i);
            }
            r.tsf_us = t;
        } else if (bit == 1) {  // FLAGS: bit 4 means FCS is included at end
            r.fcs_at_end = (buf[off] & 0x10u) != 0;
        } else if (bit == 5) {  // DBM_ANTSIGNAL: s8 dBm
            r.rssi_dbm = static_cast<int8_t>(buf[off]);
        } else if (bit == 19) {  // MCS: known, flags, index (Pass 118)
            // 0x02 = IEEE80211_RADIOTAP_MCS_HAVE_MCS. Without it the index
            // byte is not meaningful, whatever it happens to contain.
            if ((buf[off] & 0x02u) != 0 && buf[off + 2] <= 31) {
                r.mcs = buf[off + 2];
            }
        }
        off += sz;
    }
    return r;
}

}  // namespace wblink
