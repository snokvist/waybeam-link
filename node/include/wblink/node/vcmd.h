// SPDX-License-Identifier: GPL-2.0-or-later
// §15.5 REST names ↔ §11.7 command ids, and the §9.3a MTU tier names
// (#109 Phase 2c).
//
// Pure name maps with no state. They are here rather than in `core/` because
// the names are the CONTROL-PLANE spelling (§15.5), not the wire — `core/`
// owns the ids in `types.h` and must not learn the REST vocabulary.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "wblink/types.h"

namespace wblink {
namespace node {

// §15.5 vehicle/command REST names ↔ §11.7 registry ids.
inline uint8_t vcmd_id_for(const std::string& name) {
    if (name == "arq") return vcmd_id::kArq;
    if (name == "selector") return vcmd_id::kSelector;
    if (name == "fps_ladder") return vcmd_id::kFpsLadder;
    if (name == "fps_select") return vcmd_id::kFpsSelect;
    if (name == "resolution") return vcmd_id::kResolution;
    if (name == "framing") return vcmd_id::kFraming;
    if (name == "mode") return vcmd_id::kMode;  // §11.7 Pass 105
    if (name == "calibrate") return vcmd_id::kCalibrate;  // §10.6 Pass 120
    if (name == "mtu_tier") return vcmd_id::kMtuTier;  // §9.3a Pass 122
    if (name == "tx_power") return vcmd_id::kTxPower;  // §10.3 Pass 135
    return 0;
}

inline const char* vcmd_name_for(uint8_t id) {
    switch (id) {
        case vcmd_id::kArq: return "arq";
        case vcmd_id::kSelector: return "selector";
        case vcmd_id::kFpsLadder: return "fps_ladder";
        case vcmd_id::kFpsSelect: return "fps_select";
        case vcmd_id::kResolution: return "resolution";
        case vcmd_id::kFraming: return "framing";
        case vcmd_id::kMode: return "mode";  // §11.7 Pass 105
        case vcmd_id::kCalibrate: return "calibrate";  // §10.6 Pass 120
        case vcmd_id::kMtuTier: return "mtu_tier";  // §9.3a Pass 122
        case vcmd_id::kTxPower: return "tx_power";  // §10.3 Pass 135
    }
    return "";
}

inline std::optional<uint8_t> mtu_tier_for_mode(const std::string& mode,
                                                uint16_t supported) {
    if (mode == "default") return mtu_tier::kDefault;
    if (mode == "medium") return mtu_tier::kMedium;
    if (mode == "high") return mtu_tier::kHigh;
    if (mode == "auto") {
        if (supported >= mtu_tier::kHighBudget) return mtu_tier::kHigh;
        if (supported >= mtu_tier::kMediumBudget) return mtu_tier::kMedium;
        return mtu_tier::kDefault;
    }
    return std::nullopt;
}

}  // namespace node
}  // namespace wblink
