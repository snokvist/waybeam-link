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
#include <functional>
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

    static Result<UdpAir> create(const AirUdpCfg& cfg);

    // Send one air frame to every tx target. Returns targets reached.
    size_t inject(const uint8_t* frame, size_t len);

    // Drain all listen sockets; cb per frame, tagged with the adapter index.
    // Returns frames delivered, or -1 on poll error.
    int poll_once(int timeout_ms, const RxCb& cb);

    size_t rx_adapters() const { return adapters_.size(); }
    uint16_t adapter_port(size_t i) const {
        return adapters_[i].bound_port();
    }
    // Bench synthetic-drop counter for adapter i (0 unless rx_drop_permille>0).
    uint64_t rx_dropped(size_t i) const { return rx_dropped_[i]; }

  private:
    std::vector<UdpEgress> targets_;
    std::vector<UdpIngress> adapters_;
    std::vector<uint8_t> buf_ = std::vector<uint8_t>(4096);
    uint16_t rx_drop_permille_ = 0;
    std::vector<uint32_t> rng_;         // per-adapter xorshift state
    std::vector<uint64_t> rx_dropped_;  // per-adapter synthetic-drop count
};

}  // namespace wblink
