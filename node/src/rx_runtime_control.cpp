// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/node/rx_runtime_control.h"

#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "wblink/node/snapshot_copy.h"

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
    Command cmd;
    cmd.kind = CommandKind::kScoutStart;
    cmd.channels = std::move(copied);
    cmd.dwell_ms = dwell_ms;
    cmd.generation = gen;
    pending_ = std::move(cmd);
    *generation = gen;
    return 0;
}

int RxRuntimeControl::enqueue_scout_stop(uint64_t* generation) {
    if (generation == nullptr) return 2;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return 3;
    if (next_generation_ == std::numeric_limits<uint64_t>::max()) return 1;
    const uint64_t gen = ++next_generation_;
    Command cmd;
    cmd.kind = CommandKind::kScoutStop;
    cmd.generation = gen;
    pending_ = std::move(cmd);
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
    Command cmd;
    cmd.kind = CommandKind::kScoutSelect;
    cmd.originator = originator;
    cmd.generation = gen;
    pending_ = std::move(cmd);
    *generation = gen;
    return 0;
}

int RxRuntimeControl::enqueue_claim(uint16_t originator, uint16_t target_chan,
                                    uint64_t* generation) {
    if (originator == 0 || generation == nullptr) return 2;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return 3;
    if (next_generation_ == std::numeric_limits<uint64_t>::max()) return 1;
    const uint64_t gen = ++next_generation_;
    Command cmd;
    cmd.kind = CommandKind::kClaim;
    cmd.originator = originator;
    cmd.target_chan = target_chan;
    cmd.generation = gen;
    pending_ = std::move(cmd);
    *generation = gen;
    return 0;
}

int RxRuntimeControl::enqueue_vehicle_command(const char* cmd, int32_t arg,
                                              uint64_t* generation) {
    if (cmd == nullptr || generation == nullptr) return 2;
    // Bound the read before the copy: strlen on a caller-owned pointer with no
    // terminator inside the bound is the failure this exists to avoid.
    size_t len = 0;
    while (len <= kMaxCommandName && cmd[len] != '\0') ++len;
    if (len == 0 || len > kMaxCommandName) return 2;

    // Copy outside the lock, as enqueue_scout_start does, so a bad allocation
    // cannot stall the RX loop while it takes a command.
    std::string name;
    try {
        name.assign(cmd, len);
    } catch (...) {
        return 1;  // no C++ exception may escape the eventual C shim
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return 3;
    if (next_generation_ == std::numeric_limits<uint64_t>::max()) return 1;
    const uint64_t gen = ++next_generation_;
    Command out;
    out.kind = CommandKind::kVehicleCommand;
    out.cmd = std::move(name);
    out.arg = arg;
    out.generation = gen;
    pending_ = std::move(out);
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

bool RxRuntimeControl::take_command_snapshot_request() {
    const std::lock_guard<std::mutex> lock(mutex_);
    const bool requested = command_snapshot_requested_;
    command_snapshot_requested_ = false;
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

void RxRuntimeControl::publish_adapters(std::string json) {
    auto next = std::make_shared<const std::string>(std::move(json));
    const std::lock_guard<std::mutex> lock(mutex_);
    adapters_snapshot_ = std::move(next);
}

void RxRuntimeControl::publish_selection(std::string json,
                                         uint64_t generation) {
    auto next =
        std::make_shared<const GeneratedSnapshot>(std::move(json), generation);
    const std::lock_guard<std::mutex> lock(mutex_);
    selection_snapshot_ = std::move(next);
}

void RxRuntimeControl::publish_command(std::string json, uint64_t generation) {
    auto next =
        std::make_shared<const GeneratedSnapshot>(std::move(json), generation);
    const std::lock_guard<std::mutex> lock(mutex_);
    command_snapshot_ = std::move(next);
}

int RxRuntimeControl::copy_scout(char* buffer, size_t capacity,
                                 size_t* required, uint64_t* generation) {
    // Validate BEFORE the lock: a refused call must not arm the snapshot
    // request, or a caller polling with a bad argument would drive the RX
    // loop to re-serialise on every iteration for answers it never gets.
    if (required == nullptr || generation == nullptr ||
        (buffer == nullptr && capacity != 0)) {
        return 2;
    }
    std::shared_ptr<const GeneratedSnapshot> snapshot;
    uint64_t applied = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        scout_snapshot_requested_ = true;
        snapshot = scout_snapshot_;
        // Same acquire as the snapshot: one lock, and a fallback that cannot
        // be torn against the value it stands in for.
        applied = applied_generation_;
    }
    // Contract in snapshot_copy.h (Pass 176 fold); this method owns only the
    // lock and the request-flag side effect — same split as the plain copies.
    return copy_generated_snapshot_json(snapshot, applied, buffer, capacity,
                                        required, generation);
}

int RxRuntimeControl::copy_discovery(char* buffer, size_t capacity,
                                     size_t* required) {
    std::shared_ptr<const std::string> snapshot;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        discovery_snapshot_requested_ = true;
        snapshot = discovery_snapshot_;
    }
    // Contract in snapshot_copy.h (2026-08-14 review); this method owns only
    // the lock and the request-flag side effect.
    return copy_snapshot_json(snapshot, buffer, capacity, required);
}

int RxRuntimeControl::copy_adapters(char* buffer, size_t capacity,
                                    size_t* required) {
    std::shared_ptr<const std::string> snapshot;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // No request flag: the loop republishes on its own ~1 Hz cadence
        // (Pass 172 review fix — the caps are static but `channel` is live).
        snapshot = adapters_snapshot_;
    }
    return copy_snapshot_json(snapshot, buffer, capacity, required);
}

int RxRuntimeControl::copy_selection(char* buffer, size_t capacity,
                                     size_t* required, uint64_t* generation) {
    // Validate BEFORE the lock: a refused call must not arm the snapshot
    // request, or a caller polling with a bad argument would drive the RX
    // loop to re-serialise on every iteration for answers it never gets.
    if (required == nullptr || generation == nullptr ||
        (buffer == nullptr && capacity != 0)) {
        return 2;
    }
    std::shared_ptr<const GeneratedSnapshot> snapshot;
    uint64_t applied = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        selection_snapshot_requested_ = true;
        snapshot = selection_snapshot_;
        // Same acquire as the snapshot: one lock, and a fallback that cannot
        // be torn against the value it stands in for.
        applied = applied_generation_;
    }
    return copy_generated_snapshot_json(snapshot, applied, buffer, capacity,
                                        required, generation);
}

int RxRuntimeControl::copy_command(char* buffer, size_t capacity,
                                   size_t* required, uint64_t* generation) {
    // Validate BEFORE the lock: a refused call must not arm the snapshot
    // request, or a caller polling with a bad argument would drive the RX
    // loop to re-serialise on every iteration for answers it never gets.
    if (required == nullptr || generation == nullptr ||
        (buffer == nullptr && capacity != 0)) {
        return 2;
    }
    std::shared_ptr<const GeneratedSnapshot> snapshot;
    uint64_t applied = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        command_snapshot_requested_ = true;
        snapshot = command_snapshot_;
        // Same acquire as the snapshot: one lock, and a fallback that cannot
        // be torn against the value it stands in for.
        applied = applied_generation_;
    }
    return copy_generated_snapshot_json(snapshot, applied, buffer, capacity,
                                        required, generation);
}

void RxRuntimeControl::publish_stats(std::string json) {
    auto next = std::make_shared<const std::string>(std::move(json));
    const std::lock_guard<std::mutex> lock(mutex_);
    stats_snapshot_ = std::move(next);
}

void RxRuntimeControl::publish_health(std::string json) {
    auto next = std::make_shared<const std::string>(std::move(json));
    const std::lock_guard<std::mutex> lock(mutex_);
    health_snapshot_ = std::move(next);
}

int RxRuntimeControl::copy_stats(char* buffer, size_t capacity,
                                 size_t* required) {
    std::shared_ptr<const std::string> snapshot;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // No request flag: the loop republishes on the stats.hz beat
        // (Pass 176), and stats.hz=0 leaves this unpublished by design.
        snapshot = stats_snapshot_;
    }
    return copy_snapshot_json(snapshot, buffer, capacity, required);
}

int RxRuntimeControl::copy_health(char* buffer, size_t capacity,
                                  size_t* required) {
    std::shared_ptr<const std::string> snapshot;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        snapshot = health_snapshot_;
    }
    return copy_snapshot_json(snapshot, buffer, capacity, required);
}

void RxRuntimeControl::publish_control_endpoint(std::string endpoint) {
    auto next = std::make_shared<const std::string>(std::move(endpoint));
    const std::lock_guard<std::mutex> lock(mutex_);
    control_endpoint_ = std::move(next);
}

int RxRuntimeControl::copy_control_endpoint(char* buffer, size_t capacity,
                                            size_t* required) {
    std::shared_ptr<const std::string> snapshot;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        snapshot = control_endpoint_;
    }
    return copy_snapshot_json(snapshot, buffer, capacity, required);
}

}  // namespace node
}  // namespace wblink
