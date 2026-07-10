// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §7.2 TSF-anchored quiet-gap pacer (header-only, fake-
// time testable). Two independent sides:
//
//   Craft TX — after injecting an END_OF_BLOCK the craft paces a quiet
//   window [eob + guard_us, eob + guard_us + window_us] in which it does not
//   inject video and listens (its single radio is RX-deaf while it
//   transmits; this gap is its principal opportunity to hear returns).
//
//   Ground RX — its hardware TSF latched the EOB's arrival instant (tsfl);
//   propagation is ~µs, so that receive-TSF and the craft's send-of-EOB are
//   the same physical instant. Ground schedules its return at
//   rx_tsfl(EOB) + guard_us + window_us/2 — the middle of the craft's gap.
//
// Time contract: µs-domain, one timestamp per loop iteration (the ms tick is
// too coarse for a 300 µs guard). TSF values are the radio's 32-bit tsfl —
// deltas are computed in u32 (wrap-safe) and NEVER across adapters.
//
// Both sides no-op when disabled (§7.1 opportunistic return is the shipping
// baseline; guard/window seeds RE-DERIVE at §17 gate 4).
#pragma once

#include <cstdint>
#include <optional>

namespace wblink {

struct QuietGapPolicy {
    bool enabled = false;
    uint32_t guard_us = 300;    // ≥ max(ground turnaround, TX→RX settle)
    uint32_t window_us = 2000;  // craft listen window length
    // §7.2 "airtime-critical" skip, v0 heuristic (bench-gated at gate 4):
    // when this many frames are already held back, the gap has collapsed
    // under load — stop pacing and degrade to §7.1 best-effort.
    uint32_t skip_backlog = 32;
};

class QuietGap {
  public:
    QuietGap() = default;
    explicit QuietGap(const QuietGapPolicy& p) : p_(p) {}

    // --- craft TX side ----------------------------------------------------
    void note_eob_sent(uint64_t now_us) {
        if (p_.enabled) {
            gap_start_us_ = now_us + p_.guard_us;
            gap_end_us_ = gap_start_us_ + p_.window_us;
        }
    }

    // May the craft inject video right now? held_frames = frames already
    // deferred by this gate (the airtime-critical override input).
    bool can_send_video(uint64_t now_us, uint32_t held_frames = 0) const {
        if (!p_.enabled || held_frames >= p_.skip_backlog) {
            return true;
        }
        return now_us < gap_start_us_ || now_us >= gap_end_us_;
    }

    // First instant the craft may inject again (for flush scheduling).
    uint64_t gap_end_us() const { return gap_end_us_; }

    // --- ground RX side -----------------------------------------------------
    // Host-time deadline for injecting the coalesced return, given an EOB
    // that arrived stamped tsfl_eob and (optionally) the SAME adapter's TSF
    // read at now_us. Without a TSF read (control transfer raced the RX bulk
    // load, or unsupported) the EOB's host arrival approximates the anchor —
    // elapsed = 0, USB latency becomes the error term.
    uint64_t return_deadline(uint64_t now_us, uint32_t tsfl_eob,
                             std::optional<uint64_t> tsf_now) const {
        if (!p_.enabled) {
            return now_us;
        }
        const uint32_t target =
            p_.guard_us + p_.window_us / 2;  // middle of the craft's gap
        uint32_t elapsed = 0;
        if (tsf_now) {
            elapsed = static_cast<uint32_t>(*tsf_now) - tsfl_eob;  // u32 wrap
        }
        if (elapsed >= target) {
            return now_us;  // window middle already passed (or TSF garbage)
        }
        return now_us + (target - elapsed);
    }

    bool enabled() const { return p_.enabled; }

  private:
    QuietGapPolicy p_;
    uint64_t gap_start_us_ = 0;
    uint64_t gap_end_us_ = 0;
};

}  // namespace wblink
