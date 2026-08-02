# Pass 134/135 — the first calibration through the new guards (2026-08-02, 10 m)

Craft `.2.232` (SSC338Q, 8812EU), ground x86 (8812EU uplink + 8812CU
diversity), 5805/HT20, announced mode, ~10 m separation (RSSI −52..−56).
Both nodes at `max_power_qdb: 108`, `power_presets_qdb: [60,76,84,92,108]`.

Combined `start_both`: uplink ~220 s → §11.7 CALIBRATE → craft ~80 s, both
persisted, sequencer `done/done`.

## Preconditions actually checked first

- craft accepted-report cadence **10.00 Hz** (§10.6 start precondition needs
  ≥6; the run that produced the bad artifact below was at ~4.8 Hz)
- video 2‰ post-diversity, profile 5, `uplink_quality_valid: true`

## Craft downlink — `fp=26`, the craft's first valid artifact

```
rung    dBm  loss‰  rssi  wall rssi
   0   27.0      5   -44       none
   1   27.0      3   -44       none
   2   25.0      6   -45       none
   3   25.0      3   -45       none
   4   19.0      1   -51       none
   5   17.0      1   -53        -50
   6   17.0      0   -53        -50
   7   17.0      2   -53        -48
curve_qdb (level-4 ref): 27 27 27 27 23 21 23 23 dBm
```

Monotone non-increasing, 10 dB backoff, three real overload brackets booked
(rungs 5–7). Rungs 0–4 are ceiling-limited, which is why `no_wall_found` did
**not** fire — the rule needs *every* rung to be wall-free.

Against the artifact that broke video (`../pass134-hw/craft-artifact-fp43-BAD.json`):

| | fp=43 (bad) | fp=26 (this run) |
|---|---|---|
| placements | 27/27/27/25/27/27/27/27 | 27/27/25/25/19/17/17/17 |
| loss at placement | 5,2,3,2,**0,0,0,0** | 5,3,6,3,1,1,0,2 |
| walls booked | **none** | rungs 5,6,7 |
| curve max (level-4 ref) | **33.0 dBm** | 27.0 dBm |
| report cadence during run | ~4.8 Hz | 10.00 Hz |

The `0‰ at 27 dBm on MCS4–7` row is the whole reason Pass 134 exists. Note
also the level-4 transform: the bad run's 27 dBm MCS7 placement was *stored*
as 33 dBm, because `curve_qdb[m] = placement − (level[m]−4)×8` widens on
low-level rungs.

## Ground uplink — `fp=239`

```
rung mcs   gi    dBm  loss‰  rssi     wall
   0   0  LGI   27.0      0   -50     none
   1   1  LGI   27.0     15   -50     none
   2   2  SGI   25.0     15   -52     none
   3   3  SGI   21.0      5   -52     none
   4   4  SGI   21.0     10   -51 23.0 dBm
   5   5  SGI   13.0      5   -56 23.0 dBm
   6   6  SGI   17.0     15   -53 21.0 dBm
   7   7  SGI   13.0     10   -58 21.0 dBm
```

14 dB backoff; four walls booked. Reproduces the shape of `fp=110`
(`../pass132-hw/uplink-artifact-10m-final.json`) from a different position —
rungs 0–2 ceiling-limited, high rungs walling near 21–23 dBm. Rung 6 placing
4 dB above rung 7 is a seek-grid artifact (both walls at 21 dBm), not a
disagreement.

## Why the ceiling had to be raised first

At `max_power_qdb: 84` every per-rung ceiling sat *below* its measured wall,
so no rung could find one and both directions would have refused with
`no_wall_found`. Measured at the hardware ceiling, trimmed at flight time with
the §11.7 `0x0A` tier — that is the workflow the tier exists for.

## Device-confirmed in the same session (Pass 135)

| check | result |
|---|---|
| tier 0/1/2 on the ground | `iw` 15.00 / 19.00 / 21.00 dBm, matching the presets |
| `both:true` after a CSA claim | ok; vcmd echo `{"cmd":"tx_power","arg":0,"state":"acked"}` |
| craft after fan-out (pre-calibration) | tier recorded, `effective:false`, `iw` unchanged at 27.00 |
| `both` with no claim | whole action refused; ground stayed at tier −1 |
| out-of-range / malformed | 409 on an unconfigured index; 400 on 9, −1, `{}`, `"low"` |

Code-level only: the clamp of a preset above `max_power_qdb` (unit-tested and
logged; never exercised on device, since all deployed presets are ≤ ceiling).

---

# Pass 135 §11.7 `0x0A` TX_POWER — device verification (2026-08-02)

Run after the calibration above, so the craft holds `fp=26` and the ground
`fp=239`. Every row below was issued as the menu row sends it.

## Correction to how the tier is described

Earlier wording said a tier "shifts the whole tapered curve". That is true of
the **calibration sweep** ceiling (`rung_max_qdb()`, tapered by the §10.2
level). The **runtime resolve** is a plain scalar clamp —
`resolve_power_qdb()` ends in `v = min(v, max_power_qdb)` — so a tier is a
flat ceiling applied to the already-shaped calibrated value. Measured against
the live `fp=26` curve:

```
tier            MCS0  MCS1  MCS2  MCS3  MCS4  MCS5  MCS6  MCS7
4 high (27dBm)   27    27    25    25    19    17    17    17   <- the curve
2 med  (21dBm)   21    21    21    21    19    17    17    17
0 low  (15dBm)   15    15    15    15    15    15    15    15

§10.5 latch at 19 dBm, for contrast:
                 19    19    19    19    19    19    19    19
```

It bites the LOW MCS rungs first — the ones calibration put highest — and
leaves the high-order rungs on their measured backoff until the tier drops
below them. The property that matters survives intact and is the whole reason
this is not the §10.5 latch: **a tier can never raise a rung above its
calibrated placement.** The latch at 19 dBm *raises* MCS5–7 from 17 to 19.

## Craft actuation — device-confirmed

At the live rung (profile 5, calibrated placement 17.0 dBm), only tier 0 is
below it, and only tier 0 moved anything:

```
tier 4 -> {"ok":true}  17.00 dBm   ceiling_qdb 108
tier 2 -> {"ok":true}  17.00 dBm   ceiling_qdb  84
tier 0 -> {"ok":true}  15.00 dBm   ceiling_qdb  60
```

Note `tier 4` sets a **27 dBm** ceiling and the radio stayed at 17.00 — the
tier clamps, it does not pull a rung up to its ceiling.

Pinned to rung 0 (`POST /api/v1/link/profile {"min":0,"max":0}`) to exercise
the full range, then back up — no hysteresis in either direction:

```
tier 4 -> 27.00 dBm    tier 2 -> 21.00 dBm    tier 0 -> 15.00 dBm
back up:               tier 2 -> 21.00 dBm    tier 4 -> 27.00 dBm
```

`tx_power_tier_effective` reads **true** on the craft now, where it read false
before the craft had an artifact — the field tracks reality rather than being
set once.

## Preset clamp — device-confirmed

Ground loaded with `power_presets_qdb: [60,76,84,92,200]` against
`max_power_qdb: 108`. 200 qdb is 50 dBm; if it ever reached `iw` the clamp
would be a lie.

```
config: adapter eu-uplink: power preset 200 qdb clamped to max_power_qdb 108 (§10.3)
GET  -> {"tier":-1,"presets_qdb":[60,76,84,92,108],...}   # clamped, not 200
tier 4 -> iw 27.00 dBm                                     # not 50
grep '5000 mBm|200 qdb' over the whole run -> no actuator line
```

Config restored byte-identical afterwards; the restored process logs no clamp.

## Known gap, deliberately not closed

There is **no "clear tier" verb.** `{"auto":true}` clears the §10.5 latch, but
a tier only returns to `-1` on a waybeam-link restart, which re-reads
`max_power_qdb` from config. After any tier selection the index therefore
reads `0..4` rather than `-1` even when the selected value equals the boot
ceiling (hardware-identical, but not identical to report). Raised rather than
silently adding a verb.
