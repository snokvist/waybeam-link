// SPDX-License-Identifier: GPL-2.0-or-later
//
// Hardware trial harness for the two RadioAir device sources (issue #140).
// Brings adapters up, prints per-unit identity and RX counters, tears down.
//
// **It does not transmit unless you ask it to.** Without `--tx N` adapters are
// created RX-only (`allow_rx_only = tx_frames == 0`), so no code path can
// inject and it is safe to run unattended — that is the surface the Phase
// 1a/1b changes touched. `--tx N` flips that: it makes the adapter role:"tx",
// takes InitWrite rather than Init, and RADIATES. See the --tx note below.
// (The line here used to say "it never transmits" flatly, which contradicted
// that note 21 lines down and was cited as fact by a second tool.)
//
// Why this exists: Phase 1a (#138), 1b (#139) and 1a′ (#141) are all
// compile-, unit- and byte-comparison-verified. Several of their behaviours
// can only be observed against a radio — notably the bus:devaddr duplicate
// guard, which refuses two stanzas resolving to one unit and had never
// executed against a real device.
//
// The fd mode is the point. `libusb_wrap_sys_device` takes ANY usbfs fd on
// Linux, so `--fd <bus>/<dev>` exercises the whole Android-shaped path — wrap,
// do_reset=false, lock_dir, the claim, InitWrite, the EFUSE walk — with no
// phone involved. That makes B11's long-unproven leg (wrapped fd +
// do_reset=false + InitWrite + EFUSE) runnable on the bench.
//
//   hwtrial_bringup --auto                    # enumerate every Realtek unit
//   hwtrial_bringup --bus 5-2 --bus 8-4       # explicit bus paths
//   hwtrial_bringup --fd 5/2 --fd 8/4         # wrapped fds (needs usbfs rw)
//   hwtrial_bringup --fd 5/2 --bus 5-2        # duplicate-unit guard: must FAIL
//
// `--tx N` is the one mode that RADIATES. It makes the single adapter a
// role:"tx" uplink and injects N §3.0 frames at MCS 0 (the most robust rate)
// and a low TX power, then reports submitted/failed and the CCX report
// counters. Pair it with a second process running an RX ear on the same
// channel and a DIFFERENT --originator: RadioAir drops frames stamped with
// its own originator, so one process can never hear itself.
//
//   term A:  hwtrial_bringup --bus 5-1 --originator 2 --net-id 7 --seconds 12
//   term B:  hwtrial_bringup --bus 8-1 --originator 1 --net-id 7 --tx 300
//
// Term A's rx_frames is the confirmation. Keep bursts short and start from
// the low-power end (repo law: sweep from the safe end first).
//
// Exit 0 = every adapter came up and reported an identity. Anything else is a
// failure with a reason on stderr.

#include <dirent.h>
#include <fcntl.h>
#include <libusb.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "wblink/air_radio.h"

namespace {

constexpr uint16_t kRealtekVid = 0x0bda;

struct Unit {
    std::string bus_path;  // "5-2" — usb_path_of() form
    uint8_t busnum = 0;
    uint8_t devnum = 0;
};

// Enumerate Realtek units the same way RadioAir does, so --auto and --bus
// agree on what a path means.
std::vector<Unit> enumerate() {
    std::vector<Unit> out;
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        return out;
    }
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0 ||
            dd.idVendor != kRealtekVid) {
            continue;
        }
        uint8_t ports[7];
        const int np = libusb_get_port_numbers(list[i], ports, sizeof ports);
        Unit u;
        u.busnum = libusb_get_bus_number(list[i]);
        u.devnum = libusb_get_device_address(list[i]);
        u.bus_path = std::to_string(u.busnum);
        u.bus_path += '-';
        for (int k = 0; k < np; ++k) {
            if (k) u.bus_path += '.';
            u.bus_path += std::to_string(ports[k]);
        }
        std::fprintf(stderr, "unit %s (%04x:%04x, bus %u dev %u)\n",
                     u.bus_path.c_str(), dd.idVendor, dd.idProduct, u.busnum,
                     u.devnum);
        out.push_back(u);
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

// usbfs node for bus/dev. The fd is handed to libusb_wrap_sys_device and
// stays OWNED BY US — libusb marks a wrapped handle fd_keep, so RadioAir's
// teardown closes its side and leaves this descriptor open.
int open_usbfs(unsigned bus, unsigned dev) {
    char path[64];
    std::snprintf(path, sizeof path, "/dev/bus/usb/%03u/%03u", bus, dev);
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::fprintf(stderr,
                     "open %s: %s (usbfs needs rw — run as root or add a udev "
                     "rule)\n",
                     path, std::strerror(errno));
    }
    return fd;
}

void usage() {
    // The TX flags were missing here until a #140 A6 run lost a bench pass to
    // guessing "--power-offset" for --power-offset-qdb: the harness printed
    // this line, which does not mention the flag, and exited 2.
    std::fprintf(stderr,
                 "usage: hwtrial_bringup [--auto] [--bus <path>]... "
                 "[--fd <bus>/<dev>]... [--chan-mhz N] [--seconds N] "
                 "[--lock-dir DIR]\n"
                 "       TX (RADIATES): [--tx <frames>] [--mcs N] "
                 "[--power-offset-qdb N] [--originator N] [--net-id N]\n"
                 "       settle (RX-only): [--settle <cycles>] "
                 "[--settle-quiet-mhz N] [--settle-timeout-ms N] "
                 "[--settle-fast]\n");
}

// R5 retune-settle measurement (docs/findings.md). Deafness is the gap
// between retune() RETURNING and the first frame the RX callback ACCEPTS on
// the new channel — rx_frames increments in devourer's RX thread at parse
// time, so polling the counter observes arrival, not poll_once() service.
// Needs a continuous known emitter on --chan-mhz (the ~1 kHz --tx loop of a
// second unit, or a live craft); the quiet channel provides the "away" half
// of the hop and is verified quiet each cycle, because a frame heard there
// would satisfy the poll before the hop even happens.
struct SettleSample {
    double retune_call_ms = 0;  // inside the retune() call itself
    double deaf_ms = -1;        // call START -> first accepted frame; -1 = timeout
    uint64_t quiet_frames = 0;  // frames heard on the quiet channel (taint)
};

// Anchored at call START, not call return, and polled from a SECOND thread:
// the first AU run showed why. On x86 the jaguar1 full retune BLOCKS ~130 ms
// and the radio is already live at return (deaf-from-return = 0.0 on all 20
// cycles), while the 2026-08-13 Android measurement had a ~5 ms call and
// ~250 ms of post-return silence. Same code, same chip family — the settle
// sits on a different side of the call boundary per platform, so only the
// start-anchored number is comparable across rigs. A single-threaded poll
// cannot see when, inside a blocking call, frames began to flow.
SettleSample settle_cycle(wblink::RadioAir& a, uint16_t quiet_mhz,
                          uint16_t target_mhz, bool fast, int timeout_ms) {
    using clock = std::chrono::steady_clock;
    SettleSample s;
    // Away: full retune, then let the pipeline drain well past the deafness
    // under test so old-channel residue cannot be double-counted below.
    a.retune(0, quiet_mhz, 20, false);
    a.flush_rx();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    // Quiet-channel taint check: baseline over the last 100 ms of the away
    // dwell. Anything heard here means the "quiet" channel is not, and the
    // sample cannot distinguish settle from luck.
    const uint64_t pre = a.counters(0).rx_frames;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t baseline = a.counters(0).rx_frames;
    s.quiet_frames = baseline - pre;

    const auto t0 = clock::now();
    double first_ms = -1;
    std::thread poller([&] {
        const auto deadline = t0 + std::chrono::milliseconds(timeout_ms);
        while (clock::now() < deadline) {
            if (a.counters(0).rx_frames > baseline) {
                first_ms = std::chrono::duration<double, std::milli>(
                               clock::now() - t0)
                               .count();
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    const bool tuned = a.retune(0, target_mhz, 20, fast);
    s.retune_call_ms =
        std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    poller.join();
    if (tuned) {
        s.deaf_ms = first_ms;
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> buses;
    std::vector<std::pair<unsigned, unsigned>> fds;
    uint16_t chan_mhz = 5805;
    int seconds = 3;
    std::string lock_dir;
    bool automatic = false;
    int tx_frames = 0;
    int originator = 1;
    int net_id = 7;
    // Offset space (§10.5). Negative = below the die default: the safe end.
    int power_offset_qdb = -24;
    int mcs = 0;
    int settle_cycles = 0;
    uint16_t settle_quiet_mhz = 5180;
    int settle_timeout_ms = 2000;
    bool settle_fast = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (a == "--auto") {
            automatic = true;
        } else if (a == "--bus" && next) {
            buses.emplace_back(next);
            ++i;
        } else if (a == "--fd" && next) {
            unsigned b = 0, d = 0;
            if (std::sscanf(next, "%u/%u", &b, &d) != 2) {
                std::fprintf(stderr, "--fd wants <bus>/<dev>, got %s\n", next);
                return 2;
            }
            fds.emplace_back(b, d);
            ++i;
        } else if (a == "--chan-mhz" && next) {
            chan_mhz = static_cast<uint16_t>(std::atoi(next));
            ++i;
        } else if (a == "--seconds" && next) {
            seconds = std::atoi(next);
            ++i;
        } else if (a == "--lock-dir" && next) {
            lock_dir = next;
            ++i;
        } else if (a == "--tx" && next) {
            tx_frames = std::atoi(next);
            ++i;
        } else if (a == "--originator" && next) {
            originator = std::atoi(next);
            ++i;
        } else if (a == "--net-id" && next) {
            net_id = std::atoi(next);
            ++i;
        } else if (a == "--power-offset-qdb" && next) {
            power_offset_qdb = std::atoi(next);
            ++i;
        } else if (a == "--mcs" && next) {
            mcs = std::atoi(next);
            ++i;
        } else if (a == "--settle" && next) {
            settle_cycles = std::atoi(next);
            ++i;
        } else if (a == "--settle-quiet-mhz" && next) {
            settle_quiet_mhz = static_cast<uint16_t>(std::atoi(next));
            ++i;
        } else if (a == "--settle-timeout-ms" && next) {
            settle_timeout_ms = std::atoi(next);
            ++i;
        } else if (a == "--settle-fast") {
            settle_fast = true;
        } else {
            usage();
            return 2;
        }
    }

    // Settle mode is a measurement of the RX path; a DUT that also radiates
    // would be measuring its own TX side effects. One adapter, RX-only.
    if (settle_cycles != 0 && tx_frames != 0) {
        std::fprintf(stderr, "--settle and --tx are mutually exclusive\n");
        return 2;
    }

    const std::vector<Unit> units = enumerate();
    if (automatic) {
        if (units.empty()) {
            std::fprintf(stderr, "no Realtek units present\n");
            return 1;
        }
        for (const Unit& u : units) {
            buses.push_back(u.bus_path);
        }
    }
    if (buses.empty() && fds.empty()) {
        usage();
        return 2;
    }

    wblink::RadioAirCfg cfg;
    // RX-only unless --tx was asked for. Without it nothing here can
    // transmit, and every TX-die knob stays at its fail-closed default.
    cfg.allow_rx_only = tx_frames == 0;
    cfg.lock_dir = lock_dir;
    cfg.originator = static_cast<uint16_t>(originator);
    cfg.stamp_net_id = static_cast<uint8_t>(net_id);
    cfg.filter_net_id = static_cast<uint8_t>(net_id);

    std::vector<int> owned_fds;
    for (const std::string& b : buses) {
        wblink::AdapterCfg a;
        a.name = "bus-" + b;
        // Exactly one uplink, and only when --tx asked for one (§6.4).
        a.role = (tx_frames != 0 && cfg.adapters.empty()) ? wblink::Role::kTx
                                                          : wblink::Role::kRx;
        a.channel_mhz = chan_mhz;
        a.bus = b;
        cfg.adapters.push_back(a);
        cfg.adapter_fds.push_back(-1);
    }
    for (const auto& [b, d] : fds) {
        const int fd = open_usbfs(b, d);
        if (fd < 0) {
            for (int f : owned_fds) ::close(f);
            return 1;
        }
        owned_fds.push_back(fd);
        wblink::AdapterCfg a;
        a.name = "fd-" + std::to_string(b) + "/" + std::to_string(d);
        a.role = (tx_frames != 0 && cfg.adapters.empty()) ? wblink::Role::kTx
                                                          : wblink::Role::kRx;
        a.channel_mhz = chan_mhz;
        // No bus pin: create() refuses one on an fd-supplied stanza.
        cfg.adapters.push_back(a);
        cfg.adapter_fds.push_back(fd);
    }
    // B4: mandatory whenever any fd is supplied — the reset would
    // re-enumerate and orphan the descriptors opened above.
    cfg.do_reset = fds.empty();

    std::fprintf(stderr,
                 "bring-up: %zu adapter(s), %zu wrapped fd(s), ch %u MHz, "
                 "do_reset=%s, lock_dir=%s\n",
                 cfg.adapters.size(), fds.size(), chan_mhz,
                 cfg.do_reset ? "true" : "false",
                 lock_dir.empty() ? "(devourer default)" : lock_dir.c_str());
    if (tx_frames != 0) {
        std::fprintf(stderr,
                     "  *** TX MODE: %d frames, mcs %d, power offset %d qdb, "
                     "originator %d, net_id %d — THIS RADIATES ***\n",
                     tx_frames, mcs, power_offset_qdb, originator, net_id);
    }

    int rc = 0;
    size_t tx_sent = 0;
    {
        auto air = wblink::RadioAir::create(cfg);
        if (!air) {
            std::fprintf(stderr, "FAIL create: %s\n", air.error.c_str());
            for (int f : owned_fds) ::close(f);
            return 1;
        }
        wblink::RadioAir& a = *air.value;
        for (size_t i = 0; i < a.rx_adapters(); ++i) {
            const std::string mac = a.adapter_mac(i);
            std::fprintf(stderr, "  adapter %zu: mac %s%s\n", i,
                         mac.empty() ? "NONE" : mac.c_str(),
                         mac.empty() ? "  <-- no identity (D3 fail-closed)"
                                     : "");
            if (mac.empty()) {
                rc = 1;  // §10.6: a unit with no identity cannot be trusted
            }
        }
        if (tx_frames != 0) {
            // Safe end first: lowest rate, power well below the die default.
            a.set_tx_mode(static_cast<uint8_t>(mcs), false);
            if (!a.set_power_offset_qdb(a.tx_index(), power_offset_qdb)) {
                std::fprintf(stderr, "FAIL: set_power_offset_qdb refused\n");
                rc = 1;
            }
            // The body is never parsed at the layer this counts, but it is
            // NOT free-form: dot11_parse's pre-check requires the §3.1 magic
            // 0x57 0x42 ("WB") as the first two payload bytes, and a frame
            // without it is heard and counted as rx_filtered, never
            // rx_frames. Measured the hard way — an 0xA5 filler burst put
            // 236 frames in the ear's filtered counter and zero in its
            // accepted one, which looks exactly like a TX failure and is not.
            std::vector<uint8_t> payload(200, 0xA5);
            payload[0] = 0x57;
            payload[1] = 0x42;
            size_t sent = 0;
            for (int f = 0; f < tx_frames; ++f) {
                sent += a.inject(payload.data(), payload.size());
                // ~1 kHz. Deliberately unhurried: this is a liveness proof,
                // not a throughput test, and a tight loop would just measure
                // the USB bulk queue.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            tx_sent = sent;
        }
        if (settle_cycles != 0) {
            if (a.rx_adapters() != 1) {
                std::fprintf(stderr,
                             "FAIL: --settle wants exactly one adapter (the "
                             "DUT), got %zu\n",
                             a.rx_adapters());
                rc = 1;
            } else {
                const auto caps = a.adapter_caps(0);
                std::fprintf(stderr,
                             "settle: chip=%s %s-retune, %u -> %u MHz, %d "
                             "cycles (+2 warm-up), timeout %d ms\n",
                             caps.chip.c_str(), settle_fast ? "fast" : "full",
                             settle_quiet_mhz, chan_mhz, settle_cycles,
                             settle_timeout_ms);
                // Two warm-up cycles: the first hops after bring-up carry
                // one-time state (initial AGC history, first USB URB fill)
                // that the steady-state scout hop does not. Printed, never
                // summarized.
                std::vector<double> deaf;
                int timeouts = 0, tainted = 0;
                for (int cyc = -2; cyc < settle_cycles; ++cyc) {
                    const SettleSample s = settle_cycle(
                        a, settle_quiet_mhz, chan_mhz, settle_fast,
                        settle_timeout_ms);
                    char deaf_str[32];
                    if (s.deaf_ms < 0) {
                        std::snprintf(deaf_str, sizeof deaf_str, "TIMEOUT");
                    } else {
                        std::snprintf(deaf_str, sizeof deaf_str, "%.1f ms",
                                      s.deaf_ms);
                    }
                    std::fprintf(
                        stderr, "  settle[%d]: call=%.2f ms deaf=%s%s\n", cyc,
                        s.retune_call_ms, deaf_str,
                        s.quiet_frames
                            ? "  <-- TAINTED: quiet channel heard frames"
                            : "");
                    if (cyc < 0) continue;  // warm-up
                    if (s.quiet_frames) {
                        ++tainted;
                    } else if (s.deaf_ms < 0) {
                        ++timeouts;
                    } else {
                        deaf.push_back(s.deaf_ms);
                    }
                }
                std::sort(deaf.begin(), deaf.end());
                if (deaf.empty()) {
                    // No sample = no measurement. An emitter that was never
                    // heard must not read as "settles instantly".
                    std::fprintf(stderr,
                                 "FAIL settle: 0 clean samples (%d timeouts, "
                                 "%d tainted) — no emitter heard?\n",
                                 timeouts, tainted);
                    rc = 1;
                } else {
                    const auto pct = [&](double p) {
                        return deaf[static_cast<size_t>(
                            p * static_cast<double>(deaf.size() - 1))];
                    };
                    std::fprintf(stderr,
                                 "settle summary: n=%zu timeouts=%d "
                                 "tainted=%d  min=%.1f p50=%.1f p90=%.1f "
                                 "max=%.1f ms\n",
                                 deaf.size(), timeouts, tainted, deaf.front(),
                                 pct(0.5), pct(0.9), deaf.back());
                }
            }
        }
        // Dwell so the RX threads actually run, then report. Frames are not
        // required (an idle channel is legitimate); a live counter proves the
        // loop is alive when there IS traffic.
        for (int s = 0; s < seconds; ++s) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        // TX counters AFTER the dwell, not straight after the inject loop:
        // CCX reports arrive asynchronously, so reading them immediately
        // undercounts (measured 353/500 read early vs the true tail). Use
        // --seconds to size the settle.
        if (tx_frames != 0) {
            const auto c = a.counters(a.tx_index());
            std::fprintf(stderr,
                         "  TX: inject_ok=%zu submitted=%llu failed=%llu "
                         "reports=%llu report_fails=%llu\n",
                         tx_sent,
                         static_cast<unsigned long long>(c.tx_submitted),
                         static_cast<unsigned long long>(c.tx_failed),
                         static_cast<unsigned long long>(c.tx_reports),
                         static_cast<unsigned long long>(c.tx_report_fails));
            if (tx_sent != static_cast<size_t>(tx_frames) ||
                c.tx_failed != 0) {
                std::fprintf(stderr, "FAIL: not every frame was submitted\n");
                rc = 1;
            }
        }
        for (size_t i = 0; i < a.rx_adapters(); ++i) {
            const auto& c = a.counters(i);
            std::fprintf(stderr,
                         "  adapter %zu: rx_frames=%llu filtered=%llu "
                         "dropped=%llu\n",
                         i, static_cast<unsigned long long>(c.rx_frames),
                         static_cast<unsigned long long>(c.rx_filtered),
                         static_cast<unsigned long long>(c.rx_dropped));
        }
        std::fprintf(stderr, "teardown...\n");
    }
    // After teardown, so it also proves libusb left our descriptors alone
    // (fd_keep): a closed fd would make this fail.
    for (int f : owned_fds) {
        if (::fcntl(f, F_GETFD) < 0) {
            std::fprintf(stderr,
                         "FAIL: teardown closed our fd %d — ownership "
                         "contract broken\n",
                         f);
            rc = 1;
        }
        ::close(f);
    }
    std::fprintf(stderr, "%s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}
