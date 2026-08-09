// SPDX-License-Identifier: GPL-2.0-or-later
// The node's monotonic clock (#109 Phase 2a).
//
// `core/` takes time as an injected argument on purpose and must keep doing
// so. This is where the injection gets its number: one steady_clock read,
// two units, used by every node-layer object and by app/main.cpp's loops.
// `inline` rather than the old TU-local definitions, so a consumer linking
// several node/ headers gets one definition rather than one per TU.
#pragma once

#include <chrono>
#include <cstdint>

namespace wblink {
namespace node {

inline uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace node
}  // namespace wblink
