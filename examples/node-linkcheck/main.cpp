// SPDX-License-Identifier: GPL-2.0-or-later
// Take the address of everything a consumer needs and link it. Nothing here
// runs — `main` returns before touching a radio — because the property under
// test is that the symbols RESOLVE, which is exactly what a compile-only gate
// cannot check.
#include <atomic>
#include <cstdio>

#include "wblink/node/rx_node.h"

int main() {
    // The frame sink is the reason this configuration links at all (#109 B10):
    // with it, the whole-frame egress never reaches FrameShmRing.
    const wblink::node::FrameSink sink = [](uint8_t stream_id,
                                            const uint8_t* frame, size_t len) {
        (void)stream_id;
        (void)frame;
        (void)len;
    };
    // Referenced, never called: run_rx opens adapters and blocks.
    auto* entry = &wblink::node::run_rx;
    std::printf("node_linkcheck: run_rx=%p sink=%d\n",
                reinterpret_cast<const void*>(entry),
                static_cast<int>(static_cast<bool>(sink)));
    return 0;
}
