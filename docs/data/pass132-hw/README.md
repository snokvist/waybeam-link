# Pass 132 hardware campaign — 2026-08-02

Craft `.2.232` (SSC338Q, IMX335), ground = x86 NucBox, 5805 MHz HT20, announced
mode (no `csa_psk` anywhere). Both ends at branch
`impl/pass131-eight-rung-uplink-calibration`.

**Geometry caveat: craft RSSI at max power is −13 dBm — far closer than the
2–10 m procedure.** The placements below are therefore a valid measurement of
*this* geometry and a valid demonstration of the mechanism; they are NOT a
commissioning result for a flying craft.

## Result — 8 rungs, `done`, ~125 s

| rung | GI | qdb | dBm | rssi | loss‰ | first_bad_qdb |
|---|---|---:|---:|---:|---:|---|
| 0 | lgi | 108 | 27.0 | −13 | 12 | none |
| 1 | lgi | 108 | 27.0 | −13 | 10 | none |
| 2 | sgi | 92 | 23.0 | −14 | 12 | none |
| 3 | sgi | 68 | 17.0 | −22 | 15 | 100 |
| 4 | sgi | 68 | 17.0 | −22 | 15 | 100 |
| 5 | sgi | 68 | 17.0 | −22 | 12 | 84 |
| 6 | sgi | 68 | 17.0 | −22 | 15 | 84 |
| 7 | sgi | 52 | 13.0 | −26 | 10 | 84 |

**Monotonically decreasing with rung, 14 dB MCS0→MCS7.** This is the TX PA
oversaturation signature: higher-order modulation needs better EVM and so
leaves the PA's linear region at lower commanded power. `first_bad_qdb` is
booked only where a bad probe sits ABOVE a clean one (rungs 3–7); rungs 0–2
swept clean to `max_qdb` and correctly record no ceiling.

It also settles the eight-rung question empirically: the previous single-rung
artifact placed MCS0 at 27.0 dBm, and applying that to an MCS7 uplink would
drive it **14 dB into saturation**.

## Device-confirmed

- §3.16 unauthenticated (31 B) works in announced mode with no PSK configured.
- The 35→31 B wire break fails LOUDLY: before the craft was upgraded, feedback
  went to `reports_received: 0` rather than mis-parsing.
- §3.15a `report_latch_holder` satisfies the new §10.7 authority prerequisite.
- A failed run persists nothing (verified twice — the artifact kept its prior
  contents through both failures).
- All actuators restore; link returns to profile 5 / 8‰ after each run.

## Two defects this campaign found (both fixed here)

1. **Budget top-up runaway.** Re-arming the probe budget from the *committed*
   epoch count, while it is spent at *build* time, re-armed faster than the
   §7.2 batch drained: `sent=3480/100`, flooding the return path until the
   craft's §3.16 could not get out (`quality_lost`). Fixed by issuing the
   burst once and ending it on `probe_spent() && report_ret_held.empty()`.

2. **Unpaced bursts overflow the quiet gap.** Dumping 100 probes into one
   return window lost ~40% of every dwell *regardless of commanded power*
   while RSSI tracked power perfectly — a flat floor that made every rung read
   `no_clean_point`. Baseline measurement settled it: normal 1-per-window
   traffic delivers **99.7%** on this link. Paced to
   `kReportsPerReturnWindow = 3`; at 8 the floor was still 40–155‰.

## Combined `start_both` — device-confirmed

One operator action, both directions, ~300 s end to end:

```
t+0..165s   phase=uplink    8 rungs        -> uplink=done
t+180s      phase=downlink  §11.7 CALIBRATE issued, craft=running
t+180..270s craft sweeps its own 8 rungs   (r0 -> r6)
t+300s      phase=done      craft calib_state=done fp=250
```

Uplink placements at `kReportsPerReturnWindow = 1` (loss 0-10permille):

| rung | GI | dBm | rssi | loss‰ |
|---|---|---:|---:|---:|
| 0 | lgi | 27.0 | −13 | 0 |
| 1 | lgi | 27.0 | −13 | 0 |
| 2 | sgi | 27.0 | −13 | 5 |
| 3 | sgi | 25.0 | −13 | 10 |
| 4 | sgi | 17.0 | −22 | 0 |
| 5 | sgi | 17.0 | −22 | 5 |
| 6 | sgi | 17.0 | −22 | 5 |
| 7 | sgi | 17.0 | −22 | 0 |

The PA-saturation curve reproduces (27.0 dBm at the low rungs, 17.0 at the
high ones) with more margin than the 3-per-window run.

**Order law verified negatively too:** an earlier `start_both` whose uplink
phase failed stopped with `uplink_phase_failed` and `craft_calib=idle`
throughout — §11.7 `CALIBRATE` was never issued.

### Why the pacing ended at 1 per window

3 per window put verify dwells at 22-30permille — straddling `loss_ok_milli`
(15), so a run was a coin flip: one completed, the next failed
`verify_failed` after exhausting its descent budget. At 1 per window the
dwells read 0-10permille. The speedup was never about packing the gap; it is
about using EVERY gap (~60/s at 60 fps) instead of every sixth at the 10 Hz
cadence — same traffic shape §7.2 is engineered for, 6x the rate.
`uplink_verify_epochs` 400 -> 200 to hold the run near ~2.5 min/direction.
