// SPDX-License-Identifier: GPL-2.0-or-later
// The RX C ABI's cross-thread seam without a radio or run_rx: bounded command
// handoff, lifecycle, generation ordering, requested immutable snapshots and
// exact buffer-copy semantics.
#include "wblink/node/rx_runtime_control.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>

#include "wbtest.h"

namespace {

using wblink::node::RxRuntimeControl;

void test_lifecycle_and_latest_intent() {
    RxRuntimeControl ctl;
    uint64_t first = 99;
    const uint16_t channels[] = {5745, 5805};

    CHECK(!ctl.running());
    CHECK(ctl.enqueue_scout_start(channels, 2, 300, &first) == 3);
    CHECK_EQ_U(first, 99);  // refusal does not fabricate a generation

    ctl.start_run();
    CHECK(ctl.running());
    CHECK(ctl.enqueue_scout_start(nullptr, 1, 300, &first) == 2);
    CHECK(ctl.enqueue_scout_start(channels, 2, 300, nullptr) == 2);
    CHECK(ctl.enqueue_scout_start(channels, 2, 300, &first) == 0);
    CHECK_EQ_U(first, 1);

    // One slot, latest intent: lifecycle stop beats an unconsumed start.
    uint64_t second = 0;
    CHECK(ctl.enqueue_scout_stop(&second) == 0);
    CHECK_EQ_U(second, 2);
    auto cmd = ctl.take_command();
    CHECK(cmd.has_value());
    if (cmd) {
        CHECK(cmd->kind == RxRuntimeControl::CommandKind::kScoutStop);
        CHECK(cmd->channels.empty());
        CHECK_EQ_U(cmd->dwell_ms, 0);
        CHECK_EQ_U(cmd->originator, 0);
        CHECK_EQ_U(cmd->generation, second);
        ctl.note_applied(cmd->generation);
    }
    CHECK(!ctl.take_command().has_value());
    CHECK_EQ_U(ctl.applied_generation(), second);

    // A newer start similarly replaces an older one and owns its copied data.
    uint16_t mutable_channels[] = {5180, 5200};
    uint64_t third = 0, fourth = 0;
    CHECK(ctl.enqueue_scout_start(channels, 2, 200, &third) == 0);
    CHECK(ctl.enqueue_scout_start(mutable_channels, 2, 450, &fourth) == 0);
    mutable_channels[0] = 9999;
    cmd = ctl.take_command();
    CHECK(cmd.has_value());
    if (cmd) {
        CHECK(cmd->kind == RxRuntimeControl::CommandKind::kScoutStart);
        CHECK_EQ_U(cmd->generation, fourth);
        CHECK_EQ_U(cmd->dwell_ms, 450);
        CHECK_EQ_U(cmd->channels.size(), 2);
        if (cmd->channels.size() == 2) {
            CHECK_EQ_U(cmd->channels[0], 5180);
            CHECK_EQ_U(cmd->channels[1], 5200);
        }
    }
    CHECK_EQ_U(fourth, third + 1);

    // Selection is just another latest-intent command. The loop, not this
    // mailbox, resolves candidate_for and performs the spectator retune.
    uint64_t selected = 0;
    CHECK(ctl.enqueue_scout_select(0, &selected) == 2);
    CHECK(ctl.enqueue_scout_select(17, nullptr) == 2);
    CHECK(ctl.enqueue_scout_select(17, &selected) == 0);
    cmd = ctl.take_command();
    CHECK(cmd.has_value());
    if (cmd) {
        CHECK(cmd->kind == RxRuntimeControl::CommandKind::kScoutSelect);
        CHECK_EQ_U(cmd->originator, 17);
        CHECK_EQ_U(cmd->generation, selected);
        CHECK(cmd->channels.empty());
    }

    // stop_run discards anything the loop did not take and closes enqueue,
    // while preserving generations already applied.
    CHECK(ctl.enqueue_scout_stop(&fourth) == 0);
    ctl.stop_run();
    CHECK(!ctl.running());
    CHECK(!ctl.take_command().has_value());
    CHECK(ctl.enqueue_scout_stop(&fourth) == 3);
    CHECK_EQ_U(ctl.applied_generation(), second);
}

void test_snapshot_requests_and_copies() {
    RxRuntimeControl ctl;
    size_t required = 77;
    uint64_t generation = 88;

    // A read before publication asks the loop for data and reports not-ready.
    CHECK(ctl.copy_scout(nullptr, 0, &required, &generation) == 3);
    CHECK_EQ_U(required, 0);
    CHECK_EQ_U(generation, 0);
    CHECK(ctl.take_scout_snapshot_request());
    CHECK(!ctl.take_scout_snapshot_request());  // edge consumed exactly once

    CHECK(ctl.copy_discovery(nullptr, 0, &required) == 3);
    CHECK_EQ_U(required, 0);
    CHECK(ctl.take_discovery_snapshot_request());
    CHECK(!ctl.take_discovery_snapshot_request());

    CHECK(ctl.copy_selection(nullptr, 0, &required, &generation) == 3);
    CHECK_EQ_U(required, 0);
    CHECK_EQ_U(generation, 0);
    CHECK(ctl.take_selection_snapshot_request());
    CHECK(!ctl.take_selection_snapshot_request());

    const std::string scout = "{\"scanning\":true}";
    const std::string discovery = "{\"nodes\":[],\"streams\":[]}";
    const std::string selection =
        "{\"state\":\"tuned\",\"originator\":17,\"channel\":5805}";
    ctl.publish_scout(scout, 7);
    ctl.publish_discovery(discovery);
    ctl.publish_selection(selection, 9);

    // Query sizes include NUL, and each poll requests a future fresh value.
    CHECK(ctl.copy_scout(nullptr, 0, &required, &generation) == 0);
    CHECK_EQ_U(required, scout.size() + 1);
    CHECK_EQ_U(generation, 7);
    CHECK(ctl.take_scout_snapshot_request());

    std::array<char, 64> out{};
    out.fill('x');
    CHECK(ctl.copy_scout(out.data(), scout.size(), &required, &generation) == 4);
    CHECK(out.front() == 'x');  // insufficient means no partial JSON
    CHECK(ctl.copy_scout(out.data(), scout.size() + 1, &required,
                         &generation) == 0);
    CHECK(std::strcmp(out.data(), scout.c_str()) == 0);
    CHECK_EQ_U(generation, 7);

    CHECK(ctl.copy_discovery(nullptr, 0, &required) == 0);
    CHECK_EQ_U(required, discovery.size() + 1);
    out.fill('y');
    CHECK(ctl.copy_discovery(out.data(), discovery.size(), &required) == 4);
    CHECK(out.front() == 'y');
    CHECK(ctl.copy_discovery(out.data(), discovery.size() + 1, &required) == 0);
    CHECK(std::strcmp(out.data(), discovery.c_str()) == 0);

    CHECK(ctl.copy_selection(nullptr, 0, &required, &generation) == 0);
    CHECK_EQ_U(required, selection.size() + 1);
    CHECK_EQ_U(generation, 9);
    CHECK(ctl.take_selection_snapshot_request());
    out.fill('z');
    CHECK(ctl.copy_selection(out.data(), selection.size(), &required,
                             &generation) == 4);
    CHECK(out.front() == 'z');
    CHECK(ctl.copy_selection(out.data(), selection.size() + 1, &required,
                             &generation) == 0);
    CHECK(std::strcmp(out.data(), selection.c_str()) == 0);
    CHECK_EQ_U(generation, 9);

    // Argument guards neither crash nor write through missing outputs.
    CHECK(ctl.copy_scout(nullptr, 1, &required, &generation) == 2);
    CHECK(ctl.copy_scout(out.data(), out.size(), nullptr, &generation) == 2);
    CHECK(ctl.copy_scout(out.data(), out.size(), &required, nullptr) == 2);
    CHECK(ctl.copy_discovery(nullptr, 1, &required) == 2);
    CHECK(ctl.copy_discovery(out.data(), out.size(), nullptr) == 2);
    CHECK(ctl.copy_selection(nullptr, 1, &required, &generation) == 2);
    CHECK(ctl.copy_selection(out.data(), out.size(), nullptr, &generation) == 2);
    CHECK(ctl.copy_selection(out.data(), out.size(), &required, nullptr) == 2);

    // Final immutable snapshots remain readable after the run is closed.
    ctl.start_run();
    ctl.stop_run();
    out.fill('\0');
    CHECK(ctl.copy_scout(out.data(), out.size(), &required, &generation) == 0);
    CHECK(std::strcmp(out.data(), scout.c_str()) == 0);
    CHECK_EQ_U(generation, 7);
}

void test_concurrent_publish_and_copy_stays_generation_coupled() {
    RxRuntimeControl ctl;
    ctl.publish_scout("{\"generation\":0}", 0);
    std::atomic<bool> done{false};
    std::atomic<uint64_t> mismatches{0};

    std::thread writer([&] {
        for (uint64_t generation = 1; generation <= 20000; ++generation) {
            ctl.publish_scout(
                "{\"generation\":" + std::to_string(generation) + "}",
                generation);
        }
        done.store(true, std::memory_order_release);
    });
    std::thread reader([&] {
        std::array<char, 64> out{};
        do {
            size_t required = 0;
            uint64_t generation = 0;
            if (ctl.copy_scout(out.data(), out.size(), &required,
                               &generation) != 0) {
                mismatches.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            unsigned long long encoded = 0;
            if (std::sscanf(out.data(), "{\"generation\":%llu}", &encoded) !=
                    1 ||
                encoded != generation) {
                mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        } while (!done.load(std::memory_order_acquire));
    });
    writer.join();
    reader.join();
    CHECK_EQ_U(mismatches.load(std::memory_order_relaxed), 0);
}

}  // namespace

int main() {
    test_lifecycle_and_latest_intent();
    test_snapshot_requests_and_copies();
    test_concurrent_publish_and_copy_stays_generation_coupled();
    return wbtest_finish("rx_runtime_control_test");
}
