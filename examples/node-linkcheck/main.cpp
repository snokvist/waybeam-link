// SPDX-License-Identifier: GPL-2.0-or-later
// Take the address of everything a consumer needs and link it. Nothing here
// runs — `main` returns before touching a radio — because the property under
// test is that the symbols RESOLVE, which is exactly what a compile-only gate
// cannot check.
#include <atomic>
#include <cstdio>

#include "wblink/node/rx_node.h"
#include "wblink/node/rx_node_c.h"

extern "C" int wblink_c_consumer_check(void);

int main() {
    // The frame sink is the reason this configuration links at all (#109 B10):
    // with it, the whole-frame egress never reaches FrameShmRing.
    const wblink::node::FrameSink sink = [](uint8_t stream_id,
                                            const uint8_t* frame, size_t len) {
        (void)stream_id;
        (void)frame;
        (void)len;
        return true;
    };
    // Referenced, never called: run_rx opens adapters and blocks. Spell the
    // original three-argument type so this gate also proves the additive
    // runtime-control overload did not remove that embedding symbol.
    using RunRxEntry = int (*)(const wblink::node::Loaded&,
                               const std::atomic<int>&,
                               const wblink::node::FrameSink&);
    const RunRxEntry entry = static_cast<RunRxEntry>(&wblink::node::run_rx);
    const int c_rc = wblink_c_consumer_check();
    std::printf("node_linkcheck: c_abi=%d run_rx=%p sink=%d\n", c_rc,
                reinterpret_cast<const void*>(entry),
                static_cast<int>(static_cast<bool>(sink)));
    // Propagate: the C-ABI contract checks print their verdict, and a gate
    // that prints a failure while exiting 0 is not a gate. `cmake --build`
    // only ever sees this number.
    return c_rc;
}
