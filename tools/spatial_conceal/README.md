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
