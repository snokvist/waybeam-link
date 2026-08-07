// SPDX-License-Identifier: GPL-2.0-or-later
// §15.5a (Pass 161) scout evidence store — see scout_store.h.
#include "wblink/scout_store.h"

#include <algorithm>

#include "wblink/types.h"  // link_verdict (§3.16)

namespace wblink {

const char* scout_rec_reason_name(ScoutRecReason r) {
    switch (r) {
        case ScoutRecReason::kOk:
            return "OK";
        case ScoutRecReason::kNoEvidence:
            return "NO_EVIDENCE";
        case ScoutRecReason::kNoQualified:
            return "NO_QUALIFIED";
        case ScoutRecReason::kNoImprovement:
            return "NO_IMPROVEMENT";
        case ScoutRecReason::kBroadDegradation:
            return "BROAD_DEGRADATION";
        case ScoutRecReason::kActiveWeak:
            return "ACTIVE_WEAK";
        case ScoutRecReason::kActiveSaturated:
            return "ACTIVE_SATURATED";
    }
    return "NO_EVIDENCE";
}

ScoutStore::Bin* ScoutStore::bin_of(uint16_t chan_mhz) {
    for (Bin& b : bins_) {
        if (b.chan_mhz == chan_mhz) return &b;
    }
    bins_.push_back(Bin{});
    bins_.back().chan_mhz = chan_mhz;
    return &bins_.back();
}

void ScoutStore::begin_sweep(const std::string& domain,
                             const std::vector<uint16_t>& channels) {
    // §15.5a trust boundary: a foreign calibration domain resets the store
    // rather than silently mixing adapters' energy indexes.
    if (domain != domain_) {
        if (!domain_.empty()) {
            ++domain_resets_;
        }
        bins_.clear();
        rounds_ = 0;
        domain_ = domain;
    }
    for (Bin& b : bins_) {
        b.in_universe = false;  // re-declared below; retired bins keep
                                // ranking but stop vetoing rounds
    }
    for (const uint16_t c : channels) {
        bin_of(c)->in_universe = true;
    }
}

void ScoutStore::fold(const ScoutSample& s) {
    if (s.util_permille > 1000) {  // structurally implausible
        ++rej_implausible_;
        return;
    }
    Bin* b = bin_of(s.chan_mhz);
    b->ring[b->next] = s;
    b->next = (b->next + 1) % ScoutStorePolicy::kRingDepth;
    if (b->n < ScoutStorePolicy::kRingDepth) ++b->n;
    b->fresh_this_round = true;
    // The chanmig anti-starvation guard: the round advances only when
    // EVERY bin has been observed since the last advance.
    bool all = true;
    for (const Bin& bb : bins_) {
        if (bb.in_universe && !bb.fresh_this_round) {
            all = false;
            break;
        }
    }
    if (all) {
        ++rounds_;
        for (Bin& bb : bins_) {
            bb.fresh_this_round = false;
        }
    }
}

ScoutRanking ScoutStore::rank(uint64_t now_ms, uint16_t resting_chan,
                              uint8_t active_verdict) const {
    ScoutRanking out;
    out.rounds = rounds_;
    uint16_t resting_score = 0;
    bool have_resting = false;
    for (const Bin& b : bins_) {
        // Fresh samples only; stale ones are counted, never silently aged.
        uint16_t vals[ScoutStorePolicy::kRingDepth];
        size_t n = 0;
        uint64_t last = 0;
        for (size_t i = 0; i < b.n; ++i) {
            const ScoutSample& s = b.ring[i];
            if (now_ms >= s.at_ms &&
                now_ms - s.at_ms > policy_.max_evidence_age_ms) {
                ++out.stale_samples;  // gauge at this read, not cumulative
                continue;
            }
            vals[n++] = s.util_permille;
            last = std::max(last, s.at_ms);
        }
        if (n == 0) continue;
        std::sort(vals, vals + n);
        ScoutBinRank r;
        r.chan_mhz = b.chan_mhz;
        r.score = vals[n / 2];  // q50 (upper median)
        // q90 - q50 on the sorted array; the q90 index is >= the median
        // index for every n, so this cannot go negative.
        r.burstiness =
            static_cast<uint16_t>(vals[std::min(n - 1, (n * 9) / 10)] -
                                  r.score);
        r.samples = static_cast<uint8_t>(n);
        r.qualified = r.score <= policy_.qualify_max_util_permille;
        r.last_seen_ms = last;
        if (b.chan_mhz == resting_chan) {
            resting_score = r.score;
            have_resting = true;
        }
        out.bins.push_back(r);
    }
    // Score ascending; deterministic lowest-MHz tie-break (Pass 66 class).
    std::sort(out.bins.begin(), out.bins.end(),
              [](const ScoutBinRank& a, const ScoutBinRank& b) {
                  return a.score != b.score ? a.score < b.score
                                            : a.chan_mhz < b.chan_mhz;
              });
    // Confidence: rounds-degraded, and zeroed with the evidence. Low rounds
    // degrade the answer, never suppress it (§15.5a).
    if (!out.bins.empty()) {
        const uint32_t r = std::min(rounds_, policy_.min_rounds);
        const uint32_t denom = policy_.min_rounds ? policy_.min_rounds : 1;
        // rounds can be 0 with usable evidence (a partial first sweep) —
        // floor at one sample's worth so the answer is never 0-confidence
        // while backed by fresh data.
        out.confidence_permille = static_cast<uint16_t>(std::max<uint32_t>(
            1000u * r / denom, policy_.confidence_floor_permille));
    }

    // Recommendation — reasons in refusal-priority order.
    if (out.bins.empty()) {
        out.reason = ScoutRecReason::kNoEvidence;
        return out;
    }
    // §15.5a one-classifier rule (#98): a fresh Weak/Saturated verdict on
    // the active link means the impairment is not channel-attributable —
    // refuse the recommendation, keep the list.
    if (active_verdict == link_verdict::kWeak) {
        out.reason = ScoutRecReason::kActiveWeak;
        return out;
    }
    if (active_verdict == link_verdict::kSaturated) {
        out.reason = ScoutRecReason::kActiveSaturated;
        return out;
    }
    size_t unqualified = 0;
    for (const ScoutBinRank& r : out.bins) {
        if (!r.qualified) ++unqualified;
    }
    // A lone unqualified bin is NO_QUALIFIED, never "broad" — broadness
    // needs more than one opinion.
    if (out.bins.size() > 1 &&
        unqualified * 1000 >=
            static_cast<size_t>(policy_.broad_degrade_permille) *
                out.bins.size()) {
        out.reason = ScoutRecReason::kBroadDegradation;
        return out;
    }
    const ScoutBinRank& best = out.bins.front();
    if (!best.qualified) {
        out.reason = ScoutRecReason::kNoQualified;
        return out;
    }
    // The margin binds only against a measured resting score; without one
    // (resting channel never swept) it is waived, not invented.
    if (have_resting && best.chan_mhz != resting_chan &&
        resting_score < best.score + policy_.improvement_margin_permille) {
        out.reason = ScoutRecReason::kNoImprovement;
        return out;
    }
    out.reason = ScoutRecReason::kOk;
    out.recommended_chan = best.chan_mhz;
    return out;
}

}  // namespace wblink
