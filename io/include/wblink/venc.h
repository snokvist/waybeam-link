// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §9.6 venc bitrate actuator — blocking HTTP/1.0 GET
// `/api/v1/set?video0.bitrate=<kbps>` over a plain POSIX TCP socket to the
// same-SoC encoder (MUT_LIVE, sub-ms server side; ~200 ms socket timeouts
// here so a wedged encoder cannot stall the event loop for long).
//
// WRITE-ON-CHANGE is load-bearing (§9.6): every /set persists to
// /etc/waybeam.json — pushing at the 10 Hz report rate would wear flash.
// set_bitrate() is a no-op when the target equals the last pushed value.
//
// Failures are counted, never fatal: the transport must keep flying on a
// stuck encoder API (the §9.8 fail-safe handles the rest).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

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
    bool request_idr(uint64_t now_ms);

    uint64_t pushes() const { return pushes_; }
    uint64_t failures() const { return failures_; }
    uint64_t idr_requests() const { return idr_requests_; }
    uint64_t idr_failures() const { return idr_failures_; }
    bool enabled() const { return cfg_.enabled; }

  private:
    bool http_get(const std::string& path);

    VencCfg cfg_;
    std::optional<uint32_t> last_;
    uint64_t no_retry_until_ms_ = 0;
    uint64_t pushes_ = 0;
    uint64_t failures_ = 0;
    uint64_t next_idr_ms_ = 0;
    uint64_t idr_requests_ = 0;
    uint64_t idr_failures_ = 0;
};

}  // namespace wblink
