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

// A snapshot stamped with the command generation that produced it (the RX
// scout/selection/command surfaces). Immutable after construction, same as
// the plain string snapshot.
struct GeneratedSnapshot {
    explicit GeneratedSnapshot(std::string value, uint64_t gen)
        : json(std::move(value)), generation(gen) {}
    const std::string json;
    const uint64_t generation;
};

// The generation-carrying variant of the contract above (Pass 176, folding
// the three hand-written RX bodies). Identical return set and buffer rules;
// additionally `generation` is mandatory and always written — the snapshot's
// own stamp when one exists, `fallback_generation` (the caller's latest
// applied generation) on 3, so a poller can tell "no result yet for the
// command I issued" from "a result for an older command".
inline int copy_generated_snapshot_json(
    const std::shared_ptr<const GeneratedSnapshot>& snapshot,
    uint64_t fallback_generation, char* buffer, size_t capacity,
    size_t* required, uint64_t* generation) {
    if (required == nullptr || generation == nullptr ||
        (buffer == nullptr && capacity != 0)) {
        return 2;
    }
    if (!snapshot) {
        *required = 0;
        *generation = fallback_generation;
        return 3;
    }
    if (snapshot->json.size() == std::numeric_limits<size_t>::max()) return 1;
    const size_t need = snapshot->json.size() + 1;
    *required = need;
    *generation = snapshot->generation;
    if (buffer == nullptr) return capacity == 0 ? 0 : 2;
    if (capacity < need) return 4;
    std::memcpy(buffer, snapshot->json.c_str(), need);
    return 0;
}

}  // namespace node
}  // namespace wblink
