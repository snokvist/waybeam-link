// SPDX-License-Identifier: GPL-2.0-or-later
// The TX C ABI's implementation (#109 Phase 3). See tx_node_c.h for why the
// surface differs from rx_node_c.h where it does.
#include "wblink/node/tx_node_c.h"

#include <atomic>
#include <cstdio>
#include <new>
#include <string>

#include "wblink/node/load.h"
#include "wblink/node/tx_node.h"

// Same shape as `wblink_rx`, and for the same two reasons: a C caller cannot
// name std::atomic<int>, and a handle must refuse a second run rather than
// sail past a sticky stop flag and report a healthy 0 with the adapter claimed.
struct wblink_tx {
    std::atomic<int> stop{0};
    std::atomic<bool> used{false};
};

// The status space is `run_tx`'s (0/1/2) plus this shim's negatives; the two
// must not collide, because 2 is the §9.10 wedge a supervisor acts on. Asserted
// here rather than trusted, since the two headers can be edited independently.
static_assert(WBLINK_TX_OK == wblink::node::kTxOk, "TX status drift");
static_assert(WBLINK_TX_ERROR == wblink::node::kTxError, "TX status drift");
static_assert(WBLINK_TX_WEDGED == wblink::node::kTxWedged, "TX status drift");
static_assert(WBLINK_TX_BAD_ARG < 0 && WBLINK_TX_REUSED < 0,
              "shim failures must stay out of run_tx's status space");

extern "C" {

wblink_tx* wblink_tx_create(void) { return new (std::nothrow) wblink_tx(); }

void wblink_tx_request_stop(wblink_tx* tx) {
    if (tx != nullptr) tx->stop.store(1, std::memory_order_relaxed);
}

int wblink_tx_run(wblink_tx* tx, const char* config_path,
                  wblink_mode_apply_cb on_mode_apply, void* user) {
    if (tx == nullptr || config_path == nullptr) return WBLINK_TX_BAD_ARG;
    if (tx->used.exchange(true)) return WBLINK_TX_REUSED;
    // Stopped before it started: return without loading a config or opening a
    // radio.
    if (tx->stop.load(std::memory_order_relaxed) != 0) return WBLINK_TX_OK;

    wblink::node::Loaded loaded;
    try {
        if (wblink::node::load_all(config_path, loaded) != 0) {
            // Deliberately NOT `return rc`. load_all's failure is 1 today, but
            // this function's 2 means "the transmitter wedged, restart me", so
            // passing an unrelated function's code through would be one rename
            // away from a supervisor power-cycling a radio over a typo'd path.
            return WBLINK_TX_ERROR;
        }
    } catch (...) {
        std::fprintf(stderr, "wblink_tx_run: unhandled C++ exception in load\n");
        return WBLINK_TX_ERROR;
    }

    // Empty when the caller passed no callback — the documented way to say
    // "this node cannot apply modes". run_tx then fails every apply honestly
    // and §15.5 reports apply_configured false.
    wblink::node::ModeApplyFn mode_apply;
    if (on_mode_apply != nullptr) {
        mode_apply = [on_mode_apply, user](const std::string& cmd,
                                           const std::string& name) {
            return on_mode_apply(cmd.c_str(), name.c_str(), user) != 0;
        };
    }
    // No C++ exception may cross into a C caller: unwinding through C is
    // undefined rather than merely bad. std::bad_alloc is always reachable, and
    // here so is anything the caller's own callback throws back through us.
    try {
        return wblink::node::run_tx(loaded, tx->stop, mode_apply);
    } catch (...) {
        std::fprintf(stderr, "wblink_tx_run: unhandled C++ exception\n");
        return WBLINK_TX_ERROR;
    }
}

void wblink_tx_destroy(wblink_tx* tx) { delete tx; }

}  // extern "C"
