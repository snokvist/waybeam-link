// SPDX-License-Identifier: GPL-2.0-or-later
// §3.5 acceptance filter (Pass 41): preferred-only mode, first-latcher
// mode with reboot-follow and silence-based re-latch.
#include "wblink/report_gate.h"
#include "wblink/selector.h"

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

    // Gate transitions must reset the selector's reporter/epoch domain; a
    // unit-only gate success is insufficient if the consumer stays latched.
    {
        ReportGate g(ReportGatePolicy{0, 2000});
        Selector selector(SelectorPolicy{}, nullptr);
        const auto feed = [&](uint16_t originator, uint32_t session,
                              uint32_t epoch, uint64_t now) {
            LinkReport r;
            r.prefix.originator = originator;
            r.prefix.session_id = session;
            r.report_epoch = epoch;
            if (!g.accept(originator, session, now)) {
                return false;
            }
            return selector.on_report(r, now);
        };
        CHECK(feed(7, 50, 100, 1000));
        CHECK(!feed(7, 50, 99, 1100));  // replay never freshens selector
        CHECK_EQ_U(selector.report_age_ms(1100), 100);
        CHECK(feed(7, 51, 1, 2000));    // same node reboot: epoch restarts
        CHECK_EQ_U(selector.report_age_ms(2000), 0);
        CHECK_EQ_U(selector.report_epoch(), 1);
        CHECK(feed(8, 60, 1, 4101));    // silence-based source re-latch
        CHECK_EQ_U(selector.report_age_ms(4101), 0);
        CHECK_EQ_U(selector.report_epoch(), 1);
    }

    return wbtest_finish("report_gate_test");
}
