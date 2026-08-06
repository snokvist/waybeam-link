// SPDX-License-Identifier: GPL-2.0-or-later
// §9.10 TX-wedge watchdog: fires only on completion-progress ABSENCE while
// submissions advance; a progress deficit never trips it; idle windows hold
// the previous verdict; disabled = transparent. Completion is CCX on
// devourer and netdev tx_packets on kernel-monitor.
#include "wblink/txwedge.h"

#include "wbtest.h"

using namespace wblink;

int main() {
    const TxWedgePolicy pol{1000, 8};

    // Healthy saturated load: reports at ~25% of submits (the measured
    // 4500 pps floor) must NEVER fire the detector.
    {
        TxWedge w(pol);
        CHECK(w.enabled());
        uint64_t sub = 0;
        uint64_t rep = 0;
        CHECK(!w.poll(0, sub, rep));  // priming call, no verdict
        for (uint64_t t = 1000; t <= 10'000; t += 1000) {
            sub += 4500;
            rep += 4500 / 4;
            CHECK(!w.poll(t, sub, rep));
            CHECK(!w.wedged());
        }
        CHECK_EQ_U(w.wedge_windows(), 0);
    }

    // Wedge: submissions advance, reports freeze — fires on the first full
    // window (one transition), stays latched without re-transitioning.
    {
        TxWedge w(pol);
        w.poll(0, 100, 40);
        CHECK(!w.poll(999, 200, 40));  // window not yet elapsed
        CHECK(w.poll(1000, 200, 40));  // Δsub=100 ≥ 8, Δrep=0 → wedged
        CHECK(w.wedged());
        CHECK(!w.poll(2000, 300, 40));  // still wedged, no new transition
        CHECK(w.wedged());
        CHECK_EQ_U(w.wedge_windows(), 2);
        // Recovery: any report clears it (one transition back).
        CHECK(w.poll(3000, 400, 41));
        CHECK(!w.wedged());
    }

    // Idle windows (too few submissions) hold the previous verdict in both
    // directions — an idle TX is not evidence.
    {
        TxWedge w(pol);
        w.poll(0, 0, 0);
        CHECK(!w.poll(1000, 7, 0));  // 7 < min_submits: no verdict
        CHECK(!w.wedged());
        CHECK(w.poll(2000, 100, 0));  // now enough evidence
        CHECK(w.wedged());
        CHECK(!w.poll(3000, 103, 0));  // idle again: stays wedged
        CHECK(w.wedged());
    }

    // Exactly min_submits with zero reports fires; one report saves it.
    {
        TxWedge w(pol);
        w.poll(0, 0, 0);
        CHECK(w.poll(1000, 8, 0));
        CHECK(w.wedged());
    }
    {
        TxWedge w(pol);
        w.poll(0, 0, 0);
        CHECK(!w.poll(1000, 8, 1));
        CHECK(!w.wedged());
    }

    // §9.10 v2 (Pass 148): consecutive_wedged() drives the TX self-restart, so
    // it must count CONSECUTIVE windows, not the lifetime total — a craft that
    // wedged briefly twice in a flight must not accumulate its way to a
    // restart with no fault present.
    {
        TxWedge w(pol);
        w.poll(0, 0, 0);
        CHECK(w.consecutive_wedged() == 0);
        w.poll(1000, 100, 0);  // wedged window 1
        CHECK(w.consecutive_wedged() == 1);
        w.poll(2000, 200, 0);  // wedged window 2
        CHECK(w.consecutive_wedged() == 2);
        // Any backend progress clears it, even while the lifetime total keeps
        // its history. Both counters are CUMULATIVE, so a healthy window has
        // to advance the progress value, not merely be non-zero.
        w.poll(3000, 300, 5);
        CHECK(w.consecutive_wedged() == 0);
        CHECK(w.wedge_windows() == 2);
        w.poll(4000, 400, 10);
        CHECK(w.consecutive_wedged() == 0);
        w.poll(5000, 500, 15);
        CHECK(w.consecutive_wedged() == 0);
        CHECK(w.wedge_windows() == 2);  // lifetime total unchanged by progress
        // A second, separate episode counts from zero rather than resuming.
        w.poll(6000, 600, 15);
        CHECK(w.consecutive_wedged() == 1);
        CHECK(w.wedge_windows() == 3);
    }
    // An idle window holds the consecutive count rather than clearing it: an
    // idle TX is evidence of nothing, exactly as it is for the verdict itself.
    {
        TxWedge w(pol);
        w.poll(0, 0, 0);
        w.poll(1000, 100, 0);
        CHECK(w.consecutive_wedged() == 1);
        w.poll(2000, 101, 0);  // 1 submit — below min_submits, no verdict
        CHECK(w.consecutive_wedged() == 1);
        CHECK(w.wedged());
        w.poll(3000, 201, 0);  // back to real submissions, still no progress
        CHECK(w.consecutive_wedged() == 2);
    }

    // window_ms = 0 disables the watchdog entirely.
    {
        TxWedge w(TxWedgePolicy{0, 8});
        CHECK(!w.enabled());
        CHECK(!w.poll(0, 0, 0));
        CHECK(!w.poll(5000, 100'000, 0));
        CHECK(!w.wedged());
    }

    return wbtest_finish("txwedge_test");
}
