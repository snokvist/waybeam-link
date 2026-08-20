# Operating modes — the user-facing 3×3 matrix, per craft

Data, not code. One file per cell of `docs/venc-mode-matrix.md` §16.3.

**Two catalogs live here, one per craft sensor**, and a craft is deployed only
the files for its own sensor:

| prefix | craft | SoC / radio | modes | fingerprint |
|---|---|---|---|---|
| `imx335-*` | `.232` | SSC338Q + RTL8812EU | 9 matrix + 1 variable | `10-7b9d0735` |
| `imx662-*` | `.181` | CV610 + RTL8733BU | 9 matrix | `9-54cfcc7e` |

This directory is the union; `venc.modes_dir` on each craft holds the subset.
That matters because `catalog_fingerprint` is computed over **whatever is in
that directory** — install both prefixes on one craft and its fingerprint stops
matching the ground's pin for either.

The user picks two things and never sees anything in here:

| axis | user sees | this directory sets |
|---|---|---|
| **Latency** Low / Medium / High | responsiveness | `sensor.mode` + `video0.fps` — 100 / 60 / 30 |
| **Range** High / Medium / Low | how far before it degrades | `policy.select.min_profile`/`max_profile` — MCS 0–2 / 1–4 / 2–5 |

## Naming: the filename is an index

`collect_modes()` (`io/src/modes.cpp`) **name-sorts**, and §11.7 `0x07` MODE
addresses the catalog **by index** — so the sort order is the command's
argument space. Names are therefore built to sort the way a human reads the
grid:

```
<sensor>-<fps zero-padded to 3>fps-mcs<range floor>
```

Zero-padding is what makes `030 < 060 < 100`; without it `100fps` sorted
*first*. The range half is the band's **MCS floor**, so long-range → short-range
also sorts correctly, and the name states the actual band rather than a word
whose meaning has to be looked up. (The UI label — "High/Med/Low range" — is
derived from `mcs_min`/`mcs_max` at render time, not from the name.)

Both were operator rulings, 2026-08-16. **Renaming any file moves the
fingerprint and shifts every index after it**, so the ground's pinned catalogs
and the OSD menu's literal `arg` values must be regenerated in the same change.

## Encode resolution is derived, not chosen

A cell clears a **0.04 bpp floor at the lowest rung its range band allows**;
`bpp = kbps × 1000 / (fps × pixels)`. Range fixes the numerator and latency
fixes one factor of the denominator, so pixels is the only free variable. See
§16.0 for why resolution is not a third axis.

Two hard limits sit above that identity:

- **1280×720 is the floor resolution, every table** (operator ruling
  2026-08-16). Where the bpp ceiling falls below 720p the floor rung is traded
  away deliberately — a soft MCS0 picture beats a sub-720p one.
- **Encoder CPU**, on CV610 only (below).

The 0.04 floor is the one **guessed** number in the design. If it moves, every
cell moves together and the structure survives.

## IMX335 (`.232`) — uncapped 8812 ladder

| | Range High (MCS 0–2) | Range Medium (MCS 1–4) | Range Low (MCS 2–5) |
|---|---|---|---|
| **Low latency** 100 fps | 1280×720 · **0.031**→0.112 | 1280×720 · 0.062→0.196 | 1920×1080 · 0.050→0.105 |
| **Medium latency** 60 fps | 1280×720 · 0.051→0.186 | 1920×1080 · 0.046→0.145 | 1920×1080 · 0.083→0.176 |
| **High latency** 30 fps | 1920×1080 · 0.046→0.166 | 1920×1080 · 0.093→0.290 | 1920×1080 · 0.166→0.351 |

Rung bitrates come from the Pass 122 table (`table_version` 0x80): 2829 / 5754 /
9264 / 12384 / 16213 / 19646 kbps (long GI on every rung, §9.5 ruling 2026-08-20). `(Low latency, Range Medium)` is the one
knife-edge — 1600×900 lands at 0.0400 bpp, 0.1 % under the floor, so the
smaller size is taken. Raise the floor at all and that cell becomes 1600×900.

bpp is quoted worst rung → best rung. **`(Low latency, Range High)` is the one
cell that does not clear 0.04** — it sits at 0.031 at MCS0, the deliberate
trade the 720p floor buys. Every other cell clears 0.04 at its floor and 0.12+
at its top.

> Two corrections to what this table used to say. It printed 960×540 for that
> cell while the shipped file was `1024x576` — 960×540 was the raw bpp pick and
> venc rejects it (`height % 8`). And the standing claim "every cell clears
> 0.04 at its floor" has not held since the 720p floor was ruled.

## IMX662 / CV610 (`.181`) — 8733B ladder under a 12288 kbps cap

| | Range High (MCS 0–2) | Range Medium (MCS 1–4) | Range Low (MCS 2–5) |
|---|---|---|---|
| **Low latency** 100 fps | 1280×720 · **0.031**→0.101 | 1280×720 · 0.062→0.133 | 1280×720\* · 0.101→0.133 |
| **Medium latency** 60 fps | 1280×720 · 0.051→0.168 | 1920×1080 · 0.046→0.099 | 1920×1080 · 0.075→0.099 |
| **High latency** 30 fps | 1920×1080 · 0.046→0.149 | 1920×1080 · 0.093→0.198 | 1920×1080 · 0.149→0.198 |

`table-8733b.json` derives 2829 / 5754 / 9264 / 12384 / 16213 / 19646 kbps, and
the craft pins `venc.max_bitrate_kbps: 12288`, so the **effective** ladder is
2829 / 5754 / 9264 / 12288 / 12288 / 12288 — only rungs 3–5 are capped. (The
8000 first pinned here measured a *CPU* wall during the 720p100 bring-up, not a
link wall; the operator raised it 2026-08-16. 12288 sits just above the 12000
that measured zero drops at 1080p in-process and below the 13000 that dropped
0.3–2.3 fps.)

**Encoder CPU is a second limiter this craft has and IMX335 does not.** Measured
on `.181`: 1280×720@100 (92 Mpx/s) ≈ 60 % of one core, 1080p@12000
(207 Mpx/s) ≈ 99 %. Sizes are therefore
`min(bpp ceiling, 720p floor, ~130 Mpx/s)`. **At this cap, CPU — not bitrate —
is what keeps 1080p out of the 100 fps row**: bpp there would allow up to
2.32 Mpx, more than a 1080p frame.

\* **Open.** `(Low latency, Range Low)` is the one cell the CPU budget alone
holds down, and the only change that would make the 100 fps row differ by range
at all. 1600×900 is 0.064→0.085 bpp — comfortable — but 144 Mpx/s, ≈ 94 % of one
core by the fit above. Measure before raising it.

`(Low latency, Range Medium)` is the craft's shipped operating point
(1280×720@100), so the matrix contains today's configuration rather than
replacing it.

**No `imx662-variable`** — the §9.11 ladder needs a `venc.fps_ladder` span in
`craft.json` and this craft has none; shipping an unmeasured VFR mode would be
an untested runtime change, not data. **No 90 fps row** — `sensor.mode: 2`
exists and stays reachable by hand, but a fourth rung breaks the ruled symmetry
with IMX335.

## The tenth IMX335 mode: variable-fps (`imx335-variable.json`)

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

| fps | IMX335 `sensor.mode` | capture | IMX662 `sensor.mode` | capture |
|---|---|---|---|---|
| 30 | 0 | 2560×1920 (4:3) | 0 | 1920×1080 RAW12 |
| 60 | 1 | 2560×1920 (4:3) | 1 | 1920×1080 RAW12 |
| 100 | 3 | 2176×1224 (16:9) | 3 | 1920×1080 RAW10 |

**IMX335 asymmetry worth knowing:** 30 and 60 fps read the whole 4:3 sensor, so
a 16:9 encode there is a vertical crop and 4:3 output is free. At 100 fps the
only mode is already 16:9, so there is more FOV available at 30/60 than at 100.

**IMX662 has no such asymmetry** — all four modes capture 1920×1080 and differ
only in rate, bit depth and MCLK (`waybeam_venc/src/cv610_modes.c`). Since venc
`0.65.3` its selector matches SigmaStar's, so an explicit `sensor.mode` wins and
`video0.fps` is a target — the same two knobs mean the same thing on both
crafts. A non-16:9 `video0.size` centre-crops rather than squashing
(`isp.keepAspect`).

## Applying a mode

`sensor.mode` and `video0.size` are `restart_required`, so a mode is a
**pre-flight** choice, not an in-flight one. Only `video0.fps` and
`video0.bitrate` are live, and the link owns bitrate.

`deploy/modes/apply-mode.sh` does all of it; `POST /api/v1/mode` (or §11.7 MODE
over the air) is the supported entry. Two craft differences it carries, both
in `apply-mode.conf` next to it — §15.5 forks the applier with `execl()` and no
environment, so that file is the only place a craft can state how it differs:

| | `.232` (SigmaStar) | `.181` (CV610) |
|---|---|---|
| HTTP client | curl | **busybox wget only** — no curl on the board |
| venc restart | `S95waybeam restart` | **`VENC_RESTART=api`** |
| link restart | `S96waybeam-link` + re-scout/quickconnect (B9) | none — the link runs in-process inside waybeam-hub |

`VENC_RESTART=api` is not a preference. The CV610 `S95waybeam` `stop()` ends in
`$LOADER stop`, which unloads the ~25 `open_*` MPP modules, takes the board off
the network and needs a power cycle — so an init restart in flight is
unrecoverable. venc's own `POST /api/v1/restart` re-execs the daemon and leaves
the loader alone.

Verify `video0.fps` by measuring the delivered frame rate ground-side, never by
reading it back: after a craft reboot venc's live fps actuation silently
no-ops — the API returns `ok`, `/get` echoes the commanded value, and the
sensor keeps running at its old rate until a sensor-mode reinit
(`docs/venc-mode-matrix.md` §13). On CV610 the same rule holds for a different
reason: a wrong sensor MCLK makes frames arrive at `fps × actual/expected`
while both `/fps/live` and `/api/v1/modes` report the nominal rate.

## Dependencies

These bitrates assume **Pass 94** (§14.1 `min_k` gate conditioned on ARQ
eligibility) and **Pass 95** (§9.5 `fec_overhead_frac` non-zero). Without
Pass 94 the Range-High column is unsafe at its floor rung — that is exactly
the B11 failure. Without Pass 95 every bitrate here is ~18 % optimistic and
the floor cells do not actually clear 0.04.

The IMX662 numbers carry one further caveat: `table-8733b.json`'s airtime and
FEC constants are still 8812-calibrated seeds (§17 re-derive pending), so its
*derived* top rungs overshoot what the radio actually drains (~14.4 Mbps
standalone at rung 5, less in-process). The 12288 cap hides that above rung 2 —
and the MCS0/MCS1/MCS2 floors, which are what the bpp derivation above actually
depends on, sit below the cap and are used as-is.

## Not covered yet

- **`(imx662, 100 fps, Range Low)` at 1600×900** — held at 720p pending a CPU
  measurement on `.181`.
- **`imx662-variable`** — needs a `venc.fps_ladder` span on the craft first.
- **Per-mode ladder spans / differential resilience** — the variable mode's
  ladder span (min 30 / preferred 100) is a per-craft `craft.json` constant, not
  a mode field, because it is read at ladder-construct time. Differential
  resilience per mode is likewise deferred.
- **IMX415**, and aspect-ratio handling beyond the CV610 centre-crop.
