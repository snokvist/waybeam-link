// SPDX-License-Identifier: GPL-2.0-or-later
// §9 adaptive selector: cascade rules in isolation, §9.5 sequencing order,
// settle suppression, §9.7 flap layers, §9.8 fail-safe, §9.9 pressure, §9.2
// max-probability demote target, the Pass-6 derived-bitrate law, and the §10
// power resolve. All fake time. Note demotes surface bitrate-first: the
// profile commit lands bitrate_lead_ms (500) after the decision.
#include "wblink/selector.h"

#include <string_view>
#include <utility>
#include <vector>

#include "wblink/power.h"
#include "wbtest.h"

using namespace wblink;

namespace {

ProfileTable make_table() {
    ProfileTable t;
    for (uint8_t i = 0; i < 8; ++i) {
        Profile p;
        p.id = i;
        p.mcs = i;
        p.gi = GuardInterval::kLong;
        p.tx_power_level = 4;
        p.airtime_budget_permille = 600;
        p.arq_deadline_iframe_ms = 80;
        p.arq_deadline_pframe_ms = 25;
        p.bitrate_min_kbps = 2200;
        p.reserve_control_bps = 64000;
        p.reserve_telemetry_bps = 32000;
        t.profiles.push_back(p);
    }
    t.floor_profile = 0;
    return t;
}

struct Harness {
    ProfileTable table = make_table();
    Selector sel;
    uint32_t epoch = 0;

    explicit Harness(const SelectorPolicy& p = SelectorPolicy{})
        : sel(p, &table) {}

    // Boot at t0 and swallow the bootstrap actions.
    void boot(uint64_t t0 = 0) {
        const SelectorActions a = sel.tick(t0);
        CHECK(a.commit.has_value());
        CHECK(a.bitrate_kbps.has_value());
    }

    void report(uint64_t now, int8_t rssi, uint16_t loss_milli) {
        LinkReport r;
        r.prefix.originator = 9;
        r.report_epoch = ++epoch;
        r.rssi_best = rssi;
        r.rssi_mean = rssi;
        r.loss_postdiv_prearq = loss_milli;
        r.adapters = 2;
        sel.on_report(r, now);
    }

    struct Log {
        std::vector<std::pair<uint64_t, uint8_t>> commits;    // (t, profile)
        std::vector<std::pair<uint64_t, uint32_t>> bitrates;  // (t, kbps)
    };
    // Reports at 100 ms cadence + ticks at `step`; collects actions.
    Log run(uint64_t from, uint64_t to, int8_t rssi, uint16_t loss_milli,
            uint32_t step = 10) {
        Log log;
        uint64_t next_report = from;
        for (uint64_t t = from; t <= to; t += step) {
            if (t >= next_report) {
                report(t, rssi, loss_milli);
                next_report = t + 100;
            }
            const SelectorActions a = sel.tick(t);
            if (a.commit) {
                log.commits.emplace_back(t, a.commit->profile_id);
            }
            if (a.bitrate_kbps) {
                log.bitrates.emplace_back(t, *a.bitrate_kbps);
            }
        }
        return log;
    }
};

}  // namespace

int main() {
    // --- Pass-6 derived bitrate law, golden values ---------------------------
    {
        const ProfileTable t = make_table();
        // MCS0 LGI: 6500 × 0.6 = 3900 − 96 = 3804 kbps.
        CHECK_EQ_U(derive_bitrate_kbps(t.profiles[0]), 3804);
        // MCS5: 52000 × 0.6 = 31200 − 96 = 31104.
        CHECK_EQ_U(derive_bitrate_kbps(t.profiles[5]), 31104);
        // Short GI bumps PHY by 10/9 before the fraction.
        Profile sg = t.profiles[2];
        sg.gi = GuardInterval::kShort;
        CHECK_EQ_U(derive_bitrate_kbps(sg), 19500ull * 10 / 9 * 600 / 1000 - 96);
        // Floor wins when the budget math lands below it.
        Profile tiny = t.profiles[0];
        tiny.airtime_budget_permille = 10;
        CHECK_EQ_U(derive_bitrate_kbps(tiny), 2200);
    }

    // --- §10 power resolve (level offset + ceiling) --------------------------
    {
        PowerCurve c;
        c.valid = true;
        for (size_t i = 0; i < 8; ++i) {
            c.qdb[i] = 80 - static_cast<int32_t>(i) * 4;
        }
        CHECK(*resolve_power_qdb(c, 0, 4) == 80);  // level 4 = the curve
        CHECK(*resolve_power_qdb(c, 0, 5) == 88);  // +1 level = +8 qdb
        CHECK(*resolve_power_qdb(c, 0, 0) == 48);  // −4 levels = −32 qdb
        CHECK(*resolve_power_qdb(c, 7, 4) == 52);
        CHECK(*resolve_power_qdb(c, 0, 7, 90) == 90);        // ceiling clamps
        CHECK(!resolve_power_qdb(c, 8, 4).has_value());      // out of HT range
        CHECK(!resolve_power_qdb(PowerCurve{}, 0, 4).has_value());  // unloaded
    }

    // --- bootstrap: starts at the floor profile ------------------------------
    {
        Harness h;
        const SelectorActions a = h.sel.tick(0);
        CHECK(a.commit && a.commit->profile_id == 0);
        CHECK(a.bitrate_kbps && *a.bitrate_kbps == 3804);
        CHECK_EQ_U(h.sel.profile_id(), 0);
    }

    // --- rule 5: promote climbs on strong RSSI; MCS leads bitrate by grace ---
    {
        Harness h;
        h.boot();
        const auto log = h.run(0, 8000, -40, 0);
        CHECK(h.sel.profile_id() >= 4);
        CHECK(!log.commits.empty());
        bool order_ok = !log.commits.empty();
        for (const auto& [tc, prof] : log.commits) {
            bool found = false;
            for (const auto& [tb, br] : log.bitrates) {
                if (tb >= tc + 250 && tb <= tc + 300) {
                    found = true;
                }
            }
            order_ok = order_ok && found;
        }
        CHECK(order_ok);  // §9.5 promote: bitrate lags every commit by grace
    }

    // --- rule 1: reactive demote; bitrate LEADS the commit by bitrate_lead ---
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;  // isolate rule 1 from settle
        Harness h(p);
        h.boot();
        (void)h.run(0, 6000, -40, 0);  // climb to the top and sit there
        const uint8_t before = h.sel.profile_id();
        CHECK_EQ_U(before, 7);
        const auto log = h.run(6000, 7500, -40, 100);  // 10% delivered loss
        CHECK(h.sel.profile_id() < before);
        CHECK(!log.bitrates.empty());
        CHECK(!log.commits.empty());
        // First action of the demote is the bitrate drop; the commit follows
        // bitrate_lead_ms (500) later.
        CHECK(log.bitrates.front().first + 500 <= log.commits.front().first + 10);
    }

    // --- settle suppresses rule 1; loss also gates promotes ------------------
    {
        Harness h;  // default mcs_settle_ms = 5000
        h.boot();
        (void)h.run(0, 1000, -40, 0);
        const uint8_t before = h.sel.profile_id();
        CHECK(before >= 1);
        (void)h.run(1000, 2500, -40, 500);  // huge loss inside settle
        CHECK_EQ_U(h.sel.profile_id(), before);  // no demote (settle), no promote (loss)
    }

    // --- rule 2: RSSI floor demotes even inside settle ------------------------
    {
        Harness h;
        h.boot();
        (void)h.run(0, 3000, -40, 0);
        const uint8_t before = h.sel.profile_id();
        (void)h.run(3000, 4500, -90, 0);  // below the −85 floor
        CHECK(h.sel.profile_id() < before);
    }

    // --- rule 3: fade (steep slope in the armed region, 3 ticks) --------------
    {
        Harness h;
        h.boot();
        (void)h.run(0, 3000, -40, 0);
        const uint8_t before = h.sel.profile_id();
        // Walk RSSI down ~−20 dB/s inside the armed region; loss stays clean.
        uint64_t t = 3000;
        for (int8_t r = -70; r >= -84; r = static_cast<int8_t>(r - 2)) {
            h.report(t, r, 0);
            (void)h.sel.tick(t);
            t += 100;
        }
        // Let a pending bitrate-lead demote commit.
        for (uint64_t e = t; e <= t + 700; e += 10) {
            (void)h.sel.tick(e);
        }
        CHECK(h.sel.profile_id() < before);
    }

    // --- §9.7 hard flap-freeze: 3 fast re-demotes pin below -------------------
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        p.promote_dwell_ms = 100;
        p.reentry_backoff_ms = 0;  // isolate the HARD freeze from soft reentry
        Harness h(p);
        h.boot();
        uint64_t t = 0;
        int drops = 0;
        uint8_t last = h.sel.profile_id();
        while (t < 30000 && !h.sel.flap_frozen(t)) {
            const bool losing = (t / 1000) % 2 == 1;  // alternate seconds
            h.report(t, -40, losing ? 200 : 0);
            (void)h.sel.tick(t);
            if (h.sel.profile_id() < last) {
                ++drops;
            }
            last = h.sel.profile_id();
            t += 100;
        }
        CHECK(h.sel.flap_frozen(t));  // the oscillation must trip the freeze
        // Flush any in-flight bitrate-lead demote, then: while frozen,
        // perfect RSSI must NOT promote.
        (void)h.run(t, t + 700, -40, 0);
        const uint8_t pinned = h.sel.profile_id();
        (void)h.run(t + 700, t + 2000, -30, 0);
        CHECK_EQ_U(h.sel.profile_id(), pinned);
    }

    // --- §9.7 soft reentry: re-promoting into a just-demoted rung is slower --
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        Harness h(p);
        h.boot();
        (void)h.run(0, 2000, -40, 0);
        const uint8_t high = h.sel.profile_id();
        CHECK(high >= 2);
        (void)h.run(2000, 2800, -40, 100);  // demote
        (void)h.run(2800, 3400, -40, 0);    // flush pending + decay loss EWMA
        const uint8_t low = h.sel.profile_id();
        CHECK(low < high);
        // 0.5 s of clean air would satisfy the normal dwell — but this rung
        // was just demoted from, so reentry demands 2 s.
        (void)h.run(3400, 3800, -40, 0);
        CHECK_EQ_U(h.sel.profile_id(), low);
        (void)h.run(3800, 7000, -40, 0);
        CHECK(h.sel.profile_id() > low);
    }

    // --- min==max pin ----------------------------------------------------------
    {
        SelectorPolicy p;
        p.min_profile = 3;
        p.max_profile = 3;
        Harness h(p);
        h.boot();
        (void)h.run(0, 3000, -30, 0);
        CHECK_EQ_U(h.sel.profile_id(), 3);
        (void)h.run(3000, 4500, -95, 500);
        CHECK_EQ_U(h.sel.profile_id(), 3);
        CHECK(std::string_view(h.sel.state()) == "PINNED");
    }

    // --- §9.8 fail-safe: hold, then damped descent; stale never promotes -----
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        Harness h(p);
        h.boot();
        (void)h.run(0, 3000, -40, 0);
        CHECK(h.sel.profile_id() >= 2);
        // Total silence from t=3000. A promote can still legitimately land
        // inside report_timeout (data is fresh enough), so capture the
        // baseline once the link is definitively stale.
        uint64_t t = 3000;
        uint8_t before = 0;
        uint8_t during_hold = 255;
        for (; t <= 5400; t += 100) {
            (void)h.sel.tick(t);
            if (t == 3600) {
                before = h.sel.profile_id();  // stale: hold phase begins
            }
            if (t == 3900) {
                during_hold = h.sel.profile_id();  // inside timeout+hold
            }
        }
        CHECK_EQ_U(during_hold, before);         // held first...
        CHECK(h.sel.profile_id() < before);      // ...then stepped down
        CHECK(std::string_view(h.sel.state()) == "FAILSAFE");
        for (; t <= 25000; t += 100) {
            (void)h.sel.tick(t);
        }
        CHECK_EQ_U(h.sel.profile_id(), 0);  // reached the floor and stays
        // A replayed stale epoch never freshens the link.
        LinkReport stale;
        stale.prefix.originator = 9;
        stale.report_epoch = 1;
        stale.rssi_mean = -30;
        h.sel.on_report(stale, t);
        (void)h.sel.tick(t);
        CHECK(std::string_view(h.sel.state()) == "FAILSAFE");
    }

    // --- §9.9 pressure: suppresses rule 1 only; escape climbs ------------------
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        p.rung_rssi_floor_dbm.fill(-30);  // block rule-5 promotes entirely
        Harness h(p);
        h.boot();
        const uint8_t base = h.sel.profile_id();
        h.sel.set_pressure(true, 0);
        // Loss under pressure: rule 1 must NOT demote (encoder overshoot,
        // §9.9)... and at the floor there is nothing to demote to anyway, so
        // run this from a raised start via the escape first.
        (void)h.run(0, 3000, -40, 0);  // escape climbs despite blocked rule 5
        const uint8_t up = h.sel.profile_id();
        CHECK(up > base);
        h.sel.set_pressure(false, 3000);
        h.sel.set_pressure(true, 3000);
        (void)h.run(3000, 3600, -40, 300);  // loss while pressured: no demote
        CHECK_EQ_U(h.sel.profile_id(), up);
        // An RSSI floor breach still demotes under pressure (rule 2).
        (void)h.run(3600, 5600, -90, 300);
        CHECK(h.sel.profile_id() < up);
    }

    // --- §9.2: multi-rung stress demotes TOWARD the max-prob rung -------------
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        p.rung_age_rate = 0.0;  // freeze priors so history dominates
        Harness h(p);
        h.boot();
        (void)h.run(0, 4500, -40, 0);  // climb; low rungs logged clean
        const uint8_t high = h.sel.profile_id();
        CHECK(high >= 4);
        // Heavy loss at the top: the demote target must jump several rungs
        // toward the historically-clean region, not just high−1.
        (void)h.run(4500, 6000, -40, 800);
        CHECK(h.sel.profile_id() < high - 1);
    }

    // --- §11.3 CSA freeze: cascade halted, watchdog blackout excused ----------
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        Harness h(p);
        h.boot();
        // Climb to the ladder top and settle — no in-flight transition left
        // (the freeze halts new decisions; a mid-flight sequenced commit is
        // allowed to land, so the test must start from quiescence).
        (void)h.run(0, 9000, -40, 0);
        const uint8_t before = h.sel.profile_id();
        CHECK_EQ_U(before, 7);
        // Freeze 3 s; reports stop entirely (retune blackout).
        h.sel.csa_freeze(9000 + 3000);
        for (uint64_t t = 9000; t <= 12000; t += 10) {
            const SelectorActions a = h.sel.tick(t);
            CHECK(!a.commit.has_value());  // no demote/promote in the freeze
        }
        const std::string_view st = h.sel.state();
        CHECK(st == "CSA_FREEZE" || st == "HOLD");
        CHECK_EQ_U(h.sel.profile_id(), before);
        // Just past the freeze, still no report for a moment: the blackout is
        // excused — no instant FAILSAFE descent (report_timeout is 500 ms
        // default; 200 ms after the freeze end must still be fine).
        for (uint64_t t = 12010; t <= 12200; t += 10) {
            const SelectorActions a = h.sel.tick(t);
            CHECK(!a.commit.has_value());
        }
        CHECK(std::string_view(h.sel.state()) != "FAILSAFE");
        // Reports resume: normal operation continues at the same rung.
        (void)h.run(12200, 12600, -40, 0);
        CHECK_EQ_U(h.sel.profile_id(), before);
    }

    return wbtest_finish("selector_test");
}
