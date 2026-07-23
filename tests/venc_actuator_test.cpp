// SPDX-License-Identifier: GPL-2.0-or-later
// §9.6 venc actuator, volatile-first (Pass 73): pushes target
// /api/v1/live/set; the first 404 latches a one-shot per-process fallback to
// the persisting /api/v1/set and re-sends the push that drew it. Non-404
// failures must NOT latch the fallback. Driven against a scripted one-shot
// HTTP server on a real loopback socket.
#include "wblink/venc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "wbtest.h"

using namespace wblink;

namespace {

// Serves `statuses` in order, one connection each, recording request paths.
class ScriptedVenc {
  public:
    explicit ScriptedVenc(std::vector<int> statuses)
        : statuses_(std::move(statuses)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        socklen_t l = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &l);
        port_ = ntohs(a.sin_port);
        ::listen(fd_, 4);
        thread_ = std::thread([this] { serve(); });
    }
    ~ScriptedVenc() {
        thread_.join();
        ::close(fd_);
    }
    uint16_t port() const { return port_; }
    const std::vector<std::string>& paths() const { return paths_; }

  private:
    void serve() {
        for (const int status : statuses_) {
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) return;
            char buf[512];
            const ssize_t n = ::recv(c, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                // "GET <path> HTTP/1.0" — capture the path.
                const char* sp1 = std::strchr(buf, ' ');
                const char* sp2 = sp1 ? std::strchr(sp1 + 1, ' ') : nullptr;
                if (sp1 && sp2) paths_.emplace_back(sp1 + 1, sp2);
            }
            const std::string resp = "HTTP/1.0 " + std::to_string(status) +
                                     " X\r\n\r\n{}";
            ::send(c, resp.data(), resp.size(), 0);
            ::close(c);
        }
    }
    int fd_;
    uint16_t port_ = 0;
    std::vector<int> statuses_;
    std::vector<std::string> paths_;
    std::thread thread_;
};

VencCfg cfg_for(uint16_t port) {
    VencCfg cfg;
    cfg.host = "127.0.0.1:" + std::to_string(port);
    cfg.enabled = true;
    return cfg;
}

}  // namespace

int main() {
    // A live-capable venc: one push, one request, on the volatile path.
    {
        ScriptedVenc venc({200});
        VencActuator act(cfg_for(venc.port()));
        CHECK(act.set_bitrate(8192, 1000));
        CHECK(!act.live_fallback());
        CHECK_EQ_U(act.pushes(), 1);
        CHECK_EQ_U(act.failures(), 0);
    }

    // A pre-live venc: 404 + successful /set re-send latches the fallback
    // (no lost actuation); later pushes skip the volatile attempt until the
    // 10-min re-probe, which heals when /live/set starts answering.
    {
        ScriptedVenc venc({404, 200, 200, 200});
        VencActuator act(cfg_for(venc.port()));
        CHECK(act.set_bitrate(8192, 1000));
        CHECK(act.live_fallback());
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 8192);
        CHECK(act.set_fps(60, 2000));
        CHECK(act.live_fallback());
        // Past the re-probe window the volatile route is tried again and a
        // 2xx clears the latch.
        CHECK(act.set_fps(90, 1000 + 600000));
        CHECK(!act.live_fallback());
        const auto& p = venc.paths();
        CHECK_EQ_U(p.size(), 4);
        CHECK(p[0] == "/api/v1/live/set?video0.bitrate=8192");
        CHECK(p[1] == "/api/v1/set?video0.bitrate=8192");
        CHECK(p[2] == "/api/v1/set?video0.fps=60");
        CHECK(p[3] == "/api/v1/live/set?video0.fps=90");
    }

    // A transient 404 (venc bring-up: httpd bound, routes not yet
    // registered — /set 404s too) must NOT latch the fallback.
    {
        ScriptedVenc venc({404, 404, 200});
        VencActuator act(cfg_for(venc.port()));
        CHECK(!act.set_bitrate(8192, 1000));
        CHECK(!act.live_fallback());
        CHECK(act.set_bitrate(8192, 2000));  // past holdoff: volatile again
        CHECK(!act.live_fallback());
        const auto& p = venc.paths();
        CHECK_EQ_U(p.size(), 3);
        CHECK(p[2] == "/api/v1/live/set?video0.bitrate=8192");
    }

    // A non-404 failure on the volatile path does NOT latch the fallback:
    // the retry (after the 500 ms holdoff) goes to /live/set again.
    {
        ScriptedVenc venc({500, 200});
        VencActuator act(cfg_for(venc.port()));
        CHECK(!act.set_bitrate(8192, 1000));
        CHECK(!act.live_fallback());
        CHECK_EQ_U(act.failures(), 1);
        CHECK(act.set_bitrate(8192, 2000));  // past the holdoff
        const auto& p = venc.paths();
        CHECK_EQ_U(p.size(), 2);
        CHECK(p[1] == "/api/v1/live/set?video0.bitrate=8192");
    }

    return wbtest_finish("venc_actuator_test");
}
