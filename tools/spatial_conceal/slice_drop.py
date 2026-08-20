#!/usr/bin/env python3
"""slice_drop.py — delete slice NALs to build the NEGATIVE CONTROL stream.

§6.3b always emits a complete access unit or drops the whole frame, so a
decoder never sees a gap from us. This tool builds what a decoder WOULD see
without §6.3b: an AU with one slice missing and the rest present.

That stream is the control every acceptance test needs. Without it, "the
hardware decoder accepted our repaired stream" does not establish that the
repair was necessary — and an intra-picture gap is precisely the case
Rockchip MPP decodes wrong while reporting errinfo 0.

Usage:
  slice_drop.py <in.265> <out.265> --au N --slice I [--slice J ...]
  slice_drop.py <in.265> <out.265> --every K --slice I   # every Kth AU

  --slice -1  drops the LAST slice of the AU.

Prints the AU/slice table it acted on. Parameter sets and non-VCL NALs are
always kept.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hevc_ms as H  # noqa: E402


def au_split(nals):
    """Group Annex-B NALs into AUs on first_slice_segment_in_pic_flag.

    Only the flag's bit position is needed, which is the first bit of the
    slice header for every VCL type — no SPS/PPS context required.
    """
    aus, cur = [], []
    for nal in nals:
        t = H.nal_type(nal)
        if t <= 31:
            first = (nal[2] >> 7) & 1
            if first and cur and any(H.nal_type(x) <= 31 for x in cur):
                aus.append(cur)
                cur = []
        cur.append(nal)
    if cur:
        aus.append(cur)
    return aus


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--au", type=int, action="append", default=[])
    ap.add_argument("--every", type=int)
    ap.add_argument("--slice", type=int, action="append", default=[])
    a = ap.parse_args()
    if not a.slice:
        sys.exit("need at least one --slice")
    if not a.au and not a.every:
        sys.exit("need --au N or --every K")

    data = Path(a.src).read_bytes()
    # split_annexb yields (sc_off, nal_off, nal_end) spans, not bytes.
    nals = [data[nal_off:nal_end] for _, nal_off, nal_end in
            H.split_annexb(data)]
    aus = au_split(nals)
    targets = set(a.au)
    if a.every:
        targets |= {i for i in range(len(aus)) if i % a.every == 0 and i > 0}

    out = bytearray()
    dropped = 0
    for idx, au in enumerate(aus):
        vcl = [n for n in au if H.nal_type(n) <= 31]
        keep = au
        if idx in targets and vcl:
            kill = set()
            for s in a.slice:
                si = len(vcl) - 1 if s < 0 else s
                if 0 <= si < len(vcl):
                    kill.add(id(vcl[si]))
            if len(kill) >= len(vcl):
                sys.exit(f"AU {idx}: refusing to delete every slice — "
                         f"that is a dropped frame, not a gap AU "
                         f"(use ab_gallery.py for whole-AU drop)")
            keep = [n for n in au if id(n) not in kill]
            sizes = [len(n) for n in vcl]
            print(f"AU {idx}: {len(vcl)} slices {sizes} -> dropped "
                  f"{len(vcl) - len([n for n in keep if H.nal_type(n) <= 31])}")
            dropped += 1
        for n in keep:
            out += b"\x00\x00\x00\x01" + n

    Path(a.dst).write_bytes(bytes(out))
    print(f"{len(aus)} AUs in, {dropped} AU(s) damaged, wrote {a.dst} "
          f"({len(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
