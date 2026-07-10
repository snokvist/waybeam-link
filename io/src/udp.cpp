// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/binding.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace wblink {

namespace {

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

Result<int> open_udp_socket() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return Result<int>::fail(std::string("socket(): ") + std::strerror(errno));
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        const std::string err = std::string("fcntl(O_NONBLOCK): ") + std::strerror(errno);
        ::close(fd);
        return Result<int>::fail(err);
    }
    return Result<int>::ok(fd);
}

Result<sockaddr_in> to_sockaddr(const std::string& hostport) {
    auto hp = split_host_port(hostport);
    if (!hp) {
        return Result<sockaddr_in>::fail(hp.error);
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(hp.value->second);
    if (::inet_pton(AF_INET, hp.value->first.c_str(), &sa.sin_addr) != 1) {
        return Result<sockaddr_in>::fail("'" + hp.value->first +
                                         "' is not an IPv4 address (v0 is IPv4-only)");
    }
    return Result<sockaddr_in>::ok(sa);
}

}  // namespace

Result<std::pair<std::string, uint16_t>> split_host_port(const std::string& s) {
    using R = Result<std::pair<std::string, uint16_t>>;
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) {
        return R::fail("'" + s + "' is not host:port");
    }
    const std::string host = s.substr(0, colon);
    const std::string port_str = s.substr(colon + 1);
    unsigned long port = 0;
    for (const char c : port_str) {
        if (c < '0' || c > '9') {
            return R::fail("'" + s + "': port is not numeric");
        }
        port = port * 10 + static_cast<unsigned long>(c - '0');
        if (port > 65535) {
            return R::fail("'" + s + "': port out of range");
        }
    }
    return R::ok({host, static_cast<uint16_t>(port)});
}

// ---- UdpIngress -----------------------------------------------------------

UdpIngress::~UdpIngress() { close_fd(fd_); }

UdpIngress::UdpIngress(UdpIngress&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      bound_port_(std::exchange(other.bound_port_, 0)) {}

UdpIngress& UdpIngress::operator=(UdpIngress&& other) noexcept {
    if (this != &other) {
        close_fd(fd_);
        fd_ = std::exchange(other.fd_, -1);
        bound_port_ = std::exchange(other.bound_port_, 0);
    }
    return *this;
}

Result<UdpIngress> UdpIngress::open(const std::string& listen) {
    auto sa = to_sockaddr(listen);
    if (!sa) {
        return Result<UdpIngress>::fail("listen " + sa.error);
    }
    auto fd = open_udp_socket();
    if (!fd) {
        return Result<UdpIngress>::fail(fd.error);
    }
    UdpIngress in;
    in.fd_ = *fd.value;
    const int one = 1;
    ::setsockopt(in.fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(in.fd_, reinterpret_cast<const sockaddr*>(&*sa.value),
               sizeof(*sa.value)) < 0) {
        return Result<UdpIngress>::fail("bind('" + listen + "'): " +
                                        std::strerror(errno));
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(in.fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
        in.bound_port_ = ntohs(bound.sin_port);
    }
    return Result<UdpIngress>::ok(std::move(in));
}

long UdpIngress::recv_one(uint8_t* buf, size_t cap) {
    const ssize_t n = ::recv(fd_, buf, cap, 0);
    if (n >= 0) {
        return n;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    return -1;
}

// ---- UdpEgress ------------------------------------------------------------

UdpEgress::~UdpEgress() { close_fd(fd_); }

UdpEgress::UdpEgress(UdpEgress&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

UdpEgress& UdpEgress::operator=(UdpEgress&& other) noexcept {
    if (this != &other) {
        close_fd(fd_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

Result<UdpEgress> UdpEgress::open(const std::string& target) {
    auto sa = to_sockaddr(target);
    if (!sa) {
        return Result<UdpEgress>::fail("send " + sa.error);
    }
    auto fd = open_udp_socket();
    if (!fd) {
        return Result<UdpEgress>::fail(fd.error);
    }
    UdpEgress out;
    out.fd_ = *fd.value;
    if (::connect(out.fd_, reinterpret_cast<const sockaddr*>(&*sa.value),
                  sizeof(*sa.value)) < 0) {
        return Result<UdpEgress>::fail("connect('" + target + "'): " +
                                       std::strerror(errno));
    }
    return Result<UdpEgress>::ok(std::move(out));
}

bool UdpEgress::send(const uint8_t* data, size_t len) {
    return ::send(fd_, data, len, 0) == static_cast<ssize_t>(len);
}

// ---- BindingSet -----------------------------------------------------------

Result<BindingSet> BindingSet::create(const Config& cfg) {
    BindingSet set;
    for (const StreamCfg& s : cfg.streams) {
        if (s.dir == Dir::kIn) {
            auto in = UdpIngress::open(s.bind.listen);
            if (!in) {
                return Result<BindingSet>::fail(
                    "stream " + std::to_string(s.stream_id) + ": " + in.error);
            }
            set.ins_.push_back(In{s.stream_id, std::move(*in.value)});
        } else {
            auto out = UdpEgress::open(s.bind.send);
            if (!out) {
                return Result<BindingSet>::fail(
                    "stream " + std::to_string(s.stream_id) + ": " + out.error);
            }
            set.outs_.push_back(Out{s.stream_id, std::move(*out.value)});
        }
    }
    if (cfg.stats.bind) {
        auto out = UdpEgress::open(cfg.stats.bind->send);
        if (!out) {
            return Result<BindingSet>::fail("stats: " + out.error);
        }
        set.stats_ = std::move(*out.value);
    }
    return Result<BindingSet>::ok(std::move(set));
}

UdpEgress* BindingSet::egress_for(uint8_t stream_id) {
    for (Out& o : outs_) {
        if (o.stream_id == stream_id) {
            return &o.sock;
        }
    }
    return nullptr;
}

UdpEgress* BindingSet::stats_egress() {
    return stats_ ? &*stats_ : nullptr;
}

uint16_t BindingSet::ingress_port(uint8_t stream_id) const {
    for (const In& i : ins_) {
        if (i.stream_id == stream_id) {
            return i.sock.bound_port();
        }
    }
    return 0;
}

int BindingSet::poll_once(int timeout_ms,
                          const std::function<void(const IngressEvent&)>& cb) {
    if (ins_.empty()) {
        return 0;
    }
    std::vector<pollfd> fds;
    fds.reserve(ins_.size());
    for (const In& i : ins_) {
        fds.push_back(pollfd{i.sock.fd(), POLLIN, 0});
    }
    const int rc = ::poll(fds.data(), fds.size(), timeout_ms);
    if (rc < 0) {
        return errno == EINTR ? 0 : -1;
    }
    int delivered = 0;
    for (size_t idx = 0; idx < ins_.size(); ++idx) {
        if ((fds[idx].revents & POLLIN) == 0) {
            continue;
        }
        // Drain: per-fd receive order is preserved; devourer-style fairness
        // across fds is irrelevant at <=4 ingress sockets.
        for (;;) {
            const long n = ins_[idx].sock.recv_one(buf_.data(), buf_.size());
            if (n <= 0) {
                break;
            }
            cb(IngressEvent{ins_[idx].stream_id, buf_.data(),
                            static_cast<size_t>(n)});
            ++delivered;
        }
    }
    return delivered;
}

}  // namespace wblink
