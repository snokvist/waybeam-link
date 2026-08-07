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
