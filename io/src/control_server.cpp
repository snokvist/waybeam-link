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
    // Pass 178: the endpoint an embedder is told to talk to comes from the
    // socket, not from the config string that asked for it — `host:0` is
    // legal and binds an ephemeral port, so echoing the request back would
    // hand out an address nothing answers. If the read-back fails the
    // endpoint stays EMPTY rather than falling back to the request: the
    // ephemeral case is exactly where the request is a lie ("0.0.0.0:0"),
    // and an empty endpoint publishes nothing, so the C-ABI getter answers
    // "no endpoint" instead of a plausible dead address. Fail closed.
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &alen) == 0 &&
        actual.sin_family == AF_INET) {
        char host[INET_ADDRSTRLEN] = {0};
        if (::inet_ntop(AF_INET, &actual.sin_addr, host, sizeof(host)) !=
            nullptr) {
            s->endpoint_ = std::string(host) + ":" +
                           std::to_string(ntohs(actual.sin_port));
        }
    }
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
        if (path == "/api/v1/link/profile") {
            if (!h_.profile_json) {
                return reply(409, "Conflict",
                             json_err("profile envelope not available in this mode"));
            }
            return reply(200, "OK", h_.profile_json());
        }
        if (path == "/api/v1/bench/rx-drop") {
            if (!h_.bench_rx_drop_json) {
                return reply(409, "Conflict",
                             json_err("RX drop not available in this mode"));
            }
            return reply(200, "OK", h_.bench_rx_drop_json());
        }
        if (path == "/api/v1/tx/power") {  // §10.5 Pass 114
            if (!h_.tx_power_json) {
                return reply(409, "Conflict",
                             json_err("tx power not available in this mode"));
            }
            return reply(200, "OK", h_.tx_power_json());
        }
        if (path == "/api/v1/tx/power_tier") {  // §10.3/§11.7 0x0A Pass 135
            if (!h_.tx_power_tier_json) {
                return reply(409, "Conflict",
                             json_err("no power preset list configured "
                                      "(adapters[].power_presets_qdb)"));
            }
            return reply(200, "OK", h_.tx_power_tier_json());
        }
        if (path == "/api/v1/calibration") {  // §10.6 Pass 120
            if (!h_.calibration_json) {
                return reply(409, "Conflict",
                             json_err("calibration not available in this mode"));
            }
            return reply(200, "OK", h_.calibration_json());
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
        if (path == "/api/v1/link/mtu") {
            if (!h_.link_mtu_json) {
                return reply(409, "Conflict",
                             json_err("MTU negotiation not available in this mode"));
            }
            return reply(200, "OK", h_.link_mtu_json());
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
        if (!j.contains("min") || !j["min"].is_number_integer() ||
            !j.contains("max") || !j["max"].is_number_integer()) {
            return reply(400, "Bad Request",
                         json_err("min and max integers required"));
        }
        const int64_t mn = j["min"].get<int64_t>();
        const int64_t mx = j["max"].get<int64_t>();
        if (mn < 0 || mn > 255 || mx < 0 || mx > 255) {
            return reply(400, "Bad Request",
                         json_err("min/max must be 0..255"));
        }
        return done(h_.profile(static_cast<int>(mn), static_cast<int>(mx)));
    }
    if (path == "/api/v1/tx/power") {  // §10.5 Pass 114
        if (!h_.tx_power_set) return na();
        const bool has_qdb = j.contains("qdb") && j["qdb"].is_number_integer();
        const bool has_auto = j.contains("auto") && j["auto"].is_boolean() &&
                              j["auto"].get<bool>();
        if (has_qdb == has_auto) {
            return reply(400, "Bad Request",
                         json_err("exactly one of qdb (int) / auto:true"));
        }
        // §10.5 wire-range, checked on the wide type BEFORE narrowing — a
        // 64-bit JSON value must 400 here, not wrap into -511..511.
        const int64_t qdb = has_qdb ? j["qdb"].get<int64_t>() : 0;
        if (qdb < -511 || qdb > 511) {
            return reply(400, "Bad Request",
                         json_err("qdb out of range (-511..511)"));
        }
        return done(h_.tx_power_set(has_auto, static_cast<int>(qdb)));
    }
    if (path == "/api/v1/tx/power_tier") {  // §10.3/§11.7 0x0A Pass 135
        if (!h_.tx_power_tier_set) return na();
        if (!j.contains("tier") || !j["tier"].is_number_integer()) {
            return reply(400, "Bad Request",
                         json_err("tier (int) required"));
        }
        // Checked wide before narrowing, same rule as tx/power's qdb: a
        // 64-bit JSON value must 400 here rather than wrap into an index.
        const int64_t tier = j["tier"].get<int64_t>();
        if (tier < 0 || tier > kVcmdMaxArg) {
            return reply(400, "Bad Request",
                         json_err("tier out of §11.7 preset range (0..4)"));
        }
        const auto [code, jbody] = h_.tx_power_tier_set(
            static_cast<int>(tier), j.value("both", false));
        return reply(code,
                     code == 200 ? "OK"
                                 : (code == 409 ? "Conflict" : "Bad Request"),
                     jbody);
    }
    if (path == "/api/v1/calibration") {  // §10.7 Pass 125 (ground/rx node)
        if (!h_.uplink_calibrate) return na();
        // §10.7: exactly {"action":"start"} or {"action":"abort"}. A missing
        // or non-string action is malformed (400); an action the node cannot
        // honour right now is a conflict (409, via done()).
        static const char* kActionErr =
            "action must be \"start\", \"start_both\" or \"abort\"";
        if (!j.contains("action") || !j["action"].is_string()) {
            return reply(400, "Bad Request", json_err(kActionErr));
        }
        const std::string action = j["action"].get<std::string>();
        if (action != "start" && action != "start_both" && action != "abort") {
            return reply(400, "Bad Request", json_err(kActionErr));
        }
        // NOT done(): that maps any refusal to 400. §10.7 distinguishes a
        // malformed body (400, above) from a well-formed request the node
        // cannot honour right now (409) — the prerequisite list is the whole
        // point, and "bad request" would misdescribe every entry in it.
        const std::string err = h_.uplink_calibrate(action);
        return err.empty() ? reply(200, "OK", json_ok())
                           : reply(409, "Conflict", json_err(err));
    }
    if (path == "/api/v1/fec") {
        if (!h_.fec) return na();
        if (!j.contains("stream_id")) {
            return reply(400, "Bad Request", json_err("stream_id required"));
        }
        // §14.1a: absent or null => nullopt (inherit p_permille). Type- and
        // range-guard before get<>: an out-of-range or non-integer value must
        // be a 400, never a throw unwinding out of the server.
        std::optional<uint16_t> e_permille;
        if (j.contains("e_permille") && !j.at("e_permille").is_null()) {
            if (!j.at("e_permille").is_number_integer()) {
                return reply(400, "Bad Request",
                             json_err("e_permille must be an integer or null"));
            }
            const int64_t e = j.at("e_permille").get<int64_t>();
            if (e < 0 || e > 4000) {
                return reply(400, "Bad Request",
                             json_err("e_permille must be 0..4000"));
            }
            e_permille = static_cast<uint16_t>(e);
        }
        return done(h_.fec(j.value("stream_id", 0), j.value("i_permille", 250),
                           j.value("p_permille", 100), j.value("min_k", 3),
                           j.value("min_r", 2), e_permille));
    }
    if (path == "/api/v1/stats/reset") {
        if (!h_.reset_stats) return na();
        h_.reset_stats();
        return done("");
    }
    if (path == "/api/v1/reports/latch") {  // §15.5 Pass 115
        if (!h_.reports_latch) return na();
        const bool has_clear = j.contains("clear");
        const bool has_orig = j.contains("originator");
        // Exactly one: a request carrying both has no single meaning, and one
        // carrying neither would silently no-op.
        if (has_clear == has_orig) {
            return reply(400, "Bad Request",
                         json_err("exactly one of clear/originator required"));
        }
        if (has_clear && !j["clear"].is_boolean()) {
            return reply(400, "Bad Request", json_err("clear must be bool"));
        }
        // {"clear":false} has no meaning here — the release is the only thing
        // `clear` names, so reject it rather than falling through to the
        // originator path and reporting a confusing range error.
        if (has_clear && !j.value("clear", false)) {
            return reply(400, "Bad Request", json_err("clear must be true"));
        }
        // Type-guard before any get<>: a string or null "originator" throws
        // json::type_error, and nothing above this server catches — the throw
        // would unwind out of main and take the link down over one bad POST.
        if (has_orig && !j["originator"].is_number_integer()) {
            return reply(400, "Bad Request",
                         json_err("originator must be an integer"));
        }
        // Range on the WIDE type before narrowing, same reason as §10.5 above:
        // value<int> static-casts, so 2^32+8 would wrap to a legal 8 and pass
        // a check written against int.
        const int64_t orig = has_orig ? j["originator"].get<int64_t>() : 0;
        if (has_orig && (orig <= 0 || orig > 0xFFFF)) {
            return reply(400, "Bad Request",
                         json_err("originator out of range (1..65535)"));
        }
        return done(h_.reports_latch(has_clear, static_cast<int>(orig)));
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
        if (!j.contains("permille") || !j["permille"].is_number_integer()) {
            return reply(400, "Bad Request",
                         json_err("permille integer required"));
        }
        const int64_t permille = j["permille"].get<int64_t>();
        if (permille < 0 || permille > 1000) {
            return reply(400, "Bad Request",
                         json_err("permille must be 0..1000"));
        }
        return done(h_.bench_rx_drop(static_cast<int>(permille)));
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
    if (path == "/api/v1/link/mtu") {
        if (!h_.link_mtu) return na();
        if (!j.contains("mode") || !j["mode"].is_string()) {
            return reply(400, "Bad Request", json_err("mode (string) required"));
        }
        const auto [code, jbody] = h_.link_mtu(j["mode"].get<std::string>());
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
    if (path == "/api/v1/channel") {  // §15.5 Pass 113
        if (!h_.channel_set) return na();
        if (!j.contains("mhz")) {
            return reply(400, "Bad Request", json_err("mhz required"));
        }
        return done(h_.channel_set(j.value("mhz", 0)));
    }
    if (path == "/api/v1/psk") {  // §11.4a Pass 113
        if (!h_.psk_enable) return na();
        if (!j.contains("enabled") || !j["enabled"].is_boolean()) {
            return reply(400, "Bad Request",
                         json_err("enabled (bool) required"));
        }
        return done(h_.psk_enable(j.value("enabled", true)));
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
