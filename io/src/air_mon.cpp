// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/air_mon.h"

#include <arpa/inet.h>        // htons
#include <linux/if_ether.h>   // ETH_P_ALL
#include <linux/if_packet.h>  // sockaddr_ll
#include <net/if.h>           // if_nametoindex
#include <sys/socket.h>
#include <sys/time.h>  // struct timeval (SO_RCVTIMEO)
#include <unistd.h>    // close

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "wblink/dot11.h"
#include "wblink/radiotap.h"

namespace wblink {

namespace {
constexpr size_t kRxQueueCap = 512;
constexpr size_t kRxBufLen = 4096;

inline uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
}  // namespace

struct MonAir::Impl {
    struct RxFrame {
        uint8_t adapter = 0;
        int8_t rssi = -128;
        uint32_t tsfl = 0;
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
        std::atomic<int> rssi_last{-128};
        uint32_t rng = 1;
    };

    MonAirCfg cfg;
    std::vector<std::unique_ptr<Adapter>> adapters;
    size_t tx_idx = 0;

    std::mutex mu;
    std::condition_variable cv;
    std::deque<RxFrame> queue;
    std::atomic<bool> running{true};

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
    }

    void rx_loop(Adapter* a, uint8_t adapter_id);
    size_t send_frame(const uint8_t* payload, size_t len);
};

void MonAir::Impl::rx_loop(Adapter* a, uint8_t adapter_id) {
    std::vector<uint8_t> buf(kRxBufLen);
    while (running.load(std::memory_order_relaxed)) {
        const ssize_t n = ::recv(a->fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            continue;  // SO_RCVTIMEO / EINTR → re-check running
        }
        const size_t len = static_cast<size_t>(n);
        const auto rt = radiotap_parse(buf.data(), len);
        if (!rt || rt->hdr_len + kFcsLen > len) {
            a->rx_filtered.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const uint8_t* mpdu = buf.data() + rt->hdr_len;
        const size_t mpdu_len = len - rt->hdr_len - kFcsLen;  // strip FCS
        const auto d = dot11_parse(mpdu, mpdu_len, cfg.filter_net_id);
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
        f.data.assign(d->payload, d->payload + d->payload_len);
        a->rx_frames.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mu);
            if (queue.size() >= kRxQueueCap) {
                queue.pop_front();
                a->rx_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            queue.push_back(std::move(f));
        }
        cv.notify_one();
    }
}

size_t MonAir::Impl::send_frame(const uint8_t* payload, size_t len) {
    Adapter* a = adapters[tx_idx].get();
    tx_buf.resize(kMonRadiotapHtLen + kDot11HdrLen + len);
    uint8_t* p = tx_buf.data();
    mon_radiotap_ht(p, mcs, sgi, bw);
    dot11_hdr24(p + kMonRadiotapHtLen, cfg.stamp_net_id, cfg.originator,
                static_cast<uint8_t>(tx_idx), seq);
    ++seq;
    if (len > 0) {
        std::memcpy(p + kMonRadiotapHtLen + kDot11HdrLen, payload, len);
    }
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

        auto a = std::make_unique<Impl::Adapter>();
        a->name = ac.name;
        a->ifname = ac.ifname;
        a->fd = fd;
        a->tx = (ac.role == Role::kTx);
        a->rng = 0x9e3779b9u ^ (static_cast<uint32_t>(i) * 2654435761u);
        if (a->rng == 0) {
            a->rng = 1;
        }
        if (a->tx) {
            impl->tx_idx = i;
            impl->bw = ac.bw;
            ++tx_count;
        }
        impl->adapters.push_back(std::move(a));
    }
    if (tx_count != 1) {
        return Result<MonAir>::fail(
            "kernel-monitor: need exactly one role=tx adapter (the uplink)");
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

size_t MonAir::inject_return(uint16_t dest_originator, const uint8_t* frame,
                             size_t len) {
    (void)dest_originator;  // monitor: no HW ACK responder → broadcast
    impl_->unicast_fallback.fetch_add(1, std::memory_order_relaxed);
    return impl_->send_frame(frame, len);
}

void MonAir::return_counters(uint64_t& unicast_sent,
                             uint64_t& unicast_fallback) const {
    unicast_sent = 0;
    unicast_fallback = impl_->unicast_fallback.load(std::memory_order_relaxed);
}

int MonAir::poll_once(int timeout_ms, const RxCb& cb) {
    std::deque<Impl::RxFrame> local;
    {
        std::unique_lock<std::mutex> lk(impl_->mu);
        if (impl_->queue.empty() && timeout_ms > 0) {
            impl_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                               [this]() { return !impl_->queue.empty(); });
        }
        local.swap(impl_->queue);
    }
    int delivered = 0;
    for (const auto& f : local) {
        AirRxMeta meta;
        meta.adapter_id = f.adapter;
        meta.rssi = f.rssi;
        meta.tsf_us = f.tsfl;
        cb(meta, f.data.data(), f.data.size());
        ++delivered;
    }
    return delivered;
}

size_t MonAir::rx_adapters() const { return impl_->adapters.size(); }

void MonAir::set_tx_mode(uint8_t mcs, bool sgi) {
    impl_->mcs = mcs;
    impl_->sgi = sgi;
}

int MonAir::set_power_qdb(size_t adapter, int32_t qdb) {
    (void)adapter;
    // The 8812eu per-rate TXAGC curve owns power (rtw_tx_pwr_by_rate=1); the
    // monitor bring-up sets nl80211 txpower once. Logged intent only.
    return qdb;
}

std::optional<uint64_t> MonAir::read_tsf(size_t adapter) {
    (void)adapter;
    return std::nullopt;  // host-time fallback (§7.2)
}

bool MonAir::retune(size_t adapter, uint16_t chan_mhz, uint8_t bw, bool fast) {
    (void)adapter;
    (void)bw;
    (void)fast;
    std::fprintf(stderr,
                 "kernel-monitor: retune -> %u MHz requested (CSA over monitor "
                 "deferred; channel fixed at bring-up)\n",
                 chan_mhz);
    return true;
}

bool MonAir::reapply_tx_power(size_t adapter) {
    (void)adapter;
    return true;
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
    c.rssi_last =
        static_cast<int8_t>(a->rssi_last.load(std::memory_order_relaxed));
    if (a->tx) {
        c.tx_submitted = impl_->tx_submitted.load(std::memory_order_relaxed);
        c.tx_failed = impl_->tx_failed.load(std::memory_order_relaxed);
    }
    return c;
}

}  // namespace wblink
