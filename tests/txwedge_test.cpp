// SPDX-License-Identifier: GPL-2.0-or-later
// §9.10 TX-wedge watchdog: fires only on completion-progress ABSENCE while
// submissions advance; a progress deficit never trips it; idle windows hold
// the previous verdict; disabled = transparent. Completion is CCX on
// devourer. (The kernel-monitor netdev source was deleted in Pass 164.)
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
        // Prime with a nonzero report count: §9.10 (Pass 170) withholds the
        // verdict until this backend is PROVEN to complete frames, so a
        // wedge case must describe a backend that reports.
        w.poll(0, 0, 1);
        CHECK(!w.poll(1000, 7, 1));  // 7 < min_submits: no verdict
        CHECK(!w.wedged());
        CHECK(w.poll(2000, 100, 1));  // now enough evidence
        CHECK(w.wedged());
        CHECK(!w.poll(3000, 103, 1));  // idle again: stays wedged
        CHECK(w.wedged());
    }

    // Exactly min_submits with zero reports fires; one report saves it.
    {
        TxWedge w(pol);
        // Prime with a nonzero report count: §9.10 (Pass 170) withholds the
        // verdict until this backend is PROVEN to complete frames, so a
        // wedge case must describe a backend that reports.
        w.poll(0, 0, 1);
        CHECK(w.poll(1000, 8, 1));
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
        // Prime with a nonzero report count: §9.10 (Pass 170) withholds the
        // verdict until this backend is PROVEN to complete frames, so a
        // wedge case must describe a backend that reports.
        w.poll(0, 0, 1);
        CHECK(w.consecutive_wedged() == 0);
        w.poll(1000, 100, 1);  // wedged window 1
        CHECK(w.consecutive_wedged() == 1);
        w.poll(2000, 200, 1);  // wedged window 2
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
        // Prime with a nonzero report count: §9.10 (Pass 170) withholds the
        // verdict until this backend is PROVEN to complete frames, so a
        // wedge case must describe a backend that reports.
        w.poll(0, 0, 1);
        w.poll(1000, 100, 1);
        CHECK(w.consecutive_wedged() == 1);
        w.poll(2000, 101, 1);  // 1 submit — below min_submits, no verdict
        CHECK(w.consecutive_wedged() == 1);
        CHECK(w.wedged());
        w.poll(3000, 201, 1);  // back to real submissions, still no progress
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

    // §9.10 (Pass 170): a backend that has NEVER completed a frame is not a
    // wedged backend. Verbatim shape of the 2026-08-14 CV610 measurement —
    // the RTL8733BU emits no CCX reports at all, so tx_progress stays 0 while
    // tx_submitted climbs; the old rule called that a dead transmitter while
    // a second radio was receiving 22087 frames from it.
    {
        TxWedge w(pol);
        w.poll(0, 0, 0);  // no reports at prime: unproven
        CHECK(!w.progress_proven());
        uint64_t sub = 0;
        for (uint64_t t = 1000; t <= 20'000; t += 1000) {
            sub += 660;  // the measured ~660 submits/s, zero reports ever
            CHECK(!w.poll(t, sub, 0));
            CHECK(!w.wedged());
        }
        CHECK_EQ_U(w.wedge_windows(), 0);
        CHECK_EQ_U(w.consecutive_wedged(), 0);
        CHECK(!w.progress_proven());
    }

    // ...and the detector is WITHHELD, not disabled: the same backend, once it
    // has completed even one frame, wedges exactly as before. Without this the
    // fix above would be indistinguishable from deleting the watchdog.
    {
        TxWedge w(pol);
        w.poll(0, 0, 0);
        CHECK(!w.poll(1000, 100, 0));  // unproven — no verdict yet
        CHECK(!w.wedged());
        CHECK(!w.poll(2000, 200, 1));  // one completion: proven, and alive
        CHECK(w.progress_proven());
        CHECK(w.poll(3000, 300, 1));   // now reports stop -> a REAL wedge
        CHECK(w.wedged());
        CHECK_EQ_U(w.wedge_windows(), 1);
    }

    // Proof can also arrive at the prime, for a watchdog armed after the
    // backend was already reporting — otherwise a late arm would spend a
    // window unable to judge a chip that is plainly completing.
    {
        TxWedge w(pol);
        w.poll(0, 500, 40);  // already reporting when armed
        CHECK(w.progress_proven());
        CHECK(w.poll(1000, 600, 40));
        CHECK(w.wedged());
    }

    return wbtest_finish("txwedge_test");
}
