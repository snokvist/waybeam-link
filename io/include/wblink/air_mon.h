// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: kernel-monitor air backend (§3.0/§7). Same inject/poll shape
// as UdpAir/RadioAir so app/main.cpp swaps backends behind one variable — but
// this one needs NO devourer / libusb: it injects and receives raw 802.11
// frames through the Linux kernel driver in monitor mode via AF_PACKET
// SOCK_RAW, exactly like wfb-ng. Pure POSIX → compiled unconditionally (a
// devourer-free build still has a real RF path).
//
//   inject()    — mon_radiotap_ht(MCS) + the §3.0 dot11 header + wire packet,
//                 send() on the TX adapter's raw socket (rate is per-packet in
//                 the radiotap MCS field, not an out-of-band SetTxMode).
//   poll_once() — drain frames the per-adapter RX threads queued (radiotap
//                 parsed for RSSI/TSF, header + FCS stripped, §3.0-filtered).
//
// Threading mirrors RadioAir: one RX thread per adapter → a single bounded
// drop-oldest queue → poll_once drains on the caller thread. inject() and the
// control plane are main-thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wblink/air_udp.h"  // AirRxMeta (shared meta shape)
#include "wblink/config.h"   // AdapterCfg, Result

namespace wblink {

struct MonAirCfg {
    std::vector<AdapterCfg> adapters;  // each carries ifname; exactly one kTx
    uint8_t stamp_net_id = 0;          // §3.0 SA net_id (TX always stamps)
    std::optional<uint8_t> filter_net_id;  // RX enforces only when configured
    uint16_t originator = 0;               // stamped in SA; own frames dropped
    uint16_t rx_drop_permille = 0;         // bench-only synthetic RX loss
};

class MonAir {
  public:
    using RxCb = std::function<void(const AirRxMeta&, const uint8_t*, size_t)>;

    static Result<MonAir> create(const MonAirCfg& cfg);

    MonAir(MonAir&&) noexcept;
    MonAir& operator=(MonAir&&) noexcept;
    ~MonAir();

    // Send one wire packet on the TX adapter (§3.0 + radiotap MCS added here).
    // Returns 1 when submitted, 0 on failure.
    size_t inject(const uint8_t* frame, size_t len);

    // Return (NACK/LINK_REPORT) toward dest_originator. Monitor injection has
    // no hardware ACK responder, so this is a plain broadcast inject() (the
    // ground filters by originator); counted as a unicast fallback for §15.3.
    size_t inject_return(uint16_t dest_originator, const uint8_t* frame,
                         size_t len);
    void return_counters(uint64_t& unicast_sent,
                         uint64_t& unicast_fallback) const;

    // Deliver queued RX frames (§3.0 payloads, header stripped); blocks up to
    // timeout_ms when the queue is empty. Returns frames delivered.
    int poll_once(int timeout_ms, const RxCb& cb);
    int wait_fd() const;

    size_t rx_adapters() const;

    // --- control plane (main thread only) --------------------------------
    // Committed operating point → stamped into each frame's radiotap MCS.
    void set_tx_mode(uint8_t mcs, bool sgi);
    // §10.4 power: the 8812eu per-rate TXAGC curve owns power (rtw_tx_pwr_by_rate
    // =1; nl80211 txpower is set once at monitor bring-up). Logged intent here;
    // returns the requested qdb.
    int set_power_qdb(size_t adapter, int32_t qdb);
    // Monitor netdevs expose no per-adapter TSF read → always nullopt (caller
    // falls back to host time, §7.2).
    std::optional<uint64_t> read_tsf(size_t adapter);
    // §11 CSA over monitor is deferred — logged intent, channel is fixed at
    // bring-up. Returns true so retune_all doesn't spam.
    bool retune(size_t adapter, uint16_t chan_mhz, uint8_t bw, bool fast);
    bool reapply_tx_power(size_t adapter);

    struct AdapterCounters {
        std::string name;
        bool tx = false;
        uint64_t rx_frames = 0;
        uint64_t rx_filtered = 0;
        uint64_t rx_dropped = 0;
        uint64_t kernel_dropped = 0;
        int8_t rssi_last = -128;
        uint64_t tx_submitted = 0;
        uint64_t tx_failed = 0;
    };
    AdapterCounters counters(size_t adapter) const;

  private:
    MonAir();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wblink
