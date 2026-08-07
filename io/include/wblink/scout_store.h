// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §15.5a (Pass 161) scout evidence store — sweeps FOLD
// into per-bin rings instead of replacing, and the ranking gains
// hysteresis, confidence and enumerated reasons. Pure logic, injected
// time, no radio — the chanmig *shapes* with our values as §17 seeds
// (issue #100; their engine deliberately not adopted).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wblink {

// §17 RE-DERIVE seeds (Pass 161). The shapes are chanmig's; the values are
// this fleet's to measure.
struct ScoutStorePolicy {
    uint64_t max_evidence_age_ms = 15 * 60 * 1000;
    uint32_t min_rounds = 2;  // full confidence at/after this many rounds
    uint16_t qualify_max_util_permille = 200;
    uint16_t improvement_margin_permille = 80;
    uint16_t broad_degrade_permille = 700;
    static constexpr size_t kRingDepth = 8;
};

// One dwell's occupancy evidence for one bin. util is the Pass 155
// interference-inclusive total the ranking keys on.
struct ScoutSample {
    uint16_t chan_mhz = 0;
    uint16_t util_permille = 0;
    uint64_t at_ms = 0;
};

// Enumerated values, never prose (§15.5a Pass 161 explainability bar).
enum class ScoutRecReason : uint8_t {
    kOk = 0,               // recommendation.chan is the pick
    kNoEvidence = 1,       // store empty (or everything stale)
    kNoQualified = 2,      // no bin under the qualify bar
    kNoImprovement = 3,    // best bin does not clear the margin vs resting
    kBroadDegradation = 4, // most bins busy: not channel-attributable
    kActiveWeak = 5,       // §3.16 verdict: range problem, not the channel
    kActiveSaturated = 6,  // §3.16 verdict: self-jam, not the channel
};
const char* scout_rec_reason_name(ScoutRecReason r);

struct ScoutBinRank {
    uint16_t chan_mhz = 0;
    uint16_t score = 0;       // q50 util_permille over fresh samples
    uint16_t burstiness = 0;  // q90 - q50 (what one dwell cannot see)
    uint8_t samples = 0;      // fresh samples backing the score
    bool qualified = false;
    uint64_t last_seen_ms = 0;
};

struct ScoutRanking {
    std::vector<ScoutBinRank> bins;  // score ascending, ties by lowest MHz
    ScoutRecReason reason = ScoutRecReason::kNoEvidence;
    uint16_t recommended_chan = 0;   // valid only when reason == kOk
    uint32_t rounds = 0;
    uint16_t confidence_permille = 0;  // rounds- and freshness-degraded
};

class ScoutStore {
  public:
    explicit ScoutStore(const ScoutStorePolicy& p = {}) : policy_(p) {}

    // Declare a sweep's channel universe under a calibration-domain key
    // (the scout adapter's EFUSE MAC, else its index as text). A foreign
    // domain RESETS the store — raw energy indexes compare only within
    // one adapter (§15.5a). Declares (creates) every bin up front so the
    // round guard sees the full universe before the first fold — a
    // dynamically-appearing bin must not let a partial sweep read as a
    // completed round.
    void begin_sweep(const std::string& domain,
                     const std::vector<uint16_t>& channels);
    // Fold one dwell sample. Structurally implausible samples (permille >
    // 1000) are rejected and counted.
    void fold(const ScoutSample& s);

    // Rank on the fresh evidence. resting_chan's own score is the margin
    // baseline (0 = no resting score available: margin waived).
    // active_verdict is the node's §3.16 value (0 = Unknown) — a fresh
    // Weak/Saturated refuses the recommendation, never the list.
    ScoutRanking rank(uint64_t now_ms, uint16_t resting_chan,
                      uint8_t active_verdict) const;

    // Rejection accounting (§15.5a: "no candidates" must be
    // distinguishable from "everything rejected").
    uint64_t rejected_implausible() const { return rej_implausible_; }
    uint64_t domain_resets() const { return domain_resets_; }
    uint64_t rejected_stale() const { return rej_stale_; }
    const std::string& domain() const { return domain_; }
    uint32_t rounds() const { return rounds_; }

  private:
    struct Bin {
        uint16_t chan_mhz = 0;
        ScoutSample ring[ScoutStorePolicy::kRingDepth];
        size_t n = 0;     // valid entries
        size_t next = 0;  // ring cursor
        bool fresh_this_round = false;
    };
    Bin* bin_of(uint16_t chan_mhz);

    ScoutStorePolicy policy_;
    std::vector<Bin> bins_;
    std::string domain_;
    uint32_t rounds_ = 0;
    uint64_t rej_implausible_ = 0;
    uint64_t domain_resets_ = 0;
    mutable uint64_t rej_stale_ = 0;  // counted at rank time (age is a
                                      // property of "now", not of the fold)
};

}  // namespace wblink
