// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/air_udp.h"

#include <poll.h>

#include <cerrno>

#include "wblink/wire.h"

namespace wblink {

namespace {
const CommonPrefix* prefix_of(const Decoded& dec) {
    if (const auto* p = std::get_if<DataView>(&dec)) return &p->hdr.prefix;
    if (const auto* p = std::get_if<NackView>(&dec)) return &p->hdr.prefix;
    if (const auto* p = std::get_if<LinkReport>(&dec)) return &p->prefix;
    if (const auto* p = std::get_if<Heartbeat>(&dec)) return &p->prefix;
    if (const auto* p = std::get_if<CsaPacket>(&dec)) return &p->prefix;
    return nullptr;
}
}  // namespace

Result<UdpAir> UdpAir::create(const AirUdpCfg& cfg) {
    UdpAir air;
    for (const std::string& t : cfg.tx) {
        auto out = UdpEgress::open(t, cfg.broadcast);
        if (!out) {
            return Result<UdpAir>::fail("air tx: " + out.error);
        }
        air.targets_.push_back(std::move(*out.value));
    }
    for (const std::string& r : cfg.rx) {
        auto in = UdpIngress::open(r, cfg.broadcast ? cfg.originator : 0);
        if (!in) {
            return Result<UdpAir>::fail("air rx: " + in.error);
        }
        air.adapters_.push_back(std::move(*in.value));
    }
    air.rx_drop_permille_ = cfg.rx_drop_permille;
    air.broadcast_ = cfg.broadcast;
    air.originator_ = cfg.originator;
    air.pace_mbps_ = cfg.pace_mbps;
    air.rx_dropped_.assign(air.adapters_.size(), 0);
    air.rx_frames_.assign(air.adapters_.size(), 0);
    air.rx_filtered_.assign(air.adapters_.size(), 0);
    air.rng_.resize(air.adapters_.size());
    for (size_t i = 0; i < air.rng_.size(); ++i) {
        // Independent per-adapter seed (nonzero) — decorrelated synthetic loss.
        air.rng_[i] = 0x9e3779b9u + static_cast<uint32_t>(i) * 0x85ebca6bu + 1u;
    }
    return Result<UdpAir>::ok(std::move(air));
}

size_t UdpAir::inject(const uint8_t* frame, size_t len) {
    if (pace_mbps_ != 0) {
        if (targets_.empty()) {
            ++tx_failed_;
            return 0;
        }
        constexpr size_t kPacedQueueCap = 8192;
        if (tx_queue_.size() >= kPacedQueueCap) {
            ++tx_failed_;
            return 0;
        }
        tx_queue_.emplace_back(frame, frame + len);
        return 1;
    }
    size_t reached = 0;
    for (size_t target_idx = 0; target_idx < targets_.size(); ++target_idx) {
        UdpEgress& t = targets_[target_idx];
        if (t.send(frame, len)) {
            if (trace_) trace_("tx", "submitted", static_cast<int>(target_idx),
                               frame, len);
            ++reached;
            ++tx_submitted_;
            if (broadcast_) {
                // Every shared-port listener gets and rejects the local
                // broadcast copy in its socket BPF.
                for (size_t i = 0; i < adapters_.size(); ++i) {
                    ++rx_filtered_[i];
                    adapters_[i].note_socket_filtered();
                }
            }
        } else {
            if (trace_) trace_("tx", "failed", static_cast<int>(target_idx),
                               frame, len);
            ++tx_failed_;
        }
    }
    return reached;
}

void UdpAir::service_paced_tx() {
    if (pace_mbps_ == 0 || tx_queue_.empty() || targets_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (next_tx_.time_since_epoch().count() == 0) next_tx_ = now;
    size_t serviced = 0;
    constexpr size_t kCatchupCap = 16;
    while (!tx_queue_.empty() && now >= next_tx_ && serviced < kCatchupCap) {
        const std::vector<uint8_t>& frame = tx_queue_.front();
        if (targets_[0].send(frame.data(), frame.size())) {
            if (trace_) trace_("tx", "submitted", 0, frame.data(), frame.size());
            ++tx_submitted_;
            for (size_t i = 0; i < adapters_.size(); ++i) {
                ++rx_filtered_[i];
                adapters_[i].note_socket_filtered();
            }
        } else {
            if (trace_) trace_("tx", "failed", 0, frame.data(), frame.size());
            ++tx_failed_;
        }
        const uint64_t ns =
            (static_cast<uint64_t>(frame.size()) * 8000u + pace_mbps_ - 1u) /
            pace_mbps_;
        next_tx_ += std::chrono::nanoseconds(ns);
        tx_queue_.pop_front();
        ++serviced;
    }
    if (!tx_queue_.empty() && serviced == kCatchupCap && now >= next_tx_) {
        // Process stalls must not be repaid as an unbounded host-speed burst.
        const uint64_t ns =
            (static_cast<uint64_t>(tx_queue_.front().size()) * 8000u +
             pace_mbps_ - 1u) /
            pace_mbps_;
        next_tx_ = now + std::chrono::nanoseconds(ns);
    }
}

int UdpAir::poll_once(int timeout_ms, const RxCb& cb) {
    service_paced_tx();
    if (adapters_.empty()) {
        return 0;
    }
    std::vector<pollfd> fds;
    fds.reserve(adapters_.size());
    for (const UdpIngress& a : adapters_) {
        fds.push_back(pollfd{a.fd(), POLLIN, 0});
    }
    if (!tx_queue_.empty() && next_tx_.time_since_epoch().count() != 0) {
        const auto remaining = next_tx_ - std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            remaining + std::chrono::microseconds(999))
                            .count();
        timeout_ms = std::min(
            timeout_ms, static_cast<int>(std::max<int64_t>(0, ms)));
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
            if (broadcast_) {
                const Decoded dec = decode(buf_.data(), static_cast<size_t>(n));
                const CommonPrefix* prefix = prefix_of(dec);
                if (prefix == nullptr || prefix->originator == originator_) {
                    if (trace_) trace_("rx", "filtered", static_cast<int>(i),
                                       buf_.data(), static_cast<size_t>(n));
                    ++rx_filtered_[i];
                    continue;
                }
            }
            // Bench synthetic per-adapter RX drop (parity with mon/radio).
            if (rx_drop_permille_ > 0) {
                uint32_t& s = rng_[i];
                s ^= s << 13;
                s ^= s >> 17;
                s ^= s << 5;
                if (s % 1000u < rx_drop_permille_) {
                    if (trace_) trace_("rx", "synthetic_drop", static_cast<int>(i),
                                       buf_.data(), static_cast<size_t>(n));
                    ++rx_dropped_[i];
                    continue;
                }
            }
            AirRxMeta meta;
            meta.adapter_id = static_cast<uint8_t>(i);
            if (trace_) trace_("rx", "accepted", static_cast<int>(i),
                               buf_.data(), static_cast<size_t>(n));
            ++rx_frames_[i];
            cb(meta, buf_.data(), static_cast<size_t>(n));
            ++delivered;
        }
    }
    service_paced_tx();
    return delivered;
}

}  // namespace wblink
