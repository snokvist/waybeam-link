// SPDX-License-Identifier: GPL-2.0-or-later
//
// Teardown/reconstruct cycle harness — Step 0 of the §9.10 v2 in-process
// recovery question (`docs/findings.md`, 2026-08-10).
//
// THE QUESTION. §9.10 v2 answers a wedged transmitter by exiting 9 and letting
// the supervisor re-exec, because Pass 148 concluded "a full in-process re-init
// is impossible while devourer's InitWrite unconditionally assigns
// _coex_thread and std::terminates on a second call". That conclusion is about
// calling InitWrite TWICE ON ONE LIVE OBJECT. A freshly constructed device has
// a default-constructed, non-joinable `_coex_thread`, so the assignment at
// third_party/devourer/src/jaguar3/RtlJaguar3Device.cpp:900 is legal — and both
// `Stop()` (:684) and `~RtlJaguar3Device` (:390) set `_coex_stop` and join it
// first. So destroy-and-reconstruct was never actually tried.
//
// This harness tries it, and nothing more: N complete RadioAir lifecycles in
// ONE process, on a HEALTHY adapter. It is the gate, not the experiment — if a
// healthy adapter cannot survive the cycle, no wedge ever will, and the answer
// is "keep exit 9" without touching a craft.
//
// IT MUST TRANSMIT, and that is not incidental. `hwtrial_bringup` creates
// adapters `allow_rx_only`, which takes devourer's `Init` — NOT `InitWrite` —
// so it never touches the `_coex_thread` hazard this exists to test. Every
// adapter here is `role:"tx"`. Sweep from the safe end: the default power
// offset is deep below the die default and the default rate is MCS 0.
//
// LIVENESS IS MEASURED BY CCX REPORTS, not by a second radio. This bench host
// has one usable Realtek unit, so there is no ear. That is not a fudge: §9.10
// defines the wedge as "submissions advancing, zero backend TX progress", and
// `tx_reports` IS that backend progress signal. A cycle whose reports track its
// submissions is live by the same instrument the wedge detector uses. Over-air
// confirmation needs a second node and is a separate run.
//
//   hwtrial_reinit --bus 5-1 --cycles 20 --tx 200
//
// Exit 0 only if every cycle completed, every cycle transmitted, and fds,
// threads and RSS stayed at their post-first-cycle baseline. Anything else is a
// FAIL with the reason on stderr — a harness that cannot fail would answer this
// question wrongly and quietly.

// ---------------------------------------------------------------------------
// STEP 1 (--wedge-watch) tests the thing Step 0 only made plausible: whether a
// destroy/reconstruct clears an ACTUAL §9.10 wedge, and if so whether it needs
// the kernel-driver unbind that deploy/vehicle-waybeam-link.init performs
// before every supervised respawn.
//
// Two things about the method, both deliberate:
//
//   - THE DETECTOR IS THE PRODUCTION ONE. `wblink::TxWedge` (io/include/
//     wblink/txwedge.h) is a pure, clock-injected class, so this harness polls
//     exactly what run_tx polls, with the craft's own window/min-submit policy.
//     A reimplemented detector would risk answering about a different signal.
//   - THE HARNESS INDUCES THE FAULT ITSELF. Pass 147's induction is a usbfs
//     deauthorize/reauthorize; driving it from here rather than from a second
//     shell removes the operator's reaction time from every interval reported,
//     which matters when the baseline being compared against is ~12 s.
//
// Arms, matching the finding's A0/A1/A2:
//   --on-wedge report            observe and stop (what exit 9 does today)
//   --on-wedge recycle           destroy + reconstruct, no unbind        (A1)
//   --on-wedge recycle --unbind-driver X --unbind-if Y   + sysfs unbind  (A2)
// ---------------------------------------------------------------------------

#include <arpa/inet.h>
#include <dirent.h>
#include <libusb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "wblink/air_radio.h"
#include "wblink/txwedge.h"

// devourer's process-global stop flag (third_party/devourer/include/SignalStop.h).
// Declared rather than included so this file needs no vendored include path.
//
// NOTHING in core/, io/, app/ or node/ includes that header, so waybeam-link
// never installs devourer's signal handlers and the flag should be false for
// the whole process lifetime. "Should" is why it is printed: if it ever went
// true, every RX/TX loop would exit on its own and a cycle failure here would
// mean nothing about re-init.
extern volatile bool g_devourer_should_stop;

namespace {

struct Sample {
    long fds = -1;
    long threads = -1;
    long rss_kb = -1;
};

// Counting /proc/self/fd needs an fd of its own, so every sample is biased by
// exactly one. Consistent bias, and the verdict compares samples to each other.
long count_entries(const char* path) {
    DIR* d = ::opendir(path);
    if (d == nullptr) return -1;
    long n = 0;
    while (const dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        ++n;
    }
    ::closedir(d);
    return n;
}

long rss_kb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof line, f) != nullptr) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            kb = std::strtol(line + 6, nullptr, 10);
            break;
        }
    }
    std::fclose(f);
    return kb;
}

Sample sample() {
    Sample s;
    s.fds = count_entries("/proc/self/fd");
    s.threads = count_entries("/proc/self/task");
    s.rss_kb = rss_kb();
    return s;
}

struct CycleResult {
    bool created = false;
    bool transmitted = false;
    size_t inject_ok = 0;
    unsigned long long submitted = 0;
    unsigned long long failed = 0;
    unsigned long long reports = 0;
    Sample after;
};

void usage() {
    std::fprintf(stderr,
                 "usage: hwtrial_reinit --bus <path> [--cycles N] [--tx N]\n"
                 "       [--chan-mhz N] [--mcs N] [--power-offset-qdb N]\n"
                 "       [--originator N] [--net-id N] [--dwell-ms N]\n"
                 "       [--rss-slack-kb N] [--lock-dir DIR]\n"
                 "  wedge mode (step 1):\n"
                 "       --wedge-watch --episodes N --on-wedge report|recycle\n"
                 "       [--unbind-driver NAME --unbind-if IFACE]\n"
                 "       [--usb-dev 1-1] [--induce-after-ms N] "
                 "[--down-ms N]\n"
                 "       [--settle-ms N] [--restore-timeout-ms N]\n"
                 "       [--wedge-window-ms N] [--wedge-min-submits N]\n"
                 "       [--wedge-exit-windows N] [--inject-hz N]\n"
                 "       [--hold-listen PORT]   listener-survival probe\n"
                 "  THIS RADIATES: every adapter is role:\"tx\".\n");
}

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// A stand-in for the §15.5 control listener, so "the control server stays bound
// across an in-process recycle" is measured rather than reasoned. It is the
// same property the real one has — a listening fd owned by the process, not by
// the radio — and the claim is exactly that the recycle never touches it.
// Returns -1 if the port could not be bound.
int hold_listener(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 ||
        ::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Still accepting? A connect() that completes proves the listener survived.
bool listener_alive(int listen_fd, uint16_t port) {
    if (listen_fd < 0) return false;
    const int c = ::socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    const bool ok =
        ::connect(c, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0;
    ::close(c);
    if (ok) {
        const int a = ::accept(listen_fd, nullptr, nullptr);
        if (a >= 0) ::close(a);
    }
    return ok;
}

// Write one byte into a sysfs attribute. Returns false and says why — a silent
// failure here would turn "the fault was never induced" into "recovery worked".
bool sysfs_write(const std::string& path, const char* val) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        std::fprintf(stderr, "  sysfs open %s: %s\n", path.c_str(),
                     std::strerror(errno));
        return false;
    }
    const bool ok = std::fputs(val, f) >= 0;
    // fclose is where a sysfs store actually reports its error.
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        std::fprintf(stderr, "  sysfs write %s <- %s: %s\n", path.c_str(), val,
                     std::strerror(errno));
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string bus;
    std::string lock_dir;
    int cycles = 20;
    int tx_frames = 200;
    uint16_t chan_mhz = 5805;
    int mcs = 0;
    // Offset space (§10.5): negative is below the die default — the safe end.
    int power_offset_qdb = -72;
    int originator = 1;
    int net_id = 7;
    int dwell_ms = 1500;
    // CCX reports arrive asynchronously and RSS moves with allocator slack, so
    // the RSS verdict needs a stated tolerance rather than exact equality. fds
    // and threads get none: those leak in whole units or not at all.
    long rss_slack_kb = 2048;

    // --- wedge mode (step 1) ---
    bool wedge_watch = false;
    int episodes = 5;
    std::string on_wedge = "report";
    std::string usb_dev;         // "1-1" — the sysfs USB device to cycle
    std::string unbind_driver;   // e.g. "rtl88x2eu"; empty = A1 (no unbind)
    std::string unbind_if;       // e.g. "1-1:1.0"
    int induce_after_ms = 8000;  // let the link settle before faulting it
    int down_ms = 3000;          // how long the device stays deauthorized
    int settle_ms = 2000;        // after reauthorize, before reconstruct
    int restore_timeout_ms = 30000;
    int inject_hz = 200;
    int hold_listen_port = 0;  // 0 = do not test listener survival
    // Defaults are the shipped §9.10 seeds (io/include/wblink/config.h). Match
    // them to the craft's own config when running against a craft — a harness
    // with a different policy answers about a different wedge.
    uint32_t wedge_window_ms = 1000;
    uint32_t wedge_min_submits = 8;
    uint32_t wedge_exit_windows = 3;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (a == "--bus" && next) { bus = next; ++i; }
        else if (a == "--cycles" && next) { cycles = std::atoi(next); ++i; }
        else if (a == "--tx" && next) { tx_frames = std::atoi(next); ++i; }
        else if (a == "--chan-mhz" && next) {
            chan_mhz = static_cast<uint16_t>(std::atoi(next)); ++i;
        } else if (a == "--mcs" && next) { mcs = std::atoi(next); ++i; }
        else if (a == "--power-offset-qdb" && next) {
            power_offset_qdb = std::atoi(next); ++i;
        } else if (a == "--originator" && next) {
            originator = std::atoi(next); ++i;
        } else if (a == "--net-id" && next) { net_id = std::atoi(next); ++i; }
        else if (a == "--dwell-ms" && next) { dwell_ms = std::atoi(next); ++i; }
        else if (a == "--rss-slack-kb" && next) {
            rss_slack_kb = std::strtol(next, nullptr, 10); ++i;
        } else if (a == "--lock-dir" && next) { lock_dir = next; ++i; }
        else if (a == "--wedge-watch") { wedge_watch = true; }
        else if (a == "--episodes" && next) { episodes = std::atoi(next); ++i; }
        else if (a == "--on-wedge" && next) { on_wedge = next; ++i; }
        else if (a == "--unbind-driver" && next) { unbind_driver = next; ++i; }
        else if (a == "--unbind-if" && next) { unbind_if = next; ++i; }
        else if (a == "--usb-dev" && next) { usb_dev = next; ++i; }
        else if (a == "--induce-after-ms" && next) {
            induce_after_ms = std::atoi(next); ++i;
        } else if (a == "--down-ms" && next) { down_ms = std::atoi(next); ++i; }
        else if (a == "--settle-ms" && next) { settle_ms = std::atoi(next); ++i; }
        else if (a == "--restore-timeout-ms" && next) {
            restore_timeout_ms = std::atoi(next); ++i;
        } else if (a == "--wedge-window-ms" && next) {
            wedge_window_ms = static_cast<uint32_t>(std::atoi(next)); ++i;
        } else if (a == "--wedge-min-submits" && next) {
            wedge_min_submits = static_cast<uint32_t>(std::atoi(next)); ++i;
        } else if (a == "--wedge-exit-windows" && next) {
            wedge_exit_windows = static_cast<uint32_t>(std::atoi(next)); ++i;
        } else if (a == "--inject-hz" && next) {
            inject_hz = std::atoi(next); ++i;
        } else if (a == "--hold-listen" && next) {
            hold_listen_port = std::atoi(next); ++i;
        }
        else { usage(); return 2; }
    }
    if (wedge_watch) {
        // --unbind-driver and --unbind-if are the A2 pair; one without the
        // other would silently run A1 under an A2 label.
        if (bus.empty() || usb_dev.empty() || episodes < 1 || inject_hz < 1 ||
            (on_wedge != "report" && on_wedge != "recycle") ||
            unbind_driver.empty() != unbind_if.empty()) {
            usage();
            return 2;
        }
    } else if (bus.empty() || cycles < 2 || tx_frames < 1) {
        // Fewer than two cycles cannot answer the question: the whole point is
        // what the SECOND construction does.
        usage();
        return 2;
    }

    std::fprintf(stderr,
                 "hwtrial_reinit: bus %s, %d cycles, %d frames/cycle, ch %u "
                 "MHz, mcs %d, power offset %d qdb\n"
                 "  *** THIS RADIATES ***\n",
                 bus.c_str(), cycles, tx_frames, chan_mhz, mcs,
                 power_offset_qdb);
    std::fprintf(stderr, "  g_devourer_should_stop at start: %s\n",
                 g_devourer_should_stop ? "TRUE (!!)" : "false");

    std::vector<uint8_t> payload(200, 0xA5);
    // §3.1 magic: without it an ear counts the frame as rx_filtered, never
    // rx_frames — which looks exactly like a TX failure and is not.
    payload[0] = 0x57;
    payload[1] = 0x42;

    // Shared by both modes so an arm can never differ from the Step 0 gate in
    // how the adapter is brought up.
    auto build_air = [&]() {
        wblink::RadioAirCfg cfg;
        cfg.allow_rx_only = false;   // InitWrite — the whole subject
        cfg.lock_dir = lock_dir;
        cfg.originator = static_cast<uint16_t>(originator);
        cfg.stamp_net_id = static_cast<uint8_t>(net_id);
        cfg.filter_net_id = static_cast<uint8_t>(net_id);
        cfg.do_reset = true;
        wblink::AdapterCfg ad;
        ad.name = "bus-" + bus;
        ad.role = wblink::Role::kTx;
        ad.channel_mhz = chan_mhz;
        ad.bus = bus;
        cfg.adapters.push_back(ad);
        cfg.adapter_fds.push_back(-1);
        return wblink::RadioAir::create(cfg);
    };

    if (wedge_watch) {
        const std::string dev_path = "/sys/bus/usb/devices/" + usb_dev;
        std::fprintf(stderr,
                     "wedge mode: %d episode(s), on_wedge=%s, arm=%s\n"
                     "  policy: window %u ms, min_submits %u, exit_windows %u\n"
                     "  induction: %s authorized 0 -> wait %d ms -> 1, "
                     "settle %d ms\n",
                     episodes, on_wedge.c_str(),
                     on_wedge == "report"        ? "A0-observer"
                     : unbind_driver.empty()     ? "A1 (no unbind)"
                                                 : "A2 (with unbind)",
                     wedge_window_ms, wedge_min_submits, wedge_exit_windows,
                     dev_path.c_str(), down_ms, settle_ms);

        int listen_fd = -1;
        if (hold_listen_port != 0) {
            listen_fd = hold_listener(static_cast<uint16_t>(hold_listen_port));
            if (listen_fd < 0) {
                std::fprintf(stderr,
                             "FAIL: could not bind 127.0.0.1:%d for the "
                             "listener-survival check\n",
                             hold_listen_port);
                return 1;
            }
            std::fprintf(stderr,
                         "  listener-survival probe bound on 127.0.0.1:%d\n",
                         hold_listen_port);
        }
        int listener_lost = 0;

        auto air = build_air();
        if (!air) {
            std::fprintf(stderr, "FAIL: initial create: %s\n",
                         air.error.c_str());
            return 1;
        }
        int cleared = 0;
        int attempted = 0;
        const auto inject_period =
            std::chrono::microseconds(1000000 / inject_hz);

        for (int ep = 1; ep <= episodes; ++ep) {
            wblink::TxWedge wedge(
                wblink::TxWedgePolicy{wedge_window_ms, wedge_min_submits});
            const uint64_t t_start = now_ms();
            uint64_t t_induced = 0, t_verdict = 0, t_reconstructed = 0;
            uint64_t reports_at_rebuild = 0, submitted_at_rebuild = 0;
            bool down = false, up_again = false, recycled = false;
            bool episode_done = false, episode_ok = false;
            ++attempted;

            std::fprintf(stderr, "--- episode %d/%d ---\n", ep, episodes);
            while (!episode_done) {
                const uint64_t t = now_ms();
                const uint64_t dt = t - t_start;
                if (air) {
                    air.value->inject(payload.data(), payload.size());
                }
                // Induce, then release. Both are timestamped here rather than
                // in a shell so the reported intervals contain no human.
                if (!down && dt >= static_cast<uint64_t>(induce_after_ms)) {
                    down = true;
                    t_induced = t;
                    std::fprintf(stderr,
                                 "[%5llu ms] INDUCE: deauthorize %s\n",
                                 (unsigned long long)dt, usb_dev.c_str());
                    if (!sysfs_write(dev_path + "/authorized", "0")) {
                        std::fprintf(stderr,
                                     "FAIL: could not induce — the instrument "
                                     "is not working, so a negative result "
                                     "here would mean nothing\n");
                        return 1;
                    }
                }
                if (down && !up_again &&
                    t - t_induced >= static_cast<uint64_t>(down_ms)) {
                    up_again = true;
                    std::fprintf(stderr, "[%5llu ms] reauthorize %s\n",
                                 (unsigned long long)(t - t_start),
                                 usb_dev.c_str());
                    if (!sysfs_write(dev_path + "/authorized", "1")) {
                        return 1;
                    }
                }

                uint64_t submitted = 0, reports = 0;
                if (air) {
                    air.value->tx_report_counters(submitted, reports);
                }
                if (air && wedge.poll(t, submitted, reports)) {
                    std::fprintf(stderr, "[%5llu ms] wedge %s\n",
                                 (unsigned long long)(t - t_start),
                                 wedge.wedged() ? "TRUE" : "cleared");
                }
                // Restoration is measured DIRECTLY off the counters, not off
                // the detector's cleared transition. A reconstructed adapter
                // gets a fresh TxWedge that starts un-wedged, so recovery
                // produces no transition to observe — the first bench run
                // reported 0/2 while its own numbers showed 5058 CCX reports
                // on the reconstructed object. Progress after the rebuild is
                // the claim; measure the claim.
                if (recycled && !episode_done && air &&
                    reports >= reports_at_rebuild + wedge_min_submits &&
                    submitted > submitted_at_rebuild) {
                    const uint64_t t_now = now_ms();
                    const auto c = air.value->counters(air.value->tx_index());
                    std::fprintf(
                        stderr,
                        "[%5llu ms] RESTORED  submitted=%llu failed=%llu "
                        "reports=%llu (+%llu reports since rebuild)\n"
                        "  t(induce->verdict)=%llu ms  "
                        "t(verdict->reconstructed)=%llu ms  "
                        "t(reconstructed->restored)=%llu ms  "
                        "t(induce->restored)=%llu ms\n",
                        (unsigned long long)(t_now - t_start),
                        (unsigned long long)c.tx_submitted,
                        (unsigned long long)c.tx_failed,
                        (unsigned long long)c.tx_reports,
                        (unsigned long long)(reports - reports_at_rebuild),
                        (unsigned long long)(t_verdict - t_induced),
                        (unsigned long long)(t_reconstructed - t_verdict),
                        (unsigned long long)(t_now - t_reconstructed),
                        (unsigned long long)(t_now - t_induced));
                    if (listen_fd >= 0) {
                        const bool alive = listener_alive(
                            listen_fd, static_cast<uint16_t>(hold_listen_port));
                        std::fprintf(stderr,
                                     "  listener on :%d after recycle: %s\n",
                                     hold_listen_port,
                                     alive ? "STILL ACCEPTING"
                                           : "GONE (!!)");
                        if (!alive) ++listener_lost;
                    }
                    ++cleared;
                    episode_ok = true;
                    episode_done = true;
                    continue;
                }
                if (!recycled && wedge.consecutive_wedged() >= wedge_exit_windows) {
                    t_verdict = t;
                    const auto c = air.value->counters(air.value->tx_index());
                    std::fprintf(stderr,
                                 "[%5llu ms] VERDICT §9.10 after %u "
                                 "consecutive windows  submitted=%llu "
                                 "failed=%llu reports=%llu\n",
                                 (unsigned long long)(t - t_start),
                                 wedge.consecutive_wedged(),
                                 (unsigned long long)c.tx_submitted,
                                 (unsigned long long)c.tx_failed,
                                 (unsigned long long)c.tx_reports);
                    if (on_wedge == "report") {
                        std::fprintf(stderr,
                                     "  arm A0-observer: this is where the "
                                     "daemon returns kTxWedged and exits 9\n");
                        episode_ok = true;   // observing is the whole job here
                        episode_done = true;
                        continue;
                    }
                    // ---- the experiment ----
                    air.value.reset();       // full teardown (Step 0's path)
                    if (!unbind_driver.empty()) {
                        // A2: hand the device back to nobody. The kernel driver
                        // re-binds on re-enumeration, and the supervised
                        // respawn undoes that before every spawn.
                        std::fprintf(stderr, "  A2: unbind %s from %s\n",
                                     unbind_if.c_str(), unbind_driver.c_str());
                        sysfs_write("/sys/bus/usb/drivers/" + unbind_driver +
                                        "/unbind",
                                    unbind_if.c_str());
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(settle_ms));
                    const uint64_t t_try = now_ms();
                    while (true) {
                        air = build_air();
                        if (air) break;
                        if (now_ms() - t_try >
                            static_cast<uint64_t>(restore_timeout_ms)) {
                            std::fprintf(stderr,
                                         "  reconstruct FAILED for %d ms: %s\n",
                                         restore_timeout_ms,
                                         air.error.c_str());
                            break;
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(500));
                    }
                    if (!air) {
                        episode_done = true;
                        continue;   // episode_ok stays false
                    }
                    t_reconstructed = now_ms();
                    recycled = true;
                    air.value->tx_report_counters(
                        submitted_at_rebuild, reports_at_rebuild);
                    std::fprintf(stderr,
                                 "[%5llu ms] reconstructed (%llu ms after "
                                 "verdict) — watching for progress\n",
                                 (unsigned long long)(t_reconstructed - t_start),
                                 (unsigned long long)(t_reconstructed -
                                                      t_verdict));
                    // A fresh object means fresh counters, so the detector has
                    // to re-anchor or its first window compares across the
                    // discontinuity.
                    wedge = wblink::TxWedge(wblink::TxWedgePolicy{
                        wedge_window_ms, wedge_min_submits});
                    // MUST restart the iteration: `t` was read before a
                    // teardown/settle/rebuild that takes seconds, and the
                    // timeout below would compare a stale `t` against
                    // t_reconstructed and underflow (uint64) into an instant
                    // "NOT RESTORED". Measured — the first bench run reported
                    // 0/2 entirely on this bug, with a timestamp 5 s in the
                    // past printed next to the reconstruct that preceded it.
                    continue;
                }
                if (recycled && now_ms() >= t_reconstructed &&
                    now_ms() - t_reconstructed >
                        static_cast<uint64_t>(restore_timeout_ms)) {
                    std::fprintf(stderr,
                                 "[%5llu ms] NOT RESTORED within %d ms of "
                                 "reconstruct — this is where the daemon must "
                                 "still fall through to exit 9\n",
                                 (unsigned long long)(t - t_start),
                                 restore_timeout_ms);
                    episode_done = true;
                    continue;
                }
                if (!down && dt > static_cast<uint64_t>(induce_after_ms) +
                                      static_cast<uint64_t>(restore_timeout_ms)) {
                    std::fprintf(stderr, "  episode timed out before induction\n");
                    episode_done = true;
                }
                std::this_thread::sleep_for(inject_period);
            }
            if (!episode_ok) {
                std::fprintf(stderr, "episode %d: NOT CLEARED\n", ep);
            }
            // Rebuild for the next episode if the arm ended without a live
            // adapter, so one bad episode does not poison the rest. Not after
            // the last one — that only delays the verdict.
            if (!air && ep < episodes) {
                air = build_air();
                if (!air) {
                    std::fprintf(stderr,
                                 "cannot rebuild between episodes: %s\n",
                                 air.error.c_str());
                    break;
                }
            }
        }

        std::fprintf(stderr, "  g_devourer_should_stop at end: %s\n",
                     g_devourer_should_stop ? "TRUE (!!)" : "false");
        if (on_wedge == "report") {
            std::fprintf(stderr, "OBSERVED %d/%d episodes reached a verdict\n",
                         cleared, attempted);
            return cleared == attempted ? 0 : 1;
        }
        // 5/5 or it is not first-line: an intermittent recovery on a craft with
        // no second link is worse than a predictable restart.
        if (listen_fd >= 0) {
            std::fprintf(stderr, "  listener survived %d/%d recycles\n",
                         cleared - listener_lost, cleared);
        }
        const bool pass = cleared == attempted && attempted > 0 &&
                          listener_lost == 0;
        std::fprintf(stderr, "CLEARED IN-PROCESS %d/%d — %s\n", cleared,
                     attempted, pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }

    std::vector<CycleResult> results;
    results.reserve(static_cast<size_t>(cycles));
    const Sample before_all = sample();
    std::fprintf(stderr, "  before any cycle: fd=%ld task=%ld rss=%ldkB\n",
                 before_all.fds, before_all.threads, before_all.rss_kb);

    for (int c = 1; c <= cycles; ++c) {
        CycleResult r;
        {
            auto air = build_air();
            if (!air) {
                std::fprintf(stderr, "cycle %2d/%d: CREATE FAILED: %s\n", c,
                             cycles, air.error.c_str());
                r.after = sample();
                results.push_back(r);
                break;  // a failed construction makes later cycles meaningless
            }
            r.created = true;
            wblink::RadioAir& a = *air.value;
            a.set_tx_mode(static_cast<uint8_t>(mcs), false);
            if (!a.set_power_offset_qdb(a.tx_index(), power_offset_qdb)) {
                std::fprintf(stderr,
                             "cycle %2d/%d: set_power_offset_qdb REFUSED\n", c,
                             cycles);
            } else {
                for (int f = 0; f < tx_frames; ++f) {
                    r.inject_ok += a.inject(payload.data(), payload.size());
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                // Reports are asynchronous; read them after a settle or the
                // tail is undercounted.
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(dwell_ms));
                const auto ct = a.counters(a.tx_index());
                r.submitted = ct.tx_submitted;
                r.failed = ct.tx_failed;
                r.reports = ct.tx_reports;
                r.transmitted = r.inject_ok == static_cast<size_t>(tx_frames) &&
                                ct.tx_failed == 0 && ct.tx_reports > 0;
            }
        }   // <-- the teardown under test
        // Sampled AFTER the destructor, so anything the cycle leaked is here.
        r.after = sample();
        results.push_back(r);
        std::fprintf(stderr,
                     "cycle %2d/%d: create=%s inject=%zu/%d submitted=%llu "
                     "failed=%llu reports=%llu | fd=%ld task=%ld rss=%ldkB\n",
                     c, cycles, r.created ? "OK" : "FAIL", r.inject_ok,
                     tx_frames, r.submitted, r.failed, r.reports, r.after.fds,
                     r.after.threads, r.after.rss_kb);
    }

    std::fprintf(stderr, "  g_devourer_should_stop at end: %s\n",
                 g_devourer_should_stop ? "TRUE (!!)" : "false");

    // ---- verdict ----------------------------------------------------------
    // The baseline is the state after cycle 1, not before it: first-use
    // allocations (libusb, spdlog, the firmware blob) are not a leak, and
    // comparing to a pre-first-cycle sample would fail every healthy run.
    int failures = 0;
    if (static_cast<int>(results.size()) != cycles) {
        std::fprintf(stderr,
                     "FAIL: only %zu of %d cycles ran — a construction failed\n",
                     results.size(), cycles);
        ++failures;
    }
    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].created) {
            std::fprintf(stderr, "FAIL: cycle %zu did not construct\n", i + 1);
            ++failures;
        } else if (!results[i].transmitted) {
            std::fprintf(stderr,
                         "FAIL: cycle %zu came up but did not transmit "
                         "(inject_ok=%zu failed=%llu reports=%llu)\n",
                         i + 1, results[i].inject_ok, results[i].failed,
                         results[i].reports);
            ++failures;
        }
    }
    if (results.size() >= 2) {
        const Sample& base = results[0].after;
        for (size_t i = 1; i < results.size(); ++i) {
            const Sample& s = results[i].after;
            if (s.fds > base.fds) {
                std::fprintf(stderr,
                             "FAIL: cycle %zu leaked fds: %ld > baseline %ld\n",
                             i + 1, s.fds, base.fds);
                ++failures;
            }
            if (s.threads > base.threads) {
                std::fprintf(stderr,
                             "FAIL: cycle %zu leaked threads: %ld > baseline "
                             "%ld\n",
                             i + 1, s.threads, base.threads);
                ++failures;
            }
        }
        const Sample& last = results.back().after;
        if (last.rss_kb > base.rss_kb + rss_slack_kb) {
            std::fprintf(stderr,
                         "FAIL: RSS grew %ldkB over %zu cycles (slack %ldkB)\n",
                         last.rss_kb - base.rss_kb, results.size() - 1,
                         rss_slack_kb);
            ++failures;
        }
        std::fprintf(stderr,
                     "  baseline (after cycle 1): fd=%ld task=%ld rss=%ldkB\n"
                     "  final    (after cycle %zu): fd=%ld task=%ld rss=%ldkB "
                     "(rss %+ldkB)\n",
                     base.fds, base.threads, base.rss_kb, results.size(),
                     last.fds, last.threads, last.rss_kb,
                     last.rss_kb - base.rss_kb);
    }

    std::fprintf(stderr, "%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
