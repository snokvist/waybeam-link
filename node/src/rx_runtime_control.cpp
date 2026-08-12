// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/node/rx_runtime_control.h"

#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace wblink {
namespace node {

void RxRuntimeControl::start_run() {
    const std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
}

void RxRuntimeControl::stop_run() {
    const std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    pending_.reset();
}

bool RxRuntimeControl::running() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

int RxRuntimeControl::enqueue_scout_start(const uint16_t* channels, size_t n,
                                          uint32_t dwell_ms,
                                          uint64_t* generation) {
    if (generation == nullptr || (channels == nullptr && n != 0)) return 2;

    // Copy caller-owned memory before taking the mailbox lock. A large or bad
    // allocation cannot stall the RX loop while it takes a command.
    std::vector<uint16_t> copied;
    try {
        if (n != 0) copied.assign(channels, channels + n);
    } catch (...) {
        return 1;  // no C++ exception may escape the eventual C shim
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return 3;
    if (next_generation_ == std::numeric_limits<uint64_t>::max()) return 1;
    const uint64_t gen = ++next_generation_;
    pending_ = Command{CommandKind::kScoutStart, std::move(copied), dwell_ms,
                       0, gen};
    *generation = gen;
    return 0;
}

int RxRuntimeControl::enqueue_scout_stop(uint64_t* generation) {
    if (generation == nullptr) return 2;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return 3;
    if (next_generation_ == std::numeric_limits<uint64_t>::max()) return 1;
    const uint64_t gen = ++next_generation_;
    pending_ = Command{CommandKind::kScoutStop, {}, 0, 0, gen};
    *generation = gen;
    return 0;
}

int RxRuntimeControl::enqueue_scout_select(uint16_t originator,
                                           uint64_t* generation) {
    if (originator == 0 || generation == nullptr) return 2;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return 3;
    if (next_generation_ == std::numeric_limits<uint64_t>::max()) return 1;
    const uint64_t gen = ++next_generation_;
    pending_ = Command{CommandKind::kScoutSelect, {}, 0, originator, gen};
    *generation = gen;
    return 0;
}

std::optional<RxRuntimeControl::Command> RxRuntimeControl::take_command() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !pending_) return std::nullopt;
    std::optional<Command> out(std::move(pending_));
    pending_.reset();
    return out;
}

void RxRuntimeControl::note_applied(uint64_t generation) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (generation > applied_generation_) applied_generation_ = generation;
}

uint64_t RxRuntimeControl::applied_generation() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return applied_generation_;
}

bool RxRuntimeControl::take_scout_snapshot_request() {
    const std::lock_guard<std::mutex> lock(mutex_);
    const bool requested = scout_snapshot_requested_;
    scout_snapshot_requested_ = false;
    return requested;
}

bool RxRuntimeControl::take_discovery_snapshot_request() {
    const std::lock_guard<std::mutex> lock(mutex_);
    const bool requested = discovery_snapshot_requested_;
    discovery_snapshot_requested_ = false;
    return requested;
}

bool RxRuntimeControl::take_selection_snapshot_request() {
    const std::lock_guard<std::mutex> lock(mutex_);
    const bool requested = selection_snapshot_requested_;
    selection_snapshot_requested_ = false;
    return requested;
}

void RxRuntimeControl::publish_scout(std::string json, uint64_t generation) {
    auto next =
        std::make_shared<const GeneratedSnapshot>(std::move(json), generation);
    const std::lock_guard<std::mutex> lock(mutex_);
    scout_snapshot_ = std::move(next);
}

void RxRuntimeControl::publish_discovery(std::string json) {
    auto next = std::make_shared<const std::string>(std::move(json));
    const std::lock_guard<std::mutex> lock(mutex_);
    discovery_snapshot_ = std::move(next);
}

void RxRuntimeControl::publish_selection(std::string json,
                                         uint64_t generation) {
    auto next =
        std::make_shared<const GeneratedSnapshot>(std::move(json), generation);
    const std::lock_guard<std::mutex> lock(mutex_);
    selection_snapshot_ = std::move(next);
}

int RxRuntimeControl::copy_scout(char* buffer, size_t capacity,
                                 size_t* required, uint64_t* generation) {
    if (required == nullptr || generation == nullptr ||
        (buffer == nullptr && capacity != 0)) {
        return 2;
    }

    std::shared_ptr<const GeneratedSnapshot> snapshot;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        scout_snapshot_requested_ = true;
        snapshot = scout_snapshot_;
    }
    if (!snapshot) {
        *required = 0;
        *generation = applied_generation();
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

int RxRuntimeControl::copy_discovery(char* buffer, size_t capacity,
                                     size_t* required) {
    if (required == nullptr || (buffer == nullptr && capacity != 0)) return 2;

    std::shared_ptr<const std::string> snapshot;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        discovery_snapshot_requested_ = true;
        snapshot = discovery_snapshot_;
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

int RxRuntimeControl::copy_selection(char* buffer, size_t capacity,
                                     size_t* required, uint64_t* generation) {
    if (required == nullptr || generation == nullptr ||
        (buffer == nullptr && capacity != 0)) {
        return 2;
    }

    std::shared_ptr<const GeneratedSnapshot> snapshot;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        selection_snapshot_requested_ = true;
        snapshot = selection_snapshot_;
    }
    if (!snapshot) {
        *required = 0;
        *generation = applied_generation();
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
