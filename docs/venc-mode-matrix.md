# The operating-mode matrix — user-facing modes over §9 rungs

**Status: LANDED (Pass 94 + Pass 95), model not yet flown.** The two defects
this work uncovered are fixed in spec and code on this branch, and the nine
operating modes are in `profiles/modes/`. What has *not* happened is a flight
or a bench soak of the modes themselves, and the 0.04 bpp floor they are built
on is an educated guess (§16.1), not a measurement.

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

## 1a. RETRACTED by measurement, 2026-07-24 — read §11–§15 first

Sections 2, 6 and 7 below were written before the block target was derived.
The derivation (§11) shows the premise was wrong, so the conclusions built on
it do not hold. They are kept for provenance, marked, and superseded:

- **§2's "static fps forces the MCS floor" is RETRACTED.** It assumed a
  ~10 kB minimum frame. There is no such requirement once §14.1's `min_k`
  gate is fixed (§11). fps is *not* the range axis.
- **§7's "derive the 10 kB block target" is ANSWERED, and the answer is that
  there is no useful block target** (§11.4).
- **§6's 100-vs-90 fps trade is unchanged in its FOV/sensor-mode arguments but
  loses its "whole extra rung of range" argument**, which came from §2.

What replaces them: the binding constraint is **bits per pixel**, not block
size (§14). Three separate defects were found on the way (§11, §12, §13).

**The structure that replaces §2's is in §16.0** — two axes in (latency, range),
resolution derived. §2's collapse of the two axes into one does not survive,
because the fps↔range coupling is now soft (costs pixels) rather than hard
(forbids the cell).

---

## 2. ~~Measured: the per-rung block-size ceiling~~ (RETRACTED — see §1a)

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

**The restart strands the live bitrate — the applier re-asserts it (§15.5 Pass
103).** `video0.bitrate` is *live* (volatile `/api/v1/live/set`), so the restart
that applies `video0.size`/`sensor.mode` also **discards** the bitrate/caps/fps
the link had pushed; the fresh encoder boots at its persisted config value. The
link's §9.6 write-on-change actuator believes the encoder still holds the live
value and would not re-push — worst on a **same-band switch** (only fps/resolution
change, so the pin and derived bitrate are unchanged). `apply-mode.sh`'s final
step POSTs `/api/v1/venc/reassert`, dropping that cache so the link re-asserts on
its next tick. **The one supported entry is `POST /api/v1/mode`** (the hub), which
forks the applier; running `apply-mode.sh` by hand is bench-only (it self-reasserts
either way). Verified end-to-end by `tools/mode_harness.py`.

**Enumerating the catalog — `GET /api/v1/modes` (§15.5 Pass 104).** A menu (the
hub) reads the selectable modes from the **link**, not from a copy of its own:
the link enumerates `venc.modes_dir` (defaulting to the directory holding
`mode_apply_cmd`) and returns `{active, apply_configured, modes:[{name, fps,
resolution, mcs_min, mcs_max, fps_mode}]}` — the raw latency/range/resolution
facts of every mode file, name-sorted, so the caller lays out the grid and picks
its own "High/Med/Low" labels. This keeps the mode list single-sourced on the
craft. NB: this is the **link** control plane on `:8091`; majestic's own `GET
/api/v1/modes` on `:80` (§4 above) is the *sensor*-mode list — same path, a
different service and a different meaning.

**Selecting a mode from the ground — §11.7 `0x07` MODE (Pass 105).** A local
`POST /api/v1/mode` only reaches a craft on the operator's own LAN; from the
*ground* the selection must ride the PSK-guarded RF uplink like every other
vehicle command — an unauthenticated HTTP POST to the craft's network-facing hub
would be a mode-change hole. So MODE joins the §11.7 registry: the ground reads
the catalog over management HTTP (above), picks an index into the name-sorted
list, and issues `POST /api/v1/vehicle/command {cmd:"mode", arg:<index>}`; the
craft resolves the index against the *same* enumeration `GET /api/v1/modes` is
built from and forks the §16 applier — the identical path `POST /api/v1/mode`
takes, so the Pass 103 self-reassert heals the restart either way. MODE is the
one §11.7 command with a full-`u8` arg (§3.14) because the catalog is open-ended;
it is craft-session volatile (reboot → boot mode) and pre-flight (venc restart).

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

---

## 11. DERIVED (§17): the block target — and why there isn't one

The §17 derivation §9.11 has always owed. Run two ways that agree:

- **Offline**, driving the real `FrameFramer` (§5.1a) and `FrameReassembler`
  (§6.3a) over Bernoulli packet loss — the same `ceil()` `repair_count`, the
  same `min_k` gate, the same GF(256) decode the craft runs. Not a model.
- **On hardware**, craft `.232` pinned MCS5 with `venc.enabled: false`, ground
  `air.rx_drop_permille: 141` per adapter giving a measured **2.1–2.3 %
  effective post-diversity loss**, venc at 98.7 fps / 1280×720, bitrate swept
  to walk k from 2 to 15.

### 11.1 Root cause of B11: `min_k` removes ALL protection, not just FEC

`core/src/frame_framer.cpp:30`

```cpp
if (k <= cfg_.fec.min_k) {
    return 0;  // ARQ-only at small k
}
```

The craft runs `min_k: 3` **and** `arq_mode: "idr-only"`. So every P-frame of
≤ 3 symbols (≤ 3 × 1387 = **4161 B**) is emitted with **no FEC and no ARQ**.

§14.1's table states the rationale explicitly:

> `k ≤ fec.min_k` (seed 3) | `r = 0` (ARQ-only) | at k=3 one repair = 33 %
> overhead; **NACK→RETRANSMIT recovers within deadline** (§17 gate 3).

That justification is conditioned on an ARQ fallback which P-frames do not
have under `arq_mode: idr-only` — which is both the craft's deployed setting
and the §4.1 default for the P class. The rule is unconditional; its
justification is not.

This is B11. At MCS0 the derived bitrate divided by the frame rate lands
frames squarely under 4161 B, and they go out bare.

### 11.2 Offline: the cliff, and the sawtooth above it

Unrecoverable-frame rate vs frame size, `min_k: 3`, `p_rate: 200‰`,
`arq_mode: idr-only`, s = 1387 B, N = 20000 frames per point:

| bytes | k | r | p=0.5 % | p=1 % | p=2 % | p=5 % | p=10 % |
|---|---|---|---|---|---|---|---|
| 1500–2500 | 2 | 0 | 0.900 % | 1.890 % | 3.880 % | 9.595 % | 18.830 % |
| 3000–4000 | 3 | 0 | 1.380 % | 2.800 % | **5.785 %** | 13.975 % | 26.905 % |
| 4200–5000 | 4 | 1 | 0.025 % | 0.115 % | **0.340 %** | 2.070 % | 7.850 % |
| 5600 | 5 | 1 | 0.040 % | 0.160 % | 0.565 % | 3.120 % | 10.955 % |
| 7000 | 6 | 2 | 0.000 % | 0.005 % | 0.055 % | 0.495 % | 3.830 % |
| 12000 | 9 | 2 | 0.000 % | 0.000 % | 0.100 % | 1.405 % | 8.740 % |
| 30000 | 22 | 5 | 0.000 % | 0.000 % | 0.000 % | 0.185 % | 4.695 % |

Two features:

1. **A 17× cliff at exactly one byte.** 4161 B → 4162 B takes p=2 % loss from
   5.785 % to 0.340 %. That is the `min_k` gate, nothing else.
2. **Above the cliff the curve is a decreasing sawtooth, not monotone.** k=5
   (0.565 %) is worse than k=4 (0.340 %) because `ceil(k × 0.2)` gives both
   r=1 while k=5 has one more symbol to lose. Local maxima recur at k=5, 10,
   15. So "bigger blocks are safer" is only true on average.

### 11.3 Hardware: `min_k` 3 → 1, A/B on the live link

`min_k` is retunable live via `POST /api/v1/fec`, so each bitrate point was
measured twice back-to-back on the same link. Loss is ground-side
`frames_unrecoverable / (frames_unrecoverable + delivered)` — single-source,
no cross-host window skew (see §11.5). 35 s per arm.

| cmd kbps | B/frame | k | min_k=3 loss | fec_rec | min_k=1 loss | fec_rec |
|---|---|---|---|---|---|---|
| 1600 | 2065 | 2 | 0.463 % | 30 | **0.000 %** | 77 |
| 2200 | 2834 | 3 | 0.808 % | 69 | **0.172 %** | 182 |
| 2800 | 3579 | 3 | 0.228 % | 116 | **0.143 %** | 226 |
| 3400 | 4363 | 4 | 0.345 % | 190 | **0.029 %** | 277 |
| 3900 | 4994 | 4 | 0.516 % | 189 | **0.428 %** | 284 |
| 5000 | 6434 | 5 | 0.656 % | 290 | **0.171 %** | 371 |
| 6100 | 7849 | 6 | 0.457 % | 376 | **0.171 %** | 474 |
| 8300 | 10649 | 8 | 0.144 % | 629 | **0.029 %** | 604 |
| 10500 | 13444 | 10 | **0.000 %** | 731 | 0.057 % | 844 |
| 16000 | 20484 | 15 | 0.314 % | 1145 | **0.029 %** | 1151 |

**Mean 0.393 % → 0.123 %, a 3.2× reduction. Better at 9 of 10 points; the one
exception is 0.000 % vs 0.057 %, both at the noise floor.** Worst point
0.808 % → 0.428 %.

Note the improvement persists at k ≥ 4, where `min_k: 3` should already be
applying parity. That is because **B/frame is a mean and the operative
quantity is the distribution**: real P-frames vary enough that a tail of every
operating point falls under the gate. Re-running the offline sweep with a
realistic spread (cv = 0.6) reproduces the ratio structure — 4.7× at k=2
decaying to 2.0× at k=12 — against the hardware's 3.2× mean.

### 11.4 The answer: there is no useful block target

With the gate at `min_k: 1`, every measured frame size from 2065 B upward sits
at or below 0.43 % unrecoverable at 2.3 % packet loss, with no cliff and no
size below which protection collapses. **`min_p_frame_bytes = 10000` is not a
requirement the FEC layer imposes.** §9.11's §17 RE-DERIVE seed can be retired
rather than re-derived.

The consequence for this document is structural: §2 derived the whole range
axis from a 10 kB minimum frame. With that gone, **fps does not force an MCS
floor**, `(low latency, high range)` is not an empty cell, and the two
user-facing axes do not collapse. What actually binds is §14.

### 11.5 Measurement-validity finding: `frames_unrecoverable` has a blind spot

Establishing which RX counter to trust, offline, at p=2 %:

| bytes | k | true loss | `frames_unrecoverable` | `frames_superseded` |
|---|---|---|---|---|
| 1000 | 1 | 1.930 % | **0.000 %** | 0.000 % |
| 2000 | 2 | 3.880 % | 3.840 % | 3.840 % |
| 3000 | 3 | 5.785 % | 5.780 % | 5.780 % |
| 5600 | 5 | 0.565 % | 0.565 % | 0.565 % |
| 20000 | 15 | 0.020 % | 0.020 % | 0.020 % |

- **Exact for k ≥ 2** — safe to use, and the B11 3.34 % figure stands.
- **Blind at k = 1**: a frame whose only symbol is lost never opens a block, so
  the reassembler never learns it existed. Reported 0.000 % against a true
  1.930 %.
- **`frames_superseded` is a duplicate of the same event**, not an independent
  outcome. Summing the two double-counts.

### 11.6 PROPOSED RULING (needs operator sign-off)

The principled fix follows §14.1's own stated rationale: **the `k ≤ min_k`
ARQ-only branch must be conditioned on the frame actually being ARQ-eligible.**

```cpp
// §14.1: r = 0 at small k is justified only where ARQ can recover the frame
// (§17 gate 3). Under arq_mode idr-only a P-frame has no ARQ, so the branch
// would leave it with no protection at all.
if (k <= cfg_.fec.min_k && frame_has_arq) {
    return 0;
}
```

Alternative, weaker, config-only: set `min_k: 1` on the craft. Measured 3.2×
and needs no code, but leaves the spec's unconditional rule in place for any
other deployment.

---

## 12. Defect: parity airtime is not budgeted (§9.3 / §9.5)

`core/src/selector.cpp:31` debits `fec_overhead_permille` from the derived
bitrate. **`profiles/table.example.json` ships `fec_overhead_frac: 0.0` on all
eight rungs**, while `craft.json` runs `"scheme": "rlc256"` at 200 ‰ P / 300 ‰
IDR. So §9.5 derives the video bitrate as if there were no parity, and the
framer then adds parity on top.

Measured live on the craft at MCS5: 183 672 repair / 847 295 source symbols =
**21.7 % by symbol count, 22.2 % by wire bytes**. The hardware sweep saw
8.8–29.1 % depending on k. As a fraction of *capacity* — which is what
§9.5's `× (1 − fec_overhead_frac)` expects — that is **≈ 180 ‰**.

`docs/findings-pass3.md:286` called this exactly: *"If FEC is adopted it must
be budgeted, not bolted on."* FEC was adopted; it was bolted on.

Current §9.5 derived bitrate (25000 kbps `venc.max_bitrate_kbps` ceiling):

| rung | MCS | GI | table before FEC budget | Pass 95 FEC budget | **Pass 111 calibrated** |
|---|---|---|---:|---:|---:|
| 0 | 0 | long | 3804 | 2829 (250‰ FEC) | **2829** (600‰ airtime) |
| 1 | 1 | long | 7704 | 5754 (250‰ FEC) | **5754** (600‰ airtime) |
| 2 | 2 | short | 12903 | 10303 (200‰ FEC) | **10303** (600‰ airtime) |
| 3 | 3 | short | 17236 | 13769 (200‰ FEC) | **13769** (600‰ airtime) |
| 4 | 4 | short | 25000 (clamped) | 21223 (180‰ FEC) | **18025** (510‰ airtime) |
| 5 | 5 | short | 25000 (clamped) | 25000 (clamped) | **21839** (463‰ airtime) |

Read back from the shipped table via `derive_bitrate_kbps()`, not estimated.
Pass 95 corrected parity accounting; Pass 111 retains its conservative lower
rungs and lowers MCS4–5 to 95% of their measured clean local-service boundary.
The independent `venc.max_bitrate_kbps` encoder ceiling no longer binds in this
six-rung mode catalog.

**PROPOSED RULING (needs operator sign-off):** `fec_overhead_frac` must be
non-zero on any rung whose stream runs `fec_scheme: rlc256`. Whether it is
authored per-rung in the §9.3 table or derived at runtime from the configured
`p_rate`/`i_rate` is the open design choice — a static table value cannot
track the `ceil()` inflation at small k, which the sweep measured at up to
29 %.

---

## 13. Bench traps found today (each cost real time)

- **venc's live fps actuation silently no-ops after a craft reboot.**
  `GET /api/v1/live/set?video0.fps=100` returns `{"ok":true}`,
  `GET /api/v1/get?video0.fps` echoes 100, and the sensor keeps delivering
  **30**. Bouncing through another value does not fix it; nor does restarting
  venc (`S95waybeam`). It needs a **sensor mode reinit** (operator). Since the
  whole matrix treats fps as the one live in-flight lever, *always verify fps
  by measuring the delivered frame rate ground-side*, never by reading it back.
- **The encoder is scene-limited, so delivered ≠ commanded.** At 30 fps /
  1280×720 with `video0.bitrate = 25000` the craft delivered **7657 kbps**.
  §4's "the encoder fills 25 Mbps at every fps" holds only for a scene that
  demands it; on a static bench it does not. Quote commanded bitrate and
  measure delivered separately.
- **Cross-host counter deltas are useless at this precision.** Comparing craft
  `frame_count` against ground `frame_count` over an ssh hop gave ±2 % window
  skew — larger than the effect — and produced negative loss rates. Use
  single-source ground-side counters (§11.5).
- **The cache repair path is inert.** With `caches_configured: 1` and
  `caches_following: 1`, `arq_recovered_source_symbols` and
  `arq_recovered_repair_symbols` were both **0** for the whole session, so it
  did not contaminate these measurements. Worth its own look: at 100 fps the
  §14.3 `hard_close_ms: 8` budget is shorter than a frame interval.
- **Encode resolution is independent of sensor mode.** The craft runs sensor
  mode 3 (2176×1224, 100 fps) encoding to 1280×720 — the ISP downscales. So
  FOV and encode detail are **separate knobs**, and "widest FOV" does not
  force a large encode. This considerably simplifies the matrix.

---

## 14. What actually binds: bits per pixel

With the block-size floor gone (§11.4), the constraint on a (rung, fps,
resolution) cell is whether the resulting bpp is watchable. From the corrected
§12 bitrates:

**1280×720 (921 600 px)**

| rung | kbps | 30 fps | 60 fps | 90 fps | 100 fps |
|---|---|---|---|---|---|
| 0 | 2829 | 0.102 | 0.051 | 0.034 | 0.031 |
| 1 | 5754 | 0.208 | 0.104 | 0.069 | 0.062 |
| 2 | 10303 | 0.373 | 0.186 | 0.124 | 0.112 |
| 3 | 13769 | 0.498 | 0.249 | 0.166 | 0.149 |
| 4 | 18025 | 0.652 | 0.326 | 0.217 | 0.196 |
| 5 | 21839 | 0.790 | 0.395 | 0.263 | 0.237 |

**1920×1080 (2 073 600 px)**

| rung | kbps | 30 fps | 60 fps | 90 fps | 100 fps |
|---|---|---|---|---|---|
| 0 | 2829 | 0.045 | 0.023 | 0.015 | 0.014 |
| 1 | 5754 | 0.092 | 0.046 | 0.031 | 0.028 |
| 2 | 10303 | 0.166 | 0.083 | 0.055 | 0.050 |
| 3 | 13769 | 0.221 | 0.111 | 0.074 | 0.066 |
| 4 | 18025 | 0.290 | 0.145 | 0.097 | 0.087 |
| 5 | 21839 | 0.351 | 0.176 | 0.117 | 0.105 |

**960×540 (518 400 px)**

| rung | kbps | 30 fps | 60 fps | 90 fps | 100 fps |
|---|---|---|---|---|---|
| 0 | 2829 | 0.182 | 0.091 | 0.061 | 0.055 |
| 1 | 5754 | 0.370 | 0.185 | 0.123 | 0.111 |
| 2 | 10303 | 0.662 | 0.331 | 0.221 | 0.199 |
| 3 | 13769 | 0.885 | 0.443 | 0.295 | 0.266 |
| 4 | 18025 | 1.159 | 0.580 | 0.386 | 0.348 |
| 5 | 21839 | 1.404 | 0.702 | 0.468 | 0.421 |

This is the matrix's real shape, and it reproduces the operator's intuition
correctly — but inverted from §4's first reading. It is not that low
resolutions cannot absorb high bitrates; it is that **a cell is viable when
bpp clears a floor**, so at a low rung you must spend the budget on fewer
pixels or fewer frames.

**The one number still missing is that bpp floor**, and it is irreducibly
subjective — it needs the operator's eyes, not a counter. §15 sets up that
test.

---

## 15. Next: the bpp floor (operator judgement required)

Everything above is objective and settled. The matrix cannot be finalised
without one ruling that no instrument can supply: **the lowest bits/pixel that
still looks acceptable in flight**, and whether that floor differs for a
"low latency" user (who tolerates softness for cadence) versus a "high
quality" user.

Proposed procedure, on the controlled bench already built:

1. Craft pinned, `venc.enabled: false`, no synthetic loss.
2. For each of 960×540 / 1280×720 / 1920×1080 (`video0.size` is
   `restart_required` — one venc restart per resolution), hold fps at 30, 60
   and 100 and step `video0.bitrate` down until the operator calls it.
3. Record the bpp at the call, not the bitrate — bpp is what transfers across
   resolutions.

That yields one floor (or three, if it turns out to be latency-dependent), and
the 3×3 matrix falls out of the §14 tables directly: for each cell, pick the
largest resolution whose bpp at that cell's lowest allowed rung clears the
floor, and the widest-FOV sensor mode that supports the cell's fps.

### Still open from earlier sections

- **100 vs 90 fps** (§6) — the FOV and sensor-mode arguments stand; the "extra
  rung of range" argument is withdrawn with §2.
- **IMX415 hardware confirmation** (§8 item 4) — the §3 table is still read
  from driver source, not from hardware.
- **§9.11 ladder** (§10) — the four defects there are unaffected by today's
  work, but the *motivation* changes: with no block-size floor, the ladder is
  no longer needed for protection. It becomes purely a quality lever, and the
  variable-fps mode is its only remaining hard requirement.

---

## 16. THE MATRIX (educated guess, 2026-07-24)

Operator asked for a call rather than another measurement round. Here it is.
Everything below is arithmetic on §12's corrected rung bitrates plus one
judgement call.

### 16.0 The load-bearing structure — read this before the tables

Two axes in, one quantity derived:

```
  latency  ──┐
             ├──>  resolution / FOV   (the derived variable)
  range    ──┘
```

**fps and MCS band are the inputs. Resolution is the output.** Resolution is
not a third user choice and not a second axis — it is where the tension
between the two axes goes.

The mechanism, stated once:

> A cell must clear a **bpp floor at its lowest allowed rung.**
> `bpp = kbps / (fps × pixels)`. Range fixes the numerator (the floor rung's
> bitrate), latency fixes one factor of the denominator (fps), so the only
> free variable left is **pixels**.

That is the whole design. Everything in §16.3–§16.5 is this identity
evaluated.

**How this differs from §2 (retracted).** §2 had fps coupled to range
*hard* — a 10 kB minimum block meant 100 fps could not reach rung 0 **at any
resolution**, so `(low latency, high range)` was an **empty cell** and the two
axes collapsed into one. §2 then concluded the second axis "has to become
resolution/FOV".

With the block floor gone (§11.4) that coupling is **soft**: fps still trades
against range, but it now trades *through resolution* rather than forbidding
the combination. So:

- **No cell is empty.** `(low latency, high range)` exists — it is 960×540.
  It costs pixels, not viability.
- **Latency and range are genuinely independent inputs again**, which is what
  the operator asked for in the first place.
- **Resolution/FOV is therefore not an axis.** Promoting it to one would
  over-determine the system: pick all three and you are specifying bpp
  directly, which is the number the controller exists to manage.

The soft coupling is still visible and still load-bearing — read any row of
§16.3 left to right, or any column top to bottom, and resolution moves. That
movement *is* the trade. It is just no longer allowed to reach zero.

### 16.1 The judgement call

**Floor: 0.04 bpp at the cell's lowest allowed rung. Nominal: ≥ 0.10 bpp at
its top rung.**

The lowest rung is the *emergency fallback*, so the right target there is
"degraded but flyable", not "good" — H.265 at ~0.04 bpp on high-motion FPV is
soft and blocks up on motion but holds shape and horizon. For calibration:
HDZero runs ~0.075–0.15 bpp at 720p90, DJI O3 ~0.20–0.32 at 1080p60. 0.04 is
deliberately below both, because it is a floor and not a design point.

This is the one number in this document that was guessed rather than measured.
If it is wrong, every cell moves together and the structure survives — rerun
§15 and re-derive.

### 16.2 Axes

| axis | user sees | derives |
|---|---|---|
| **Latency** Low / Medium / High | responsiveness | fps **100 / 60 / 30** |
| **Range** High / Medium / Low | how far before it degrades | MCS band **0–2 / 1–4 / 2–5** |
| **Aspect** 16:9 / 4:3 | picture shape | which sensor modes are eligible |

Range sets the *lowest* rung the link may fall to, which is what actually
determines usable distance. The band's top rung is the nominal operating point.

### 16.3 16:9 — the primary matrix

Cells show encode resolution and the bpp range from the band's lowest rung to
its top rung.

| | **Range High** (MCS 0–2) | **Range Medium** (MCS 1–4) | **Range Low** (MCS 2–5) |
|---|---|---|---|
| **Latency Low** (100 fps) | 960×540 · 0.055→0.199 | 1280×720 · 0.062→0.196 | 1920×1080 · 0.050→0.105 |
| **Latency Medium** (60 fps) | 1280×720 · 0.051→0.186 | 1920×1080 · 0.046→0.145 | 1920×1080 · 0.083→0.176 |
| **Latency High** (30 fps) | 1920×1080 · 0.046→0.166 | 1920×1080 · 0.093→0.290 | 1920×1080 · 0.166→0.351 |

Rung bitrates are §9.5-derived from the **Pass 111 table** (`table_version`
0xBF) — 2829 / 5754 / 10303 / 13769 / 18025 / 21839 kbps. Pass 95's graduated
FEC overhead remains in force; Pass 111 adds the measured local-service ceiling
for MCS4–5. The floor rungs that choose each resolution are unchanged, so no
mode changes size. **(Latency Low, Range Medium)** remains the knife edge:
1600×900 lands at 0.0400 bpp, under the floor by 0.1 %. If the guessed 0.04
floor moves up at all, the cell goes back to 1600×900.

Every cell clears the floor at its worst rung and 0.12+ at its best. The
diagonal reads exactly as intended: trading latency for range costs pixels,
and 1080p is reachable from every range band if you accept 30 fps.

### 16.4 4:3 — two rows, not three

The IMX335 has no 4:3 mode above 60 fps, so **4:3 has no Latency-Low row**.

| | **Range High** | **Range Medium** | **Range Low** |
|---|---|---|---|
| **Latency Medium** (60 fps) | 1280×960 · 0.042→0.143 | 1440×1080 · 0.068→0.227 | 1440×1080 · 0.113→0.268 |
| **Latency High** (30 fps) | 1440×1080 · 0.066→0.226 | 1440×1080 · 0.135→0.455 | 1440×1080 · 0.226→0.536 |

`1280×960 · 0.042` is the tightest cell in the design.

### 16.5 Sensor mode — widest FOV that supports the row's fps

Independent of encode resolution (§13: the ISP downscales), so this is chosen
per *row*, not per cell.

**Scope (operator, 2026-07-24): IMX335 only, sensor modes 0 / 1 / 3.** Aspect
ratio comes from cropping / re-encoding rather than from separate sensor modes,
and is out of scope for now. IMX415 is deferred.

| fps | `sensor.mode` | capture | FOV |
|---|---|---|---|
| 30 | **0** | 2560×1920 (4:3) | full sensor readout |
| 60 | **1** | 2560×1920 (4:3) | full sensor readout |
| 100 | **3** | 2176×1224 (16:9) | natively 16:9, already cropped at the sensor |

Taking modes 0/1 rather than mode 2 (2560×1440) for the 30/60 rows buys 480
extra rows of sensor readout — the full 4:3 frame — at no cost, since encode
resolution is independent anyway.

**The resulting asymmetry:** 30 and 60 fps read the whole sensor, so a 16:9
encode there is a vertical crop and 4:3 output is free. At 100 fps the only
available mode is already 16:9. So there is genuinely *more FOV available* at
30/60 than at 100 — one more thing the latency axis buys, beyond cadence.

The 90-vs-100 question (§6) is unaffected by this choice and still open.

### 16.6 The tenth mode: variable-fps

One extra mode outside the matrix, explicitly **not record-friendly**:
1280×720, fps free to float 30–100, MCS band 0–5. It is the only configuration
that spans the full link envelope, because it can trade cadence for rung all
the way down. Offer it as *"maximum range, no recording."*

**How it is wired (Pass 99).** fps behaviour is a mode field:
`link.policy.fps_mode` ∈ `{"static","variable"}` (default `"static"`). The nine
matrix modes are `"static"` — the §9.11 ladder held off, `video0.fps` pinned by
the mode; `imx335-variable` is `"variable"` — the ladder run on, `video0.fps`
free to float 30–100. `apply-mode.sh` reads the field and toggles the ladder
through the craft-local `POST /api/v1/link/fps {"ladder": bool}` endpoint (Pass
99, §15.5), which routes through the same §11.7 `FPS_LADDER` transition the
ground uses — **no link restart, no CSA re-pair**. The ladder span
(`min 30`/`preferred 100`) is a per-craft constant in `craft.json`
(`venc.fps_ladder`); the ladder object is now instantiated on every venc craft
(construct ≠ run, §9.11), so the toggle works both ways at runtime. For a reboot
to reproduce the mode, `apply-mode.sh` also persists `venc.fps_ladder.enabled`
as the boot run-state.

**Why "not record-friendly" is a hard property, not a caution.** The mode is
VFR: a moving fps breaks CFR muxers/recorders, and — because these modes run
`resilience=range` (GDR intra-refresh) — the refresh period is frame-indexed, so
a changing fps stretches/compresses the refresh interval. Live-view only. A
time-based (not frame-indexed) refresh for the variable mode is a follow-up, not
a v1 requirement.

### 16.7 What this depends on

- §11's `min_k` fix. Without it the Range-High column is unsafe at its floor
  rung, which is exactly the B11 failure.
- §12's `fec_overhead_frac` fix. Without it every bitrate here is ~18 % too
  optimistic and the floor cells do not actually clear 0.04.

**Both landed in this PR** as **Pass 94** and **Pass 95** (operator, 2026-07-24:
*"in the scope of this PR i would say they land and we try out the model"*).
See `docs/review-log.md`. Pass 95 moved `table_version` **0x41 → 0xD1**;
Pass 111 moves it **0xD1 → 0xBF**. Each semantic table change requires a
lockstep craft-and-ground redeploy.

The nine profiles are in `profiles/modes/` — one JSON per cell, applied by
setting the fields and restarting, per the operator's chosen mechanism.
