// SPDX-License-Identifier: GPL-2.0-or-later
// §15.3 (Pass 198) trailing-window loss — the "what is the link doing NOW"
// half of the two loss ratios.
//
// §3.7's `loss_prediversity_milli` and `loss_postdiv_prearq_milli` are
// cumulative since latch. That makes them a session average whose inertia
// grows without bound: measured on the x86 ground, the pre-diversity figure
// moved 33.6 -> 33.4 across 90 s and 88,000 delivered frames. A burst now
// barely moves it; a transient minutes ago is still on screen. Worse for an
// operator, a scout sweep parks one ear off-channel and bakes a 100%-loss
// stretch into the average for the rest of the session, so an OSD loss bar
// driven by it can sit at alert on a link that is delivering cleanly.
//
// A trailing window rather than an EWMA: "loss in the last N ms" is what a
// loss bar claims to show, and it forgets COMPLETELY rather than trailing a
// tail past the event.
#pragma once

#include <cstdint>
#include <vector>

namespace wblink {
namespace node {

class LossWindow {
   public:
    struct Out {
        uint32_t pre_milli = 0;
        uint32_t post_milli = 0;
        // Did the window actually contain opportunities this call, or is
        // pre_milli a HELD value? A caller comparing several windows against
        // each other must exclude the held ones: a silent ear holds whatever
        // it last measured, and if that was 0 it wins any minimum forever
        // while the one live ear is losing. Measured before this flag existed:
        // ear A losing 300 permille as the only live ear, dead ear B holding
        // 0, min() reporting 0 — a bar claiming a perfect link.
        bool pre_valid = false;
        bool post_valid = false;
    };

    explicit LossWindow(uint64_t window_ms, size_t max_samples = 64)
        : window_ms_(window_ms), max_samples_(max_samples) {}

    // Feed the CUMULATIVE counters as of `now`; get loss over the trailing
    // window. Anchored on the oldest sample still inside it, so the span is at
    // most window_ms however irregularly the caller ticks — the stats cadence
    // is config (`stats.hz`), not a constant, and a caller may stall.
    //
    // An empty denominator HOLDS the previous value instead of reading 0. A
    // link that has gone silent expects nothing and loses nothing, and "0%
    // loss" is the one answer that must never appear for a dead link.
    Out update(uint64_t now, uint64_t prediv_expected, uint64_t prediv_lost,
               uint64_t uniq, uint64_t lost_declared) {
        const Sample cur{now, prediv_expected, prediv_lost, uniq,
                         lost_declared};
        // Age out first, THEN read the oldest survivor: that is the window's
        // near edge. Keep one sample even when everything has aged out, so a
        // caller that stalls longer than the window still has an anchor and
        // resumes measuring immediately rather than after a second tick.
        const uint64_t cutoff = now > window_ms_ ? now - window_ms_ : 0;
        size_t drop = 0;
        while (drop + 1 < samples_.size() && samples_[drop].t_ms < cutoff) {
            ++drop;
        }
        if (drop != 0) {
            samples_.erase(samples_.begin(),
                           samples_.begin() + static_cast<long>(drop));
        }
        if (!samples_.empty()) {
            const Sample& old_s = samples_.front();
            // Counters are monotonic, but a stream that re-latches resets
            // them; a negative delta would wrap to something enormous, so
            // treat any backward step as a restart and re-anchor.
            if (cur.prediv_expected < old_s.prediv_expected ||
                cur.prediv_lost < old_s.prediv_lost || cur.uniq < old_s.uniq ||
                cur.lost_declared < old_s.lost_declared) {
                samples_.clear();
                last_ = Out{};
            } else {
                const uint64_t d_exp = cur.prediv_expected - old_s.prediv_expected;
                const uint64_t d_lost = cur.prediv_lost - old_s.prediv_lost;
                last_.pre_valid = d_exp != 0;
                if (last_.pre_valid) {
                    last_.pre_milli = static_cast<uint32_t>(d_lost * 1000 / d_exp);
                }
                const uint64_t d_uniq = cur.uniq - old_s.uniq;
                const uint64_t d_decl = cur.lost_declared - old_s.lost_declared;
                last_.post_valid = d_uniq + d_decl != 0;
                if (last_.post_valid) {
                    last_.post_milli = static_cast<uint32_t>(
                        d_decl * 1000 / (d_uniq + d_decl));
                }
            }
        }
        samples_.push_back(cur);
        // Bound the ring against a caller ticking far faster than the window.
        if (samples_.size() > max_samples_) {
            samples_.erase(samples_.begin());
        }
        return last_;
    }

   private:
    struct Sample {
        uint64_t t_ms = 0;
        uint64_t prediv_expected = 0;
        uint64_t prediv_lost = 0;
        uint64_t uniq = 0;
        uint64_t lost_declared = 0;
    };
    uint64_t window_ms_;
    size_t max_samples_;
    std::vector<Sample> samples_;
    Out last_;
};

}  // namespace node
}  // namespace wblink
