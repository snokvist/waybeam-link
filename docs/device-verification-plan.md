# §6.3b device verification plan — the continuation runbook

For the Claude Code CLI session on the bench machine that picks this branch
up. Everything below assumes the offline story is DONE and green (see
`docs/spatial-concealment.md`, findings 2026-08-19 + 2026-08-20, Pass 185):
the question on the table is no longer "does slice concealment work" but
"does the real SSC338Q bitstream, the real radios, and the real hardware
decoders agree".

**Status as of 2026-08-20 (end of the x86-only session).** Done: Phase D
(udp-air, 996/1000 at 20% loss vs 313 conceal-off), CTU32 real-content
validation, GDR convergence measurement, plus three defects found and fixed
by that work (TMVP §7.4.7.1 collocated mirroring; RPS-steady-state freeze
gate; its per-frame — not latched — form). Remaining and bench-only:
**Phase A → B → Phase D re-run with the real capture → E**, in that order.
rk3566 and Android are deliberately deferred (operator, 2026-08-20).
Draft PRs: waybeam-link #218, waybeam_venc #236.

Branches (same name in all three repos): `claude/waybeam-spatial-hevc-dkoqq3`
— waybeam-link (spec §6.3b + core + RX wiring + tools), waybeam_venc (0.66.0
`video0.sliceCount`), waybeam-hub (no changes needed; AUs are opaque to it).

## Setup on the bench (nothing from the cloud session carries over)

The cloud container's toolchains, HM build, and test vectors were all local
to it. A fresh bench session needs:

1. **waybeam_venc cross toolchain**: `make toolchain` in waybeam_venc
   (fetches the OpenIPC Infinity6E tarball), then `make verify` — the deploy
   binary is `out/star6e/waybeam`.
2. **waybeam-link dev build**: `cmake --preset dev && cmake --build --preset
   dev` (needs no radios for Phases A/D; `hevc_conceal_cli`,
   `spatial_conceal_bench`, `frame_shm_feed` land in `build/dev/`). For the
   §15.3-timing-honest bench numbers use `--preset release` (dev carries
   ASan, ~12× slower salvage).
3. **HM reference decoder** (the conformance judge — it caught two bugs
   ffmpeg/libde265 tolerated; keep it in the loop):
   ```
   curl -LO https://vcgit.hhi.fraunhofer.de/jvet/HM/-/archive/HM-18.0/HM-HM-18.0.tar.gz
   tar xzf HM-HM-18.0.tar.gz && cd HM-HM-18.0
   cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-Wno-error -w" \
         -DCMAKE_POLICY_VERSION_MINIMUM=3.5
   make -C build -j TAppDecoder     # binary under bin/umake/*/release/
   ```
4. **A synthetic ES** until Phase A supplies a real capture (this is the
   exact vector behind the Phase D numbers; `udp_air_run.sh` defaults
   `CONCEAL_ES` to `$CONCEAL_WORK/long.265`):
   ```
   ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=100:duration=10 \
     -pix_fmt yuv420p -c:v libx265 -x265-params \
     "slices=4:bframes=0:ref=1:keyint=100:no-sao=1:no-temporal-mvp=1:no-weightp=1:rc-lookahead=0:frame-threads=1:repeat-headers=1:crf=30:scenecut=0" \
     -f hevc long.265
   ```
5. ffmpeg + gstreamer (`h265parse`/`avdec_h265`) for the decode judgments;
   `tools/spatial_conceal/README.md` has the full judging loop.

Feature is opt-in end to end: `video0.sliceCount` defaults to 1 and
`streams[].conceal.mode` defaults to `"off"` — nothing changes on any craft
until both are set. Rollback at any phase = unset them.

Bench context (from the repo CLAUDE.md files — re-verify, buses shuffle):
craft `.232` = SSC338Q + RTL8812EU on 5805; ground station `192.168.2.20`
(RK3566, deploy via waybeam-hub's `deploy-ground` skill); x86 rig has the
8812AU (`6-1`) and two 8812CUs; unload kernel drivers before radio runs and
kill bench processes only from a script file (SIGTERM).

## Phase A — SSC338Q stream shape (top risk, do first)

The one thing offline could not answer: how the SDK exposes N slices through
`MI_VENC_GetStream` — one pack per slice, or several `packetInfo[]` entries
per pack — and whether an IDR AU (VPS+SPS+PPS+N slices) stays inside the
8-entry table. Everything downstream assumes the frame-ring blob carries all
N slice NALs.

1. Deploy venc 0.66.0 to `.232` (`scripts/star6e_direct_deploy.sh`), set
   `video0.sliceCount: 4` (`/api/v1/set`), restart-class → venc restarts.
2. Boot log must show `VENC: H.265 slice split ON: <r> CTU rows/slice
   (<rows> rows -> 4 slices)` and MUST NOT show the new
   `WARN: pack has %u NALs, packetInfo caps at %u` line. That WARN firing =
   the 8-entry clamp is real at this count → lower sliceCount until quiet
   and record the ceiling in findings.
3. Capture ~5 s of raw HEVC ES (venc recorder, or the frame-SHM consumer
   test tool) **including at least two IDRs**, pull it to the bench.
4. Judge the capture with the offline loop (this is the same loop that
   validated x265/HM vectors):
   ```
   build/dev/hevc_conceal_cli capture.265 <P-au> 1 rep.265
   python3 tools/spatial_conceal/validate.py capture.265 rep.265 W H <au> <rows>
   TAppDecoder -b rep.265 -o /dev/null       # zero asserts required
   ```
   Expected: parse reports 4 independent (non-dependent) slices at fixed
   addresses, no tiles, no WPP; conceal+validate pass as on HM vectors.
   Parse refusals here = a stream shape outside the §6.3b envelope — stop,
   file a finding with the SPS/PPS dump before writing any code.
5. Record in findings: TMVP flag (frozen vs motion-extrapolated conceal),
   slice sizes, IDR AU NAL count.

## Phase B — encoder cost of N slices

Same scene, fixed bitrate, `sliceCount` ∈ {1, 2, 4, 8}: record achieved
bitrate/QP (venc stats), slice-size distribution, and any fps drop. Pick the
fleet default from this table (expectation: 4). Findings entry.

## Phase C — RK3566 + hub decode of repaired AUs

1. On the bench: `build/release/spatial_conceal_bench capture.265 out.265
   200 42` (real SSC338Q content now), confirm `dropped=0`.
2. Ground station on the hub branch (unchanged code, just current): feed
   `out.265` AUs through the frame-SHM path (waybeam-link
   `tools/frame_shm_feed`, ring `venc_frame_out`) into the hub's rk3566
   pipeline. Watch `GET /pixelpilot/stats`: frame count advancing at the
   feed rate, zero pipeline rebuilds; hub log must not print
   `MPP: dropping/presenting partial frame errinfo=...` for repaired AUs.
   MPP acceptance is the point — ffmpeg/GStreamer/HM already passed offline.
3. Same feed on x86 `ground_x86` (vah265dec) if a VAAPI host is handy.

## Phase D — two-node link, synthetic loss (udp-air, no RF) — **DONE on x86 (2026-08-20)**

Run with `tools/spatial_conceal/udp_air_run.sh <drop_permille> <mode> <tag>`
(`frame_shm_feed play/dump` carry real HEVC through the rings). Result —
findings 2026-08-20: x265 1080p 4-slice, 1000 frames, p_rate 100‰:
20% loss delivered **996** frames conceal-on vs **313** conceal-off; 30%
still 992; every conceal-on egress decoded clean on ffmpeg + GStreamer;
`salvage_failed` single digits, all fail-safe drops.

**Re-run once with a real `.232` capture as `CONCEAL_ES`** (after Phase A) —
the synthetic-content pass does not re-prove the SSC338Q stream shape.
Carry-over for Phase E: IDR ARQ convergence is partial under blanket loss
(6/10 IDR AUs at 20%) — the freeze stands in and the stream rides ref-gaps
to the next IDR; if that looks bad on RF, raise IDR protection
(i_rate/min_r), don't touch the concealment.

## Phase E — live RF around the FEC cliff

The Pass-175-shaped run: craft `.232` TX, single 8812AU ground. Attenuate /
walk the link down from the safe end (never sweep from the wall). Desired
visual: clean → one/few frozen regions → more regions → heavily degraded but
temporally continuous; compare with `conceal.mode: "off"` (whole-frame
stutter). Record §15.3 lines both sides; screen capture for the doc.
Measure end-to-end latency unchanged when clean (salvage is off the hot
path; repaired frames may add ≤~1 ms on RK3566-class CPUs — verify).

## Phase F — deferred / follow-ups

- Android MediaCodec acceptance (Waybeam-android `:wifi` consumer).
- GDR × concealment: measured offline 2026-08-20 (asymptotic convergence
  under x265 PIR, exact resync at IDR). Remaining: the same burst
  measurement against the SigmaStar IntraRefresh implementation.
- Restore encoder tools one by one (SAO, TMVP already handled; weighted
  pred / B frames stay refused) and re-measure.
- `cabac_init_flag=1` donors: parse handles it; synthesis always writes 0 —
  fine (per-slice field), but confirm the SDK never emits donor shapes the
  parser refuses.
- Consider a per-frame "slices repaired" debug overlay in the hub OSD.

## Merge criteria

Phases A, C, D green (+ E strongly preferred) → undraft. Any phase that
fails in a way requiring a spec change goes through a new Pass, not a quiet
edit — §6.3b's refusal-to-drop fallback means a failed phase never blocks
shipping the branch dark (defaults off).
