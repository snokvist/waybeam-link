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

## Pass 157 — TX coding becomes commandable: air.ldpc / air.stbc, RX-proved (2026-08-07)

**Verdict.** Issue #97. The radiotap MCS known mask has always claimed FEC
and STBC known while leaving both flag bits clear — every frame we air
affirmatively commands BCC and zero STBC streams. The two bits become
config: `air.ldpc` (FEC = LDPC) and `air.stbc` (stream count 1), **both
default off** — the enable is a future ruling on cliff-A/B evidence, this
pass is the mechanism plus its proof surface. Two knobs, not one: LDPC is
coding gain on one stream, STBC is diversity across two chains; separate
arms. Ruling #120 reframes the issue's open kernel-monitor strip question:
the backend is frozen, the knobs are refused there (Pass 154 mac posture),
devourer-only.

**Changed sections.**
- §3.0 (rate-mechanism block): coding paragraph — per-packet mechanism,
  node-wide policy (calibration probes inherit the node coding, so delivery
  walls are per-coding); Pass 156 capability leg on `TxCaps.ldpc_ok` /
  `stbc_ok`; fleet-decision warning (a TX cannot see a remote ear's caps —
  a non-decoding receiver reads an LDPC arm as pure loss; fleet
  `ldpc_rx_ht` is true on all three dies); proof-over-inference via
  devourer `RxAtrib.ldpc`/`stbc` on `ldpc_rx_flag` dies; default-on flip is
  a §9.3/§17 RE-DERIVE trigger.
- §15.2: the two keys, radio-backend-only (refused elsewhere), defaults
  off, sample updated.
- §15.3: per-adapter `rx_ldpc` / `rx_stbc` counters + static
  `ldpc_flag_ok` (a zero counter means nothing on a flag-incapable die —
  the 8814A decodes but cannot report). Advisory like `rx_mcs`.

**Evidence.** Issue #97 (fleet caps table, measurement caveats);
`io/include/wblink/radiotap.h:41` (the known mask);
`third_party/devourer/src/RadiotapTxFlags.h` (send-path decode, all three
jaguar generations); `AdapterCaps.h:128–139` (ldpc_rx trio),
`TxCaps.h:20–36`; LDPC ≈ +3 dB at the 10 %-delivery crossing, MCS7/20 MHz
(devourer `tests/ldpc_waterfall.sh`).

## Pass 156 — hardware-ACK hybrid: responder and retry limit are one decision (2026-08-07)

**Verdict.** Issue #96. devourer #354 moved the TX retry-limit default to 0
(WFB posture), and `RadioAir` never set `dc.tx.retry_limit` — so the armed
§3.0 Pass 12 hybrid was a one-ended ARQ loop: a peer that ACKs correctly and
a sender that never retransmits, silently inert. The responder knob and the
retry knob are one decision, not two independent defaults.

**Changed sections.**
- §3.0 (Pass 12 hybrid): the stale "descriptor retry limit 12" claim
  corrected; retries are `air.tx_retry_limit`, and the coupling is law — on
  the radio backend `return.unicast` or `air.ack_responder` with
  `tx_retry_limit: 0` is a config validation error, never an inert run.
  Two inherited devourer behaviours pinned load-bearing: RX-pool posture
  stays `backpressure` (`drop` produces ACKed-but-undelivered — the loop's
  one way to lie), and a Jaguar3 retry may fall down the rate ladder
  (`MCS3 ×4 → MCS2 → 6M ×4` witnessed), so no airtime accounting assumes a
  retry flew at the commanded rung.
- §15.2: `air.tx_retry_limit`, default **8** (operator-ruled 2026-08-07),
  range 0–63. The ruling point: devourer's sweep gives 8 → 99.97 %
  delivered where airtime is precious and a FEC floor exists (§14 runs
  one); 16 buys the last 0.03 % at +5.4 % retry airtime. Inert for
  broadcast, so the default costs nothing on a broadcast-only node.

**Out of scope, recorded so it stays closed:** A-MPDU stays rejected — the
+30 % headline is a high-MCS broadcast shape; at this hybrid's exact shape
(MCS3, 512 B, ACK+retry 8) aggregation measured −8 %, suppresses ~60 % of
SPE_RPT (blinding the §9.10 wedge sensor and the retry distribution), and
paces launches 0.8–3 ms in front of the §7.2 quiet-gap design (issue #96
evidence at vendored `800c3c8`).

**Review addendum (pre-merge, same pass).** The config coupling closed the
config-value hole but not the die-capability hole: devourer keeps the
vendor `DATA_RETRY_LIMIT` carve-out where `caps.tx_retry_limit_ok = false`
(8814A; 8821C false-as-unmeasured) — the descriptor write is silently
skipped and a validated nonzero limit still never retransmits. §3.0 gains
the capability leg: `return.unicast` refuses bring-up when the resolved TX
unit (post §15.2 re-bind) reports the cap false. `ack_responder` is
deliberately not caps-gated — `SetAckResponder` refusal is loud at arm time
and the run degrades (returns still received via broadcast RX), while the
skipped retry field has no signal at all. §15.2 also records Kestrel's
attempts-counting WD field: devourer folds the +1, effective ceiling 62,
authored 63 runs 62 with a device-log note. No fleet impact: AU (8812A),
CU (8812C/jaguar3), EU (8822E) all read the cap true.

**Evidence.** Issue #96 (devourer sweep table, per-die responder rates);
`third_party/devourer/src/DeviceConfig.h` retry_limit doc; witness rate
ladder in the issue notes; capability rows
`third_party/devourer/src/jaguar1/RtlJaguarDevice.cpp:1807` /
`jaguar2/RtlJaguar2Device.cpp:1138` / `jaguar3/RtlJaguar3Device.cpp:1541` /
`kestrel/RtlKestrelDevice.cpp:829` (clamp at :81–86, :1122–1124).

## Pass 155 — §15.5a occupancy: frame-free fields become real, ranking follows (2026-08-07)

**Verdict.** Issue #95 implemented. The scout's occupancy was fed exclusively
by decoded waybeam frames, so `emptiest()` ranked channels by how much of
*our own* traffic they carried — a channel saturated by a non-decodable
emitter scored pristine and maximally attractive. The reserved §15.5a fields
become real frame-free measurements on the radio backend, and the ranking
input moves to the interference-inclusive total (filling the fields alone
would have left the defect intact).

**Changed sections (§15.5a occupancy block).**
- `interference_util_permille`: saturating index `1000·r/(r+H)` of the
  frame-free false-alarm rate over the observe window (H = 200 FA/s Tier-2
  seed — the form the vendored chanmig scorer proved on-air). An index
  comparable within one adapter, not an absolute duty cycle. `null` on
  sensor-less backends.
- `noise_dbm`: absolute idle floor where the generation provides it, else
  the passive `rssi − snr` floor, else the v1 min-RSSI proxy (labeled).
- `util_permille`: total occupancy `min(1000, wifi_util + interference)`;
  equals `wifi_util` on sensor-less backends by construction.
- `emptiest()` ranks on `util_permille` — the structural fallback covers
  kernel-monitor (frozen, #120) without a special case.
- Dwell hygiene: retune → settle → discard barrier (throwaway delta drain)
  → observe → read; interference denominator is the observe window.
- NHM excluded from reported fields (generation-dependent floor); it may
  inform the #100 scoring layer only. Nullable convention restated.
- Struck the false clause that the candidate craft's own traffic is
  excluded from its channel's counts — exclusion is per adapter (Pass 65),
  and the accounting never excluded the craft.

**Evidence.** Issue #95 (the blind-metric chain, symbol-cited); vendored
chanmig `ChannelScore.cpp` `cell_occupancy` (fa_half model, NHM exclusion
rationale); `docs/scout-design.md` §6 field reservation.

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
