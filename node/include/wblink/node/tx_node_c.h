/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C ABI for the TX node (#109 Phase 3).
 *
 * The sibling of `rx_node_c.h`, and it exists for the same consumer: waybeam-hub
 * is a C daemon that runs a node IN-PROCESS. On the ground that node receives;
 * on the vehicle it transmits, and the vehicle keeps its venc->link frame-SHM
 * ingest either way — only the ground's link->hub ring goes away.
 *
 * The three shim decisions argued in `rx_node_c.h` hold unchanged here: the
 * shim owns the stop flag (a C caller cannot name `std::atomic<int>`), the shim
 * owns no thread (the caller supplies it, as `app/main.cpp` does), and it takes
 * a config path rather than a `Loaded`. What follows is what is DIFFERENT,
 * because the differences are the whole reason this is not a copy.
 *
 * 1. IT CARRIES A CALLBACK IN, not just out. `run_tx` needs a way to apply an
 *    operating mode (§15.5 / §11.7 0x07), and the app-side implementation
 *    double-forks — which B9 forbids `node/` from doing. So the fork stays in
 *    the caller and arrives here as a function pointer plus `void *user`, the
 *    same shape `wblink_frame_cb` uses in the other direction. A caller that
 *    passes NULL is a node that cannot apply modes; see below.
 *
 * 2. THE RETURN CODES ARE NOT rx's, AND MUST NOT BE. `run_tx` already owns 0,
 *    1 and 2 as *link* statuses — 2 is the §9.10 wedge (Pass 148), which
 *    `app/main.cpp` maps to exit 9 and which a supervisor acts on by
 *    re-exec'ing the node. `wblink_rx_run` uses 2 for "NULL argument" and 3 for
 *    "handle reused", and copying that here would make a caller's own NULL
 *    pointer indistinguishable from a wedged transmitter: the supervisor would
 *    restart the radio in response to a programming error. Shim-level failures
 *    are therefore NEGATIVE, and the non-negative space is left entirely to
 *    `run_tx`. Read the constants, do not assume the numbering.
 *
 * 3. IT IS NOT IN EVERY BUILD. `run_tx` uses frame-SHM, the REST control server
 *    and venc actuation unconditionally, so these four symbols exist only when
 *    WBLINK_FRAME_SHM, WBLINK_CONTROL_SERVER and WBLINK_VENC are all ON — the
 *    same three WBLINK_BUILD_APP already requires. A receive-only consumer
 *    (Android's `:wifi` on bionic, which has no shm_open) links `wblink::node`
 *    with them OFF and gets the RX half alone. This header still compiles
 *    there; the reference is what fails, and it fails at LINK time with a
 *    named symbol rather than silently.
 */
#ifndef WBLINK_NODE_TX_NODE_C_H
#define WBLINK_NODE_TX_NODE_C_H

#include <stddef.h> /* size_t (Pass 173: wblink_tx_set_adapter_fds) */

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque; created and destroyed by the two calls below. */
typedef struct wblink_tx wblink_tx;

/*
 * `wblink_tx_run`'s result. The non-negative values are `run_tx`'s own status
 * (`wblink/node/tx_node.h`), passed through unchanged; the negative ones are
 * this shim's. See note 2 in the header comment for why they cannot overlap.
 */
enum {
    WBLINK_TX_OK = 0,     /* stopped cleanly, or was stopped before starting */
    WBLINK_TX_ERROR = 1,  /* could not start, or the run loop gave up */
    WBLINK_TX_WEDGED = 2, /* §9.10 (Pass 148): the transmitter wedged and is
                           * asking to be restarted. app/main.cpp turns this
                           * into exit 9; an in-process consumer is free to do
                           * something less drastic, but it must not treat it
                           * as a plain error — that is the distinction the
                           * Pass 148 contract exists to make. */
    WBLINK_TX_BAD_ARG = -1, /* NULL handle or NULL config path */
    WBLINK_TX_REUSED = -2   /* this handle has already run; see below */
};

/*
 * Apply an operating mode by name. `cmd` is the configured applier
 * (`venc.mode_apply_cmd`) and `name` a charset-validated mode name. Return
 * NONZERO if the apply was started, zero if it could not be.
 *
 * Called SYNCHRONOUSLY from the flight loop, so it MUST NOT BLOCK — applying a
 * mode restarts venc, which takes seconds. `app/main.cpp`'s implementation
 * double-forks and returns immediately; a consumer that cannot spawn processes
 * should pass NULL rather than block here. The flight loop is the thread that
 * called wblink_tx_run — no dispatch thread exists, so every apply arrives on
 * that one thread for the life of the run (Pass 172).
 *
 * NEITHER POINTER OUTLIVES THE CALL — the same rule `wblink_frame_cb` states
 * for its frame, and it needs saying twice as loudly here because the two
 * sentences above push an implementer toward "note it down and handle it
 * later". `name` in particular is a function-local built per request; copy it
 * before deferring anything. `cmd` happens to be stable today (it is the
 * configured applier path), which only means a consumer that stashes it will
 * work until it does not.
 *
 * PASSING NULL IS A REAL ANSWER, NOT A DEFAULT. It means "this node cannot
 * apply modes": every apply request fails honestly, and §15.5's
 * `apply_configured` reports false even when `venc.mode_apply_cmd` is set in
 * the config, so the ground's mode control is disabled rather than enabled and
 * broken. That distinction was a review finding on the lift (#164), not a
 * design flourish.
 */
typedef int (*wblink_mode_apply_cb)(const char *cmd, const char *name,
                                    void *user);

/* NULL on allocation failure. */
wblink_tx *wblink_tx_create(void);

/*
 * Pre-opened USB device fds for the node's adapters, one per config
 * `adapters[]` slot, with -1 meaning "enumerate this one by bus path". Call
 * BEFORE `wblink_tx_run`; a call after it has started is ignored and returns
 * 3. The TX twin of `wblink_rx_set_adapter_fds` under the same 2026-08-08
 * contract (Pass 173): a call, not a config key; OWNERSHIP STAYS WITH THE
 * CALLER (libusb marks a wrapped handle `fd_keep`, so teardown closes the
 * handle and leaves the fd open — each fd must stay valid for the whole run
 * and be closed by the caller afterwards; the array itself is copied);
 * supplying any fd forces the bring-up `libusb_reset_device` off, because a
 * wrapped fd must not be reset. Everything else — a daemon reading JSON —
 * should not call this at all: empty (the default) is enumerate-by-bus-path,
 * byte for byte today's behaviour.
 *
 * Returns 0 on success, 1 if copying the array failed to allocate, 2 on a
 * NULL handle or a NULL `fds` with `n > 0`, 3 if the node has already been
 * started. Passing n == 0 clears any previous set. (Plain small ints like
 * the RX twin — this is not a run status, so the WBLINK_TX_* space does not
 * apply.)
 */
int wblink_tx_set_adapter_fds(wblink_tx *tx, const int *fds, size_t n);

/*
 * §15.5 (Pass 172/174) the per-die capability answers — the same
 * `{"adapters":[...]}` object `wblink_rx_adapters` documents, published at
 * backend bring-up and republished at ~1 Hz — the caps fields are static
 * per die, but `channel` is LIVE (CSA and craft-local retunes move it).
 * 3 means the backend has not come up yet.
 *
 * Buffer contract (both calls below): `buffer == NULL && capacity == 0` is a
 * size query; `required` receives the byte count INCLUDING the trailing NUL;
 * a successful copy is NUL-terminated; an undersized buffer is left
 * untouched. Returns 0 on a size query/copy, 2 for invalid arguments, 3
 * before the first publication, 4 when `capacity` is too small (1 is
 * reserved for an internal size overflow no real snapshot can reach). Final
 * snapshots remain readable after the run stops, until the handle is
 * destroyed.
 */
int wblink_tx_adapters(wblink_tx *tx, char *buffer, size_t capacity,
                       size_t *required);

/*
 * The TX node's own account of its state (Pass 174), republished at 1 Hz on
 * its own cadence — deliberately independent of `stats.hz`, so a node with
 * §15.3 output disabled does not blind its embedder:
 *
 *   {"session":N,"channel":N,"csa":"...","claimed":B,"claimed_by":N,
 *    "mode":{"active":"...","apply_configured":B},
 *    "wedge":{"enabled":B,"progress_proven":B,"wedged":B,"consecutive":N,
 *             "windows":N}}
 *
 * Every field keeps its existing semantics: `mode` mirrors §15.5 GET
 * /api/v1/mode; `wedge` mirrors §9.10 — `progress_proven` false with
 * `enabled` true is Pass 170's inert-watchdog state, VISIBLE here by
 * design; `claimed`/`claimed_by` mirror §11.4; `channel` is live (CSA and
 * retune included). Same buffer contract as wblink_tx_adapters.
 */
int wblink_tx_status(wblink_tx *tx, char *buffer, size_t capacity,
                     size_t *required);

/*
 * Pass 176: the §15.3 stats line and the §15.4 health object — byte for byte
 * what the control server serves as GET /api/v1/stats and /api/v1/health,
 * because all three transports copy the SAME strings the run loop publishes
 * from its one fill path. Republished at the stats cadence, so `stats.hz`
 * governs freshness and `stats.hz=0` leaves them unpublished (3): an
 * embedder that disables the stats walk has chosen a blind link view, and
 * these getters do not re-enable it behind its back. The always-on surface
 * is `wblink_tx_status`. Same buffer contract as the calls above.
 */
int wblink_tx_stats(wblink_tx *tx, char *buffer, size_t capacity,
                    size_t *required);
int wblink_tx_health(wblink_tx *tx, char *buffer, size_t capacity,
                     size_t *required);

/*
 * Ask a running node to stop. Safe from any thread, and from a signal handler:
 * the flag underneath is a lock-free atomic. Returns immediately — the loop
 * polls, so expect up to one poll period before `wblink_tx_run` returns.
 *
 * Calling it BEFORE `wblink_tx_run` makes that call return WBLINK_TX_OK without
 * loading a config or opening a radio.
 */
void wblink_tx_request_stop(wblink_tx *tx);

/*
 * Load `config_path` and run a transmitting node until stopped. Blocks.
 *
 * ONE HANDLE RUNS ONCE, for the reason spelled out in `rx_node_c.h`: the stop
 * flag is sticky (`run_tx` takes it by const reference), so a second run would
 * fall straight out of the loop and report a clean stop having already claimed
 * the adapter — a dead node that looks healthy. Reuse returns WBLINK_TX_REUSED.
 * Create a handle per start.
 *
 * THAT IS ALSO THE RECOVERY CONTRACT (Pass 175). On WBLINK_TX_WEDGED (§9.10),
 * a supervisor — in-process or external — destroys the handle and creates a
 * fresh one; no external prep, no root sysfs help. create() owns adapter
 * preparation: a missing adapter fails loudly by name, a kernel driver bound
 * to the interface is detached at claim, a stale claim is retried (BUSY,
 * 6x250 ms). Fresh-object recovery measured 5/5; every in-place alternative
 * measured 0/5 or terminates the process (a second InitWrite on a live
 * devourer object), which is why no recover() call exists.
 *
 * `on_mode_apply` may be NULL; that is a claim about the node, not an omission.
 *
 * LINKING: this is a C++ library behind a C header. A C consumer must link the
 * C++ runtime — `-lstdc++ -lm` with gcc/clang — and link the archives in
 * order: wblink_node, wblink_io, wblink_core (plus devourer/usb-1.0 when
 * WBLINK_RADIO is on). `wblink::node` is not part of the installed
 * `find_package(wblink)` set; consume the tree with add_subdirectory, or point
 * a Makefile at the build directory.
 *
 * A TX node reads video from its configured input binding and needs frame-SHM
 * or an RTP feed to have anything to send; unlike the RX side there is no
 * callback egress, because the egress is the radio.
 */
int wblink_tx_run(wblink_tx *tx, const char *config_path,
                  wblink_mode_apply_cb on_mode_apply, void *user);

/* Frees the handle. Must not be called while `wblink_tx_run` is in flight. */
void wblink_tx_destroy(wblink_tx *tx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WBLINK_NODE_TX_NODE_C_H */
