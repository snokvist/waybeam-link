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

#include "wblink/air_iface.h"  // AirIface (the backend contract)
#include "wblink/air_udp.h"  // AirRxMeta (shared meta shape)
#include "wblink/config.h"   // AdapterCfg, Result
#include "wblink/types.h"    // kRxMcsBuckets, mtu_tier

namespace wblink {

struct RadioAirCfg {
    std::vector<AdapterCfg> adapters;  // at most one Role::kTx among them;
                                       // zero only with allow_rx_only
    uint8_t stamp_net_id = 0;          // §3.0 SA net_id (TX always stamps)
    std::optional<uint8_t> filter_net_id;  // RX enforces only when configured
    uint16_t originator = 0;           // stamped in SA; own frames dropped
    // Bench-only synthetic RX loss (air.rx_drop_permille): drop this share
    // of filter-passed frames, independently per adapter. 0 = off.
    uint16_t rx_drop_permille = 0;
    // §3.0 Pass 12 hardware-ACK hybrid (gate-4 A/B; both default off):
    // arm the TX adapter's ACK responder with its own SA (craft half) /
    // send returns as unicast QoS-Data to the target's latched SA (ground
    // half; unlatched targets fall back to broadcast, counted).
    bool ack_responder = false;
    bool unicast_returns = false;
    // §15.2 (Pass 156): unicast hardware-retry limit (devourer
    // dc.tx.retry_limit). Config load enforces the §3.0 coupling law, so a
    // constructed RadioAir with either hybrid half on always has this
    // nonzero.
    int tx_retry_limit = 8;
    // §3.0 (Pass 157) node TX coding, stamped into every frame's radiotap
    // MCS field. Both default off; create() refuses when the TX die's
    // TxCaps reads the enabled coding unsupported (capability leg).
    bool ldpc = false;
    bool stbc = false;
    // §9.4 Pass 163: sequence-derived rate probing intent — a TX-die
    // property; create() refuses it on an RX-only node (fail closed). The
    // schedule itself arrives later via set_mcs_probe().
    bool mcs_probe = false;
    // Clear the MAC carrier-sense gate at bring-up (see AirCfg::disable_cca).
    bool disable_cca = false;
    // §14.2 authored transport-efficiency calibration (1..1000, 0 = off).
    // Per transport: the monitor rig's 600 does not carry over to devourer.
    uint16_t airtime_efficiency_permille = 0;
    // §3.11 (Pass 162) RX-only bring-up: permit zero role:"tx" adapters.
    // Derived from the archetype (cache-no-streams, §2 spectator), never a
    // config key. With no TX, every TX-die knob above (ack_responder,
    // unicast_returns, ldpc, stbc) refuses create — fail closed.
    bool allow_rx_only = false;

    // --- device source (B1/B4/B5). Programmatic-only by operator ruling
    // (2026-08-08): no config key, nothing for the #106 key registry, and
    // io/src/config.cpp never writes any of the three. A JSON-driven daemon
    // therefore gets exactly today's behaviour, byte for byte.

    // Pre-opened USB device descriptors, parallel to adapters[]. Empty (the
    // default) or -1 in a slot means "enumerate this stanza by bus path" —
    // the shipped path. Otherwise the fd is handed to
    // libusb_wrap_sys_device(), the only device source unrooted Android has:
    // it cannot enumerate usbfs, so its fds come from the Java UsbManager.
    //
    // OWNERSHIP STAYS WITH THE CALLER. libusb marks a wrapped handle
    // fd_keep, so teardown closes the libusb handle and leaves the fd open;
    // the caller must outlive the RadioAir and close each fd afterwards.
    // The fd must also stay valid for the whole RadioAir lifetime.
    //
    // An fd-supplied stanza has no bus path (libusb fills no port numbers on
    // the wrap path), so it is excluded from bus-path claiming entirely; the
    // §15.2 mac re-bind still binds it, because identity is read off the die.
    //
    // "Parallel" means parallel AT CLAIM TIME, exactly as a bus pin is. With
    // any mac pin present the §15.2 re-bind may hand adapters[i] a different
    // physical unit than adapter_fds[i] opened — that is the point of the
    // re-bind, and it displaces a bus pin the same way (loudly, on stderr).
    // Do not read adapter_fds[i] as "the fd behind adapter i" afterwards.
    std::vector<int> adapter_fds;

    // B4: libusb_reset_device during the claim. True is today's hardcoded
    // behaviour. It MUST be false when any fd is supplied — the reset
    // re-enumerates the device and orphans the caller's fd — and create()
    // refuses the combination rather than overriding it silently.
    //
    // What this does NOT cost: §11.6 recover() performs no USB reset (it is
    // StopRxLoop / SetMonitorChannel / StartRxLoop, air_radio.cpp), so
    // recovery is unaffected by do_reset=false. Only the bring-up reset goes.
    bool do_reset = true;

    // B5: devourer's advisory lock-file directory (UsbOpen.h). Empty = its
    // /tmp default. Android has no /tmp; its consumer passes the app cache
    // dir, so the parameter has to reach devourer from here.
    std::string lock_dir;
};

class RadioAir : public AirIface {
  public:
    static Result<RadioAir> create(const RadioAirCfg& cfg);

    RadioAir(RadioAir&&) noexcept;
    RadioAir& operator=(RadioAir&&) noexcept;
    ~RadioAir() override;

    // Send one wire packet on the TX adapter (§3.0 encapsulation added
    // here). Returns 1 when submitted, 0 on failure.
    size_t inject(const uint8_t* frame, size_t len) override;
    size_t inject_resend(const uint8_t* frame, size_t len) override;

    // Send one return (NACK/LINK_REPORT) toward dest_originator. With
    // unicast_returns on and an SA latched for the target, this goes out as
    // the §3.0 Pass-12 hardware-ACKed unicast QoS-Data; otherwise it is a
    // plain broadcast inject() (fallback counted).
    size_t inject_return(uint16_t dest_originator, const uint8_t* frame,
                         size_t len, bool urgent) override;
    // Cumulative unicast-return counters for §15.3.
    void return_counters(uint64_t& unicast_sent,
                         uint64_t& unicast_fallback) const;

    // Deliver queued RX frames (§3.0 payloads, header stripped); blocks up
    // to timeout_ms when the queue is empty. Returns frames delivered.
    int poll_once(int timeout_ms, const RxCb& cb) override;
    int wait_fd() const;
    std::vector<int> wait_fds() const override { return {wait_fd()}; }

    size_t rx_adapters() const override;

    // --- control plane (main thread only) --------------------------------
    // Committed operating point → the TX adapter's rate-less default
    // (§9.5/§10.4). 20 MHz HT only in v0 (§1 craft constraint).
    void set_tx_mode(uint8_t mcs, bool sgi) override;
    void set_mcs_probe(uint16_t period, uint16_t slot,
                       uint8_t candidate_mcs) override;
    // §10.4 absolute power apply. In-process, so it always reports accepted
    // (§10.5's false means "backend refused"); devourer's own applied-qdb
    // return was never read by any caller.
    bool set_power_qdb(size_t adapter, int32_t qdb) override;
    bool set_power_offset_qdb(size_t adapter, int32_t qdb) override;
    // Hardware TSF of one adapter (µs, control transfer — can fail under
    // heavy RX load; nullopt then, caller falls back to host time, §7.2).
    std::optional<uint64_t> read_tsf(size_t adapter) override;
    // §11 CSA retune. fast + bw==0 → FastRetune (class 0, TXAGC untouched —
    // follow with reapply_tx_power); otherwise a full SetMonitorChannel.
    // false = bad adapter/channel. bw: §11.1 encoding (0=20 1=40 2=80).
    // NOTE the parameter is the interface's canonical MHz width, but the
    // dual-encoding tolerance the callers relied on is preserved verbatim: a
    // value <=2 is taken as an already-encoded §11.1 class. That wart moved
    // here from two caller-side `bw > 2 ? bw_code(bw) : bw` expressions; it is
    // preserved, not endorsed, and resolving it is its own change.
    bool retune(size_t adapter, uint16_t chan_mhz, uint8_t width_mhz,
                bool fast) override;
    // §10.4/§11.2: re-program TX power at the current channel post-retune.
    bool reapply_tx_power(size_t adapter) override;
    // §15.5a runtime net_id roles. The stamp is main-thread only (the inject*
    // paths); the filter is read per frame on every adapter's RX thread, so it
    // is an atomic here rather than a cfg field. Filtering is software-only in
    // this backend, so a widen takes effect on the next frame with no
    // kernel pre-filter to re-attach.
    void set_stamp_net_id(uint8_t net_id) override;
    void set_filter_net_id(std::optional<uint8_t> net_id) override;
    // §15.5a scout: the uplink adapter the sweep roams, resolved from the
    // role:"tx" adapter. Meaningful only under has_tx(); an RX-only node
    // reads 0 (§3.11 Pass 162).
    size_t tx_index() const override;
    // §11.6 Pass 80 RX-liveness recovery: stop the RX loop, join it, re-run
    // the write-side bring-up at the target channel, restart the loop. Not a
    // USB-level reset — whether a devourer re-init clears an RTL88x2 USB wedge
    // is unmeasured, so this recovers the half-applied-retune case it can
    // reach and does not claim the other. Counters survive on purpose: rx_frames
    // is the guard's own liveness baseline.
    bool recover(size_t adapter, uint16_t chan_mhz,
                 uint8_t width_mhz) override;
    // §11.6 verify hygiene (Pass 69): discard the process-queue RX backlog
    // captured before a retune completed, so post-retune consumers only see
    // frames from the new channel. devourer's internal USB pipeline is below
    // this boundary, the same way driver buffers would be.
    void flush_rx() override;

    // --- declared limits -------------------------------------------------
    // Everything below is a capability this backend does NOT have today. Each
    // was previously an `if (mon)` in app/main.cpp's AirBackend with no radio
    // branch — documented at the definition, but invisible at the ~98 call
    // sites, where the call reads as working code. Stating them here does not
    // make the backend more capable; it puts the answer somewhere a test can
    // assert it and somewhere the compiler notices when a new method is added.
    // Behaviour is preserved exactly; ids are docs/devourer-parity-plan.md.

    // §10.5 "auto": zero the offset, undoing the latch and leaving any §10.2
    // curve resolve to re-apply on top (§10.5).
    bool set_power_auto(size_t adapter) override;
    // §10.5 (Pass 169): the actuator's own account of the last write —
    // devourer's applied offset plus its rail flags, latched AT the apply
    // because that is what `saturated_low/high` describe. Latching also keeps
    // this read free of USB, so the §15.5 GET cannot touch the control plane.
    std::optional<TxPowerApplied> tx_power_applied(size_t adapter) const override;
    // §15.5 (Pass 172): the per-die answers, cached at bring-up beside
    // ldpc_flag_ok/power_actuator_ok so this read never touches USB.
    AdapterCapsView adapter_caps(size_t adapter) const override;
    // §3.11 (Pass 162): truthful. False on the RX-only bring-up
    // (allow_rx_only, zero role:"tx"), where inject*/set_tx_mode report
    // not-sent/no-op and the §15.3 TX counters stay 0.
    bool has_tx() const override;
    // §9.3a tier. Asserted, not probed — there is no netdev gate on raw MPDU
    // injection, and the two bounds devourer does have (no TX cap; the 16 KiB
    // bulk-IN URB, floored at 4 KiB) clear the High budget by a wide margin.
    // Reasoning at the definition; logged at bring-up like the monitor path.
    uint16_t mtu_supported() const override;
    // §14.2 service-rate estimate. nullopt when uncalibrated — the opt-in
    // posture is the point, an uncalibrated node reads unavailable rather than
    // optimistic. include_pending is ignored: devourer's send_packet is a
    // synchronous bulk-OUT, so there is no queue to add (spec §14.2).
    std::optional<uint32_t> estimate_airtime_us(
        size_t bytes, bool include_pending,
        uint16_t packet_budget) const override;
    bool is_rf() const override { return true; }
    // §10.6 (Pass 154): the per-unit EFUSE MAC read at bring-up
    // (GetPermanentMacAddress), lowercase "aa:bb:cc:dd:ee:ff"; empty = the
    // unit reports no identity (callers fail closed — D3).
    std::string adapter_mac(size_t adapter) const override;
    // §15.5a (Pass 155): devourer GetRxQuality — it SUBSUMES GetRxEnergy
    // (drains the FA/CCA delta and the frame-quality window internally;
    // devourer's contract forbids polling both on one cadence). A USB
    // glitch folds into nullopt: a sweep must degrade, never crash.
    std::optional<AirSense> rx_sense(size_t adapter) override;

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
        // §15.3 Pass 118: accepted frames per HT MCS 0..7, plus the frames
        // whose rate the backend could not resolve. Sums to rx_frames.
        uint64_t rx_mcs[kRxMcsBuckets] = {};
        uint64_t rx_mcs_unknown = 0;
        // §9.4 Pass 163 guard 3: CRC/ICV-errored frames per descriptor MCS
        // (pre-FCS rate; bodies dropped). Feeds the RX probe window only.
        uint64_t rx_crc_mcs[kRxMcsBuckets] = {};
        // §15.3 Pass 157: accepted frames whose RX path reported LDPC
        // coding / a nonzero STBC stream count. ldpc_flag_ok is the static
        // die truth (devourer ldpc_rx_flag) — when false the counters stay
        // 0 however the air was coded, so a zero means nothing there.
        uint64_t rx_ldpc = 0;
        uint64_t rx_stbc = 0;
        bool ldpc_flag_ok = false;
        bool rx_dead = false;  // §15.3 Pass 101: RX thread exited (definitive)
    };
    AdapterCounters counters(size_t adapter) const;
    uint64_t rx_frames(size_t adapter) const override {
        return counters(adapter).rx_frames;
    }

    // §15.3 (Pass 158) per-adapter quality window over accepted frames,
    // folded with devourer's RxQuality conventions (path-A raw; rssi<=0
    // skipped; EVM only when present; PEAK RSSI — saturation trashes a
    // fraction of frames to low apparent power, so a mean inverts the
    // signal). Pass 159 amends the drain contract: the drain happens
    // INSIDE the backend at most once per second, shared by this read and
    // link_verdict() — both return the cached last window, so a second
    // consumer can never split the delta.
    struct RxQualityWindow {
        uint32_t frames = 0;      // quality samples folded (0 = empty)
        int32_t rssi_peak_dbm = 0;
        int32_t rssi_mean_dbm = 0;
        int32_t snr_db = 0;
        int32_t evm_db = 0;       // lower (more negative) = cleaner
        bool evm_valid = false;   // false = no frame carried EVM, not "0"
        int32_t noise_dbm = 0;    // passive floor: rssi_dbm - snr_db
        bool noise_valid = false;
    };
    RxQualityWindow rx_quality_window(size_t adapter);
    // §3.16 (Pass 159) node link verdict (0..6, types.h link_verdict),
    // classified at the shared drain from the best-peak ear's window via
    // the vendored thresholds — frame-metric legs only, the energy/IGI
    // legs stay with the scout (Pass 155).
    uint8_t link_verdict();

    // TX adapter's cumulative (tx_submitted, tx_reports) for the §9.10
    // wedge watchdog — cheap per-iteration accessor, no string copies.
    void tx_report_counters(uint64_t& submitted, uint64_t& reports) const;

  private:
    RadioAir();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wblink
