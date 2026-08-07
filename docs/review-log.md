# Review log

Numbered record of **Tier-1 spec rulings** — the contract law of `PROTOCOL.md`.
Passes 1–152 (2026-07-10 → 2026-08-06) live in
`review-log-archive-p001-152.md`; this file continues the numbering from
Pass 153. The two-tier split itself is defined in `CLAUDE.md` ("The law").

## Format contract

- **Tier-1 rulings only.** An entry exists because a *contract* changed: wire
  format, trust/auth machinery, a state-machine behaviour peers depend on, or
  config semantics. Measurement-phase results — walls, gates, dwell counts,
  seeds, sweep parameters still being characterised — go to `findings.md`,
  not here, and not into `PROTOCOL.md`.
- **One numbered Pass per entry, ≤40 lines**: the verdict, the changed spec
  sections (`§N.M`), and an evidence pointer (branch, data dir, findings
  entry). No narration, no method essays.
- **No addenda.** A new fact after an entry is committed is a new Pass or a
  new finding — an entry is never reopened.
- **Method lessons** (process traps, measurement discipline) go to the
  coordination repo's memory, not this log.
- Spec-amendment-commits-first still applies to every entry here; it does
  **not** apply to findings.

## Passes

## Pass 153 — 0xF CALIBRATION family: symmetric probe exchange (2026-08-07)

**Verdict.** Calibration v2 adopted as spec
(`docs/calibration-v2-symmetric-probes.md`, operator rulings Q1–Q3 2026-08-06,
D-A/D-C 2026-08-07). Type `0xF` becomes the CALIBRATION subtype family;
the §3.16 UPLINK_QUALITY cumulative-counter layout is retired. One dwell
primitive, both directions: N MTU-padded PROBE frames at `(rung, qdb)`,
one per-dwell TALLY back, self-denominating loss. Video pauses for the run
(input-starve), restored on the rate/power restore edge.

**Changed sections.**
- §3.1: type table `0xF UPLINK_QUALITY` → `0xF CALIBRATION`.
- §3.16: rewritten — PROBE (22B fixed + pad to `mtu_effective`, range-length)
  and TALLY (26B exact; carries `rx_mcs` + `adapter_fingerprint` — D-A ruling
  keeps the delivered-rung cross-check and evidence identity gate). Unknown
  subtype = ignorable, not a decode error. Craft feed pause on first accepted
  PROBE of a new run, resume on probe-quiet timeout (D-C ruling — no VCMD).
  FEC/ARQ exemption stated structurally.
- §10.6: evidence = per-dwell tallies; Pass 134 report-health precondition
  deleted (self-denominating evidence cannot author false-clean); blackout
  rules keep addendum semantics with `evidence_lost` trigger; feed-pause and
  2 Hz unconditional §3.15-word emission while paused.
- §10.7: Pass 125/126/128/132 counting apparatus withdrawn whole; walls
  absolute again (contention floor structurally zero — closes the wall-origin
  question, `docs/findings.md` entries struck); **single rung again**
  (reverts Pass 131's widening; artifact list shape unchanged, loader accepts
  any length); no feedback-freshness precondition.
- §15.2: dwell knobs `dwell_probe_frames`/`dwell_verify_frames`/
  `probe_pace_us`/`tally_wait_ms`/`tally_retries`/`feed_quiet_ms` (Tier-2
  seeds); nine keys retired. §15.3: `uplink_quality_*` (6 fields) →
  `calib_probes_sent`/`calib_tallies_rx`/`calib_rx_mcs`/`feed_paused`.
- §11.7 `0x08` row, §11 trust note, threat-model row updated to the family.

**Evidence.** `docs/findings.md` 2026-08-06 (report-loss under-powered:
σ≈19‰ at the 80‰ baseline, n≈1500 needed), 2026-08-07 (floor mechanism);
archived Pass 152 field addendum; design doc §1 churn record (~14 reversals
traced to probe-is-payload + cumulative counters).
