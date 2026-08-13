// SPDX-License-Identifier: GPL-2.0-or-later
// Thread-safe mailbox between an RX node's owner thread and run_rx's event
// loop. The mailbox owns no thread and never exposes loop-owned ScoutEngine or
// DiscoveryCatalog state to another thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wblink {
namespace node {

class RxRuntimeControl {
  public:
    enum class CommandKind : uint8_t {
        kScoutStart,
        kScoutStop,
        kScoutSelect,
        kClaim,           // §11.4 CSA claim of a scouted craft
        kVehicleCommand,  // §11.7 command campaign toward the bound craft
    };

    struct Command {
        CommandKind kind = CommandKind::kScoutStop;
        std::vector<uint16_t> channels;
        uint32_t dwell_ms = 0;
        uint16_t originator = 0;
        uint64_t generation = 0;
        // kClaim: 0 means "the emptiest allowlisted channel", matching the
        // claim helper's own contract rather than inventing a sentinel here.
        uint16_t target_chan = 0;
        // kVehicleCommand: the §15.5 REST spelling, resolved to a §11.7 id by
        // the loop. Kept as a name and not an id because the name map is the
        // control-plane vocabulary and lives in node/vcmd.h, not here.
        std::string cmd;
        int32_t arg = 0;
    };

    // The longest §15.5 command name is 10 characters. A C caller hands us a
    // pointer, so the copy is bounded before it is made rather than after —
    // an unterminated or hostile string must not turn into an allocation.
    static constexpr size_t kMaxCommandName = 32;

    RxRuntimeControl() = default;
    RxRuntimeControl(const RxRuntimeControl&) = delete;
    RxRuntimeControl& operator=(const RxRuntimeControl&) = delete;

    // One control belongs to one run_rx invocation. start_run opens the
    // enqueue side; stop_run closes it and discards a command the loop did not
    // take. Published snapshots deliberately survive stop_run so a caller can
    // inspect the final state after joining the RX thread.
    void start_run();
    void stop_run();
    bool running() const;

    // C-ABI-shaped results: 0 accepted, 1 allocation/internal failure,
    // 2 invalid argument, 3 no running RX loop. The channel array is copied;
    // NULL is legal only when n == 0. A single pending slot implements latest
    // intent: a new start or stop supersedes a command not yet taken by the
    // loop. Accepted commands receive monotonically increasing generations.
    int enqueue_scout_start(const uint16_t* channels, size_t n,
                            uint32_t dwell_ms, uint64_t* generation);
    int enqueue_scout_stop(uint64_t* generation);
    int enqueue_scout_select(uint16_t originator, uint64_t* generation);
    // §11.4 / §11.7. Same convention as the scout enqueues above, and the same
    // single pending slot — a claim supersedes an un-taken sweep, exactly as
    // the loop's own claim path abandons a running one. `target_chan` 0 asks
    // for the emptiest allowlisted channel. `cmd` is copied; it need not
    // outlive the call, and a name longer than kMaxCommandName is rejected as
    // an invalid argument rather than truncated into a different command.
    int enqueue_claim(uint16_t originator, uint16_t target_chan,
                      uint64_t* generation);
    int enqueue_vehicle_command(const char* cmd, int32_t arg,
                                uint64_t* generation);
    std::optional<Command> take_command();

    void note_applied(uint64_t generation);
    uint64_t applied_generation() const;

    // A reader requests a fresh snapshot as part of every copy attempt. The
    // RX loop consumes these edge flags and serializes loop-owned state on its
    // own thread. A request arriving after a take remains set for the next
    // iteration; publish does not clear it.
    bool take_scout_snapshot_request();
    bool take_discovery_snapshot_request();
    bool take_selection_snapshot_request();
    bool take_command_snapshot_request();

    // Publication swaps an immutable string. Scout generation names the
    // command state represented by that JSON and is returned atomically with
    // it by copy_scout.
    void publish_scout(std::string json, uint64_t generation);
    void publish_discovery(std::string json);
    void publish_selection(std::string json, uint64_t generation);
    // The §11.7 campaign state, plus the synchronous verdict of the queued
    // command that carried this generation. A queued caller cannot be handed
    // the helper's return value, so the outcome has to arrive here or nowhere.
    void publish_command(std::string json, uint64_t generation);

    // Buffer-copy contract for the C shim: `required` includes the trailing
    // NUL. NULL + zero is a size query. Returns 0 on success/query, 2 for bad
    // arguments, 3 before the first snapshot, and 4 when capacity is short.
    // No partial JSON is copied. Reads remain valid after stop_run.
    int copy_scout(char* buffer, size_t capacity, size_t* required,
                   uint64_t* generation);
    int copy_discovery(char* buffer, size_t capacity, size_t* required);
    int copy_selection(char* buffer, size_t capacity, size_t* required,
                       uint64_t* generation);
    int copy_command(char* buffer, size_t capacity, size_t* required,
                     uint64_t* generation);

  private:
    struct GeneratedSnapshot {
        explicit GeneratedSnapshot(std::string value, uint64_t gen)
            : json(std::move(value)), generation(gen) {}
        const std::string json;
        const uint64_t generation;
    };

    mutable std::mutex mutex_;
    bool running_ = false;
    uint64_t next_generation_ = 0;
    uint64_t applied_generation_ = 0;
    std::optional<Command> pending_;
    bool scout_snapshot_requested_ = false;
    bool discovery_snapshot_requested_ = false;
    bool selection_snapshot_requested_ = false;
    bool command_snapshot_requested_ = false;
    std::shared_ptr<const GeneratedSnapshot> scout_snapshot_;
    std::shared_ptr<const std::string> discovery_snapshot_;
    std::shared_ptr<const GeneratedSnapshot> selection_snapshot_;
    std::shared_ptr<const GeneratedSnapshot> command_snapshot_;
};

}  // namespace node
}  // namespace wblink
