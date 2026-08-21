#!/usr/bin/env python3
"""decode_compare.py — decode two elementary streams and compare the PICTURES.

The Phase C readout. A decoder's error flags are not an acceptance signal:
Rockchip MPP's HEVC parser decodes an intra-picture slice gap with
`errinfo == 0` on every frame while ~8% of the picture is wrong (external
lab, docs/findings.md 2026-08-20). So "the hub logged no MPP warning" proves
nothing on its own — compare the decoded output.

Reports per-frame luma PSNR and WHERE the damage is, as CTU-64 row indices,
so a slice-band failure is named rather than averaged away.

Usage:
  decode_compare.py <ref.265> <test.265> [options]

    --size WxH        frame size (default: probed from ref)
    --decoder SPEC    ffmpeg (default) | gst:<element>, e.g. gst:vah265dec
                      (VAAPI) | raw (the file is already packed I420 at
                      --size, decoded elsewhere). Applies to BOTH streams
                      unless --ref-decoder is given.
                      `raw` is how the rk3566 is measured: the shipped
                      OpenIPC ground image has librockchip_mpp but no
                      gstreamer rockchip plugin, so the picture is produced
                      on the device by mpp_dec_yuv.c and compared here.
    --ref-decoder S   decode the reference with a different decoder — use
                      ffmpeg for the reference and gst:<hw> for the test to
                      ask "does the hardware decoder agree with software?"
    --diff-thresh N   per-pixel |delta| above which a row counts as damaged (2)
    --fail-psnr X     exit 1 if any compared frame is below X dB
    --limit N         stop after N frames
    --quiet           summary only

Exit: 0 clean, 1 a frame failed --fail-psnr, 2 decode/plumbing failure.
"""
import argparse
import math
import shutil
import subprocess
import sys

import numpy as np

CTU = 64


def probe_size(path):
    exe = shutil.which("ffprobe")
    if not exe:
        return None
    out = subprocess.run(
        [exe, "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0:s=x", path],
        capture_output=True)
    txt = out.stdout.decode().strip().splitlines()
    if not txt:
        return None
    try:
        w, h = txt[0].split("x")[:2]
        return int(w), int(h)
    except ValueError:
        return None


def decode(path, spec):
    """Decode to raw I420 bytes. Returns (bytes, stderr_text)."""
    if spec == "raw":
        with open(path, "rb") as f:
            return f.read(), ""
    if spec == "ffmpeg":
        # -threads 1: the SSC338Q mixes TRAIL_N/TRAIL_R inside one picture
        # (non-conformant), and ffmpeg's frame-threaded recovery of that mix
        # is non-deterministic — two decodes of one file diverge.
        cmd = ["ffmpeg", "-v", "error", "-threads", "1", "-i", path,
               "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"]
    elif spec.startswith("gst:"):
        element = spec[4:]
        # fdsink to stdout; h265parse gives the decoder whole AUs.
        cmd = ["gst-launch-1.0", "-q", "filesrc", f"location={path}", "!",
               "h265parse", "!", element, "!", "videoconvert", "!",
               "video/x-raw,format=I420", "!", "fdsink", "fd=1", "sync=false"]
    else:
        sys.exit(f"unknown --decoder {spec!r} "
                 f"(want ffmpeg, raw, or gst:<element>)")
    out = subprocess.run(cmd, capture_output=True)
    return out.stdout, out.stderr.decode()


def frames_of(raw, w, h):
    fsz = w * h * 3 // 2
    n = len(raw) // fsz
    if n == 0:
        return np.empty((0, fsz), dtype=np.uint8)
    return np.frombuffer(raw[:n * fsz], dtype=np.uint8).reshape(n, fsz)


def psnr(a, b):
    d = a.astype(np.int32) - b.astype(np.int32)
    mse = float(np.mean(d * d))
    if mse == 0.0:
        return math.inf
    return 10.0 * math.log10(255.0 * 255.0 / mse)


def damaged_rows(ref_y, test_y, thresh):
    """CTU-64 row indices whose worst pixel differs by more than thresh."""
    h = ref_y.shape[0]
    rows = []
    for r0 in range(0, h, CTU):
        band = slice(r0, min(r0 + CTU, h))
        d = np.abs(ref_y[band].astype(np.int32) - test_y[band].astype(np.int32))
        if d.max() > thresh:
            rows.append(r0 // CTU)
    return rows


def fmt_rows(rows):
    """Collapse [3,4,5,9] -> '3-5,9'."""
    if not rows:
        return "-"
    parts, start, prev = [], rows[0], rows[0]
    for r in rows[1:]:
        if r == prev + 1:
            prev = r
            continue
        parts.append(f"{start}" if start == prev else f"{start}-{prev}")
        start = prev = r
    parts.append(f"{start}" if start == prev else f"{start}-{prev}")
    return ",".join(parts)


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("ref")
    ap.add_argument("test")
    ap.add_argument("--size")
    ap.add_argument("--decoder", default="ffmpeg")
    ap.add_argument("--ref-decoder")
    ap.add_argument("--diff-thresh", type=int, default=2)
    ap.add_argument("--fail-psnr", type=float)
    ap.add_argument("--limit", type=int)
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    if a.size:
        w, h = (int(v) for v in a.size.lower().split("x"))
    else:
        got = probe_size(a.ref)
        if not got:
            sys.exit("cannot probe frame size — pass --size WxH")
        w, h = got

    ref_spec = a.ref_decoder or a.decoder
    ref_raw, ref_err = decode(a.ref, ref_spec)
    test_raw, test_err = decode(a.test, a.decoder)
    ref = frames_of(ref_raw, w, h)
    test = frames_of(test_raw, w, h)

    print(f"size {w}x{h}  ref decoder {ref_spec}  test decoder {a.decoder}")
    print(f"frames: ref {len(ref)}  test {len(test)}"
          + ("" if len(ref) == len(test) else "   *** COUNT MISMATCH ***"))
    for name, err in (("ref", ref_err), ("test", test_err)):
        err = err.strip()
        if err:
            head = "\n    ".join(err.splitlines()[:6])
            print(f"  {name} decoder stderr:\n    {head}")
    if len(ref) == 0 or len(test) == 0:
        print("VERDICT: DECODE FAILED")
        return 2

    n = min(len(ref), len(test))
    if a.limit:
        n = min(n, a.limit)
    ysz = w * h

    worst = (math.inf, -1)
    identical = 0
    row_hits = {}
    rows_by_frame = []
    if not a.quiet:
        print(f"{'frame':>6} {'PSNR dB':>9}  damaged CTU-64 rows")
    for i in range(n):
        ry = ref[i][:ysz].reshape(h, w)
        ty = test[i][:ysz].reshape(h, w)
        p = psnr(ry, ty)
        if p is math.inf:
            identical += 1
            rows = []
        else:
            rows = damaged_rows(ry, ty, a.diff_thresh)
            for r in rows:
                row_hits[r] = row_hits.get(r, 0) + 1
            if p < worst[0]:
                worst = (p, i)
        rows_by_frame.append(rows)
        if not a.quiet and rows:
            print(f"{i:>6} {p:>9.2f}  {fmt_rows(rows)}")

    print(f"\ncompared {n} frames: {identical} bit-identical, "
          f"{n - identical} differ")
    if worst[1] >= 0:
        print(f"worst frame {worst[1]}: {worst[0]:.2f} dB  "
              f"rows {fmt_rows(rows_by_frame[worst[1]])}")
    if row_hits:
        top = sorted(row_hits.items(), key=lambda kv: -kv[1])[:6]
        print("most-damaged CTU-64 rows (row:frames): "
              + " ".join(f"{r}:{c}" for r, c in top))
        print(f"  (row r covers luma lines {CTU}*r .. {CTU}*r+{CTU - 1})")

    if len(ref) != len(test):
        print("VERDICT: FRAME COUNT MISMATCH — the decoder dropped pictures")
        return 1
    if a.fail_psnr is not None:
        bad = [i for i in range(n)
               if psnr(ref[i][:ysz].reshape(h, w), test[i][:ysz].reshape(h, w))
               < a.fail_psnr]
        if bad:
            print(f"VERDICT: FAIL — {len(bad)} frame(s) below "
                  f"{a.fail_psnr} dB, first {bad[0]}")
            return 1
    if identical < n:
        # Not a pass. Without --fail-psnr there is no threshold to judge
        # against, and printing OK over differing pictures is how a silent
        # corruption gets shipped.
        print(f"VERDICT: DIFFERS — {n - identical}/{n} frames not "
              f"bit-identical (no --fail-psnr given, so no pass/fail)")
        return 0
    print("VERDICT: OK — bit-identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
