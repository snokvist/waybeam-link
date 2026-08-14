// SPDX-License-Identifier: GPL-2.0-or-later
// The ONE C-ABI snapshot buffer-copy contract (2026-08-14 review): `required`
// includes the trailing NUL, NULL + zero capacity is a size query, no partial
// JSON is ever copied, and the return set is 0 (query/copy), 2 (bad
// arguments), 3 (no snapshot yet), 4 (capacity short) — with 1 reserved for
// an internal size overflow no real snapshot can reach. Seven hand-written
// bodies had grown from this one contract across RxRuntimeControl and
// TxRuntimeInfo; a fix applied to six of them would silently fork the ABI
// between the wblink_rx_* and wblink_tx_* getters, so the decision lives
// here once. The caller fetches `snapshot` under its own lock (and performs
// any request-flag side effect there); this helper only implements the copy.
#pragma once

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace wblink {
namespace node {

inline int copy_snapshot_json(
    const std::shared_ptr<const std::string>& snapshot, char* buffer,
    size_t capacity, size_t* required) {
    if (required == nullptr || (buffer == nullptr && capacity != 0)) return 2;
    if (!snapshot) {
        *required = 0;
        return 3;
    }
    if (snapshot->size() == std::numeric_limits<size_t>::max()) return 1;
    const size_t need = snapshot->size() + 1;
    *required = need;
    if (buffer == nullptr) return capacity == 0 ? 0 : 2;
    if (capacity < need) return 4;
    std::memcpy(buffer, snapshot->c_str(), need);
    return 0;
}

}  // namespace node
}  // namespace wblink
