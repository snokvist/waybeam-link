/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C ABI for the RX node (#109 Phase 3 prep).
 *
 * `run_rx` and its `FrameSink` are C++; the two consumers that want them are
 * not. waybeam-hub is a C daemon that will run a node IN-PROCESS — replacing
 * its frame-SHM ingest, not sitting beside it — and Android's `:wifi` reaches
 * native code through JNI. Without this each writes its own `extern "C"`
 * wrapper: two wrappers, two lifetimes to get wrong, and the stop flag
 * exposed as a C++ type neither can name.
 *
 * The surface is deliberately four functions:
 *
 *   - THE SHIM OWNS THE STOP FLAG. A C caller cannot name `std::atomic<int>`,
 *     and C11's `_Atomic int` is not guaranteed layout-compatible with it, so
 *     handing the flag across the boundary would be a portability bet. An
 *     opaque handle removes the question.
 *
 *   - THE SHIM DOES NOT OWN A THREAD. `wblink_rx_run` blocks; the caller
 *     supplies the thread. That is what `app/main.cpp` does and what the hub's
 *     module model already does, and it keeps `node/` out of thread policy for
 *     the same reason B9 keeps it out of process policy.
 *
 *   - IT TAKES A CONFIG PATH, not a `Loaded`. `Loaded` is a C++ type and
 *     `load_all` already takes a path, so re-exposing it buys nothing.
 *
 * This header must compile as C. `examples/node-linkcheck` builds a C
 * translation unit against it precisely so that a `std::` leaking in here
 * fails a gate rather than a consumer.
 */
#ifndef WBLINK_NODE_RX_NODE_C_H
#define WBLINK_NODE_RX_NODE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque; created and destroyed by the two calls below. */
typedef struct wblink_rx wblink_rx;

/*
 * One whole reassembled frame, ready for a decoder, and the §15.2 stream it
 * arrived on. Called SYNCHRONOUSLY on the RX loop thread, before the next
 * frame is reassembled: copy what you need and return. `frame` does not
 * outlive the call, and a slow callback is backpressure on the receiver.
 */
typedef void (*wblink_frame_cb)(uint8_t stream_id, const uint8_t *frame,
                                size_t len, void *user);

/* NULL on allocation failure. */
wblink_rx *wblink_rx_create(void);

/*
 * Ask a running node to stop. Safe from any thread, and from a signal handler:
 * the flag underneath is a lock-free atomic. Returns immediately — the loop
 * polls, so expect up to one poll period before `wblink_rx_run` returns.
 *
 * Calling it BEFORE `wblink_rx_run` makes that call return 0 without loading a
 * config or opening a radio, so a caller that has already decided to shut down
 * costs nothing.
 */
void wblink_rx_request_stop(wblink_rx *rx);

/*
 * Load `config_path` and run a receiving node until stopped. Blocks.
 *
 * ONE HANDLE RUNS ONCE. A second call on the same handle returns 3 and does
 * nothing: the stop flag is sticky (`run_rx` takes it by const reference), so
 * a re-run would fall straight out of the loop and report a clean 0 having
 * already claimed the adapter — a dead node that looks healthy. Clearing the
 * flag on entry instead would lose a stop issued between spawning a thread and
 * that thread reaching this call. Create a handle per start; that is race-free.
 *
 * Returns 0 on a clean stop (or a pre-stop), 1 on a startup/runtime failure,
 * 2 on a NULL argument, 3 on reuse. `on_frame` may be NULL, in which case
 * egress goes wherever the config says — which, on a build without frame-SHM,
 * is a startup refusal rather than a silent drop.
 *
 * LINKING: this is a C++ library behind a C header. A C consumer must link the
 * C++ runtime — `-lstdc++ -lm` with gcc/clang — and link the archives in
 * order: wblink_node, wblink_io, wblink_core (plus devourer/usb-1.0 when
 * WBLINK_RADIO is on). `wblink::node` is not part of the installed
 * `find_package(wblink)` set; consume the tree with add_subdirectory, or point
 * a Makefile at the build directory.
 */
int wblink_rx_run(wblink_rx *rx, const char *config_path,
                  wblink_frame_cb on_frame, void *user);

/* Frees the handle. Must not be called while `wblink_rx_run` is in flight. */
void wblink_rx_destroy(wblink_rx *rx);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* WBLINK_NODE_RX_NODE_C_H */
