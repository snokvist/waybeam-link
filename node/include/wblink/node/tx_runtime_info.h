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
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

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

  private:
    using Snap = std::shared_ptr<const std::string>;

    void publish(Snap& slot, std::string json) {
        auto next = std::make_shared<const std::string>(std::move(json));
        const std::lock_guard<std::mutex> lock(mutex_);
        slot = std::move(next);
    }

    int copy(const Snap& slot, char* buffer, size_t capacity,
             size_t* required) {
        if (required == nullptr || (buffer == nullptr && capacity != 0)) {
            return 2;
        }
        Snap snapshot;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            snapshot = slot;
        }
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

    std::mutex mutex_;
    Snap adapters_;
    Snap status_;
};

}  // namespace node
}  // namespace wblink
