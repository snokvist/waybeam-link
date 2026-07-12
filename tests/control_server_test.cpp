// SPDX-License-Identifier: GPL-2.0-or-later
// §15.5 control plane: drive the ControlServer over a real loopback socket in
// one process (bind port 0, discover it, connect a client, interleave
// service() with client I/O). Covers the read surface, the write knobs (200 /
// 400 / 409), unknown-path 404, malformed JSON 400, and the SSE feed.
#include "wblink/control_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "wbtest.h"

using namespace wblink;

namespace {

uint16_t bound_port(int fd) {
    sockaddr_in a{};
    socklen_t l = sizeof(a);
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
    return ntohs(a.sin_port);
}

int connect_client(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    timeval tv{};
    tv.tv_sec = 1;  // never hang the suite if a response is missing
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// One request → full response (connection closed by the server after).
std::string roundtrip(ControlServer& s, uint16_t port, const std::string& req) {
    const int c = connect_client(port);
    CHECK(c >= 0);
    if (c < 0) return "";
    s.service(0);  // accept
    ::send(c, req.data(), req.size(), 0);
    for (int i = 0; i < 6; ++i) s.service(0);  // read + dispatch + respond
    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(c);
    return out;
}

int status_of(const std::string& resp) {
    // "HTTP/1.0 <code> ..."
    const size_t sp = resp.find(' ');
    if (sp == std::string::npos) return -1;
    return std::atoi(resp.c_str() + sp + 1);
}

std::string body_of(const std::string& resp) {
    const size_t p = resp.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : resp.substr(p + 4);
}

}  // namespace

int main() {
    // Captured knob state, mutated by the handlers.
    int pin_min = -1, pin_max = -1;
    int fec_sid = -1, fec_i = -1, fec_p = -1, fec_k = -1;
    bool fec_ok = true;
    int reset_calls = 0;

    auto srv = ControlServer::create("127.0.0.1:0");
    CHECK(static_cast<bool>(srv));
    if (!srv) return wbtest_finish("control_server_test");
    ControlServer& s = **srv.value;
    const uint16_t port = bound_port(s.listen_fd());
    CHECK(port != 0);

    ControlHandlers h;
    h.stats_line = [] { return std::string("{\"t_ms\":1,\"node\":7}"); };
    h.info_json = [] { return std::string("{\"role\":\"tx\"}"); };
    h.health_json = [] { return std::string("{\"state\":\"HOLD\"}"); };
    h.profile = [&](int mn, int mx) -> std::string {
        if (mx != 255 && mn > mx) return "min>max";
        pin_min = mn;
        pin_max = mx;
        return "";
    };
    h.fec = [&](int sid, int ip, int pp, int mk) -> std::string {
        fec_sid = sid;
        fec_i = ip;
        fec_p = pp;
        fec_k = mk;
        return fec_ok ? "" : "no frame-shm stream with that id";
    };
    h.reset_stats = [&] { ++reset_calls; };
    // h.csa intentionally left null → endpoint must 409.
    s.set_handlers(std::move(h));

    // --- reads -----------------------------------------------------------
    {
        const std::string r =
            roundtrip(s, port, "GET /api/v1/stats HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r) == "{\"t_ms\":1,\"node\":7}");
    }
    {
        const std::string r =
            roundtrip(s, port, "GET /api/v1/info HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r).find("\"role\":\"tx\"") != std::string::npos);
    }
    {
        const std::string r =
            roundtrip(s, port, "GET /api/v1/health HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
    }
    // Query string is stripped.
    {
        const std::string r =
            roundtrip(s, port, "GET /api/v1/stats?foo=1 HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
    }
    // Unknown path → 404.
    {
        const std::string r =
            roundtrip(s, port, "GET /nope HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 404);
    }

    // --- writes ----------------------------------------------------------
    // profile pin round-trip.
    {
        const std::string body = "{\"min\":3,\"max\":3}";
        const std::string req =
            "POST /api/v1/link/profile HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        const std::string r = roundtrip(s, port, req);
        CHECK_EQ_U(status_of(r), 200);
        CHECK_EQ_U(pin_min, 3);
        CHECK_EQ_U(pin_max, 3);
    }
    // profile validation error → 400 (min>max), handler ran but rejected.
    {
        const std::string body = "{\"min\":5,\"max\":2}";
        const std::string req =
            "POST /api/v1/link/profile HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        const std::string r = roundtrip(s, port, req);
        CHECK_EQ_U(status_of(r), 400);
    }
    // profile missing field → 400 before the handler.
    {
        const std::string body = "{\"min\":1}";
        const std::string req =
            "POST /api/v1/link/profile HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    // fec round-trip.
    {
        const std::string body =
            "{\"stream_id\":0,\"i_permille\":300,\"p_permille\":120,\"min_k\":4}";
        const std::string req =
            "POST /api/v1/fec HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(fec_sid, 0);
        CHECK_EQ_U(fec_i, 300);
        CHECK_EQ_U(fec_p, 120);
        CHECK_EQ_U(fec_k, 4);
    }
    // fec handler rejects → 400.
    {
        fec_ok = false;
        const std::string body = "{\"stream_id\":9}";
        const std::string req =
            "POST /api/v1/fec HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
        fec_ok = true;
    }
    // reset.
    {
        const std::string req =
            "POST /api/v1/stats/reset HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}";
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(reset_calls, 1);
    }
    // csa with a null handler → 409 (not applicable in this mode).
    {
        const std::string body = "{\"mhz\":5805}";
        const std::string req =
            "POST /api/v1/csa HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 409);
    }
    // malformed JSON → 400.
    {
        const std::string body = "{not json";
        const std::string req =
            "POST /api/v1/stats/reset HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }

    // --- SSE stream ------------------------------------------------------
    {
        const int c = connect_client(port);
        CHECK(c >= 0);
        s.service(0);  // accept
        const std::string req = "GET /api/v1/stats/stream HTTP/1.0\r\n\r\n";
        ::send(c, req.data(), req.size(), 0);
        for (int i = 0; i < 6; ++i) s.service(0);
        char buf[4096];
        ssize_t n = ::recv(c, buf, sizeof(buf), 0);  // headers + primed frame
        std::string first(buf, n > 0 ? static_cast<size_t>(n) : 0);
        CHECK(first.find("text/event-stream") != std::string::npos);
        CHECK(first.find("data: {\"t_ms\":1") != std::string::npos);
        // A new snapshot is pushed to the open stream.
        s.publish_stats("{\"t_ms\":2,\"node\":7}\n");
        n = ::recv(c, buf, sizeof(buf), 0);
        std::string second(buf, n > 0 ? static_cast<size_t>(n) : 0);
        CHECK(second.find("data: {\"t_ms\":2") != std::string::npos);
        ::close(c);
        s.service(0);  // reap the closed stream conn
    }

    return wbtest_finish("control_server_test");
}
