# spatial_conceal — offline harness for §6.3b slice concealment

The decoder-in-the-loop test path that proved §6.3b before any hardware was
involved (docs/findings.md 2026-08-19, Pass 185). The **C++ in `core/` is the
authoritative implementation**; this directory is the prototype + the
validation drivers that judge repaired streams with real decoders, which unit
tests cannot do.

## Files

- `hevc_ms.py` — Python prototype of the parser + concealment generator
  (SPS/PPS/slice-header parse, all-skip CABAC writer, AU model). One known
  divergence from the C++: its WPP path writes `num_entry_point_offsets = 0`,
  which ffmpeg/libde265 accept but HM rejects; the C++ writes real entry
  points. Trust the C++ (`tools/hevc_conceal_cli`) for conformance work.
- `repair_test.py <in.265> <au> <slice[,..]> <out.265>` — replace chosen
  slices of one AU with generated concealment slices (Python generator).
- `validate.py <ref.265> <rep.265> <W> <H> <au> <y0:y1[,..]>` — decode both
  with ffmpeg and assert: same frame count, frames before the repair
  identical, the concealed rows' interior byte-equal to the previous frame,
  untouched rows byte-equal to the reference, resync at the next IDR.
- `salvage_sim.py <in.265> <s> <r> <loss%> <seed> <out.265>` — whole-pipeline
  simulation (chunking, i.i.d. loss, MDS-model FEC, salvage, rebuild) with
  the Python generator. The C++ twin over the real FrameFramer/Reassembler
  is `tools/spatial_conceal_bench.cpp` — prefer it.

## The loop that matters

```
# multi-slice vector (x265 needs WPP for slices; HM does not)
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=100:duration=0.4 \
  -pix_fmt yuv420p -c:v libx265 -x265-params \
  "slices=4:bframes=0:ref=1:keyint=100:no-sao=1:no-temporal-mvp=1:no-weightp=1:rc-lookahead=0:frame-threads=1:repeat-headers=1:crf=30:scenecut=0" \
  -f hevc hd.265

# production chain: FrameFramer -> loss -> FrameReassembler -> SpatialRepair
build/release/spatial_conceal_bench hd.265 out.265 200 42 100 100

# judge with decoders the project actually targets
ffmpeg -v error -i out.265 -f null -                      # VAAPI-class path
gst-launch-1.0 filesrc location=out.265 ! h265parse ! avdec_h265 ! fakesink
TAppDecoder -b out.265 -o /dev/null                       # HM = conformance
```

Keep HM in the loop for any writer change: it caught two §6.3b conformance
bugs (an extra alignment bit after end_of_subset, missing WPP entry points)
that ffmpeg and libde265 silently tolerated.

Real SSC338Q elementary streams: capture one AU stream via the venc recorder
(`waybeam_venc` raw HEVC ES recording) with `video0.sliceCount > 1` and feed
it through the same loop before trusting a new encoder configuration.

## Acceptance readout (`decode_compare.py` + `slice_drop.py`)

A decoder's error flags are not an acceptance signal — MPP decodes an
intra-picture slice gap with `errinfo == 0` while ~8% of the picture is
wrong. Compare the pictures instead:

```
# does the hardware decoder agree with software on the repaired stream?
python3 tools/spatial_conceal/decode_compare.py ref.265 repaired.265 \
    --ref-decoder ffmpeg --decoder gst:vah265dec        # rk3566: gst:<mpp element>

# the negative control: what a decoder sees WITHOUT §6.3b
python3 tools/spatial_conceal/slice_drop.py ref.265 gap.265 --au 50 --slice 1
```

`decode_compare.py` reports per-frame luma PSNR and the damaged **CTU-64 row**
indices, so a slice-band failure is named rather than averaged away. Three
traps, all paid for once (findings 2026-08-20):

- **Cut the reference at an AU boundary** (`ffmpeg -c copy -frames:v N`). A
  `head -c` cut truncates the last AU and fabricates ~11 dB of "damage".
- **Run the do-nothing control** (`ref` against `ref`) before reading any
  number. On the SSC338Q every 5th picture decodes ~42 dB differently between
  software and hardware — the TRAIL_N mixed-NAL-type non-conformance, not
  concealment.
- **Always pair a repaired-stream test with the gap-AU control**, or decoder
  acceptance of the repair does not show the repair was necessary.

## Fault/recovery gallery (`ab_gallery.py`)

Renders the operator-review image set from any capture — the one behind the
2026-08-20 "Spatial Concealment A/B" review page. One command:

```
python3 tools/spatial_conceal/ab_gallery.py capture.265 outdir \
  --whole-au 83 --slice-au 50 --slice 1
```

It builds three variants of the capture (slice salvage via
`hevc_conceal_cli`, whole-frame freeze via `all`, plain drop by deleting the
AU's NALs), decodes each with `ffmpeg -threads 1` (threaded decode of the
SSC338Q TRAIL_N/TRAIL_R mix is non-deterministic), and writes full-frame
JPEGs plus |Δluma|×6 heatmaps with per-frame mean/max over the fault→heal
window. The review page is those images inlined as data URIs into a static
HTML shell — nothing beyond the images and the printed numbers.

To redo it with a **moving scene**: capture with motion in frame (the drop
scenario's smear scales with motion; salvage and freeze are
content-independent), then pick the AUs by eye. Two donor traps, measured
2026-08-20 (`docs/findings.md`): the CLI bypasses the production RPS gate,
so a `--whole-au` right after a base picture (POC%5==1 on the SSC338Q
SVC-T ladder) freezes from a base donor and reproduces the permanent-desync
gate-bypass case, not production behaviour — pick mid base-period; and a
TRAIL_N donor (previous AU at POC%5==4) is refused, as in production.
