// SPDX-License-Identifier: GPL-2.0-or-later
// §7.2 aim instrumentation (issue #99 stage 1) — moved with AirBackend in
// #109 Phase 2a because AirBackend writes the ReadTsf histogram.
//
// Tier-2 bench knob: no spec surface, findings.md 2026-08-07.
//
// The two accumulators are `inline` variables, not the file-static ones they
// were in main.cpp. In a single translation unit those are the same thing; in
// a header included by more than one they are NOT, and a per-TU copy would
// mean AirBackend accumulating into one histogram while run_rx dumps another.
// The move is what makes the distinction matter.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace wblink {
namespace node {

// §7.2 aim instrumentation (issue #99 stage 1, Tier-2 bench knob — no spec
// surface, findings.md 2026-08-07): WBLINK_AIM_LOG=1 histograms (a) release
// lateness past the computed return deadline and (b) the ReadTsf() control
// transfer cost — the §7.2 error term with no measured number — dumped to
// stderr every 30 s. Distributions, not means: the tail is the contract.
struct AimHist {
    uint64_t n = 0, sum_us = 0, max_us = 0;
    uint64_t b[8] = {};  // <50 <100 <200 <500 <1000 <2000 <5000 >=5000
    void add(uint64_t us) {
        static constexpr uint64_t edge[7] = {50, 100, 200, 500,
                                             1000, 2000, 5000};
        ++n;
        sum_us += us;
        if (us > max_us) max_us = us;
        size_t i = 0;
        while (i < 7 && us >= edge[i]) ++i;
        ++b[i];
    }
    void dump(const char* name) const {
        if (n == 0) return;
        std::fprintf(stderr,
                     "aim: %s n=%llu mean=%lluus max=%lluus "
                     "buckets[<50,<100,<200,<500,<1k,<2k,<5k,>=5k]="
                     "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                     name, (unsigned long long)n,
                     (unsigned long long)(sum_us / n),
                     (unsigned long long)max_us, (unsigned long long)b[0],
                     (unsigned long long)b[1], (unsigned long long)b[2],
                     (unsigned long long)b[3], (unsigned long long)b[4],
                     (unsigned long long)b[5], (unsigned long long)b[6],
                     (unsigned long long)b[7]);
    }
};

inline bool aim_log_enabled() {
    static const bool on = std::getenv("WBLINK_AIM_LOG") != nullptr;
    return on;
}

// rx-role only: the 30 s dump lives in the run_rx loop — a tx node with
// the flag set collects and never prints (findings.md says so).
inline AimHist g_aim_release;   // release lateness past the return deadline
inline AimHist g_aim_read_tsf;  // ReadTsf() control-transfer cost

}  // namespace node
}  // namespace wblink
