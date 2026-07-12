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
#include <vector>

#include "wblink/binding.h"

namespace wblink {

struct AirRxMeta {
    uint8_t adapter_id = 0;
    int8_t rssi = 0;      // synthetic on udp-air
    uint64_t tsf_us = 0;  // synthetic on udp-air (loopback has no TSF, §16)
};

class UdpAir {
  public:
    using RxCb =
        std::function<void(const AirRxMeta&, const uint8_t*, size_t)>;
    // Bench-only packet-event observation. direction is "tx" or "rx";
    // outcome is "submitted", "failed", "accepted", "filtered", or
    // "synthetic_drop". Disabled unless a caller installs the callback.
    using TraceCb = std::function<void(const char* direction, const char* outcome,
                                       int adapter, const uint8_t*, size_t)>;

    static Result<UdpAir> create(const AirUdpCfg& cfg);

    // Send one air frame to every tx target. Returns targets reached.
    size_t inject(const uint8_t* frame, size_t len);
    // Enqueue an already-authorized §5.3 retransmission ahead of paced live
    // traffic. Identical to inject() when pacing is disabled.
    size_t inject_resend(const uint8_t* frame, size_t len);

    // Drain all listen sockets; cb per frame, tagged with the adapter index.
    // Returns frames delivered, or -1 on poll error.
    int poll_once(int timeout_ms, const RxCb& cb);
    void set_trace(TraceCb cb) { trace_ = std::move(cb); }
    void set_rx_drop_permille(uint16_t value) { rx_drop_permille_ = value; }
    uint16_t rx_drop_permille() const { return rx_drop_permille_; }

    size_t rx_adapters() const { return adapters_.size(); }
    uint16_t adapter_port(size_t i) const {
        return adapters_[i].bound_port();
    }
    // Bench synthetic-drop counter for adapter i (0 unless rx_drop_permille>0).
    uint64_t rx_dropped(size_t i) const { return rx_dropped_[i]; }
    uint64_t rx_frames(size_t i) const { return rx_frames_[i]; }
    uint64_t rx_filtered(size_t i) const { return rx_filtered_[i]; }
    uint64_t kernel_dropped(size_t i) const {
        return adapters_[i].kernel_drops();
    }
    uint64_t tx_submitted() const { return tx_submitted_; }
    uint64_t tx_failed() const { return tx_failed_; }
    bool tx_pending() const {
        return !tx_queue_.empty() || !resend_queue_.empty();
    }
    std::vector<int> wait_fds() const;

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
