// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: kernel-monitor air backend (§3.0/§7). Same inject/poll shape
// as UdpAir/RadioAir so app/main.cpp swaps backends behind one variable — but
// this one needs NO devourer / libusb: it injects and receives raw 802.11
// frames through the Linux kernel driver in monitor mode via AF_PACKET
// SOCK_RAW, exactly like wfb-ng. Pure POSIX → compiled unconditionally (a
// devourer-free build still has a real RF path).
//
//   inject()    — radiotap_tx_ht(MCS) + the §3.0 dot11 header + wire packet,
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

#include "wblink/air_iface.h"  // AirIface (the backend contract)
#include "wblink/air_udp.h"  // AirRxMeta (shared meta shape)
#include "wblink/config.h"   // AdapterCfg, Result
#include "wblink/types.h"

namespace wblink {

struct MonAirCfg {
    // Each carries ifname; exactly one kTx unless allow_rx_only is set.
    std::vector<AdapterCfg> adapters;
    // A store-only Ethernet repair cache needs an RF ear but has no reason to
    // inject on air. Keep this opt-in so ordinary ground/craft configs still
    // fail closed when their designated uplink is missing.
    bool allow_rx_only = false;
    uint8_t stamp_net_id = 0;          // §3.0 SA net_id (TX always stamps)
    std::optional<uint8_t> filter_net_id;  // RX enforces only when configured
    uint16_t originator = 0;               // stamped in SA; own frames dropped
    uint16_t rx_drop_permille = 0;         // bench-only synthetic RX loss
    uint16_t airtime_efficiency_permille = 0;  // §14.2; 0 = unavailable
};

class MonAir : public AirIface {
  public:
    static Result<MonAir> create(const MonAirCfg& cfg);

    MonAir(MonAir&&) noexcept;
    MonAir& operator=(MonAir&&) noexcept;
    ~MonAir() override;

    // Send one wire packet on the TX adapter (§3.0 + radiotap MCS added here).
    // Returns 1 when submitted, 0 on failure.
    size_t inject(const uint8_t* frame, size_t len) override;
    size_t inject_resend(const uint8_t* frame, size_t len) override;

    // Return (NACK/LINK_REPORT) toward dest_originator. Monitor injection has
    // no hardware ACK responder, so this is a plain broadcast inject() (the
    // ground filters by originator); counted as a unicast fallback for §15.3.
    size_t inject_return(uint16_t dest_originator, const uint8_t* frame,
                         size_t len, bool urgent) override;
    void return_counters(uint64_t& unicast_sent,
                         uint64_t& unicast_fallback) const;

    // §9.10 monitor TX-progress source: send() proves submission, while the
    // Linux netdev tx_packets counter proves driver transmission progress.
    // Both counters are cumulative.
    void tx_progress_counters(uint64_t& submitted, uint64_t& completed) const;

    // §14.2 effective HT20 serialization time. include_pending adds socket
    // outbound bytes when the kernel exposes them; zero calibration = unknown.
    std::optional<uint32_t> estimate_airtime_us(
        size_t bytes, bool include_pending,
        uint16_t packet_budget) const override;

    // Deliver queued RX frames (§3.0 payloads, header stripped); blocks up to
    // timeout_ms when the queue is empty. Returns frames delivered.
    int poll_once(int timeout_ms, const RxCb& cb) override;
    int wait_fd() const;
    // One socket per backend here; the plural is the interface's shape (udp-air
    // waits per adapter).
    std::vector<int> wait_fds() const override { return {wait_fd()}; }

    size_t rx_adapters() const override;
    bool has_tx() const override;
    // §9.3a minimum packet budget across the live monitor netdevs. Unknown
    // interface MTU reads resolve conservatively to Default.
    uint16_t mtu_supported() const override;
    // §15.5a scout: index of the designated `role:"tx"` uplink adapter — the one
    // the scout roams during a sweep (Pass 64). Not assumed to be 0.
    size_t tx_index() const override;
    // Frames go on real RF through the kernel driver.
    bool is_rf() const override { return true; }
    // Deliberately empty: kernel-monitor is deprecated-frozen (ruling #120)
    // and keeps its own §10.6 identity tiers (calib_id/ifname) — it gains no
    // Pass 154 MAC leg.
    std::string adapter_mac(size_t) const override { return {}; }

    // --- control plane (main thread only) --------------------------------
    // Committed operating point → stamped into each frame's radiotap MCS.
    void set_tx_mode(uint8_t mcs, bool sgi) override;
    // §15.5a scout: decouple the two net_id roles at runtime. The scout widens
    // the RX filter to hear all net_ids during a sweep, then narrows it to the
    // claimed craft's net_id post-lock; the stamp follows so return traffic
    // (NACK/CSA) lands inside the craft's filter. set_filter_net_id re-attaches
    // the §3.0 BPF pre-filter (nullopt = waybeam-shape only, any net_id).
    void set_stamp_net_id(uint8_t net_id) override;
    void set_filter_net_id(std::optional<uint8_t> net_id) override;
    // §10.4/§10.5 power: nl80211 fixed power via a bounded `iw set txpower
    // fixed <qdb×25 mBm>` fork — the kernel-monitor leg of the backend
    // actuation matrix (Pass 114). False on CLI failure — callers must not
    // cache the value as applied (§10.5).
    bool set_power_qdb(size_t adapter, int32_t qdb) override;
    bool set_power_offset_qdb(size_t adapter, int32_t qdb) override;
    // §10.5 auto restore: `iw set txpower auto` — hand power back to the
    // driver default / per-rate TXAGC curve (the mon-up.sh posture).
    bool set_power_auto(size_t adapter) override;
    // Monitor netdevs expose no per-adapter TSF read → always nullopt (caller
    // falls back to host time, §7.2).
    std::optional<uint64_t> read_tsf(size_t adapter) override;
    // §11.5/§15.5a channel retune: drives `iw dev <if> set freq` (the ssc338q
    // SDK lacks libnl-3). Changes the wiphy channel without a down/up, so RX
    // sockets survive. Returns false if the iw call fails. Serves both the CSA
    // follower switch and the scout sweep.
    bool retune(size_t adapter, uint16_t chan_mhz, uint8_t width_mhz,
                bool fast) override;
    // §11.6 Pass 80 RX-liveness recovery: full monitor re-init of one adapter
    // (link down → monitor type → link up → MTU → set freq — the bring-up
    // sequence). For the RTL88x2 half-applied in-place retune (TX airs, RX
    // deaf). RX sockets are bound by ifindex and survive the down/up.
    bool recover(size_t adapter, uint16_t chan_mhz,
                 uint8_t width_mhz) override;
    bool reapply_tx_power(size_t adapter) override;
    // §11.6 issuer verify hygiene (Pass 69): discard RX backlog captured
    // before a retune completed — kernel socket buffers and the process
    // queue — so post-retune consumers only see frames actually received on
    // the new channel (the Pass 66 stale-drain artifact must not satisfy a
    // video-verify). Called after retune_all; a handful of genuinely fresh
    // frames may be discarded with the backlog, which the data path absorbs.
    void flush_rx() override;

    struct AdapterCounters {
        std::string name;
        bool tx = false;
        uint64_t rx_frames = 0;
        uint64_t rx_filtered = 0;
        uint64_t rx_dropped = 0;
        uint64_t kernel_dropped = 0;
        uint64_t bpf_filtered = 0;
        int8_t rssi_last = -128;
        uint64_t tx_submitted = 0;
        uint64_t tx_failed = 0;
        // §15.3 Pass 118: accepted frames per HT MCS 0..7, plus the frames
        // whose rate the backend could not resolve. Sums to rx_frames.
        uint64_t rx_mcs[kRxMcsBuckets] = {};
        uint64_t rx_mcs_unknown = 0;
    };
    AdapterCounters counters(size_t adapter) const;
    uint64_t rx_frames(size_t adapter) const override {
        return counters(adapter).rx_frames;
    }

  private:
    MonAir();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wblink
