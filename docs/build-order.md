# Build order + de-risking gates

Aligned to PROTOCOL.md v1 §19 (build order) and §17 (bench gates). Do not start
code until PROTOCOL.md is signed off.

## Bench gates — the measurements that can change the design

Four gates (PROTOCOL.md §17). Two are premise-critical (2, 3); one is now largely
resolved (1); one governs the craft return path (4). Annotated with the minimum
hardware each needs — `loopback` mode has no hardware TSF and cannot validate the
TSF-dependent gates.

1. **One injecting `IRtlDevice` + N monitoring siblings in one process.**
   *Largely resolved:* multi-adapter single-process RX is proven at N=3 in
   Waybeam-android `:wifi` (per-adapter `libusb_context` + per-adapter thread,
   RX-only). Residual unknown = the **injector + monitors mix**: ground's
   designated-NACK-TX adapter running beside RX siblings, and the craft's
   single-adapter TX-video + RX-return (devourer Jaguar3 `enable_with_tx` set
   before `InitWrite`; measure the TX→RX settle time; 8812EU RX+TX is field-proven
   on wfb_ng at **20 MHz** — its 40 MHz bug is out of scope, run the craft at
   20 MHz). *Hardware-required.*

2. **Cross-adapter loss correlation ρ** — the whole "diversity primary, ARQ
   non-load-bearing, no FEC" thesis rests on per-adapter losses being decorrelated.
   Compute ρ as a **windowed P95** (not a one-shot mean — ρ is attitude/geometry
   dependent; a banking turn is the operative ρ→1 transient) from the per-adapter
   `uniq`/`diversity`/`adapters` counters, under the synthetic injector and in a
   real fade. **Gate:** low enough that post-diversity delivered loss ≪
   single-adapter loss. If the P95 tail is high, FEC (§14, GF(256) — not XOR)
   becomes justified. *Real-RF geometry required.*

3. **NACK → RETRANSMIT round-trip latency (P90)** — ARQ's value is recovering the
   short correlated-fade band *within deadline*. Measure issued-NACK →
   received-RETRANSMIT under load on the saturated half-duplex uplink. **Gate:** P90
   fits the I-frame-class deadline (§8). If not, ARQ only helps the longest
   deadlines — scope it down, and weight the §14 reactive-coded-repair hybrid.
   *Hardware-required.*

4. **Return-window fit + adaptive-loop stability** — governs the single-adapter
   craft return path. At target fps/bitrate: does the §7.2 TSF quiet-gap beat
   pure-opportunistic return delivery, and does the §9.8 damped step-down/promote
   pair hold a stable operating point rather than oscillate at the floor? This is
   where the "craft return is best-effort by physics" question is *resolved
   empirically*, per operator direction — not designed further on paper. *Real-RF,
   saturating injector, TSF-capable RX required.*

## Order (PROTOCOL.md §19)

1. Wire header codec + session model + big-endian + `table_version` hashing +
   plausible-forward clamp (§2–3, §6.6). Pure, unit-testable.
2. I/O binding layer + JSON config + JSON stats (§15), UDP-only.
3. TX framer + RTP boundary detection + resend ring (§5); classifier stubbed to the
   size heuristic (§4.1 fallback).
4. RX merge/dedup/gap-detector + both short-circuits + liveness watchdog (§6) +
   NACK generation.
5. Air-side resend scheduler: priority / airtime cap / global per-seq hold-down +
   first-latcher lock + per-originator budget (§5.3, §12).
6. `loopback` mode + synthetic-loss injector + counters (§16). *Run the
   loopback-measurable knobs; gates 1–4 need hardware (below).*
7. NAL-type classifier for the RTP profile (§4.1).
8. Adaptive selector: metric reporter (RX) + decision cascade + venc actuation +
   flap/fail-safe (§9); per-adapter TX power (§10).
9. Return-telemetry TSF quiet-gap optimisation (§7.2) — gated on hardware gate 1/4.
10. Follow-me CSA (§11) — gated on gate 1.
11. Field bring-up; run gates 1–4. FEC (§14) only if gate 2's P95 says so.

## Non-negotiable operational rules (carried from the ecosystem)

- **Single bitrate authority** (§9.6) — verify no competing writer before flight
  (disable hub `venc.bitrate_enabled`; do not run wfb_ng `link_controller`).
- **Write bitrate only on change** — every venc `/set` persists to the overlay;
  10 Hz writes = flash wear.
- **Fail toward degradation** on lost feedback (§9.8) — never hold a high operating
  point on a link you've lost contact with; under single-adapter craft this loop
  must be hysteretic + damped (gate 4).
- **Craft runs 20 MHz** (8812EU 40 MHz bug); intra-band fast-retune (§11) assumes
  same-BW.
