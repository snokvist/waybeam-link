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

    // Reports due now, one per latched stream (empty between cadence ticks).
    std::vector<LinkReport> build(const RxEngine& engine, uint64_t now_ms);

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
    std::map<uint64_t, Snap> last_;  // packed stream key -> last window end
};

}  // namespace wblink
