// SPDX-License-Identifier: GPL-2.0-or-later
// Authored conservative service-rate model for §14.2 JSCC. A zero
// efficiency is deliberately unavailable rather than an optimistic default.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wblink {

inline std::optional<uint32_t> ht20_service_time_us(
    size_t bytes, uint8_t mcs, bool sgi, uint16_t efficiency_permille) {
    static constexpr std::array<uint32_t, 8> kLongGiKbps = {
        6500, 13000, 19500, 26000, 39000, 52000, 58500, 65000};
    if (mcs >= kLongGiKbps.size() || efficiency_permille == 0 ||
        efficiency_permille > 1000) {
        return std::nullopt;
    }
    uint64_t phy_kbps = kLongGiKbps[mcs];
    if (sgi) {
        phy_kbps = phy_kbps * 10u / 9u;  // floor is conservative
    }
    const uint64_t service_kbps =
        phy_kbps * efficiency_permille / 1000u;
    if (service_kbps == 0) return std::nullopt;
    const uint64_t bits_x_1000 =
        static_cast<uint64_t>(bytes) * 8u * 1000u;
    const uint64_t us =
        (bits_x_1000 + service_kbps - 1u) / service_kbps;
    return static_cast<uint32_t>(std::min<uint64_t>(us, UINT32_MAX));
}

}  // namespace wblink
