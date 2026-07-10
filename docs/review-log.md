# Review log

This spec is reviewed several times before any code is written. Log each pass
here: date, reviewer, what changed, open questions.

## Pass 1 — 2026-07-10 — initial adversarial review + grounding

- Reviewed the v0 draft (§1–12) against the ecosystem and physics.
- **Findings folded in:**
  - §3 risk-3 (uncapped live bitrate) → resolved by the adaptive quota (§13.4):
    live video is now hard-bounded by airtime minus the ARQ reserve.
  - Diversity-correlation and NACK-round-trip flagged as the two premise-critical
    measurements → promoted to build gates (docs/build-order.md).
- **Adaptive link layer (§13) added**, calibrated from production controllers.
  Grounding overturned three assumptions (see groundwork.md): no composite quality
  score (rule cascade), promote is a V+2 probe, venc has no bitrate-authority flag.
- **Per-MCS TX power (§14) added** [pending — see groundwork.md TX-power section].

## Pass 2 — 2026-07-10 — TX-power corrections (operator)

- **Per-adapter, not fleet-global.** Each diversity adapter is a separate devourer
  `IRtlDevice` with its own efuse power calibration/antenna/role — power is set per
  device and differs per adapter. §14 reworked: power indexed by (adapter × MCS);
  absolute values live in a node-local per-adapter table; the on-air profile
  carries only a portable power *level/intent*. `tx_power_qdb` → `tx_power_level`
  in the example profile.
- **No regulatory clamp.** devourer's `SetTxPowerIndexOverride` is a raw absolute
  index (uncapped); the earlier "efuse owns the ceiling / regulatory-safe by
  construction" claim was WRONG and is removed (§14.2.1). Power is fully the
  operator's responsibility and may intentionally exceed regulatory limits;
  recommended opt-in `max_power` sanity ceiling in the controller.

## Open questions for the next pass

- [ ] Promote mechanism: ship v0 RSSI-margin (§13.4a) or invest in the active
      probe burst (§13.4b) from the start?
- [ ] `active_profile` echo on DATA header vs. a dedicated HEARTBEAT (§13.2.1) —
      header bytes vs. loss-robustness.
- [ ] ARQ scope after gate 2: if the round trip only fits the longest deadlines,
      is ARQ worth the complexity over pure diversity + concealment?
- [ ] Per-MCS TX-power granularity: per individual MCS vs. per rate-group, and
      whether devourer can write per-rate TXAGC live or only global power (§14).
- [ ] Bidirectional RC (uplink CONTROL stream) — currently out of scope (§11);
      decide if/when it folds into the multi-stream reserve (§13.9).
- [ ] Repo vendoring: private submodule vs. snapshot-into-public-consumer during
      the private phase (README "Consumers").
