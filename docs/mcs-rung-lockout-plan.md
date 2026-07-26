# Per-rung MCS lockout after a loss-driven demote — plan (not designed, not scheduled)

Status: **backlog placeholder.** This document exists so the idea survives
until someone picks it up; it makes no ruling and proposes no config/wire
change. Per `CLAUDE.md`'s law, any actual cascade change needs an operator
ruling and a numbered `docs/review-log.md` Pass before code — this doc is the
"raise it" step, not the ruling.

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

## Proposed direction (sketch only — needs real design)

A temporary lockout keyed to the specific rung a loss-driven demote (§9.1
rule 1, not RSSI-floor/fade) just left, so promote's rung selection skips it
until the lockout clears — independent of (or layered under) the existing
flap-avoidance timers.

## Open questions this doc deliberately does not answer

1. **Cascade placement** — does this gate promote's target selection
   (§9.4), or does it need to be its own cascade rule ahead of promote?
2. **Scope** — per-rung state (an array parallel to `rung_rssi_floor_dbm`)
   or just "the single most-recently-demoted rung"? The former handles a
   link with multiple bad rungs; the latter is much less state.
3. **Duration** — a fixed §17-style seed (bench-derived, config-overridable
   like everything else in this cascade), or adaptive to how far over
   `demote_milli` the triggering loss measurement was?
4. **Decay/reset semantics** — plain timer expiry, N consecutive clean
   reports at a lower rung, or cleared unconditionally on a CSA channel
   move (a new channel has no reason to inherit the old one's lockout)?
5. **Relationship to §9.2's max-probability rung** — that machinery already
   ages per-rung success-EWMAs toward a physics prior and demotes *toward*
   the max-probability rung under multi-rung stress. Is a lockout a
   distinct mechanism, or does it belong as an extension of the §9.2
   EWMA/aging state instead of new state?
6. **Relationship to existing §9.7 layers** — does this replace hard
   flap-freeze for the loss-driven case specifically, extend it (e.g.
   flap-freeze duration scaled by rung-specific history), or coexist as an
   independent, finer-grained layer? Simply re-deriving
   `flap_freeze_count`/`flap_freeze_window_s`/`flap_freeze_s` (they are
   §17 "measure, not design" seeds already) might close some of this gap
   with zero new code — worth ruling out before designing anything new.
7. **Observability** — if it exists, an operator needs to see it (§15.3
   stats) — "rung 5 locked until Tms" — or a lockout that silently changes
   selector behavior is a debugging trap for the next session that hits it.

## Not done

No code. No config keys. No spec ruling. No Pass entry — this document is
the pre-ruling backlog item, not the ruling itself. Pick a design (or decide
retuning §9.7's existing seeds is sufficient and this isn't needed at all)
before writing any of the above into PROTOCOL.md.
