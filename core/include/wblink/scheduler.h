// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: air-side resend scheduler + multi-RX arbitration
// (PROTOCOL.md §5.3, §12, §13).
//
// Consumes decoded NACKs against the resend ring and emits RETRANSMIT
// frames under the §5.3 discipline: live packets always outrank resends
// (the caller injects live directly and only drains this scheduler with
// leftover airtime), an airtime cap partitioned per requesting originator,
// the load-bearing GLOBAL per-seq hold-down (1000 NACKs for seq N ⇒ one
// resend), importance/deadline/attempt gates, freshness-priority ordering,
// and the §12 first-latcher lock with preferred-originator preemption and
// contested-only release. §13 bitmap sanity clamp on ingest.
//
// Pure tick-driven core: injected time, emission callback, no IO.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "wblink/ring.h"
#include "wblink/table.h"
#include "wblink/wire.h"

namespace wblink {

struct SchedulerPolicy {
    uint32_t holddown_ms = 20;         // §5.3 global per-seq hold-down
    uint8_t attempt_cap = 3;           // §5.3 bounded attempts per seq
    double airtime_frac = 0.15;        // resend share of downlink airtime
    uint16_t preferred_originator = 0;  // §12 preemption; 0 = none
    uint32_t release_timeout_ms = 500;  // §12 contested-only release
    uint32_t min_recoverable_ms = 0;    // §5.3 RTT floor; 0 = off (gate 3)
    uint32_t interval_ms = 100;         // budget accounting window
    uint32_t max_block_pkts = 64;       // §13 bitmap popcount sanity clamp
    size_t budget_floor_bytes = 4096;   // per window, when live is idle
};

struct SchedulerCounters {
    uint64_t nacks_accepted = 0;
    uint64_t nacks_rejected_sanity = 0;  // §13 clamp hits
    uint64_t resends_sent = 0;
    uint64_t holddown_suppressed = 0;  // §5.3 double-send suppression
    uint64_t budget_deferred = 0;
    uint64_t dropped_deadline = 0;
    uint64_t dropped_attempts = 0;
    uint64_t dropped_not_arq = 0;   // importance gate
    uint64_t dropped_evicted = 0;   // ring no longer has it
    uint16_t lock_holder = 0;       // current §12 latch (0 = parked free)
};

class ResendScheduler {
  public:
    using EmitResend = std::function<void(const uint8_t* frame, size_t len)>;

    ResendScheduler(const SchedulerPolicy& policy, const ProfileTable* table)
        : policy_(policy), table_(table) {}

    // §8: TX applies its own deadline = first_tx + budget(profile, class).
    void set_active_profile(uint8_t profile) { active_profile_ = profile; }

    // A NACK arrived (already decoded); requester = its common-prefix
    // originator. Validates (§13), arbitrates the lock (§12), queues.
    void on_nack(const NackView& nack, ResendRing& ring, uint64_t now_ms);

    // Budget base: live bytes injected since the last drain window.
    void note_live_bytes(size_t bytes) { live_bytes_window_ += bytes; }

    // Emit due resends within the airtime budget, freshness-first. The
    // emitted frame already carries RETRANSMIT (§5.3).
    void drain(ResendRing& ring, uint64_t now_ms, const EmitResend& emit);

    const SchedulerCounters& counters() const { return counters_; }

    // §15.5 stats/reset: zero the cumulative counters (in-flight requests and
    // scheduling state are untouched).
    void reset_counters() { counters_ = {}; }

  private:
    struct Request {
        uint16_t requester = 0;  // upgraded to preferred if it also asks
        uint64_t requested_ms = 0;
    };

    uint64_t entry_deadline(const RingEntry& e) const;
    void arbitrate_lock(uint16_t requester, uint64_t now_ms);

    SchedulerPolicy policy_;
    const ProfileTable* table_;
    uint8_t active_profile_ = 0;

    std::map<uint32_t, Request> pending_;       // seq -> request
    std::map<uint32_t, uint64_t> last_resend_;  // §5.3 global hold-down
    std::map<uint16_t, uint64_t> last_nack_ms_;  // §12 release evidence
    uint16_t lock_holder_ = 0;

    uint64_t window_start_ms_ = 0;
    size_t live_bytes_window_ = 0;
    std::map<uint16_t, size_t> spent_window_;  // per-originator bytes

    SchedulerCounters counters_;
};

}  // namespace wblink
