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

## Results

(populated from bench runs; see the dated sections below)
