// SPDX-License-Identifier: GPL-2.0-or-later
// §9.11 FPS ladder (Pass 39): initial preferred command, distress-gated
// reduction with dwell, slow restore, stale-feedback hold, envelope bounds.
#include "wblink/fps_ladder.h"

#include "wbtest.h"

using namespace wblink;

namespace {
FpsLadderPolicy fast() {
    FpsLadderPolicy p;
    p.min_fps = 60;
    p.preferred_fps = 90;
    p.reduce_after_ms = 300;
    p.reduce_dwell_ms = 400;
    p.restore_after_ms = 800;
    p.settle_ms = 100;
    p.report_timeout_ms = 500;
    return p;
}
}  // namespace

int main() {
    CHECK(fps_ladder_member(90));
    CHECK(!fps_ladder_member(50));

    // First tick commands preferred, then settles.
    {
        FpsLadder l(fast());
        auto f = l.tick(1000, false);
        CHECK(f && *f == 90);
        CHECK(!l.tick(1050, false).has_value());  // settle freeze
    }

    // Reduce: floor + sustained distress, dwell-limited, never below min.
    {
        FpsLadder l(fast());
        l.tick(1000, false);
        for (uint64_t t = 1100; t <= 4000; t += 100) {
            l.note_report(100, t);  // 10% loss
            const auto f = l.tick(t, /*at_floor=*/true);
            if (f) {
                // Steps must be adjacent ladder members, descending.
                CHECK(*f == 75 || *f == 60);
            }
        }
        CHECK_EQ_U(l.current_fps(), 60);  // clamped at min
        // Still distressed: no further command below min.
        l.note_report(100, 4100);
        CHECK(!l.tick(4100, true).has_value());
    }

    // Stale feedback holds: no reports => neither reduce nor restore.
    {
        FpsLadder l(fast());
        l.tick(1000, false);
        for (uint64_t t = 1200; t <= 3000; t += 100) {
            CHECK(!l.tick(t, true).has_value());  // distress but no report
        }
        CHECK_EQ_U(l.current_fps(), 90);
    }

    // Restore: off-floor + low loss, slower than reduce, back to preferred.
    {
        FpsLadder l(fast());
        l.tick(1000, false);
        for (uint64_t t = 1200; t <= 4000; t += 100) {  // drive to 60
            l.note_report(100, t);
            l.tick(t, true);
        }
        CHECK_EQ_U(l.current_fps(), 60);
        uint64_t first_restore = 0;
        for (uint64_t t = 4100; t <= 9000; t += 100) {
            l.note_report(0, t);
            const auto f = l.tick(t, /*at_floor=*/false);
            if (f && first_restore == 0) {
                first_restore = t;
                CHECK_EQ_U(*f, 75);
            }
        }
        CHECK_EQ_U(l.current_fps(), 90);  // never above preferred
        CHECK(first_restore >= 4100 + 700);  // restore_after gates the step
        // Healthy forever: no command past preferred.
        l.note_report(0, 9100);
        CHECK(!l.tick(9100, false).has_value());
    }

    // Moderate loss off-floor is neither distress nor healthy: hold.
    {
        FpsLadder l(fast());
        l.tick(1000, false);
        for (uint64_t t = 1200; t <= 4000; t += 100) {
            l.note_report(10, t);  // between restore(5) and distress(20)
            CHECK(!l.tick(t, false).has_value());
        }
        CHECK_EQ_U(l.current_fps(), 90);
    }

    return wbtest_finish("fps_ladder_test");
}
