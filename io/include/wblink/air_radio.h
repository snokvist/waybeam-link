// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: devourer radio air backend (§3.0/§7). Same inject/poll
// shape as UdpAir so the app swaps backends behind one variable:
//   inject()    — wrap the wire packet in radiotap + the §3.0 data frame and
//                 send_packet() it on the designated TX adapter.
//   poll_once() — drain frames the per-adapter RX threads queued (already
//                 §3.0-filtered), tagged with adapter id / RSSI dBm / tsfl.
//
// Threading: devourer runs one RX loop thread per adapter (the proven N=3
// pattern — per-adapter libusb_context); frames cross into the caller's
// single-threaded world through one bounded queue. Everything else —
// inject(), the control plane (set_tx_mode/set_power_qdb/read_tsf), create
// and destruction — is main-thread only, per devourer's control-plane
// threading contract.
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

struct RadioAirCfg {
    std::vector<AdapterCfg> adapters;  // exactly one Role::kTx among them
    uint8_t stamp_net_id = 0;          // §3.0 SA net_id (TX always stamps)
    std::optional<uint8_t> filter_net_id;  // RX enforces only when configured
    uint16_t originator = 0;           // stamped in SA; own frames dropped
};

class RadioAir {
  public:
    using RxCb = std::function<void(const AirRxMeta&, const uint8_t*, size_t)>;

    static Result<RadioAir> create(const RadioAirCfg& cfg);

    RadioAir(RadioAir&&) noexcept;
    RadioAir& operator=(RadioAir&&) noexcept;
    ~RadioAir();

    // Send one wire packet on the TX adapter (§3.0 encapsulation added
    // here). Returns 1 when submitted, 0 on failure.
    size_t inject(const uint8_t* frame, size_t len);

    // Deliver queued RX frames (§3.0 payloads, header stripped); blocks up
    // to timeout_ms when the queue is empty. Returns frames delivered.
    int poll_once(int timeout_ms, const RxCb& cb);

    size_t rx_adapters() const;

    // --- control plane (main thread only) --------------------------------
    // Committed operating point → the TX adapter's rate-less default
    // (§9.5/§10.4). 20 MHz HT only in v0 (§1 craft constraint).
    void set_tx_mode(uint8_t mcs, bool sgi);
    // §10.4 absolute power apply; returns the qdb devourer reports applied.
    int set_power_qdb(size_t adapter, int32_t qdb);
    // Hardware TSF of one adapter (µs, control transfer — can fail under
    // heavy RX load; nullopt then, caller falls back to host time, §7.2).
    std::optional<uint64_t> read_tsf(size_t adapter);
    // §11 CSA retune. fast + bw==0 → FastRetune (class 0, TXAGC untouched —
    // follow with reapply_tx_power); otherwise a full SetMonitorChannel.
    // false = bad adapter/channel. bw: §11.1 encoding (0=20 1=40 2=80).
    bool retune(size_t adapter, uint16_t chan_mhz, uint8_t bw, bool fast);
    // §10.4/§11.2: re-program TX power at the current channel post-retune.
    bool reapply_tx_power(size_t adapter);

    struct AdapterCounters {
        std::string name;
        bool tx = false;
        uint64_t rx_frames = 0;   // §3.0-accepted frames delivered
        uint64_t rx_filtered = 0; // heard but not ours
        uint64_t rx_dropped = 0;  // queue overflow (bounded, drop-oldest)
        int8_t rssi_last = -128;  // dBm of the last accepted frame
        uint64_t tx_submitted = 0;
        uint64_t tx_failed = 0;
        // Per-frame TX-status CCX reports (devourer tx.report, Pass 8) —
        // TX adapter only. Reports stalling while tx_submitted advances is
        // the TX-wedge signal; fails counts state != 0 (retry/lifetime
        // drop — always 0 for broadcast, meaningful for unicast uplink).
        uint64_t tx_reports = 0;
        uint64_t tx_report_fails = 0;
    };
    AdapterCounters counters(size_t adapter) const;

  private:
    RadioAir();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wblink
