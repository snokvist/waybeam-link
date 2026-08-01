#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Bench: closed-loop per-rung power calibration -> an authored par.10.2 curve.
#
# The "calibration feature" workflow: run ONCE against the current
# craft/ground pairing, persist the artifacts, let the par.10.2 commit
# resolve apply them from then on; re-run when the pairing changes (craft
# adapter, ground adapters, antennas, mounting).
#
# Why closed-loop: devourer bring-up TXAGC wanders ~6 dB between starts and
# per-rate TXAGC differs per chip cal (the measured MCS5-hot anomaly), so an
# open-loop dBm curve is not trustworthy. This steers each rung's power until
# the GROUND-reported RSSI lands in a target band, then verifies PER there,
# then probes upward for the rung's overload ceiling.
#
# Per rung r (all 8 by default):
#   1. pin r (min=max=r via craft :8091)
#   2. steer: probe-dwell, adjust power via a live dBm->RSSI fit until
#      rssi in [target-tol, target+tol] (or power limits hit)
#   3. verify-dwell: wire PER (craft tx_submitted delta vs ground rx delta,
#      per adapter) at the placement -- must be <= per_ok_milli
#   4. ceiling probe: step +ceil_step dB until PER > per_bad_milli or the
#      power cap; record the bracketing RSSIs
#
# Outputs:
#   --curve-out   PHY_REG_PG-subset curve file (par.10.2). The authored value
#                 is the LEVEL-4 BASELINE, i.e. placement compensated for the
#                 profile table's tx_power_level so the runtime resolve
#                 (curve[mcs] + (level-4)*8qdb) reproduces the calibrated
#                 placement. Non-HT indices carry the MCS0 value.
#   --report-out  JSON: per-rung placement power/RSSI/PER + ceiling bracket.
#                 The ceiling column is the future par.9.4 banded-gate input.
#
# Kernel-monitor actuation (iw fixed) -- run against the operational craft,
# no restart needed. Restores profile window and txpower on any exit.

import argparse
import json
import sys
import time

from mcs_power_sweep import (craft_pin, craft_stats, craft_txpower,
                             ground_stats, tx_total)


def measure(args, secs):
    c0, g0 = craft_stats(args.craft), ground_stats(args.ground)
    time.sleep(secs)
    c1, g1 = craft_stats(args.craft), ground_stats(args.ground)
    tx_d = tx_total(c1) - tx_total(c0)
    prev = {a["name"]: a for a in g0.get("adapters", [])}
    ads = []
    for a in g1.get("adapters", []):
        p = prev.get(a["name"])
        if p is None:
            continue
        rx_d = a["rx"] - p["rx"]
        ads.append({"name": a["name"], "rx": rx_d,
                    "per_milli": (round(1000 * (1 - rx_d / tx_d))
                                  if tx_d > 0 else None),
                    "rssi": a.get("rssi_mean")})
    best = max((a["rssi"] for a in ads if a["rssi"] is not None),
               default=None)
    worst_per = max((a["per_milli"] for a in ads
                     if a["per_milli"] is not None), default=None)
    return {"tx": tx_d, "adapters": ads, "rssi": best,
            "per_milli": worst_per}


def steer(args, dbm_guess, slope):
    """Adjust power until ground RSSI lands in the target band. Returns
    (dbm, probe measurement, updated slope estimate)."""
    dbm = max(args.min_dbm, min(args.max_dbm, dbm_guess))
    last = None
    for _ in range(args.steer_tries):
        craft_txpower(args.craft, args.ifname, dbm)
        time.sleep(args.settle)
        m = measure(args, args.probe_dwell)
        if m["rssi"] is None:
            raise RuntimeError("no RSSI from ground during steer")
        err = args.target_rssi - m["rssi"]
        if last is not None and last[0] != dbm and m["rssi"] != last[1]:
            slope = max(0.3, min(2.0,
                        (m["rssi"] - last[1]) / (dbm - last[0])))
        last = (dbm, m["rssi"])
        if abs(err) <= args.rssi_tol:
            return dbm, m, slope
        nxt = max(args.min_dbm, min(args.max_dbm,
                                    round(dbm + err / slope)))
        if nxt == dbm:  # power-limited; take what we can get
            return dbm, m, slope
        dbm = nxt
    return dbm, m, slope


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--craft", default="root@192.168.2.232")
    ap.add_argument("--ifname", default="wlan0")
    ap.add_argument("--ground", default="http://127.0.0.1:8092/api/v1/stats")
    ap.add_argument("--rungs", default="0,1,2,3,4,5,6,7")
    ap.add_argument("--levels", default="4,4,3,3,2,2,1,1",
                    help="profile-table tx_power_level per MCS 0..7")
    ap.add_argument("--target-rssi", type=int, default=-32)
    ap.add_argument("--rssi-tol", type=int, default=3)
    ap.add_argument("--per-ok-milli", type=int, default=15)
    ap.add_argument("--per-bad-milli", type=int, default=50)
    ap.add_argument("--ceil-step", type=int, default=4)
    ap.add_argument("--min-dbm", type=int, default=1)
    ap.add_argument("--max-dbm", type=int, default=27)
    ap.add_argument("--probe-dwell", type=float, default=8.0)
    ap.add_argument("--verify-dwell", type=float, default=15.0)
    ap.add_argument("--settle", type=float, default=4.0)
    ap.add_argument("--steer-tries", type=int, default=4)
    ap.add_argument("--restore-profile", default="0,2")
    ap.add_argument("--restore-dbm", type=int, default=1)
    ap.add_argument("--curve-out", default="curve.authored.txt")
    ap.add_argument("--report-out", default="curve.report.json")
    args = ap.parse_args()

    rungs = [int(x) for x in args.rungs.split(",")]
    levels = [int(x) for x in args.levels.split(",")]
    rlo, rhi = (int(x) for x in args.restore_profile.split(","))

    slope = 0.85  # dB RSSI per dB TX, refined online
    dbm_guess = args.min_dbm + round(
        (args.target_rssi + 41) / slope)  # seed from the measured floor map
    report = {"target_rssi": args.target_rssi, "rungs": {}}
    try:
        for r in rungs:
            craft_pin(args.craft, r, r)
            time.sleep(args.settle)
            dbm, probe, slope = steer(args, dbm_guess, slope)
            dbm_guess = dbm  # rungs share the PA; start neighbors nearby
            v = measure(args, args.verify_dwell)
            entry = {"placement_dbm": dbm, "rssi": v["rssi"],
                     "per_milli": v["per_milli"],
                     "per_adapter": v["adapters"],
                     "placement_ok": (v["per_milli"] is not None and
                                      v["per_milli"] <= args.per_ok_milli)}
            print(f"MCS{r}: placed {dbm} dBm rssi {v['rssi']} "
                  f"PER {v['per_milli']}‰ ok={entry['placement_ok']}",
                  flush=True)
            # Ceiling probe: walk up until it breaks or the cap.
            last_clean, first_bad = (v["rssi"], v["per_milli"]), None
            p = dbm
            while p < args.max_dbm:
                p = min(args.max_dbm, p + args.ceil_step)
                craft_txpower(args.craft, args.ifname, p)
                time.sleep(args.settle)
                m = measure(args, args.probe_dwell)
                print(f"  ceil probe {p} dBm rssi {m['rssi']} "
                      f"PER {m['per_milli']}‰", flush=True)
                if m["per_milli"] is not None and \
                        m["per_milli"] > args.per_bad_milli:
                    first_bad = (m["rssi"], m["per_milli"])
                    break
                last_clean = (m["rssi"], m["per_milli"])
            entry["ceiling_last_clean_rssi"] = last_clean[0]
            entry["ceiling_first_bad_rssi"] = (first_bad[0] if first_bad
                                               else None)  # None = cap-clean
            report["rungs"][r] = entry
            craft_txpower(args.craft, args.ifname, dbm)  # back off the edge
    finally:
        try:
            craft_pin(args.craft, rlo, rhi)
        except Exception as e:  # noqa: BLE001
            print(f"RESTORE profile failed: {e}", file=sys.stderr)
        try:
            craft_txpower(args.craft, args.ifname, args.restore_dbm)
        except Exception as e:  # noqa: BLE001
            print(f"RESTORE txpower failed: {e}", file=sys.stderr)

    with open(args.report_out, "w") as f:
        json.dump(report, f, indent=1)

    # Author the level-4 baseline: runtime resolve applies
    # (level-4)*8 qdb = (level-4)*2 dB, so bake the inverse in. Rungs that
    # were not calibrated inherit their nearest calibrated neighbor.
    base = {}
    for r in range(8):
        src = min(report["rungs"], key=lambda k: abs(int(k) - r)) \
            if r not in report["rungs"] else r
        e = report["rungs"][src]
        base[r] = e["placement_dbm"] - (levels[r] - 4) * 2
    vals = [base[0]] * 12 + [base[m] for m in range(8)]  # legacy idx 0-11
    with open(args.curve_out, "w") as f:
        f.write("#[v2][Exact]#\n#[5G]A\n")
        for i in range(0, 20, 4):
            row = "  ".join(f"{vals[i + j]:.1f}" for j in range(4))
            f.write(f"[1]  0xc20  0xffffffff  {row}\n")
        f.write("0xffff\n")
    print(f"\ncurve -> {args.curve_out}\nreport -> {args.report_out}")


if __name__ == "__main__":
    main()
