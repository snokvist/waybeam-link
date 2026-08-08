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
#include <optional>
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
    int txp_auto = -1, txp_qdb = -1000;
    std::string ucal_action, ucal_refuse;
    int ucal_calls = 0;
    int latch_calls = 0, latch_clear = -1, latch_orig = -1;
    std::optional<uint16_t> fec_e;  // §14.1a; nullopt = inherit p_permille
    int fec_calls = 0;
    int fec_sid = -1, fec_i = -1, fec_p = -1, fec_k = -1, fec_r = -1;
    bool fec_ok = true;
    int reset_calls = 0;
    int recovery_stream = -2;
    int bench_drop = -1;

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
    h.discovery_json = [] {
        return std::string(
            "{\"nodes\":[{\"originator\":17,\"session\":3,"
            "\"last_seen_ms\":9}],\"streams\":[]}");
    };
    h.selection_json = [] {
        return std::string("{\"state\":\"committed\",\"originator\":17}");
    };
    h.cache_assignment_json = [] {
        return std::string("{\"controller\":9,\"target_originator\":17}");
    };
    h.profile = [&](int mn, int mx) -> std::string {
        if (mx != 255 && mn > mx) return "min>max";
        pin_min = mn;
        pin_max = mx;
        return "";
    };
    // §10.5 tx/power override-latch hooks (Pass 114).
    h.tx_power_set = [&](bool is_auto, int qdb) -> std::string {
        txp_auto = is_auto ? 1 : 0;
        txp_qdb = qdb;
        return "";
    };
    h.tx_power_json = [&]() -> std::string {
        return std::string(
            "{\"override_active\":false,\"backend\":\"kernel-monitor\"}");
    };
    // §10.7 (Pass 125): both directions answer at /api/v1/calibration, so
    // `direction` is what tells a Hub which one it is holding. This node
    // serves the ground/uplink shape.
    h.calibration_json = [] {
        return std::string(
            "{\"direction\":\"uplink\",\"state\":\"done\",\"rung\":0,"
            "\"power_qdb\":72,\"quality\":{\"valid\":true,\"rx_mcs\":0}}");
    };
    // §10.7 ground-uplink calibration POST (Pass 125).
    h.uplink_calibrate = [&](const std::string& action) -> std::string {
        ucal_action = action;
        ++ucal_calls;
        return ucal_refuse;  // non-empty = failed prerequisite -> 409
    };
    h.fec = [&](int sid, int ip, int pp, int mk, int mr,
                std::optional<uint16_t> ep) -> std::string {
        fec_sid = sid;
        fec_i = ip;
        fec_p = pp;
        fec_k = mk;
        fec_r = mr;
        fec_e = ep;  // §14.1a: nullopt = inherit p_permille
        ++fec_calls;
        return fec_ok ? "" : "no frame-shm stream with that id";
    };
    h.reset_stats = [&] { ++reset_calls; };
    // §3.5 Pass 115 report-authority override.
    h.reports_latch = [&](bool clear, int originator) -> std::string {
        ++latch_calls;
        latch_clear = clear ? 1 : 0;
        latch_orig = originator;
        return "";
    };
    h.video_recover = [&](int stream_id) -> std::string {
        recovery_stream = stream_id;
        return stream_id < 0 ? "no matching latched RTP stream" : "";
    };
    h.bench_rx_drop = [&](int permille) -> std::string {
        if (permille < 0 || permille > 1000) return "out of range";
        bench_drop = permille;
        return "";
    };
    // §11.7 vehicle command: typed {code, body} outcome + campaign GET.
    std::string vcmd_last_cmd;
    int vcmd_last_arg = -1;
    bool vcmd_bound = true;
    h.vehicle_command_json = [] {
        return std::string(
            "{\"nonce\":42,\"cmd\":\"arq\",\"arg\":0,\"state\":\"acked\"}");
    };
    h.vehicle_command = [&](const std::string& cmd, int arg)
        -> std::pair<int, std::string> {
        if (cmd != "arq" && cmd != "selector" && cmd != "fps_ladder") {
            return {400, "{\"ok\":false,\"error\":\"unknown cmd\"}"};
        }
        if (!vcmd_bound) {
            return {409, "{\"ok\":false,\"error\":\"no bound craft\"}"};
        }
        vcmd_last_cmd = cmd;
        vcmd_last_arg = arg;
        return {200, "{\"ok\":true,\"nonce\":42}"};
    };
    std::string mtu_mode = "default";
    h.link_mtu_json = [&] {
        return "{\"mode\":\"" + mtu_mode +
               "\",\"requested\":1424,\"effective\":1424,"
               "\"supported\":3072}";
    };
    h.link_mtu = [&](const std::string& mode)
        -> std::pair<int, std::string> {
        if (mode != "default" && mode != "medium" && mode != "high" &&
            mode != "auto") {
            return {400, "{\"ok\":false,\"error\":\"bad mode\"}"};
        }
        mtu_mode = mode;
        return {200, "{\"ok\":true}"};
    };
    // §6.4 RX-local NACK-emission gate.
    int arq_state = -1;
    h.arq_enable = [&](bool enabled) -> std::string {
        arq_state = enabled ? 1 : 0;
        return "";
    };
    // §15.5 operating-mode selection (Pass 96).
    std::string mode_state = "boot-mode";
    h.mode_get = [&]() -> std::string {
        return "{\"active\":\"" + mode_state + "\",\"apply_configured\":true}";
    };
    h.mode_set = [&](const std::string& name) -> std::string {
        if (name == "reject-me") return "not a known mode";
        mode_state = name;
        return "";
    };
    // §9.11 craft-local FPS-ladder toggle (Pass 99).
    int fps_ladder_state = -1;
    h.link_fps = [&](bool ladder_on) -> std::string {
        fps_ladder_state = ladder_on ? 1 : 0;
        return "";
    };
    // §15.5 Pass 113: craft-local channel set + runtime pairing gate.
    int channel_state = 0;
    h.channel_set = [&](int mhz) -> std::string {
        if (mhz != 5805 && mhz != 5745) return "mhz not in channel_allowlist";
        channel_state = mhz;
        return "";
    };
    int psk_state = -1;
    h.psk_enable = [&](bool enabled) -> std::string {
        psk_state = enabled ? 1 : 0;
        return "";
    };
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
    {
        const std::string r =
            roundtrip(s, port, "GET /api/v1/discovery HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r).find("\"originator\":17") != std::string::npos);
    }
    {
        const std::string r = roundtrip(
            s, port, "GET /api/v1/link/selection HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r).find("\"originator\":17") != std::string::npos);
    }
    {
        const std::string r = roundtrip(
            s, port, "GET /api/v1/cache/assignment HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r).find("\"controller\":9") != std::string::npos);
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
    // §3.5 Pass 115 /api/v1/reports/latch. The malformed-body cases are the
    // point: a non-integer originator threw json::type_error out of an
    // uncaught dispatch (killing the daemon over one POST), and a 64-bit
    // value narrowed into range before the bounds check saw it.
    {
        const auto post = [&](const std::string& body) {
            return roundtrip(s, port,
                             "POST /api/v1/reports/latch HTTP/1.0\r\n"
                             "Content-Length: " +
                                 std::to_string(body.size()) + "\r\n\r\n" +
                                 body);
        };
        CHECK_EQ_U(status_of(post("{\"clear\":true}")), 200);
        CHECK_EQ_U(latch_clear, 1);
        CHECK_EQ_U(status_of(post("{\"originator\":42}")), 200);
        CHECK_EQ_U(latch_clear, 0);
        CHECK_EQ_U(latch_orig, 42);
        const int calls_before = latch_calls;
        // Exactly-one gate, and clear:false has no meaning.
        CHECK_EQ_U(status_of(post("{}")), 400);
        CHECK_EQ_U(status_of(post("{\"clear\":true,\"originator\":5}")), 400);
        CHECK_EQ_U(status_of(post("{\"clear\":false}")), 400);
        CHECK_EQ_U(status_of(post("{\"clear\":1}")), 400);
        // Type confusion: these must 400, not throw.
        CHECK_EQ_U(status_of(post("{\"originator\":\"5\"}")), 400);
        CHECK_EQ_U(status_of(post("{\"originator\":null}")), 400);
        CHECK_EQ_U(status_of(post("{\"originator\":1.5}")), 400);
        // Wide value that would wrap to a legal 8 if narrowed first.
        CHECK_EQ_U(status_of(post("{\"originator\":4294967304}")), 400);
        CHECK_EQ_U(status_of(post("{\"originator\":0}")), 400);
        CHECK_EQ_U(status_of(post("{\"originator\":65536}")), 400);
        CHECK_EQ_U(status_of(post("{\"originator\":-1}")), 400);
        CHECK_EQ_U(latch_calls, calls_before);  // hook never reached
    }
    // §10.5 tx/power: GET state, POST qdb latch, POST auto clear, body gates.
    {
        const std::string r =
            roundtrip(s, port, "GET /api/v1/tx/power HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r).find("\"override_active\":false") !=
              std::string::npos);
    }
    {
        const std::string body = "{\"qdb\":20}";
        const std::string req =
            "POST /api/v1/tx/power HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(txp_auto, 0);
        CHECK_EQ_U(txp_qdb, 20);
    }
    {
        const std::string body = "{\"auto\":true}";
        const std::string req =
            "POST /api/v1/tx/power HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(txp_auto, 1);
    }
    // Neither key, both keys, and auto:false-with-no-qdb → 400 before the hook.
    {
        const std::string body = "{}";
        const std::string req =
            "POST /api/v1/tx/power HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    // §10.7 GET: the body carries the direction discriminator verbatim.
    {
        const std::string r =
            roundtrip(s, port, "GET /api/v1/calibration HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r).find("\"direction\":\"uplink\"") != std::string::npos);
        CHECK(body_of(r).find("\"rx_mcs\":0") != std::string::npos);
    }
    // §10.7 POST /api/v1/calibration: exactly start|abort. A malformed body
    // is 400 and must not reach the hook; a refused prerequisite is 409 and
    // carries WHICH one, since the operator cannot guess from a bare code.
    {
        const auto post_cal = [&](const std::string& body) {
            const std::string req =
                "POST /api/v1/calibration HTTP/1.0\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body;
            return roundtrip(s, port, req);
        };
        CHECK_EQ_U(status_of(post_cal("{\"action\":\"start\"}")), 200);
        CHECK(ucal_action == "start");
        CHECK_EQ_U(status_of(post_cal("{\"action\":\"abort\"}")), 200);
        CHECK(ucal_action == "abort");
        // §10.7 (Pass 125): the bi-directional action is a third verb on the
        // same endpoint, so `start` and `abort` keep working unchanged.
        CHECK_EQ_U(status_of(post_cal("{\"action\":\"start_both\"}")), 200);
        CHECK(ucal_action == "start_both");

        const int calls_before = ucal_calls;
        CHECK_EQ_U(status_of(post_cal("{}")), 400);
        CHECK_EQ_U(status_of(post_cal("{\"action\":\"resume\"}")), 400);
        CHECK_EQ_U(status_of(post_cal("{\"action\":7}")), 400);
        CHECK_EQ_U(ucal_calls, calls_before);  // hook never reached

        ucal_refuse = "no fresh authenticated feedback";
        const std::string r = post_cal("{\"action\":\"start\"}");
        CHECK_EQ_U(status_of(r), 409);
        CHECK(body_of(r).find("no fresh authenticated feedback") !=
              std::string::npos);
        ucal_refuse.clear();
    }
    {
        const std::string body = "{\"qdb\":20,\"auto\":true}";
        const std::string req =
            "POST /api/v1/tx/power HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    {
        const std::string body = "{\"auto\":false}";
        const std::string req =
            "POST /api/v1/tx/power HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    // §10.5 wire-range on the wide type: 2^32+20 must 400, not wrap to 20.
    {
        txp_qdb = -1000;
        const std::string body = "{\"qdb\":4294967316}";
        const std::string req =
            "POST /api/v1/tx/power HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
        CHECK_EQ_U(txp_qdb, -1000);  // hook never ran
    }
    {
        const std::string body = "{\"qdb\":512}";
        const std::string req =
            "POST /api/v1/tx/power HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
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
            "{\"stream_id\":0,\"i_permille\":300,\"p_permille\":120,\"min_k\":4,"
            "\"min_r\":3}";
        const std::string req =
            "POST /api/v1/fec HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(fec_sid, 0);
        CHECK_EQ_U(fec_i, 300);
        CHECK_EQ_U(fec_p, 120);
        CHECK_EQ_U(fec_k, 4);
        CHECK_EQ_U(fec_r, 3);
        CHECK(!fec_e.has_value());  // §14.1a: omitted => inherit p_permille
    }
    // §14.1a e_permille: accepted, explicit null clears, out-of-range and
    // non-integer are 400s that never reach the handler (a throw here would
    // unwind out of the server, so the type guard is the point).
    {
        auto post = [&](const std::string& body) {
            return status_of(roundtrip(
                s, port,
                "POST /api/v1/fec HTTP/1.0\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\n\r\n" + body));
        };
        CHECK_EQ_U(post("{\"stream_id\":0,\"e_permille\":0}"), 200);
        CHECK(fec_e.has_value());
        CHECK_EQ_U(*fec_e, 0u);  // 0 is a value, not "unset"
        CHECK_EQ_U(post("{\"stream_id\":0,\"e_permille\":null}"), 200);
        CHECK(!fec_e.has_value());
        const int before = fec_calls;
        CHECK_EQ_U(post("{\"stream_id\":0,\"e_permille\":4001}"), 400);
        CHECK_EQ_U(post("{\"stream_id\":0,\"e_permille\":-1}"), 400);
        CHECK_EQ_U(post("{\"stream_id\":0,\"e_permille\":\"x\"}"), 400);
        CHECK_EQ_U(fec_calls, before);  // none of them actuated
    }
    // fec min_r defaults to 2 when the body omits it.
    {
        const std::string body =
            "{\"stream_id\":0,\"i_permille\":300,\"p_permille\":200,\"min_k\":3}";
        const std::string req =
            "POST /api/v1/fec HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(fec_r, 2);
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
    // video recovery stream selection and handler error propagation.
    {
        const std::string body = "{\"stream_id\":7}";
        const std::string req =
            "POST /api/v1/video/recover HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(recovery_stream, 7);
    }
    {
        const std::string req =
            "POST /api/v1/video/recover HTTP/1.0\r\n"
            "Content-Length: 2\r\n\r\n{}";
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
        CHECK_EQ_U(recovery_stream, static_cast<uint64_t>(-1));
    }
    // An empty optional body behaves like {}, never a null JSON value that
    // can throw from value() and terminate the server.
    {
        const std::string req =
            "POST /api/v1/video/recover HTTP/1.0\r\n\r\n";
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
        CHECK_EQ_U(recovery_stream, static_cast<uint64_t>(-1));
    }
    // UDP-air synthetic loss retunes in-process and validates its range.
    {
        const std::string body = "{\"permille\":175}";
        const std::string req =
            "POST /api/v1/bench/rx-drop HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(bench_drop, 175);
    }
    {
        const std::string body = "{\"permille\":1001}";
        const std::string req =
            "POST /api/v1/bench/rx-drop HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    // §11.7 vehicle command: GET state, POST round-trip incl. the typed
    // 409/400 outcomes, missing fields, and the §6.4 arq gate.
    {
        const std::string r = roundtrip(
            s, port, "GET /api/v1/vehicle/command HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r).find("\"state\":\"acked\"") != std::string::npos);
    }
    {  // §9.3a MTU preference GET/POST and typed validation.
        const std::string r = roundtrip(
            s, port, "GET /api/v1/link/mtu HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r).find("\"supported\":3072") != std::string::npos);
        const std::string body = "{\"mode\":\"auto\"}";
        const std::string req =
            "POST /api/v1/link/mtu HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK(mtu_mode == "auto");
    }
    {
        const std::string body = "{\"mode\":7}";
        const std::string req =
            "POST /api/v1/link/mtu HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    {
        const std::string body = "{\"cmd\":\"arq\",\"arg\":0}";
        const std::string req =
            "POST /api/v1/vehicle/command HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        const std::string r = roundtrip(s, port, req);
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r).find("\"nonce\":42") != std::string::npos);
        CHECK(vcmd_last_cmd == "arq");
        CHECK_EQ_U(vcmd_last_arg, 0);
    }
    {
        const std::string body = "{\"cmd\":\"warp\",\"arg\":1}";
        const std::string req =
            "POST /api/v1/vehicle/command HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    {
        vcmd_bound = false;  // handler's 409 (unbound / pending) rides through
        const std::string body = "{\"cmd\":\"selector\",\"arg\":1}";
        const std::string req =
            "POST /api/v1/vehicle/command HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 409);
        vcmd_bound = true;
    }
    {
        const std::string body = "{\"cmd\":\"arq\"}";  // arg required
        const std::string req =
            "POST /api/v1/vehicle/command HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    {
        const std::string body = "{\"enabled\":false}";
        const std::string req =
            "POST /api/v1/arq HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(arq_state, 0);
    }
    {
        const std::string body = "{\"enabled\":1}";  // must be a bool
        const std::string req =
            "POST /api/v1/arq HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    // §9.11 craft-local FPS-ladder toggle (Pass 99).
    {  // ladder on → 200, handler sees true.
        const std::string body = "{\"ladder\":true}";
        const std::string req =
            "POST /api/v1/link/fps HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(fps_ladder_state, 1);
    }
    {  // ladder off → 200, handler sees false.
        const std::string body = "{\"ladder\":false}";
        const std::string req =
            "POST /api/v1/link/fps HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(fps_ladder_state, 0);
    }
    {  // missing ladder → 400.
        const std::string body = "{}";
        const std::string req =
            "POST /api/v1/link/fps HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    {  // non-bool ladder → 400.
        const std::string body = "{\"ladder\":1}";
        const std::string req =
            "POST /api/v1/link/fps HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    // §15.5 Pass 113: craft-local channel set.
    {  // allowlisted channel → 200, handler sees it.
        const std::string body = "{\"mhz\":5745}";
        const std::string req =
            "POST /api/v1/channel HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(channel_state, 5745);
    }
    {  // handler rejection (off-allowlist) rides through as 400.
        const std::string body = "{\"mhz\":2412}";
        const std::string req =
            "POST /api/v1/channel HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
        CHECK_EQ_U(channel_state, 5745);  // unchanged
    }
    {  // missing mhz → 400.
        const std::string req =
            "POST /api/v1/channel HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}";
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    // §11.4a Pass 113: runtime pairing gate.
    {  // open pairing → 200, handler sees false.
        const std::string body = "{\"enabled\":false}";
        const std::string req =
            "POST /api/v1/psk HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(psk_state, 0);
    }
    {  // lock → 200, handler sees true.
        const std::string body = "{\"enabled\":true}";
        const std::string req =
            "POST /api/v1/psk HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        CHECK_EQ_U(psk_state, 1);
    }
    {  // non-bool enabled → 400.
        const std::string body = "{\"enabled\":\"yes\"}";
        const std::string req =
            "POST /api/v1/psk HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
        CHECK_EQ_U(psk_state, 1);  // unchanged
    }
    // §15.5 operating-mode selection (Pass 96).
    {
        const std::string r =
            roundtrip(s, port, "GET /api/v1/mode HTTP/1.0\r\n\r\n");
        CHECK_EQ_U(status_of(r), 200);
        CHECK(body_of(r).find("\"active\":\"boot-mode\"") != std::string::npos);
    }
    {  // a valid apply → 200, and the label updates.
        const std::string body = "{\"name\":\"imx335-60fps-medrange\"}";
        const std::string req =
            "POST /api/v1/mode HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 200);
        const std::string r =
            roundtrip(s, port, "GET /api/v1/mode HTTP/1.0\r\n\r\n");
        CHECK(body_of(r).find("imx335-60fps-medrange") != std::string::npos);
    }
    {  // missing name → 400.
        const std::string body = "{}";
        const std::string req =
            "POST /api/v1/mode HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
    }
    {  // handler-level rejection → 400 with the error string.
        const std::string body = "{\"name\":\"reject-me\"}";
        const std::string req =
            "POST /api/v1/mode HTTP/1.0\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        CHECK_EQ_U(status_of(roundtrip(s, port, req)), 400);
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

    // §11 CSA trigger with a LIVE handler (Pass 108). The hook returns
    // (status, body) rather than an error string precisely so the two
    // unbound-craft refusals can be told apart: "no craft selected" is a 409
    // like every other unbound-craft path, while "selected but no cached key"
    // stays a 400. Conflating them was what sent operators to re-scout a
    // healthy link. Driven on a second server so the null-handler 409 above
    // (endpoint not applicable in this mode) still stands.
    {
        auto srv2 = ControlServer::create("127.0.0.1:0");
        CHECK(static_cast<bool>(srv2));
        if (srv2) {
            ControlServer& s2 = **srv2.value;
            const uint16_t port2 = bound_port(s2.listen_fd());
            CHECK(port2 != 0);

            uint16_t selected = 0;   // 0 = nothing latched, no claim
            bool have_key = false;
            uint32_t csa_mhz = 0;
            ControlHandlers h2;
            h2.csa = [&](uint32_t mhz,
                         uint32_t klass) -> std::pair<int, std::string> {
                (void)klass;
                if (selected == 0) {
                    return {409,
                            "{\"ok\":false,\"error\":\"no craft selected\"}"};
                }
                if (!have_key) {
                    return {400,
                            "{\"ok\":false,\"error\":\"no live CSA key\"}"};
                }
                csa_mhz = mhz;
                return {200, "{\"ok\":true}"};
            };
            s2.set_handlers(std::move(h2));

            const auto csa_post = [&](const char* body) {
                const std::string b = body;
                return roundtrip(s2, port2,
                                 "POST /api/v1/csa HTTP/1.0\r\nContent-Length: " +
                                     std::to_string(b.size()) + "\r\n\r\n" + b);
            };

            {  // nothing selected → 409, distinct from the keyless 400 below.
                const std::string r = csa_post("{\"mhz\":5805}");
                CHECK_EQ_U(status_of(r), 409);
                CHECK(body_of(r).find("no craft selected") != std::string::npos);
                CHECK_EQ_U(csa_mhz, 0u);
            }
            {  // a craft IS selected but no announced token is cached → 400.
                selected = 17;
                const std::string r = csa_post("{\"mhz\":5805}");
                CHECK_EQ_U(status_of(r), 400);
                CHECK(body_of(r).find("no live CSA key") != std::string::npos);
                CHECK_EQ_U(csa_mhz, 0u);
            }
            {  // selected + keyed → the campaign starts.
                have_key = true;
                const std::string r = csa_post("{\"mhz\":5745}");
                CHECK_EQ_U(status_of(r), 200);
                CHECK(body_of(r).find("\"ok\":true") != std::string::npos);
                CHECK_EQ_U(csa_mhz, 5745u);
            }
            {  // mhz is still validated ahead of the hook.
                const std::string r = csa_post("{}");
                CHECK_EQ_U(status_of(r), 400);
                CHECK_EQ_U(csa_mhz, 5745u);  // hook not reached
            }
        }
    }

    return wbtest_finish("control_server_test");
}
