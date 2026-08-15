// SPDX-License-Identifier: GPL-2.0-or-later
// The TX C ABI's implementation (#109 Phase 3). See tx_node_c.h for why the
// surface differs from rx_node_c.h where it does.
#include "wblink/node/tx_node_c.h"

#include <atomic>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

#include "wblink/log.h"
#include "wblink/node/load.h"
#include "wblink/node/tx_node.h"
#include "wblink/node/tx_runtime_info.h"

// Same shape as `wblink_rx`, and for the same two reasons: a C caller cannot
// name std::atomic<int>, and a handle must refuse a second run rather than
// sail past a sticky stop flag and report a healthy 0 with the adapter claimed.
struct wblink_tx {
    std::atomic<int> stop{0};
    std::atomic<bool> used{false};
    // Programmatic device source (Pass 173). Same caveat as the RX twin: the
    // `used` check in the setter is best-effort DETECTION of a violated
    // call-before-run contract, not a barrier.
    std::vector<int> adapter_fds;
    // Cross-thread status/adapters surface (Pass 174). The caller only
    // copies immutable strings; run_tx alone publishes.
    wblink::node::TxRuntimeInfo runtime_info;
    // Pass 177 lifecycle. Written only by wblink_tx_run's wrapper; readable
    // from any thread through wblink_tx_state.
    std::atomic<int> state{WBLINK_NODE_CREATED};
    std::atomic<int> exit_rc{0};
    // Pass 179: config as text. No selection twin — pinning a scouted craft
    // is what a RECEIVER does; a craft is the thing being selected.
    std::string config_json;
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

int wblink_tx_set_config_json(wblink_tx* tx, const char* json) {
    if (tx == nullptr) return 2;
    if (tx->used.load(std::memory_order_relaxed)) return 3;
    if (json == nullptr || *json == '\0') return 2;
    try {
        tx->config_json.assign(json);
    } catch (...) {
        wblink::wb_logf("wblink_tx_set_config_json: allocation failed\n");
        return 1;
    }
    return 0;
}

int wblink_tx_set_adapter_fds(wblink_tx* tx, const int* fds, size_t n) {
    if (tx == nullptr) return 2;
    if (fds == nullptr && n > 0) return 2;
    // Reject after start rather than accept-and-ignore — same reasoning as
    // the RX twin: the config is consumed by then, and a caller with the
    // order wrong would watch enumerate-by-bus-path fail on a device it
    // cannot see, with nothing pointing at the real mistake.
    if (tx->used.load(std::memory_order_relaxed)) return 3;
    // assign() allocates, so bad_alloc and length_error are both reachable
    // from a caller-supplied n, and unwinding through C is undefined.
    try {
        tx->adapter_fds.assign(fds, fds + n);
    } catch (...) {
        wblink::wb_logf("wblink_tx_set_adapter_fds: allocation failed\n");
        return 1;
    }
    return 0;
}

// The body of the claimed run, split out so wblink_tx_run can record the
// Pass 177 lifecycle transitions at exactly two points instead of at every
// return site below.
static int wblink_tx_run_claimed(wblink_tx* tx, const char* config_path,
                                 wblink_mode_apply_cb on_mode_apply,
                                 void* user) {
    // Stopped before it started: return without loading a config or opening a
    // radio.
    if (tx->stop.load(std::memory_order_relaxed) != 0) return WBLINK_TX_OK;

    wblink::node::Loaded loaded;
    try {
        // Exactly one source, guaranteed by the wrapper (Pass 179).
        const int lrc = tx->config_json.empty()
                            ? wblink::node::load_all(config_path, loaded)
                            : wblink::node::load_all_json(tx->config_json,
                                                          loaded);
        if (lrc != 0) {
            // Deliberately NOT `return rc`. load_all's failure is 1 today, but
            // this function's 2 means "the transmitter wedged, restart me", so
            // passing an unrelated function's code through would be one rename
            // away from a supervisor power-cycling a radio over a typo'd path.
            return WBLINK_TX_ERROR;
        }
    } catch (...) {
        wblink::wb_logf("wblink_tx_run: unhandled C++ exception in load\n");
        return WBLINK_TX_ERROR;
    }
    // After load_all, so a config can never smuggle these in, and before
    // run_tx, which is where AirBackend::create reads them (Pass 173 — the
    // same ordering the RX shim documents). In a try because a vector copy
    // allocates.
    try {
        loaded.cfg.adapter_fds = tx->adapter_fds;
    } catch (...) {
        wblink::wb_logf("wblink_tx_run: allocation failed\n");
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
        return wblink::node::run_tx(loaded, tx->stop, mode_apply,
                                    &tx->runtime_info);
    } catch (...) {
        wblink::wb_logf("wblink_tx_run: unhandled C++ exception\n");
        return WBLINK_TX_ERROR;
    }
}

int wblink_tx_run(wblink_tx* tx, const char* config_path,
                  wblink_mode_apply_cb on_mode_apply, void* user) {
    if (tx == nullptr) return WBLINK_TX_BAD_ARG;
    // Pass 179: exactly one config source — see the RX twin.
    const bool have_json = !tx->config_json.empty();
    if (config_path == nullptr && !have_json) return WBLINK_TX_BAD_ARG;
    if (config_path != nullptr && have_json) return WBLINK_TX_BAD_ARG;
    if (tx->used.exchange(true)) return WBLINK_TX_REUSED;
    // Pass 177: the refusals above never ran and never transition; from the
    // successful claim on, the handle's state is this wrapper's to tell. The
    // rc is stored BEFORE the EXITED flip so a reader that observes EXITED
    // always reads the real code, never the initializer.
    tx->state.store(WBLINK_NODE_RUNNING);
    const int rc = wblink_tx_run_claimed(tx, config_path, on_mode_apply, user);
    tx->exit_rc.store(rc);
    tx->state.store(WBLINK_NODE_EXITED);
    return rc;
}

int wblink_tx_adapters(wblink_tx* tx, char* buffer, size_t capacity,
                       size_t* required) {
    if (tx == nullptr) return 2;
    return tx->runtime_info.copy_adapters(buffer, capacity, required);
}

int wblink_tx_status(wblink_tx* tx, char* buffer, size_t capacity,
                     size_t* required) {
    if (tx == nullptr) return 2;
    return tx->runtime_info.copy_status(buffer, capacity, required);
}

int wblink_tx_stats(wblink_tx* tx, char* buffer, size_t capacity,
                    size_t* required) {
    if (tx == nullptr) return 2;
    return tx->runtime_info.copy_stats(buffer, capacity, required);
}

int wblink_tx_health(wblink_tx* tx, char* buffer, size_t capacity,
                     size_t* required) {
    if (tx == nullptr) return 2;
    return tx->runtime_info.copy_health(buffer, capacity, required);
}

int wblink_tx_control_endpoint(wblink_tx* tx, char* buffer, size_t capacity,
                               size_t* required) {
    if (tx == nullptr) return 2;
    return tx->runtime_info.copy_control_endpoint(buffer, capacity, required);
}

int wblink_tx_state(wblink_tx* tx, int* exit_rc) {
    if (tx == nullptr) return -1;
    const int state = tx->state.load();
    if (state == WBLINK_NODE_EXITED && exit_rc != nullptr) {
        *exit_rc = tx->exit_rc.load();
    }
    return state;
}

void wblink_tx_destroy(wblink_tx* tx) { delete tx; }

}  // extern "C"
