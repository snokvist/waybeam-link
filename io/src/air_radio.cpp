// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/air_radio.h"

#include <libusb.h>

#include <stdio.h>  // fopencookie (glibc/musl extension)

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "IRtlDevice.h"
#include "RxPacket.h"
#include "SelectedChannel.h"
#include "TxMode.h"
#include "UsbOpen.h"
#include "WiFiDriver.h"
#include "logger.h"
#include "wblink/dot11.h"

namespace wblink {

namespace {

constexpr uint16_t kRealtekVid = 0x0bda;
constexpr size_t kRxQueueCap = 512;

// AdapterCfg.channel_mhz (center frequency) → 802.11 channel number.
uint8_t mhz_to_channel(uint16_t mhz) {
    if (mhz == 2484) {
        return 14;
    }
    if (mhz >= 2412 && mhz < 2484) {
        return static_cast<uint8_t>((mhz - 2407) / 5);
    }
    if (mhz >= 5000) {
        return static_cast<uint8_t>((mhz - 5000) / 5);
    }
    return 0;
}

// "bus-port[.port...]" (lsusb -t style), e.g. "3-1.2".
std::string usb_path_of(libusb_device* dev) {
    uint8_t ports[7];
    const int n = libusb_get_port_numbers(dev, ports, sizeof(ports));
    std::string s = std::to_string(libusb_get_bus_number(dev));
    s += '-';
    for (int i = 0; i < n; ++i) {
        if (i) {
            s += '.';
        }
        s += std::to_string(ports[i]);
    }
    return s;
}

struct RxFrame {
    uint8_t adapter;
    int8_t rssi;
    uint32_t tsfl;
    std::vector<uint8_t> data;  // §3.0 payload (802.11 header stripped)
};

}  // namespace

struct RadioAir::Impl {
    struct Adapter {
        std::string name;
        std::string path;
        bool tx = false;
        libusb_context* ctx = nullptr;
        libusb_device_handle* handle = nullptr;
        std::shared_ptr<devourer::UsbDeviceLock> lock;
        std::unique_ptr<IRtlDevice> dev;
        std::thread rx_thread;
        // RX-thread-owned counters (relaxed atomics; read from stats).
        std::atomic<uint64_t> rx_frames{0};
        std::atomic<uint64_t> rx_filtered{0};
        std::atomic<uint64_t> rx_dropped{0};
        std::atomic<int8_t> rssi_last{-128};
        uint64_t tx_submitted = 0;  // main-thread only
        uint64_t tx_failed = 0;
        // Bench-only synthetic-drop PRNG (RX-thread only; xorshift32).
        uint32_t drop_rng = 0;
    };

    RadioAirCfg cfg;
    Logger_t logger;
    std::vector<std::unique_ptr<Adapter>> adapters;
    size_t tx_idx = 0;
    uint16_t seq = 0;
    std::vector<uint8_t> tx_buf;

    // devourer's machine-event sink (Logger::events()) defaults to stdout,
    // which would interleave with our stats stream. A cookie stream both
    // silences it and harvests the tx.report lines (per-frame CCX TX status,
    // Pass 8). Written from RX threads via the shared FILE* (stdio-locked);
    // counters are relaxed atomics.
    FILE* ev_stream = nullptr;
    std::atomic<uint64_t> tx_reports{0};
    std::atomic<uint64_t> tx_report_fails{0};

    static bool ev_contains(const char* buf, size_t n, const char* pat,
                            size_t m) {
        return std::search(buf, buf + n, pat, pat + m) != buf + n;
    }
    static ssize_t ev_write(void* cookie, const char* buf, size_t n) {
        auto* im = static_cast<Impl*>(cookie);
        // EveryLine flush policy delivers one complete event line per write.
        if (ev_contains(buf, n, "\"tx.report\"", 11)) {
            im->tx_reports.fetch_add(1, std::memory_order_relaxed);
            if (ev_contains(buf, n, "\"ok\":false", 10)) {
                im->tx_report_fails.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return static_cast<ssize_t>(n);  // always consume (drop non-reports)
    }

    std::mutex mu;
    std::condition_variable cv;
    std::deque<RxFrame> queue;

    // §3.0 Pass 12: last-heard SA per originator (RX threads write, the
    // main-thread inject_return reads). Tiny linear table — the fleet has
    // a handful of originators at most.
    struct SaEntry {
        uint16_t orig;
        uint8_t sa[6];
    };
    std::mutex sa_mu;
    std::vector<SaEntry> sa_latch;
    // Main-thread unicast-return counters (§15.3).
    uint64_t ret_unicast_sent = 0;
    uint64_t ret_unicast_fallback = 0;

    void latch_sa(const Dot11Rx& d) {
        SaEntry e;
        e.orig = d.originator;
        e.sa[0] = kWbSaPrefix0;
        e.sa[1] = kWbSaPrefix1;
        e.sa[2] = d.net_id;
        e.sa[3] = static_cast<uint8_t>(d.originator >> 8);
        e.sa[4] = static_cast<uint8_t>(d.originator & 0xff);
        e.sa[5] = d.adapter_idx;
        std::lock_guard<std::mutex> lk(sa_mu);
        for (SaEntry& s : sa_latch) {
            if (s.orig == e.orig) {
                std::memcpy(s.sa, e.sa, 6);
                return;
            }
        }
        sa_latch.push_back(e);
    }

    bool lookup_sa(uint16_t orig, uint8_t out[6]) {
        std::lock_guard<std::mutex> lk(sa_mu);
        for (const SaEntry& s : sa_latch) {
            if (s.orig == orig) {
                std::memcpy(out, s.sa, 6);
                return true;
            }
        }
        return false;
    }

    ~Impl() {
        // Stop loops first, then join, then power the chips down and release
        // USB — the ordering devourer's demos use. A join can block while a
        // bring-up is still in flight (bring-up does not poll the stop flag).
        for (auto& a : adapters) {
            if (a->dev) {
                a->dev->StopRxLoop();
            }
        }
        for (auto& a : adapters) {
            if (a->rx_thread.joinable()) {
                a->rx_thread.join();
            }
            if (a->dev) {
                a->dev->Stop();
            }
            if (a->handle) {
                libusb_release_interface(a->handle, 0);
                libusb_close(a->handle);
            }
            a->lock.reset();
            if (a->ctx) {
                libusb_exit(a->ctx);
            }
        }
        // Only after every writer (RX threads, device Stop paths) is gone.
        if (ev_stream != nullptr) {
            if (logger) {
                logger->events().disable();
            }
            std::fclose(ev_stream);
        }
    }

    // RX-loop callback: §3.0 filter on the RX thread, accepted frames cross
    // into the caller's world through the bounded queue.
    void on_packet(Adapter& a, uint8_t adapter_id, const Packet& p) {
        if (p.RxAtrib.pkt_rpt_type != RX_PACKET_TYPE::NORMAL_RX ||
            p.RxAtrib.crc_err || p.RxAtrib.icv_err) {
            return;
        }
        // §3.0: monitor RX delivers the MPDU with the chip-validated 4-byte
        // FCS still appended — strip it before the length-exact parse.
        if (p.Data.size() <= kFcsLen) {
            return;
        }
        const auto d = dot11_parse(p.Data.data(), p.Data.size() - kFcsLen,
                                   cfg.filter_net_id);
        if (!d || d->originator == cfg.originator) {  // not ours / our own TX
            a.rx_filtered.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Bench-only synthetic RX loss, independent per adapter: a dropped
        // frame vanishes exactly like real air loss (before every counter).
        if (cfg.rx_drop_permille != 0) {
            uint32_t r = a.drop_rng;
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            a.drop_rng = r;
            if (r % 1000 < cfg.rx_drop_permille) {
                return;
            }
        }
        // Per-chain power byte, dBm = value − 110; 0 = no phy report on
        // this frame → keep the previous value.
        uint8_t best = 0;
        for (uint8_t chain : p.RxAtrib.rssi) {
            if (chain > best) {
                best = chain;
            }
        }
        int8_t rssi = a.rssi_last.load(std::memory_order_relaxed);
        if (best != 0) {
            const int dbm = static_cast<int>(best) - 110;
            rssi = static_cast<int8_t>(dbm < -128 ? -128
                                       : dbm > 0  ? 0
                                                  : dbm);
            a.rssi_last.store(rssi, std::memory_order_relaxed);
        }
        a.rx_frames.fetch_add(1, std::memory_order_relaxed);
        // Pass 12: remember the sender's exact SA (adapter-idx byte and
        // all) so unicast returns match its armed ACK-responder MACID.
        if (cfg.unicast_returns) {
            latch_sa(*d);
        }
        RxFrame f;
        f.adapter = adapter_id;
        f.rssi = rssi;
        f.tsfl = p.RxAtrib.tsfl;
        f.data.assign(d->payload, d->payload + d->payload_len);
        {
            std::lock_guard<std::mutex> lk(mu);
            if (queue.size() >= kRxQueueCap) {
                queue.pop_front();
                a.rx_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            queue.push_back(std::move(f));
        }
        cv.notify_one();
    }
};

RadioAir::RadioAir() : impl_(new Impl) {}
RadioAir::RadioAir(RadioAir&&) noexcept = default;
RadioAir& RadioAir::operator=(RadioAir&&) noexcept = default;
RadioAir::~RadioAir() = default;

Result<RadioAir> RadioAir::create(const RadioAirCfg& cfg) {
    if (cfg.adapters.empty()) {
        return Result<RadioAir>::fail("radio: no adapters configured");
    }
    size_t n_tx = 0;
    for (const auto& a : cfg.adapters) {
        n_tx += (a.role == Role::kTx) ? 1 : 0;
    }
    if (n_tx != 1) {
        return Result<RadioAir>::fail(
            "radio: exactly one adapter must have role \"tx\" (the "
            "designated uplink / craft radio), got " +
            std::to_string(n_tx));
    }

    RadioAir air;
    Impl& im = *air.impl_;
    im.cfg = cfg;
    im.logger = std::make_shared<Logger>();
    im.logger->set_level(Logger::Level::Info);
    // Route the machine-event sink into the tx.report harvester (also keeps
    // event lines out of our stdout stats stream). No cookie stream → just
    // silence the sink; tx.report counters then stay 0.
    {
        cookie_io_functions_t io{};
        io.write = &Impl::ev_write;
        im.ev_stream = fopencookie(&im, "w", io);
        if (im.ev_stream != nullptr) {
            im.logger->events().configure(im.ev_stream);
        } else {
            im.logger->events().disable();
        }
    }

    std::vector<std::string> used_paths;
    for (size_t i = 0; i < cfg.adapters.size(); ++i) {
        const AdapterCfg& ac = cfg.adapters[i];
        auto ad = std::make_unique<Impl::Adapter>();
        ad->name = ac.name.empty() ? ("adapter" + std::to_string(i))
                                   : ac.name;
        ad->tx = (ac.role == Role::kTx);
        // Nonzero, adapter-distinct xorshift32 seed for the bench drop knob.
        ad->drop_rng = 0x9E3779B9u ^ static_cast<uint32_t>(i + 1);

        // Per-adapter libusb_context (the proven multi-adapter pattern).
        if (libusb_init(&ad->ctx) != 0) {
            return Result<RadioAir>::fail("radio: libusb_init failed");
        }
        libusb_device** list = nullptr;
        const ssize_t n = libusb_get_device_list(ad->ctx, &list);
        libusb_device* found = nullptr;
        std::string found_path;
        for (ssize_t k = 0; k < n; ++k) {
            libusb_device_descriptor dd;
            if (libusb_get_device_descriptor(list[k], &dd) != 0 ||
                dd.idVendor != kRealtekVid) {
                continue;
            }
            const std::string path = usb_path_of(list[k]);
            bool taken = false;
            for (const auto& u : used_paths) {
                taken = taken || (u == path);
            }
            if (taken || (!ac.bus.empty() && ac.bus != path)) {
                continue;
            }
            found = list[k];
            found_path = path;
            break;
        }
        int open_rc = found ? libusb_open(found, &ad->handle)
                            : LIBUSB_ERROR_NO_DEVICE;
        libusb_free_device_list(list, 1);
        if (open_rc != 0) {
            return Result<RadioAir>::fail(
                "radio: adapter \"" + ad->name + "\" (bus \"" + ac.bus +
                "\"): no matching Realtek device / open failed");
        }
        used_paths.push_back(found_path);
        ad->path = found_path;

        const int rc = devourer::claim_interface_then_reset(
            ad->handle, 0, im.logger, /*do_reset=*/true, ad->lock);
        if (rc != 0) {
            return Result<RadioAir>::fail("radio: adapter \"" + ad->name +
                                          "\" claim/reset failed (in use?)");
        }
        devourer::DeviceConfig dc{};
        // Jaguar3 needs the RX path armed during the InitWrite bring-up so
        // StartRxLoop works on the same claimed handle (gate-1 pattern);
        // ignored on Jaguar1.
        dc.rx.enable_with_tx = true;
        // Per-frame TX-status CCX reports on the injecting adapter only
        // (Pass 8: TX-wedge detector; SPE_RPT is per-descriptor, so RX-only
        // adapters keep byte-identical bring-up).
        dc.tx.report = ad->tx;
        WiFiDriver wd(im.logger);
        ad->dev = wd.CreateRtlDevice(ad->handle, ad->ctx, ad->lock, dc);
        if (!ad->dev) {
            return Result<RadioAir>::fail("radio: adapter \"" + ad->name +
                                          "\": unsupported chip");
        }
        if (ad->tx) {
            im.tx_idx = i;
        }
        im.adapters.push_back(std::move(ad));
    }

    // Bring-up serialized on this (the control) thread, then one RX loop
    // thread per adapter over the already-up chip.
    for (size_t i = 0; i < im.adapters.size(); ++i) {
        Impl::Adapter& ad = *im.adapters[i];
        const uint8_t chan = mhz_to_channel(cfg.adapters[i].channel_mhz);
        if (chan == 0) {
            return Result<RadioAir>::fail(
                "radio: adapter \"" + ad.name + "\": bad channel_mhz " +
                std::to_string(cfg.adapters[i].channel_mhz));
        }
        ad.dev->InitWrite(SelectedChannel{chan, 0, CHANNEL_WIDTH_20});
        Impl* imp = air.impl_.get();
        Impl::Adapter* adp = &ad;
        const uint8_t id = static_cast<uint8_t>(i);
        ad.rx_thread = std::thread([imp, adp, id]() {
            try {
                adp->dev->StartRxLoop([imp, adp, id](const Packet& p) {
                    imp->on_packet(*adp, id, p);
                });
            } catch (const std::exception& e) {
                std::fprintf(stderr, "radio: rx loop \"%s\" died: %s\n",
                             adp->name.c_str(), e.what());
            }
        });
    }
    // §3.0 Pass 12 (craft half): arm the TX adapter's hardware ACK
    // responder with its own SA — the exact addr1 the ground's unicast
    // returns will carry. Opt-in: this makes a passive monitor transmit.
    if (cfg.ack_responder) {
        Impl::Adapter& tx = *im.adapters[im.tx_idx];
        devourer::MacAddr mac;
        mac.bytes = {kWbSaPrefix0,
                     kWbSaPrefix1,
                     cfg.stamp_net_id,
                     static_cast<uint8_t>(cfg.originator >> 8),
                     static_cast<uint8_t>(cfg.originator & 0xff),
                     static_cast<uint8_t>(im.tx_idx)};
        if (tx.dev->SetAckResponder(mac)) {
            std::fprintf(stderr,
                         "radio: ack responder armed on \"%s\" "
                         "(56:42:%02x:%02x:%02x:%02x)\n",
                         tx.name.c_str(), mac.bytes[2], mac.bytes[3],
                         mac.bytes[4], mac.bytes[5]);
        } else {
            std::fprintf(stderr,
                         "radio: ack responder unsupported on \"%s\"\n",
                         tx.name.c_str());
        }
    }
    return Result<RadioAir>::ok(std::move(air));
}

size_t RadioAir::inject(const uint8_t* frame, size_t len) {
    Impl& im = *impl_;
    Impl::Adapter& tx = *im.adapters[im.tx_idx];
    im.tx_buf.resize(kDot11TxPrefixLen + len);
    dot11_tx_prefix(im.tx_buf.data(), im.cfg.stamp_net_id, im.cfg.originator,
                    static_cast<uint8_t>(im.tx_idx), im.seq++);
    std::memcpy(im.tx_buf.data() + kDot11TxPrefixLen, frame, len);
    ++tx.tx_submitted;
    if (tx.dev->send_packet(im.tx_buf.data(), im.tx_buf.size())) {
        return 1;
    }
    ++tx.tx_failed;
    return 0;
}

size_t RadioAir::inject_return(uint16_t dest_originator, const uint8_t* frame,
                               size_t len) {
    Impl& im = *impl_;
    uint8_t sa[6];
    if (!im.cfg.unicast_returns) {
        return inject(frame, len);
    }
    if (!im.lookup_sa(dest_originator, sa)) {
        ++im.ret_unicast_fallback;  // no SA heard yet — broadcast (§3.0)
        return inject(frame, len);
    }
    Impl::Adapter& tx = *im.adapters[im.tx_idx];
    im.tx_buf.resize(kDot11TxUnicastPrefixLen + len);
    dot11_tx_prefix_unicast(im.tx_buf.data(), sa, im.cfg.stamp_net_id,
                            im.cfg.originator,
                            static_cast<uint8_t>(im.tx_idx), im.seq++);
    std::memcpy(im.tx_buf.data() + kDot11TxUnicastPrefixLen, frame, len);
    ++tx.tx_submitted;
    ++im.ret_unicast_sent;
    if (tx.dev->send_packet(im.tx_buf.data(), im.tx_buf.size())) {
        return 1;
    }
    ++tx.tx_failed;
    return 0;
}

void RadioAir::return_counters(uint64_t& unicast_sent,
                               uint64_t& unicast_fallback) const {
    unicast_sent = impl_->ret_unicast_sent;
    unicast_fallback = impl_->ret_unicast_fallback;
}

int RadioAir::poll_once(int timeout_ms, const RxCb& cb) {
    Impl& im = *impl_;
    std::deque<RxFrame> local;
    {
        std::unique_lock<std::mutex> lk(im.mu);
        if (im.queue.empty() && timeout_ms > 0) {
            im.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return !im.queue.empty(); });
        }
        local.swap(im.queue);
    }
    for (const RxFrame& f : local) {
        AirRxMeta meta;
        meta.adapter_id = f.adapter;
        meta.rssi = f.rssi;
        meta.tsf_us = f.tsfl;
        cb(meta, f.data.data(), f.data.size());
    }
    return static_cast<int>(local.size());
}

size_t RadioAir::rx_adapters() const { return impl_->adapters.size(); }

void RadioAir::set_tx_mode(uint8_t mcs, bool sgi) {
    devourer::TxMode m;
    m.mode = devourer::TxMode::Mode::HT;
    m.ht_mcs = mcs;
    m.bw_mhz = 20;
    m.sgi = sgi;
    impl_->adapters[impl_->tx_idx]->dev->SetTxMode(m);
}

int RadioAir::set_power_qdb(size_t adapter, int32_t qdb) {
    if (adapter >= impl_->adapters.size()) {
        return 0;
    }
    return impl_->adapters[adapter]->dev->SetTxPowerOffsetQdb(
        static_cast<int>(qdb));
}

bool RadioAir::retune(size_t adapter, uint16_t chan_mhz, uint8_t bw,
                      bool fast) {
    if (adapter >= impl_->adapters.size()) {
        return false;
    }
    const uint8_t chan = mhz_to_channel(chan_mhz);
    if (chan == 0) {
        return false;
    }
    IRtlDevice& dev = *impl_->adapters[adapter]->dev;
    if (fast && bw == 0) {
        // §11.2 class 0: same-width hop, ~0.5–2.5 ms. FastRetune skips the
        // TXAGC re-apply, so the caller follows up with reapply_tx_power().
        dev.FastRetune(chan);
    } else {
        SelectedChannel c{};
        c.Channel = chan;
        c.ChannelOffset = 0;
        c.ChannelWidth = bw == 2   ? CHANNEL_WIDTH_80
                         : bw == 1 ? CHANNEL_WIDTH_40
                                   : CHANNEL_WIDTH_20;
        dev.SetMonitorChannel(c);
    }
    return true;
}

bool RadioAir::reapply_tx_power(size_t adapter) {
    if (adapter >= impl_->adapters.size()) {
        return false;
    }
    return impl_->adapters[adapter]->dev->ReApplyTxPower();
}

std::optional<uint64_t> RadioAir::read_tsf(size_t adapter) {
    if (adapter >= impl_->adapters.size()) {
        return std::nullopt;
    }
    try {
        const uint64_t t = impl_->adapters[adapter]->dev->ReadTsf();
        if (t == 0) {
            return std::nullopt;  // unsupported
        }
        return t;
    } catch (const std::exception&) {
        return std::nullopt;  // control transfer raced the RX bulk load
    }
}

void RadioAir::tx_report_counters(uint64_t& submitted,
                                  uint64_t& reports) const {
    submitted = impl_->adapters[impl_->tx_idx]->tx_submitted;
    reports = impl_->tx_reports.load(std::memory_order_relaxed);
}

RadioAir::AdapterCounters RadioAir::counters(size_t adapter) const {
    AdapterCounters c;
    if (adapter >= impl_->adapters.size()) {
        return c;
    }
    const Impl::Adapter& a = *impl_->adapters[adapter];
    c.name = a.name;
    c.tx = a.tx;
    c.rx_frames = a.rx_frames.load(std::memory_order_relaxed);
    c.rx_filtered = a.rx_filtered.load(std::memory_order_relaxed);
    c.rx_dropped = a.rx_dropped.load(std::memory_order_relaxed);
    c.rssi_last = a.rssi_last.load(std::memory_order_relaxed);
    c.tx_submitted = a.tx_submitted;
    c.tx_failed = a.tx_failed;
    if (a.tx) {  // reports only exist for the injecting adapter's frames
        c.tx_reports = impl_->tx_reports.load(std::memory_order_relaxed);
        c.tx_report_fails =
            impl_->tx_report_fails.load(std::memory_order_relaxed);
    }
    return c;
}

}  // namespace wblink
