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
// SIGINT/SIGTERM stop it, and the summary prints on the way out.
//
// It drives the C ABI (`rx_node_c.h`) rather than `run_rx` directly, on
// purpose: that is the surface waybeam-hub and Android will use, so the
// runtime proof runs through the same boundary they do. A C++ probe over the
// C++ API would leave the ABI itself unexercised at runtime.
#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "wblink/node/rx_node_c.h"

namespace {

// Frames to dump; decremented per frame from the RX thread.
std::atomic<long> g_inspect_left{0};

// The C ABI owns the stop flag; the handle is what the handler reaches, so
// the handle itself has to be safe to read from one. A plain pointer is not.
std::atomic<wblink_rx*> g_rx{nullptr};
static_assert(std::atomic<wblink_rx*>::is_always_lock_free,
              "read from a signal handler");
void on_signal(int) { wblink_rx_request_stop(g_rx.load(std::memory_order_relaxed)); }

struct StreamTally {
    uint64_t frames = 0;
    uint64_t bytes = 0;
    size_t smallest = 0;
    size_t largest = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <rx-config.json> [--fd <bus>/<dev>]...\n\n"
                     "Runs an RX node with a callback frame sink and reports\n"
                     "what arrived. The config's out-stream must be\n"
                     "bind.kind \"frame-shm\" — that is the whole-frame egress\n"
                     "kind, and the sink takes it.\n\n"
                     "--fd opens /dev/bus/usb/<bus>/<dev> and hands the\n"
                     "descriptor to wblink_rx_set_adapter_fds, which is the\n"
                     "ANDROID-SHAPED path: an unrooted app cannot enumerate\n"
                     "usbfs, so its adapters can only arrive as UsbManager\n"
                     "fds. libusb_wrap_sys_device takes any usbfs fd on Linux\n"
                     "(waybeam-link #140), so the whole path is reachable on a\n"
                     "bench with no phone. Repeat --fd per adapter, in the\n"
                     "config's adapters[] order; needs usbfs write access, so\n"
                     "run under sudo with the kernel driver unloaded.\n\n"
                     "--inspect N dumps the §15.4 VencFrameMeta prefix and the\n"
                     "first bytes after it for the first N frames, so a\n"
                     "consumer can check what it will actually be fed before\n"
                     "wiring a decoder to it.\n",
                     argv[0]);
        return 2;
    }

    const char* config_path = argv[1];
    long inspect_n = 0;
    std::vector<int> fds;
    std::vector<std::string> fd_paths;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--inspect" && i + 1 < argc) {
            inspect_n = std::strtol(argv[++i], nullptr, 10);
            continue;
        }
        if (std::string(argv[i]) != "--fd" || i + 1 >= argc) {
            std::fprintf(stderr, "probe: unexpected argument \"%s\"\n", argv[i]);
            return 2;
        }
        unsigned bus = 0, dev = 0;
        if (std::sscanf(argv[++i], "%u/%u", &bus, &dev) != 2) {
            std::fprintf(stderr, "probe: --fd wants <bus>/<dev>, got \"%s\"\n",
                         argv[i]);
            return 2;
        }
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u", bus, dev);
        const int fd = ::open(path, O_RDWR);
        if (fd < 0) {
            std::perror(path);
            std::fprintf(stderr,
                         "probe: cannot open %s — usbfs needs write access "
                         "(sudo) and the kernel driver unloaded\n", path);
            for (int held : fds) ::close(held);
            return 1;
        }
        fds.push_back(fd);
        fd_paths.emplace_back(path);
        std::fprintf(stderr, "probe: %s -> fd %d\n", path, fd);
    }

    g_inspect_left.store(inspect_n, std::memory_order_relaxed);
    std::map<uint8_t, StreamTally> tally;
    const auto on_frame = [](uint8_t stream_id, const uint8_t* frame,
                             size_t len, void* user) {
        auto& by_stream = *static_cast<std::map<uint8_t, StreamTally>*>(user);
        if (g_inspect_left.fetch_sub(1, std::memory_order_relaxed) > 0 &&
            frame != nullptr && len > 8) {
            // §15.4: u32 pts | u8 codec | u8 flags | u8 gdr_pos | u8 gdr_len,
            // then the Annex-B access unit. A consumer wiring a decoder needs
            // to know these are what it thinks they are, and this is cheaper
            // than finding out on a phone.
            uint32_t pts = 0;
            std::memcpy(&pts, frame, 4);
            std::fprintf(stderr,
                         "probe: meta pts=%u codec=0x%02x flags=0x%02x "
                         "gdr=%u/%u | au[0..7]=%02x %02x %02x %02x %02x %02x "
                         "%02x %02x (%zu B)\n",
                         pts, frame[4], frame[5], frame[6], frame[7],
                         frame[8], frame[9], frame[10], frame[11], frame[12],
                         frame[13], frame[14], frame[15], len - 8);
        }
        StreamTally& t = by_stream[stream_id];
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
    wblink_rx* rx = wblink_rx_create();
    if (rx == nullptr) {
        std::fprintf(stderr, "probe: wblink_rx_create failed\n");
        return 1;
    }
    g_rx.store(rx, std::memory_order_relaxed);

    if (!fds.empty()) {
        const int src = wblink_rx_set_adapter_fds(rx, fds.data(), fds.size());
        if (src != 0) {
            std::fprintf(stderr,
                         "probe: wblink_rx_set_adapter_fds failed (%d)\n", src);
            wblink_rx_destroy(rx);
            for (int held : fds) ::close(held);
            return 1;
        }
    }

    // The driver owns the process, exactly as app/main.cpp does; node/ does not.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    const int rc = wblink_rx_run(rx, config_path, on_frame, &tally);
    // Clear BEFORE freeing: a signal landing between the two would otherwise
    // reach a destroyed handle, and the bench script sends SIGTERM twice.
    g_rx.store(nullptr, std::memory_order_relaxed);
    wblink_rx_destroy(rx);
    // OWNERSHIP IS OURS. libusb marks a wrapped handle fd_keep, so tearing the
    // node down leaves every fd open; closing them is this process's job and
    // must happen only after wblink_rx_run has returned.
    for (size_t i = 0; i < fds.size(); ++i) {
        ::close(fds[i]);
        std::fprintf(stderr, "probe: closed %s\n", fd_paths[i].c_str());
    }

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
