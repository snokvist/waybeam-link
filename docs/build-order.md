# Build order + de-risking gates

Do not start code until PROTOCOL.md is signed off. When it is, build in this
order (from spec §12, with the two measurement gates front-loaded).

## The two gates that can kill the premise — measure FIRST

Before trusting the ARQ / adaptive machinery, prove these two numbers on the
bench rig (the 1×8812AU + 2×8812CU fleet already works for RX):

1. **Cross-adapter loss correlation.** The whole "diversity is primary, ARQ is
   non-load-bearing" thesis assumes per-adapter losses are substantially
   *decorrelated*. Same-channel co-located antennas can be highly correlated
   (correlated fade → diversity gives little → ARQ silently becomes load-bearing,
   the one thing the design forbids). Compute the loss-correlation coefficient
   from the per-adapter `uniq`/`lost` + `diversity` counters (LINK_REPORT, §13.2)
   under the synthetic-loss injector and in a real fade. **Gate: correlation low
   enough that post-diversity delivered loss ≪ single-adapter loss.** If high,
   revisit FEC before building ARQ.

2. **NACK → RETRANSMIT round-trip latency distribution.** ARQ's entire value is
   recovering the ~5–30 ms correlated-fade band *within deadline*. The recovery
   round trip on a saturated, half-duplex, same-channel uplink may not fit.
   Measure issued-NACK → received-RETRANSMIT latency under load. **Gate: the P90
   fits inside the I-frame-class deadline budget (§8/§13.5).** If it doesn't, ARQ
   only helps the very longest-deadline blocks — scope it down accordingly.

## Order

1. Wire header encode/decode + session model (§2–3). Pure, unit-testable.
2. TX framer with RTP boundary detection + resend ring (§5); classifier stubbed
   to the size-heuristic first (§4.1 fallback).
3. RX merge/dedup/gap-detector with both short-circuits (§6) + NACK generation.
4. `loopback` mode + synthetic-loss injector + counters (§9.2) — **run gates 1&2.**
5. Air-side resend scheduler: priority / airtime cap / hold-down (§5.3, §7).
6. Adaptive link layer (§13): LINK_REPORT, rule cascade, MCS↔bitrate sequencing,
   fail-safe watchdog. Reuse `link_controller` constants verbatim (docs/groundwork).
7. Per-MCS TX power (§14).
8. NAL-type classifier for the RTP profile (§4.1).
9. Field bring-up (§9.3).

## Non-negotiable operational rules (carried from the ecosystem)

- **Single bitrate authority** (§13.6) — verify no competing writer before flight.
- **Write bitrate only on change** — every venc `/set` persists to the overlay;
  10 Hz writes = flash wear.
- **Fail toward degradation** on lost feedback (§13.8) — never hold a high
  operating point on a link you've lost contact with.
