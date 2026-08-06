// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: the devourer RX descriptor decode rules, as pure functions.
//
// These three rules used to live inline in RadioAir::on_packet, where nothing
// could reach them without a libusb handle and a real adapter — the whole of
// G8's "the devourer-specific path has no test". They are pure functions of a
// buffer and a few descriptor fields, so they need no fake, only somewhere a
// test can call them from.
//
// Nothing here knows a devourer type: on_packet reads the fields out of
// Packet::RxAtrib and passes plain values, so the header stays usable from a
// test target that does not link the vendored driver.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "wblink/dot11.h"     // kFcsLen
#include "wblink/radiotap.h"  // kRxMcsUnknown
#include "wblink/types.h"     // kRxMcsBuckets

namespace wblink {

// Realtek RX descriptor rate code -> HT MCS index. hal_com.h pins
// DESC_RATEMCS0 = 0x0c (0..3 CCK, 4..11 legacy OFDM, 12.. HT), so anything
// below 12 or above the §9.3 ladder is reported unresolved (§15.3).
inline uint8_t desc_rate_to_mcs(uint16_t code) {
    constexpr uint16_t kDescRateMcs0 = 0x0c;
    if (code < kDescRateMcs0) return kRxMcsUnknown;
    const uint16_t mcs = static_cast<uint16_t>(code - kDescRateMcs0);
    return mcs < kRxMcsBuckets ? static_cast<uint8_t>(mcs) : kRxMcsUnknown;
}

// §3.0: monitor RX delivers the MPDU with the chip-validated 4-byte FCS still
// appended. Strip it before the length-exact parse; a frame that is all
// trailer (or shorter) is not a frame. nullopt = drop.
inline std::optional<size_t> mpdu_len_without_fcs(size_t delivered) {
    if (delivered <= kFcsLen) return std::nullopt;
    return delivered - kFcsLen;
}

// Per-chain power bytes -> dBm for the best chain. The descriptor reports
// `value - 110` dBm, and 0 means "no PHY report on this frame" — for which the
// previous reading is kept rather than inventing a -110 floor, because a
// report-less frame says nothing about signal.
//
// Note the input is unsigned, so the reachable range is -110..+145 dBm and only
// the upper clamp can fire; the lower one is kept as a guard on the arithmetic,
// not because a chain byte can produce it.
inline int8_t rssi_dbm_from_chains(const uint8_t* chains, size_t n,
                                   int8_t previous) {
    uint8_t best = 0;
    for (size_t i = 0; i < n; ++i) {
        if (chains[i] > best) best = chains[i];
    }
    if (best == 0) return previous;
    const int dbm = static_cast<int>(best) - 110;
    return static_cast<int8_t>(dbm < -128 ? -128 : dbm > 0 ? 0 : dbm);
}

}  // namespace wblink
