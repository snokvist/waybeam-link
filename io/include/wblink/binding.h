// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: v0 UDP binding layer (PROTOCOL.md §15.1).
//
// One BindingSet per process, built from the validated Config. In-streams own
// a bound non-blocking socket; out-streams own a connected send socket.
// poll_once() is the surface the step-3 TX framer (and later the RX egress)
// consumes: it drains every readable ingress and hands each datagram to the
// callback tagged with its stream_id. Control packets never touch a binding —
// the core consumes them (§15.1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "wblink/config.h"

namespace wblink {

// "host:port" -> (IPv4 dotted host, port). v0 is IPv4-only.
Result<std::pair<std::string, uint16_t>> split_host_port(const std::string& s);

class UdpIngress {
  public:
    UdpIngress() = default;
    ~UdpIngress();
    UdpIngress(const UdpIngress&) = delete;
    UdpIngress& operator=(const UdpIngress&) = delete;
    UdpIngress(UdpIngress&& other) noexcept;
    UdpIngress& operator=(UdpIngress&& other) noexcept;

    // Binds "host:port" non-blocking; port 0 = ephemeral (tests).
    static Result<UdpIngress> open(const std::string& listen,
                                   uint16_t reject_originator = 0);

    int fd() const { return fd_; }
    uint16_t bound_port() const { return bound_port_; }
    uint64_t kernel_drops() const {
        return kernel_drops_ > socket_filtered_ ? kernel_drops_ - socket_filtered_
                                                : 0;
    }
    void note_socket_filtered(uint64_t count = 1) { socket_filtered_ += count; }

    // One datagram; >0 = bytes, 0 = nothing pending, -1 = error.
    long recv_one(uint8_t* buf, size_t cap);

  private:
    int fd_ = -1;
    uint16_t bound_port_ = 0;
    uint32_t kernel_drop_last_ = 0;
    uint64_t kernel_drops_ = 0;
    uint64_t socket_filtered_ = 0;
};

class UdpEgress {
  public:
    UdpEgress() = default;
    ~UdpEgress();
    UdpEgress(const UdpEgress&) = delete;
    UdpEgress& operator=(const UdpEgress&) = delete;
    UdpEgress(UdpEgress&& other) noexcept;
    UdpEgress& operator=(UdpEgress&& other) noexcept;

    // Connects a datagram socket to "host:port".
    static Result<UdpEgress> open(const std::string& target,
                                  bool broadcast = false);

    int fd() const { return fd_; }
    bool send(const uint8_t* data, size_t len);
    size_t send_many(const std::vector<std::vector<uint8_t>>& frames);

  private:
    int fd_ = -1;
};

struct IngressEvent {
    uint8_t stream_id;
    const uint8_t* data;  // valid only for the duration of the callback
    size_t len;
};

class BindingSet {
  public:
    // Opens every stream binding + the stats binding from a validated config.
    static Result<BindingSet> create(const Config& cfg);

    // nullptr if the stream is not an out-stream.
    UdpEgress* egress_for(uint8_t stream_id);
    // nullptr if no stats binding is configured.
    UdpEgress* stats_egress();
    // 0 if the stream is not an in-stream (tests / ephemeral ports).
    uint16_t ingress_port(uint8_t stream_id) const;

    // Polls all ingress fds for up to timeout_ms, drains every readable one,
    // invokes cb per datagram. Returns datagrams delivered, or -1 on error.
    int poll_once(int timeout_ms,
                  const std::function<void(const IngressEvent&)>& cb);

    // As above, but also waits on extra_fds (frame-shm consumer eventfds).
    // For each readable extra fd, on_extra(index-into-extra_fds) fires so the
    // app can drain that ring. Unifies the wait so a frame-shm-only node (no
    // UDP ingress) still blocks on its ring's eventfd.
    int poll_once(int timeout_ms,
                  const std::function<void(const IngressEvent&)>& cb,
                  const std::vector<int>& extra_fds,
                  const std::function<void(size_t)>& on_extra);

  private:
    struct In {
        uint8_t stream_id;
        UdpIngress sock;
    };
    struct Out {
        uint8_t stream_id;
        UdpEgress sock;
    };
    std::vector<In> ins_;
    std::vector<Out> outs_;
    std::optional<UdpEgress> stats_;
    // Max DATA payload is 1424 (§3.2); 4 KiB absorbs any config mistake and
    // lets the framer report oversize_ingress instead of silently truncating.
    std::vector<uint8_t> buf_ = std::vector<uint8_t>(4096);
};

}  // namespace wblink
