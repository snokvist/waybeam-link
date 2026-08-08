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
        // Saturating: a now_ms that lands BEFORE last_ms_ (a caller sampling a
        // fresh clock inside a callback, after the tick's now was captured)
        // would underflow this u64 subtraction to ~2^64 and hand the latch to
        // the first displaced reporter that speaks.
        if (now_ms > last_ms_ && now_ms - last_ms_ > p_.relatch_ms) {
            latched_ = {originator, session};
            last_ms_ = now_ms;
            return true;
        }
        ++rejected_;
        return false;
    }

    // §3.5 Pass 115 authority transfer. force_latch() is driven by the §11.4
    // CSA acceptance event: the claiming issuer takes the latch immediately,
    // without waiting out relatch_ms. The session is left unset (0) — the
    // first accepted report from that originator fills it in through the
    // same-originator rule in accept(). clear_latch() releases, so the next
    // reporter takes it within relatch_ms. Both are no-ops under a configured
    // preferred_originator: config outranks the override.
    // now_ms is required: it seeds the silence clock, so a report from a
    // DIFFERENT originator arriving right after the transfer is measured
    // against relatch_ms from the transfer instant. Leaving it at 0 would let
    // the displaced reporter satisfy the relatch check on its very next packet
    // and take the latch straight back.
    void force_latch(uint16_t originator, uint64_t now_ms) {
        if (p_.preferred_originator != 0) return;
        // 0 is the "no latch" sentinel in latched_originator(); latching to it
        // would report as unlatched in the very stat added to expose this.
        if (originator == 0) return;
        latched_ = {originator, 0};
        last_ms_ = now_ms;
    }

    void clear_latch() {
        if (p_.preferred_originator != 0) return;
        latched_.reset();
        last_ms_ = 0;
    }

    bool overridable() const { return p_.preferred_originator == 0; }

    // 0 = no latch. Under preferred_originator the configured node is the
    // holder by definition, whether or not it has reported yet.
    uint16_t latched_originator() const {
        if (p_.preferred_originator != 0) return p_.preferred_originator;
        return latched_ ? latched_->first : 0;
    }

    // §3.16 (Pass 153): the latched session, for the calibration family's
    // exact-tuple acceptance. 0 = unknown (no latch yet, or a §3.5 Pass 115
    // force_latch whose session the next accepted report will fill in) — the
    // caller treats 0 as "originator match suffices".
    uint32_t latched_session() const {
        return latched_ ? latched_->second : 0;
    }

    uint64_t rejected() const { return rejected_; }

  private:
    ReportGatePolicy p_;
    std::optional<std::pair<uint16_t, uint32_t>> latched_;
    uint64_t last_ms_ = 0;
    uint64_t rejected_ = 0;
};

}  // namespace wblink
