// SPDX-License-Identifier: GPL-2.0-or-later
//
// #109 Phase 3: the TX run loop as a library entry point.
//
// WHY THIS ONE COULD NOT BE A VERBATIM LIFT, when run_rx was.
//
// run_rx reached exactly ONE app-scope name (the stop flag), so it moved
// unchanged. run_tx reaches four, and three of them are precisely the
// process-owning behaviours B9 says node/ must not have:
//
//   mode_catalog_dir    a pure string helper  -> moved here, no interface
//   g_stop              the signal-handler flag -> a parameter, as in run_rx
//   spawn_mode_applier  double-fork + setsid + exec -> a CALLBACK; the fork
//                       stays in app/, because a library that forks decides
//                       for its host what a process is
//   kExitTxWedged = 9   a process exit code  -> run_tx returns a STATUS and
//                       app/ maps it, so the Pass 148 contract is unchanged
//                       on the wire while the decision to exit moves out
//
// The shape mirrors B10's frame sink: an optional caller-supplied function,
// defaulting to "cannot do that", so a consumer which has no business forking
// simply does not pass one.
#ifndef WBLINK_NODE_TX_NODE_H
#define WBLINK_NODE_TX_NODE_H

#include <atomic>
#include <functional>
#include <string>

#include "wblink/config.h"
#include "wblink/node/load.h"

namespace wblink::node {

// §15.5 Pass 104: where GET /api/v1/modes enumerates from. Explicit
// venc.modes_dir wins; otherwise derive it from mode_apply_cmd's directory (the
// §16 layout co-locates the applier and the modes/<name>.json files), so a
// deployed craft serves the catalog with no extra config. "" when neither set.
inline std::string mode_catalog_dir(const wblink::VencCfg& venc) {
    if (!venc.modes_dir.empty()) return venc.modes_dir;
    const auto slash = venc.mode_apply_cmd.find_last_of('/');
    if (slash != std::string::npos) return venc.mode_apply_cmd.substr(0, slash);
    return {};
}

// Apply an operating mode by name (§15.5 / §11.7 0x07). Returns false if the
// mode could not be applied. Called from the flight loop, so it MUST NOT
// block — the app-side implementation double-forks and returns immediately.
// The flight loop is the thread that called run_tx (Pass 172 threading
// contract): no dispatch thread exists, so every apply arrives on that one
// thread for the life of the run.
//
// An empty function means this node cannot apply modes; run_tx treats that
// exactly as a failed apply, which is the honest answer for a consumer that
// cannot spawn processes. It is never a silent success.
using ModeApplyFn =
    std::function<bool(const std::string& cmd, const std::string& name)>;

// run_tx's status, deliberately NOT a process exit code.
//
// kTxWedged is the Pass 148 condition. app/main.cpp maps it to exit 9
// (kExitTxWedged) so the deployed contract is byte-for-byte what it was; a
// library consumer is free to do something less drastic than die.
//
// The body still returns raw 0 and 1 — byte-identity with the pre-move code was
// the gate on the lift, so folding the literals to these names is a deliberate
// follow-up, not drift. Until it happens: A NEW FAILURE PATH MUST NOT
// `return 2`. Two is kTxWedged, and app/main.cpp turns it into exit 9, so the
// next-free-integer reflex would silently report a wedged TX.
inline constexpr int kTxOk = 0;
inline constexpr int kTxError = 1;
inline constexpr int kTxWedged = 2;

static_assert(std::atomic<int>::is_always_lock_free,
              "run_tx's stop flag is documented as safe to set from a signal "
              "handler; that is only true while std::atomic<int> is lock-free");

class TxRuntimeInfo;

// R6: the prototypes are guarded like tx_node_c.h's — `wblink::node` defines
// WBLINK_NODE_TX PUBLIC as 1/0, so a receive-only build that references
// run_tx fails at compile time; without the macro (out-of-tree reader) the
// whole surface stays visible and the linker judges, as before.
#if !defined(WBLINK_NODE_TX) || WBLINK_NODE_TX

// Blocks until `stop` is set or the loop gives up. Owns no process: it does
// not exit, fork, or install a signal handler. See the header note above.
int run_tx(const Loaded& l, const std::atomic<int>& stop,
           const ModeApplyFn& mode_apply = {});

// Runtime-info overload (Pass 174): the loop publishes immutable status and
// adapters/caps snapshots into the mailbox for in-process consumers. The
// overload above remains a real symbol for existing embedders and forwards
// here with nullptr.
int run_tx(const Loaded& l, const std::atomic<int>& stop,
           const ModeApplyFn& mode_apply, TxRuntimeInfo* runtime_info);

#endif  // WBLINK_NODE_TX

}  // namespace wblink::node

#endif  // WBLINK_NODE_TX_NODE_H
