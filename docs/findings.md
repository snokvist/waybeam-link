# Findings

The **Tier-2 channel** (see `CLAUDE.md`, "The law"): dated notes on anything
still being measured — loss walls, gates, dwell counts, seeds, sweep bounds,
estimator behaviour. A finding records evidence and an open question; it never
amends `PROTOCOL.md`. When a mechanism settles, ONE spec amendment plus a
numbered Pass in `review-log.md` closes it out, citing the finding.

Format per entry: date, title, what was measured (setup + numbers), what it
means, what stays open. Newest first. Delete or strike entries a later Pass
has closed, with a pointer to the Pass.

---

## 2026-08-07 — first frame-free occupancy sweep: the two axes are demonstrably independent on ambient air

**Setup.** x86 devourer ground (8812AU scout), Pass 155 build, 7-channel
allowlist sweep at 300 ms dwells, craft link stopped (zero waybeam traffic
anywhere).

**Measured.** `wifi_util_permille` 0 on all seven bins (correct — nothing
decodable of ours on air) while `interference_util_permille` independently
ranked them: 5180 = 638, 5220 = 590, 5825 = 468, 5805 = 311, 5745 = 249,
5765 = 176, 5785 = 175. 5180/5220 are where this bench's household APs
live. `noise_dbm` filled only on 5180 (−81, passive floor — the one bin
with decodable foreign frames); null elsewhere, no fake zeros.

**Means.** The pre-155 ranking would have scored all seven bins identically
pristine (wifi_util 0 everywhere); the interference-inclusive ranking picks
5785/5765 over the AP-occupied bins. Ambient "quiet" UNII-3 bins read
~175–300 on the index — the fa-half seed (200 FA/s) puts the ambient FA
floor mid-scale, which is fine for ranking (monotone within the adapter)
but is a reminder the index is not a duty cycle.

**Open.** The #95 operator bench gate (a *known controlled* interferer
out-ranking quiet bins; craft video on its home channel not inflating its
own bin) — needs a hand on the signal generator. Whether the fa-half seed
wants re-derivation per §17 once #100's rank normalisation lands.

## 2026-08-07 — CU RF re-baseline after the #384 re-vendor (rfe_type 0→3): placements within flat-field noise, wall pattern unchanged

**Setup.** Same rig as the entry below (x86 devourer ground: 8812AU TX
`mac/20:0d:b0:c4:a7:6a`, 8812CU diversity ear `40:a5:ef:2f:23:08`; craft .232
8822EU `dc:57:5b:00:d0:57`, 5805/HT20, dwells 500/1000). Ground running the
Pass-154 branch with devourer re-vendored to `5a5dd62` — the first run on
this rig where the 8812CU's PHY tables load with EFUSE `rfe_type` 3 (the
#384 walk fix; the measured delta vs the old column is 7 RF register values,
see `docs/devourer-revendor-review.md`). Craft on the pre-bump deployed
binary (8822E tables are untouched by #384).

**Measured.** Bi-directional `start_both` completed (after a §11.5a claim —
the first attempt without one failed `downlink_no_ack` with the craft never
starting, which is the binding working, not a defect). Downlink placements
`[0, 0, 0, +14, +10, +2, −6, +4]` (all 0‰), brackets booked on rungs 3–7
(first_bad_rssi −33/−36/−36/−41/−38, placement RSSI −31..−40); uplink MCS0
flat to +24 (no bracket), capped placement 0 @ 4‰, RSSI −53. Morning
pre-bump baseline (below): downlink `[−8, 0, 0, +8, +8, 0, −8, 0]` with 5
rungs bracketed, uplink capped −8 @ 2‰ (RSSI −48).

**Means.** Same shape either side of the bump: flat clean low rungs, walls
on the top half, uplink wall outside the window. Placement deltas are
one-to-two seek steps inside a 0‰ flat field — the entry below already
concluded such differences are noise, and geometry/power state moved between
sessions too (uplink RSSI −53 vs −48). No gross CU RF regression: the
diversity ear delivers, the link held 1‰ at HOLD pre-run, calibration
completes. **The rig's baseline is now these post-bump numbers.** Nothing
measured pre-bump survives as an artifact either way: the Pass 154 identity
change re-keys every stored artifact (`id/radio/…` reads STALE), so no
pre-bump RF state can silently apply.

**Open.** A same-session A/B (old vs new devourer on the CU, rig unmoved)
was not run — placements are noise-bounded at this range, so only a
purpose-built RX-sensitivity A/B would resolve the CU delta finer. The
8822EU per-unit 64-QAM early-wall note (below) stands.

## 2026-08-07 — flat-field verify selection is noise; widening the offset window recovers real walls

**Setup.** x86 devourer ground (8812AU TX `ground-au-1`, 8812CU diversity) +
.232 craft (8822EU `craft-eu-1`), 10 m, 5805/HT20, calibration v2 dwells
(500/1000 frames).

**Measured.** With the original [−24, 0] offset window the whole field is
flat (1–10‰ everywhere, no bracket bookable), so the §10.7 verify walk's
"best" is noise-selected: morning runs placed (−8 @ 1‰, then 0 @ 6‰),
midday runs refused `no_wall_found` **six consecutive times** (verify at the
ceiling kept reading 1–3‰ vs 4‰ one step down) — the outcome tracked slow
RF drift, not the link. After the same-day rulings (offset-space exemption +
window widened to [−24, +24] with the unbracketed-placement cap) the same
bench books **real walls on 5 of 8 downlink rungs** (fp=133 placements
`[-8, 0, 0, +8, +8, 0, -8, 0]`, brackets at first_bad_rssi −66/−41): the
walls were simply above the old window's ceiling. Uplink at MCS0 stays
wall-less even at +24 (RSSI −48, 4‰) — capped placement −8 @ 2‰.

**Means.** Within a flat region, placement differences of one seek step are
not reproducible measurements; only a booked bracket makes a placement a
property of the channel. The window should be wide enough to contain the
wall, and the reference cap handles the case where it is not.

**Open.** Uplink MCS0 wall not yet within [−24, +24] at 10 m — either a
longer placement or a higher-MCS uplink rung would book it. The craft's
rung-6 (64-QAM) early wall (first_bad −41) matches the known per-unit
8822EU 64-QAM TX weakness; unify with that finding when the unit is
re-characterised.

## 2026-08-07 — ground binary wedges on SIGTERM after an in-process calibration run

Twice this session the x86-ground process ignored SIGTERM (stop script +
direct kill; REST already dead, process alive until SIGKILL) — both times
after it had completed at least one §10.7 run in-process; a fresh instance
stops cleanly. Suspect a teardown path wedged in devourer USB close while
calibration-era actuator state is present. Bench impact only (SIGKILL is
acceptable on x86, never on SigmaStar). Open: reproduce under gdb / with
devourer verbose teardown logging; check whether the §10.7 restore path
leaves an actuator thread parked.

## 2026-08-07 — §7.2 aim error budget: instruments landed, numbers owed (issue #99)

**What exists now (bench knob, no spec surface):** `WBLINK_AIM_LOG=1`
histograms two of issue #99's three error terms — (a) *release lateness*,
how late past the computed `QuietGap::return_deadline` the host loop
actually fired the return window, and (b) the `ReadTsf()` control-transfer
cost, the §7.2 term with **no measured number at all** (devourer bounds it
0.5–1.2 ms on Jaguar3; a bulk-flooded adapter additionally starves the
read). Dumped to stderr every 30 s as bucketed distributions
(<50/<100/<200/<500/<1k/<2k/<5k/≥5k µs) — the tail is the contract, means
hide it. The third term (craft-side arrival phase relative to its own gap)
is **deliberately not instrumented yet**: it needs either a host↔TSF fit
(devourer tdma example) or a host-time proxy whose error is exactly the
terms under study — that placement choice is part of the §17 gate-4
evaluation itself.

**What the vendored bench already says (act on it now):** submit→air p99 is
**101 µs on Jaguar1 (8812AU, async USB2)** vs **2.2 ms on Jaguar3 (8822CU,
sync USB3)** against a ±1000 µs window budget — on a Jaguar3 uplink, that
one term alone blows the budget ~1 % of the time, and the failure is
*correlated* return loss inside a window (defeating Pass 78's redundancy,
which assumes independence). **The ground's `role:"tx"` adapter should be
the 8812AU** wherever the rig has a choice; the x86 bench rig already
complies (AU = `au-uplink`), now as a rule rather than an accident (also
noted in `deploy/README.md`).

**Open:** the gate-4 campaign — run the instruments per uplink generation
(AU vs CU), measure end-to-end aim as a distribution, report the miss rate
against `[eob+guard, eob+guard+window]`, then recommend re-derived
`guard_us`/`return_window_us` seeds or a documented miss budget.
`disable_cca` is NOT a lever (Pass 139: clearing it costs ~45 % of the
uplink). TDMA stays deferred per the issue's own assessment.

## ~~2026-08-07 — §10.7 walls referenced to a measured at-rest floor~~ CLOSED by Pass 153

The floor mechanism (and its `uplink_floor_min_samples` knob) is deleted:
calibration v2 pauses the craft's video for the run, so the contention floor
the walls were being referenced against is structurally zero and the walls are
absolute again. See `review-log.md` Pass 153.

## ~~2026-08-06 — §10.7 report-loss is an under-powered observable~~ CLOSED by Pass 153

Resolved in the direction the entry proposed: §10.7 (and §10.6) measure with
dedicated MTU-padded §3.16 PROBE bursts and per-dwell TALLYs — probe density
is no longer capped by the 10 Hz report cadence, so the n≈1500-per-dwell
sample the estimator arithmetic demanded is cheap. The at-rest σ evidence
(21 windows of n≈150: sd 23.3‰ vs binomial 22.1‰) lives on in the archived
Pass 152 addendum and the Pass 153 entry. See `review-log.md` Pass 153.

## ~~2026-08-06 — §10.7 spec/code drift: `uplink_verify_epochs` 400 vs 200~~ CLOSED by Pass 153

Dissolved: the key is retired; the v2 dwell knobs are `dwell_probe_frames`
(500) / `dwell_verify_frames` (1000). See `review-log.md` Pass 153.
