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
            AirRxMeta meta;
            meta.adapter_id = static_cast<uint8_t>(i);
            cb(meta, buf_.data(), static_cast<size_t>(n));
            ++delivered;
        }
    }
    return delivered;
}

}  // namespace wblink
