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
#include <vector>

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

    // §15.5 (Pass 172) adapters: not-ready before the bring-up publish, and
    // deliberately NO snapshot-request edge — the fields are static per die,
    // so there is nothing for the loop to refresh.
    CHECK(ctl.copy_adapters(nullptr, 0, &required) == 3);
    CHECK_EQ_U(required, 0);

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

    // §15.5 (Pass 172) adapters: same buffer contract as the calls above —
    // size query includes NUL, undersize copies no partial JSON.
    const std::string adapters =
        "{\"adapters\":[{\"name\":\"a0\",\"chip\":\"udp\"}]}";
    ctl.publish_adapters(adapters);
    CHECK(ctl.copy_adapters(nullptr, 0, &required) == 0);
    CHECK_EQ_U(required, adapters.size() + 1);
    out.fill('z');
    CHECK(ctl.copy_adapters(out.data(), adapters.size(), &required) == 4);
    CHECK(out.front() == 'z');
    CHECK(ctl.copy_adapters(out.data(), adapters.size() + 1, &required) == 0);
    CHECK(std::strcmp(out.data(), adapters.c_str()) == 0);
    CHECK(ctl.copy_adapters(nullptr, 1, &required) == 2);
    CHECK(ctl.copy_adapters(out.data(), out.size(), nullptr) == 2);

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

// §11.4 / §11.7. The mailbox is the only thing standing between a C caller's
// arguments and a loop that will TRANSMIT on them, so the rejections matter as
// much as the acceptances.
void test_control_tx_commands() {
    RxRuntimeControl ctl;
    uint64_t gen = 77;

    // Closed mailbox first: neither verb may fabricate a generation.
    CHECK(ctl.enqueue_claim(1, 0, &gen) == 3);
    CHECK(ctl.enqueue_vehicle_command("arq", 1, &gen) == 3);
    CHECK_EQ_U(gen, 77);

    ctl.start_run();

    CHECK(ctl.enqueue_claim(0, 0, &gen) == 2);       // originator 0
    CHECK(ctl.enqueue_claim(1, 0, nullptr) == 2);
    CHECK(ctl.enqueue_vehicle_command(nullptr, 0, &gen) == 2);
    CHECK(ctl.enqueue_vehicle_command("arq", 0, nullptr) == 2);
    CHECK(ctl.enqueue_vehicle_command("", 0, &gen) == 2);  // empty name
    CHECK_EQ_U(gen, 77);  // every refusal above left it untouched

    // An over-long name is REJECTED, never truncated: a prefix of a long
    // string can spell a different, valid command.
    const std::string too_long(RxRuntimeControl::kMaxCommandName + 1, 'a');
    CHECK(ctl.enqueue_vehicle_command(too_long.c_str(), 0, &gen) == 2);
    // An unterminated buffer must be read no further than the bound. Sized
    // exactly so a read of kMaxCommandName + 1 stays in bounds and finds no
    // NUL, which is the rejection this asserts.
    std::vector<char> unterminated(RxRuntimeControl::kMaxCommandName + 1, 'b');
    CHECK(ctl.enqueue_vehicle_command(unterminated.data(), 0, &gen) == 2);
    CHECK_EQ_U(gen, 77);

    // The longest legal name is accepted at the boundary, not one short of it.
    const std::string at_limit(RxRuntimeControl::kMaxCommandName, 'c');
    CHECK(ctl.enqueue_vehicle_command(at_limit.c_str(), 0, &gen) == 0);
    CHECK_EQ_U(gen, 1);

    uint64_t claim_gen = 0;
    CHECK(ctl.enqueue_claim(4242, 5805, &claim_gen) == 0);
    CHECK_EQ_U(claim_gen, 2);  // supersedes the un-taken command above

    auto cmd = ctl.take_command();
    CHECK(cmd.has_value());
    if (cmd) {
        CHECK(cmd->kind == RxRuntimeControl::CommandKind::kClaim);
        CHECK_EQ_U(cmd->originator, 4242);
        CHECK_EQ_U(cmd->target_chan, 5805);
        CHECK_EQ_U(cmd->generation, claim_gen);
        CHECK(cmd->cmd.empty());
    }
    CHECK(!ctl.take_command().has_value());

    // The name is COPIED — the caller's buffer may die immediately after.
    {
        std::string scratch = "fps_select";
        uint64_t vgen = 0;
        CHECK(ctl.enqueue_vehicle_command(scratch.c_str(), -3, &vgen) == 0);
        scratch = "clobbered";
        cmd = ctl.take_command();
        CHECK(cmd.has_value());
        if (cmd) {
            CHECK(cmd->kind ==
                  RxRuntimeControl::CommandKind::kVehicleCommand);
            CHECK(cmd->cmd == "fps_select");
            CHECK(cmd->arg == -3);  // range is the loop's call, not ours
            CHECK_EQ_U(cmd->generation, vgen);
        }
    }

    // The campaign snapshot follows the same publish/copy contract as the
    // other three, including not-ready before the first publication.
    RxRuntimeControl fresh;
    std::array<char, 64> out{};
    size_t required = 0;
    uint64_t snap_gen = 0;
    CHECK(fresh.copy_command(out.data(), out.size(), &required, &snap_gen) ==
          3);
    CHECK(fresh.take_command_snapshot_request());  // the copy attempt asked
    CHECK(!fresh.take_command_snapshot_request());  // and the edge cleared
    fresh.publish_command("{\"campaign\":{}}", 9);
    CHECK(fresh.copy_command(out.data(), out.size(), &required, &snap_gen) ==
          0);
    CHECK_EQ_U(snap_gen, 9);
    CHECK(std::strcmp(out.data(), "{\"campaign\":{}}") == 0);
    CHECK(fresh.copy_command(out.data(), 3, &required, &snap_gen) == 4);
    CHECK_EQ_U(required, std::strlen("{\"campaign\":{}}") + 1);
}

// Pass 176: stats/health slots — plain snapshot contract, no request edge
// (the loop republishes on the stats.hz beat), unpublished (3) until the
// first beat, and readable after stop_run like every other snapshot.
void test_stats_and_health_slots() {
    wblink::node::RxRuntimeControl ctl;
    size_t required = 77;
    std::array<char, 64> out{};
    CHECK(ctl.copy_stats(nullptr, 0, &required) == 3);
    CHECK_EQ_U(required, 0);
    CHECK(ctl.copy_health(nullptr, 0, &required) == 3);

    ctl.start_run();
    ctl.publish_stats("{\"t_ms\":9}");
    ctl.publish_health("{\"state\":\"RAISE\"}");
    ctl.stop_run();
    // Survives stop_run — the embedder inspects the final link view after
    // joining the RX thread, exactly as the scout snapshots promise.
    CHECK(ctl.copy_stats(out.data(), out.size(), &required) == 0);
    CHECK(std::strcmp(out.data(), "{\"t_ms\":9}") == 0);
    CHECK(ctl.copy_health(out.data(), out.size(), &required) == 0);
    CHECK(std::strcmp(out.data(), "{\"state\":\"RAISE\"}") == 0);
    CHECK(ctl.copy_stats(out.data(), 4, &required) == 4);
    CHECK_EQ_U(required, std::strlen("{\"t_ms\":9}") + 1);
    CHECK(ctl.copy_health(nullptr, 1, &required) == 2);
    CHECK(ctl.copy_health(out.data(), out.size(), nullptr) == 2);

    // 2026-08-14 review (D2): a REFUSED copy must not arm the snapshot
    // request. The Pass 176 fold briefly inverted this, which would have made
    // a caller polling with a bad argument drive the RX loop to re-serialise
    // scout/selection/command JSON on every iteration, forever, for answers
    // it never receives. Assert the flag stays clear, not just the return.
    {
        wblink::node::RxRuntimeControl fresh;
        uint64_t gen = 0;
        std::array<char, 32> buf{};
        CHECK(fresh.copy_scout(buf.data(), buf.size(), nullptr, &gen) == 2);
        CHECK(fresh.copy_selection(buf.data(), buf.size(), nullptr, &gen) == 2);
        CHECK(fresh.copy_command(buf.data(), buf.size(), nullptr, &gen) == 2);
        CHECK(fresh.copy_scout(buf.data(), buf.size(), &required, nullptr) == 2);
        CHECK(!fresh.take_scout_snapshot_request());
        CHECK(!fresh.take_selection_snapshot_request());
        CHECK(!fresh.take_command_snapshot_request());
        // Control: a well-formed copy DOES arm it, so the assertions above
        // cannot pass because the flags never work.
        CHECK(fresh.copy_scout(nullptr, 0, &required, &gen) == 3);
        CHECK(fresh.take_scout_snapshot_request());
    }

    // Pass 178: the control endpoint is a bare "addr:port" string, not JSON,
    // and it round-trips through the same contract.
    CHECK(ctl.copy_control_endpoint(nullptr, 0, &required) == 3);
    ctl.publish_control_endpoint("127.0.0.1:8092");
    CHECK(ctl.copy_control_endpoint(out.data(), out.size(), &required) == 0);
    CHECK(std::strcmp(out.data(), "127.0.0.1:8092") == 0);
    CHECK_EQ_U(required, std::strlen("127.0.0.1:8092") + 1);
}

}  // namespace

int main() {
    test_lifecycle_and_latest_intent();
    test_snapshot_requests_and_copies();
    test_concurrent_publish_and_copy_stays_generation_coupled();
    test_control_tx_commands();
    test_stats_and_health_slots();
    return wbtest_finish("rx_runtime_control_test");
}
