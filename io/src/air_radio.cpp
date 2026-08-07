// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/air_radio.h"

#include <libusb.h>

#include <stdio.h>  // fopencookie (glibc/musl extension)
#include <sys/eventfd.h>
#include <unistd.h>

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
#include "RxQuality.h"  // §15.3 Pass 158: the vendored fold conventions
#include "SelectedChannel.h"
#include "TxMode.h"
#include "UsbOpen.h"
#include "WiFiDriver.h"
#include "logger.h"
#include "wblink/airtime.h"
#include "wblink/dot11.h"
#include "wblink/radio_decode.h"

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
    uint8_t net_id;             // §3.0 SA tag of the sender (§15.5a scout)
    uint8_t rx_mcs;             // §15.3 Pass 118, kRxMcsUnknown if unresolved
    uint32_t tsfl;
    uint32_t gen;               // flush generation at RX time (Pass 69)
    std::vector<uint8_t> data;  // §3.0 payload (802.11 header stripped)
};

}  // namespace

struct RadioAir::Impl {
    struct Adapter {
        std::string name;
        std::string path;
        // §10.6 (Pass 154) per-unit EFUSE MAC ("aa:bb:cc:dd:ee:ff", lowercase),
        // read once after InitWrite (the 8822E caches it during hal init and
        // its OTP is not reliably readable later; the 8822C walks on demand).
        // Empty = the unit reports no identity — callers fail closed.
        std::string mac;
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
        std::atomic<bool> rx_dead{false};  // §15.3 Pass 101: RX loop exited
        // §15.3 Pass 118 per-MCS accepted-frame histogram + unresolved bucket.
        std::atomic<uint64_t> rx_mcs[kRxMcsBuckets] = {};
        std::atomic<uint64_t> rx_mcs_unknown{0};
        // §15.3 Pass 157 received-coding counters; ldpc_flag_ok is the die's
        // static ldpc_rx_flag cap (set at bring-up, read-only after).
        std::atomic<uint64_t> rx_ldpc{0};
        std::atomic<uint64_t> rx_stbc{0};
        bool ldpc_flag_ok = false;
        // §15.3 Pass 158 quality window (internally locked; RX thread
        // add()s, the stats emitter drains via rx_quality_window()).
        devourer::RxQualityAccumulator quality;
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
    // §15.5a runtime net_id roles. The stamp is TX-side and main-thread only
    // (the inject* paths read cfg.stamp_net_id directly). The filter is read
    // by every RX thread in on_packet, so it lives here as an atomic rather
    // than in cfg; -1 encodes "no filter" (hear any net_id), matching MonAir.
    // cfg.filter_net_id is therefore the BOOT value only — seeded into this
    // atomic in create() and never read again. Do not surface it as the live
    // filter.
    std::atomic<int16_t> filter_net_id{-1};

    std::optional<uint8_t> filter_opt() const {
        const int16_t v = filter_net_id.load(std::memory_order_relaxed);
        return v < 0 ? std::nullopt
                     : std::optional<uint8_t>(static_cast<uint8_t>(v));
    }
    // §3.0 Pass 118: the committed operating point, stamped into every
    // frame's radiotap. set_tx_mode keeps SetTxMode in lockstep with it.
    TxRate rate;
    std::vector<uint8_t> tx_buf;

    // devourer's machine-event sink (Logger::events()) defaults to stdout,
    // which would interleave with our stats stream. A cookie stream both
    // silences it and harvests the tx.report lines (per-frame CCX TX status,
    // Pass 8). Written from RX threads via the shared FILE* (stdio-locked);
    // counters are relaxed atomics.
    FILE* ev_stream = nullptr;
    std::atomic<uint64_t> tx_reports{0};
    std::atomic<uint64_t> tx_report_fails{0};

    // devourer's human diagnostics default to stderr, one Info line PER
    // TRANSMITTED PACKET ("bulk_send EP n OK n bytes"). Measured on the craft
    // at ~250 pps: 3.3 MB per 30 s, which fills its 45 MB /tmp in about seven
    // minutes — after which the log silently stops recording and every other
    // user of /tmp on the vehicle is out of space. A ground node running the
    // radio backend feeds the same rate into journald, whose rate limiter
    // drops lines instead. Both end the same way: blind at the moment a fault
    // needs diagnosing (Pass 147 lost two hours of craft log to exactly this).
    //
    // Filtering the sink rather than lowering the level, because
    // set_level(Warn) would also drop the bring-up block (endpoint selection,
    // efuse decode, chip cut) that devourer work actually reads, and the
    // logger documents its fields as "configure before worker threads spawn;
    // intentionally unsynchronized" (logger.h:24) — so lowering the level
    // after bring-up, once the RX threads are running, is a documented race.
    // A cookie stream installed before any thread starts is contract-clean.
    FILE* diag_stream = nullptr;

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
    // Human-diagnostics sink: forward everything except the per-packet
    // success line (see diag_stream). Same EveryLine contract as ev_write —
    // one complete line per write.
    //
    // Matched POSITIVELY on the flood's exact shape, not on the substring
    // "bulk_send". UsbTransport.cpp logs two lines with that substring —
    // `info("bulk_send EP {} OK {} bytes")` (the flood) and
    // `error("bulk_send EP {} FAIL rc={} got {}/{}")` — and the FAIL line is
    // the one diagnostic that matters while TX is breaking. A substring
    // filter would swallow it precisely during a §9.10 wedge. Anything this
    // does not recognise is kept, so an unfamiliar line is never dropped.
    static bool is_bulk_send_ok(const char* buf, size_t n) {
        return ev_contains(buf, n, "bulk_send EP", 12) &&
               ev_contains(buf, n, " OK ", 4);
    }
    static ssize_t diag_write(void*, const char* buf, size_t n) {
        if (!is_bulk_send_ok(buf, n)) {
            std::fwrite(buf, 1, n, stderr);
            std::fflush(stderr);
        }
        return static_cast<ssize_t>(n);
    }

    std::mutex mu;
    std::condition_variable cv;
    std::deque<RxFrame> queue;
    int ready_fd = -1;
    // Pass 69 §11.6 verify hygiene: bumped by flush_rx(); frames are stamped
    // at callback entry so anything captured before a flush is droppable at
    // poll_once. devourer's internal USB pipeline (~ms deep) is below this
    // boundary, like driver-internal buffers on the kernel-monitor backend.
    std::atomic<uint32_t> flush_gen{0};

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

    // One RX loop thread per adapter (the proven N=3 pattern). Used at
    // bring-up and again by recover() — the two must stay identical, which is
    // the whole reason this is a function.
    void start_rx_thread(Adapter& a, uint8_t id) {
        Impl* imp = this;
        Adapter* adp = &a;
        a.rx_thread = std::thread([imp, adp, id]() {
            try {
                adp->dev->StartRxLoop([imp, adp, id](const Packet& p) {
                    imp->on_packet(*adp, id, p);
                });
            } catch (const std::exception& e) {
                // §15.3 Pass 101: a definitive death (vs the §6.5 stall
                // heuristic a quiet channel also trips). Set before the log so
                // a stats read racing the exit still sees the ear as dead.
                adp->rx_dead.store(true, std::memory_order_relaxed);
                std::fprintf(stderr, "radio: rx loop \"%s\" died: %s\n",
                             adp->name.c_str(), e.what());
            }
        });
    }

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
        // Null entries exist transiently during the Pass 154 re-bind (the
        // permutation moves adapters through a cleared vector), and devourer
        // register I/O can throw mid-re-bind (USB glitch) — this destructor
        // must survive running at that point.
        for (auto& a : adapters) {
            if (a && a->dev) {
                a->dev->StopRxLoop();
            }
        }
        for (auto& a : adapters) {
            if (!a) continue;
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
        if (diag_stream != nullptr) {
            // Put stderr back before closing, so any later devourer line (or
            // a destructor's own diagnostics) still lands somewhere.
            if (logger) {
                logger->set_diag_stream(stderr);
            }
            std::fclose(diag_stream);
        }
        if (ready_fd >= 0) ::close(ready_fd);
    }

    // RX-loop callback: §3.0 filter on the RX thread, accepted frames cross
    // into the caller's world through the bounded queue.
    void on_packet(Adapter& a, uint8_t adapter_id, const Packet& p) {
        if (p.RxAtrib.pkt_rpt_type != RX_PACKET_TYPE::NORMAL_RX ||
            p.RxAtrib.crc_err || p.RxAtrib.icv_err) {
            return;
        }
        const auto mpdu_len = mpdu_len_without_fcs(p.Data.size());
        if (!mpdu_len) {
            return;
        }
        const auto d = dot11_parse(p.Data.data(), *mpdu_len, filter_opt());
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
                a.rx_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        const int8_t prev = a.rssi_last.load(std::memory_order_relaxed);
        const int8_t rssi = rssi_dbm_from_chains(
            p.RxAtrib.rssi, sizeof(p.RxAtrib.rssi) / sizeof(p.RxAtrib.rssi[0]),
            prev);
        if (rssi != prev) {
            a.rssi_last.store(rssi, std::memory_order_relaxed);
        }
        a.rx_frames.fetch_add(1, std::memory_order_relaxed);
        // Pass 12: remember the sender's exact SA (adapter-idx byte and
        // all) so unicast returns match its armed ACK-responder MACID.
        if (cfg.unicast_returns) {
            latch_sa(*d);
        }
        // §15.3 Pass 158: fold path-A raw quality (the accumulator skips
        // rssi_raw<=0 itself and folds SNR/EVM only when present). Same
        // acceptance point as rx_mcs — post-filter, post-synthetic-drop —
        // so the window describes the frames the link actually delivered.
        a.quality.add(p.RxAtrib.rssi[0], p.RxAtrib.snr[0], p.RxAtrib.evm[0]);
        // §15.3 Pass 157: received coding as the die reported it. On
        // ldpc_flag_ok=false dies the bits are never set, so the counters
        // read 0 there — the flag, exported with them, says so.
        if (p.RxAtrib.ldpc) a.rx_ldpc.fetch_add(1, std::memory_order_relaxed);
        if (p.RxAtrib.stbc) a.rx_stbc.fetch_add(1, std::memory_order_relaxed);
        const uint8_t rx_mcs = desc_rate_to_mcs(p.RxAtrib.data_rate);
        if (rx_mcs < kRxMcsBuckets) {
            a.rx_mcs[rx_mcs].fetch_add(1, std::memory_order_relaxed);
        } else {
            a.rx_mcs_unknown.fetch_add(1, std::memory_order_relaxed);
        }
        RxFrame f;
        f.adapter = adapter_id;
        f.rssi = rssi;
        // §15.5a: which net_id this was heard on. Only interesting while the
        // filter is wide (a sweep), but it is the sender's tag either way.
        f.net_id = d->net_id;
        f.rx_mcs = rx_mcs;
        f.tsfl = p.RxAtrib.tsfl;
        f.gen = flush_gen.load(std::memory_order_acquire);  // Pass 69
        f.data.assign(d->payload, d->payload + d->payload_len);
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
    im.ready_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (im.ready_fd < 0) {
        return Result<RadioAir>::fail(std::string("radio: eventfd: ") +
                                      std::strerror(errno));
    }
    im.cfg = cfg;
    im.filter_net_id.store(cfg.filter_net_id
                               ? static_cast<int16_t>(*cfg.filter_net_id)
                               : static_cast<int16_t>(-1),
                           std::memory_order_relaxed);
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
    // Bound the human-diagnostics volume before any worker thread spawns.
    {
        cookie_io_functions_t io{};
        io.write = &Impl::diag_write;
        im.diag_stream = fopencookie(&im, "w", io);
        if (im.diag_stream != nullptr) {
            std::setvbuf(im.diag_stream, nullptr, _IOLBF, 0);
            im.logger->set_diag_stream(im.diag_stream);
            // Never a silent drop: say once that the per-packet line is gone,
            // so a reader who expects it knows why it is missing.
            std::fprintf(stderr,
                         "radio: devourer per-packet bulk_send OK lines "
                         "suppressed (log volume); failures and all other "
                         "devourer diagnostics unchanged\n");
        }
    }

    // §15.2 (Pass 154) stanza matching, precedence mac > bus > first-free.
    // The 8822E EFUSE is only reliably readable during InitWrite, so a MAC
    // pin cannot drive the CLAIM: stanzas claim provisionally (bus-pinned
    // exact, else first free Realtek device), every unit is brought up and
    // its identity read, then stanzas RE-BIND to their matching unit before
    // any RX thread starts or a frame is injected. With no mac pins the
    // provisional binding is final and the claim behaves as before.
    bool any_mac_pin = false;
    for (const AdapterCfg& a : cfg.adapters) {
        any_mac_pin = any_mac_pin || !a.mac.empty();
    }
    // Paths strict-pinned by a mac-less stanza are reserved: the claim is
    // sequential, so a non-strict claim (first-free, or a mac stanza's
    // pass-2 fallback) consuming such a device would fail bring-up on a
    // config that has a valid assignment — the strict owner claims later
    // and finds its port taken.
    std::vector<std::string> strict_pins;
    for (const AdapterCfg& a : cfg.adapters) {
        if (!a.bus.empty() && a.mac.empty()) {
            strict_pins.push_back(a.bus);
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
        // A bus pin on a stanza WITHOUT a mac stays strict (an explicit port
        // pin, §15.2). With a mac it degrades to a claim preference: the
        // dongle may have moved, and the post-bring-up re-bind (or its D2
        // fallback) is what decides, so pass 2 takes any free device.
        const int passes = (!ac.bus.empty() && !ac.mac.empty()) ? 2 : 1;
        for (int pass = 0; pass < passes && found == nullptr; ++pass) {
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
                if (taken ||
                    (pass == 0 && !ac.bus.empty() && ac.bus != path)) {
                    continue;
                }
                const bool strict_attempt = pass == 0 && !ac.bus.empty();
                if (!strict_attempt && path != ac.bus) {
                    bool reserved = false;
                    for (const auto& sp : strict_pins) {
                        reserved = reserved || (sp == path);
                    }
                    if (reserved) {
                        continue;
                    }
                }
                found = list[k];
                found_path = path;
                break;
            }
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
        // adapters keep byte-identical bring-up). With a §15.2 mac pin the
        // TX stanza's unit is unknown until after bring-up, so every unit is
        // created report-capable — the flag only gates TX-descriptor
        // stamping (RtlJaguar*Device TX path), and a diversity ear never
        // injects, so it stays inert there.
        dc.tx.report = any_mac_pin ? 1 : ad->tx;
        // §3.0 (Pass 156): unicast hardware-retry limit. Consumed
        // per-TX-descriptor (like tx.report), so it is set uniformly — an
        // RX-only ear never injects and broadcast frames carry no ACK
        // policy, so it binds exactly on the hybrid's unicast returns.
        dc.tx.retry_limit = cfg.tx_retry_limit;
        // MAC carrier-sense gate. Applied to every adapter, not just the TX
        // one: an RX-only ear that defers has nothing to defer, but the
        // bring-up posture stays uniform across the node.
        dc.tuning.disable_cca = cfg.disable_cca;
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
    // thread per adapter over the already-up chip. RX threads start only
    // after the Pass 154 re-bind below — an adapter id crossing the queue
    // must be the FINAL stanza index.
    for (size_t i = 0; i < im.adapters.size(); ++i) {
        Impl::Adapter& ad = *im.adapters[i];
        const uint8_t chan = mhz_to_channel(cfg.adapters[i].channel_mhz);
        if (chan == 0) {
            return Result<RadioAir>::fail(
                "radio: adapter \"" + ad.name + "\": bad channel_mhz " +
                std::to_string(cfg.adapters[i].channel_mhz));
        }
        ad.dev->InitWrite(SelectedChannel{chan, 0, CHANNEL_WIDTH_20});
        // §15.3 (Pass 157): whether this die reports received coding at
        // all — static per die, so read once here.
        ad.ldpc_flag_ok = ad.dev->GetAdapterCaps().ldpc_rx_flag;
        // §10.6 (Pass 154): the per-unit identity, read while it is reliably
        // readable (the accessor folds USB glitches into false — a unit with
        // no answer has no identity, and callers fail closed on that).
        uint8_t mac[6];
        if (ad.dev->GetPermanentMacAddress(mac)) {
            char buf[18];
            std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x",
                          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            ad.mac = buf;
        }
    }

    // §15.2 (Pass 154) re-bind: mac pins first (they outrank a bus claim),
    // then bus pins among what remains, then first-free leftovers in claim
    // order. Skipped entirely with no mac pins — provisional == final.
    if (any_mac_pin) {
        const size_t n = im.adapters.size();
        std::vector<std::unique_ptr<Impl::Adapter>> devs =
            std::move(im.adapters);
        im.adapters.clear();
        im.adapters.resize(n);
        std::vector<bool> taken(n, false);
        for (size_t i = 0; i < n; ++i) {
            const AdapterCfg& ac = cfg.adapters[i];
            if (ac.mac.empty()) continue;
            for (size_t d = 0; d < n; ++d) {
                if (!taken[d] && !devs[d]->mac.empty() &&
                    devs[d]->mac == ac.mac) {
                    im.adapters[i] = std::move(devs[d]);
                    taken[d] = true;
                    break;
                }
            }
        }
        for (size_t i = 0; i < n; ++i) {
            const AdapterCfg& ac = cfg.adapters[i];
            if (im.adapters[i] || ac.bus.empty()) continue;
            for (size_t d = 0; d < n; ++d) {
                if (!taken[d] && devs[d]->path == ac.bus) {
                    im.adapters[i] = std::move(devs[d]);
                    taken[d] = true;
                    break;
                }
            }
        }
        for (size_t i = 0; i < n; ++i) {
            if (im.adapters[i]) continue;
            for (size_t d = 0; d < n; ++d) {
                if (!taken[d]) {
                    im.adapters[i] = std::move(devs[d]);
                    taken[d] = true;
                    break;
                }
            }
            // A bus pin is explicit (§15.2); when a mac match outranked it
            // and took the unit at that port, the displacement must not be
            // silent — this stanza is now on a different physical unit.
            const AdapterCfg& ac = cfg.adapters[i];
            if (ac.mac.empty() && !ac.bus.empty() &&
                im.adapters[i]->path != ac.bus) {
                std::fprintf(stderr,
                             "radio: adapter \"%s\": bus pin %s DISPLACED by "
                             "a mac match — bound to path %s (mac %s) "
                             "instead (§15.2 precedence)\n",
                             ac.name.c_str(), ac.bus.c_str(),
                             im.adapters[i]->path.c_str(),
                             im.adapters[i]->mac.empty()
                                 ? "none"
                                 : im.adapters[i]->mac.c_str());
            }
        }
        // Re-stamp the stanza-owned fields onto the re-bound units, surface
        // every D2 fallback loudly, and retune any unit whose provisional
        // bring-up channel is not its final stanza's.
        for (size_t i = 0; i < n; ++i) {
            Impl::Adapter& ad = *im.adapters[i];
            const AdapterCfg& ac = cfg.adapters[i];
            ad.name = ac.name.empty() ? ("adapter" + std::to_string(i))
                                      : ac.name;
            ad.tx = (ac.role == Role::kTx);
            ad.drop_rng = 0x9E3779B9u ^ static_cast<uint32_t>(i + 1);
            if (ad.tx) {
                im.tx_idx = i;
            }
            if (!ac.mac.empty() && ad.mac != ac.mac) {
                // §10.6 D2 (Pass 154): fail closed, stay flyable. The §10.5
                // safe boot offset is applied by the caller as on any boot;
                // what is withheld is every identity-bound absolute — the
                // artifact for the configured unit cannot match this one.
                std::fprintf(
                    stderr,
                    "radio: adapter \"%s\": configured mac %s NOT PRESENT — "
                    "bound to path %s (mac %s) at the safe boot offset; "
                    "identity-bound calibration is withheld (§10.6 D2)\n",
                    ad.name.c_str(), ac.mac.c_str(), ad.path.c_str(),
                    ad.mac.empty() ? "none" : ad.mac.c_str());
            }
            const uint8_t chan = mhz_to_channel(ac.channel_mhz);
            if (ad.dev->GetSelectedChannel().Channel != chan) {
                ad.dev->SetMonitorChannel(
                    SelectedChannel{chan, 0, CHANNEL_WIDTH_20});
            }
        }
    }

    // §3.0 (Pass 156) capability leg: on dies keeping the vendor
    // DATA_RETRY_LIMIT carve-out (caps.tx_retry_limit_ok=false — 8814A;
    // 8821C false-as-unmeasured) the descriptor field is silently skipped,
    // so a validated nonzero limit still never retransmits. Checked on the
    // resolved TX unit, after the §15.2 re-bind — the unit, not the stanza.
    // ack_responder is deliberately not gated here: SetAckResponder refuses
    // loudly at arm time below; a skipped retry field has no signal at all.
    if (cfg.unicast_returns) {
        Impl::Adapter& tx = *im.adapters[im.tx_idx];
        if (!tx.dev->GetAdapterCaps().tx_retry_limit_ok) {
            return Result<RadioAir>::fail(
                "radio: adapter \"" + tx.name +
                "\": this die cannot honor air.tx_retry_limit "
                "(caps.tx_retry_limit_ok=false) — return.unicast would run "
                "silently inert (§3.0 Pass 156)");
        }
    }
    // §3.0 (Pass 157) same leg for the coding knobs: an enabled coding the
    // TX die cannot emit must refuse, not silently air BCC/no-STBC.
    if (cfg.ldpc || cfg.stbc) {
        Impl::Adapter& tx = *im.adapters[im.tx_idx];
        const devourer::TxCaps tc = tx.dev->GetAdapterCaps().tx;
        if (cfg.ldpc && !tc.ldpc_ok) {
            return Result<RadioAir>::fail(
                "radio: adapter \"" + tx.name +
                "\": this die cannot emit LDPC (caps.tx.ldpc_ok=false) — "
                "air.ldpc would run silently inert (§3.0 Pass 157)");
        }
        if (cfg.stbc && !tc.stbc_ok) {
            return Result<RadioAir>::fail(
                "radio: adapter \"" + tx.name +
                "\": this die cannot emit STBC (caps.tx.stbc_ok=false, "
                "1T1R) — air.stbc would run silently inert (§3.0 Pass 157)");
        }
    }
    // Node coding into the committed TxRate — unconditionally, so the
    // coupling to config never rides on the two defaults happening to
    // agree. §9.5 set_tx_mode rewrites mcs/sgi and leaves these standing.
    im.rate.ldpc = cfg.ldpc;
    im.rate.stbc = cfg.stbc ? 1 : 0;

    for (size_t i = 0; i < im.adapters.size(); ++i) {
        Impl::Adapter& ad = *im.adapters[i];
        // The declared adapter set (§15.5 /info mirrors this): name, port,
        // per-unit identity, role. "mac none" is the D3 posture announcing
        // itself — that unit can never hold an absolute curve.
        std::fprintf(stderr, "radio: adapter \"%s\" up (path %s, mac %s%s)\n",
                     ad.name.c_str(), ad.path.c_str(),
                     ad.mac.empty() ? "none" : ad.mac.c_str(),
                     ad.tx ? ", tx" : "");
        im.start_rx_thread(ad, static_cast<uint8_t>(i));
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
    dot11_tx_prefix(im.tx_buf.data(), im.rate, im.cfg.stamp_net_id,
                    im.cfg.originator, static_cast<uint8_t>(im.tx_idx),
                    im.seq++);
    std::memcpy(im.tx_buf.data() + kDot11TxPrefixLen, frame, len);
    ++tx.tx_submitted;
    if (tx.dev->send_packet(im.tx_buf.data(), im.tx_buf.size())) {
        return 1;
    }
    ++tx.tx_failed;
    return 0;
}

size_t RadioAir::inject_resend(const uint8_t* frame, size_t len) {
    Impl& im = *impl_;
    Impl::Adapter& tx = *im.adapters[im.tx_idx];
    im.tx_buf.resize(kDot11TxUrgentPrefixLen + len);
    dot11_tx_prefix_urgent(im.tx_buf.data(), im.rate, im.cfg.stamp_net_id,
                           im.cfg.originator, static_cast<uint8_t>(im.tx_idx),
                           im.seq++);
    std::memcpy(im.tx_buf.data() + kDot11TxUrgentPrefixLen, frame, len);
    ++tx.tx_submitted;
    if (tx.dev->send_packet(im.tx_buf.data(), im.tx_buf.size())) return 1;
    ++tx.tx_failed;
    return 0;
}

size_t RadioAir::inject_return(uint16_t dest_originator, const uint8_t* frame,
                               size_t len, bool urgent) {
    Impl& im = *impl_;
    uint8_t sa[6];
    if (!im.cfg.unicast_returns) {
        return urgent ? inject_resend(frame, len) : inject(frame, len);
    }
    if (!im.lookup_sa(dest_originator, sa)) {
        ++im.ret_unicast_fallback;  // no SA heard yet — broadcast (§3.0)
        return urgent ? inject_resend(frame, len) : inject(frame, len);
    }
    Impl::Adapter& tx = *im.adapters[im.tx_idx];
    im.tx_buf.resize(kDot11TxUnicastPrefixLen + len);
    dot11_tx_prefix_unicast(im.tx_buf.data(), im.rate, sa,
                            im.cfg.stamp_net_id, im.cfg.originator,
                            static_cast<uint8_t>(im.tx_idx), im.seq++,
                            urgent ? kUrgentTid : 0);
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
    uint64_t ready = 0;
    while (::read(im.ready_fd, &ready, sizeof(ready)) > 0) {}
    {
        std::unique_lock<std::mutex> lk(im.mu);
        if (im.queue.empty() && timeout_ms > 0) {
            im.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return !im.queue.empty(); });
        }
        local.swap(im.queue);
    }
    int delivered = 0;
    const uint32_t cur_gen = im.flush_gen.load(std::memory_order_acquire);
    for (const RxFrame& f : local) {
        if (f.gen != cur_gen) {
            continue;  // Pass 69: captured before the last flush_rx()
        }
        AirRxMeta meta;
        meta.adapter_id = f.adapter;
        meta.rssi = f.rssi;
        meta.net_id = f.net_id;
        meta.rx_mcs = f.rx_mcs;
        meta.tsf_us = f.tsfl;
        cb(meta, f.data.data(), f.data.size());
        ++delivered;
    }
    return delivered;
}

void RadioAir::flush_rx() {
    // Pass 69 §11.6 verify hygiene — see Impl::flush_gen. Bump first so a
    // frame mid-enqueue carries a stale generation, then clear the queue.
    impl_->flush_gen.fetch_add(1, std::memory_order_release);
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->queue.clear();
}

int RadioAir::wait_fd() const { return impl_->ready_fd; }

size_t RadioAir::rx_adapters() const { return impl_->adapters.size(); }

void RadioAir::set_tx_mode(uint8_t mcs, bool sgi) {
    // §3.0 Pass 118: radiotap is authoritative — this is what every injected
    // frame now carries.
    impl_->rate.mcs = mcs;
    impl_->rate.sgi = sgi;
    impl_->rate.bw = 20;  // §1 craft constraint: HT20 only in v0
    // ...and SetTxMode stays committed in lockstep as the fallback-only
    // default. Devourer consults it solely for frames whose radiotap carries
    // no rate, so it never fires on a healthy path; what it buys is that a
    // malformed prefix degrades to the committed operating point instead of
    // silently airing the link at the driver's legacy 6M default.
    devourer::TxMode m;
    m.mode = devourer::TxMode::Mode::HT;
    m.ht_mcs = mcs;
    m.bw_mhz = 20;
    m.sgi = sgi;
    // §3.0 Pass 157: the fallback carries the node coding too, so the
    // malformed-prefix degradation keeps "every frame airs the same coding".
    m.ldpc = impl_->rate.ldpc;
    m.stbc = impl_->rate.stbc != 0;
    impl_->adapters[impl_->tx_idx]->dev->SetTxMode(m);
}

bool RadioAir::set_power_qdb(size_t adapter, int32_t qdb) {
    if (adapter >= impl_->adapters.size()) {
        return false;
    }
    // devourer returns the qdb it applied; no caller has ever read it, and
    // §10.5's bool means "the backend accepted the write", which an
    // in-process offset always does.
    (void)impl_->adapters[adapter]->dev->SetTxPowerOffsetQdb(
        static_cast<int>(qdb));
    return true;
}

// §10.5 (Pass 150): devourer's native lever already IS the relative one —
// SetTxPowerOffsetQdb moves the whole efuse per-rate curve and preserves its
// shape. So the relative contract is the identity here, and set_power_qdb's
// long-standing "absolute" name was the lie the spec inherited.
bool RadioAir::set_power_offset_qdb(size_t adapter, int32_t qdb) {
    return set_power_qdb(adapter, qdb);
}

bool RadioAir::retune(size_t adapter, uint16_t chan_mhz, uint8_t width_mhz,
                      bool fast) {
    if (adapter >= impl_->adapters.size()) {
        return false;
    }
    // Dual-encoding tolerance moved here verbatim from the two caller-side
    // `bw > 2 ? bw_code(bw) : bw` expressions: >2 is an MHz width, <=2 is an
    // already-encoded §11.1 class. Preserved, not endorsed (see the header).
    const uint8_t bw = width_mhz > 2 ? (width_mhz >= 80   ? 2
                                        : width_mhz >= 40 ? 1
                                                          : 0)
                                     : width_mhz;
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
    // G3 (Pass 143): both actuators are `void`, so this used to answer true
    // unconditionally — a devourer node could report COMMITTED with nothing
    // applied. GetSelectedChannel() returns the driver's own record of where
    // it put the radio, so a mismatch means the call was refused or landed
    // somewhere else (an unsupported channel, a generation whose FastRetune
    // no-ops, a width the chip declined). It is bookkeeping, not RF: it cannot
    // see a half-applied retune where the chip disagrees with the driver.
    // That case is the §11.6 RX-liveness guard's, which is backend-agnostic
    // and now has a recover() under it. This is exactly the confidence
    // kernel-monitor gets from `iw` reporting success.
    const SelectedChannel got = dev.GetSelectedChannel();
    if (got.Channel != chan) {
        std::fprintf(stderr,
                     "radio: retune \"%s\" to ch %u not applied "
                     "(driver reports ch %u)\n",
                     impl_->adapters[adapter]->name.c_str(), chan,
                     static_cast<unsigned>(got.Channel));
        return false;
    }
    return true;
}

bool RadioAir::recover(size_t adapter, uint16_t chan_mhz, uint8_t width_mhz) {
    // G4 (Pass 143): the §11.6 Pass 80 RX-liveness guard fires when a retune
    // half-applies — TX airs on the new channel, RX hears nothing. On
    // kernel-monitor the answer is a full netdev bring-up; here it is a
    // re-init of the RX pipeline: stop the loop, join it, re-run the write-side
    // bring-up at the target channel, restart the loop.
    //
    // Two things bound what this can be, both measured (Pass 143):
    //
    //   InitWrite is ONE-SHOT. It unconditionally assigns `_coex_thread`
    //   (jaguar3 RtlJaguar3Device.cpp:207), so calling it a second time
    //   destroys a joinable thread and std::terminate()s the process — which
    //   is exactly what the first cut of this function did on the bench. The
    //   restartable surface devourer documents is StartRxLoop (IRtlDevice.h:58)
    //   plus SetMonitorChannel, and that is what this uses. So this is an
    //   RX-path restart, not the full MAC/PHY bring-up kernel-monitor gets from
    //   an `ip link down/up`; it is weaker on purpose rather than by omission.
    //
    //   It is not a USB-level reset either. `CLAUDE.md` records that an RTL88x2
    //   USB wedge (RX counter frozen) needs a physical re-plug, and whether any
    //   in-process re-init clears that is unmeasured.
    //
    // Counters are preserved across the restart on purpose: rx_frames is the
    // §11.6 liveness baseline, and zeroing it would forge liveness.
    Impl& im = *impl_;
    if (adapter >= im.adapters.size()) return false;
    Impl::Adapter& a = *im.adapters[adapter];
    if (!a.dev) return false;
    const uint8_t chan = mhz_to_channel(chan_mhz);
    if (chan == 0) return false;
    const uint8_t bw = width_mhz > 2 ? (width_mhz >= 80   ? 2
                                        : width_mhz >= 40 ? 1
                                                          : 0)
                                     : width_mhz;
    a.dev->StopRxLoop();
    if (a.rx_thread.joinable()) {
        a.rx_thread.join();
    }
    SelectedChannel c{};
    c.Channel = chan;
    c.ChannelOffset = 0;
    c.ChannelWidth = bw == 2   ? CHANNEL_WIDTH_80
                     : bw == 1 ? CHANNEL_WIDTH_40
                               : CHANNEL_WIDTH_20;
    a.dev->SetMonitorChannel(c);
    const SelectedChannel got = a.dev->GetSelectedChannel();
    // Clear the Pass 101 death latch only once the bring-up has answered:
    // a stats read racing this must not see a healthy ear that is not back.
    a.rx_dead.store(false, std::memory_order_relaxed);
    im.start_rx_thread(a, static_cast<uint8_t>(adapter));
    const bool ok = got.Channel == chan;
    std::fprintf(stderr, "radio: RX-liveness recovery on \"%s\" -> %u MHz %s\n",
                 a.name.c_str(), chan_mhz, ok ? "ok" : "FAILED");
    return ok;
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

// --- declared limits (docs/devourer-parity-plan.md) ----------------------
// These were `if (mon)` branches in AirBackend with no radio arm. Behaviour is
// preserved exactly; what changes is that the answer is now stated per backend
// instead of being the absence of a branch at 98 call sites.

bool RadioAir::set_power_auto(size_t adapter) {
    // §10.5: "auto" here is a one-shot offset 0 — it undoes the latch and lets
    // the §10.2 curve resolve re-apply on top. Deliberately not the same as the
    // kernel driver's `txpower auto`, and the spec says so per backend
    // (PROTOCOL.md §10.5); this is not an undeclared asymmetry.
    return set_power_qdb(adapter, 0);
}

void RadioAir::set_stamp_net_id(uint8_t net_id) {
    // TX-side identity, read only by the main-thread inject* paths. Every
    // caller today is the ground's §15.5a scout / §11 CSA selection in run_rx.
    // NOTE the Pass-12 ACK responder (opt-in) latches its SA — net_id byte
    // included — at bring-up and is never re-armed, so a node that both arms it
    // and retargets its stamp ends up matching a stale MACID. Unguarded, but
    // inert: only a ground retargets (every caller is in run_rx) and nothing
    // unicasts *to* a ground — §3.0 scopes unicast returns to ground→craft.
    // A craft that ever retargets would have to re-arm.
    impl_->cfg.stamp_net_id = net_id;
}

void RadioAir::set_filter_net_id(std::optional<uint8_t> net_id) {
    // §15.5a: widen (nullopt) or re-pin the §3.0 RX filter mid-sweep. The
    // filter is software-only in this backend — on_packet reads the atomic per
    // frame — so unlike kernel-monitor there is no pre-filter to re-attach.
    impl_->filter_net_id.store(
        net_id ? static_cast<int16_t>(*net_id) : static_cast<int16_t>(-1),
        std::memory_order_relaxed);
}

size_t RadioAir::tx_index() const {
    return impl_->tx_idx;  // create() resolves it from role:"tx"
}

bool RadioAir::has_tx() const {
    return true;  // create() requires exactly one role:"tx" adapter
}

uint16_t RadioAir::mtu_supported() const {
    // G5 (Pass 143): kernel-monitor reads each netdev MTU because the kernel
    // path is what would reject or fragment an oversized frame. There is no
    // netdev here — frames go as raw MPDUs straight to bulk-OUT — so the
    // question is what devourer itself bounds. Two bounds exist and neither
    // binds at §9.3a's High budget:
    //
    //   TX: none. send_packet sizes its TXDMA block from the frame
    //       (jaguar3 RtlJaguar3Device.cpp:1713) and imposes no length cap.
    //   RX: the bulk-IN URB, DeviceConfig::Rx::urb_bytes, default 16 KiB and
    //       floored at 4 KiB — an aggregate must not span two URBs. We never
    //       set it, so the effective floor is 4 KiB against a 3072 B budget.
    //
    // So the tier is asserted, and now with the reason stated rather than on
    // the strength of create() having succeeded. Logged like the monitor path
    // logs its netdev derivation, so a boot log shows what each backend chose.
    std::fprintf(stderr,
                 "radio: no netdev MTU gate (raw MPDU injection); "
                 "packet budget %u\n",
                 static_cast<unsigned>(mtu_tier::kHighBudget));
    return mtu_tier::kHighBudget;
}

std::optional<uint32_t> RadioAir::estimate_airtime_us(
    size_t bytes, bool include_pending, uint16_t packet_budget) const {
    // §14.2 (Pass 143). Same conservative service-rate model kernel-monitor
    // uses, minus the pending term: devourer's send_packet is a synchronous
    // bulk-OUT that returns once the transfer has completed or failed, so the
    // frame is already with the chip and there is no queue to query. Absent by
    // construction, not approximated — which is why include_pending is ignored
    // here rather than treated as an unmet request.
    (void)include_pending;
    const Impl& im = *impl_;
    if (im.cfg.airtime_efficiency_permille == 0) {
        return std::nullopt;  // uncalibrated reads unavailable, never optimistic
    }
    // Input bytes are Waybeam wire packets. Account for one 802.11 header +
    // FCS per standard-rung-sized MPDU; service efficiency owns preamble,
    // contention, and the other measured transport effects.
    const uint64_t budget = std::max<uint16_t>(packet_budget, 1);
    const uint64_t packets = (bytes + budget - 1u) / budget;
    const uint64_t total = bytes + packets * (kDot11HdrLen + kFcsLen);
    return ht20_service_time_us(
        static_cast<size_t>(std::min<uint64_t>(total, SIZE_MAX)), im.rate.mcs,
        im.rate.sgi, im.cfg.airtime_efficiency_permille);
}

std::string RadioAir::adapter_mac(size_t adapter) const {
    if (adapter >= impl_->adapters.size()) {
        return {};
    }
    return impl_->adapters[adapter]->mac;
}

std::optional<AirIface::AirSense> RadioAir::rx_sense(size_t adapter) {
    Impl& im = *impl_;
    if (adapter >= im.adapters.size() || !im.adapters[adapter]->dev) {
        return std::nullopt;
    }
    devourer::RxQuality q;
    try {
        q = im.adapters[adapter]->dev->GetRxQuality();
    } catch (const std::exception&) {
        // Register I/O can throw on a USB glitch; a sweep degrades to the
        // sensor-less fallback rather than taking the process down.
        return std::nullopt;
    }
    AirSense s;
    s.fa_valid = q.energy_valid;
    s.fa_ofdm = q.fa_ofdm;
    s.cca_ofdm = q.cca_ofdm;
    s.igi_valid = q.igi_valid;
    s.igi = q.igi;
    s.nf_valid = q.nf_valid;
    s.nf_dbm = q.noise_floor_dbm;
    s.abs_nf_valid = q.abs_nf_valid;
    s.abs_nf_dbm = q.abs_noise_floor_dbm;
    return s;
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
    c.rx_dead = a.rx_dead.load(std::memory_order_relaxed);
    c.rssi_last = a.rssi_last.load(std::memory_order_relaxed);
    c.tx_submitted = a.tx_submitted;
    c.tx_failed = a.tx_failed;
    for (size_t i = 0; i < kRxMcsBuckets; ++i) {
        c.rx_mcs[i] = a.rx_mcs[i].load(std::memory_order_relaxed);
    }
    c.rx_mcs_unknown = a.rx_mcs_unknown.load(std::memory_order_relaxed);
    c.rx_ldpc = a.rx_ldpc.load(std::memory_order_relaxed);
    c.rx_stbc = a.rx_stbc.load(std::memory_order_relaxed);
    c.ldpc_flag_ok = a.ldpc_flag_ok;
    // (Pass 158 quality lives in rx_quality_window(), not here — its read
    // drains the window, and counters() must stay non-destructive.)
    if (a.tx) {  // reports only exist for the injecting adapter's frames
        c.tx_reports = impl_->tx_reports.load(std::memory_order_relaxed);
        c.tx_report_fails =
            impl_->tx_report_fails.load(std::memory_order_relaxed);
    }
    return c;
}

RadioAir::RxQualityWindow RadioAir::rx_quality_window(size_t adapter) {
    RxQualityWindow w;
    if (adapter >= impl_->adapters.size()) return w;
    // snapshot() drains and resets the vendored accumulator — the delta
    // semantics §15.3 documents. Unit folds per LinkHealth.h: PWDB dBm ≈
    // raw − 110; SNR/EVM signed half-dB (dB = raw/2, EVM lower = cleaner).
    const devourer::RxQualitySnapshot s =
        impl_->adapters[adapter]->quality.snapshot();
    w.frames = s.frames;
    if (s.frames > 0) {
        w.rssi_peak_dbm = s.rssi_max_raw - 110;
        w.rssi_mean_dbm = s.rssi_mean_raw - 110;
        w.snr_db = s.snr_mean_raw / 2;
        if (s.evm_valid) {
            w.evm_db = s.evm_mean_raw / 2;
            w.evm_valid = true;
        }
        if (s.nf_valid) {
            w.noise_dbm = static_cast<int32_t>(s.nf_mean_dbm);
            w.noise_valid = true;
        }
    }
    return w;
}

}  // namespace wblink
