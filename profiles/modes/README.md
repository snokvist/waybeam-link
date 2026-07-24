# Operating modes — the user-facing 3×3 matrix

Data, not code. One file per cell of `docs/venc-mode-matrix.md` §16.3, for the
**IMX335** only (IMX415 is deferred).

The user picks two things and never sees anything in here:

| axis | user sees | this directory sets |
|---|---|---|
| **Latency** Low / Medium / High | responsiveness | `sensor.mode` + `video0.fps` — 100 / 60 / 30 |
| **Range** High / Medium / Low | how far before it degrades | `policy.select.min_profile`/`max_profile` — MCS 0–2 / 1–4 / 2–5 |

**Encode resolution is derived, not chosen.** A cell must clear a **0.04 bpp
floor at the lowest rung its range band allows**; `bpp = kbps / (fps × pixels)`,
range fixes the numerator and latency fixes one factor of the denominator, so
pixels is the only free variable. Every `video0.size` here is that identity
evaluated against the §12-corrected rung bitrates. See §16.0 for why resolution
is not a third axis.

The 0.04 floor is the one **guessed** number in the design. If it moves, every
cell moves together and the structure survives.

## The matrix

| | Range High (MCS 0–2) | Range Medium (MCS 1–4) | Range Low (MCS 2–5) |
|---|---|---|---|
| **Low latency** 100 fps | 960×540 · 0.060→0.204 | 1600×900 · 0.044→0.147 | 1920×1080 · 0.051→0.121 |
| **Medium latency** 60 fps | 1280×720 · 0.056→0.191 | 1920×1080 · 0.051→0.171 | 1920×1080 · 0.085→0.201 |
| **High latency** 30 fps | 1920×1080 · 0.050→0.170 | 1920×1080 · 0.101→0.341 | 1920×1080 · 0.170→0.402 |

bpp is quoted worst rung → best rung. Every cell clears 0.04 at its floor and
0.12+ at its top.

## Sensor modes

Chosen per **row**, not per cell — the ISP downscales, so encode resolution
costs sharpness and never FOV.

| fps | `sensor.mode` | capture | note |
|---|---|---|---|
| 30 | 0 | 2560×1920 | full 4:3 sensor readout — widest FOV available |
| 60 | 1 | 2560×1920 | full 4:3 sensor readout — widest FOV available |
| 100 | 3 | 2176×1224 | natively 16:9, already cropped at the sensor |

**Asymmetry worth knowing:** 30 and 60 fps read the whole 4:3 sensor, so a 16:9
encode there is a vertical crop and 4:3 output is free. At 100 fps the only
mode is already 16:9, so there is more FOV available at 30/60 than at 100.
Aspect handling beyond this (crop / re-encode) is out of scope for now.

## Applying a mode

`sensor.mode` and `video0.size` are `restart_required`, so a mode is a
**pre-flight** choice, not an in-flight one. Only `video0.fps` and
`video0.bitrate` are live, and the link owns bitrate.

1. Push the `venc` fields to waybeam_venc, restart it (`S95waybeam`).
2. Merge the `link` fields into `/etc/waybeam-link/craft.json`, restart
   (`S96waybeam-link`).
3. **Re-scout + quickconnect.** A craft restart leaves CSA silently dead
   otherwise (issue B9).

Verify `video0.fps` by measuring the delivered frame rate ground-side, never by
reading it back: after a craft reboot venc's live fps actuation silently
no-ops — the API returns `ok`, `/get` echoes the commanded value, and the
sensor keeps running at its old rate until a sensor-mode reinit
(`docs/venc-mode-matrix.md` §13).

## Dependencies

These bitrates assume **Pass 94** (§14.1 `min_k` gate conditioned on ARQ
eligibility) and **Pass 95** (§9.5 `fec_overhead_frac` non-zero). Without
Pass 94 the Range-High column is unsafe at its floor rung — that is exactly
the B11 failure. Without Pass 95 every bitrate here is ~18 % optimistic and
the floor cells do not actually clear 0.04.

## Not covered yet

- **The variable-fps mode** — 1280×720, fps floating 30–100, MCS 0–5,
  explicitly not record-friendly. The only configuration that spans the full
  link envelope. Needs the §9.11 ladder rework first (§10).
- **IMX415**, and aspect-ratio handling.
