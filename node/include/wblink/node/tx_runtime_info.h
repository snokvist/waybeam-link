// SPDX-License-Identifier: GPL-2.0-or-later
// TxRuntimeInfo — the snapshot half of the RX mailbox pattern, for the TX
// node (Pass 174). Publication swaps an immutable string under a mutex; the
// caller only copies. Deliberately NO command half: TX runtime *control* is
// a later pass, and grafting an enqueue mailbox onto this class would
// smuggle in an ownership decision Pass 174 does not make.
//
// Reads remain valid after the run stops, until the handle that owns this
// object is destroyed — same lifetime the RX snapshots promise.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/. All definitions in-class (implicitly inline) per
// tests/node_layering_test.py.
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#include "wblink/node/snapshot_copy.h"

namespace wblink {
namespace node {

class TxRuntimeInfo {
  public:
    // §15.5 (Pass 172/174): the adapters/caps object, published once at
    // backend bring-up — static per die, so that publish IS the freshest
    // state for the life of the run.
    void publish_adapters(std::string json) {
        publish(adapters_, std::move(json));
    }
    // Pass 174: republished at 1 Hz by the run loop, on its own cadence so a
    // node with §15.3 stats disabled does not blind its embedder.
    void publish_status(std::string json) { publish(status_, std::move(json)); }
    // Pass 176: the §15.3 stats line and §15.4 health object, published at
    // the stats cadence from the ONE fill path (emit_stats /
    // build_health_json). stats.hz=0 leaves them unpublished by design — the
    // always-on surface is the status snapshot above.
    void publish_stats(std::string json) { publish(stats_, std::move(json)); }
    void publish_health(std::string json) { publish(health_, std::move(json)); }
    // Pass 178: "addr:port" as resolved from the listening socket, published
    // once, after a successful bind. Not JSON — a bare endpoint string.
    void publish_control_endpoint(std::string endpoint) {
        publish(control_endpoint_, std::move(endpoint));
    }

    // Buffer-copy contract shared with RxRuntimeControl: `required` includes
    // the trailing NUL. NULL + zero is a size query. Returns 0 on
    // success/query, 2 for bad arguments, 3 before the first publication, 4
    // when capacity is short. No partial JSON is copied.
    int copy_adapters(char* buffer, size_t capacity, size_t* required) {
        return copy(adapters_, buffer, capacity, required);
    }
    int copy_status(char* buffer, size_t capacity, size_t* required) {
        return copy(status_, buffer, capacity, required);
    }
    int copy_stats(char* buffer, size_t capacity, size_t* required) {
        return copy(stats_, buffer, capacity, required);
    }
    int copy_health(char* buffer, size_t capacity, size_t* required) {
        return copy(health_, buffer, capacity, required);
    }
    int copy_control_endpoint(char* buffer, size_t capacity,
                              size_t* required) {
        return copy(control_endpoint_, buffer, capacity, required);
    }

  private:
    using Snap = std::shared_ptr<const std::string>;

    void publish(Snap& slot, std::string json) {
        auto next = std::make_shared<const std::string>(std::move(json));
        const std::lock_guard<std::mutex> lock(mutex_);
        slot = std::move(next);
    }

    int copy(const Snap& slot, char* buffer, size_t capacity,
             size_t* required) {
        Snap snapshot;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            snapshot = slot;
        }
        // The contract itself lives once, in snapshot_copy.h (2026-08-14
        // review) — this method only owns the lock.
        return copy_snapshot_json(snapshot, buffer, capacity, required);
    }

    std::mutex mutex_;
    Snap adapters_;
    Snap status_;
    Snap stats_;
    Snap health_;
    Snap control_endpoint_;
};

}  // namespace node
}  // namespace wblink
