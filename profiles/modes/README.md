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
| **Low latency** 100 fps | 960×540 · 0.055→0.199 | 1280×720 · 0.062→0.196 | 1920×1080 · 0.050→0.105 |
| **Medium latency** 60 fps | 1280×720 · 0.051→0.186 | 1920×1080 · 0.046→0.145 | 1920×1080 · 0.083→0.176 |
| **High latency** 30 fps | 1920×1080 · 0.046→0.166 | 1920×1080 · 0.093→0.290 | 1920×1080 · 0.166→0.351 |

Rung bitrates come from the Pass 122 table (`table_version` 0x80): 2829 / 5754 /
10303 / 13769 / 18025 / 21839 kbps. `(Low latency, Range Medium)` is the one
knife-edge — 1600×900 lands at 0.0400 bpp, 0.1 % under the floor, so the
smaller size is taken. Raise the floor at all and that cell becomes 1600×900.

bpp is quoted worst rung → best rung. Every cell clears 0.04 at its floor and
0.12+ at its top.

## The tenth mode: variable-fps (`imx335-variable.json`)

Outside the matrix and **not record-friendly**: 1280×720, MCS 0–5, fps free to
float 30–100 via the §9.11 ladder (Pass 39/99). The only configuration that
spans the full link envelope — it trades cadence for rung all the way down. VFR
plus GDR intra-refresh means **live-view only, do not record**. See
`docs/venc-mode-matrix.md` §16.6.

Each mode carries `link.policy.fps_mode` (`static` default | `variable`): the
nine matrix modes are `static` (fps pinned, the §9.11 ladder held off);
`imx335-variable` is `variable` (the ladder runs). `apply-mode.sh` toggles the
ladder **live** through `POST /api/v1/link/fps` — **no link restart, no B9
re-pair** — so switching to/from the variable mode is a runtime change (only the
sensor.mode/size of a *different* mode still needs a venc restart).

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

- **Per-mode ladder spans / differential resilience** — the variable mode's
  ladder span (min 30 / preferred 100) is a per-craft `craft.json` constant, not
  a mode field, because it is read at ladder-construct time. Differential
  resilience per mode is likewise deferred.
- **IMX415**, and aspect-ratio handling.
