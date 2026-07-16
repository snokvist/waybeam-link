// SPDX-License-Identifier: GPL-2.0-or-later
// §3.5 acceptance filter (Pass 41): preferred-only mode, first-latcher
// mode with reboot-follow and silence-based re-latch.
#include "wblink/report_gate.h"

#include "wbtest.h"

using namespace wblink;

int main() {
    // Preferred mode: only the preferred originator passes, any session.
    {
        ReportGate g(ReportGatePolicy{9, 2000});
        CHECK(g.accept(9, 100, 1000));
        CHECK(!g.accept(10, 100, 1001));  // spoof/second reporter dropped
        CHECK(g.accept(9, 101, 1002));    // preferred rebooted — follows
        CHECK(!g.accept(10, 101, 9999));  // silence never admits others
        CHECK_EQ_U(g.rejected(), 2);
    }

    // First-latcher: first reporter wins; same-originator reboot follows;
    // a different originator needs relatch_ms of latched silence.
    {
        ReportGate g(ReportGatePolicy{0, 2000});
        CHECK(g.accept(7, 50, 1000));     // latch
        CHECK(!g.accept(8, 60, 1500));    // fresh latch holds
        CHECK(g.accept(7, 51, 2000));     // reboot of the latched node
        CHECK(!g.accept(8, 60, 3900));    // 1900 ms silence: still held
        CHECK(g.accept(8, 60, 4100));     // 2100 ms silence: re-latched
        CHECK(!g.accept(7, 51, 4200));    // the old node is now the outsider
        CHECK_EQ_U(g.rejected(), 3);
    }

    return wbtest_finish("report_gate_test");
}
