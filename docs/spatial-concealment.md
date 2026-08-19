# Spatial concealment (§6.3b) — architecture, measured results, limits

Status 2026-08-19: **offline-proven, device-unverified.** The complete chain
below runs in production code and was validated with three software decoders
(ffmpeg 6.1, libde265 1.0.15, HM-18.0) plus the ground hub's exact x86
GStreamer graph shape (`h265parse ! avdec_h265`). No RF, RK3566, or Android
run has happened yet. Contract: PROTOCOL.md §6.3b (Pass 185); Tier-2 numbers:
findings.md 2026-08-19.

## The chain

```
SSC338Q venc — video0.sliceCount N  (waybeam_venc 0.66.0, MI_VENC_SetH265SliceSplit)
   -> one access unit per frame-SHM slot, now N slice NALs inside (unchanged §15.4)
FrameFramer — unchanged §5.1a/§14.1: s-byte symbols, systematic Cauchy-RS parity
   -> radio -> loss
FrameReassembler
   all k sources ............ fast path                     (unchanged)
   >= k src+repair .......... full RS decode                (unchanged)
   < k at deadline/supersede  -> SalvageHook                (new, opt-in)
SpatialRepair (core/spatial_repair.cpp)
   verified source chunks -> present byte ranges -> Annex-B scan inside them
   -> per-slice complete/erased -> erased slices replaced by synthesized
      all-skip P slices (core/hevc_conceal.cpp) -> complete AU egress
   -> no survivor at all -> whole-frame freeze (POC advanced)   [freeze_frame]
   -> any refusal -> drop, byte-for-byte the pre-§6.3b behaviour
hub / decoder — no changes: the AU is ordinary, syntactically complete HEVC
```

Nothing changed on the wire; salvage consumes only what the §5.1a/§14.1
subheaders already carry (`k`, `i`, `s`, `frame_len`). Slice geometry is
learned from delivered frames, never configured, and a geometry mismatch
fails toward drop.

## The synthesized slice

Header rewritten from a surviving slice of the same picture (HEVC §7.4.7.1
makes the picture-level fields shared), with SAO off, one active reference,
`MaxNumMergeCand = 1`, deblocking disabled where the PPS allows an override.
Payload: per CTU, `split_cu_flag=0`, `cu_skip_flag=1`, terminate — every CU
merges to a zero (or, with TMVP on, collocated) motion vector with no
residual, so the decoder reconstructs the span from the reference picture.
Partial CTUs at the picture's right/bottom edge take the inferred-split
quadtree path; WPP streams get real entry-point offsets. 10–21 B per slice.

## Measured (x86, release; test content 1920×1080@100 4 slices, x265)

- Substitution correctness: concealed region interior **byte-equal** to the
  previous decoded picture; untouched slices byte-equal to the reference
  decode; next IDR resyncs exactly. Holds for first/middle/last slice, pairs,
  scattered pairs, all-four, at 512×512 and 1080p, WPP and no-WPP, SAO on and
  off, TMVP on and off.
- Full production chain (`tools/spatial_conceal_bench`, IDR loss exempt as
  ARQ would make it): 16% symbol loss → 0/40 frames dropped (23 salvaged,
  3 frozen); 31% → 0/40 (27 salvaged, 11 frozen). Every output decodes to
  full frame count on every decoder tried.
- Cost: salvage (assembly + scan + synthesis) avg **97 µs**, max 211 µs per
  repaired 1080p frame; bare synthesis 21.6 µs per quarter-1080p CTU32
  slice, 89 µs for a whole-1080p freeze. Budget at 100 fps is 10 ms/frame.
- Cross builds: ssc338q (ARMv7, 32-bit) clean; dev ASan/UBSan clean.

## Known limits / what the device work must answer

1. **SSC338Q stream shape** (top risk): does the SDK emit one pack per slice
   or several `packetInfo[]` entries per pack, and does an IDR AU
   (VPS+SPS+PPS+N slices) stay within the 8-entry table? venc warns (once)
   on truncation now; read that log before raising `sliceCount`. Verify the
   real elementary stream with `tools/spatial_conceal/` before trusting a
   count.
2. **Bitrate/quality cost of N slices** on the real encoder: unmeasured.
   Sweep sliceCount ∈ {1,2,4,8} at fixed bitrate and compare.
3. **RK3566 MPP and Android MediaCodec** acceptance: unverified (the rk3566
   path feeds MPP whole AUs with split-parse; MediaCodec gets them via the
   Android consumer). ffmpeg/GStreamer/HM acceptance is necessary, not
   sufficient.
4. **GDR interaction**: a concealed region voids that GDR cycle's clean-area
   claim for the affected rows; convergence on the following pass is the
   design assumption — measure real error persistence on hardware.
5. **B slices / weighted prediction / tiles / dependent slice segments**:
   out of the supported envelope, refused at parse. The fleet encoder emits
   none of them.
6. **Freeze of a frame that was actually an IRAP** leaves the same reference
   gap a drop would; §3.9 recovery covers both. Loss telemetry
   (`frames_frozen` vs `frames_salvaged`) shows how often freezes happen.

## Where things live

| piece | location |
|---|---|
| spec | PROTOCOL.md §6.3b (+§6.3a/4-5, §14.1, §15.2, §15.3), Pass 185 |
| parser + CABAC writer | `core/src/hevc_conceal.cpp` |
| salvage engine | `core/src/spatial_repair.cpp` |
| reassembler hook | `core/src/frame_reassembler.cpp` (`try_salvage`) |
| RX wiring + config | `node/src/rx_node.cpp`, `streams[].conceal` |
| encoder knob | waybeam_venc `video0.sliceCount` (0.66.0) |
| decoder-in-the-loop | `tools/hevc_conceal_cli`, `tools/spatial_conceal_bench`, `tools/spatial_conceal/` |
| unit tests | `tests/hevc_conceal_test.cpp`, `tests/spatial_repair_test.cpp`, `tests/frame_reassembler_test.cpp` |
