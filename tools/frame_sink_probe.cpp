// SPDX-License-Identifier: GPL-2.0-or-later
// Run a receiving node whose whole-frame egress goes to a CALLBACK, and report
// what came out (#109 B10).
//
// This exists because the B10 link gate proves the wrong half. `node-linkcheck`
// proves the symbols resolve with the optional subsystems compiled out; it
// cannot prove a frame ever reaches the sink, and no unit test can either —
// `run_rx` opens adapters and blocks, so the path is only reachable from a
// running node.
//
// So this is the smallest thing that IS a consumer: it links `wblink::node`,
// supplies a `FrameSink`, and prints per-stream frame counts and sizes. Point
// it at any rx config whose out-stream is `bind.kind: "frame-shm"` — the sink
// takes those streams instead of a ring, so no ring is created and none is
// needed. Over the `udp` dev air backend that is a whole verification with no
// radio, no hardware and no second repo:
//
//   waybeam-link tx -c examples/config.air-tx.sample.json      # one terminal
//   tools/rtp_feed.py --dest 127.0.0.1:5600                    # another
//   build/dev/frame_sink_probe examples/config.frame-shm-rx.sample.json
//
// SIGINT/SIGTERM stop it, and the summary prints on the way out. It is also
// the harness the waybeam-hub and Android bring-ups start from: the question
// "did a frame arrive" is answered here once, so a failure there is theirs.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <map>
#include <string>

#include "wblink/node/load.h"
#include "wblink/node/rx_node.h"

namespace {

std::atomic<int> g_stop{0};
static_assert(std::atomic<int>::is_always_lock_free,
              "written from a signal handler");
void on_signal(int) { g_stop = 1; }

struct StreamTally {
    uint64_t frames = 0;
    uint64_t bytes = 0;
    size_t smallest = 0;
    size_t largest = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: %s <rx-config.json>\n\n"
                     "Runs an RX node with a callback frame sink and reports\n"
                     "what arrived. The config's out-stream must be\n"
                     "bind.kind \"frame-shm\" — that is the whole-frame egress\n"
                     "kind, and the sink takes it.\n",
                     argv[0]);
        return 2;
    }

    wblink::node::Loaded loaded;
    if (const int rc = wblink::node::load_all(argv[1], loaded); rc != 0) {
        return rc;
    }

    std::map<uint8_t, StreamTally> tally;
    const wblink::node::FrameSink sink = [&tally](uint8_t stream_id,
                                                  const uint8_t* frame,
                                                  size_t len) {
        (void)frame;  // a decoder would read it here; counting is the proof
        StreamTally& t = tally[stream_id];
        if (t.frames == 0) {
            std::fprintf(stderr, "probe: FIRST FRAME on stream %u, %zu bytes\n",
                         static_cast<unsigned>(stream_id), len);
            t.smallest = len;
            t.largest = len;
        }
        ++t.frames;
        t.bytes += len;
        if (len < t.smallest) t.smallest = len;
        if (len > t.largest) t.largest = len;
        // Cheap liveness without drowning the log at frame rate.
        if (t.frames % 100 == 0) {
            std::fprintf(stderr, "probe: stream %u %llu frames\n",
                         static_cast<unsigned>(stream_id),
                         static_cast<unsigned long long>(t.frames));
        }
    };

    // The driver owns the process, exactly as app/main.cpp does; node/ does not.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    const int rc = wblink::node::run_rx(loaded, g_stop, sink);

    std::fprintf(stderr, "\nprobe: summary (%zu stream(s))\n", tally.size());
    if (tally.empty()) {
        std::fprintf(stderr,
                     "probe: NO FRAMES REACHED THE SINK — this is a failure,\n"
                     "       not an empty run. Check that the TX node is\n"
                     "       feeding, and that the out-stream is frame-shm.\n");
    }
    for (const auto& [sid, t] : tally) {
        std::fprintf(stderr,
                     "probe:   stream %u: %llu frames, %llu bytes, "
                     "size %zu..%zu\n",
                     static_cast<unsigned>(sid),
                     static_cast<unsigned long long>(t.frames),
                     static_cast<unsigned long long>(t.bytes), t.smallest,
                     t.largest);
    }
    return tally.empty() ? 1 : rc;
}
