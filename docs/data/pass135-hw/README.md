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
