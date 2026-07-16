// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §3.5 LINK_REPORT acceptance filter (Pass 41). Runs at
// TX ingest, before the §9 selector and the §9.11 fps ladder consume a
// report: preferred-originator-only when configured (its session follows
// reboots), first-latcher otherwise with silence-based re-latch. Pure,
// time-injected.
#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace wblink {

struct ReportGatePolicy {
    uint16_t preferred_originator = 0;  // 0 = first-latcher
    uint32_t relatch_ms = 2000;         // seed 4 x report_timeout_ms (§3.5)
};

class ReportGate {
  public:
    explicit ReportGate(const ReportGatePolicy& p) : p_(p) {}

    bool accept(uint16_t originator, uint32_t session, uint64_t now_ms) {
        if (p_.preferred_originator != 0) {
            if (originator != p_.preferred_originator) {
                ++rejected_;
                return false;
            }
            last_ms_ = now_ms;
            return true;
        }
        if (!latched_) {
            latched_ = {originator, session};
            last_ms_ = now_ms;
            return true;
        }
        if (latched_->first == originator) {
            latched_->second = session;  // §2: same node rebooted — follow
            last_ms_ = now_ms;
            return true;
        }
        // A DIFFERENT originator takes the latch only after the latched
        // reporter has been silent for relatch_ms (the §9.8 watchdog fires
        // first, so the fail-safe owns the gap in between).
        if (now_ms - last_ms_ > p_.relatch_ms) {
            latched_ = {originator, session};
            last_ms_ = now_ms;
            return true;
        }
        ++rejected_;
        return false;
    }

    uint64_t rejected() const { return rejected_; }

  private:
    ReportGatePolicy p_;
    std::optional<std::pair<uint16_t, uint32_t>> latched_;
    uint64_t last_ms_ = 0;
    uint64_t rejected_ = 0;
};

}  // namespace wblink
