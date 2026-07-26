# Per-rung MCS lockout and loss-classification plan

Status: **operator-approved for implementation, Pass 110.** `PROTOCOL.md`
§3.15/§9 is authoritative. This document retains the field context and records
the staged implementation/verification plan.

## Background

2026-07-26 field session on craft `.232`: operator reported the adaptive
selector falling back to MCS0 repeatedly, with other (fixed-MCS) venc modes
looking stable by comparison. Root cause had two parts:

1. Leftover vehicle-command test state from an unrelated hardware sweep
   (`cmd_arq=false`, `cmd_selector_frozen=true`) — fixed by resetting those
   flags. Real, but not the whole story.
2. A genuine, sustained RF condition: on channel 5220 MHz (DFS/UNII-2), the
   top rung (MCS5) measured 2-4% real packet loss despite excellent RSSI
   (-26 to -29 dBm, far above the rung's -73 dBm floor). A/B against 5805 MHz
   (non-DFS): 0.2-0.6% loss at the same RSSI, zero demotes. Channel-specific
   interference/congestion, not a code defect.

Immediate mitigation (merged, `waybeam-link` PR #63,
`core/include/wblink/selector.h` / `io/include/wblink/config.h`): raised
`demote_milli` default 20‰ → 45‰ (2% → 4.5%), per PROTOCOL.md §17's own
guidance for this knob ("raise until decode errors clear at target range").
Bench-verified on 5220 MHz: zero demotes over 30s at profile 5, loss
1.0-2.4%. See `docs/step11-bench.md` §4.8 for the full writeup.

**This widens the tolerance band. It does not change the underlying
dynamic.** Whatever loss rate finally exceeds the (now higher) threshold will
still trigger the same cycle described below — it just takes longer to
arrive at it.

## The problem

PROTOCOL.md §9.1's cascade is a strict priority order:
reactive-demote → RSSI-floor → RSSI-fade → backpressure-escape → promote →
hold. **Promote (§9.4 v0) is RSSI-margin-gated only** — `rssi ≥
next_rung_floor + promote_rssi_hyst_db` — it has no memory of *why* the
selector left that rung. A demote triggered by real, sustained packet loss
at excellent RSSI looks identical to promote's gate as a demote that never
happened: RSSI recovers instantly (it never left the floor), so promote
re-attempts the same rung as soon as `promote_dwell_s` elapses.

§9.7 already has three flap-avoidance layers, and they are the closest
existing thing to this idea — worth being precise about why they don't
close the gap on their own:

- **Soft reentry** — re-promoting into a just-demoted rung within
  `reentry_backoff_s` (5.0s) needs `reentry_dwell_s` (2.0s) dwell instead of
  the normal 0.5s. This only slows the re-attempt down; if the channel
  condition doesn't happen to manifest inside that one 2s dwell window
  (plausible for bursty/duty-cycle interference, which is a reasonable model
  for DFS-adjacent congestion), promote commits anyway.
- **Hard flap-freeze** — 3 fast re-demotes within a 10s window freezes the
  rung below for 10s. This is the layer that should eventually catch a
  repeat-offender rung, but it is uniform and time-boxed regardless of *how
  bad* the loss was or *how persistent* the RF condition is. For a channel
  condition that is sustained (not transient), a fixed 10s freeze doesn't
  avoid the bad rung — it just delays the next attempt at it by 10s, and
  operator language for this failure mode was literally "over and over,"
  consistent with a freeze that keeps expiring and re-triggering rather than
  one that never engages.
- **`min==max` pin** — works, but is a manual/operator action (or a mode
  apply setting the MCS window), not something the selector does on its own
  in response to what it's observing.

None of the three are *loss-rate-aware* or *rung-specific* in the sense of
"rung 5 specifically has a sustained loss problem, don't re-attempt it until
conditions genuinely look different" — they're uniform dampers keyed on
demote *frequency*, not on which rung or how far over threshold the loss
was.

## Adopted design

- Raw confidence-qualified loss separates a one-window emergency from a
  five-point leaky persistent score. EWMA remains observable but does not make
  that classification.
- Emergency loss moves to the mode/table-resolved floor. Persistent loss moves
  exactly one rung. Both charge only the vacated rung.
- Per-rung strike count + timeout + latch: 30 s for strikes 1–3, strike 4
  latched until channel/bandwidth change, reporter session/source change, or
  restart.
- The lowest blocked rung is an upward ceiling; promotion and pressure escape
  cannot skip it. Existing soft reentry/hard flap freeze remain independent.
- Pins retain operator precedence and expose a conflict instead of being
  silently weakened.
- Craft authority crosses to ground in a 25-byte 2 Hz `SELECTOR_STATE`; the
  ground hub displays the limit and never derives it from local loss.

## Implementation stages

1. Codec, policy/config, selector state/reason observability, and correction of
   LINK_REPORT `uniq` to the specified interval denominator.
2. Emergency/persistent classifier, exact-one-rung persistent demotion,
   lockout/latch, promotion gates, environmental reset.
3. §15.3 local stats plus craft→ground `SELECTOR_STATE` mirroring and expiry.
4. waybeam-hub semantic metric and ground flight OSD text:
   timed `RF ≤M4 23s` (amber), latched `RF ≤M4 CHAN` (red).
5. Host sanitizer tests, SSC338Q cross-build, hub tests/build, then device
   validation on craft `.232`.

## Device matrix

- One high-loss window versus an under-filled/high-percentage window.
- Sustained 4–6% and recurrent 2-bad/1-good windows.
- One-rung persistent transition versus emergency-to-resolved-floor.
- Resolved floors at MCS0/MCS1/MCS2 mode bands.
- 30 s expiry, retained strikes, fourth-strike latch.
- Same-channel mode change retains state; successful channel/bandwidth change
  clears it; reconnect/session replacement clears it.
- Promotion and backpressure escape stop at the effective ceiling.
- Ground stats and OSD agree with craft stats; no automatic CSA is issued.
