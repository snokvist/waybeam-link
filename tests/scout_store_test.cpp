// SPDX-License-Identifier: GPL-2.0-or-later
// §15.5a (Pass 161) scout evidence store: fold/replace semantics, rounds,
// staleness, the trust boundary, hysteresis and the verdict refusal — pure
// logic with injected time (issue #100 verification list).
#include "wblink/scout_store.h"

#include <string>
#include <vector>

#include "wblink/types.h"
#include "wbtest.h"

using namespace wblink;

namespace {

ScoutSample sample(uint16_t chan, uint16_t util, uint64_t at) {
    ScoutSample s;
    s.chan_mhz = chan;
    s.util_permille = util;
    s.at_ms = at;
    return s;
}

void sweep(ScoutStore& st, const std::string& dom, uint64_t at,
           std::initializer_list<std::pair<uint16_t, uint16_t>> bins) {
    std::vector<uint16_t> chans;
    for (const auto& [chan, util] : bins) chans.push_back(chan);
    st.begin_sweep(dom, chans);
    for (const auto& [chan, util] : bins) {
        st.fold(sample(chan, util, at));
    }
}

}  // namespace

int main() {
    const std::string dom = "mac/20:0d:b0:c4:a7:6a";

    // --- sweeps fold, rounds accrue, second sweep does not replace ----------
    {
        ScoutStore st;
        sweep(st, dom, 1000, {{5805, 100}, {5825, 300}});
        CHECK_EQ_U(st.rounds(), 1);
        sweep(st, dom, 61000, {{5805, 120}, {5825, 280}});
        CHECK_EQ_U(st.rounds(), 2);
        const ScoutRanking r = st.rank(62000, 0, 0);
        CHECK_EQ_U(r.bins.size(), 2);
        CHECK_EQ_U(r.bins[0].chan_mhz, 5805);
        CHECK_EQ_U(r.bins[0].samples, 2);  // folded, not replaced
        CHECK_EQ_U(r.rounds, 2);
        CHECK_EQ_U(r.confidence_permille, 1000);
    }

    // --- a partial sweep does not advance the round (anti-starvation) -------
    {
        ScoutStore st;
        sweep(st, dom, 1000, {{5805, 100}, {5825, 300}});
        st.begin_sweep(dom, {5805, 5825});
        st.fold(sample(5805, 90, 2000));  // 5825 not revisited
        CHECK_EQ_U(st.rounds(), 1);
        // low rounds degrade confidence, never suppress the answer
        const ScoutRanking r = st.rank(3000, 0, 0);
        CHECK(r.reason == ScoutRecReason::kOk);
        CHECK(r.confidence_permille < 1000);
        CHECK(r.confidence_permille > 0);
    }

    // --- staleness: old evidence stops counting, and is counted -------------
    {
        ScoutStore st;
        sweep(st, dom, 1000, {{5805, 100}});
        const uint64_t later = 1000 + 16 * 60 * 1000;  // past the 15 min bound
        ScoutRanking r = st.rank(later, 0, 0);
        CHECK(r.reason == ScoutRecReason::kNoEvidence);
        CHECK(r.stale_samples > 0);
        // gauge, not cumulative: a second read reports the same number
        const ScoutRanking r_again = st.rank(later, 0, 0);
        CHECK_EQ_U(r_again.stale_samples, r.stale_samples);
        // fresh evidence revives the bin
        st.begin_sweep(dom, {5805});
        st.fold(sample(5805, 100, later));
        r = st.rank(later + 1, 0, 0);
        CHECK(r.reason == ScoutRecReason::kOk);
    }

    // --- trust boundary: foreign domain resets; implausible rejected --------
    {
        ScoutStore st;
        sweep(st, dom, 1000, {{5805, 100}, {5825, 300}});
        st.begin_sweep("mac/40:a5:ef:2f:23:08", {5805});
        CHECK_EQ_U(st.domain_resets(), 1);
        CHECK_EQ_U(st.rounds(), 0);  // reset with the store
        st.fold(sample(5805, 50, 2000));
        CHECK_EQ_U(st.rounds(), 1);  // ...and the new domain accrues afresh
        const ScoutRanking r = st.rank(3000, 0, 0);
        CHECK_EQ_U(r.bins.size(), 1);  // only the new domain's evidence
        st.fold(sample(5805, 1500, 2100));
        CHECK_EQ_U(st.rejected_implausible(), 1);
    }

    // --- hysteresis: qualify bar, margin vs resting, tie-break --------------
    {
        ScoutStore st;
        // resting 5805 at 150; candidate 5785 at 100 — under the 80 margin.
        sweep(st, dom, 1000, {{5805, 150}, {5785, 100}});
        ScoutRanking r = st.rank(2000, 5805, 0);
        CHECK(r.reason == ScoutRecReason::kNoImprovement);
        // candidate clearly better than resting → recommended
        sweep(st, dom, 3000, {{5805, 400}, {5785, 100}});
        r = st.rank(4000, 5805, 0);
        CHECK(r.reason == ScoutRecReason::kOk);
        CHECK_EQ_U(r.recommended_chan, 5785);
        // deterministic tie-break: equal scores pick the lowest MHz
        ScoutStore st2;
        sweep(st2, dom, 1000, {{5825, 100}, {5765, 100}});
        const ScoutRanking r2 = st2.rank(2000, 0, 0);
        CHECK_EQ_U(r2.bins[0].chan_mhz, 5765);
        CHECK_EQ_U(r2.recommended_chan, 5765);
    }

    // --- shrinking universe: retired bins stop vetoing rounds ---------------
    {
        ScoutStore st;
        sweep(st, dom, 1000, {{5745, 100}, {5805, 100}, {5825, 100}});
        CHECK_EQ_U(st.rounds(), 1);
        // narrower sweeps thereafter: 5745 retired, rounds must still accrue
        sweep(st, dom, 2000, {{5805, 110}, {5825, 120}});
        CHECK_EQ_U(st.rounds(), 2);
        sweep(st, dom, 3000, {{5805, 110}, {5825, 120}});
        CHECK_EQ_U(st.rounds(), 3);
        // ...while the retired bin keeps ranking while fresh
        const ScoutRanking r = st.rank(4000, 0, 0);
        CHECK_EQ_U(r.bins.size(), 3);
    }

    // --- a lone unqualified bin reads NO_QUALIFIED, never "broad" -----------
    {
        ScoutStore st;
        sweep(st, dom, 1000, {{5805, 900}});
        const ScoutRanking r = st.rank(2000, 0, 0);
        CHECK(r.reason == ScoutRecReason::kNoQualified);
    }

    // --- broad degradation: everything busy → hold, not a jump --------------
    {
        ScoutStore st;
        sweep(st, dom, 1000, {{5805, 600}, {5825, 700}, {5785, 90}});
        // 2 of 3 unqualified = 667‰ < 700‰ seed → not broad yet, 5785 wins
        ScoutRanking r = st.rank(2000, 0, 0);
        CHECK(r.reason == ScoutRecReason::kOk);
        ScoutStore st2;
        sweep(st2, dom, 1000, {{5805, 600}, {5825, 700}, {5785, 800}});
        r = st2.rank(2000, 0, 0);
        CHECK(r.reason == ScoutRecReason::kBroadDegradation);
    }

    // --- #98 verdict reuse: Weak/Saturated refuses the recommendation only --
    {
        ScoutStore st;
        sweep(st, dom, 1000, {{5805, 100}, {5785, 50}});
        ScoutRanking r = st.rank(2000, 0, link_verdict::kSaturated);
        CHECK(r.reason == ScoutRecReason::kActiveSaturated);
        CHECK_EQ_U(r.bins.size(), 2);  // the list survives the refusal
        r = st.rank(2000, 0, link_verdict::kWeak);
        CHECK(r.reason == ScoutRecReason::kActiveWeak);
        r = st.rank(2000, 0, link_verdict::kHealthy);
        CHECK(r.reason == ScoutRecReason::kOk);
        r = st.rank(2000, 0, link_verdict::kUnknown);
        CHECK(r.reason == ScoutRecReason::kOk);  // absence of evidence
    }

    // --- burstiness: a transient burst widens q90-q50, score holds ----------
    {
        ScoutStore st;
        st.begin_sweep(dom, {5805});
        for (int i = 0; i < 7; ++i) {
            st.fold(sample(5805, 100, 1000 + i));
        }
        st.fold(sample(5805, 900, 1007));  // one burst dwell
        const ScoutRanking r = st.rank(2000, 0, 0);
        CHECK_EQ_U(r.bins[0].score, 100);  // median unmoved by one burst
        CHECK(r.bins[0].burstiness >= 700);
        CHECK(r.reason == ScoutRecReason::kOk);  // ranking did not flip
    }

    return wbtest_finish("scout_store_test");
}
