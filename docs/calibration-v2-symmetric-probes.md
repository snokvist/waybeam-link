# Calibration v2 — symmetric probe exchange

Status: **design approved — operator rulings Q1–Q3 resolved 2026-08-06**
(see §6). Still Tier 2 in the sense that nothing here amends `PROTOCOL.md`:
the implementing PR (issue #116) opens with ONE tier-1 spec amendment + Pass
that realises §6's rulings, then the code.

Direct ancestor: the Pass 152 field addendum (last entry of
`review-log-archive-p001-152.md`), which ends on exactly the question this
design answers — *"whether report-loss is the right observable for §10.7 at
all, or whether the section needs dedicated probe traffic rather than
piggybacked LINK_REPORTs."*

## 1. Why redesign instead of patch

The §10.5–§10.7 arc spent ~17 numbered passes (120–152) on one subsystem and
reversed itself at least 14 times (churn record: coordination review,
2026-08-06). The reversals were not random — nearly every one traces to two
design choices made before the physics was understood:

1. **The probe is the payload.** Downlink calibration scores the *live video
   stream's* loss from ordinary LINK_REPORTs; uplink calibration extracts loss
   from 2 Hz cumulative counters summarising 10 Hz report delivery. Video is
   therefore simultaneously the measurement source and the interference that
   pollutes it — the §10.7 contention floor, the floor-relative walls
   (Pass 152), the frozen pre-run floor window, and the "is the observable
   even right" open question all descend from this single coupling.
2. **Cumulative counters instead of per-dwell tallies.** §3.16's
   `reports_received` counter forced counter domains, wrap handling, resync
   detection, drain windows (`uplink_drain_ms`), and a dual-clock liveness
   gate (`UplinkQualityGate`) — four passes (125/126/128/132) repaired the
   counting scheme before Pass 132 noticed the ground could always just count
   its own probes.

Add the statistics: at the measured ~80 ‰ report-loss baseline, a 200-probe
verify dwell has σ≈19 ‰ — placements 15 vs 10 ‰ are indistinguishable and
"unreachable" rungs are part coin flip (`findings.md`, 2026-08-06). The 10 Hz
report cadence caps sample rate, so honest discrimination needs n≈1500/dwell —
hours per run. The observable is under-powered *by construction*, and no
threshold fixes that.

What DID survive contact with hardware, distilled from the same 17 passes:

- **Sweep power up from the safe end**, full commanded range, per rung.
- **Count your own probes** — the sender's own emission count is the exact
  denominator (Pass 132).
- **Score delivered loss at fixed MCS**, not RSSI — with the loss-minimum
  interior placement, because the PA compresses: maximum RSSI is not maximum
  usable power (Passes 150/151, `PowerSeek`'s min-hunt + overload brackets).
- **Identity-gated artifacts + STALE** (3-tier `calib_id`/ifname-MAC/bus-path,
  Pass 146 — hardware-forced: USB serials are all `123456`).
- **Refuse false success** (artifact-write-failed, flat-at-ceiling — each
  refusal later fired correctly on hardware).
- **The restore law** — every exit restores rate then power on one edge.
- **The craft has no IP path in flight** — craft-side runs stay VCMD-triggered
  (§11.7 `0x08`) with progress aired in the §3.15 calibration word.

v2 keeps every item on that list and deletes the machinery that existed only
to serve choices (1) and (2).

## 2. The design

**Premise shift:** calibration is stationary and pre-flight (already ruled,
Pass 132 era), and it needs video neither displayed nor **transmitted**. So:
pause the video feed for the run, and give both directions one dedicated,
identical measurement primitive.

### 2.1 One dwell, both directions

```
sender:    at (rung, qdb): emit N probe frames, numbered 1..N, MTU-padded
receiver:  count them, then return ONE tally: {run_id, dwell_id, received, rssi_sum}
sender:    loss = 1000 * (N - received) / N     — self-denominating
```

- **Probe frame:** `run_id`, `dwell_id`, `seq`, padded to the negotiated MTU
  so loss-at-MCS transfers to flight load. (Today's uplink probes are small
  10 Hz reports — representative of nothing the link carries in flight.
  Frame size is a first-order term in PER at a given MCS.)
- **Tally:** per-dwell, not cumulative. A lost tally is retried by dwell_id;
  a duplicate is idempotent. No counter domains, no resync, no drain window,
  no dual-clock gate — the dwell_id *is* the synchronisation.
- **Density:** with video silent the channel belongs to the probes. N is a
  knob (Tier 2), but nothing caps it at 10 Hz any more — n in the hundreds
  per second is available, which is what the statistics finding says honest
  discrimination needs. Dwell wall-time can *drop* while sample count rises
  ~100×.
- With video silent the contention floor is structurally zero, so **absolute
  loss walls work again**. Walls/gates stay config knobs (Tier 2) until the
  mechanism has enough hardware history for one settling amendment.

### 2.2 Roles: the same engine, swapped

- **Downlink (§10.6):** craft is sender, ground is receiver. The run stays
  craft-resident (no IP path in flight): ground triggers via VCMD `0x08`,
  watches the §3.15 word, exactly as today. Only the *evidence source*
  changes: instead of inferring loss from LINK_REPORT video statistics, the
  ground returns per-dwell tallies on the existing report path.
- **Uplink (§10.7):** ground is sender, craft is receiver. The ground-resident
  sweep, REST trigger, and artifact store stay; the craft returns per-dwell
  tallies over the air (0xF TALLY subtype — ruled, §6).
- **Sequencer:** `start_both` keeps its order law (uplink first) but both
  phases now use the same dwell primitive, so the sequencer stops needing
  per-direction special cases.
- **Uplink scope reduction:** calibrate only the configured `air.uplink_rate`
  rung until a shadow-the-downlink policy actually exists. The artifact
  schema was designed for appending entries (§10.7 forward-validity), so the
  other seven rungs cost nothing to defer — this reverts Pass 131's widening,
  which served a hypothetical.

### 2.3 What this deletes

| Deleted | Why it existed |
|---|---|
| `UplinkFloor` estimator (app/main.cpp §10.7 pre-run floor) | video-vs-probe contention |
| Floor-relative walls + sample-count floor gate (Pass 152) | same |
| §3.16 cumulative-counter apparatus (domains, wrap, resync) | cumulative counters |
| `UplinkQualityGate` dual clocks (`uplink_quality.h`, ~210 lines) | same |
| Drain windows / `burst_spent` micro-states (`uplink_calibrate.h`) | same |
| "quality only before live DATA" prepend rule + `tools/verify_quality_guard.py` | riding the video stream |
| Report-health start precondition | probe delivery is its own health check |
| First-clean confirmation dwell (likely — n is now large) | small-n flukes (Pass 133) |
| 7 of 8 uplink rung sweeps (deferred, not lost) | Pass 131 hypothetical |

**Kept unchanged:** `PowerSeek` (`core/include/wblink/calibrate.h`) — its
sweep/verify phases, min-hunt, brackets and 3-descent budget encode the PA
physics; both artifact stores + identity tiers + STALE; the restore law;
VCMD trigger + §3.15 word; REST surface.

### 2.4 Video pause

The run starts by silencing the video input and ends by restoring it (both
edges under the existing restore law — rate, power, and now feed). HEARTBEAT
and ANNOUNCE continue untouched. Mechanism: input-starve (ruled, §6). Consequence
worth stating: a calibration run no longer needs a display, a decoder, or
even a camera feed — a headless bench with two nodes is sufficient.

## 3. Library shaping

The engine becomes a pure `core/` module: state machine with injected time
(already the house style), plus injected `send_probe(bytes)` and
`on_tally(dwell)` callbacks. `io/`/`app/` own the wire and the pause
actuation. This is exactly the extraction boundary PR #109 wants — the
calibration engine should be the first module written *natively* in the
library shape rather than migrated into it.

## 4. Statistical sizing (Tier 2, knobs)

Initial seeds, to be measured not ruled: probe N per dwell such that
`1000/N ≤ loss_ok_milli` still holds (one lost probe decidable — Pass 132's
rule, kept), with N≈500–1000 now cheap at probe density. Verify dwells sized
so σ at the expected loss scale separates the walls by ≥3σ; the findings
entry has the arithmetic. These live in `policy.calibration` and
`findings.md` until settled.

## 5. Relationship to PR #114 and migration order

1. **PR #114 (offset-space re-base) lands first**, as the safety re-base —
   offset-space actuation is understood physics (PA compression, Tier 1
   keepable), but its still-contested wall/placement text should be softened
   to knobs + findings before merge, per the two-tier law.
2. The v2 spec amendment (one Pass) then replaces §10.7's evidence plumbing
   and §10.6's evidence source, deleting §3.16's cumulative form.
3. `uplink_verify_epochs` 400-vs-200 drift (`findings.md`) dissolves in the
   same amendment — the parameter set changes shape.
4. Implementation in the library shape (§3), behind the existing REST/VCMD
   triggers so hub/menu consumers see no interface change.

## 6. Operator rulings (resolved 2026-08-06)

Context that forced the shape: the packet type is the low nibble of
`ver_type` (§3.1) — types `0x1`–`0xF` are all assigned, so **the type space
is full** and a fresh type does not exist without a version bump.

- **Q1 + Q3 — RULED: type 0xF becomes the CALIBRATION family.** The
  cumulative §3.16 UPLINK_QUALITY layout is retired. 0xF carries a subtype
  byte: **PROBE** (run_id, dwell_id, seq, padded to negotiated MTU; both
  directions) and **TALLY** (~16 bytes per-dwell: run_id, dwell_id,
  received, rssi_sum). Probes are distinct from DATA and exempt from
  FEC/ARQ accounting (§14.1a class-exemption precedent). Receivers already
  ignore unrecognised 0xF layouts, so a mixed-version pair degrades to
  "calibration unavailable", not a fault.
- **Q2 — RULED: input-starve.** The link node stops reading its
  RTP/frame-shm input for the run; venc keeps encoding and frames drop at
  the ring (drop-not-block — harmless). No venc API coupling; restore is
  "resume reading", on the same edge as the rate/power restore law. Works
  identically on bench rigs driven by `tools/rtp_feed.py`.

## 7. Verification plan (for the eventual implementation)

- Unit: engine dwell protocol under injected loss/duplication/reorder of
  tallies; restore-on-every-exit including feed restore.
- Loopback bench (no radio): two processes, `air.rx_drop_permille` shaping
  known loss — placement must track the shaped knee within one seek step.
- Hardware, headless (no display): x86 ground + .232 craft at 2–10 m,
  `start_both`; compare v2 placements and run wall-time against the archived
  Pass 152 campaign (`fp=0x6c` downlink / `fp=0x99` uplink) at the same
  geometry. Success = same-or-better placements, run time reduced, and rung
  repeatability across 3 consecutive runs within one seek step — the
  repeatability the old observable's σ could not deliver.
