// SPDX-License-Identifier: GPL-2.0-or-later
// Ground-side admission/freshness rules for the advisory §3.15 selector
// summary. Kept pure so source/session/table/expiry boundaries are unit tested.
#pragma once

#include <cstdint>
#include <optional>

#include "wblink/wire.h"

namespace wblink {

inline constexpr uint64_t kSelectorStateExpiryMs = 1500;

inline bool selector_state_admissible(
    const SelectorState& state, std::optional<uint8_t> local_table_version,
    uint16_t rtp_originator, uint32_t rtp_session) {
    return local_table_version.has_value() &&
           state.table_version == *local_table_version &&
           state.prefix.destination == 0 &&
           state.prefix.originator == rtp_originator &&
           state.prefix.session_id == rtp_session;
}

inline bool selector_state_fresh(uint64_t now_ms, uint64_t received_ms) {
    return now_ms >= received_ms &&
           now_ms - received_ms <= kSelectorStateExpiryMs;
}

}  // namespace wblink
