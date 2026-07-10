// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §10 per-adapter TX power model.
//
// One PowerCurve per physical adapter: the operator-authored absolute qdb
// (quarter-dBm) value for each HT MCS0–7, sourced from a node-local file in
// the stock Realtek PHY_REG_PG.txt row format (parsed in io/ — core holds
// only the resolved numbers). Per the §10.2 Pass-6 ruling, the authored
// curve IS tx_power_level 4; other levels are a monotonic ±2 dB/step offset:
//
//   absolute_qdb = curve[mcs] + (level − 4) × 8 qdb
//
// then the §10.3 opt-in max_power_qdb ceiling. There is deliberately NO
// regulatory clamp (§10.3) — the ceiling exists so a mis-authored table
// cannot silently cook a PA, not to enforce law.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace wblink {

inline constexpr int kPowerLevelBaseline = 4;   // the authored curve's level
inline constexpr int32_t kQdbPerLevel = 8;      // 2 dB per level step

struct PowerCurve {
    std::array<int32_t, 8> qdb{};  // absolute qdb per MCS0–7
    bool valid = false;            // false until a map file was loaded
};

// (adapter's curve, profile.mcs, profile.tx_power_level) → absolute qdb for
// SetTxPowerOffsetQdb (§10.4). nullopt when the curve is unloaded or the MCS
// is out of the HT0–7 range — the caller then leaves hardware power alone.
inline std::optional<int32_t> resolve_power_qdb(
    const PowerCurve& curve, uint8_t mcs, uint8_t level,
    std::optional<int32_t> max_power_qdb = std::nullopt) {
    if (!curve.valid || mcs >= curve.qdb.size()) {
        return std::nullopt;
    }
    int32_t v = curve.qdb[mcs] +
                (static_cast<int32_t>(level) - kPowerLevelBaseline) * kQdbPerLevel;
    if (max_power_qdb) {
        v = std::min(v, *max_power_qdb);
    }
    return v;
}

}  // namespace wblink
