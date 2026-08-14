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
 * The original surface was deliberately five functions. The runtime-control
 * additions below preserve the same ownership rule: they only enqueue work or
 * copy immutable snapshots. They never call node state from the caller's
 * thread, and the shim still does not own a thread.
 *
 * That rule is what lets `wblink_rx_claim` and `wblink_rx_vehicle_command`
 * exist here at all. They cause the node to TRANSMIT, which reads like a
 * second radio owner and is not one: they enqueue, and the RX loop issues on
 * its own thread through the same code the REST control plane calls. A
 * receiving node has always owned a designated role:"tx" uplink adapter; only
 * the trigger was missing, and it was missing because it sat behind
 * WBLINK_CONTROL_SERVER. `run_tx` — the VIDEO transmitter, which really is
 * absent from a frame-SHM-less build — is not involved.
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

#include "wblink/node/node_state_c.h" /* Pass 177: WBLINK_NODE_* */

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
 *
 * The RX loop thread IS the thread that called wblink_rx_run — the node
 * spawns no dispatch thread, so every callback arrives on that one thread
 * for the life of the run (a JNI consumer attaches it once; Pass 172).
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
 * Supply pre-opened USB device descriptors, parallel to the config's
 * `adapters[]`, with -1 in a slot meaning "enumerate this one by bus path".
 * Call BEFORE `wblink_rx_run`; a call after it has started is ignored and
 * returns 3.
 *
 * THIS IS THE ONLY DEVICE SOURCE UNROOTED ANDROID HAS. It cannot enumerate
 * usbfs, so its fds come from the Java UsbManager and reach libusb through
 * `libusb_wrap_sys_device`. Everything else — a daemon reading JSON — should
 * not call this at all: empty (the default) is enumerate-by-bus-path, byte for
 * byte today's behaviour. There is deliberately no config key for it (ruling
 * 2026-08-08), which is why it is a call rather than a field.
 *
 * OWNERSHIP STAYS WITH THE CALLER. libusb marks a wrapped handle `fd_keep`, so
 * teardown closes the libusb handle and leaves the fd open. Each fd must stay
 * valid for the whole run and be closed by the caller afterwards. The array
 * itself is copied, so it need not outlive this call.
 *
 * Supplying any fd forces the bring-up `libusb_reset_device` off; a wrapped fd
 * must not be reset.
 *
 * Returns 0 on success, 1 if copying the array failed to allocate, 2 on a
 * NULL handle or a NULL `fds` with `n > 0`, 3 if the node has already been
 * started. Passing n == 0 clears any previous set.
 */
int wblink_rx_set_adapter_fds(wblink_rx *rx, const int *fds, size_t n);

/*
 * Queue a ScoutEngine sweep. The channels are MHz values and are copied before
 * this call returns. `channels == NULL && channel_count == 0` selects the node
 * config's `scout.channels`, then its CSA allowlist, exactly like the REST
 * control path. `dwell_ms == 0` selects the configured dwell.
 *
 * The call is thread-safe but not signal-safe. It never retunes directly: the
 * RX loop applies the newest queued intent and publishes its generation in the
 * scout snapshot call below. A later start/stop/select supersedes a pending
 * command, so the mailbox is bounded and lifecycle stop cannot wait behind a
 * backlog.
 *
 * Returns 0 when queued, 2 for invalid arguments, 3 when no run is active, or
 * 1 on allocation failure. `generation` may be NULL; otherwise it receives the
 * monotonic accepted-command generation.
 */
int wblink_rx_scout_start(wblink_rx *rx, const uint16_t *channels,
                          size_t channel_count, uint32_t dwell_ms,
                          uint64_t *generation);

/* Queue a sweep stop. Same threading, generation and return contract above. */
int wblink_rx_scout_stop(wblink_rx *rx, uint64_t *generation);

/*
 * Queue passive selection of a scouted craft by originator. On a spectator the
 * RX loop resolves ScoutEngine's heard-most channel (including private frame
 * evidence), abandons any active sweep, pins the craft's net_id and retunes all
 * ears. Non-spectator nodes refuse this Android-shaped passive-select command.
 */
int wblink_rx_scout_select(wblink_rx *rx, uint16_t originator,
                           uint64_t *generation);

/*
 * §11.4. Queue a CSA claim of a scouted craft: re-key the issuer from the
 * craft's key, bind the link to its net_id, move every ear onto its channel to
 * be heard, and issue a §11 campaign moving it to `target_chan`.
 * `target_chan == 0` asks for the emptiest allowlisted channel.
 *
 * THIS IS THE CONTROL TRANSMITTER, AND IT IS NOT A SECOND RADIO OWNER. The
 * command is queued from your thread and applied by the RX loop on its own,
 * exactly as the scout commands above are; nothing outside `wblink_rx_run`
 * ever touches the radio, the issuers or the ScoutEngine. That is the whole
 * reason a consumer built with WBLINK_FRAME_SHM / WBLINK_CONTROL_SERVER /
 * WBLINK_VENC OFF can transmit control frames at all: `run_tx` is the VIDEO
 * transmitter and is genuinely absent from such a build, but a receiving node
 * has always owned a designated role:"tx" uplink adapter and issued claims on
 * it. Only the trigger was missing.
 *
 * A claim SUPERSEDES a queued-but-untaken sweep command, matching the loop's
 * own behaviour — the claim path abandons a running sweep, because a claim is
 * what a sweep is for.
 *
 * Queueing is not succeeding. The synchronous refusals (unknown craft, stale
 * candidate, no key, retune failure) and the asynchronous outcome (the campaign
 * committing or rolling back) both surface through `wblink_rx_selection`,
 * whose `state` moves through "claiming" to committed or back. A controlled
 * cache refuses outright: it follows its receiver's selection and must not be
 * able to split from it.
 *
 * Same return contract as the scout calls: 0 queued, 2 invalid argument
 * (originator 0), 3 no active run, 1 allocation failure.
 */
int wblink_rx_claim(wblink_rx *rx, uint16_t originator, uint16_t target_chan,
                    uint64_t *generation);

/*
 * §11.7. Queue a command campaign toward the claimed craft. `cmd` is the §15.5
 * REST spelling ("arq", "fps_select", "mode", "mtu_tier", ...; the map is
 * `node/vcmd.h`) and is copied before this call returns — it need not outlive
 * the call. A name longer than 32 bytes is rejected rather than truncated,
 * since a truncated name could name a DIFFERENT command.
 *
 * This is the untyped entry point, so commands §15.5 restricts to a typed REST
 * endpoint are refused rather than issued with a bare integer argument.
 *
 * A campaign needs a craft claimed first: §11.7 has no bootstrap, and a craft
 * silently drops commands from an issuer whose CSA it has not accepted, so an
 * unclaimed send would report accepted and then always time out. Call
 * `wblink_rx_claim` first.
 *
 * 0 queued, 2 invalid argument (NULL/empty/over-long `cmd`), 3 no active run,
 * 1 allocation failure. A queued command that the loop then REFUSES still
 * returns 0 here — read `wblink_rx_command_status` for the verdict.
 */
int wblink_rx_vehicle_command(wblink_rx *rx, const char *cmd, int32_t arg,
                              uint64_t *generation);

/*
 * Copy immutable JSON snapshots published by the RX loop. The JSON shapes are
 * the existing §15.5/§15.5a scout, discovery and selection payloads; this ABI
 * does not add fields to them.
 *
 * `buffer == NULL && capacity == 0` is a size query. `required` receives the
 * byte count INCLUDING the trailing NUL. A successful copy is always
 * NUL-terminated; an undersized buffer is left untouched. Snapshot calls also
 * request a fresh publication from the RX loop, so the first call may report
 * not-ready and a later poll observes it.
 *
 * Returns 0 on a size query/copy, 2 for invalid arguments, 3 when no snapshot
 * has been published yet, or 4 when `capacity` is too small (1 is reserved
 * for an internal size overflow that no real snapshot can reach). Final
 * snapshots remain readable after the run stops, until the handle is
 * destroyed.
 */
int wblink_rx_scout_results(wblink_rx *rx, char *buffer, size_t capacity,
                            size_t *required,
                            uint64_t *applied_generation);
int wblink_rx_discovery(wblink_rx *rx, char *buffer, size_t capacity,
                        size_t *required);
/*
 * §15.5 (Pass 172) the per-die capability answers, as the /info adapters[]
 * array wrapped in one object:
 *
 *   {"adapters":[{"name":"...","role":"rx","channel":N,"mac":...,
 *                 "chip":"...","power_actuator":B,"ldpc_rx_flag":B,
 *                 "fastretune":B}, ...]}
 *
 * Published at backend bring-up and republished at ~1 Hz. The capability
 * fields are static per die and never change between publishes; `channel`
 * is LIVE (CSA, craft-local retunes and scout dwells all move it), current
 * as of the last publish. Unlike the snapshot calls above there is no
 * fresh-publication request to poll for: 3 means the backend has not come
 * up yet (or run_rx was never called). On a consumer built
 * WBLINK_CONTROL_SERVER=OFF this call is the only capability surface. Same
 * buffer/return contract as the calls above.
 */
int wblink_rx_adapters(wblink_rx *rx, char *buffer, size_t capacity,
                       size_t *required);
/*
 * Pass 176: the §15.3 stats line and the §15.4 health object — byte for byte
 * what the control server serves as GET /api/v1/stats and /api/v1/health,
 * published from the run loop's one fill path. Deliberately available in a
 * receive-only build (control server compiled out): the C ABI is that
 * build's only telemetry surface. Republished at the stats cadence, so
 * `stats.hz` governs freshness and `stats.hz=0` leaves them unpublished (3)
 * by design rather than re-enabling the walk behind the embedder's back.
 * Same buffer contract as the snapshot calls above.
 */
int wblink_rx_stats(wblink_rx *rx, char *buffer, size_t capacity,
                    size_t *required);
int wblink_rx_health(wblink_rx *rx, char *buffer, size_t capacity,
                     size_t *required);

/*
 * Pass 177: the handle's lifecycle, stated by the library instead of
 * re-derived by every embedder — wblink_rx_run returns within milliseconds
 * on a missing radio or a bad config, and every consumer of this header has
 * grown its own "is it actually running" latch in response (waybeam-hub's
 * mod_wblink_running, Android's resume latch). Returns WBLINK_NODE_CREATED /
 * RUNNING / EXITED (node_state_c.h; -1 on a NULL handle). Once EXITED and
 * when `exit_rc` is non-NULL, the run's return code — THIS header's 0/1/2/3
 * space — is written through it; before EXITED, `*exit_rc` is left
 * untouched. Safe from any thread. A refused run (NULL argument, reused
 * handle) never transitions, so a late double-start cannot overwrite the
 * real run's record.
 */

int wblink_rx_state(wblink_rx *rx, int *exit_rc);

/*
 * Pass 178: where this node's §15.5 control server is ACTUALLY listening,
 * "addr:port", under the same buffer contract as the snapshot calls above.
 *
 * The value is resolved from the listening socket, not echoed from
 * `control.bind` — `host:0` is a legal request that binds an ephemeral port,
 * so the config string is a wish and the resolved PORT is the fact. The
 * ADDRESS half is whatever was bound, and a wildcard bind stays a wildcard:
 * the fleet's `"0.0.0.0:8091"` reads back as `0.0.0.0`, which names the
 * interface set, not a host to dial. A local embedder should dial loopback
 * with this port; nothing here invents an address it cannot verify.
 *
 * Published only after a successful bind, and 3 means there is no control
 * plane to talk to — none configured, the control server compiled out, or
 * the bind's own address could not be read back. That is deliberately a
 * different answer from "here is an address", because an embedder that
 * resolves the endpoint from its own parallel config key instead
 * (waybeam-hub's `metrics.waybeam_link`) 502s every route when the two files
 * disagree, with a plausible-looking address in both.
 *
 * LIKE EVERY SNAPSHOT HERE, IT DESCRIBES THE RUN THAT PUBLISHED IT and
 * survives that run's exit — the control server is gone once the run
 * returns, but this still answers 0. Pair it with wblink_rx_state() when you
 * need to know whether the endpoint is still live; a getter that erased
 * itself would break the deliberate read-the-final-state property the other
 * snapshots rely on.
 */
int wblink_rx_control_endpoint(wblink_rx *rx, char *buffer, size_t capacity,
                               size_t *required);

int wblink_rx_selection(wblink_rx *rx, char *buffer, size_t capacity,
                        size_t *required,
                        uint64_t *applied_generation);

/*
 * The §11.7 campaign readout, and the ONLY place a queued command's verdict is
 * visible — an enqueue call returns before the loop has judged it.
 *
 *   {"campaign":{"nonce":N,"cmd":"...","arg":N,"state":"..."},
 *    "verdict":{"code":200,"result":{...}}}
 *
 * `campaign` is the live §11.7 state, identical to what §15.5's
 * /vehicle/command GET returns, and advances on its own as the craft ACKs.
 * `verdict` is present ONLY when `applied_generation` names a generation that
 * tried to start a campaign — so a poll after a claim, or a plain refresh
 * between commands, shows a campaign with no verdict attached rather than an
 * older verdict that would read as this command's.
 *
 * Same buffer/return contract as the three calls above.
 */
int wblink_rx_command_status(wblink_rx *rx, char *buffer, size_t capacity,
                             size_t *required,
                             uint64_t *applied_generation);

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
 * THAT IS ALSO THE RECOVERY CONTRACT (Pass 175): after any failed or wedged
 * run, destroy the handle and create a fresh one IN THE SAME PROCESS — no
 * external prep, no root sysfs help. create() owns adapter preparation:
 * a missing adapter fails loudly by name, a kernel driver bound to the
 * interface is detached at claim, a stale claim is retried (BUSY, 6x250 ms).
 * Fresh-object recovery measured 5/5; every in-place alternative measured
 * 0/5 or terminates the process, which is why no recover() call exists.
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
