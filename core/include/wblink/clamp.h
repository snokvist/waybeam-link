// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: plausible-forward clamp (PROTOCOL.md §6.6).
//
// The load-bearing injection defence: RX rejects any DATA seq/block_id or
// NACK base_seq that jumps more than clamp_k ahead of the current cursor.
// Real monotonic traffic never jumps by millions; this one check neutralises
// forged far-future block_id video-flush, garbage NACK bitmaps, and discovery
// cursor poisoning.
//
// seq/block_id do not wrap within a flight (§2), so plain integer comparison
// applies — there is deliberately NO modular/wrap arithmetic here. Backward
// candidates (candidate <= cursor) are always plausible: duplicates and late
// arrivals are dedup's problem, not the clamp's.
#pragma once

#include <cstdint>

namespace wblink {

constexpr bool plausible_forward(uint32_t cursor, uint32_t candidate,
                                 uint32_t clamp_k) {
    if (candidate <= cursor) {
        return true;
    }
    return (candidate - cursor) <= clamp_k;
}

}  // namespace wblink
