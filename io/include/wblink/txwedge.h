// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §9.10 TX-wedge watchdog — backend completion-progress
// liveness. Devourer supplies CCX reports; kernel-monitor supplies Linux
// netdev tx_packets. Pure and clock-injected (the quietgap.h pattern) so it
// unit-tests dry. The trigger is progress ABSENCE, never deficit: one
// completion in a window proves the backend is still moving frames.
#pragma once

#include <cstdint>

namespace wblink {

struct TxWedgePolicy {
    uint32_t window_ms = 1000;  // §17 seed; 0 disables the watchdog
    uint32_t min_submits = 8;   // fewer submissions per window = no verdict
};

class TxWedge {
  public:
    explicit TxWedge(const TxWedgePolicy& p) : p_(p) {}

    bool enabled() const { return p_.window_ms != 0; }
    bool wedged() const { return wedged_; }
    uint64_t wedge_windows() const { return wedge_windows_; }

    // Poll at any cadence with the TX adapter's cumulative counters; one
    // verdict per elapsed window. Returns true when the wedged state
    // CHANGED on this call (the caller logs the transition).
    bool poll(uint64_t now_ms, uint64_t tx_submitted, uint64_t tx_progress) {
        if (!enabled()) {
            return false;
        }
        if (!primed_) {  // first call anchors the window, no verdict
            primed_ = true;
            next_eval_ms_ = now_ms + p_.window_ms;
            last_submitted_ = tx_submitted;
            last_progress_ = tx_progress;
            return false;
        }
        if (now_ms < next_eval_ms_) {
            return false;
        }
        const uint64_t d_sub = tx_submitted - last_submitted_;
        const uint64_t d_progress = tx_progress - last_progress_;
        last_submitted_ = tx_submitted;
        last_progress_ = tx_progress;
        next_eval_ms_ = now_ms + p_.window_ms;
        const bool was = wedged_;
        if (d_progress > 0) {
            wedged_ = false;  // any completion proves the TX path alive
        } else if (d_sub >= p_.min_submits) {
            wedged_ = true;  // submissions advanced, zero completions
            ++wedge_windows_;
        }  // else: idle window — not evidence either way, hold the verdict
        return wedged_ != was;
    }

  private:
    TxWedgePolicy p_;
    bool primed_ = false;
    bool wedged_ = false;
    uint64_t wedge_windows_ = 0;
    uint64_t next_eval_ms_ = 0;
    uint64_t last_submitted_ = 0;
    uint64_t last_progress_ = 0;
};

}  // namespace wblink
