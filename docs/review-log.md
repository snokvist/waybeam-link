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

## Pass 153 — 0xF EXTENDED type + calibration v2 probe exchange (2026-08-07)

**Verdict.** Calibration v2 adopted as spec
(`docs/calibration-v2-symmetric-probes.md`, operator rulings Q1–Q3 2026-08-06,
D-A/D-C 2026-08-07, EXTENDED generalization 2026-08-07). Type `0xF` becomes
the **EXTENDED type**: first payload byte = extended type ID registry
(`0x00` reserved-invalid, `0x01` CAL_PROBE, `0x02` CAL_TALLY, `0x03`–`0xFF`
unassigned; unknown IDs ignored). The version nibble is reserved for
breaking changes only — additive growth goes through the registry. The
§3.16 UPLINK_QUALITY cumulative-counter layout is retired. One dwell
primitive, both directions: N MTU-padded CAL_PROBE frames at `(rung, qdb)`,
one per-dwell CAL_TALLY back, self-denominating loss. Video pauses for the
run (input-starve), restored on the rate/power restore edge.

**Changed sections.**
- §3.1: type table `0xF UPLINK_QUALITY` → `0xF EXTENDED`; version nibble
  reserved for breaking changes.
- §3.16: rewritten — the type-ID registry, then CAL_PROBE (22B fixed + pad
  to `mtu_effective`, range-length) and CAL_TALLY (26B exact; carries
  `rx_mcs` + `adapter_fingerprint` — D-A ruling keeps the delivered-rung
  cross-check and evidence identity gate). Unknown ID = ignorable, not a
  decode error; `0x00` = reserved-invalid. Craft feed pause on first
  accepted CAL_PROBE of a new run, resume on probe-quiet timeout (D-C
  ruling — no VCMD). FEC/ARQ exemption stated structurally.
- §10.6: evidence = per-dwell tallies; Pass 134 report-health precondition
  deleted (self-denominating evidence cannot author false-clean); blackout
  rules keep addendum semantics with `evidence_lost` trigger; feed-pause and
  2 Hz unconditional §3.15-word emission while paused.
- §10.7: Pass 125/126/128/132 counting apparatus withdrawn whole; walls
  absolute again (contention floor structurally zero — closes the wall-origin
  question, `docs/findings.md` entries struck); **single rung again**
  (reverts Pass 131's widening; artifact list shape unchanged, loader accepts
  any length); no feedback-freshness precondition.
- §3.15: the word-acceptance tuple **latches** — once accepted from a
  live-consumed RTP `(originator,session)`, the tuple survives that stream's
  §2 idle teardown until a different tuple's live stream replaces it.
  Without this the §3.16 pause-emission clause is unreceivable: the pause
  starves the stream past teardown, the ground refuses every mid-run word,
  and the §10.7 sequencer falsely fails `downlink_no_ack` (found on the
  2026-08-07 hardware bench; craft-side runs completed correctly throughout).
- §10.6/§10.7: the Pass 134 flat-at-ceiling `no_wall_found` refusal is
  scoped to **absolute space**. In offset space (Pass 151 backends) a flat
  window completes and persists (operator-ruled 2026-08-07 after six
  consecutive refusals hard-blocked the sequencer at 10 m). The window may
  extend above offset 0 (second ruling, same day: 0 is the efuse
  reference, not a proven per-unit maximum), with one placement cap: a run
  that booked **no overload bracket places no higher than offset 0** —
  above the reference sits per-unit PA compression a close-range flat
  field cannot see, so an unbracketed best there is noise, not evidence.
  A bracketed sweep places below its wall as measured.
- §15.2: dwell knobs `dwell_probe_frames`/`dwell_verify_frames`/
  `probe_pace_us`/`tally_wait_ms`/`tally_retries`/`feed_quiet_ms` (Tier-2
  seeds); nine keys retired. §15.3: `uplink_quality_*` (6 fields) →
  `calib_probes_sent`/`calib_tallies_rx`/`calib_rx_mcs`/`feed_paused`.
- §11.7 `0x08` row, §11 trust note, threat-model row updated to the family.

**Evidence.** `docs/findings.md` 2026-08-06 (report-loss under-powered:
σ≈19‰ at the 80‰ baseline, n≈1500 needed), 2026-08-07 (floor mechanism);
archived Pass 152 field addendum; design doc §1 churn record (~14 reversals
traced to probe-is-payload + cumulative counters).
