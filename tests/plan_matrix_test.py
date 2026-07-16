#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# plan_matrix generator vs the design doc's worked examples (§4, §9-§11)
# plus the emitted-table envelope law (§9.5 budget never exceeds user max).
import json
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import plan_matrix as pm  # noqa: E402

failures = 0


def check(name, cond):
    global failures
    if not cond:
        print(f"FAIL {name}", file=sys.stderr)
        failures += 1


def args(argv):
    return pm.parse_args(argv)


# --- §4 average-frame table -------------------------------------------------
check("avg 30fps 2Mbps", abs(pm.avg_frame_kb(2, 30) - 8.333) < 0.01)
check("avg 90fps 20Mbps", abs(pm.avg_frame_kb(20, 90) - 27.778) < 0.01)

# --- §9 cell math on the illustrative capacities ------------------------------
# 20 MHz, MCS 5, 90 fps, user 2-30 => 3.6-24 Mbps, deadline <= 33.3 KB.
rng, dl = pm.cell(90, 24000, 2000, 30000)
check("cell mcs5 90fps range", rng == (3600, 24000))
check("cell mcs5 90fps deadline", abs(dl - 33.333) < 0.01)
# 20 MHz, MCS 0, 75 fps => exactly the 5 KB floor point (3 Mbps).
rng, _ = pm.cell(75, 3000, 2000, 30000)
check("cell mcs0 75fps floor point", rng == (3000, 3000))
# 20 MHz, MCS 0, 90 fps => unavailable (floor 3.6 > cap 3).
rng, _ = pm.cell(90, 3000, 2000, 30000)
check("cell mcs0 90fps unavailable", rng is None)
# §10 deadline-frame ceilings: 20 MHz MCS7 30 fps = 125 KB; 40 MHz MCS5
# 30 fps caps at the 196 KB transport ceiling.
_, dl = pm.cell(30, 30000, 2000, 30000)
check("deadline 20MHz mcs7 30fps", abs(dl - 125.0) < 0.01)
_, dl = pm.cell(30, 48000, 2000, 30000)
check("deadline 40MHz mcs5 30fps capped", dl == 196.0)

# --- §11 preferred point: the doc's example lands on MCS 3 @ 40 MHz ----------
a = args(["--illustrative", "--widths", "40", "--mcs-min", "1",
          "--mcs-max", "5", "--fps-min", "60", "--fps-preferred", "90",
          "--fps-max", "144", "--bitrate-min-kbps", "8000",
          "--bitrate-preferred-kbps", "20000", "--bitrate-max-kbps", "30000"])
caps = pm.capacities(a, base_table=None)
r = pm.recommend(caps, a)
check("recommend mcs3", r is not None and r["mcs"] == 3)
check("recommend fits", r["fits_preferred"])
check("recommend fps", r["fps"] == 90)
check("recommend bitrate", r["bitrate_kbps"] == 20000)

# Infeasible preferred bitrate falls back to the highest-capacity cell.
a2 = args(["--illustrative", "--widths", "20", "--mcs-min", "0",
           "--mcs-max", "2", "--bitrate-preferred-kbps", "20000",
           "--bitrate-min-kbps", "2000", "--bitrate-max-kbps", "30000"])
caps2 = pm.capacities(a2, base_table=None)
r2 = pm.recommend(caps2, a2)
check("fallback flagged", r2 is not None and not r2["fits_preferred"])
check("fallback highest capacity", r2["mcs"] == 2)

# --- emitted table: the §9.5 budget law respects the envelope ----------------
base = json.load(open(os.path.join(ROOT, "profiles", "table.example.json"),
                      encoding="utf-8"))
a3 = args(["--widths", "20", "--mcs-min", "1", "--mcs-max", "4",
           "--bitrate-min-kbps", "3000", "--bitrate-max-kbps", "12000"])
with tempfile.NamedTemporaryFile("r", suffix=".json") as tf:
    out = pm.emit_table(base, pm.capacities(a3, base), a3, tf.name)
    check("emitted rung count", len(out["profiles"]) == 4)
    check("emitted ids sequential",
          [p["id"] for p in out["profiles"]] == [0, 1, 2, 3])
    check("emitted floor", out["floor_profile"] == 0)
    for p in out["profiles"]:
        budget = pm.derived_capacity_kbps(p, 20)
        check(f"budget cap mcs{p['mcs']}",
              budget <= max(12000, p["bitrate_min_kbps"]))
        check(f"budget floor mcs{p['mcs']}", p["bitrate_min_kbps"] >= 3000)
        check(f"frac sane mcs{p['mcs']}",
              0 < p["airtime_budget_frac"] <= 1.0)
    reparsed = json.load(open(tf.name, encoding="utf-8"))
    check("emitted json loads", reparsed["profiles"] == out["profiles"])

print(f"plan_matrix_test: {'PASS' if failures == 0 else 'FAIL'} "
      f"({failures} failures)")
sys.exit(1 if failures else 0)
