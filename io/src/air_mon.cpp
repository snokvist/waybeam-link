// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/air_mon.h"

#include <arpa/inet.h>        // htons
#include <linux/filter.h>     // sock_filter, sock_fprog, SO_ATTACH_FILTER
#include <linux/if_ether.h>   // ETH_P_ALL
#include <linux/if_packet.h>  // sockaddr_ll
#include <net/if.h>           // if_nametoindex
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/time.h>  // struct timeval (SO_RCVTIMEO)
#include <sys/wait.h>  // waitpid (retune via iw)
#include <csignal>     // kill, SIGKILL (B2 bounded CLI wait)
#include <unistd.h>    // close, fork, execvp, _exit

#include <linux/sockios.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>  // strtoull
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "wblink/dot11.h"
#include "wblink/airtime.h"
#include "wblink/radiotap.h"
#include "wblink/types.h"

namespace wblink {

namespace {
constexpr size_t kRxQueueCap = 512;
constexpr size_t kRxBufLen = 4096;
constexpr auto kTxProgressPoll = std::chrono::milliseconds(100);

inline uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

uint64_t read_iface_rx_packets(const std::string& ifname) {
    const std::string path =
        "/sys/class/net/" + ifname + "/statistics/rx_packets";
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return 0;
    char buf[32];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    return std::strtoull(buf, nullptr, 10);
}

std::optional<uint64_t> read_iface_tx_packets(const std::string& ifname) {
    const std::string path =
        "/sys/class/net/" + ifname + "/statistics/tx_packets";
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return std::nullopt;
    char buf[32];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    return std::strtoull(buf, nullptr, 10);
}

// §3.0 BPF pre-filter: rejects non-waybeam frames in the kernel before the
// recvmsg() copy to userspace.  Mirrors dot11_parse()'s cheapest checks:
// frame-control, SA prefix 0x56/0x42, optional net_id, payload magic 0x57/0x42.
// The variable-length radiotap header is handled by loading the LE u16 it_len
// into the X register — all 802.11 field accesses use BPF_IND (X + offset).
// dot11_parse() remains the correctness check; this is a performance filter.
void attach_bpf_filter(int fd, std::optional<uint8_t> net_id,
                       const char* ifname) {
    // Layout: PREAMBLE(6) | FC0_DISPATCH(3) | DATA_PATH | QOS_PATH | ACCEPT(1) | REJECT(1)
    const int data_path_len = net_id ? 12 : 10;
    const int qos_path_len = net_id ? 13 : 11;
    const int total = 6 + 3 + data_path_len + qos_path_len + 2;
    const int data_start = 9;
    const int qos_start = data_start + data_path_len;
    const int accept_line = total - 2;
    const int reject_line = total - 1;
    auto jmp = [](int from, int to) -> uint8_t {
        return static_cast<uint8_t>(to - from - 1);
    };

    std::vector<sock_filter> f;
    f.reserve(static_cast<size_t>(total));

    // Radiotap it_len (LE u16 at bytes 2..3) → X.
    f.push_back(BPF_STMT(BPF_LD  | BPF_B | BPF_ABS, 3));
    f.push_back(BPF_STMT(BPF_ALU | BPF_LSH | BPF_K, 8));
    f.push_back(BPF_STMT(BPF_MISC| BPF_TAX, 0));
    f.push_back(BPF_STMT(BPF_LD  | BPF_B | BPF_ABS, 2));
    f.push_back(BPF_STMT(BPF_ALU | BPF_OR | BPF_X, 0));
    f.push_back(BPF_STMT(BPF_MISC| BPF_TAX, 0));

    // FC0 dispatch: Data (0x08) or QoS-Data (0x88), reject everything else.
    int ln = 6;
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 0));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x08,
                          jmp(ln, data_start), 0));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x88,
                          jmp(ln, qos_start), jmp(ln, reject_line)));
    ++ln;

    // --- Data path: fc1 exact 0x00, SA prefix, [net_id], payload magic at +24 ---
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 1));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x00,
                          0, jmp(ln, reject_line)));
    ++ln;
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 10));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x56,
                          0, jmp(ln, reject_line)));
    ++ln;
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 11));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x42,
                          0, jmp(ln, reject_line)));
    ++ln;
    if (net_id) {
        f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 12));
        ++ln;
        f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, *net_id,
                              0, jmp(ln, reject_line)));
        ++ln;
    }
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 24));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x57,
                          0, jmp(ln, reject_line)));
    ++ln;
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 25));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x42,
                          jmp(ln, accept_line), jmp(ln, reject_line)));
    ++ln;

    // --- QoS path: fc1 masked (retry bit), SA prefix, [net_id], magic at +26 ---
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 1));
    ++ln;
    f.push_back(BPF_STMT(BPF_ALU | BPF_AND | BPF_K,
                          static_cast<uint32_t>(~kDot11Fc1RetryBit & 0xff)));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x00,
                          0, jmp(ln, reject_line)));
    ++ln;
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 10));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x56,
                          0, jmp(ln, reject_line)));
    ++ln;
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 11));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x42,
                          0, jmp(ln, reject_line)));
    ++ln;
    if (net_id) {
        f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 12));
        ++ln;
        f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, *net_id,
                              0, jmp(ln, reject_line)));
        ++ln;
    }
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 26));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x57,
                          0, jmp(ln, reject_line)));
    ++ln;
    f.push_back(BPF_STMT(BPF_LD | BPF_B | BPF_IND, 27));
    ++ln;
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x42,
                          jmp(ln, accept_line), jmp(ln, reject_line)));
    ++ln;

    f.push_back(BPF_STMT(BPF_RET | BPF_K, 0xFFFFu));
    f.push_back(BPF_STMT(BPF_RET | BPF_K, 0));

    sock_fprog prog{};
    prog.len = static_cast<unsigned short>(f.size());
    prog.filter = f.data();
    if (::setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog,
                     sizeof(prog)) != 0) {
        std::fprintf(stderr,
                     "kernel-monitor: SO_ATTACH_FILTER on %s: %s "
                     "(non-fatal, userspace filter still active)\n",
                     ifname, std::strerror(errno));
    }
}

// B2 (pre-flight audit): a forked CLI child sits on the flight loop — CSA
// retune/revert via iw_set_freq(), monitor re-bring-up via run_cli() in
// MonAir::recover(). `iw`/`ip` normally return in well under 100 ms, but a
// driver deadlock on a wedged USB adapter can hang the underlying syscall
// indefinitely; an untimed waitpid() would then wedge the whole flight process
// (and with glibc SA_RESTART a SIGTERM aimed at it would just restart the
// wait). Bound it: poll WNOHANG up to kCliWaitDeadline, then SIGKILL and reap.
// Returns the child's exit status via *status; false if the child was killed on
// the deadline or the reap failed.
constexpr auto kCliWaitPoll = std::chrono::milliseconds(5);
constexpr auto kCliWaitDeadline = std::chrono::milliseconds(2000);

bool wait_bounded(pid_t pid, int* status) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const pid_t r = ::waitpid(pid, status, WNOHANG);
        if (r == pid) return true;  // reaped normally
        if (r < 0) {
            if (errno == EINTR) continue;  // interrupted (no SA_RESTART) — retry
            return false;                  // ECHILD or other terminal error
        }
        if (std::chrono::steady_clock::now() - t0 >= kCliWaitDeadline) {
            ::kill(pid, SIGKILL);  // unblockable — the blocking reap below is bounded
            ::waitpid(pid, status, 0);
            return false;  // a timed-out retune is a failed retune
        }
        std::this_thread::sleep_for(kCliWaitPoll);  // r == 0: child still running
    }
}

// §11.5/§15.5a channel retune over a monitor netdev. The ssc338q SDK ships the
// kernel nl80211 UAPI but not libnl-3, so we drive the stable `iw` CLI (present
// on the target) rather than hand-roll genl: `iw dev <if> set freq <mhz>
// <width>` changes the wiphy channel without a down/up, so the RX threads keep
// their sockets. fork/exec is async-signal-safe here (only snprintf pre-fork,
// then execvp/_exit in the child).
bool iw_set_freq(const std::string& ifname, uint16_t mhz, uint8_t bw) {
    const char* width = (bw >= 40) ? "HT40+" : "HT20";
    char freq[8];
    std::snprintf(freq, sizeof(freq), "%u", static_cast<unsigned>(mhz));
    const char* argv[] = {"iw",  "dev",  ifname.c_str(), "set",
                          "freq", freq,  width,          nullptr};
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::execvp("iw", const_cast<char* const*>(argv));
        _exit(127);  // iw not on PATH
    }
    int status = 0;
    if (!wait_bounded(pid, &status)) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Pass 80: one forked CLI step of the monitor bring-up sequence.
bool run_cli(const char* const argv[]) {
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }
    int status = 0;
    if (!wait_bounded(pid, &status)) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

struct MonAir::Impl {
    struct RxFrame {
        uint8_t adapter = 0;
        int8_t rssi = -128;
        uint32_t tsfl = 0;
        uint8_t net_id = 0;  // §15.5a candidate/occupancy attribution
        uint32_t gen = 0;    // flush generation at recv time (Pass 69)
        std::vector<uint8_t> data;
    };
    struct Adapter {
        std::string name;
        std::string ifname;
        int fd = -1;
        bool tx = false;
        std::thread rx_thread;
        std::atomic<uint64_t> rx_frames{0};
        std::atomic<uint64_t> rx_filtered{0};
        std::atomic<uint64_t> rx_dropped{0};
        std::atomic<uint64_t> kernel_dropped{0};
        std::atomic<int> rssi_last{-128};
        uint32_t kernel_drop_last = 0;  // RX thread only
        uint32_t rng = 1;
        uint64_t iface_rx_baseline = 0;  // sysfs rx_packets at socket open
        uint32_t seen_gen = 0;           // RX thread only (Pass 69 flush)
    };

    MonAirCfg cfg;
    std::vector<std::unique_ptr<Adapter>> adapters;
    size_t tx_idx = 0;
    bool has_tx = false;

    // §15.5a runtime net_id roles. stamp is TX-only (main thread). filter is
    // read on every RX thread → atomic; -1 encodes "no filter" (hear any).
    uint8_t stamp_net_id = 0;
    std::atomic<int16_t> filter_net_id{-1};

    std::optional<uint8_t> filter_opt() const {
        const int16_t v = filter_net_id.load(std::memory_order_relaxed);
        return v < 0 ? std::nullopt
                     : std::optional<uint8_t>(static_cast<uint8_t>(v));
    }

    std::mutex mu;
    std::condition_variable cv;
    std::deque<RxFrame> queue;
    std::atomic<bool> running{true};
    int ready_fd = -1;
    // Pass 69 flush generation: bumped by flush_rx(); RX threads drain their
    // socket backlog on a bump and stamp frames with the generation loaded
    // BEFORE the recv, so poll_once can drop anything captured pre-flush.
    std::atomic<uint32_t> flush_gen{0};

    // TX state — main thread only.
    uint16_t seq = 0;
    uint8_t mcs = 0;
    bool sgi = false;
    uint8_t bw = 20;
    std::vector<uint8_t> tx_buf;
    // Written by inject()/inject_return() on the main thread, read by
    // counters()/return_counters() — atomic (relaxed) to match RadioAir and
    // stay race-free if stats are ever read off-thread.
    std::atomic<uint64_t> tx_submitted{0};
    std::atomic<uint64_t> tx_failed{0};
    std::atomic<uint64_t> unicast_fallback{0};
    std::chrono::steady_clock::time_point next_tx_progress_poll{};
    uint64_t tx_progress_cached = 0;
    bool tx_progress_available = false;

    ~Impl() {
        running.store(false, std::memory_order_relaxed);
        cv.notify_all();
        for (auto& a : adapters) {
            if (a->rx_thread.joinable()) {
                a->rx_thread.join();  // wakes within SO_RCVTIMEO
            }
            if (a->fd >= 0) {
                ::close(a->fd);
            }
        }
        if (ready_fd >= 0) ::close(ready_fd);
    }

    void rx_loop(Adapter* a, uint8_t adapter_id);
    size_t send_frame(const uint8_t* payload, size_t len, bool urgent = false);
};

void MonAir::Impl::rx_loop(Adapter* a, uint8_t adapter_id) {
    std::vector<uint8_t> buf(kRxBufLen);
    while (running.load(std::memory_order_relaxed)) {
        // Pass 69: on a flush-generation bump, drain the kernel socket
        // backlog (frames captured on the pre-retune channel) before
        // resuming. The generation is loaded BEFORE the blocking recv so a
        // frame that raced the bump carries the stale gen and is dropped at
        // poll_once.
        const uint32_t gen = flush_gen.load(std::memory_order_acquire);
        if (gen != a->seen_gen) {
            for (;;) {
                const ssize_t r =
                    ::recv(a->fd, buf.data(), buf.size(), MSG_DONTWAIT);
                if (r > 0) continue;                       // discard backlog
                if (r < 0 && errno == EINTR) continue;     // retry
                break;  // 0 or EAGAIN/other: drained
            }
            a->seen_gen = gen;
            continue;  // reload gen — flushes may stack
        }
        iovec iov{};
        iov.iov_base = buf.data();
        iov.iov_len = buf.size();
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(uint32_t))]{};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        const ssize_t n = ::recvmsg(a->fd, &msg, 0);
        if (n <= 0) {
            // EAGAIN (SO_RCVTIMEO idle) / EINTR are the normal idle path — just
            // re-check running. A hard error (a wedged or removed USB device
            // returns immediately) would otherwise spin this thread hot with no
            // backoff; B3: throttle it so a dead ear cannot burn a core.
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                errno != EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            continue;
        }
        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET ||
                cmsg->cmsg_type != SO_RXQ_OVFL ||
                cmsg->cmsg_len < CMSG_LEN(sizeof(uint32_t))) {
                continue;
            }
            uint32_t total = 0;
            std::memcpy(&total, CMSG_DATA(cmsg), sizeof(total));
            const uint32_t delta = total - a->kernel_drop_last;
            a->kernel_drop_last = total;
            a->kernel_dropped.fetch_add(delta, std::memory_order_relaxed);
        }
        const size_t len = static_cast<size_t>(n);
        const auto rt = radiotap_parse(buf.data(), len);
        const size_t fcs_len = rt && rt->fcs_at_end ? kFcsLen : 0;
        if (!rt || rt->hdr_len + fcs_len > len) {
            a->rx_filtered.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const uint8_t* mpdu = buf.data() + rt->hdr_len;
        const size_t mpdu_len = len - rt->hdr_len - fcs_len;
        const auto d = dot11_parse(mpdu, mpdu_len, filter_opt());
        if (!d || d->originator == cfg.originator) {
            a->rx_filtered.fetch_add(1, std::memory_order_relaxed);
            continue;  // not ours, or our own injected frame looped back
        }
        if (cfg.rx_drop_permille > 0 &&
            (xorshift32(a->rng) % 1000u) < cfg.rx_drop_permille) {
            a->rx_dropped.fetch_add(1, std::memory_order_relaxed);
            continue;  // bench synthetic loss
        }
        int8_t rssi = static_cast<int8_t>(a->rssi_last.load(
            std::memory_order_relaxed));
        if (rt->rssi_dbm) {
            rssi = *rt->rssi_dbm;
            a->rssi_last.store(rssi, std::memory_order_relaxed);
        }
        RxFrame f;
        f.adapter = adapter_id;
        f.rssi = rssi;
        f.tsfl = rt->tsf_us ? static_cast<uint32_t>(*rt->tsf_us) : 0u;
        f.net_id = d->net_id;
        f.gen = gen;  // Pass 69: pre-recv generation, see loop top
        f.data.assign(d->payload, d->payload + d->payload_len);
        a->rx_frames.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mu);
            if (queue.size() >= kRxQueueCap) {
                const uint8_t dropped_adapter = queue.front().adapter;
                queue.pop_front();
                adapters[dropped_adapter]->rx_dropped.fetch_add(
                    1, std::memory_order_relaxed);
            }
            queue.push_back(std::move(f));
        }
        cv.notify_one();
        const uint64_t one = 1;
        const ssize_t notified = ::write(ready_fd, &one, sizeof(one));
        (void)notified;  // EAGAIN means an unread wakeup is already pending.
    }
}

size_t MonAir::Impl::send_frame(const uint8_t* payload, size_t len,
                                bool urgent) {
    if (!has_tx) return 0;
    Adapter* a = adapters[tx_idx].get();
    const size_t hdr_len = urgent ? kDot11QosHdrLen : kDot11HdrLen;
    tx_buf.resize(kMonRadiotapHtLen + hdr_len + len);
    uint8_t* p = tx_buf.data();
    mon_radiotap_ht(p, mcs, sgi, bw);
    if (urgent) {
        static constexpr uint8_t kBroadcast[6] = {0xff, 0xff, 0xff,
                                                   0xff, 0xff, 0xff};
        dot11_hdr_qos26(p + kMonRadiotapHtLen, kBroadcast,
                        stamp_net_id, cfg.originator,
                        static_cast<uint8_t>(tx_idx), seq, kUrgentTid);
    } else {
        dot11_hdr24(p + kMonRadiotapHtLen, stamp_net_id, cfg.originator,
                    static_cast<uint8_t>(tx_idx), seq);
    }
    ++seq;
    if (len > 0) {
        std::memcpy(p + kMonRadiotapHtLen + hdr_len, payload, len);
    }
    const int priority = urgent ? kUrgentTid : 0;
    (void)::setsockopt(a->fd, SOL_SOCKET, SO_PRIORITY, &priority,
                       sizeof(priority));
    const ssize_t w = ::send(a->fd, tx_buf.data(), tx_buf.size(), 0);
    if (w < 0 || static_cast<size_t>(w) != tx_buf.size()) {
        tx_failed.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    tx_submitted.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

// ---- MonAir shell (pImpl; move-only) ---------------------------------------
MonAir::MonAir() = default;
MonAir::MonAir(MonAir&&) noexcept = default;
MonAir& MonAir::operator=(MonAir&&) noexcept = default;
MonAir::~MonAir() = default;

Result<MonAir> MonAir::create(const MonAirCfg& cfg) {
    if (cfg.adapters.empty()) {
        return Result<MonAir>::fail("kernel-monitor: no adapters configured");
    }
    auto impl = std::make_unique<Impl>();
    impl->cfg = cfg;
    impl->stamp_net_id = cfg.stamp_net_id;
    impl->filter_net_id.store(
        cfg.filter_net_id ? static_cast<int16_t>(*cfg.filter_net_id)
                          : static_cast<int16_t>(-1),
        std::memory_order_relaxed);
    impl->ready_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (impl->ready_fd < 0) {
        return Result<MonAir>::fail(std::string("kernel-monitor: eventfd: ") +
                                    std::strerror(errno));
    }

    int tx_count = 0;
    for (size_t i = 0; i < cfg.adapters.size(); ++i) {
        const AdapterCfg& ac = cfg.adapters[i];
        if (ac.ifname.empty()) {
            return Result<MonAir>::fail("kernel-monitor: adapter \"" + ac.name +
                                        "\" has no \"ifname\"");
        }
        const unsigned ifindex = ::if_nametoindex(ac.ifname.c_str());
        if (ifindex == 0) {
            return Result<MonAir>::fail(
                "kernel-monitor: ifname \"" + ac.ifname +
                "\" not found (bring up monitor mode first)");
        }
        const int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (fd < 0) {
            return Result<MonAir>::fail(
                std::string("kernel-monitor: socket(AF_PACKET): ") +
                std::strerror(errno));
        }
        int rxq_overflow = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &rxq_overflow,
                         sizeof(rxq_overflow)) != 0) {
            const std::string err = std::strerror(errno);
            ::close(fd);
            return Result<MonAir>::fail(
                "kernel-monitor: setsockopt(SO_RXQ_OVFL): " + err);
        }
        struct sockaddr_ll sll;
        std::memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = htons(ETH_P_ALL);
        sll.sll_ifindex = static_cast<int>(ifindex);
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&sll),
                   sizeof(sll)) < 0) {
            const std::string e = std::strerror(errno);
            ::close(fd);
            return Result<MonAir>::fail("kernel-monitor: bind " + ac.ifname +
                                        ": " + e);
        }
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;  // 200 ms so the RX thread can observe shutdown
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        attach_bpf_filter(fd, cfg.filter_net_id, ac.ifname.c_str());

        auto a = std::make_unique<Impl::Adapter>();
        a->name = ac.name;
        a->ifname = ac.ifname;
        a->fd = fd;
        a->tx = (ac.role == Role::kTx);
        a->iface_rx_baseline = read_iface_rx_packets(ac.ifname);
        a->rng = 0x9e3779b9u ^ (static_cast<uint32_t>(i) * 2654435761u);
        if (a->rng == 0) {
            a->rng = 1;
        }
        if (a->tx) {
            impl->tx_idx = i;
            impl->has_tx = true;
            impl->bw = ac.bw;
            ++tx_count;
        }
        impl->adapters.push_back(std::move(a));
    }
    if (tx_count > 1 || (tx_count == 0 && !cfg.allow_rx_only)) {
        return Result<MonAir>::fail(
            "kernel-monitor: need exactly one role=tx adapter (the uplink), "
            "unless this is an explicitly RX-only cache");
    }

    for (size_t i = 0; i < impl->adapters.size(); ++i) {
        Impl* ip = impl.get();
        Impl::Adapter* a = impl->adapters[i].get();
        const uint8_t id = static_cast<uint8_t>(i);
        a->rx_thread = std::thread([ip, a, id]() { ip->rx_loop(a, id); });
    }

    MonAir m;
    m.impl_ = std::move(impl);
    return Result<MonAir>::ok(std::move(m));
}

size_t MonAir::inject(const uint8_t* frame, size_t len) {
    return impl_->send_frame(frame, len);
}

size_t MonAir::inject_resend(const uint8_t* frame, size_t len) {
    return impl_->send_frame(frame, len, true);
}

size_t MonAir::inject_return(uint16_t dest_originator, const uint8_t* frame,
                             size_t len, bool urgent) {
    (void)dest_originator;  // monitor: no HW ACK responder → broadcast
    impl_->unicast_fallback.fetch_add(1, std::memory_order_relaxed);
    return impl_->send_frame(frame, len, urgent);
}

void MonAir::return_counters(uint64_t& unicast_sent,
                             uint64_t& unicast_fallback) const {
    unicast_sent = 0;
    unicast_fallback = impl_->unicast_fallback.load(std::memory_order_relaxed);
}

void MonAir::tx_progress_counters(uint64_t& submitted,
                                  uint64_t& completed) const {
    submitted = impl_->tx_submitted.load(std::memory_order_relaxed);
    if (!impl_->has_tx) {
        completed = 0;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= impl_->next_tx_progress_poll) {
        const auto v = read_iface_tx_packets(
            impl_->adapters[impl_->tx_idx]->ifname);
        impl_->tx_progress_available = v.has_value();
        if (v) impl_->tx_progress_cached = *v;
        impl_->next_tx_progress_poll = now + kTxProgressPoll;
    }
    // A missing sysfs surface must fail open: mirror submissions so the
    // absence-only watchdog cannot manufacture a wedge verdict.
    completed = impl_->tx_progress_available ? impl_->tx_progress_cached
                                             : submitted;
}

int MonAir::poll_once(int timeout_ms, const RxCb& cb) {
    std::deque<Impl::RxFrame> local;
    uint64_t ready = 0;
    while (::read(impl_->ready_fd, &ready, sizeof(ready)) > 0) {}
    {
        std::unique_lock<std::mutex> lk(impl_->mu);
        if (impl_->queue.empty() && timeout_ms > 0) {
            impl_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                               [this]() { return !impl_->queue.empty(); });
        }
        local.swap(impl_->queue);
    }
    int delivered = 0;
    const uint32_t cur_gen =
        impl_->flush_gen.load(std::memory_order_acquire);
    for (const auto& f : local) {
        if (f.gen != cur_gen) {
            continue;  // Pass 69: captured before the last flush_rx()
        }
        AirRxMeta meta;
        meta.adapter_id = f.adapter;
        meta.rssi = f.rssi;
        meta.tsf_us = f.tsfl;
        meta.net_id = f.net_id;
        cb(meta, f.data.data(), f.data.size());
        ++delivered;
    }
    return delivered;
}

int MonAir::wait_fd() const { return impl_->ready_fd; }

size_t MonAir::rx_adapters() const { return impl_->adapters.size(); }
bool MonAir::has_tx() const { return impl_->has_tx; }
size_t MonAir::tx_index() const { return impl_->tx_idx; }  // §15.5a scout (Pass 64)

std::optional<uint32_t> MonAir::estimate_airtime_us(
    size_t bytes, bool include_pending) const {
    if (!impl_->has_tx) return std::nullopt;
    if (impl_->cfg.airtime_efficiency_permille == 0) return std::nullopt;
    uint64_t total = bytes;
    if (include_pending) {
        int pending = 0;
        const int fd = impl_->adapters[impl_->tx_idx]->fd;
        if (::ioctl(fd, SIOCOUTQ, &pending) == 0 && pending > 0) {
            total += static_cast<uint32_t>(pending);
        }
    }
    // Input bytes are Waybeam wire packets. Account for one 802.11 header +
    // FCS per standard-rung-sized MPDU; service efficiency owns preamble,
    // contention, driver aggregation, and other measured transport effects.
    const uint64_t packets =
        (total + kDefaultMaxPayload - 1u) / kDefaultMaxPayload;
    total += packets * (kDot11HdrLen + kFcsLen);
    return ht20_service_time_us(
        static_cast<size_t>(std::min<uint64_t>(total, SIZE_MAX)), impl_->mcs,
        impl_->sgi, impl_->cfg.airtime_efficiency_permille);
}

void MonAir::set_tx_mode(uint8_t mcs, bool sgi) {
    impl_->mcs = mcs;
    impl_->sgi = sgi;
}

void MonAir::set_stamp_net_id(uint8_t net_id) {
    impl_->stamp_net_id = net_id;  // TX main-thread only
}

void MonAir::set_filter_net_id(std::optional<uint8_t> net_id) {
    impl_->filter_net_id.store(
        net_id ? static_cast<int16_t>(*net_id) : static_cast<int16_t>(-1),
        std::memory_order_relaxed);
    // Re-attach the §3.0 kernel pre-filter to match: nullopt keeps the
    // waybeam-shape filter but drops the net_id equality test (hears all
    // net_ids), a value re-adds it. SO_ATTACH_FILTER replaces atomically, so
    // the RX threads never see a filter-less window.
    for (auto& a : impl_->adapters) {
        attach_bpf_filter(a->fd, net_id, a->ifname.c_str());
    }
}

int MonAir::set_power_qdb(size_t adapter, int32_t qdb) {
    // §10.5 (Pass 114): the kernel-monitor actuator is nl80211 fixed power —
    // `iw set txpower fixed <mBm>`, 1 qdb = 25 mBm. Bounded CLI like every
    // other monitor control write; profile/override cadence only.
    if (adapter >= impl_->adapters.size()) return 0;
    const std::string& ifname = impl_->adapters[adapter]->ifname;
    char mbm[16];
    std::snprintf(mbm, sizeof(mbm), "%d", static_cast<int>(qdb) * 25);
    const char* argv[] = {"iw",  "dev",     ifname.c_str(), "set",
                          "txpower", "fixed", mbm,          nullptr};
    if (!run_cli(argv)) {
        std::fprintf(stderr,
                     "kernel-monitor: iw set txpower fixed %s mBm on %s "
                     "failed\n",
                     mbm, ifname.c_str());
        return 0;
    }
    std::fprintf(stderr, "kernel-monitor: %s txpower fixed %d qdb (%s mBm)\n",
                 ifname.c_str(), qdb, mbm);
    return qdb;
}

bool MonAir::set_power_auto(size_t adapter) {
    // §10.5 auto restore: hand power back to the driver default (the same
    // `txpower auto` mon-up.sh / recover() issue — Pass 48: a fixed value
    // left behind can read healthy while the per-rate TXAGC curve is bypassed).
    if (adapter >= impl_->adapters.size()) return false;
    const std::string& ifname = impl_->adapters[adapter]->ifname;
    const char* argv[] = {"iw",      "dev",  ifname.c_str(), "set",
                          "txpower", "auto", nullptr};
    const bool ok = run_cli(argv);
    std::fprintf(stderr, "kernel-monitor: %s txpower auto %s\n",
                 ifname.c_str(), ok ? "ok" : "FAILED");
    return ok;
}

std::optional<uint64_t> MonAir::read_tsf(size_t adapter) {
    (void)adapter;
    return std::nullopt;  // host-time fallback (§7.2)
}

bool MonAir::retune(size_t adapter, uint16_t chan_mhz, uint8_t bw, bool fast) {
    (void)fast;  // §11.5a fast-path is a devourer optimization; iw is one-shot
    if (adapter >= impl_->adapters.size()) return false;
    const std::string& ifname = impl_->adapters[adapter]->ifname;
    if (!iw_set_freq(ifname, chan_mhz, bw)) {
        std::fprintf(stderr, "kernel-monitor: iw set freq %u (%u MHz bw) on %s "
                             "failed\n",
                     chan_mhz, bw, ifname.c_str());
        return false;
    }
    return true;
}

bool MonAir::recover(size_t adapter, uint16_t chan_mhz, uint8_t bw) {
    // §11.6 Pass 80: the RTL88x2 in-place retune can half-apply (TX airs on
    // the new channel, RX deaf). Full bring-up sequence — mirrors mon-up.sh;
    // AF_PACKET RX sockets are ifindex-bound and survive the down/up.
    if (adapter >= impl_->adapters.size()) return false;
    const std::string& ifname = impl_->adapters[adapter]->ifname;
    const char* ifc = ifname.c_str();
    const char* down[] = {"ip", "link", "set", ifc, "down", nullptr};
    // `set monitor otherbss` is what admits foreign-BSS frames on Realtek —
    // without it the RX can come back "up" and still deliver no waybeam DATA.
    // mon-up.sh falls back to `set type monitor` on drivers that reject it.
    const char* mon[] = {"iw", "dev", ifc, "set", "monitor", "otherbss",
                         nullptr};
    const char* mon_fb[] = {"iw", "dev", ifc, "set", "type", "monitor",
                            nullptr};
    const char* up[] = {"ip", "link", "set", ifc, "up", nullptr};
    const char* mtu[] = {"ip", "link", "set", ifc, "mtu", "4052", nullptr};
    // Pass 48: a bare `ip link up` on the EU/CU leaves txpower at -100 dBm and
    // the radio packet-silent. mon-up.sh hands power back to the driver's
    // per-rate TXAGC curve; recovery must do the same or it restores a mute
    // radio that reads as healthy.
    const char* txp[] = {"iw", "dev", ifc, "set", "txpower", "auto", nullptr};
    bool ok = run_cli(down);
    if (!run_cli(mon)) {
        ok = run_cli(mon_fb) && ok;
    }
    ok = run_cli(up) && ok;
    run_cli(mtu);  // best-effort, as in mon-up.sh
    ok = iw_set_freq(ifname, chan_mhz, bw) && ok;
    run_cli(txp);  // best-effort, as in mon-up.sh
    std::fprintf(stderr,
                 "kernel-monitor: RX-liveness recovery on %s -> %u MHz %s\n",
                 ifc, chan_mhz, ok ? "ok" : "FAILED");
    return ok;
}

bool MonAir::reapply_tx_power(size_t adapter) {
    (void)adapter;
    return true;
}

void MonAir::flush_rx() {
    // Pass 69 §11.6 verify hygiene. Bump first so any frame the RX threads
    // are mid-enqueueing carries a stale generation, then clear the process
    // queue; each RX thread drains its own kernel socket on noticing the
    // bump (rx_loop top).
    impl_->flush_gen.fetch_add(1, std::memory_order_release);
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->queue.clear();
}

MonAir::AdapterCounters MonAir::counters(size_t adapter) const {
    AdapterCounters c;
    if (adapter >= impl_->adapters.size()) {
        return c;
    }
    const Impl::Adapter* a = impl_->adapters[adapter].get();
    c.name = a->name;
    c.tx = a->tx;
    c.rx_frames = a->rx_frames.load(std::memory_order_relaxed);
    c.rx_filtered = a->rx_filtered.load(std::memory_order_relaxed);
    c.rx_dropped = a->rx_dropped.load(std::memory_order_relaxed);
    c.kernel_dropped = a->kernel_dropped.load(std::memory_order_relaxed);
    const uint64_t iface_rx = read_iface_rx_packets(a->ifname);
    const uint64_t userspace =
        c.rx_frames + c.rx_filtered + c.rx_dropped + c.kernel_dropped;
    const uint64_t iface_delta =
        iface_rx >= a->iface_rx_baseline ? iface_rx - a->iface_rx_baseline : 0;
    c.bpf_filtered = iface_delta > userspace ? iface_delta - userspace : 0;
    c.rssi_last =
        static_cast<int8_t>(a->rssi_last.load(std::memory_order_relaxed));
    if (a->tx) {
        c.tx_submitted = impl_->tx_submitted.load(std::memory_order_relaxed);
        c.tx_failed = impl_->tx_failed.load(std::memory_order_relaxed);
    }
    return c;
}

}  // namespace wblink
