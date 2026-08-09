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
// WHAT THIS DOES NOT YET DO. `run_rx` names `FrameShmRing` and `ControlServer`
// unconditionally, and the `android-arm64` preset — the one that exists to
// prove the Phase 3 consumer — builds with `WBLINK_FRAME_SHM=OFF` and
// `WBLINK_CONTROL_SERVER=OFF` (bionic has no `shm_open`). Compiling is not the
// problem: the headers are always present, so this TU builds green on that
// preset. LINKING is. A static archive does not resolve its own undefined
// symbols, so the gap is invisible until a consumer links it. Closing that is
// B10 (§4.8) — the callback egress sink — and it comes with the link check
// that proves it, because a build cannot.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/. `node/` owns no process: the signal handlers, `spawn_mode_applier`'s
// double fork and the §9.10 wedge exit all stay in `app/main.cpp`, and
// `tests/node_layering_test.py` fails the build if that changes.
#pragma once

#include <atomic>

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
static_assert(std::atomic<int>::is_always_lock_free,
              "run_rx's stop flag is documented as safe to set from a signal "
              "handler; that is only true while std::atomic<int> is lock-free");

int run_rx(const Loaded& l, const std::atomic<int>& stop);

}  // namespace node
}  // namespace wblink
