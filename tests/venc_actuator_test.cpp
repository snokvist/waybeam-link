// SPDX-License-Identifier: GPL-2.0-or-later
// §9.6 venc actuator, volatile-first (Pass 73): pushes target
// /api/v1/live/set; the first 404 latches a one-shot per-process fallback to
// the persisting /api/v1/set and re-sends the push that drew it. Non-404
// failures must NOT latch the fallback. B1: the actuator is non-blocking —
// setters record the desired value and poll() drives the HTTP; the harness
// pumps poll() to quiescence at a fixed clock. Driven against a scripted
// one-shot HTTP server on a real loopback socket.
#include "wblink/venc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
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

// Drive the non-blocking state machine to quiescence at a fixed clock: keep
// polling until an idle actuator stays idle across a poll (nothing pending to
// start — the 404 fallback chain restarts within one poll, so this drains it).
// While a transaction is in flight, YIELD real wall-time between polls: poll()
// uses a 0 ms timeout, so a tight spin can exhaust its budget before the kernel
// finishes the loopback connect or the scripted server accepts/answers, leaving
// the transaction wedged mid-flight (and then ScriptedVenc's join() deadlocks
// on the accept it never got). The production loop has real work between polls;
// the sleep restores that here. Bounded at ~5 s so a genuinely stuck actuator
// still fails fast rather than hanging.
void pump(VencActuator& act, uint64_t now_ms) {
    for (int i = 0; i < 100000; ++i) {
        const bool was_busy = act.busy();
        act.poll(now_ms);
        if (!was_busy && !act.busy()) {
            return;
        }
        if (act.busy()) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

}  // namespace

int main() {
    // A live-capable venc: one push on the volatile path.
    {
        ScriptedVenc venc({200});
        VencActuator act(cfg_for(venc.port()));
        act.set_bitrate(8192);
        pump(act, 1000);
        CHECK(!act.live_fallback());
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 8192);
        CHECK_EQ_U(act.pushes(), 1);
        CHECK_EQ_U(act.failures(), 0);
    }

    // A pre-live venc: 404 + successful /set re-send latches the fallback
    // (no lost actuation); later pushes skip the volatile attempt until the
    // 10-min re-probe, which heals when /live/set starts answering.
    {
        ScriptedVenc venc({404, 200, 200, 200});
        VencActuator act(cfg_for(venc.port()));
        act.set_bitrate(8192);
        pump(act, 1000);
        CHECK(act.live_fallback());
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 8192);
        CHECK_EQ_U(act.pushes(), 1);  // one logical push, even with the 404 chain
        act.set_fps(60);
        pump(act, 2000);
        CHECK(act.live_fallback());
        // Past the re-probe window the volatile route is tried again and a
        // 2xx clears the latch.
        act.set_fps(90);
        pump(act, 1000 + 600000);
        CHECK(!act.live_fallback());
        const auto& p = venc.paths();
        CHECK_EQ_U(p.size(), 4);
        CHECK(p[0] == "/api/v1/live/set?video0.bitrate=8192");
        CHECK(p[1] == "/api/v1/set?video0.bitrate=8192");
        CHECK(p[2] == "/api/v1/set?video0.fps=60");
        CHECK(p[3] == "/api/v1/live/set?video0.fps=90");
    }

    // A transient 404 (venc bring-up: httpd bound, routes not yet
    // registered — /set 404s too) must NOT latch the fallback, and must not
    // commit the value.
    {
        ScriptedVenc venc({404, 404, 200});
        VencActuator act(cfg_for(venc.port()));
        act.set_bitrate(8192);
        pump(act, 1000);
        CHECK(!act.live_fallback());
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 0);  // both failed: uncommitted
        CHECK_EQ_U(act.failures(), 1);
        act.set_bitrate(8192);
        pump(act, 2000);  // past holdoff: volatile again
        CHECK(!act.live_fallback());
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 8192);
        const auto& p = venc.paths();
        CHECK_EQ_U(p.size(), 3);
        CHECK(p[2] == "/api/v1/live/set?video0.bitrate=8192");
    }

    // A non-404 failure on the volatile path does NOT latch the fallback:
    // the retry (after the 500 ms holdoff) goes to /live/set again.
    {
        ScriptedVenc venc({500, 200});
        VencActuator act(cfg_for(venc.port()));
        act.set_bitrate(8192);
        pump(act, 1000);
        CHECK(!act.live_fallback());
        CHECK_EQ_U(act.failures(), 1);
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 0);
        act.set_bitrate(8192);
        pump(act, 2000);  // past the holdoff
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 8192);
        const auto& p = venc.paths();
        CHECK_EQ_U(p.size(), 2);
        CHECK(p[1] == "/api/v1/live/set?video0.bitrate=8192");
    }

    // request_idr: queued only when recovery is enabled and outside the 1 s
    // rate gate; the send happens in poll(). A 2xx counts no failure.
    {
        ScriptedVenc venc({200});
        VencCfg c = cfg_for(venc.port());
        c.recovery_enabled = true;
        VencActuator act(c);
        CHECK(act.request_idr(1000));  // queued
        pump(act, 1000);               // sends; arms the 1 s rate gate
        CHECK_EQ_U(act.idr_requests(), 1);
        CHECK_EQ_U(act.idr_failures(), 0);
        CHECK(act.request_idr(1200));  // inside the gate: true, not re-queued
        pump(act, 1200);
        CHECK_EQ_U(act.idr_requests(), 1);  // no second send
        const auto& p = venc.paths();
        CHECK_EQ_U(p.size(), 1);
        CHECK(p[0] == "/request/idr");
    }

    // §15.5 Pass 103 invalidate(): a venc restart strands the encoder because
    // write-on-change suppresses re-pushing the SAME value. invalidate() drops
    // the cache so the identical target is re-asserted onto the fresh encoder.
    {
        ScriptedVenc venc({200, 200});  // first push, then the re-assert
        VencActuator act(cfg_for(venc.port()));
        act.set_bitrate(8192);
        pump(act, 1000);
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 8192);
        CHECK_EQ_U(act.pushes(), 1);
        // Same value again = write-on-change no-op: nothing re-pushed. This is
        // exactly what strands venc after a restart.
        act.set_bitrate(8192);
        pump(act, 1100);
        CHECK_EQ_U(act.pushes(), 1);
        // The restart happened: invalidate, then re-offer the identical value.
        act.invalidate();
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 0);  // cache dropped
        act.set_bitrate(8192);
        pump(act, 1200);
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 8192);  // re-asserted
        CHECK_EQ_U(act.pushes(), 2);
        const auto& p = venc.paths();
        CHECK_EQ_U(p.size(), 2);
        CHECK(p[0] == "/api/v1/live/set?video0.bitrate=8192");
        CHECK(p[1] == "/api/v1/live/set?video0.bitrate=8192");
    }

    // invalidate() also re-probes the volatile path: a fallback latched before
    // the restart must not skip /live/set on the fresh (possibly upgraded)
    // encoder.
    {
        ScriptedVenc venc({404, 200, 200});
        VencActuator act(cfg_for(venc.port()));
        act.set_bitrate(8192);
        pump(act, 1000);
        CHECK(act.live_fallback());
        act.invalidate();
        CHECK(!act.live_fallback());  // latch cleared
        act.set_bitrate(8192);
        pump(act, 2000);
        CHECK_EQ_U(act.commanded_bitrate_kbps(), 8192);
        const auto& p = venc.paths();
        CHECK_EQ_U(p.size(), 3);
        CHECK(p[2] == "/api/v1/live/set?video0.bitrate=8192");  // volatile retried
    }

    return wbtest_finish("venc_actuator_test");
}
