// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/venc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wblink/binding.h"

namespace wblink {

int VencActuator::http_get_status(const std::string& path) {
    const auto hp = split_host_port(cfg_.host);
    if (!hp) {
        return 0;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
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
        return 0;  // venc lives at a literal IP (127.0.0.1); no DNS here
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }
    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " +
                            cfg_.host + "\r\n\r\n";
    if (::send(fd, req.data(), req.size(), 0) !=
        static_cast<ssize_t>(req.size())) {
        ::close(fd);
        return 0;
    }
    char buf[128];
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    // "HTTP/1.x <code> ..." — return the code (0 on a garbled line).
    const char* sp = std::strchr(buf, ' ');
    if (sp == nullptr || sp[1] < '1' || sp[1] > '9') {
        return 0;
    }
    return std::atoi(sp + 1);
}

bool VencActuator::http_get(const std::string& path) {
    const int status = http_get_status(path);
    return status >= 200 && status < 300;
}

// §9.6 volatile-first (Pass 73): try /api/v1/live/set; a 404 (pre-live venc)
// latches the persisting-/set fallback for the process lifetime and re-sends
// the push that drew it, so no actuation is lost.
bool VencActuator::push_set(const std::string& query) {
    if (!live_fallback_) {
        const int status = http_get_status("/api/v1/live/set?" + query);
        if (status != 404) {
            return status >= 200 && status < 300;
        }
        live_fallback_ = true;
        std::fprintf(stderr,
                     "venc: /api/v1/live/set unsupported (pre-live venc) — "
                     "falling back to persisting /api/v1/set\n");
    }
    return http_get("/api/v1/set?" + query);
}

bool VencActuator::set_bitrate(uint32_t kbps, uint64_t now_ms) {
    if (!cfg_.enabled) {
        return true;
    }
    if (last_ && *last_ == kbps) {
        return true;  // §9.6 write-on-change
    }
    if (now_ms < no_retry_until_ms_) {
        return false;  // failure hold-off; the caller re-offers next tick
    }
    ++pushes_;
    const bool ok = push_set("video0.bitrate=" + std::to_string(kbps));
    if (ok) {
        last_ = kbps;
        last_change_ms_ = now_ms;  // §9.6 settling window anchor
    } else {
        ++failures_;
        no_retry_until_ms_ = now_ms + 500;
        // last_ stays unset/stale so a later tick retries the push.
    }
    return ok;
}

bool VencActuator::set_max_frame_size(uint32_t max_i_bytes,
                                      uint32_t max_p_bytes, uint64_t now_ms) {
    if (!cfg_.enabled || !cfg_.frame_caps) {
        return true;
    }
    if (max_i_bytes == 0 && max_p_bytes == 0) {
        return true;  // §9.6: insufficient cap inputs — leave venc alone
    }
    if (last_caps_ && last_caps_->first == max_i_bytes &&
        last_caps_->second == max_p_bytes) {
        return true;  // §9.6 write-on-change
    }
    if (now_ms < no_retry_until_ms_) {
        return false;
    }
    ++pushes_;
    const bool ok = push_set("video0.maxIBytes=" + std::to_string(max_i_bytes) +
                             "&video0.maxPBytes=" + std::to_string(max_p_bytes));
    if (ok) {
        last_caps_ = {max_i_bytes, max_p_bytes};
        last_change_ms_ = now_ms;
    } else {
        ++failures_;
        no_retry_until_ms_ = now_ms + 500;
    }
    return ok;
}

bool VencActuator::set_fps(uint16_t fps, uint64_t now_ms) {
    if (!cfg_.enabled || fps == 0) {
        return true;
    }
    if (last_fps_ && *last_fps_ == fps) {
        return true;  // §9.6 write-on-change
    }
    if (now_ms < no_retry_until_ms_) {
        return false;
    }
    ++pushes_;
    const bool ok = push_set("video0.fps=" + std::to_string(fps));
    if (ok) {
        last_fps_ = fps;
        last_change_ms_ = now_ms;
    } else {
        ++failures_;
        no_retry_until_ms_ = now_ms + 500;
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
