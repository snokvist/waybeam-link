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
