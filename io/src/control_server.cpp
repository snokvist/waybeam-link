// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/control_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

#include <nlohmann/json.hpp>

#include "wblink/binding.h"  // split_host_port

namespace wblink {

namespace {
constexpr size_t kMaxConns = 16;
constexpr size_t kMaxRequest = 8192;      // header+body cap; oversize → 400
constexpr uint64_t kRequestTimeoutMs = 2000;  // slow-client drop
using json = nlohmann::json;

void set_nonblock(int fd) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

// Best-effort full write (small payloads); returns false on hard error.
bool send_all(int fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (w > 0) {
            off += static_cast<size_t>(w);
            continue;
        }
        if (w < 0 && (errno == EINTR)) {
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;  // drop the tail rather than block the flight loop
        }
        return false;
    }
    return true;
}

std::string http_response(int code, const char* reason, const char* ctype,
                          const std::string& body) {
    std::string out = "HTTP/1.0 ";
    out += std::to_string(code);
    out += ' ';
    out += reason;
    out += "\r\nContent-Type: ";
    out += ctype;
    out += "\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    out += body;
    return out;
}

std::string json_ok(const std::string& extra = std::string()) {
    return extra.empty() ? std::string("{\"ok\":true}")
                         : "{\"ok\":true," + extra + "}";
}

std::string json_err(const std::string& msg) {
    json j;
    j["ok"] = false;
    j["error"] = msg;
    return j.dump();
}

// Case-insensitive header value lookup in a raw request (headers only).
long content_length(const std::string& head) {
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const size_t p = lower.find("content-length:");
    if (p == std::string::npos) {
        return 0;
    }
    return std::strtol(head.c_str() + p + 15, nullptr, 10);
}
}  // namespace

Result<std::unique_ptr<ControlServer>> ControlServer::create(
    const std::string& bind) {
    using R = Result<std::unique_ptr<ControlServer>>;
    const auto hp = split_host_port(bind);
    if (!hp) {
        return R::fail("control.bind: " + hp.error);
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return R::fail(std::string("control socket: ") + std::strerror(errno));
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(hp.value->second);
    if (::inet_pton(AF_INET, hp.value->first.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return R::fail("control.bind: bad address '" + hp.value->first + "'");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string e = std::strerror(errno);
        ::close(fd);
        return R::fail("control bind " + bind + ": " + e);
    }
    if (::listen(fd, 8) != 0) {
        const std::string e = std::strerror(errno);
        ::close(fd);
        return R::fail("control listen: " + e);
    }
    set_nonblock(fd);
    std::unique_ptr<ControlServer> s(new ControlServer());
    s->listen_fd_ = fd;
    return R::ok(std::move(s));
}

ControlServer::~ControlServer() {
    for (Conn& c : conns_) {
        if (c.fd >= 0) {
            ::close(c.fd);
        }
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
}

void ControlServer::accept_new(uint64_t now_ms) {
    for (;;) {
        const int cfd = ::accept(listen_fd_, nullptr, nullptr);
        if (cfd < 0) {
            break;  // EAGAIN / no more pending
        }
        if (conns_.size() >= kMaxConns) {
            ::close(cfd);  // shed load rather than grow unbounded
            continue;
        }
        set_nonblock(cfd);
        Conn c;
        c.fd = cfd;
        c.opened_ms = now_ms;
        conns_.push_back(std::move(c));
    }
}

void ControlServer::service(uint64_t now_ms) {
    std::vector<pollfd> fds;
    fds.reserve(conns_.size() + 1);
    fds.push_back(pollfd{listen_fd_, POLLIN, 0});
    for (const Conn& c : conns_) {
        fds.push_back(pollfd{c.fd, POLLIN, 0});
    }
    if (::poll(fds.data(), fds.size(), 0) < 0) {
        if (errno != EINTR) {
            return;
        }
    }
    if (fds[0].revents & POLLIN) {
        accept_new(now_ms);
    }
    // Index alignment holds only for the pre-existing conns (accept_new appended
    // new ones at the end, which we simply service next tick).
    for (size_t i = 0; i < conns_.size(); ++i) {
        Conn& c = conns_[i];
        const bool ready = (i + 1 < fds.size()) &&
                           (fds[i + 1].fd == c.fd) &&
                           (fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR));
        if (ready) {
            service_conn(c, now_ms);
        } else if (!c.streaming &&
                   now_ms - c.opened_ms > kRequestTimeoutMs) {
            ::close(c.fd);
            c.fd = -1;  // slow client: drop
        }
    }
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [](const Conn& c) { return c.fd < 0; }),
                 conns_.end());
}

void ControlServer::service_conn(Conn& c, uint64_t now_ms) {
    (void)now_ms;
    char buf[4096];
    const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
    if (n == 0) {
        ::close(c.fd);
        c.fd = -1;  // peer closed
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return;
        }
        ::close(c.fd);
        c.fd = -1;
        return;
    }
    if (c.streaming) {
        return;  // SSE feed: ignore client chatter, keep pushing
    }
    c.inbuf.append(buf, static_cast<size_t>(n));
    if (c.inbuf.size() > kMaxRequest) {
        const std::string r =
            http_response(400, "Bad Request", "application/json",
                          json_err("request too large"));
        send_all(c.fd, r.data(), r.size());
        ::close(c.fd);
        c.fd = -1;
        return;
    }
    const size_t hdr_end = c.inbuf.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        return;  // headers incomplete — wait for more
    }
    const std::string head = c.inbuf.substr(0, hdr_end);
    const std::string body_have = c.inbuf.substr(hdr_end + 4);
    const long clen = content_length(head);
    if (clen > 0 && body_have.size() < static_cast<size_t>(clen)) {
        return;  // body incomplete — wait for more
    }
    // Request line: METHOD SP PATH SP HTTP/x
    const size_t sp1 = head.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? std::string::npos
                                                : head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        const std::string r = http_response(400, "Bad Request",
                                            "application/json",
                                            json_err("malformed request line"));
        send_all(c.fd, r.data(), r.size());
        ::close(c.fd);
        c.fd = -1;
        return;
    }
    const std::string method = head.substr(0, sp1);
    std::string path = head.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = path.find('?');
    if (q != std::string::npos) {
        path.resize(q);  // drop the query string
    }
    const std::string body =
        clen > 0 ? body_have.substr(0, static_cast<size_t>(clen))
                 : std::string();
    dispatch(c, method, path, body);
}

void ControlServer::dispatch(Conn& c, const std::string& method,
                             const std::string& path, const std::string& body) {
    auto reply = [&](int code, const char* reason, const std::string& jbody) {
        const std::string r =
            http_response(code, reason, "application/json", jbody);
        send_all(c.fd, r.data(), r.size());
        ::close(c.fd);
        c.fd = -1;
    };

    // ---- reads ----------------------------------------------------------
    if (method == "GET") {
        if (path == "/api/v1/stats") {
            if (!h_.stats_line) {
                return reply(503, "Service Unavailable",
                             json_err("no stats yet"));
            }
            const std::string s = h_.stats_line();
            if (s.empty()) {
                return reply(503, "Service Unavailable",
                             json_err("no stats yet"));
            }
            return reply(200, "OK", s);
        }
        if (path == "/api/v1/info") {
            return reply(200, "OK", h_.info_json ? h_.info_json() : "{}");
        }
        if (path == "/api/v1/health") {
            return reply(200, "OK", h_.health_json ? h_.health_json() : "{}");
        }
        if (path == "/api/v1/discovery") {
            return reply(200, "OK",
                         h_.discovery_json ? h_.discovery_json()
                                           : "{\"nodes\":[],\"streams\":[]}");
        }
        if (path == "/api/v1/scout/results") {
            if (!h_.scout_results) {
                return reply(409, "Conflict",
                             json_err("scout not available in this mode"));
            }
            return reply(200, "OK", h_.scout_results());
        }
        if (path == "/api/v1/link/selection") {
            if (!h_.selection_json) {
                return reply(409, "Conflict",
                             json_err("selection not available in this mode"));
            }
            return reply(200, "OK", h_.selection_json());
        }
        if (path == "/api/v1/cache/assignment") {
            if (!h_.cache_assignment_json) {
                return reply(409, "Conflict",
                             json_err("cache assignment not available"));
            }
            return reply(200, "OK", h_.cache_assignment_json());
        }
        if (path == "/api/v1/vehicle/command") {
            if (!h_.vehicle_command_json) {
                return reply(409, "Conflict",
                             json_err("vehicle command not available in this "
                                      "mode"));
            }
            return reply(200, "OK", h_.vehicle_command_json());
        }
        if (path == "/api/v1/mode") {
            if (!h_.mode_get) {
                return reply(409, "Conflict",
                             json_err("mode selection not available in this "
                                      "mode"));
            }
            return reply(200, "OK", h_.mode_get());
        }
        if (path == "/api/v1/modes") {  // §15.5 Pass 104
            if (!h_.modes_list) {
                return reply(409, "Conflict",
                             json_err("mode selection not available in this "
                                      "mode"));
            }
            return reply(200, "OK", h_.modes_list());
        }
        if (path == "/api/v1/stats/stream") {
            const std::string hdr =
                "HTTP/1.0 200 OK\r\nContent-Type: text/event-stream\r\n"
                "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n";
            if (!send_all(c.fd, hdr.data(), hdr.size())) {
                ::close(c.fd);
                c.fd = -1;
                return;
            }
            c.streaming = true;  // held open; fed by publish_stats()
            c.inbuf.clear();
            // Prime with the latest snapshot so a fresh subscriber isn't blank.
            if (h_.stats_line) {
                publish_one(c, h_.stats_line());
            }
            return;
        }
        return reply(404, "Not Found", json_err("unknown path"));
    }

    // ---- writes ---------------------------------------------------------
    if (method != "POST") {
        return reply(405, "Method Not Allowed", json_err("use GET or POST"));
    }
    json j = json::object();
    if (!body.empty()) {
        j = json::parse(body, nullptr, false);
        if (j.is_discarded()) {
            return reply(400, "Bad Request", json_err("invalid JSON body"));
        }
    }
    // A knob absent from this mode's handler table yields 409; a present hook
    // that rejects the request yields 400 with its error string.
    auto na = [&] {
        return reply(409, "Conflict",
                     json_err("endpoint not available in this mode"));
    };
    auto done = [&](const std::string& err) {
        return err.empty() ? reply(200, "OK", json_ok())
                           : reply(400, "Bad Request", json_err(err));
    };

    if (path == "/api/v1/csa") {
        if (!h_.csa) return na();
        const uint32_t mhz = j.value("mhz", 0u);
        const uint32_t klass = j.value("class", 0u);
        if (mhz == 0) {
            return reply(400, "Bad Request", json_err("mhz required"));
        }
        const auto [code, jbody] = h_.csa(mhz, klass);
        return reply(code,
                     code == 200 ? "OK"
                                 : (code == 409 ? "Conflict" : "Bad Request"),
                     jbody);
    }
    if (path == "/api/v1/scout/start") {
        if (!h_.scout_start) return na();
        std::vector<uint16_t> channels;
        if (j.contains("channels") && j["channels"].is_array()) {
            for (const auto& e : j["channels"]) {
                if (e.is_number_unsigned()) {
                    channels.push_back(static_cast<uint16_t>(
                        e.get<unsigned>()));
                }
            }
        }
        const uint32_t dwell = j.value("dwell_ms", 0u);
        const std::string mode = j.value("mode", std::string("list"));
        int target = -1;
        if (j.contains("target") && j["target"].is_object()) {
            target = j["target"].value("originator", -1);
        }
        return done(h_.scout_start(channels, dwell, mode, target));
    }
    if (path == "/api/v1/scout/stop") {
        if (!h_.scout_stop) return na();
        return done(h_.scout_stop());
    }
    if (path == "/api/v1/scout/quickconnect") {
        if (!h_.scout_quickconnect) return na();
        if (!j.contains("originator")) {
            return reply(400, "Bad Request", json_err("originator required"));
        }
        return done(h_.scout_quickconnect(j.value("originator", -1),
                                          j.value("target_chan", 0)));
    }
    if (path == "/api/v1/link/profile") {
        if (!h_.profile) return na();
        if (!j.contains("min") || !j.contains("max")) {
            return reply(400, "Bad Request", json_err("min and max required"));
        }
        return done(h_.profile(j.value("min", 0), j.value("max", 255)));
    }
    if (path == "/api/v1/fec") {
        if (!h_.fec) return na();
        if (!j.contains("stream_id")) {
            return reply(400, "Bad Request", json_err("stream_id required"));
        }
        return done(h_.fec(j.value("stream_id", 0), j.value("i_permille", 250),
                           j.value("p_permille", 100), j.value("min_k", 3),
                           j.value("min_r", 2)));
    }
    if (path == "/api/v1/stats/reset") {
        if (!h_.reset_stats) return na();
        h_.reset_stats();
        return done("");
    }
    if (path == "/api/v1/venc/reassert") {  // §15.5 Pass 103
        if (!h_.venc_reassert) return na();
        h_.venc_reassert();
        return done("");
    }
    if (path == "/api/v1/video/recover") {
        if (!h_.video_recover) return na();
        return done(h_.video_recover(j.value("stream_id", -1)));
    }
    if (path == "/api/v1/bench/rx-drop") {
        if (!h_.bench_rx_drop) return na();
        if (!j.contains("permille")) {
            return reply(400, "Bad Request", json_err("permille required"));
        }
        return done(h_.bench_rx_drop(j.value("permille", -1)));
    }
    if (path == "/api/v1/vehicle/command") {
        if (!h_.vehicle_command) return na();
        const std::string cmd = j.value("cmd", std::string());
        if (cmd.empty() || !j.contains("arg")) {
            return reply(400, "Bad Request", json_err("cmd and arg required"));
        }
        const auto [code, jbody] = h_.vehicle_command(cmd, j.value("arg", -1));
        return reply(code,
                     code == 200 ? "OK"
                                 : (code == 409 ? "Conflict" : "Bad Request"),
                     jbody);
    }
    if (path == "/api/v1/arq") {
        if (!h_.arq_enable) return na();
        if (!j.contains("enabled") || !j["enabled"].is_boolean()) {
            return reply(400, "Bad Request",
                         json_err("enabled (bool) required"));
        }
        return done(h_.arq_enable(j.value("enabled", true)));
    }
    if (path == "/api/v1/link/fps") {
        if (!h_.link_fps) return na();
        if (!j.contains("ladder") || !j["ladder"].is_boolean()) {
            return reply(400, "Bad Request",
                         json_err("ladder (bool) required"));
        }
        return done(h_.link_fps(j.value("ladder", false)));
    }
    if (path == "/api/v1/mode") {
        if (!h_.mode_set) return na();
        const std::string name = j.value("name", std::string());
        if (name.empty()) {
            return reply(400, "Bad Request", json_err("name required"));
        }
        return done(h_.mode_set(name));
    }
    return reply(404, "Not Found", json_err("unknown path"));
}

void ControlServer::publish_stats(const std::string& line) {
    bool any_dead = false;
    for (Conn& c : conns_) {
        if (c.fd >= 0 && c.streaming) {
            publish_one(c, line);
            if (c.fd < 0) {
                any_dead = true;
            }
        }
    }
    if (any_dead) {
        conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                    [](const Conn& c) { return c.fd < 0; }),
                     conns_.end());
    }
}

void ControlServer::publish_one(Conn& c, const std::string& line) {
    std::string payload = line;
    while (!payload.empty() &&
           (payload.back() == '\n' || payload.back() == '\r')) {
        payload.pop_back();
    }
    std::string frame = "data: ";
    frame += payload;
    frame += "\n\n";
    if (!send_all(c.fd, frame.data(), frame.size())) {
        ::close(c.fd);
        c.fd = -1;
    }
}

}  // namespace wblink
