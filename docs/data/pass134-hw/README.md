# Pass 134 hardware evidence — 2026-08-02

Craft `.2.232` (SSC338Q, 8812EU `wlan0`), ground = x86 host (8812EU
`wlx84fc1450bcde` uplink + 8812CU diversity), 5805/HT20, announced mode.
Bench range throughout (RSSI −20..−26) — **deliberately**, because the
question Pass 134 answers is what the guards do when no wall exists.

## Uncalibrated TX power (measured)

With neither `power_map` nor an artifact, `resolve_power_qdb()` returns
`nullopt` and the controller issues **no power command at all**; the adapter
stays on `iw txpower auto`.

| node | adapter | `auto` reads |
|---|---|---|
| ground uplink | 8812EU | **19.00 dBm** |
| ground diversity | 8812CU | 25.00 dBm (RX-only) |
| craft | 8812EU (SigmaStar) | **27.00 dBm** |

The reported number is a *cap the driver will not exceed*, not evidence of a
per-rate taper below it — `iw` cannot see the TXAGC table.

Nothing else clamps. On the ground uplink, whose phy advertises 20.0 dBm max
at 5805:

```
cmd 2000 mBm -> readback 20.00
cmd 2700 mBm -> readback 27.00
cmd 3000 mBm -> readback 30.00     accepted, no error
```

## `craft-artifact-fp43-BAD.json`

The §10.6 artifact that broke video. `placement_qdb`
`[108,108,108,100,108,108,108,108]` (27 dBm on seven rungs) with
`placement_loss_milli` `[5,2,3,2,0,0,0,0]`. Produced through a return path
starved to ~4.8 Hz by the §7.2 flush regression fixed in `f9a4f35`. Its
`curve.txt` re-referenced MCS4–7 to **31/31/33/33 dBm** via the §10.2 level
transform — the transform widens the exposure.

## `uplink-artifact-fp164-bench-ceiling-mask.json`

The run that falsified the §10.7 exemption (addendum 2). §3.16 counter stream
healthy throughout — verify dwells returned real 0–10‰ on every rung — and
every rung still placed at its §10.3 ceiling with `first_bad_qdb: null`:

```
rung  mcs   gi   place    dBm  loss‰  rssi  first_bad
   0    0  LGI      84   21.0      5   -20       None
   1    1  LGI      84   21.0      5   -20       None
   2    2  SGI      76   19.0      0   -22       None
   3    3  SGI      76   19.0      5   -22       None
   4    4  SGI      68   17.0      0   -24       None
   5    5  SGI      68   17.0     10   -24       None
   6    6  SGI      60   15.0      5   -26       None
   7    7  SGI      60   15.0      5   -26       None
```

That is `max_power_qdb` 84 tapered by levels `{4,4,3,3,2,2,1,1}` read back
verbatim. It reached `done` and replaced the good 10 m `fp=110`
(`../pass132-hw/uplink-artifact-10m-final.json`), which had to be restored by
hand.

## Verified after the fixes

| check | result |
|---|---|
| craft return-path cadence | **9.83 Hz** accepted (was ~4.8 Hz) |
| §10.6 run at bench range | `no_wall_found`, nothing persisted, 3× |
| 8-rung §10.6 wall clock | ~80 s |
| §10.6 restore with no curve | `restore -> backend auto`, craft back to 27.00 dBm |
| §10.7 run at bench range | refused, `fp=110` intact on disk |
| combined `start_both` | stopped at `uplink_phase_failed`, craft left `idle` — never asked |
| ground artifact apply + §10.3 clamp | placement 108 qdb → `txpower fixed 84 qdb` → `iw` 21.00 dBm |
| video throughout | 2–3‰ post-diversity, profile 5 |

## Not measured here

No usable placement curve exists for either direction from this session — by
construction, since bench range has no wall. The 10 m `fp=110` uplink curve
from Pass 132/133 remains the only real measurement, and the craft has **no**
artifact at all. Both need a 10 m run.
