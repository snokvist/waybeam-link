#!/usr/bin/env python3
"""validate.py — decode reference + repaired streams to YUV and check:
   1. repaired stream decodes to same frame count
   2. frames before the repaired one are identical
   3. in the repaired frame: erased slice region == previous frame's region
      (interior; boundary rows may differ if loop filtering crosses slices),
      untouched slice regions == reference decode
   4. after the next IDR, frames are identical to reference again

Usage: validate.py <ref.265> <rep.265> <W> <H> <frame_idx> <slice_rows_spec>
  slice_rows_spec: comma list of "y0:y1" luma-row ranges that were replaced
"""
import sys, subprocess, numpy as np

def decode(path, w, h):
    # -threads 1: SSC338Q captures mix TRAIL_N/TRAIL_R inside one picture
    # (non-conformant), and ffmpeg's frame-threaded recovery of that mix is
    # non-deterministic — two decodes of the same file diverge.
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-threads", "1", "-i", path,
         "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"],
        capture_output=True)
    err = out.stderr.decode()
    frames = np.frombuffer(out.stdout, dtype=np.uint8)
    fsz = w * h * 3 // 2
    n = len(frames) // fsz
    return frames[:n * fsz].reshape(n, fsz), err

def luma(f, w, h):
    return f[:w * h].reshape(h, w)

def main():
    ref_p, rep_p, w, h, fidx = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
    ranges = [tuple(map(int, r.split(":"))) for r in sys.argv[6].split(",")]
    ref, eref = decode(ref_p, w, h)
    rep, erep = decode(rep_p, w, h)
    print(f"decoder stderr ref={eref!r} rep={erep!r}")
    print(f"frames: ref {len(ref)} rep {len(rep)}", "OK" if len(ref) == len(rep) else "FAIL")
    pre_ok = all(np.array_equal(ref[i], rep[i]) for i in range(fidx))
    print(f"frames 0..{fidx-1} identical:", "OK" if pre_ok else "FAIL")
    ry = luma(rep[fidx], w, h); py = luma(rep[fidx - 1], w, h); fy = luma(ref[fidx], w, h)
    for (y0, y1) in ranges:
        m = 8  # interior margin for boundary filtering
        interior = slice(y0 + m, y1 - m)
        frozen = np.array_equal(ry[interior], py[interior])
        print(f"  rows {y0}:{y1} interior == prev frame:", "OK" if frozen else
              f"FAIL maxdiff={np.abs(ry[interior].astype(int)-py[interior].astype(int)).max()}")
    # untouched regions equal reference
    mask = np.ones(h, dtype=bool)
    for (y0, y1) in ranges:
        mask[max(0, y0 - 8):min(h, y1 + 8)] = False
    same = np.array_equal(ry[mask], fy[mask])
    print("  untouched rows == reference:", "OK" if same else
          f"FAIL maxdiff={np.abs(ry[mask].astype(int)-fy[mask].astype(int)).max()}")
    # propagation + resync at next IDR (keyint=100 -> frame 100)
    idr = ((fidx // 100) + 1) * 100
    if idr < len(ref):
        resync = np.array_equal(ref[idr], rep[idr])
        print(f"  frame {idr} (next IDR) == reference:", "OK" if resync else "FAIL")
    drift = [i for i in range(fidx, min(idr, len(ref)))
             if not np.array_equal(ref[i], rep[i])]
    print(f"  frames differing before resync: {len(drift)} (expected >0, propagation)")

if __name__ == "__main__":
    main()
