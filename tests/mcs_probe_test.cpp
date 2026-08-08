// SPDX-License-Identifier: GPL-2.0-or-later
// §9.4 Pass 163 sequence-derived rate probe: schedule/candidate derivation
// and the three normative McsProbeWindow guards — rate-verified successes,
// epoch-gated gap losses, CRC seq-free attribution — plus context resets,
// freshness, and the selector's veto-not-warrant property.
#include "wblink/mcs_probe.h"

#include "wblink/selector.h"
#include "wbtest.h"

using namespace wblink;

namespace {

// Four-rung ladder; ids deliberately non-contiguous and MCS != rung index,
// so any rung_index==mcs shortcut fails loudly. Rungs 2/3 (ids 6/9) share
// an MCS to exercise the same-MCS-adjacent disarm.
ProfileTable make_table(uint16_t period, uint16_t slot) {
    ProfileTable t;
    const uint8_t ids[] = {2, 4, 6, 9};
    const uint8_t mcs[] = {1, 3, 5, 5};
    for (size_t i = 0; i < 4; ++i) {
        Profile p;
        p.id = ids[i];
        p.mcs = mcs[i];
        p.max_payload = 1424;
        t.profiles.push_back(p);
    }
    t.floor_profile = 2;
    t.probe_period = period;
    t.probe_slot = slot;
    return t;
}

constexpr uint64_t kT0 = 1'000'000;

// Drive a window with a clean run: non-probe frames at the selected rate
// (confirming), probe slots at the candidate rate.
void feed_clean(McsProbeWindow& w, const ProfileTable& t, uint8_t profile,
                uint8_t selected, uint8_t candidate, uint32_t first_seq,
                uint32_t n, uint64_t now, uint32_t lose_every_probe = 0) {
    uint32_t probe_no = 0;
    for (uint32_t s = first_seq; s < first_seq + n; ++s) {
        const bool probe = probe_slot_hit(s, t.probe_period, t.probe_slot);
        if (probe) {
            ++probe_no;
            if (lose_every_probe != 0 && probe_no % lose_every_probe == 0) {
                continue;  // probe frame lost on air
            }
            w.on_data(s, profile, table_version(t), candidate, now);
        } else {
            w.on_data(s, profile, table_version(t), selected, now);
        }
    }
}

}  // namespace

int main() {
    const ProfileTable t = make_table(16, 4);
    const uint8_t tv = table_version(t);

    // ---- schedule + candidate derivation ------------------------------
    CHECK(!probe_slot_hit(4, 0, 4));      // period 0 = off
    CHECK(probe_slot_hit(4, 16, 4));
    CHECK(probe_slot_hit(20, 16, 4));
    CHECK(!probe_slot_hit(5, 16, 4));

    // id 2 -> next id 4's mcs (3); id 4 -> 5; id 6 -> id 9 shares mcs 5 =>
    // disarmed; id 9 top rung => disarmed; unknown id => disarmed.
    CHECK_EQ_U(probe_up_candidate_mcs(t, 2).value(), 3);
    CHECK_EQ_U(probe_up_candidate_mcs(t, 4).value(), 5);
    CHECK(!probe_up_candidate_mcs(t, 6).has_value());
    CHECK(!probe_up_candidate_mcs(t, 9).has_value());
    CHECK(!probe_up_candidate_mcs(t, 7).has_value());

    McsProbeWindow::Params prm;
    prm.min_samples = 4;
    prm.gap_horizon = 8;
    prm.max_age_ms = 8000;

    // ---- clean run: all probes delivered at the candidate rate --------
    {
        McsProbeWindow w(&t, tv, prm);
        feed_clean(w, t, 2, 1, 3, 0, 200, kT0);
        CHECK(w.confirmed());
        CHECK(w.successes() >= prm.min_samples);
        CHECK_EQ_U(w.failures(), 0);
        CHECK_EQ_U(w.probe_per(kT0).value(), 0);
    }

    // ---- guard 1: successes are rate-verified -------------------------
    // Probe-slot frames that arrive at the SELECTED rate (resend/suppressed
    // probe) are ignored, never credited.
    {
        McsProbeWindow w(&t, tv, prm);
        for (uint32_t s = 0; s < 200; ++s) {
            w.on_data(s, 2, tv, 1, kT0);  // everything flies selected
        }
        CHECK_EQ_U(w.successes(), 0);
        // No gap losses either (every seq arrived): window has no opinion.
        CHECK(!w.probe_per(kT0).has_value());
    }

    // ---- gap attribution: a lost probe frame charges the candidate ----
    {
        McsProbeWindow w(&t, tv, prm);
        feed_clean(w, t, 2, 1, 3, 0, 320, kT0, /*lose_every_probe=*/2);
        CHECK(w.failures() > 0);
        const uint16_t per = w.probe_per(kT0).value();
        // Half the probes lost => PER near 500‰.
        CHECK(per > 350 && per < 650);
    }

    // ---- guard 4: failure-only evidence never reports -----------------
    // A NON-probing TX (stage-0-unproven die, probing off) on a
    // probe-scheduled table: non-probe frames confirm, environmentally lost
    // probe-slot seqs accrue as failures — but with zero direct
    // candidate-rate observations the window must report nothing, or
    // ordinary air loss manufactures a phantom veto (operator ruling
    // 2026-08-08).
    {
        McsProbeWindow w(&t, tv, prm);
        for (uint32_t s = 0; s < 320; ++s) {
            if (probe_slot_hit(s, t.probe_period, t.probe_slot)) {
                continue;  // TX never probed; slot seqs simply lost
            }
            w.on_data(s, 2, tv, 1, kT0);
        }
        CHECK(w.confirmed());
        CHECK(w.failures() > 0);
        CHECK(!w.probe_per(kT0).has_value());
        // One direct observation (a CRC-verified candidate failure) makes
        // the accrued evidence reportable.
        w.on_crc_frames(3, 1, kT0);
        CHECK(w.probe_per(kT0).has_value());
    }

    // ---- guard 2: gap losses are epoch-gated --------------------------
    // TX demonstrably NOT flying the commanded rate (non-probe frames at a
    // different MCS) => un-confirmed => missing probe slots attribute
    // nothing.
    {
        McsProbeWindow w(&t, tv, prm);
        uint32_t fed = 0;
        for (uint32_t s = 0; s < 320; ++s) {
            if (probe_slot_hit(s, t.probe_period, t.probe_slot)) {
                continue;  // every probe frame "lost"
            }
            // Command lag: frames fly mcs 5, not the commanded 1.
            w.on_data(s, 2, tv, 5, kT0);
            ++fed;
        }
        CHECK(fed > 0);
        CHECK(!w.confirmed());
        CHECK_EQ_U(w.failures(), 0);
        CHECK(!w.probe_per(kT0).has_value());
    }

    // ---- guard 3: CRC frames attribute rate-free, gated on confirm ----
    {
        McsProbeWindow w(&t, tv, prm);
        // Not yet confirmed: crc evidence must be refused.
        w.on_crc_frames(3, 10, kT0);
        CHECK_EQ_U(w.failures(), 0);
        feed_clean(w, t, 2, 1, 3, 0, 100, kT0);
        const uint32_t f0 = w.failures();
        w.on_crc_frames(3, 5, kT0);   // candidate rate: 5 failures
        w.on_crc_frames(1, 50, kT0);  // selected rate: not candidate, ignored
        w.on_crc_frames(7, 50, kT0);  // unrelated rate: ignored
        CHECK_EQ_U(w.failures(), f0 + 5);
    }

    // ---- operating-context reset: profile change wipes evidence -------
    {
        McsProbeWindow w(&t, tv, prm);
        feed_clean(w, t, 2, 1, 3, 0, 200, kT0);
        CHECK(w.successes() > 0);
        w.on_data(1000, 4, tv, 3, kT0);  // sender moved to profile id 4
        CHECK_EQ_U(w.successes(), 0);
        CHECK_EQ_U(w.failures(), 0);
        CHECK_EQ_U(w.candidate_mcs().value(), 5);
        CHECK(!w.probe_per(kT0).has_value());
    }

    // ---- table_version mismatch: refuse to score ----------------------
    {
        McsProbeWindow w(&t, tv, prm);
        for (uint32_t s = 0; s < 200; ++s) {
            w.on_data(s, 2, static_cast<uint8_t>(tv + 1), 1, kT0);
        }
        CHECK(!w.confirmed());
        CHECK(!w.probe_per(kT0).has_value());
    }

    // ---- staleness: old evidence gates nothing ------------------------
    {
        McsProbeWindow w(&t, tv, prm);
        feed_clean(w, t, 2, 1, 3, 0, 200, kT0);
        CHECK(w.probe_per(kT0).has_value());
        CHECK(!w.probe_per(kT0 + prm.max_age_ms + 1).has_value());
    }

    // ---- same-MCS adjacency / top rung: window stays idle -------------
    {
        McsProbeWindow w(&t, tv, prm);
        feed_clean(w, t, 6, 5, 5, 0, 200, kT0);
        CHECK(!w.probe_per(kT0).has_value());
    }

    // ---- selector veto: fresh bad probe_per blocks the RSSI promote ---
    // ...and NEVER authorizes one (veto-not-warrant).
    {
        SelectorPolicy p;
        p.rung_rssi_floor_dbm = {-88, -85, -83, -80, -77, -73, -71, -70};
        p.probe_veto_permille = 50;
        p.probe_veto_ttl_ms = 3000;
        ProfileTable lt = make_table(16, 4);
        Selector sel(p, &lt);
        uint64_t now = kT0;
        (void)sel.tick(now);  // boot commit at the floor rung

        auto report = [&](uint16_t probe_per_val, uint32_t epoch) {
            LinkReport r;
            r.prefix.originator = 9;
            r.prefix.session_id = 1;
            r.report_epoch = epoch;
            r.rssi_best = -20;  // huge margin: RSSI would promote
            r.rssi_mean = -20;
            r.loss_postdiv_prearq = 0;
            r.uniq = 1000;
            r.adapters = 1;
            r.probe_per = probe_per_val;
            CHECK(sel.on_report(r, now));
        };

        // Fresh veto-worthy evidence: no promote, counter advances.
        uint32_t epoch = 1;
        for (int i = 0; i < 30; ++i) {
            report(800, epoch++);
            now += 100;
            const SelectorActions a = sel.tick(now);
            CHECK(!a.commit.has_value());
        }
        CHECK(sel.promote_blocked_probe() > 0);

        // Evidence goes stale (kNoProbe reports): the veto ages out and the
        // ordinary RSSI-margin promote proceeds — clean probe evidence was
        // never REQUIRED, so this also pins veto-not-warrant: absence of
        // probe evidence must behave exactly like pre-163.
        bool promoted = false;
        for (int i = 0; i < 60 && !promoted; ++i) {
            report(kNoProbe, epoch++);
            now += 100;
            promoted = sel.tick(now).commit.has_value();
        }
        CHECK(promoted);
    }

    return 0;
}
