// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: radiotap TX for the devourer air backend (§3.0 Pass 118).
// Pure and dependency-free so it unit-tests dry:
//   radiotap_tx_ht()   — build the 13-byte HT radiotap header that carries the
//                        per-packet MCS. Pass 118 retired the Pass-13 split:
//                        devourer honours the radiotap rate fields on every
//                        generation and falls back to its committed SetTxMode
//                        only for frames whose radiotap carries no rate, so
//                        this one header now prefixes every injected frame.
// Format reference (not vendored): third_party/devourer/src/ieee80211_radiotap.h
// enum + Radiotap.c align/size table + RadiotapBuilder.cpp build_ht.
#pragma once

#include <cstddef>
#include <cstdint>

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

// ---- RX: received-MCS observation ------------------------------------------
// §15.3 Pass 118: the HT MCS a frame was received at, or kRxMcsUnknown when
// the backend could not resolve one (no radiotap MCS field, a non-HT rate
// code, or an index past the §9.3 ladder). Advisory observation — no control
// path reads it. Lives here, not with AirRxMeta, so the §15.3 writer can
// size its histogram without depending on an air backend.
inline constexpr uint8_t kRxMcsUnknown = 0xff;
inline constexpr size_t kRxMcsBuckets = 8;  // the §9.3 ladder, MCS0-7


}  // namespace wblink
