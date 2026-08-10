// SPDX-License-Identifier: GPL-2.0-or-later
// The C ABI's implementation (#109 Phase 3 prep). See rx_node_c.h for why the
// surface is shaped the way it is.
#include "wblink/node/rx_node_c.h"

#include <atomic>
#include <cstdio>
#include <new>
#include <vector>

#include "wblink/node/load.h"
#include "wblink/node/rx_node.h"

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
};

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

int wblink_rx_run(wblink_rx* rx, const char* config_path,
                  wblink_frame_cb on_frame, void* user) {
    if (rx == nullptr || config_path == nullptr) return 2;
    if (rx->used.exchange(true)) return 3;  // see the comment on `used`
    // Stopped before it started: return without loading a config or opening a
    // radio. The header promises this is prompt, and the first read of `stop`
    // inside the loop is ~1800 lines and one AirBackend::create away.
    if (rx->stop.load(std::memory_order_relaxed) != 0) return 0;

    wblink::node::Loaded loaded;
    try {
        if (const int rc = wblink::node::load_all(config_path, loaded);
            rc != 0) {
            return rc;
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
        return wblink::node::run_rx(loaded, rx->stop, sink);
    } catch (...) {
        std::fprintf(stderr, "wblink_rx_run: unhandled C++ exception\n");
        return 1;
    }
}

void wblink_rx_destroy(wblink_rx* rx) { delete rx; }

}  // extern "C"
