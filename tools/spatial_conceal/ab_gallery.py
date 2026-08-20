#!/usr/bin/env python3
"""ab_gallery.py — decoded-frame fault/recovery gallery for §6.3b review.

Builds the three fault variants of one capture and renders review images:
  A. slice salvage   — one slice of --slice-au replaced (hevc_conceal_cli)
  B. freeze          — --whole-au fully replaced by the whole-frame stand-in
  C. drop            — the same --whole-au deleted outright (freeze-off shape)
Each variant is decoded with ffmpeg -threads 1 (the SSC338Q TRAIL_N/TRAIL_R
mix makes threaded decode non-deterministic) and compared to the reference
decode: full frames as JPEG, |Δluma|×6 heatmaps, and per-frame mean/max
printed for the fault..heal window.

Usage: ab_gallery.py <capture.265> <outdir> --whole-au N --slice-au M --slice K
       [--cli build/dev/hevc_conceal_cli] [--geometry y0:y1]

Pick AUs by eye from the capture (a moving-scene AU makes scenario C's smear
honest). Two traps, both measured 2026-08-20 (docs/findings.md):
  - the CLI bypasses the production RPS-steady-state gate, so a --whole-au
    whose donor (the PREVIOUS au) is a base picture reproduces the permanent
    desync of the gate-bypass case, not production freeze — pick an AU whose
    previous two AUs carry identical RPS bits (mid base-period);
  - a TRAIL_N donor is refused by the CLI (as in production).
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hevc_ms as H  # noqa: E402

W, HT = 1920, 1080  # overridden from the SPS below
AMP = 6


def decode(path, w, h):
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-threads", "1", "-i", path,
         "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], capture_output=True)
    fsz = w * h * 3 // 2
    a = np.frombuffer(out.stdout, dtype=np.uint8)
    n = len(a) // fsz
    return a[:n * fsz].reshape(n, fsz)


def yuv2rgb(f, w, h):
    y = f[:w * h].reshape(h, w).astype(np.float32)
    u = f[w * h:w * h + w * h // 4].reshape(h // 2, w // 2).astype(np.float32)
    v = f[w * h + w * h // 4:].reshape(h // 2, w // 2).astype(np.float32)
    u = np.repeat(np.repeat(u, 2, 0), 2, 1) - 128
    v = np.repeat(np.repeat(v, 2, 0), 2, 1) - 128
    rgb = np.dstack([y + 1.402 * v, y - 0.344136 * u - 0.714136 * v,
                     y + 1.772 * u])
    return np.clip(rgb, 0, 255).astype(np.uint8)


def save(f, path, w, h, width=880):
    im = Image.fromarray(yuv2rgb(f, w, h))
    im.thumbnail((width, width))
    im.save(path, quality=87)


def heat(a, b, path, w, h, width=880):
    d = np.abs(a[:w * h].reshape(h, w).astype(np.int16) -
               b[:w * h].reshape(h, w).astype(np.int16))
    v = np.clip(d * AMP, 0, 255).astype(np.uint8)
    rgb = np.zeros((h, w, 3), np.uint8)
    rgb[..., 0] = v
    rgb[..., 1] = (v * 0.5).astype(np.uint8)
    im = Image.fromarray(rgb)
    im.thumbnail((width, width))
    im.save(path, quality=85)
    return round(float(d.mean()), 3), int(d.max())


def cut_au(data, target):
    """Return the stream with every slice NAL of AU `target` removed."""
    au = -1
    cuts = []
    for sc, off, end in H.split_annexb(data):
        t = (data[off] >> 1) & 0x3F
        if t < 32:
            if (data[off + 2] >> 7) & 1:
                au += 1
            if au == target:
                cuts.append((sc, end))
            if au > target:
                break
    keep = bytearray()
    pos = 0
    for sc, end in cuts:
        keep += data[pos:sc]
        pos = end
    keep += data[pos:]
    return bytes(keep)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("outdir")
    ap.add_argument("--whole-au", type=int, required=True)
    ap.add_argument("--slice-au", type=int, required=True)
    ap.add_argument("--slice", type=int, default=1)
    ap.add_argument("--cli", default="build/dev/hevc_conceal_cli")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    data = open(args.capture, "rb").read()
    sps = list(H.stream_context(data)[0].values())[0]
    w, h = sps["pic_width"], sps["pic_height"]

    frz_p = f"{args.outdir}/freeze_{args.whole_au}.265"
    sal_p = f"{args.outdir}/salvage_{args.slice_au}_{args.slice}.265"
    drp_p = f"{args.outdir}/drop_{args.whole_au}.265"
    for target, spec, outp in ((args.whole_au, "all", frz_p),
                               (args.slice_au, str(args.slice), sal_p)):
        r = subprocess.run([args.cli, args.capture, str(target), spec, outp])
        if r.returncode != 0:
            sys.exit(f"conceal refused AU {target} ({spec}) — see the donor "
                     "traps in this script's docstring")
    open(drp_p, "wb").write(cut_au(data, args.whole_au))

    ref = decode(args.capture, w, h)
    frz = decode(frz_p, w, h)
    sal = decode(sal_p, w, h)
    drp = decode(drp_p, w, h)
    O = args.outdir
    A, B = args.slice_au, args.whole_au
    save(ref[A], f"{O}/a_ref_f{A}.jpg", w, h)
    save(sal[A], f"{O}/a_salvage_f{A}.jpg", w, h)
    print("A salvage fault:", heat(ref[A], sal[A],
                                   f"{O}/a_salvage_f{A}_diff.jpg", w, h))
    save(ref[B], f"{O}/b_ref_f{B}.jpg", w, h)
    save(frz[B], f"{O}/b_freeze_f{B}.jpg", w, h)
    print("B freeze fault:", heat(ref[B], frz[B],
                                  f"{O}/b_freeze_f{B}_diff.jpg", w, h))
    # drop shifts decode indices by -1 from the cut AU on
    save(drp[B], f"{O}/c_drop_slot{B}.jpg", w, h)
    save(ref[B + 1], f"{O}/c_ref_f{B + 1}.jpg", w, h)
    print("C drop fault:", heat(ref[B + 1], drp[B],
                                f"{O}/c_drop_slot{B}_diff.jpg", w, h))
    print("frame  freeze(mean,max)  drop(mean,max)   [heal window]")
    for k in range(B, min(B + 8, len(drp))):
        fz = heat(ref[k], frz[k], f"{O}/b_freeze_f{k}_diff.jpg", w, h)
        dr = heat(ref[k + 1], drp[k], f"{O}/c_drop_slot{k}_diff.jpg", w, h)
        print(f"{k:5d}  {fz}  {dr}")


if __name__ == "__main__":
    main()
