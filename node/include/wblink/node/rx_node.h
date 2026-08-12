// SPDX-License-Identifier: GPL-2.0-or-later
// The RX node's run loop (#109 Phase 2c step 2).
//
// This is the piece that makes `node/` a library a consumer can *run* rather
// than only build objects from. Before this, linking `wblink::node` got you an
// `RxCore`, a `TxCore`, an `AirBackend` and the ability to fill a §15.3
// snapshot — but the only run loop in the tree sat in `app/main.cpp`'s
// anonymous namespace, unreachable from any other translation unit.
//
// It moved verbatim. The plan (`docs/library-extraction-plan.md` §4.6) warned
// that this would not be a relocation, because `run_rx` keeps its state in
// locals captured by reference from ~40 lambdas. That structure is real — it
// is what produced the Pass 166 `issue_vcmd` stack-use-after-scope crash — but
// it is INTERNAL to the function, and the measurement that mattered was of
// what the function reaches OUTSIDE itself: after Phase 2c step 1 moved its
// free-function dependencies, `run_rx` referenced exactly one app-scope name,
// the stop flag. So the lift is a move plus one parameter, and
// `tools/move_identity.py` still gates it.
//
// IT LINKS WITHOUT THE OPTIONAL SUBSYSTEMS, as of B10 (§4.10). It did not at
// first: `run_rx` named `FrameShmRing` and `ControlServer` unconditionally,
// while `android-arm64` — the preset that exists to prove the Phase 3
// consumer — builds with both OFF (bionic has no `shm_open`). Compiling was
// never the problem; the headers are always present, so that preset was GREEN
// while the archive carried nine unresolvable references. A static archive
// does not resolve its own undefined symbols, so only a LINK sees it, which
// is why `examples/node-linkcheck` is a gate of its own.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/. `node/` owns no process: the signal handlers, `spawn_mode_applier`'s
// double fork and the §9.10 wedge exit all stay in `app/main.cpp`, and
// `tests/node_layering_test.py` fails the build if that changes.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "wblink/node/stats_fill.h"

namespace wblink {
namespace node {

// Run a receiving node until `stop` is non-zero, then tear down and return an
// exit status (0 = clean).
//
// `stop` is the one thing this could not inherit from `app/main.cpp`. There it
// was a file-scope `volatile sig_atomic_t` written by a signal handler; a
// library cannot own that, and a consumer that stops a node from another
// thread — which is what Android does — needs a flag with defined cross-thread
// semantics rather than one that merely happens to work. `std::atomic<int>` is
// both: lock-free on every target here, so it stays legal to write from a
// signal handler, and properly synchronised for the threaded case.
//
// The loop polls; it does not block on the flag. Expect up to one poll period
// of latency between setting it and return.
// A whole reassembled frame, ready for a decoder, and the stream it arrived
// on (§15.2 `streams[].stream_id`). This is B10 of #109: the egress that
// §15.2 spells `bind.kind: "frame-shm"` is *whole-frame* egress, and where the
// frame goes stops being a constant.
//
// PROGRAMMATIC ONLY, following the PR #139 precedent — there is no `BindKind`
// value for it and nothing in §15.2 changes, so a config cannot select it and
// this needs no spec amendment. A supplied sink takes the frame-shm-bound
// out-streams; UDP-bound ones are datagram egress and are left alone.
//
// It is called from the RX loop, synchronously, before the next frame is
// reassembled: a slow sink is backpressure on the receiver. Copy what you need
// and return — the buffer does not outlive the call.
using FrameSink =
    std::function<void(uint8_t stream_id, const uint8_t* frame, size_t len)>;

class RxRuntimeControl;

static_assert(std::atomic<int>::is_always_lock_free,
              "run_rx's stop flag is documented as safe to set from a signal "
              "handler; that is only true while std::atomic<int> is lock-free");

// `frame_out` empty = egress goes where the config says. Non-empty = it takes
// the frame-shm-bound out-streams instead, which is what lets a node run on a
// build with WBLINK_FRAME_SHM=OFF. Configuring a frame-shm stream on such a
// build with NO sink is refused at startup rather than silently dropped.
int run_rx(const Loaded& l, const std::atomic<int>& stop,
           const FrameSink& frame_out = {});

// Runtime-control overload for in-process consumers. The mailbox owns no
// thread; `run_rx` drains it on this same loop and publishes immutable
// snapshots back to callers. The overload above remains as a real symbol for
// existing C++ embedding consumers and forwards here with nullptr.
int run_rx(const Loaded& l, const std::atomic<int>& stop,
           const FrameSink& frame_out, RxRuntimeControl* runtime_control);

}  // namespace node
}  // namespace wblink
