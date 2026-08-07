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

## Pass 154 — EFUSE-MAC adapter identity; calibration binds to the unit (2026-08-07)

**Verdict.** Per-adapter calibration binds to the per-unit EFUSE MAC, not the
USB bus path (operator rulings D1–D3, issue #118, 2026-08-07; unblocked by
vendored devourer #383 `GetPermanentMacAddress` + #384 Jaguar3 EFUSE-walk
fix). A USB path identifies a *port*, not a device — swapping two dongles
silently applied each other's absolute qdb curve to the wrong PA, with no
regulatory clamp behind it (§10.3).

- **D1** — new optional `adapters[].mac`; stanza match precedence
  **`mac` > `bus` > first-free**, `bus` kept as an explicit port pin. The
  §10.6/§10.7 artifacts anchor on the same MAC.
- **D2** — **fail closed + safe offset**: an identity that does not match
  never applies a `power_map`/artifact; the adapter still comes up at the
  §10.5 safe boot offset (−24 qdb) with a loud log — flyable, curve withheld.
- **D3** — a unit reporting no identity (`GetPermanentMacAddress` → false)
  is refused an absolute curve outright; no declared/bus fallback tier on
  the radio backend (no dual code path). Upstream Jaguar2 wiring is
  hardware-gated (no unit on the bench); upstream independently landed #386
  meanwhile — moot for our builds (family compiled out).

**Changed sections.**
- §10.6 identity block: radio resolution collapses to the single derived
  tier `mac/<efuse-mac>` (Pass 146's 3-tier order survives only on
  kernel-monitor, frozen per the backend ruling, issue #120);
  `id/radio/<calib_id>` and `bus/…` retired on radio — artifacts keyed to
  them read STALE, a re-run re-keys. "devourer cannot reach it" paragraph
  replaced: the upstream request is fulfilled.
- §10.2: "per physical adapter" binds by unit identity, never USB position.
- §15.2: `adapters[].mac` key (format, post-bring-up binding, D2 fallback,
  duplicate/backend rejection); example updated.
- §15.5: `GET /api/v1/info` `adapters[]` gains `mac` (null = none).

**Evidence.** Issue #118 (rulings + measured serial-placeholder/bus-shuffle
record); coordination memory `devourer_efuse_walk_and_mac_identity` (vendor
offset 0x157, rfe 0→3 seven-register delta); `third_party/README.md`
provenance `5a5dd62`; CU re-baseline in `docs/findings.md` (2026-08-07).

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
