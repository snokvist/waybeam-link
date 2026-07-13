// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/venc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>

#include "wblink/binding.h"

namespace wblink {

bool VencActuator::http_get(const std::string& path) {
    const auto hp = split_host_port(cfg_.host);
    if (!hp) {
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    timeval tv{};
    tv.tv_usec = 200 * 1000;  // 200 ms connect/send/recv budget
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(hp.value->second);
    if (::inet_pton(AF_INET, hp.value->first.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;  // venc lives at a literal IP (127.0.0.1); no DNS here
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " +
                            cfg_.host + "\r\n\r\n";
    if (::send(fd, req.data(), req.size(), 0) !=
        static_cast<ssize_t>(req.size())) {
        ::close(fd);
        return false;
    }
    char buf[128];
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    // "HTTP/1.x 2xx ..." — anything else is a failure.
    const char* sp = std::strchr(buf, ' ');
    return sp != nullptr && sp[1] == '2';
}

bool VencActuator::set_bitrate(uint32_t kbps, uint64_t now_ms) {
    if (!cfg_.enabled) {
        return true;
    }
    if (last_ && *last_ == kbps) {
        return true;  // §9.6 write-on-change: flash wear
    }
    if (now_ms < no_retry_until_ms_) {
        return false;  // failure hold-off; the caller re-offers next tick
    }
    ++pushes_;
    const bool ok =
        http_get("/api/v1/set?video0.bitrate=" + std::to_string(kbps));
    if (ok) {
        last_ = kbps;
    } else {
        ++failures_;
        no_retry_until_ms_ = now_ms + 500;
        // last_ stays unset/stale so a later tick retries the push.
    }
    return ok;
}

bool VencActuator::request_idr(uint64_t now_ms) {
    if (!cfg_.recovery_enabled) {
        return false;
    }
    if (now_ms < next_idr_ms_) {
        return true;
    }
    next_idr_ms_ = now_ms + 1000;
    ++idr_requests_;
    if (http_get("/request/idr")) {
        return true;
    }
    ++idr_failures_;
    return false;
}

}  // namespace wblink
