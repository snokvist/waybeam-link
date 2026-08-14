// SPDX-License-Identifier: GPL-2.0-or-later
// The C ABI's implementation (#109 Phase 3 prep). See rx_node_c.h for why the
// surface is shaped the way it is.
#include "wblink/node/rx_node_c.h"

#include <atomic>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

#include "wblink/node/load.h"
#include "wblink/node/rx_node.h"
#include "wblink/node/rx_runtime_control.h"

// The stop flag a C caller cannot name. Same type app/main.cpp uses, for the
// same reason: lock-free everywhere we build, so it stays legal to set from a
// signal handler while also meaning something across threads.
struct wblink_rx {
    std::atomic<int> stop{0};
    // A handle runs ONCE. `stop` is sticky — `run_rx` takes it by const
    // reference and cannot clear it — so a second run would sail past the
    // loop condition on its first iteration and return 0, after having
    // claimed the adapter. A consumer whose model is start/stop/start (the
    // hub's is) would see a clean return and a dead node. Clearing the flag
    // on entry instead would lose a stop issued between the caller spawning
    // its thread and the thread reaching this function, which is a race a
    // library should not hand its user. One handle per run is neither.
    std::atomic<bool> used{false};
    // Programmatic device source. ORDERING IS THE CALLER'S CONTRACT — the
    // header says "call BEFORE wblink_rx_run", and this field is not
    // protected. The `used` check in the setter is best-effort DETECTION of a
    // violated contract, not a barrier: a setter that reads `used == false`
    // just before run() exchanges it can still be assigning while run() reads
    // this vector, which is a data race. Do not read the `return 3` as
    // thread-safety.
    std::vector<int> adapter_fds;
    // Cross-thread scout/discovery surface. The caller only enqueues commands
    // or copies immutable strings; run_rx alone touches its engine/catalog.
    wblink::node::RxRuntimeControl runtime_control;
    // Pass 177 lifecycle. Written only by wblink_rx_run's wrapper; readable
    // from any thread through wblink_rx_state.
    std::atomic<int> state{WBLINK_NODE_CREATED};
    std::atomic<int> exit_rc{0};
    // Pass 179: config as text, and a scouted selection pinned before the
    // run. Both are call-before-run under the same contract as adapter_fds —
    // the run consumes them and nothing rereads them afterwards.
    std::string config_json;
    bool have_selection = false;
    wblink::node::Selection selection;
};

namespace {

// Marks every exit after the run becomes externally controllable. Config and
// backend failures are exits too; without this, later control calls would keep
// accepting commands into a loop that no longer exists.
class RuntimeRunGuard {
  public:
    explicit RuntimeRunGuard(wblink::node::RxRuntimeControl& control)
        : control_(control) {
        control_.start_run();
    }
    ~RuntimeRunGuard() { control_.stop_run(); }

  private:
    wblink::node::RxRuntimeControl& control_;
};

}  // namespace

extern "C" {

wblink_rx* wblink_rx_create(void) { return new (std::nothrow) wblink_rx(); }

void wblink_rx_request_stop(wblink_rx* rx) {
    if (rx != nullptr) rx->stop.store(1, std::memory_order_relaxed);
}

int wblink_rx_set_adapter_fds(wblink_rx* rx, const int* fds, size_t n) {
    if (rx == nullptr) return 2;
    if (fds == nullptr && n > 0) return 2;
    // Reject after start rather than accept-and-ignore: the config is already
    // consumed by then, so a caller that got the order wrong would otherwise
    // watch the node enumerate by bus path and fail on a device it cannot
    // see, with nothing pointing at the real mistake.
    if (rx->used.load(std::memory_order_relaxed)) return 3;
    // assign() allocates, so bad_alloc and length_error are both reachable
    // from a caller-supplied n. Unwinding through C is undefined rather than
    // merely bad — the same rule wblink_rx_run's two try blocks exist for.
    try {
        rx->adapter_fds.assign(fds, fds + n);
    } catch (...) {
        std::fprintf(stderr,
                     "wblink_rx_set_adapter_fds: allocation failed\n");
        return 1;
    }
    return 0;
}

int wblink_rx_set_config_json(wblink_rx* rx, const char* json) {
    if (rx == nullptr || json == nullptr || *json == '\0') return 2;
    if (rx->used.load(std::memory_order_relaxed)) return 3;
    try {
        rx->config_json.assign(json);
    } catch (...) {
        std::fprintf(stderr, "wblink_rx_set_config_json: allocation failed\n");
        return 1;
    }
    return 0;
}

int wblink_rx_set_selection(wblink_rx* rx, uint16_t originator, uint8_t net_id,
                            uint16_t channel_mhz) {
    if (rx == nullptr) return 2;
    // Bounds are the library's to state, not each consumer's to re-derive:
    // originator 0 is "none" (§12) and a selection naming it is a caller
    // bug, and a 0 MHz channel would silently build a node tuned nowhere.
    if (originator == 0 || channel_mhz == 0) return 2;
    if (rx->used.load(std::memory_order_relaxed)) return 3;
    rx->selection.originator = originator;
    rx->selection.net_id = net_id;
    rx->selection.channel_mhz = channel_mhz;
    rx->have_selection = true;
    return 0;
}

int wblink_rx_scout_start(wblink_rx* rx, const uint16_t* channels,
                          size_t channel_count, uint32_t dwell_ms,
                          uint64_t* generation) {
    if (rx == nullptr) return 2;
    uint64_t ignored = 0;
    return rx->runtime_control.enqueue_scout_start(
        channels, channel_count, dwell_ms,
        generation != nullptr ? generation : &ignored);
}

int wblink_rx_scout_stop(wblink_rx* rx, uint64_t* generation) {
    if (rx == nullptr) return 2;
    uint64_t ignored = 0;
    return rx->runtime_control.enqueue_scout_stop(
        generation != nullptr ? generation : &ignored);
}

int wblink_rx_scout_select(wblink_rx* rx, uint16_t originator,
                           uint64_t* generation) {
    if (rx == nullptr) return 2;
    uint64_t ignored = 0;
    return rx->runtime_control.enqueue_scout_select(
        originator, generation != nullptr ? generation : &ignored);
}

int wblink_rx_claim(wblink_rx* rx, uint16_t originator, uint16_t target_chan,
                    uint64_t* generation) {
    if (rx == nullptr) return 2;
    uint64_t ignored = 0;
    return rx->runtime_control.enqueue_claim(
        originator, target_chan, generation != nullptr ? generation : &ignored);
}

int wblink_rx_vehicle_command(wblink_rx* rx, const char* cmd, int32_t arg,
                              uint64_t* generation) {
    if (rx == nullptr) return 2;
    uint64_t ignored = 0;
    return rx->runtime_control.enqueue_vehicle_command(
        cmd, arg, generation != nullptr ? generation : &ignored);
}

int wblink_rx_command_status(wblink_rx* rx, char* buffer, size_t capacity,
                             size_t* required, uint64_t* applied_generation) {
    if (rx == nullptr) return 2;
    uint64_t ignored = 0;
    return rx->runtime_control.copy_command(
        buffer, capacity, required,
        applied_generation != nullptr ? applied_generation : &ignored);
}

int wblink_rx_scout_results(wblink_rx* rx, char* buffer, size_t capacity,
                            size_t* required, uint64_t* applied_generation) {
    if (rx == nullptr) return 2;
    uint64_t ignored = 0;
    return rx->runtime_control.copy_scout(
        buffer, capacity, required,
        applied_generation != nullptr ? applied_generation : &ignored);
}

int wblink_rx_discovery(wblink_rx* rx, char* buffer, size_t capacity,
                        size_t* required) {
    if (rx == nullptr) return 2;
    return rx->runtime_control.copy_discovery(buffer, capacity, required);
}

int wblink_rx_adapters(wblink_rx* rx, char* buffer, size_t capacity,
                       size_t* required) {
    if (rx == nullptr) return 2;
    return rx->runtime_control.copy_adapters(buffer, capacity, required);
}

int wblink_rx_stats(wblink_rx* rx, char* buffer, size_t capacity,
                    size_t* required) {
    if (rx == nullptr) return 2;
    return rx->runtime_control.copy_stats(buffer, capacity, required);
}

int wblink_rx_health(wblink_rx* rx, char* buffer, size_t capacity,
                     size_t* required) {
    if (rx == nullptr) return 2;
    return rx->runtime_control.copy_health(buffer, capacity, required);
}

int wblink_rx_selection(wblink_rx* rx, char* buffer, size_t capacity,
                        size_t* required, uint64_t* applied_generation) {
    if (rx == nullptr) return 2;
    uint64_t ignored = 0;
    return rx->runtime_control.copy_selection(
        buffer, capacity, required,
        applied_generation != nullptr ? applied_generation : &ignored);
}

// The body of the claimed run, split out so wblink_rx_run can record the
// Pass 177 lifecycle transitions at exactly two points instead of at every
// return site below.
static int wblink_rx_run_claimed(wblink_rx* rx, const char* config_path,
                                 wblink_frame_cb on_frame, void* user) {
    // Stopped before it started: return without loading a config or opening a
    // radio. The header promises this is prompt, and the first read of `stop`
    // inside the loop is ~1800 lines and one AirBackend::create away.
    if (rx->stop.load(std::memory_order_relaxed) != 0) return 0;
    RuntimeRunGuard runtime_guard(rx->runtime_control);

    wblink::node::Loaded loaded;
    try {
        // Exactly one source, guaranteed by the wrapper (Pass 179).
        const int rc = rx->config_json.empty()
                           ? wblink::node::load_all(config_path, loaded)
                           : wblink::node::load_all_json(rx->config_json,
                                                         loaded);
        if (rc != 0) return rc;
        if (rx->have_selection) {
            wblink::node::apply_selection(rx->selection, loaded);
        }
    } catch (...) {
        std::fprintf(stderr, "wblink_rx_run: unhandled C++ exception in load\n");
        return 1;
    }
    // After load_all, so a config can never smuggle these in, and before
    // run_rx, which is where AirBackend::create reads them. Inside a try for
    // the same reason as the assign(): a vector copy allocates.
    try {
        loaded.cfg.adapter_fds = rx->adapter_fds;
    } catch (...) {
        std::fprintf(stderr, "wblink_rx_run: allocation failed\n");
        return 1;
    }

    // Empty when the caller passed no callback, which is the documented way to
    // say "egress goes where the config says" — NOT a sink that drops.
    wblink::node::FrameSink sink;
    if (on_frame != nullptr) {
        sink = [on_frame, user](uint8_t stream_id, const uint8_t* frame,
                                size_t len) {
            on_frame(stream_id, frame, len, user);
        };
    }
    // No C++ exception may cross into a C caller. Nothing below is known to
    // throw — config parsing catches its own — but std::bad_alloc is always
    // reachable, and unwinding through C is undefined rather than merely bad.
    try {
        return wblink::node::run_rx(loaded, rx->stop, sink,
                                    &rx->runtime_control);
    } catch (...) {
        std::fprintf(stderr, "wblink_rx_run: unhandled C++ exception\n");
        return 1;
    }
}

int wblink_rx_run(wblink_rx* rx, const char* config_path,
                  wblink_frame_cb on_frame, void* user) {
    if (rx == nullptr) return 2;
    // Pass 179: exactly one config source. Neither is nothing to run; both
    // is an ambiguity only a guess could resolve, so it is refused rather
    // than silently ranked.
    const bool have_json = !rx->config_json.empty();
    if (config_path == nullptr && !have_json) return 2;  // nothing to run
    if (config_path != nullptr && have_json) return 2;   // two, ambiguous
    if (rx->used.exchange(true)) return 3;  // see the comment on `used`
    // Pass 177: the refusals above never ran and never transition; from the
    // successful claim on, the handle's state is this wrapper's to tell. The
    // rc is stored BEFORE the EXITED flip so a reader that observes EXITED
    // always reads the real code, never the initializer.
    rx->state.store(WBLINK_NODE_RUNNING);
    const int rc = wblink_rx_run_claimed(rx, config_path, on_frame, user);
    rx->exit_rc.store(rc);
    rx->state.store(WBLINK_NODE_EXITED);
    return rc;
}

int wblink_rx_control_endpoint(wblink_rx* rx, char* buffer, size_t capacity,
                               size_t* required) {
    if (rx == nullptr) return 2;
    return rx->runtime_control.copy_control_endpoint(buffer, capacity,
                                                     required);
}

int wblink_rx_state(wblink_rx* rx, int* exit_rc) {
    if (rx == nullptr) return -1;
    const int state = rx->state.load();
    if (state == WBLINK_NODE_EXITED && exit_rc != nullptr) {
        *exit_rc = rx->exit_rc.load();
    }
    return state;
}

void wblink_rx_destroy(wblink_rx* rx) { delete rx; }

}  // extern "C"
