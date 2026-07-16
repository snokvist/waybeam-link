// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/cache_udp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "wblink/binding.h"

namespace wblink {

CacheUdp::~CacheUdp() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

CacheUdp::CacheUdp(CacheUdp&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      bound_port_(std::exchange(other.bound_port_, 0)) {}

CacheUdp& CacheUdp::operator=(CacheUdp&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
        bound_port_ = std::exchange(other.bound_port_, 0);
    }
    return *this;
}

Result<CacheEndpoint> CacheUdp::resolve(const std::string& hostport) {
    auto hp = split_host_port(hostport);
    if (!hp) {
        return Result<CacheEndpoint>::fail(hp.error);
    }
    CacheEndpoint ep;
    in_addr addr{};
    if (::inet_pton(AF_INET, hp.value->first.c_str(), &addr) != 1) {
        return Result<CacheEndpoint>::fail(
            "'" + hp.value->first + "' is not an IPv4 address (v0 is IPv4-only)");
    }
    ep.addr_be = addr.s_addr;
    ep.port_be = htons(hp.value->second);
    return Result<CacheEndpoint>::ok(ep);
}

Result<CacheUdp> CacheUdp::open(const std::string& listen) {
    auto hp = split_host_port(listen);
    if (!hp) {
        return Result<CacheUdp>::fail(hp.error);
    }
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return Result<CacheUdp>::fail(std::string("socket(): ") +
                                      std::strerror(errno));
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        const std::string err =
            std::string("fcntl(O_NONBLOCK): ") + std::strerror(errno);
        ::close(fd);
        return Result<CacheUdp>::fail(err);
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(hp.value->second);
    if (::inet_pton(AF_INET, hp.value->first.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        return Result<CacheUdp>::fail(
            "'" + hp.value->first + "' is not an IPv4 address (v0 is IPv4-only)");
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        const std::string err = "bind('" + listen + "'): " + std::strerror(errno);
        ::close(fd);
        return Result<CacheUdp>::fail(err);
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    CacheUdp out;
    out.fd_ = fd;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
        out.bound_port_ = ntohs(bound.sin_port);
    }
    return Result<CacheUdp>::ok(std::move(out));
}

bool CacheUdp::send_to(const CacheEndpoint& to, const uint8_t* data,
                       size_t len) {
    if (fd_ < 0 || data == nullptr || len == 0) {
        return false;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = to.addr_be;
    sa.sin_port = to.port_be;
    const ssize_t n = ::sendto(fd_, data, len, 0,
                               reinterpret_cast<const sockaddr*>(&sa),
                               sizeof(sa));
    return n == static_cast<ssize_t>(len);
}

long CacheUdp::recv_one(uint8_t* buf, size_t cap, CacheEndpoint* from) {
    if (fd_ < 0 || buf == nullptr || from == nullptr) {
        return -1;
    }
    sockaddr_in sa{};
    socklen_t slen = sizeof(sa);
    const ssize_t n = ::recvfrom(fd_, buf, cap, 0,
                                 reinterpret_cast<sockaddr*>(&sa), &slen);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    from->addr_be = sa.sin_addr.s_addr;
    from->port_be = sa.sin_port;
    return n;
}

}  // namespace wblink
