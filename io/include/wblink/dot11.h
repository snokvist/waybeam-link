// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §3.0 on-air 802.11 encapsulation — the pinned data frame
// every waybeam-link packet rides in through devourer, and the RX-side
// filter that separates our frames from ambient monitor-mode traffic.
// Pure byte-level build/parse (no radio dependency) so it unit-tests dry.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace wblink {

// Rate-less radiotap prefix (version 0, len 10, present = TX_FLAGS only,
// tx_flags = NOACK). The PHY rate comes from the adapter's committed
// SetTxMode (§9.5/§10.4), never per-packet.
inline constexpr uint8_t kRadiotapTx[10] = {0x00, 0x00, 0x0a, 0x00, 0x00,
                                            0x80, 0x00, 0x00, 0x08, 0x00};
inline constexpr size_t kRadiotapTxLen = sizeof(kRadiotapTx);
inline constexpr size_t kDot11HdrLen = 24;
inline constexpr size_t kDot11TxPrefixLen = kRadiotapTxLen + kDot11HdrLen;

// §3.0 pinned constants.
inline constexpr uint8_t kDot11FrameControl0 = 0x08;  // Data, not QoS
inline constexpr uint8_t kDot11FrameControl1 = 0x00;  // ToDS=0 FromDS=0
// 0x56 not the payload magic's 0x57: I/G clear = valid unicast TA (Pass 8).
inline constexpr uint8_t kWbSaPrefix0 = 0x56;
inline constexpr uint8_t kWbSaPrefix1 = 0x42;
inline constexpr uint8_t kWbBssid[6] = {0x56, 0x42, 0x4c, 0x4b, 0x00, 0x00};

// Writes radiotap + the pinned 802.11 header into out (which must hold
// kDot11TxPrefixLen bytes); the waybeam-link packet follows as the frame
// body. seq is the injector-incremented sequence number (fragment 0).
inline size_t dot11_tx_prefix(uint8_t* out, uint8_t net_id,
                              uint16_t originator, uint8_t adapter_idx,
                              uint16_t seq) {
    std::memcpy(out, kRadiotapTx, kRadiotapTxLen);
    uint8_t* h = out + kRadiotapTxLen;
    h[0] = kDot11FrameControl0;
    h[1] = kDot11FrameControl1;
    h[2] = 0;  // duration
    h[3] = 0;
    std::memset(h + 4, 0xff, 6);  // addr1 DA = broadcast (never MAC-ACKed)
    h[10] = kWbSaPrefix0;         // addr2 SA = 56:42:NN:OO:OO:AA
    h[11] = kWbSaPrefix1;
    h[12] = net_id;
    h[13] = static_cast<uint8_t>(originator >> 8);
    h[14] = static_cast<uint8_t>(originator & 0xff);
    h[15] = adapter_idx;
    std::memcpy(h + 16, kWbBssid, 6);  // addr3 BSSID = "VBLK" tag
    h[22] = static_cast<uint8_t>((seq << 4) & 0xff);  // seq ctl, fragment 0
    h[23] = static_cast<uint8_t>(seq >> 4);
    return kDot11TxPrefixLen;
}

struct Dot11Rx {
    uint8_t net_id = 0;
    uint16_t originator = 0;
    uint8_t adapter_idx = 0;
    const uint8_t* payload = nullptr;  // view into the caller's MPDU
    size_t payload_len = 0;
};

// §3.0 RX filter over a raw 802.11 MPDU (radiotap already stripped —
// devourer's Packet.Data). Cheapest checks first: exact pinned Frame
// Control, SA prefix, optional net_id equality, then the payload magic.
// Full wire-header validation stays in the §3.1 codec. nullopt = not ours.
inline std::optional<Dot11Rx> dot11_parse(
    const uint8_t* mpdu, size_t len,
    std::optional<uint8_t> want_net_id = std::nullopt) {
    // Header + at least the 2-byte magic.
    if (mpdu == nullptr || len < kDot11HdrLen + 2) {
        return std::nullopt;
    }
    if (mpdu[0] != kDot11FrameControl0 || mpdu[1] != kDot11FrameControl1) {
        return std::nullopt;
    }
    if (mpdu[10] != kWbSaPrefix0 || mpdu[11] != kWbSaPrefix1) {
        return std::nullopt;
    }
    if (want_net_id && mpdu[12] != *want_net_id) {
        return std::nullopt;
    }
    // Payload magic pre-check (§3.1 does the full validation later).
    if (mpdu[24] != 0x57 || mpdu[25] != 0x42) {
        return std::nullopt;
    }
    Dot11Rx r;
    r.net_id = mpdu[12];
    r.originator = static_cast<uint16_t>((mpdu[13] << 8) | mpdu[14]);
    r.adapter_idx = mpdu[15];
    r.payload = mpdu + kDot11HdrLen;
    r.payload_len = len - kDot11HdrLen;
    return r;
}

}  // namespace wblink
