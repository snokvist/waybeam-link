// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: udp-air DEV backend — the air-frame path carried over UDP
// sockets so tx and rx run as two real processes (two hosts over ethernet)
// without radios or devourer. One listen socket = one virtual adapter; every
// tx target receives every injected frame (a poor man's broadcast).
//
// This is bench tooling, not §15 I/O and not the radio path: the devourer
// backend replaces it at hardware bring-up (build steps 9–11) behind the
// same inject/poll shape. rssi/tsf metadata is synthetic (0).
#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <deque>
#include <optional>
#include <vector>

#include "wblink/air_iface.h"  // AirIface (the backend contract)
#include "wblink/binding.h"

namespace wblink {

class UdpAir : public AirIface {
  public:
    // Bench-only packet-event observation. direction is "tx" or "rx";
    // outcome is "submitted", "failed", "accepted", "filtered", or
    // "synthetic_drop". Disabled unless a caller installs the callback.
    using TraceCb = std::function<void(const char* direction, const char* outcome,
                                       int adapter, const uint8_t*, size_t)>;

    static Result<UdpAir> create(const AirUdpCfg& cfg);

    // Send one air frame to every tx target. Returns targets reached.
    size_t inject(const uint8_t* frame, size_t len) override;
    // Enqueue an already-authorized §5.3 retransmission ahead of paced live
    // traffic. Identical to inject() when pacing is disabled.
    size_t inject_resend(const uint8_t* frame, size_t len) override;

    // Drain all listen sockets; cb per frame, tagged with the adapter index.
    // Returns frames delivered, or -1 on poll error.
    int poll_once(int timeout_ms, const RxCb& cb) override;
    void set_trace(TraceCb cb) { trace_ = std::move(cb); }
    void set_rx_drop_permille(uint16_t value) { rx_drop_permille_ = value; }
    uint16_t rx_drop_permille() const { return rx_drop_permille_; }

    size_t rx_adapters() const override { return adapters_.size(); }
    uint16_t adapter_port(size_t i) const {
        return adapters_[i].bound_port();
    }
    // Bench synthetic-drop counter for adapter i (0 unless rx_drop_permille>0).
    uint64_t rx_dropped(size_t i) const { return rx_dropped_[i]; }
    // Bounds-checked because this one is on the AirIface contract: both RF
    // backends answer an out-of-range adapter with a zeroed counters struct,
    // and a caller holding an AirIface* cannot tell which backend it has. The
    // sibling accessors below are not on the contract and keep their existing
    // (caller-bounded) shape.
    uint64_t rx_frames(size_t i) const override {
        return i < rx_frames_.size() ? rx_frames_[i] : 0;
    }
    uint64_t rx_filtered(size_t i) const { return rx_filtered_[i]; }
    uint64_t kernel_dropped(size_t i) const {
        return adapters_[i].kernel_drops();
    }
    uint64_t tx_submitted() const { return tx_submitted_; }
    uint64_t tx_failed() const { return tx_failed_; }
    bool tx_pending() const {
        return !tx_queue_.empty() || !resend_queue_.empty();
    }
    // Paced Ethernet bench airtime model (§14.2). include_pending models a
    // new live frame appended behind both current queues; false models one
    // prioritized resend. Unpaced UDP has no authored serialization rate.
    // packet_budget is the interface's third parameter; the Ethernet bench
    // model has no per-packet budget to apply and ignores it.
    std::optional<uint32_t> estimate_airtime_us(
        size_t bytes, bool include_pending,
        uint16_t packet_budget) const override;
    std::vector<int> wait_fds() const override;

    // --- declared limits -------------------------------------------------
    // udp-air is the bench/loopback transport: no PHY, no radio, no L2
    // addressing. Every entry below states that in the one place a caller or
    // a test can see it, instead of it being the absence of a branch in
    // AirBackend. Behaviour is exactly what the old fall-throughs did.

    // No L2 addressing: the target is ignored and this is a plain inject.
    size_t inject_return(uint16_t dest_originator, const uint8_t* frame,
                         size_t len, bool urgent) override;
    // Nothing buffers below this boundary — the RX drain is the socket read.
    void flush_rx() override {}
    // A retune is logged intent, so it "succeeds": the CSA state machines stay
    // exercisable end-to-end without radios (§16).
    bool retune(size_t adapter, uint16_t chan_mhz, uint8_t width_mhz,
                bool fast) override;
    // §11.6 Pass 80 re-init has no meaning here; the caller reads false as
    // "recovery unavailable on this backend".
    bool recover(size_t adapter, uint16_t chan_mhz,
                 uint8_t width_mhz) override;
    // Nothing to re-apply. True = "nothing failed", which is what the retune
    // loop's ignored return has always meant for this backend.
    bool reapply_tx_power(size_t /*adapter*/) override { return true; }
    // Power is logged intent, accepted the same way an in-process write is.
    bool set_power_qdb(size_t adapter, int32_t qdb) override;
    bool set_power_offset_qdb(size_t adapter, int32_t qdb) override;
    bool set_power_auto(size_t adapter) override;
    // §10.5 (Pass 169): no actuator, so nothing can rail and there is no
    // applied value distinct from the request. Written down rather than
    // inherited from a default — the whole point of this contract.
    std::optional<TxPowerApplied> tx_power_applied(
        size_t /*adapter*/) const override {
        return std::nullopt;
    }
    // No PHY: there is no rate to stamp and no hardware clock to read.
    void set_tx_mode(uint8_t /*mcs*/, bool /*sgi*/) override {}
    void set_mcs_probe(uint16_t, uint16_t, uint8_t) override {}  // no PHY
    std::optional<uint64_t> read_tsf(size_t /*adapter*/) override {
        return std::nullopt;
    }
    // §3.0 net_id rides the SA, which loopback has no room for (AirRxMeta
    // documents net_id as 0 on udp).
    void set_stamp_net_id(uint8_t /*net_id*/) override {}
    void set_filter_net_id(std::optional<uint8_t> /*net_id*/) override {}
    // The dev backend scouts index 0 and has an uplink by construction.
    size_t tx_index() const override { return 0; }
    bool has_tx() const override { return true; }
    // Compatibility tier: no driver matrix to assert anything better.
    uint16_t mtu_supported() const override { return kDefaultMaxPayload; }
    bool is_rf() const override { return false; }
    // §15.5 (Pass 172): no silicon — every capability stated false, and the
    // chip name says which backend answered. §15.2 (Pass 195): `part` and
    // `aliases` stay EMPTY rather than repeating "udp" — there is no die here,
    // and a consumer must be able to tell "no die" from a die it does not
    // recognise.
    AdapterCapsView adapter_caps(size_t /*adapter*/) const override {
        return AdapterCapsView{"udp", "", "", false, false, false};
    }
    // No hardware, no per-unit identity (§10.6 Pass 154).
    std::string adapter_mac(size_t) const override { return {}; }
    // No RF, no frame-free sensor (§15.5a Pass 155).
    std::optional<AirSense> rx_sense(size_t) override { return std::nullopt; }

  private:
    std::vector<UdpEgress> targets_;
    std::vector<UdpIngress> adapters_;
    std::vector<uint8_t> buf_ = std::vector<uint8_t>(4096);
    uint16_t rx_drop_permille_ = 0;
    bool broadcast_ = false;
    uint16_t originator_ = 0;
    uint32_t pace_mbps_ = 0;
    std::chrono::steady_clock::time_point next_tx_{};
    std::vector<uint32_t> rng_;         // per-adapter xorshift state
    std::vector<uint64_t> rx_dropped_;  // per-adapter synthetic-drop count
    std::vector<uint64_t> rx_frames_;   // accepted frames per adapter
    std::vector<uint64_t> rx_filtered_; // malformed/self broadcast frames
    uint64_t tx_submitted_ = 0;         // successful target datagrams
    uint64_t tx_failed_ = 0;            // failed target datagrams
    std::deque<std::vector<uint8_t>> tx_queue_;
    std::deque<std::vector<uint8_t>> resend_queue_;
    TraceCb trace_;

    void service_paced_tx();
};

}  // namespace wblink
