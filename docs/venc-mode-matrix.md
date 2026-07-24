# The operating-mode matrix — user-facing modes over §9 rungs

**Status: DESIGN IN PROGRESS.** No spec amendment and no code yet. This file
collects the operator's product intent, the measurements taken so far, and the
questions still open. It exists so the work survives a context boundary.

Started 2026-07-24, out of the **B11** finding in
`docs/preflight-open-issues.md`: the §9.8 fail-safe floor rung (MCS0) delivers
3.34 % unrecoverable frames because nothing reduces the frame rate when the
rung's bitrate collapses. Fixing that turned out to need a user-facing model,
not a constant.

---

## 1. The goal (operator, 2026-07-24)

The user picks **two things**, and never sees a frame size, an MCS rung or an
fps number:

- **Latency** — high / medium / low. Lower latency = higher fps.
- **Range** — high / medium / low. Higher range = lower MCS rungs allowed.

Plus one shaping knob: **aspect ratio** (4:3 or 16:9), which selects among
sensor modes.

From those, the controller derives sensor mode, encode resolution, fps and the
allowed MCS band, and the dynamic loop (JSCC + bitrate + rung) keeps the video
inside the link. *"I don't want the user to have to know about frame sizes vs
mcs rungs vs fps — this is what we will figure out to give a good experience."*

Two later refinements from the operator:

- **fps is STATIC per mode.** Variable fps makes recording messy. There is one
  additional **variable-fps mode**, explicitly not record-friendly, which lets
  the link traverse the whole MCS 0–5 range by trading fps.
- **Standardize on 30 / 60 / 100 fps** — present on both the IMX335 and IMX415
  drivers. (See §6: the measurements argue for reconsidering 100 → 90.)
- The user generally always wants the **widest FOV** available for the
  trade-offs they chose.

---

## 2. Measured: the per-rung block-size ceiling

§9.5 derives the video bitrate per rung. §9.11's block target is
`min_p_frame_bytes` (seed **10000**, never §17-derived — see §7).

Derived bitrate assumes the deployed craft's `venc.max_bitrate_kbps: 25000`
clamp (Pass 75, SSC338Q CPU ceiling) and the 96 kbps control+telemetry reserve.

| rung | MCS | derived kbps | max fps that still yields a 10 kB frame |
|---|---|---|---|
| 0 | 0 | 3804 | **45** |
| 1 | 1 | 7704 | **90** |
| 2 | 2 | 12903 | 144 |
| 3 | 3 | 17236 | 144 |
| 4 | 4 | 25000 (clamped) | 144 |
| 5 | 5 | 25000 (clamped) | 144 |

**Inverted — this is the load-bearing table.** With fps static, the choice of
fps *forces* the lowest usable rung, and therefore the maximum range:

| static fps | kbps needed | forced MCS floor | consequence |
|---|---|---|---|
| **30** | 2400 | **rung 0** | full range; MCS0 fallback stays legal |
| **60** | 4800 | **rung 1** | MCS0 excluded |
| **90** | 7200 | **rung 1** (7 % margin) | MCS0 excluded |
| **100** | 8000 | **rung 2** | MCS0 *and* MCS1 excluded |

### Consequence: under static fps, fps *is* the range axis

Range is set by how low the MCS floor goes, and fps sets that floor. So the
two proposed axes collapse onto one variable: `(low latency, high range)` is
not a hard cell, it is an **empty** one — 100 fps cannot reach rung 0 at any
resolution.

The second axis therefore has to become **resolution / FOV** (detail vs
robustness), which *is* genuinely orthogonal to fps. Naming needs an operator
ruling; "range" now lives on the fps axis.

### Consequence: variable-fps mode is not a curiosity

A static-fps mode spans at most `floor(fps) .. 5`. Only the variable-fps mode
can traverse **0–5**, because it can drop to 30 fps at the bottom and climb at
the top. It is the only configuration that uses the full link envelope — at the
cost of being unrecordable. A good thing to offer as *"maximum range, no
recording."*

---

## 3. Measured: sensor mode tables

### IMX335 / star6e — queried live from the craft `.232`, `GET /api/v1/modes`

| idx | capture | aspect | fps range |
|---|---|---|---|
| 0 | 2560×1920 | 4:3 | 3–30 |
| 1 | 2560×1920 | 4:3 | 3–60 |
| 2 | 2560×1440 | 16:9 | 3–**90** |
| 3 | 2176×1224 | 16:9 | 3–**100** |
| 4 | 1920×1080 | 16:9 | 3–120 |
| 5 | 1600×900 | 16:9 | 3–144 |

`min_fps`/`max_fps` is a **range**, not a fixed value — one mode covers every
fps up to its ceiling. That matters for mode selection: pick the widest-FOV
mode whose `max_fps` ≥ the target.

**4:3 has only two modes, both capped at 60 fps** — so 4:3 has no 90/100 fps
row at all. 4:3 is a 2-row matrix, not 3.

### IMX415 / star6e — from `waybeam_venc/drivers/sensor_imx415_star6e.c` (authoritative) and `documentation/STAR6E_IMX415_MODES.md`

| idx | capture | fps | readout | FOV (sensor area) |
|---|---|---|---|---|
| 0 | 3840×2160 | 30 | all-pixel, non-binned | **100 %** |
| 1 | 3840×2160 | 33 | non-binned, full 4K | **100 %** |
| 2 | 2816×1584 | 60 | crop, non-binned | 73 % width |
| 3 | 3840×1152 | 60 | non-binned, full width | 100 % w × 53 % h (ultrawide 3.33:1) |
| 4 | 1920×1080 | **90** | **2×2 binned, FULL FOV** | **100 %** |
| 5 | 2304×1296 | 100 | crop, non-binned (sharp) | 36 % area |
| 6 | 1728×972 | 100 | 2×2 binned, wide crop | 81 % area |
| 7 | 1472×816 | 120 | 2×2 binned crop | 77 % width |
| 8 | 1728×816 | 120 | 2×2 binned wide crop | 90 % w × 76 % h |

**Output resolution ≠ FOV when binning is involved.** A 2×2-binned 1728×972
covers 3456×1944 of sensor area — far more than a non-binned 2304×1296. The
operator's "widest FOV" rule must therefore rank by *sensor area covered*, not
by output pixels. Under that rule idx6 beats idx5 at 100 fps.

HDR/DOL is not exposed on either driver.

---

## 4. Measured: the encoder does NOT clip at low fps

The operator's hypothesis was that low resolutions cannot absorb high bitrates
and that the encoder might under-deliver at lower fps. Measured on the craft,
1280×720, `video0.bitrate = 25000` held constant, `video0.fps` driven live via
`GET /api/v1/live/set?video0.fps=N`, ground-side `frame_count`/`frame_bytes`
deltas over 14 s:

| commanded fps | measured fps | actual kbps | avg frame | bits/pixel/frame |
|---|---|---|---|---|
| 30 | 30.0 | 25558 | 106506 B | **0.925** |
| 60 | 60.0 | 25557 | 53262 B | **0.462** |
| 100 | 100.0 | 25568 | 31969 B | **0.278** |

**The encoder fills 25 Mbps at every fps** — there is no low-fps ceiling and no
720p bitrate cap. So "resolution caps bitrate" is about **diminishing returns**,
not a hard limit, and resolution must not be used to hard-cap the rung band.

The real form of the operator's intuition is the inverse and it is important:
at 30 fps, 720p at 25 Mbps is **0.925 bpp** — visually lossless, i.e. the link
budget is being *wasted* on a small picture. So **low fps should be paired with
high resolution**, and high fps with lower resolution. That is the resolution
axis, and it is now measured rather than assumed.

Still to measure (§8): where the knee actually sits per resolution, i.e. the
bpp beyond which quality stops improving. That needs an A/B at fixed fps across
resolutions, which requires a venc restart per point (`video0.size` is
`restart_required`).

---

## 5. Measured: venc field mutability (`GET /api/v1/capabilities`)

| field | mutability |
|---|---|
| `video0.fps` | **live** |
| `video0.bitrate` | **live** |
| `video0.gop_size` | live |
| `video0.max_i_bytes` / `max_p_bytes` | live |
| `video0.size` | **restart_required** |
| `sensor.mode` | **restart_required** |
| `isp.keep_aspect` | restart_required |

**This validates the architecture.** Resolution and sensor mode are a
**ground / pre-flight** choice; the only in-flight levers are fps, bitrate and
the MCS rung. A mode change mid-flight would mean a venc restart and a video
outage, so the matrix must be selected before launch.

---

## 6. Open: 100 fps vs 90 fps

The operator chose 30/60/100 because all three exist on both drivers. The
measurements argue for 30/60/**90**:

| | 30/60/100 | 30/60/90 |
|---|---|---|
| MCS floors | 0 / 1 / **2** | 0 / 1 / **1** |
| IMX415 top mode | idx6 1728×972, **81 % FOV** | idx4 1920×1080, **100 % FOV** |
| IMX335 modes needed | mode 2 (30/60) + mode 3 (100) | **mode 2 covers all three** |
| IMX335 top FOV | 2176 wide (15 % narrower) | 2560 wide |

Three wins for 90: a whole extra rung of range (rung 1 vs rung 2), full FOV on
IMX415, and — because IMX335 mode 2 spans 3–90 fps — **one sensor mode for the
entire latency axis**, which makes fps changes live rather than restart-gated
on that sensor.

The cost is 10 % latency, and that the range axis no longer differentiates 60
from 90 (both floor at rung 1). Whether that is a loss depends on whether the
operator wants latency and range coupled or decoupled — decoupling was the
original stated goal, so it may be a win too.

**Needs an operator ruling.**

---

## 7. Open: the 10 kB block target is an unvalidated seed

Everything above pivots on `min_p_frame_bytes = 10000`. §9.11 ships it as a §17
RE-DERIVE seed and it never was derived. It is also expressed in **bytes** when
the quantity that matters is **source symbols per FEC block** — 10000 B is
"≈7 packets at ~1400 B MTU", which silently means something different if MTU or
symbol size changes.

The 100 fps → rung 2 result is **marginal**: rung 1 gives 7704 kbps against
8000 needed, 4 % short. A target of 9600 B would put 100 fps on rung 1 and
shift the whole range column. Derive this before casting the matrix in spec.

---

## 8. Remaining exploratory tests (operator-directed)

To be run with **adaptive MCS and bitrate logic disabled**, driving the venc
API directly, so each point is a controlled measurement rather than a
selector-chased one:

1. **Resolution knee per fps.** For each of 30/60/100 fps, sweep encode
   resolution (960×540 → 1280×720 → 1600×900 → 1920×1080) at fixed bitrates
   and find where bpp stops buying visible quality. `video0.size` is
   restart_required, so each point costs a venc restart.
2. **Bitrate floor per (resolution, fps).** The lowest bitrate that still looks
   acceptable — this is what actually sets each cell's usable rung floor,
   independent of the block-size constraint.
3. **Block-size target derivation (§7).** Vary `min_p_frame_bytes` against
   measured `frames_unrecoverable` at a fixed bad-link operating point. This is
   the §17 derivation §9.11 has always owed.
4. **IMX415 confirmation.** Re-run the mode query on an IMX415 craft; the table
   in §3 is read from driver source, not from hardware.
5. **Re-measure the B11 case.** MCS0 at the matrix-selected fps, against the
   3.34 % unrecoverable baseline.

### Bench procedure for these tests

- Pin the rung: `select.min_profile == max_profile` in the craft config (this
  is the §9.7 PINNED branch, which returns before the §9.8 fail-safe).
- `venc.enabled: false` in the craft config to stop waybeam-link owning
  `video0.bitrate`, then drive bitrate and fps directly:
  `GET http://127.0.0.1/api/v1/live/set?video0.bitrate=N` and `...?video0.fps=N`.
- Resolution/mode changes: `POST /api/v1/set` + venc restart (`S95waybeam`).
- Read delivered rate ground-side from `frame_count` / `frame_bytes` deltas —
  do not trust `frame_size_last` alone, and note `frame_size_max` is a
  since-start maximum that survives config changes.

---

## 9. Where the current bench sits

- Craft `.232`: IMX335, `sensor.mode 3` (2176×1224@100), encode 1280×720,
  100 fps, bitrate commanded 25000 by waybeam-link (`venc.enabled: true`),
  adaptive MCS1–5, `venc.max_bitrate_kbps 25000`. Running
  `/usr/bin/waybeam-link` under `S96waybeam-link`.
- `venc.fps_ladder` is **absent** from the craft config, so the §9.11 ladder is
  disabled — MCS0 currently has no mitigation at all.
- Fleet on 5805, MCS5, video and audio flowing.

Note: after the craft reboot on 2026-07-24 venc came back from disk at **30
fps**, not the 100 fps its running config had. `video0.fps` on disk and the
running value can diverge; check both before trusting a measurement.

---

## 10. The §9.11 ladder, as it stands today

Relevant because the variable-fps mode will be built on it, and because it was
reviewed in depth on 2026-07-24. Findings (all from driving the real
`FpsLadder` — simulators kept out-of-tree in the session scratchpad):

**Sound:** swept every derived bitrate 2000–26000 kbps over 120 s — max 5 fps
changes, i.e. the one-time descent, **no flapping anywhere**, and none under
±25 % frame-size noise either. Evidence comes from encoder output at frame-SHM
ingress, so RF loss does not blind it.

**Broken:**

1. **The shipped `min: 60` seed cannot fix MCS0.** Rung 0 needs ≤45 fps; at
   min 60 the ladder floors at 60 fps / 7925 B — 21 % short, state `FLOOR`.
2. **The reduce timer needs 3000 ms of *continuous* sub-threshold evidence and
   anything resets it to zero.** Two measured starvation modes:
   - *Actuator settling*: with a 1000 ms settle window the ladder never acts
     unless the gap between settles exceeds **3000 ms**. The §9.5 bitrate
     actuator fires on every rung change and `down_cooldown_s` is 0.2 s, so a
     fading link can starve it indefinitely — it is starved exactly when it is
     most needed.
   - *Encoder noise*: at 1.25 % below target, **±5 % noise starves it
     forever**; at 20 % below it is robust to ±25 %. There is a dead band
     immediately under the threshold, and MCS1 at 100 fps (9630 B, 3.7 % under)
     sits in it.
3. **Descent takes 18 s** (4 steps × 4500 ms) while the fail-safe drop is
   instantaneous. §9.11 defers "emergency reduction (bypassing dwell)... until
   bench data motivates it" — this is that data.
4. **`preferred` is a static config value, not venc's sensor mode.** On start
   `tick()` commands `preferred` unconditionally; if the sensor cannot do it the
   ladder believes a wrong `current_` and every `predicted_up` is computed from
   a wrong base. No read-back, though venc exposes `/api/v1/get`.
5. **§11.7 FPS_SELECT is rejected while the ladder runs**
   (`app/main.cpp:2118`). Any "mandatory ladder" decision kills that command
   unless FPS_SELECT is changed to re-anchor the ceiling instead.

**The architectural fix identified:** make fps **derived, not discovered**. The
rung's bitrate is already known, so the correct fps is closed-form —
`target_fps = highest ladder member f where derived_bitrate_bytes_per_s / f ≥
block_target` — with the measured EWMA demoted to a correction for when the
encoder does not fill its budget. That removes the 18 s descent, both
starvation modes and the noise dead band at once, and multi-rung skipping falls
out for free.

Asymmetry the operator asked for: **down immediately** (multi-rung, no dwell —
the answer is already known), **up on a ~1500 ms per-step dwell** giving
30 → 100 in about 7.5 s, with multi-rung jumps allowed when the bitrate climbs
several rungs at once.

**Honest limit:** feed-forward assumes the encoder fills its bitrate. On a
static scene it will not, and then frames are small at *any* fps, so no cadence
reduction produces a big block. Below that point the right lever is FEC parity,
not fps.
