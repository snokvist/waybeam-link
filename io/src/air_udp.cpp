// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/air_udp.h"

#include <poll.h>

#include <cerrno>

namespace wblink {

Result<UdpAir> UdpAir::create(const AirUdpCfg& cfg) {
    UdpAir air;
    for (const std::string& t : cfg.tx) {
        auto out = UdpEgress::open(t);
        if (!out) {
            return Result<UdpAir>::fail("air tx: " + out.error);
        }
        air.targets_.push_back(std::move(*out.value));
    }
    for (const std::string& r : cfg.rx) {
        auto in = UdpIngress::open(r);
        if (!in) {
            return Result<UdpAir>::fail("air rx: " + in.error);
        }
        air.adapters_.push_back(std::move(*in.value));
    }
    air.rx_drop_permille_ = cfg.rx_drop_permille;
    air.rx_dropped_.assign(air.adapters_.size(), 0);
    air.rng_.resize(air.adapters_.size());
    for (size_t i = 0; i < air.rng_.size(); ++i) {
        // Independent per-adapter seed (nonzero) — decorrelated synthetic loss.
        air.rng_[i] = 0x9e3779b9u + static_cast<uint32_t>(i) * 0x85ebca6bu + 1u;
    }
    return Result<UdpAir>::ok(std::move(air));
}

size_t UdpAir::inject(const uint8_t* frame, size_t len) {
    size_t reached = 0;
    for (UdpEgress& t : targets_) {
        reached += t.send(frame, len) ? 1u : 0u;
    }
    return reached;
}

int UdpAir::poll_once(int timeout_ms, const RxCb& cb) {
    if (adapters_.empty()) {
        return 0;
    }
    std::vector<pollfd> fds;
    fds.reserve(adapters_.size());
    for (const UdpIngress& a : adapters_) {
        fds.push_back(pollfd{a.fd(), POLLIN, 0});
    }
    const int rc = ::poll(fds.data(), fds.size(), timeout_ms);
    if (rc < 0) {
        return errno == EINTR ? 0 : -1;
    }
    int delivered = 0;
    for (size_t i = 0; i < adapters_.size(); ++i) {
        if ((fds[i].revents & POLLIN) == 0) {
            continue;
        }
        for (;;) {
            const long n = adapters_[i].recv_one(buf_.data(), buf_.size());
            if (n <= 0) {
                break;
            }
            // Bench synthetic per-adapter RX drop (parity with mon/radio).
            if (rx_drop_permille_ > 0) {
                uint32_t& s = rng_[i];
                s ^= s << 13;
                s ^= s >> 17;
                s ^= s << 5;
                if (s % 1000u < rx_drop_permille_) {
                    ++rx_dropped_[i];
                    continue;
                }
            }
            AirRxMeta meta;
            meta.adapter_id = static_cast<uint8_t>(i);
            cb(meta, buf_.data(), static_cast<size_t>(n));
            ++delivered;
        }
    }
    return delivered;
}

}  // namespace wblink
