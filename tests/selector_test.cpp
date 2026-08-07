// SPDX-License-Identifier: GPL-2.0-or-later
// §9 adaptive selector: cascade rules in isolation, §9.5 sequencing order,
// Pass 110 loss classification/lockout, §9.7 flap layers, §9.8 fail-safe,
// §9.9 pressure, the Pass-6 derived-bitrate law, and the §10 power resolve.
// All fake time. Note demotes surface bitrate-first: the
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

    void report(uint64_t now, int8_t rssi, uint16_t loss_milli,
                uint32_t uniq = 100) {
        LinkReport r;
        r.prefix.originator = 9;
        r.prefix.session_id = 1;
        r.report_epoch = ++epoch;
        r.rssi_best = rssi;
        r.rssi_mean = rssi;
        r.loss_postdiv_prearq = loss_milli;
        r.uniq = uniq;
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

    // --- §9.6 Pass 75: encoder-capability ceiling clamp ----------------------
    {
        CHECK_EQ_U(clamp_bitrate_kbps(34570, 25000), 25000);  // over → capped
        CHECK_EQ_U(clamp_bitrate_kbps(12903, 25000), 12903);  // under → intact
        CHECK_EQ_U(clamp_bitrate_kbps(25000, 25000), 25000);  // at the ceiling
        CHECK_EQ_U(clamp_bitrate_kbps(34570, 0), 34570);      // 0 = unlimited
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

    // --- §9.4 Pass 160: a fresh Saturated verdict suppresses every climb ----
    {
        Harness h;
        h.boot();
        // Establish steady strong-RSSI reports, then plant Saturated: the
        // exact regime the RSSI-margin promote would climb in.
        (void)h.run(0, 1000, -40, 0);
        const uint8_t before = h.sel.profile_id();
        h.sel.on_verdict(link_verdict::kSaturated, 1000);
        (void)h.run(1000, 3500, -40, 0);
        CHECK_EQ_U(h.sel.profile_id(), before);  // pinned while fresh
        CHECK(h.sel.promote_blocked_saturated() > 0);
        // TTL (seed 3000 ms) expires with no refresh → climbing resumes.
        (void)h.run(4200, 8000, -40, 0);
        CHECK(h.sel.profile_id() > before);
    }
    {
        // Healthy and Unknown gate nothing; Saturated never blocks demote.
        Harness h;
        h.boot();
        h.sel.on_verdict(link_verdict::kHealthy, 0);
        (void)h.run(0, 4000, -40, 0);
        const uint8_t climbed = h.sel.profile_id();
        CHECK(climbed > 1);  // Healthy did not gate the climb
        h.sel.on_verdict(link_verdict::kSaturated, 4000);
        (void)h.run(4000, 5500, -90, 0);  // RSSI floor demotes regardless
        CHECK(h.sel.profile_id() < climbed);
    }

    // --- Pass 110 persistent loss: five windows, exactly one rung ------------
    {
        SelectorPolicy p;
        Harness h(p);
        h.boot();
        (void)h.run(0, 6000, -40, 0);  // climb to the top and sit there
        const uint8_t before = h.sel.profile_id();
        CHECK_EQ_U(before, 7);
        const auto log = h.run(6000, 6900, -40, 100);
        CHECK_EQ_U(h.sel.profile_id(), before - 1);
        CHECK(!log.bitrates.empty());
        CHECK(!log.commits.empty());
        CHECK(h.sel.reason() == SelectorReason::kLossPersistent);
        CHECK(std::string_view(selector_reason_name(h.sel.reason())) ==
              "LOSS_PERSISTENT");
        const SelectorLockout lock = h.sel.lockout(6900);
        CHECK(lock.active && !lock.latched);
        CHECK_EQ_U(lock.profile, before);
        CHECK_EQ_U(lock.strikes, 1);
        CHECK(log.bitrates.front().first + 500 <= log.commits.front().first + 10);
    }

    // --- one moderate spike decays; it is neither persistent nor emergency --
    {
        Harness h;
        h.boot();
        (void)h.run(0, 1000, -40, 0);
        const uint8_t before = h.sel.profile_id();
        CHECK(before >= 1);
        h.report(1010, -40, 100);
        (void)h.sel.tick(1010);
        (void)h.run(1110, 1800, -40, 0);
        CHECK(h.sel.profile_id() >= before);
        CHECK_EQ_U(h.sel.lockout(1800).active_mask, 0);
    }

    // --- one acute qualified window bypasses settle/pressure to safe floor ---
    {
        Harness h;
        h.boot();
        (void)h.run(0, 6000, -40, 0);
        CHECK_EQ_U(h.sel.profile_id(), 7);
        h.sel.set_pressure(true, 6000);
        h.report(6010, -40, 500, 100);
        const SelectorActions lead = h.sel.tick(6010);
        CHECK(lead.bitrate_kbps.has_value());
        CHECK(!lead.commit.has_value());
        CHECK(h.sel.reason() == SelectorReason::kLossEmergency);
        const SelectorActions commit = h.sel.tick(6510);
        CHECK(commit.commit && commit.commit->profile_id == 0);
        CHECK_EQ_U(h.sel.profile_id(), 0);
        const SelectorLockout lock = h.sel.lockout(6510);
        CHECK(lock.active && lock.profile == 7);
    }

    // --- acute fail-safe resolves to the mode floor, not literal MCS0 --------
    {
        SelectorPolicy p;
        p.min_profile = 2;
        p.max_profile = 5;
        Harness h(p);
        h.boot();
        CHECK_EQ_U(h.sel.safe_floor_profile(), 2);
        (void)h.run(0, 5000, -35, 0);
        CHECK_EQ_U(h.sel.profile_id(), 5);
        h.report(5010, -35, 500);
        (void)h.sel.tick(5010);
        const SelectorActions commit = h.sel.tick(5510);
        CHECK(commit.commit && commit.commit->profile_id == 2);
        CHECK_EQ_U(h.sel.profile_id(), 2);
    }

    // --- high percentage without enough samples cannot emergency-demote -----
    {
        Harness h;
        h.boot();
        (void)h.run(0, 6000, -40, 0);
        const uint8_t before = h.sel.profile_id();
        h.report(6010, -40, 900, 3);
        (void)h.sel.tick(6010);
        (void)h.sel.tick(6600);
        CHECK_EQ_U(h.sel.profile_id(), before);
        CHECK_EQ_U(h.sel.lockout(6600).active_mask, 0);
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
            const bool fading = (t / 1000) % 2 == 1;  // alternate seconds
            h.report(t, fading ? -90 : -40, 0);
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
        (void)h.run(2000, 2800, -90, 0);    // RSSI demote (no §9.2 lockout)
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

    // --- §9.7 runtime re-pin snaps the operating point (bench lever) ----------
    {
        Harness h;
        h.boot();
        // Climb to a high rung on a strong, clean link.
        (void)h.run(0, 8000, -30, 0);
        const uint8_t climbed = h.sel.profile_id();
        CHECK(climbed >= 5);

        // Apply a runtime pin and drain any in-flight promote/bitrate phase so
        // evaluate() (which only runs in kIdle) actually sees the pin. Returns
        // the profile committed while landing on the pin (0xFF if none seen).
        // Capture the bitrate emitted alongside the pin commit: the pinned
        // rung's rate MUST move with it (else venc stays at the prior rung's
        // rate — measured as a ~3.6x MCS0 oversubscription on hardware).
        uint32_t pin_bitrate = 0;
        auto repin_to = [&](uint8_t rung, uint64_t t0) -> uint8_t {
            h.sel.set_profile_pin(rung, rung);
            uint8_t committed = 0xFF;
            pin_bitrate = 0;
            for (uint64_t t = t0; t < t0 + 500; t += 10) {
                const SelectorActions a = h.sel.tick(t);
                if (a.commit) committed = a.commit->profile_id;
                if (a.bitrate_kbps) pin_bitrate = *a.bitrate_kbps;
                if (h.sel.profile_id() == rung &&
                    std::string_view(h.sel.state()) == "PINNED") {
                    break;
                }
            }
            return committed;
        };

        // Re-pin DOWN at runtime: must jump to the pinned rung, not freeze,
        // AND re-derive the bitrate to that rung.
        CHECK_EQ_U(repin_to(1, 8100), 1);
        CHECK_EQ_U(h.sel.profile_id(), 1);
        CHECK(std::string_view(h.sel.state()) == "PINNED");
        CHECK_EQ_U(pin_bitrate, derive_bitrate_kbps(h.table.profiles[1]));
        // Re-pin UP: the pin overrides adaptation in either direction, and the
        // bitrate follows up too.
        CHECK_EQ_U(repin_to(6, 8600), 6);
        CHECK_EQ_U(h.sel.profile_id(), 6);
        CHECK_EQ_U(pin_bitrate, derive_bitrate_kbps(h.table.profiles[6]));
        // Idempotent: re-evaluating an already-satisfied pin emits no commit.
        const SelectorActions same = h.sel.tick(9200);
        CHECK(!same.commit.has_value());
        CHECK_EQ_U(h.sel.profile_id(), 6);
        // Unpin ({max:255}) and confirm adaptation resumes off the pinned rung.
        h.sel.set_profile_pin(0, 255);
        (void)h.run(9300, 13000, -95, 500);
        CHECK(std::string_view(h.sel.state()) != "PINNED");
    }

    // --- §9.7 range re-pin clamp (Pass 100) ----------------------------------
    // DOWN-CLAMP: a range whose max is below the current rung snaps DOWN to max
    // immediately, even on a clean link (no loss/§9.8 trigger). Runs before the
    // failsafe check, so it needs no fresh feedback.
    {
        Harness h;
        h.boot();
        (void)h.run(0, 8000, -30, 0);          // climb high on a strong link
        CHECK(h.sel.profile_id() > 2);
        h.sel.set_profile_pin(0, 2);           // envelope excludes current
        uint8_t committed = 0xFF;
        uint32_t clamp_br = 0;
        for (uint64_t t = 8010; t < 8300; t += 10) {
            const SelectorActions a = h.sel.tick(t);  // no reports: pre-failsafe
            if (a.commit) committed = a.commit->profile_id;
            if (a.bitrate_kbps) clamp_br = *a.bitrate_kbps;
            if (h.sel.profile_id() == 2 &&
                std::string_view(h.sel.state()) == "REPIN") {
                break;
            }
        }
        CHECK_EQ_U(committed, 2);
        CHECK_EQ_U(h.sel.profile_id(), 2);          // snapped to hi, not stuck
        CHECK(std::string_view(h.sel.state()) == "REPIN");
        CHECK_EQ_U(clamp_br, derive_bitrate_kbps(h.table.profiles[2]));
    }
    // UP-CLAMP with fresh feedback: a range whose min is above the current rung
    // snaps UP to min — a promotion, allowed only because feedback is fresh.
    {
        Harness h;
        h.boot();
        h.sel.set_profile_pin(0, 0);           // sit at the floor
        (void)h.run(0, 2000, -30, 0);
        CHECK_EQ_U(h.sel.profile_id(), 0);
        h.sel.set_profile_pin(4, 6);           // raise the floor above current
        h.report(2001, -30, 0);                // keep feedback fresh
        const SelectorActions a = h.sel.tick(2001);
        CHECK(a.commit && a.commit->profile_id == 4);
        CHECK_EQ_U(h.sel.profile_id(), 4);          // snapped up to lo
        CHECK(std::string_view(h.sel.state()) == "REPIN");
    }
    // UP-CLAMP deferred under stale feedback: a raised min must NOT pull the
    // rung UP on a lost link (§9.8 "never fail optimistic"). It stays put.
    {
        Harness h;
        h.boot();
        h.sel.set_profile_pin(0, 0);
        (void)h.run(0, 2000, -30, 0);
        CHECK_EQ_U(h.sel.profile_id(), 0);
        h.sel.set_profile_pin(4, 6);           // raise floor, then let feedback rot
        const SelectorActions a = h.sel.tick(3000);  // > report_timeout stale
        CHECK(!(a.commit && a.commit->profile_id == 4));  // no up-clamp
        CHECK(h.sel.profile_id() != 4);             // did not promote on stale
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
        stale.prefix.session_id = 1;
        stale.report_epoch = 1;
        stale.rssi_mean = -30;
        h.sel.on_report(stale, t);
        (void)h.sel.tick(t);
        CHECK(std::string_view(h.sel.state()) == "FAILSAFE");
    }

    // --- §9.9 pressure suppresses persistent loss only; escape climbs --------
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        p.rung_rssi_floor_dbm.fill(-30);  // block rule-5 promotes entirely
        Harness h(p);
        h.boot();
        const uint8_t base = h.sel.profile_id();
        h.sel.set_pressure(true, 0);
        // Persistent loss under pressure must NOT demote (encoder overshoot,
        // §9.9)... and at the floor there is nothing to demote to anyway, so
        // run this from a raised start via the escape first.
        (void)h.run(0, 3000, -40, 0);  // escape climbs despite blocked rule 5
        const uint8_t up = h.sel.profile_id();
        CHECK(up > base);
        h.sel.set_pressure(false, 3000);
        h.sel.set_pressure(true, 3000);
        (void)h.run(3000, 3600, -40, 100);  // moderate loss under pressure
        CHECK_EQ_U(h.sel.profile_id(), up);
        // An RSSI floor breach still demotes under pressure (rule 2).
        (void)h.run(3600, 5600, -90, 100);
        CHECK(h.sel.profile_id() < up);
    }

    // --- §9.2 lockout gates promotion, expires, and retains strikes ----------
    {
        SelectorPolicy p;
        p.rung_lockout_ms = 1000;
        Harness h(p);
        h.boot();
        (void)h.run(0, 6000, -40, 0);
        const uint8_t high = h.sel.profile_id();
        CHECK_EQ_U(high, 7);
        (void)h.run(6000, 6900, -40, 100);
        CHECK_EQ_U(h.sel.profile_id(), 6);
        CHECK(h.sel.lockout(6900).active);
        // Strong clean RSSI cannot cross the active ceiling.
        (void)h.run(6900, 7900, -30, 0);
        CHECK_EQ_U(h.sel.profile_id(), 6);
        // After expiry the rung is eligible again, but its strike is retained.
        (void)h.run(8000, 9000, -30, 0);
        CHECK_EQ_U(h.sel.profile_id(), 7);
        CHECK_EQ_U(h.sel.lockout(9000).active_mask, 0);
        CHECK_EQ_U(h.sel.loss_score(), 0);
        // The old event did not rebuild evidence during the bitrate-lead
        // phase. A recurrence starts from one new bad window, not score 5.
        h.report(9010, -30, 100);
        (void)h.sel.tick(9010);
        CHECK_EQ_U(h.sel.profile_id(), 7);
        CHECK_EQ_U(h.sel.loss_score(), 1);
    }

    // --- multiple bad rungs: the lowest lock is the no-skip ceiling ----------
    {
        SelectorPolicy p;
        p.rung_lockout_ms = 10000;
        Harness h(p);
        h.boot();
        (void)h.run(0, 6000, -40, 0);
        CHECK_EQ_U(h.sel.profile_id(), 7);
        for (uint64_t t = 6100; t <= 6500; t += 100) {
            h.report(t, -40, 100);
            (void)h.sel.tick(t);
        }
        (void)h.sel.tick(7000);  // commit 7 -> 6
        CHECK_EQ_U(h.sel.profile_id(), 6);
        for (uint64_t t = 7100; t <= 7500; t += 100) {
            h.report(t, -40, 100);
            (void)h.sel.tick(t);
        }
        (void)h.sel.tick(8000);  // commit 6 -> 5
        CHECK_EQ_U(h.sel.profile_id(), 5);
        const SelectorLockout lock = h.sel.lockout(8000);
        CHECK_EQ_U(lock.profile, 6);
        CHECK_EQ_U(lock.ceiling_profile, 5);
        CHECK_EQ_U(lock.active_mask, 0xC0);

        // An accepted reporter identity change clears channel-conditioned
        // evidence and names the reset cause.
        LinkReport fresh;
        fresh.prefix.originator = 10;
        fresh.prefix.session_id = 2;
        fresh.report_epoch = 1;
        fresh.rssi_best = -40;
        fresh.rssi_mean = -40;
        fresh.uniq = 100;
        CHECK(h.sel.on_report(fresh, 8010));
        CHECK_EQ_U(h.sel.lockout(8010).active_mask, 0);
        CHECK(h.sel.reason() == SelectorReason::kEnvironmentReset);
    }

    // --- §9.2 fourth strike latches; RF tuple change clears all evidence -----
    {
        SelectorPolicy p;
        p.rung_lockout_ms = 1000;
        p.promote_dwell_ms = 0;
        p.reentry_backoff_ms = 0;
        p.reentry_dwell_ms = 0;
        p.flap_freeze_count = 255;  // isolate rung latch from §9.7 freeze
        Harness h(p);
        h.boot();
        CHECK(!h.sel.on_rf_environment(5220, 20, 0));  // baseline only
        (void)h.run(0, 6000, -40, 0);
        CHECK_EQ_U(h.sel.profile_id(), 7);
        uint64_t t = 6000;
        for (uint8_t strike = 1; strike <= 4; ++strike) {
            (void)h.run(t, t + 900, -40, 100);
            t += 900;
            CHECK_EQ_U(h.sel.profile_id(), 6);
            const SelectorLockout lock = h.sel.lockout(t);
            CHECK_EQ_U(lock.strikes, strike);
            CHECK_EQ_U(lock.latched, strike == 4);
            if (strike < 4) {
                // Let the short timer expire and promote back into rung 7.
                (void)h.run(t + 600, t + 1400, -40, 0);
                t += 1400;
                CHECK_EQ_U(h.sel.profile_id(), 7);
            }
        }
        const SelectorLockout latched = h.sel.lockout(t + 5000);
        CHECK(latched.active && latched.latched);
        CHECK_EQ_U(h.sel.profile_id(), 6);
        // Same tuple and mode/range changes do not clear it.
        CHECK(!h.sel.on_rf_environment(5220, 20, t + 5000));
        CHECK(h.sel.lockout(t + 5000).latched);
        h.sel.set_profile_pin(0, 6);
        CHECK((h.sel.lockout(t + 5000).latched_mask & 0x80u) != 0);
        h.sel.set_profile_pin(7, 7);
        CHECK(h.sel.lockout(t + 5000).conflict);
        // A successful channel change starts a clean environment.
        CHECK(h.sel.on_rf_environment(5805, 20, t + 5001));
        CHECK_EQ_U(h.sel.lockout(t + 5001).active_mask, 0);
        CHECK(h.sel.reason() == SelectorReason::kEnvironmentReset);
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

    // §9.8 Pass 102 (supersedes Pass 84): the fail-safe floors at
    // max(min_profile, floor_profile), NOT the table floor_profile below the
    // band. The deployed vehicle runs min 1 / max 5; under the mode harness
    // min_profile is the band's verified lowest rung, so a lost-feedback fade
    // lands on MCS1, never MCS0 (floor_profile 0 no longer drags it below the
    // band — "don't let the mode mechanics fight each other").
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        p.min_profile = 1;  // Range band floor == verified lowest rung...
        p.max_profile = 5;
        Harness h(p);       // ...floor_profile is 0 (make_table) but cannot win
        h.boot(0);
        // Climb off the floor on healthy reports so there is somewhere to fall.
        uint64_t t = 0;
        for (; t <= 20000; t += 100) {
            h.report(t, -35, 0);
            (void)h.sel.tick(t);
        }
        CHECK(h.sel.profile_id() > 1);
        // Feedback stops entirely: hold, then damped descent — but only to the
        // band floor, not below it.
        for (; t <= 60000; t += 100) {
            (void)h.sel.tick(t);
        }
        CHECK(std::string_view(h.sel.state()) == "FAILSAFE");
        CHECK_EQ_U(h.sel.profile_id(), 1);  // min_profile band floor, not MCS0
    }
    // §9.8 Pass 102: a Range-Low band (MCS 2-5) fades no lower than MCS2 — the
    // rung its resolution/fps were co-designed for. floor_profile 0 is ignored
    // below the band.
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        p.min_profile = 2;
        p.max_profile = 5;
        Harness h(p);
        h.boot(0);
        uint64_t t = 0;
        for (; t <= 20000; t += 100) {
            h.report(t, -35, 0);
            (void)h.sel.tick(t);
        }
        CHECK(h.sel.profile_id() > 2);
        for (; t <= 60000; t += 100) {
            (void)h.sel.tick(t);
        }
        CHECK(std::string_view(h.sel.state()) == "FAILSAFE");
        CHECK_EQ_U(h.sel.profile_id(), 2);  // band floor, never below
    }
    // §9.8 Pass 102: floor_profile ABOVE min_profile still binds — max() lets
    // the table absolute floor RAISE the fail-safe floor, never lower it. Band
    // 0-5 but floor_profile 2: the fade stops at MCS2.
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        p.min_profile = 0;
        p.max_profile = 5;
        Harness h(p);
        h.table.floor_profile = 2;  // absolute floor above the band minimum
        h.boot(0);
        uint64_t t = 0;
        for (; t <= 20000; t += 100) {
            h.report(t, -35, 0);
            (void)h.sel.tick(t);
        }
        CHECK(h.sel.profile_id() > 2);
        for (; t <= 60000; t += 100) {
            (void)h.sel.tick(t);
        }
        CHECK(std::string_view(h.sel.state()) == "FAILSAFE");
        CHECK_EQ_U(h.sel.profile_id(), 2);  // floor_profile raises the floor
    }
    // §9.7 Pass 83: min/max_profile are profile IDs, not ladder indices. A
    // table whose ids do not equal their positions must still pin correctly.
    {
        SelectorPolicy p;
        p.mcs_settle_ms = 0;
        p.min_profile = 20;  // id 20 == index 0
        p.max_profile = 20;
        Harness h(p);
        h.table.profiles.clear();
        for (uint8_t i = 0; i < 3; ++i) {  // ids 20,21,22 — offset from index
            Profile pr;
            pr.id = static_cast<uint8_t>(20 + i);
            pr.mcs = i;
            pr.gi = GuardInterval::kLong;
            pr.tx_power_level = 4;
            pr.airtime_budget_permille = 600;
            pr.arq_deadline_iframe_ms = 80;
            pr.arq_deadline_pframe_ms = 25;
            pr.bitrate_min_kbps = 2200;
            h.table.profiles.push_back(pr);
        }
        h.table.floor_profile = 20;
        h.boot(0);
        (void)h.sel.tick(100);
        CHECK(std::string_view(h.sel.state()) == "PINNED");
        CHECK_EQ_U(h.sel.profile_id(), 20);  // the id, not profiles[20]
    }

    return wbtest_finish("selector_test");
}
