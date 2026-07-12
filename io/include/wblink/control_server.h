// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §15.5 REST control plane. A minimal HTTP/1.0 server folded
// into the single-threaded event loop — no threads, no locks. The caller polls
// it once per tick (service(), a 0 ms-timeout poll over the listen socket and
// any in-flight connections); each connection serves ONE bounded request and
// closes, except GET /api/v1/stats/stream which is held open as an SSE feed and
// fed by publish_stats(). A slow or oversize request is dropped, never awaited,
// so a client cannot stall the flight path.
//
// No auth (matches the §13 data-plane posture); bind 127.0.0.1 to keep it
// host-local. Handlers are std::function hooks owned by the caller; a null
// write-hook means "not applicable in this mode" and yields HTTP 409.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wblink/config.h"  // Result<T>

namespace wblink {

struct ControlHandlers {
    // Reads — return a JSON body (GET /stats returns the §15.3 object with no
    // trailing newline). A null hook yields 503 (not ready).
    std::function<std::string()> stats_line;
    std::function<std::string()> info_json;
    std::function<std::string()> health_json;
    std::function<std::string()> discovery_json;

    // Writes — return "" on success, else a short error string (→ HTTP 400).
    // A null hook means the endpoint does not apply to this mode (→ HTTP 409).
    std::function<std::string(uint32_t mhz, uint32_t klass)> csa;
    std::function<std::string(int min_profile, int max_profile)> profile;
    std::function<std::string(int stream_id, int i_permille, int p_permille,
                              int min_k)>
        fec;
    std::function<void()> reset_stats;  // side-effect only; always 200
    std::function<std::string(int stream_id)> video_recover;
};

class ControlServer {
  public:
    // bind = "addr:port" (e.g. "0.0.0.0:8091", "127.0.0.1:8091"). Listens on a
    // non-blocking TCP socket. Never call with an empty bind (caller gates).
    static Result<std::unique_ptr<ControlServer>> create(
        const std::string& bind);

    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    void set_handlers(ControlHandlers h) { h_ = std::move(h); }

    // Accept new connections and service ready ones without blocking. now_ms
    // drives the slow-client timeout (a request that never completes is
    // dropped after kRequestTimeoutMs).
    void service(uint64_t now_ms);

    // Push one §15.3 line (trailing '\n' tolerated) to every SSE subscriber.
    void publish_stats(const std::string& line);

    int listen_fd() const { return listen_fd_; }

  private:
    ControlServer() = default;

    struct Conn {
        int fd = -1;
        bool streaming = false;   // upgraded to an SSE feed
        uint64_t opened_ms = 0;   // for the slow-client timeout
        std::string inbuf;        // accumulates until a full request
    };

    void accept_new(uint64_t now_ms);
    void service_conn(Conn& c, uint64_t now_ms);  // may set c.fd = -1 to close
    void dispatch(Conn& c, const std::string& method, const std::string& path,
                  const std::string& body);
    void publish_one(Conn& c, const std::string& line);  // one SSE frame

    int listen_fd_ = -1;
    std::vector<Conn> conns_;
    ControlHandlers h_;
};

}  // namespace wblink
