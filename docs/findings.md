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

## 2026-08-07 — §10.7 walls referenced to a measured at-rest floor (mechanism demoted from spec)

The Pass 152 floor-relative-walls text shipped into `PROTOCOL.md` via the
#115 squash; this entry is its Tier-2 home after demotion — the spec now
marks the walls' reference point as unruled. The **mechanism stays in the
code** (ground `run_rx`), governed by config, not by spec:

- **What it does:** the ground samples an at-rest uplink loss floor over a
  rolling window of ordinary operation, only while no sweep is running (the
  same exclusion, for the same reason, as §10.6's Pass 134 report-health
  precondition: once the sweep starts, elevated loss *is* the measurement).
  A §10.7 start with no floor yet measured is refused. The run's walls are
  then `floor + loss_ok_milli` / `floor + loss_bad_milli`. On a quiet link
  the floor is ~0 and behaviour equals the absolute seeds.
- **Why it exists (measured):** the fleet's at-rest report loss at 10 m sat
  well above the 15 ‰ absolute `loss_ok_milli` seed, putting the acceptance
  gate below the link's own floor — every rung read `verify_failed` at any
  power, and the one artifact ever produced was the Pass 134
  starved-feedback signature (all eight rungs at 108, no bracket).
- **Window sizing (measured):** the window is a sample-count gate, not a
  clock — knob `policy.calibration.uplink_floor_min_samples` (default
  **300**). A 4 s / n=40 first cut read a 50 ‰ floor against a 40 s
  window's 24.8 ± 7.7 ‰ and admitted a rung at 45 ‰ the true floor would
  have refused; n=300 puts σ near 9 ‰ at a 25 ‰ floor.
- **Open:** whether report-loss is the right observable at all — the
  2026-08-06 entry below shows even n=300 may be too small to rank rungs at
  a lossy baseline (separating 20 ‰ at an 80 ‰ floor needs n≈1500/dwell).
  Both questions dissolve into the calibration-v2 ruling
  (`calibration-v2-symmetric-probes.md` §5-§6); this mechanism is interim
  and expected to be deleted with §10.7's evidence plumbing.

## 2026-08-06 — §10.7 report-loss is an under-powered observable at this link's baseline

Carried over from the Pass 152 field addendum (the last entry of the archived
log, which states this correction's rationale in full).

- **Measured:** at-rest uplink report-loss readings of 16–110 ‰ at fixed
  19 dBm, RSSI pinned −64, video steady 21.5–22.9 Mbps; 21 windows of n≈150
  gave mean 79.6 ‰, sd 23.3 ‰ vs a binomial prediction of 22.1 ‰,
  correlation with video bitrate −0.12. The scatter is *entirely* sampling
  noise — there was no interference or bitrate phenomenon to explain.
- **Meaning:** §10.7's dwells (probe 100 / verify 200) are sized for a
  near-zero baseline. At an ~80 ‰ baseline a verify dwell has σ≈19 ‰, so
  placements 15 vs 10 ‰ are indistinguishable and "unreachable" rungs are
  part coin flip. Separating 20 ‰ there needs n≈1500 per dwell — hours per
  run. No threshold choice fixes an under-sampled estimator. Meanwhile RSSI
  moved cleanly and monotonically with commanded power all evening.
- **Open:** whether report-loss is the right §10.7 observable at all, or
  whether calibration needs dedicated probe traffic. Design answer proposed
  in `calibration-v2-symmetric-probes.md` (paused video + MTU-sized probe
  bursts + per-dwell tallies); a ruling waits until that design is reviewed.

## 2026-08-06 — §10.7 spec/code drift: `uplink_verify_epochs` 400 (spec) vs 200 (code)

- **Found by inspection**, not measurement: `PROTOCOL.md` seeds
  `uplink_verify_epochs` = 400 (three occurrences, incl. the §10.7 body and
  the config table) while the code seeds 200
  (`io/include/wblink/config.h` `uplink_verify_epochs`,
  `core/include/wblink/uplink_calibrate.h`, locked by
  `tests/config_test.cpp`). At 200, five lost verify probes read 25 ‰ —
  between the 15/40 walls, the exact one-probe-decidability ambiguity
  Pass 132's `1000/N <= loss_ok_milli` rule was meant to retire.
- **Open:** which number is intended. Given the finding above (n≈1500 for
  real discrimination at a lossy baseline), the reconciliation should fall
  out of the calibration-v2 decision rather than be patched twice. Until
  then the deployed value is the code's 200.
