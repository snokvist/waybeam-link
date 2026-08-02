// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §7.3 RX metric reporter — builds one LINK_REPORT per
// latched stream at the report cadence (~10 Hz seed). The RX reports, it
// does not decide (§9.1): every field is a measurement pulled from the
// RxEngine; the TX-side Selector turns them into actions.
//
// loss_postdiv_prearq is WINDOWED per report (delta of lost/uniq counters
// since the previous report, in ‰) — the selector's reactive-demote reads
// fresh loss, not a lifetime average. `uniq` is the same interval denominator
// (§3.5 Pass 110); diversity remains cumulative. report_epoch is monotonic.
//
// Like NACKs, the built reports carry only the target identity; the caller
// stamps its own common prefix (originator/session) and encodes/injects.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "wblink/rx.h"
#include "wblink/wire.h"

namespace wblink {

struct ReporterPolicy {
    uint32_t interval_ms = 100;  // §7.3 ~10 Hz seed
};

class Reporter {
  public:
    Reporter(const ReporterPolicy& policy,
             std::optional<uint8_t> local_table_version)
        : policy_(policy), tv_(local_table_version) {}

    // §10.7 (Pass 132) probe burst. A calibration dwell needs an EXACT
    // denominator, and the cheapest way to get one is for the ground to stop
    // inferring how many reports it sent and simply send a COUNTED burst.
    // While a budget is set, build() ignores its cadence gate and emits on
    // every call until the budget is spent, then emits nothing at all — that
    // silence is the drain window in which the craft's counter settles before
    // the dwell is scored.
    //
    // This is the whole reason §10.7 no longer needs the craft's
    // `last_report_epoch` as a denominator, and with it goes the anchoring
    // identity, the local-epoch blackout fallback, and that fallback's
    // scoring rule — the three mechanisms Passes 126 and 128 found defects in.
    // Burst probes are synthetic traffic, which §10.7 forbade; that rule was
    // written for a flying craft, and calibration is stationary and pre-flight.
    void set_probe_budget(uint32_t n) {
        probing_ = true;
        probe_budget_ = n;
    }
    void clear_probe_mode() {
        probing_ = false;
        probe_budget_ = 0;
    }
    bool probing() const { return probing_; }

    // Reports due now, one per latched stream (empty between cadence ticks,
    // or once a probe burst is spent).
    std::vector<LinkReport> build(const RxEngine& engine, uint64_t now_ms);

    // §3.5: `report_epoch` advances once per EMITTED report, and §10.7's loss
    // identity divides by the craft's delta of exactly this field — so an
    // epoch spent on a report the radio never took is phantom loss on the
    // ground's seek. build() therefore leaves the field at 0; the caller
    // stamps `next_epoch()` into the encoded frame at the radio call and
    // commits only on success. A report dropped between build and injection
    // (no uplink adapter, TX queue full) burns no number, and the next report
    // reuses it.
    uint32_t next_epoch() const { return epoch_ + 1; }
    void commit_epoch() { ++epoch_; }
    uint32_t epoch() const { return epoch_; }
    void reset_link() { last_.clear(); }

  private:
    struct Snap {
        uint64_t uniq = 0;
        uint64_t lost = 0;
    };

    ReporterPolicy policy_;
    std::optional<uint8_t> tv_;
    uint32_t epoch_ = 0;
    uint64_t next_ms_ = 0;
    bool probing_ = false;
    uint32_t probe_budget_ = 0;
    std::map<uint64_t, Snap> last_;  // packed stream key -> last window end
};

}  // namespace wblink
