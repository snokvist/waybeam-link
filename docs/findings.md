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
