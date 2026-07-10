// SPDX-License-Identifier: GPL-2.0-or-later
// §7.2 quiet-gap pacer: craft window gating (guard offset, window length,
// backlog override), ground return-deadline math (TSF anchor, u32 wrap,
// past-window clamp, no-TSF fallback), disabled = transparent.
#include "wblink/quietgap.h"

#include "wbtest.h"

using namespace wblink;

int main() {
    const QuietGapPolicy pol{true, 300, 2000, 4};

    // --- craft side ----------------------------------------------------------
    {
        QuietGap g(pol);
        // No EOB yet: always clear.
        CHECK(g.can_send_video(0));
        g.note_eob_sent(10'000);
        // Guard region [eob, eob+300) is still send-clear (the radio is
        // just turning around); the quiet window is [10300, 12300).
        CHECK(g.can_send_video(10'000));
        CHECK(g.can_send_video(10'299));
        CHECK(!g.can_send_video(10'300));
        CHECK(!g.can_send_video(11'000));
        CHECK(!g.can_send_video(12'299));
        CHECK(g.can_send_video(12'300));
        CHECK_EQ_U(g.gap_end_us(), 12'300);
        // Airtime-critical override: backlog at/above the threshold sends.
        CHECK(!g.can_send_video(11'000, 3));
        CHECK(g.can_send_video(11'000, 4));
        // A later EOB re-arms.
        g.note_eob_sent(20'000);
        CHECK(!g.can_send_video(20'500));
    }

    // --- ground side ---------------------------------------------------------
    {
        QuietGap g(pol);
        // Target = guard + window/2 = 1300 µs after the EOB instant.
        // TSF says 200 µs already elapsed → fire in 1100 µs.
        CHECK_EQ_U(g.return_deadline(50'000, 1'000'000,
                                     uint64_t{1'000'200}),
                   51'100);
        // No TSF read: anchor at host arrival (elapsed 0).
        CHECK_EQ_U(g.return_deadline(50'000, 1'000'000, std::nullopt),
                   51'300);
        // Window middle already passed → immediate.
        CHECK_EQ_U(g.return_deadline(50'000, 1'000'000,
                                     uint64_t{1'002'000}),
                   50'000);
        // u32 wrap across the tsfl boundary: eob at 0xFFFFFF00, now-TSF 0x120
        // → elapsed 0x220 = 544 µs → fire in 1300−544.
        CHECK_EQ_U(g.return_deadline(50'000, 0xFFFFFF00u, uint64_t{0x120}),
                   50'000 + (1'300 - 544));
        // TSF garbage (appears to have gone backwards → huge u32 delta):
        // clamps to immediate, never blocks the return.
        CHECK_EQ_U(g.return_deadline(50'000, 5'000, uint64_t{4'000}),
                   50'000);
    }

    // --- disabled = transparent ---------------------------------------------
    {
        QuietGap off;  // default policy: enabled=false
        off.note_eob_sent(1'000);
        CHECK(off.can_send_video(1'500));
        CHECK_EQ_U(off.return_deadline(9'000, 123, uint64_t{456}), 9'000);
    }

    return wbtest_finish("quietgap_test");
}
