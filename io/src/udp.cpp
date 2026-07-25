// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/binding.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace wblink {

namespace {

constexpr int kUdpReceiveBufferBytes = 4 * 1024 * 1024;

// B6: cap datagrams drained per fd per poll pass. The ingress sockets carry no
// source filter (reject_originator defaults off), so an unbounded drain lets any
// host that can reach the RTP port hold the flight loop inside recv_one. A cap
// lets the loop breathe — ready data simply re-fires poll on the next pass.
constexpr int kMaxDrainPerFd = 64;

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
      bound_port_(std::exchange(other.bound_port_, 0)),
      kernel_drop_last_(std::exchange(other.kernel_drop_last_, 0)),
      kernel_drops_(std::exchange(other.kernel_drops_, 0)),
      socket_filtered_(std::exchange(other.socket_filtered_, 0)) {}

UdpIngress& UdpIngress::operator=(UdpIngress&& other) noexcept {
    if (this != &other) {
        close_fd(fd_);
        fd_ = std::exchange(other.fd_, -1);
        bound_port_ = std::exchange(other.bound_port_, 0);
        kernel_drop_last_ = std::exchange(other.kernel_drop_last_, 0);
        kernel_drops_ = std::exchange(other.kernel_drops_, 0);
        socket_filtered_ = std::exchange(other.socket_filtered_, 0);
    }
    return *this;
}

Result<UdpIngress> UdpIngress::open(const std::string& listen,
                                    uint16_t reject_originator) {
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
    // Frame-SHM emits an encoded frame's symbols as a tight burst. Give the
    // UDP-air bench enough queue to model air delivery rather than localhost
    // scheduler jitter; the kernel may clamp this to net.core.rmem_max.
    ::setsockopt(in.fd_, SOL_SOCKET, SO_RCVBUF, &kUdpReceiveBufferBytes,
                 sizeof(kUdpReceiveBufferBytes));
    ::setsockopt(in.fd_, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one));
    if (reject_originator != 0) {
        // Drop our own originator before it can consume receive-queue capacity.
        sock_filter code[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
            BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 8 + 5, 0, 3),
            // Linux presents the UDP header (8 bytes) before datagram payload
            // to a classic filter attached to an AF_INET/SOCK_DGRAM socket.
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 8 + 3),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, reject_originator, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, 0),
            BPF_STMT(BPF_RET | BPF_K, UINT32_MAX),
        };
        sock_fprog prog{};
        prog.len = static_cast<unsigned short>(sizeof(code) / sizeof(code[0]));
        prog.filter = code;
        if (::setsockopt(in.fd_, SOL_SOCKET, SO_ATTACH_FILTER, &prog,
                         sizeof(prog)) != 0) {
            return Result<UdpIngress>::fail(
                "setsockopt(SO_ATTACH_FILTER): " +
                std::string(std::strerror(errno)));
        }
    }
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
    iovec iov{buf, cap};
    alignas(cmsghdr) uint8_t control[CMSG_SPACE(sizeof(uint32_t))]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    const ssize_t n = ::recvmsg(fd_, &msg, 0);
    if (n >= 0) {
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr;
             c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL &&
                c->cmsg_len >= CMSG_LEN(sizeof(uint32_t))) {
                uint32_t count = 0;
                std::memcpy(&count, CMSG_DATA(c), sizeof(count));
                kernel_drops_ += static_cast<uint32_t>(count - kernel_drop_last_);
                kernel_drop_last_ = count;
            }
        }
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

Result<UdpEgress> UdpEgress::open(const std::string& target, bool broadcast) {
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
    if (broadcast) {
        int one = 1;
        if (::setsockopt(out.fd_, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) !=
            0) {
            return Result<UdpEgress>::fail("setsockopt(SO_BROADCAST): " +
                                           std::string(std::strerror(errno)));
        }
    }
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
        // §15.4 frame-shm streams are owned by the app (FrameShmRing), not by
        // the UDP binding layer — skip them here.
        if (s.bind.kind == BindKind::kFrameShm) {
            continue;
        }
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
        // across fds is irrelevant at <=4 ingress sockets. B6: bounded.
        for (int drained = 0; drained < kMaxDrainPerFd; ++drained) {
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

int BindingSet::poll_once(int timeout_ms,
                          const std::function<void(const IngressEvent&)>& cb,
                          const std::vector<int>& extra_fds,
                          const std::function<void(size_t)>& on_extra) {
    // Unified wait over the UDP ingress fds AND the caller's extra fds (the
    // frame-shm consumer eventfds). A frame-shm-only node has no UDP ingress,
    // so the blocking wait must live on the ring's eventfd here.
    std::vector<pollfd> fds;
    fds.reserve(ins_.size() + extra_fds.size());
    for (const In& i : ins_) {
        fds.push_back(pollfd{i.sock.fd(), POLLIN, 0});
    }
    for (const int fd : extra_fds) {
        fds.push_back(pollfd{fd, POLLIN, 0});
    }
    if (fds.empty()) {
        return 0;
    }
    const int rc = ::poll(fds.data(), fds.size(), timeout_ms);
    if (rc < 0) {
        return errno == EINTR ? 0 : -1;
    }
    int delivered = 0;
    // Extra readiness includes the vehicle's return-radio eventfd. Dispatch it
    // before draining ingress sockets so an ARQ request cannot sit behind a
    // burst of live video datagrams.
    for (size_t j = 0; j < extra_fds.size(); ++j) {
        if ((fds[ins_.size() + j].revents & POLLIN) != 0) {
            on_extra(j);
        }
    }
    for (size_t idx = 0; idx < ins_.size(); ++idx) {
        if ((fds[idx].revents & POLLIN) == 0) {
            continue;
        }
        for (int drained = 0; drained < kMaxDrainPerFd; ++drained) {  // B6
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
