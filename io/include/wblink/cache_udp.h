// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §14.3 cache-control UDP socket. One unconnected datagram
// socket per cache role: the aggregator sends CACHE_REQUESTs to configured
// cache endpoints and receives replies/status on its listen address; a cache
// node answers requests to their source endpoint and pushes CACHE_STATUS to
// the configured aggregators. Control-plane socket — outside the §15.1
// stream-binding pools.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "wblink/config.h"

namespace wblink {

// IPv4 endpoint in network order — POD so §13 source-endpoint checks are a
// plain comparison and no socket headers leak into consumers.
struct CacheEndpoint {
    uint32_t addr_be = 0;
    uint16_t port_be = 0;
    friend bool operator==(const CacheEndpoint&,
                           const CacheEndpoint&) = default;
};

class CacheUdp {
  public:
    CacheUdp() = default;
    ~CacheUdp();
    CacheUdp(const CacheUdp&) = delete;
    CacheUdp& operator=(const CacheUdp&) = delete;
    CacheUdp(CacheUdp&& other) noexcept;
    CacheUdp& operator=(CacheUdp&& other) noexcept;

    // Binds "host:port" non-blocking; port 0 = ephemeral (tests).
    static Result<CacheUdp> open(const std::string& listen);
    // "host:port" -> endpoint (IPv4 dotted only, matching §15.1 v0).
    static Result<CacheEndpoint> resolve(const std::string& hostport);

    int fd() const { return fd_; }
    uint16_t bound_port() const { return bound_port_; }

    bool send_to(const CacheEndpoint& to, const uint8_t* data, size_t len);
    // One datagram; >0 = bytes, 0 = nothing pending, -1 = error. from is
    // filled on success (never nullptr).
    long recv_one(uint8_t* buf, size_t cap, CacheEndpoint* from);

  private:
    int fd_ = -1;
    uint16_t bound_port_ = 0;
};

}  // namespace wblink
