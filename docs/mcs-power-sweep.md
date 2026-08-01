# MCS × TX-power sweep — wire PER without bad-FCS

**Status:** tooling + first bench dataset. Follow-on from
`docs/per-mcs-per-ladder-plan.md` §6, where the Pass-119 bad-FCS numerator
was found blocked on every fleet chip (no driver delivers FCS-failed
frames). This is the "another way": measure per-rung failure rate from the
two ends' own counters, and use TX power as the controlled degradation axis
the plan's B2 wanted to get from physically walking the craft.

## Method

One dwell = (pinned rung, fixed craft TX power) held for N seconds.

- **Denominator** — craft `tx_submitted` delta over the dwell (§15.3).
- **Numerator survivors** — ground per-adapter `rx` delta. Wire PER per
  adapter = `1 − rx_Δ/tx_Δ`. Pre-diversity, per-adapter — deliberately so:
  this is the quantity a per-rung selector gate would consume; diversity
  merge sits *after* it.
- **Attribution is exact, not inferred.** Single-rung policy (§3.0 Pass 118)
  means every frame in a dwell was aired at the pinned rung; `rx_mcs` deltas
  confirm it per dwell (and `rx_mcs_unknown` must stay 0).
- **Axes** — rung via `POST :8091/api/v1/link/profile {"min":N,"max":N}`;
  power via `iw dev wlan0 set txpower fixed <mBm>` on the craft
  (kernel-monitor backend). Sweeping power *up* is as important as down: the
  §4.8 anomaly (2–4% loss at −26 dBm) is a high-power/high-RSSI failure, so
  the interesting region includes PA-overload territory, not just the fade
  floor.

Known measurement limits, stated so nobody plans around them silently:

- Snapshot skew: craft and ground stats are read ~100 ms apart; at a 20 s
  dwell that bounds the PER error at ~1‰. Don't shrink dwells below ~10 s.
- The craft `tx_submitted` includes HEARTBEAT/ANNOUNCE alongside the video
  feed — negligible against a live venc feed, dominant if the feed dies
  (visible as a collapsed `tx_frames`; discard such rows).
- Bench range (~50 cm) means the *fade* floor of upper rungs is unreachable
  by power alone (1 dBm floor ≈ −40 dBm RSSI at the desk). What the bench
  sweep does reach is the high-RSSI loss-floor region — the regime §4.8
  actually failed in. Fade-floor tails still need attenuators or range.

## Tool

`tools/mcs_power_sweep.py` — drives the live rig from the ground host,
restores the profile window and TX power on any exit. Output is one JSON
row per dwell (adapters: PER‰, RSSI, `rx_mcs` deltas; streams: delivered /
loss gauges).

```
python3 tools/mcs_power_sweep.py --rungs 0,2,4,5,7 \
    --powers 1,5,10,15,20,27 --dwell 20 --settle 6 --out sweep.jsonl
```

## Results — 2026-08-01 (craft .232 8812EU ↔ ground .242 EU+CU, 5805/HT20, ~50 cm)

Dataset: `docs/data/mcs-power-sweep-20260801.jsonl` (30 dwells × 20 s,
rungs {0,2,4,5,7} × {1,5,10,15,20,27} dBm). PER‰ shown as eu-uplink /
cu-diversity; RSSI is eu's mean. `rx_mcs_unknown` stayed 0 and the `rx_mcs`
delta landed in the pinned bucket on every dwell — attribution held.

| rung \ dBm | 1 (−40) | 5 (−39) | 10 (−33) | 15 (−28) | 20 (−23) | 27 (−19) |
|---|---|---|---|---|---|---|
| MCS0 | 16/13 | 12/10 | 15/13 | 16/14 | 16/13 | 19/19 |
| MCS2 | 4/5 | 2/0 | 2/0 | 12/10 | 4/1 | 8/7 |
| MCS4 | 8/8 | 8/5 | 3/1 | 7/6 | 0/0 | **1000/1000** |
| MCS5 | 1/4 | 6/3 | 4/2 | 9/6 | **49/16** | **1000/1000** |
| MCS7 | 1/4 | 4/0 | 0/0 | 13/10 | **628/328** | **1000/1000** |

(Values ≤ ~5‰ are at the floor of the method — snapshot skew bounds ~1‰,
and the USB2-hub contention baseline sits at a few ‰. A couple of −3‰
readings are that skew, not physics.)

### Finding 1 — the overload cliff is real, rung-ordered, and total

Above ~−25 dBm RSSI the link does not degrade gracefully at high rungs — it
falls off a cliff, in strict modulation order: MCS7 is already at 33–63%
loss at 20 dBm and **dead** (rx = 0 on both adapters) at 27; MCS5 survives
20 dBm at 2–5% and dies at 27; MCS4 is clean at 20 and dead at 27; MCS2 and
MCS0 survive everything. This is receiver-side nonlinearity (EVM floor at
strong signal), not fade — and it is precisely the §4.8 regime: the recorded
"2–4% sustained loss at −26 dBm" was not an anomaly but the *edge* of this
cliff.

**Both adapters die together.** In the overload region the diversity
assumption (§14, decorrelated per-adapter loss) inverts — loss correlation
→ 1, so diversity buys nothing exactly where the selector most needs help.

### Finding 2 — RSSI is non-monotonic as a quality proxy; §4.8's gate is refuted, not just miscalibrated

At −40 dBm every rung including MCS7 runs at ≤ 4‰. At −23 dBm — the
*highest* margin the §9.4 gate can see — MCS7 runs at 33–63% loss. Across
the top half of the observable range, more RSSI is *worse* for high rungs.
A promote gate of the form "RSSI ≥ rung floor + margin" is therefore wrong
in kind, not in tuning: no margin constant fixes a proxy that changes sign.

### Finding 3 — MCS0 has a flat ~10–19‰ loss floor, power-independent

MCS0 is *lossier* than every other rung at low power, flat across the whole
power axis (so not signal-related). Plausible mechanism: ~8× longer airtime
per frame → proportionally higher exposure to collisions with the uplink
return traffic on the same channel. Open question, worth its own look
(candidate check: correlate with return-window activity; the per-dwell JSONL
rows carry stream gauges).

### Method note

The dwell at rung 4 / 27 dBm also killed the *uplink-visible* downlink
completely; the selector was pinned so nothing reacted, but an unpinned
selector living purely on reports would have seen report loss, not a rate
signal. Recovery after the sweep was immediate and clean on restore.

## Implications for the §9.4 selector — proposal sketch (operator input wanted)

1. **Stop treating RSSI as monotonic.** The gate needs an upper operating
   band, not just a floor: promotion into rung r should require RSSI within
   `[floor_r + margin, overload_r − margin]`, with `overload_r` per-rung
   (measured here: ≈ −25 dBm for MCS7/5, ≈ −20 for MCS4 at this cal).
   Bench-calibratable per hardware; this sweep is the calibration tool.
2. **Per-rung PER memory (route 1, now trivial).** Under single-rung policy
   the selector can key its existing loss window by the committed rung —
   a per-rung PER EWMA with staleness, dwell-guarded around transitions.
   That is the passive ladder from the Pass-119 plan with *exact*
   attribution and zero wire change; the §4 emission-shape ruling shrinks
   to "how much of this table does §15.3 export".
3. **Promotion becomes probe-shaped.** With per-rung PER memory, promote =
   enter target rung as a probation window and let the existing §9 lockout
   machinery demote-on-strike; the probe result refreshes the rung's PER
   entry. RSSI's only remaining job is choosing *which* rung to probe.
4. **Couple power into the operating point.** The cliff moves with rung, so
   at close range the right move before promoting is often power *down*
   (§10.2 tiering / §10.5 override), not rung up. A joint (mcs, power)
   selection targeting an RSSI band of roughly [−45, −28] at the receiver
   would have avoided every failed dwell in this dataset.
5. **Re-run the sweep with the fleet `power_map` tiering active** to
   confirm shipped configs stay clear of the cliff at realistic ranges —
   this run bypassed tiering deliberately by forcing `iw` power.
