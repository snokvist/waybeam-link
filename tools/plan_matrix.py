#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Feasibility-matrix / preferred-point generator (design-doc §6-§12/§15 as
TOOLING — the runtime keeps the §9.3 rung ladder; this tool computes the
user-facing matrices and can EMIT a profiles table that encodes the user's
envelopes in the §9.5 budget law, so the flight-critical selector never
grows a 3-D search).

Inputs: user envelopes (fps / bitrate / MCS min-preferred-max, channel
width) and a per-(MCS, width) safe-capacity model:
  - default: derived from a base profile table via the §9.5 Pass-6 integer
    law (HT20/HT40 PHY rates x airtime fraction x (1 - FEC overhead) -
    reserves),
  - --illustrative: the design doc's example capacities (UI demos, tests),
  - --capacity-file JSON {"20": {"0": kbps, ...}, "40": {...}}: calibrated
    bench values (§17) — always wins when given.

Outputs: the steady-state bitrate matrix and the deadline-safe individual
frame matrix per width, a recommended operating point card, and optionally
(--emit-table) a generated profiles/table JSON.
"""
import argparse
import json
import sys

LADDER_FPS = [30, 45, 60, 75, 90, 100, 120, 144]
HT20_LGI_KBPS = [6500, 13000, 19500, 26000, 39000, 52000, 58500, 65000]
HT40_LGI_KBPS = [13500, 27000, 40500, 54000, 81000, 108000, 121500, 135000]
# Design-doc §8 illustrative safe video capacities (kbps).
ILLUSTRATIVE_KBPS = {
    "20": [3000, 6000, 9000, 12000, 18000, 24000, 27000, 30000],
    "40": [6000, 12000, 18000, 24000, 36000, 48000, 54000, 60000],
}
SYSTEM_MIN_KBPS = 2000      # doc: 2-30 Mbps hard range
SYSTEM_MAX_KBPS = 30000
FRAME_FLOOR_KB = 5.0        # doc: 5-196 KB planning envelope
FRAME_CEILING_KB = 196.0


def phy_kbps(mcs, width_mhz, short_gi):
    base = (HT40_LGI_KBPS if width_mhz == 40 else HT20_LGI_KBPS)[mcs]
    return base * 10 // 9 if short_gi else base


def derived_capacity_kbps(profile, width_mhz):
    """The §9.5 Pass-6 budget law, mirrored integer-exact."""
    kbps = phy_kbps(profile["mcs"], width_mhz,
                    profile.get("guard_interval", "long") == "short")
    kbps = kbps * round(profile["airtime_budget_frac"] * 1000) // 1000
    kbps = kbps * (1000 - round(profile.get("fec_overhead_frac", 0.0) * 1000)) // 1000
    reserve = (profile["reserve_bps"]["control"] +
               profile["reserve_bps"]["telemetry"]) // 1000
    return max(kbps - reserve if kbps > reserve else 0,
               profile["bitrate_min_kbps"])


def avg_frame_kb(bitrate_mbps, fps):
    return 125.0 * bitrate_mbps / fps  # doc §4


def cell(fps, c_safe_kbps, user_min_kbps, user_max_kbps):
    """Doc §7: steady-state feasible bitrate range for one cell (kbps), or
    None when unavailable. Also returns the deadline-safe frame KB."""
    frame_floor_kbps = int(FRAME_FLOOR_KB * 8 * fps)        # 0.04F Mbps
    frame_ceiling_kbps = int(FRAME_CEILING_KB * 8 * fps)    # 1.568F Mbps
    lo = max(SYSTEM_MIN_KBPS, user_min_kbps, frame_floor_kbps)
    hi = min(SYSTEM_MAX_KBPS, user_max_kbps, frame_ceiling_kbps, c_safe_kbps)
    deadline_kb = min(FRAME_CEILING_KB, 125.0 * (c_safe_kbps / 1000.0) / fps)
    if lo > hi:
        return None, deadline_kb
    return (lo, hi), deadline_kb


def capacities(args, base_table):
    out = {}
    for width in args.widths:
        key = str(width)
        if args.capacity_file:
            table = json.load(open(args.capacity_file, encoding="utf-8"))
            out[width] = {int(m): int(v) for m, v in table[key].items()}
        elif args.illustrative:
            out[width] = {m: ILLUSTRATIVE_KBPS[key][m] for m in range(8)}
        else:
            out[width] = {
                p["mcs"]: derived_capacity_kbps(p, width)
                for p in base_table["profiles"]
            }
    return out


def build_matrix(caps, width, args):
    rows = {}
    for mcs in range(args.mcs_min, args.mcs_max + 1):
        if mcs not in caps[width]:
            continue
        row = {}
        for fps in LADDER_FPS:
            if not (args.fps_min <= fps <= args.fps_max):
                continue
            row[fps] = cell(fps, caps[width][mcs],
                            args.bitrate_min_kbps, args.bitrate_max_kbps)
        rows[mcs] = row
    return rows


def recommend(caps, args):
    """Doc §11/§14: at the preferred fps, the LOWEST allowed MCS that carries
    the preferred bitrate with >= headroom reserve; fall back to the
    highest-capacity allowed cell when none does."""
    best = None
    for width in args.widths:
        for mcs in range(args.mcs_min, args.mcs_max + 1):
            if mcs not in caps[width]:
                continue
            rng, deadline_kb = cell(args.fps_preferred, caps[width][mcs],
                                    args.bitrate_min_kbps,
                                    args.bitrate_max_kbps)
            if rng is None:
                continue
            c = caps[width][mcs]
            fits = c * (1000 - args.headroom_permille) // 1000 >= \
                args.bitrate_preferred_kbps and rng[1] >= \
                args.bitrate_preferred_kbps
            cand = {
                "width_mhz": width, "mcs": mcs,
                "fps": args.fps_preferred,
                "bitrate_kbps": min(args.bitrate_preferred_kbps, rng[1]),
                "range_kbps": rng, "deadline_frame_kb": round(deadline_kb, 1),
                "reserve_permille":
                    max(0, 1000 - args.bitrate_preferred_kbps * 1000 // c),
                "fits_preferred": fits,
            }
            if fits:
                return cand  # lowest MCS on the narrowest width wins outright
            if best is None or cand["range_kbps"][1] > best["range_kbps"][1]:
                best = cand
    return best


def emit_table(base_table, caps, args, path):
    """Encode the envelopes into a runtime table via the §9.5 law: clip the
    rung set to the MCS envelope, cap each rung's airtime fraction so the
    derived budget never exceeds the user max, and floor bitrate_min at the
    user min."""
    out = json.loads(json.dumps(base_table))  # deep copy
    profiles = []
    for p in out["profiles"]:
        if not (args.mcs_min <= p["mcs"] <= args.mcs_max):
            continue
        reserve = (p["reserve_bps"]["control"] +
                   p["reserve_bps"]["telemetry"]) // 1000
        phy = phy_kbps(p["mcs"], args.widths[0],
                       p.get("guard_interval", "long") == "short")
        fec = 1000 - round(p.get("fec_overhead_frac", 0.0) * 1000)
        want = (args.bitrate_max_kbps + reserve) * 1000 * 1000 // (phy * fec)
        frac = min(round(p["airtime_budget_frac"] * 1000), want)
        p["airtime_budget_frac"] = frac / 1000.0
        p["bitrate_min_kbps"] = max(p["bitrate_min_kbps"],
                                    args.bitrate_min_kbps)
        profiles.append(p)
    for i, p in enumerate(profiles):
        p["id"] = i
    out["profiles"] = profiles
    out["floor_profile"] = 0
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    return out


def fmt_mbps(kbps):
    v = kbps / 1000.0
    return f"{v:g}"


def print_report(caps, args):
    for width in args.widths:
        m = build_matrix(caps, width, args)
        print(f"\n## {width} MHz steady-state bitrate (Mbps) / "
              "deadline-safe frame (KB)")
        fps_cols = [f for f in LADDER_FPS if args.fps_min <= f <= args.fps_max]
        print("MCS | " + " | ".join(f"{f} fps" for f in fps_cols))
        for mcs, row in m.items():
            cells = []
            for f in fps_cols:
                rng, dl = row[f]
                cells.append("--" if rng is None else
                             f"{fmt_mbps(rng[0])}-{fmt_mbps(rng[1])} "
                             f"(≤{dl:.1f}KB)")
            print(f"{mcs}   | " + " | ".join(cells))
    r = recommend(caps, args)
    if r:
        print(f"\nRecommended: {r['fps']} FPS · "
              f"{fmt_mbps(r['bitrate_kbps'])} Mbps · MCS {r['mcs']} · "
              f"{r['width_mhz']} MHz")
        print(f"Average frame: "
              f"{avg_frame_kb(r['bitrate_kbps'] / 1000.0, r['fps']):.1f} KB · "
              f"deadline ceiling {r['deadline_frame_kb']} KB · "
              f"reserve {r['reserve_permille'] / 10:.0f}%"
              + ("" if r["fits_preferred"] else
                 " · WARNING: preferred bitrate does not fit — "
                 "highest-capacity fallback shown"))


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base-table", default="profiles/table.example.json")
    ap.add_argument("--capacity-file")
    ap.add_argument("--illustrative", action="store_true")
    ap.add_argument("--widths", type=lambda s: [int(w) for w in s.split(",")],
                    default=[20],
                    help="channel widths in PREFERENCE order (the recommender "
                         "returns the first width whose lowest allowed MCS "
                         "fits the preferred bitrate)")
    ap.add_argument("--fps-min", type=int, default=60)
    ap.add_argument("--fps-preferred", type=int, default=90)
    ap.add_argument("--fps-max", type=int, default=144)
    ap.add_argument("--bitrate-min-kbps", type=int, default=8000)
    ap.add_argument("--bitrate-preferred-kbps", type=int, default=20000)
    ap.add_argument("--bitrate-max-kbps", type=int, default=30000)
    ap.add_argument("--mcs-min", type=int, default=0)
    ap.add_argument("--mcs-max", type=int, default=5)
    ap.add_argument("--headroom-permille", type=int, default=150)
    ap.add_argument("--emit-table")
    args = ap.parse_args(argv)
    for f in (args.fps_min, args.fps_preferred, args.fps_max):
        if f not in LADDER_FPS:
            ap.error(f"{f} is not a ladder fps {LADDER_FPS}")
    if not (args.fps_min <= args.fps_preferred <= args.fps_max):
        ap.error("fps envelope must be min <= preferred <= max")
    for w in args.widths:
        if w not in (20, 40):
            ap.error("widths must be 20 and/or 40")
    return args


def main(argv):
    args = parse_args(argv)
    base_table = json.load(open(args.base_table, encoding="utf-8"))
    caps = capacities(args, base_table)
    print_report(caps, args)
    if args.emit_table:
        emit_table(base_table, caps, args, args.emit_table)
        print(f"\nwrote {args.emit_table}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
