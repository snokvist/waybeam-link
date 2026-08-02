// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/reporter.h"

#include <algorithm>

namespace wblink {

namespace {
uint64_t pack(const StreamKey& k) {
    return (static_cast<uint64_t>(k.originator) << 40) |
           (static_cast<uint64_t>(k.session_id) << 8) | k.stream_id;
}
}  // namespace

std::vector<LinkReport> Reporter::build(const RxEngine& engine,
                                        uint64_t now_ms) {
    std::vector<LinkReport> out;
    if (probing_) {
        // §10.7 burst: emit on every call until the counted budget is spent,
        // then go silent so the craft's counter can settle before scoring.
        if (probe_budget_ == 0) return out;
        // Keep the ordinary cadence clock running THROUGH the burst. Left
        // stale, next_ms_ is already in the past when clear_probe_mode()
        // returns, so the first post-burst build() fires immediately instead
        // of on cadence — an unpaced report straight after a run.
        next_ms_ = now_ms + policy_.interval_ms;
    } else if (now_ms < next_ms_) {
        return out;
    } else {
        next_ms_ = now_ms + policy_.interval_ms;
    }

    // Adapter-level RF summary (§7.3): best = strongest recent packet across
    // live adapters; mean = average of the per-adapter EWMAs.
    int8_t best = 0;
    int32_t mean_sum = 0;
    int mean_n = 0;
    for (const auto& [id, a] : engine.adapters()) {
        if (a.stalled || !a.have_rssi) {
            continue;
        }
        if (mean_n == 0 || a.rssi_last > best) {
            best = a.rssi_last;
        }
        mean_sum += a.rssi_ewma;
        ++mean_n;
    }
    const int8_t mean =
        mean_n > 0 ? static_cast<int8_t>(mean_sum / mean_n)
                   : static_cast<int8_t>(0);
    const uint8_t live = engine.live_adapter_count();

    for (const RxStreamInfo& s : engine.streams()) {
        // §7.3 Pass 79: reports are the §9 selector's feedback channel and
        // are emitted for RTP streams only — a low-rate stream's per-stream
        // loss fraction must not steer selection.
        if (s.stream_type != stream_type::kRtp) {
            continue;
        }
        Snap& prev = last_[pack(s.key)];
        const uint64_t d_uniq = s.counters.uniq - prev.uniq;
        const uint64_t d_lost = s.counters.lost_declared - prev.lost;
        prev.uniq = s.counters.uniq;
        prev.lost = s.counters.lost_declared;
        const uint64_t denom = d_uniq + d_lost;

        LinkReport r;  // prefix stamped by the caller
        r.target_originator = s.key.originator;
        r.target_session = s.key.session_id;
        r.target_stream_id = s.key.stream_id;
        r.report_epoch = 0;  // stamped at injection (§3.5/§10.7)
        r.table_version = tv_.value_or(0);
        r.rssi_best = best;
        r.rssi_mean = mean;
        r.loss_postdiv_prearq =
            denom == 0 ? static_cast<uint16_t>(0)
                       : static_cast<uint16_t>(d_lost * 1000 / denom);
        r.uniq = static_cast<uint32_t>(
            std::min<uint64_t>(denom, 0xFFFFFFFFull));
        r.diversity = static_cast<uint32_t>(s.counters.diversity);
        r.adapters = live;
        r.probe_per = kNoProbe;  // §9.4: no probe machinery in v0
        r.recommended_prof = 0;
        out.push_back(r);
    }
    // Spend the budget on what was BUILT. A report the radio then refuses to
    // take burns budget but no epoch (§3.5 commits on injection only), so the
    // burst is at most `n` frames and the ground's own epoch delta stays the
    // exact count of what actually went out — which is the denominator.
    if (probing_) {
        probe_budget_ =
            out.size() >= probe_budget_
                ? 0
                : probe_budget_ - static_cast<uint32_t>(out.size());
    }
    return out;
}

}  // namespace wblink
