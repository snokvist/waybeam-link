#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""P0a analyzer: diff two §10.6 craft artifacts.

The point is not "are they identical" — RSSI noise moves a placement by a step
now and then, which Pass 121's own 10-run campaign measured. The point is that
the two Pass 125/126 changes to §10.6 did not move anything they promised not
to:

  bracket     a bracket of ZERO WIDTH (first_bad_rssi == last_clean_rssi) is
              the W9 signature: a retreat that booked the clean probe's
              reading as the bad one.
  limiter     from Pass 130 a placement below max_qdb with NO bad probe has no
              evidence behind it -- the loss wall is the only thing entitled
              to stop a sweep early, and an RSSI plateau no longer does.
"""
import json
import sys

STEP_QDB = 16  # default seek_step_qdb — one step is within noise


def load(p):
    with open(p) as f:
        return json.load(f)


def bracket(art, rung):
    for c in art.get("ceilings", []):
        if c.get("rung") == rung:
            return c
    return {}


def main():
    base, new = load(sys.argv[1]), load(sys.argv[2])
    bq, nq = base["placement_qdb"], new["placement_qdb"]
    bl, nl = base["placement_loss_milli"], new["placement_loss_milli"]
    fails = 0

    print("\n  rung  base_qdb  new_qdb  d_steps  base_loss  new_loss  bracket(new)")
    for m in range(8):
        d = (nq[m] - bq[m]) / STEP_QDB
        nb = bracket(new, m)
        lc, fb = nb.get("last_clean_rssi"), nb.get("first_bad_rssi")
        bw = "-" if fb is None else f"{lc}..{fb}"
        # A movement is only a regression if the BASELINE placement was
        # evidence-based. A baseline rung stopped by the (now deleted) RSSI
        # plateau was never a measurement, so the sweep disagreeing with it is
        # the correction, not a regression.
        ob = bracket(base, m)
        base_grounded = ob.get("first_bad_rssi") is not None or bq[m] >= 108
        flag = ""
        if abs(d) > 1:
            if base_grounded:
                flag = "  <-- MOVED >1 step"
                fails += 1
            else:
                flag = "  (baseline was plateau-limited, not measured)"
        print(f"  {m:>4}  {bq[m]:>8}  {nq[m]:>7}  {d:>+7.0f}  "
              f"{bl[m]:>9}  {nl[m]:>8}  {bw}{flag}")

    # W9: a booked bracket must have real width. Zero width means the retreat
    # recorded the LAST CLEAN reading as first_bad — the pre-fix behaviour.
    print()
    for m in range(8):
        nb = bracket(new, m)
        lc, fb = nb.get("last_clean_rssi"), nb.get("first_bad_rssi")
        if fb is None:
            continue
        if fb == lc:
            print(f"  FAIL rung {m}: overload bracket has ZERO WIDTH "
                  f"(last_clean={lc}, first_bad={fb}) — W9 signature")
            fails += 1
        else:
            print(f"  ok   rung {m}: bracket width {abs(fb - lc)} dB "
                  f"(last_clean={lc}, first_bad={fb})")

    # Every placement must verify clean, and every placement below max_qdb
    # must sit one step under a MEASURED loss wall. Before Pass 130 three of
    # this craft's eight rungs were stopped by an RSSI plateau instead.
    print()
    for m in range(8):
        if nl[m] > 15:
            print(f"  note rung {m}: placement verified at {nl[m]}permille "
                  f"(> loss_ok 15) — §10.6 records it, §10.7 would fail")
        nb = bracket(new, m)
        if nq[m] < 108 and nb.get("first_bad_rssi") is None:
            print(f"  FAIL rung {m}: placed at {nq[m]} qdb below max with no "
                  f"loss wall behind it — nothing stopped the sweep")
            fails += 1

    print(f"\n  placements moved >1 step: "
          f"{sum(1 for m in range(8) if abs(nq[m] - bq[m]) > STEP_QDB)}/8")
    if fails:
        print(f"  P0a ANALYZER: {fails} problem(s)")
        return 1
    print("  P0a ANALYZER: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
