// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §9.6 venc bitrate actuator — blocking HTTP/1.0 GET
// `/api/v1/live/set?video0.bitrate=<kbps>` over a plain POSIX TCP socket to
// the same-SoC encoder (MUT_LIVE, sub-ms server side; ~200 ms socket
// timeouts here so a wedged encoder cannot stall the event loop for long).
//
// VOLATILE-FIRST (§9.6 Pass 73): every push targets /api/v1/live/set (no
// flash write); the first 404 latches a one-shot per-process fallback to the
// persisting /api/v1/set (pre-live venc) and re-sends the push that drew it.
// WRITE-ON-CHANGE stays load-bearing (§9.6): flash wear on the fallback
// path, pointless HTTP churn on the live path. set_bitrate() is a no-op
// when the target equals the last pushed value.
//
// Failures are counted, never fatal: the transport must keep flying on a
// stuck encoder API (the §9.8 fail-safe handles the rest).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "wblink/config.h"

namespace wblink {

class VencActuator {
  public:
    explicit VencActuator(const VencCfg& cfg) : cfg_(cfg) {}

    // Push kbps if it differs from the last SUCCESSFULLY pushed value.
    // Returns true when no HTTP call was needed or the call succeeded (2xx
    // status line). Callers may invoke this every tick: write-on-change
    // dedupes, and after a failure retries are held off for 500 ms so a
    // wedged encoder (200 ms socket budget per attempt) cannot degrade the
    // event loop.
    bool set_bitrate(uint32_t kbps, uint64_t now_ms);
    // §9.6 Pass 37 horizon caps: one /set carrying both fields (venc applies
    // them as one live group). Same write-on-change + holdoff discipline;
    // gated on cfg.enabled AND cfg.frame_caps. {0,0} is a no-op (never
    // command "unlimited" implicitly).
    bool set_max_frame_size(uint32_t max_i_bytes, uint32_t max_p_bytes,
                            uint64_t now_ms);
    // §9.11 FPS ladder: write-on-change like the others; venc applies fps
    // live (skipping no-op rebinds) and requests an IDR after a real change.
    bool set_fps(uint16_t fps, uint64_t now_ms);
    bool request_idr(uint64_t now_ms);

    uint64_t pushes() const { return pushes_; }
    uint64_t failures() const { return failures_; }
    uint64_t idr_requests() const { return idr_requests_; }
    uint64_t idr_failures() const { return idr_failures_; }
    bool enabled() const { return cfg_.enabled; }
    bool recovery_enabled() const { return cfg_.recovery_enabled; }
    bool frame_caps_enabled() const { return cfg_.enabled && cfg_.frame_caps; }
    // §15.3 actuator state: last commanded values (0 = never pushed) and the
    // doc-model "pending transition" — within settle_ms of the last accepted
    // value-changing push.
    uint32_t commanded_bitrate_kbps() const { return last_ ? *last_ : 0; }
    uint32_t commanded_max_i_bytes() const {
        return last_caps_ ? last_caps_->first : 0;
    }
    uint32_t commanded_max_p_bytes() const {
        return last_caps_ ? last_caps_->second : 0;
    }
    uint16_t commanded_fps() const { return last_fps_ ? *last_fps_ : 0; }
    bool settling(uint64_t now_ms) const {
        return last_change_ms_ != 0 &&
               now_ms < last_change_ms_ + cfg_.settle_ms;
    }
    // §9.6 Pass 73: true once a 404 latched the persisting-/set fallback.
    bool live_fallback() const { return live_fallback_; }

  private:
    bool http_get(const std::string& path);
    int http_get_status(const std::string& path);  // HTTP code; 0 = transport
    bool push_set(const std::string& query);       // §9.6 volatile-first

    VencCfg cfg_;
    bool live_fallback_ = false;
    std::optional<uint32_t> last_;
    std::optional<std::pair<uint32_t, uint32_t>> last_caps_;
    std::optional<uint16_t> last_fps_;
    uint64_t last_change_ms_ = 0;
    uint64_t no_retry_until_ms_ = 0;
    uint64_t pushes_ = 0;
    uint64_t failures_ = 0;
    uint64_t next_idr_ms_ = 0;
    uint64_t idr_requests_ = 0;
    uint64_t idr_failures_ = 0;
};

}  // namespace wblink
