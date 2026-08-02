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
#include <utility>
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
    // §15.5a ground scout state (GET); null → 503 like the other reads.
    std::function<std::string()> scout_results;
    std::function<std::string()> selection_json;
    std::function<std::string()> cache_assignment_json;
    // §11.7 issuer campaign state (GET; issuer/ground only, null → 409).
    std::function<std::string()> vehicle_command_json;
    // §9.3a negotiated packet-budget state (every node when wired).
    std::function<std::string()> link_mtu_json;
    // §10.5 TX-power override-latch state (GET; TX node only, null → 409).
    std::function<std::string()> tx_power_json;
    // §10.6 (Pass 120) calibration surface (GET; TX node only, null → 409).
    std::function<std::string()> calibration_json;
    // §10.7 (Pass 125) POST /api/v1/calibration on a ground/rx node: "start"
    // or "abort". Returns "" on success, else the failed prerequisite — the
    // list is long and every entry is a real hazard, so the operator needs
    // to know WHICH one, not just that it was refused.
    std::function<std::string(const std::string& action)> uplink_calibrate;

    // Writes — return "" on success, else a short error string (→ HTTP 400).
    // A null hook means the endpoint does not apply to this mode (→ HTTP 409).
    // §11 CSA campaign trigger. Returns (status, full JSON body) like
    // `vehicle_command` rather than an error string: Pass 108 needs to
    // distinguish "no craft selected" (409, the same refusal every other
    // unbound-craft path gives) from "selected but no key cached" (400).
    std::function<std::pair<int, std::string>(uint32_t mhz, uint32_t klass)> csa;
    std::function<std::string(int min_profile, int max_profile)> profile;
    // §10.5 (Pass 114) TX-power override-latch: is_auto=true clears the
    // latch (qdb ignored), else latch the absolute qdb on every tx adapter.
    std::function<std::string(bool is_auto, int qdb)> tx_power_set;
    std::function<std::string(int stream_id, int i_permille, int p_permille,
                              int min_k, int min_r)>
        fec;
    std::function<void()> reset_stats;  // side-effect only; always 200
    // §15.5 Pass 103: drop the venc actuator's write-on-change cache so the
    // next tick re-asserts bitrate/caps/fps after an out-of-loop venc restart
    // (the §16 mode applier POSTs this). Side-effect only; always 200.
    std::function<void()> venc_reassert;
    // §15.5a scout writes (ground/rx only; null → 409). start takes the parsed
    // sweep request; empty channels → the engine substitutes the allowlist.
    std::function<std::string(const std::vector<uint16_t>& channels,
                              uint32_t dwell_ms, const std::string& mode,
                              int target_originator)>
        scout_start;
    std::function<std::string()> scout_stop;
    // §15.5a claim: quickconnect to a scouted craft by originator. target_chan 0
    // → the engine picks the emptiest allowlisted channel. null hook → 409.
    std::function<std::string(int originator, int target_chan)> scout_quickconnect;
    std::function<std::string(int stream_id)> video_recover;
    std::function<std::string(int permille)> bench_rx_drop;
    // §11.7 vehicle command (issuer/ground only; null → 409). Returns the
    // full HTTP outcome: {200, {"ok":true,"nonce":N}} on start, {409, …}
    // while a campaign is pending or with no bound craft, {400, …} on a bad
    // cmd/arg — the campaign itself is polled via vehicle_command_json.
    std::function<std::pair<int, std::string>(const std::string& cmd, int arg)>
        vehicle_command;
    // §9.3a ground-local preference. Returns a complete HTTP outcome so an
    // in-flight command can be reported as 409 without losing the preference.
    std::function<std::pair<int, std::string>(const std::string& mode)> link_mtu;
    // §6.4 RX-local NACK-emission gate (rx only; null → 409).
    std::function<std::string(bool enabled)> arq_enable;
    // §9.11 craft-local FPS-ladder toggle (Pass 99; tx/craft only, null → 409).
    // true = variable fps (ladder runs), false = static (ladder holds). Routes
    // through the same §11.7 transition as the over-air FPS_LADDER command.
    std::function<std::string(bool ladder_on)> link_fps;
    // §15.5 operating-mode selection (Pass 96; tx/craft only, null → 409).
    // mode_get returns the JSON body for GET /api/v1/mode. mode_set applies a
    // named mode (docs/venc-mode-matrix.md §16): "" on success, else a short
    // error string (→ 400). The link owns the active mode; the hub calls here.
    std::function<std::string()> mode_get;
    std::function<std::string(const std::string& name)> mode_set;
    // §15.5 Pass 104: modes_list returns the JSON body for GET /api/v1/modes —
    // the operating-mode catalog (tx/craft only, null → 409).
    std::function<std::string()> modes_list;
    // §15.5 Pass 113 (tx/craft only, null → 409): local channel-set within
    // the CSA allowlist ("" on success, error string → 400), and the §11.4a
    // runtime pairing gate (false = open pairing / fresh announced token,
    // true = locked / stop announcing).
    std::function<std::string(int mhz)> channel_set;
    std::function<std::string(bool enabled)> psk_enable;
    // §15.5 Pass 115 (tx/craft only, null → 409): §3.5 report-authority
    // override. clear = release the LINK_REPORT + JSCC_FEEDBACK latch so the
    // next reporter takes it within relatch_ms; otherwise force it to
    // `originator`. Returns "" on success, else a short error string (→ 400).
    std::function<std::string(bool clear, int originator)> reports_latch;
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
