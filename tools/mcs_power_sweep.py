#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Bench: MCS-rung x TX-power sweep -> per-dwell wire PER, without bad-FCS.
#
# The Pass-119 B1 probe (docs/per-mcs-per-ladder-plan.md par.6) found no fleet
# chip delivers FCS-failed frames, so the PER numerator comes from the other
# end instead: the craft's tx_submitted delta IS the denominator and the
# ground's per-adapter rx delta IS the survivor count -- true wire PER per
# (rung, power) dwell, no FCS access needed. Single-rung policy (par.3.0
# Pass 118) makes the attribution exact: every frame in a dwell was aired at
# the pinned rung.
#
# Drives the live rig from the ground host:
#   - pin rung:   POST craft :8091/api/v1/link/profile {"min":N,"max":N}
#                 (craft control binds 127.0.0.1 -> reached via ssh+curl)
#   - set power:  ssh craft "iw dev <ifname> set txpower fixed <mBm>"
#                 (kernel-monitor backend; the sweep's power axis)
#   - measure:    craft + ground /api/v1/stats snapshots at dwell start/end
#
# Restores profile window and txpower on exit, including on Ctrl-C/failure.
#
# Usage (defaults match the .232/.242 bench):
#   python3 tools/mcs_power_sweep.py --out sweep.jsonl
#   python3 tools/mcs_power_sweep.py --rungs 0,2,4,5,7 \
#       --powers 1,5,10,15,20,27 --dwell 20 --settle 6

import argparse
import json
import subprocess
import sys
import time
import urllib.request


def sh_craft(host, cmd, timeout=10):
    r = subprocess.run(["ssh", "-o", "ConnectTimeout=5", host, cmd],
                       capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"ssh {cmd!r}: rc={r.returncode} {r.stderr.strip()}")
    return r.stdout


def craft_stats(host):
    out = sh_craft(host, "curl -s -m3 http://127.0.0.1:8091/api/v1/stats")
    return json.loads(out)


def craft_pin(host, lo, hi):
    body = json.dumps({"min": lo, "max": hi})
    out = sh_craft(host,
                   "curl -s -m3 -X POST http://127.0.0.1:8091/api/v1/link/profile"
                   f" -H 'Content-Type: application/json' -d '{body}'")
    if '"ok":true' not in out.replace(" ", ""):
        raise RuntimeError(f"profile pin {lo}..{hi} refused: {out.strip()}")


def craft_txpower(host, ifname, dbm):
    sh_craft(host, f"iw dev {ifname} set txpower fixed {dbm * 100}")


def ground_stats(url):
    with urllib.request.urlopen(url, timeout=3) as f:
        return json.loads(f.read())


def tx_total(stats):
    return sum(a.get("tx_submitted", 0) for a in stats.get("adapters", []))


def craft_iw_dbm(host, ifname):
    try:
        out = sh_craft(host, f"iw dev {ifname} info | grep txpower")
        return out.split("txpower")[1].strip().split()[0]
    except Exception:  # noqa: BLE001 - observability only
        return None


def dwell_row(args, rung, dbm):
    c0, g0 = craft_stats(args.craft), ground_stats(args.ground)
    time.sleep(args.dwell)
    c1, g1 = craft_stats(args.craft), ground_stats(args.ground)

    tx_d = tx_total(c1) - tx_total(c0)
    row = {"rung": rung, "tx_dbm": dbm, "dwell_s": args.dwell,
           "tx_frames": tx_d, "adapters": [], "streams": [],
           "craft_link": {
               "tx_power_qdb": c1.get("link", {}).get("tx_power_qdb"),
               "iw_dbm": craft_iw_dbm(args.craft, args.ifname)}}
    prev = {a["name"]: a for a in g0.get("adapters", [])}
    for a in g1.get("adapters", []):
        p = prev.get(a["name"])
        if p is None:
            continue
        rx_d = a["rx"] - p["rx"]
        mcs_d = [x - y for x, y in zip(a.get("rx_mcs", []),
                                       p.get("rx_mcs", []))]
        row["adapters"].append({
            "name": a["name"], "rx_frames": rx_d,
            "per_milli": (round(1000 * (1 - rx_d / tx_d)) if tx_d > 0 else None),
            "rssi_mean": a.get("rssi_mean"), "rssi_best": a.get("rssi_best"),
            "rx_mcs_delta": mcs_d,
            "rx_mcs_unknown_delta": (a.get("rx_mcs_unknown", 0) -
                                     p.get("rx_mcs_unknown", 0)),
        })
    sprev = {s["stream_id"]: s for s in g0.get("streams", [])}
    for s in g1.get("streams", []):
        p = sprev.get(s["stream_id"])
        if p is None:
            continue
        row["streams"].append({
            "stream_id": s["stream_id"],
            "delivered_delta": s.get("delivered", 0) - p.get("delivered", 0),
            "loss_prediversity_milli": s.get("loss_prediversity_milli"),
            "loss_postdiv_prearq_milli": s.get("loss_postdiv_prearq_milli"),
        })
    link = g1.get("link", {})
    row["ground_link"] = {"mcs": link.get("mcs"), "state": link.get("state")}
    return row


def fmt_row(row):
    ads = " | ".join(
        f"{a['name']}: PER {a['per_milli'] if a['per_milli'] is not None else '?'}‰ "
        f"rssi {a['rssi_mean']} rx {a['rx_frames']}"
        for a in row["adapters"])
    pwr = (f"{row['tx_dbm']:>2} dBm" if row["tx_dbm"] != "auto" else
           f"auto({row['craft_link'].get('iw_dbm')} dBm)")
    return f"rung {row['rung']} @ {pwr}  tx {row['tx_frames']:>6}  {ads}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--craft", default="root@192.168.2.232")
    ap.add_argument("--ifname", default="wlan0")
    ap.add_argument("--ground", default="http://127.0.0.1:8092/api/v1/stats")
    ap.add_argument("--rungs", default="0,2,4,5,7")
    ap.add_argument("--powers", default="1,5,10,15,20,27",
                    help="craft TX dBm steps, swept low->high per rung")
    ap.add_argument("--dwell", type=float, default=20.0)
    ap.add_argument("--settle", type=float, default=6.0,
                    help="seconds after a pin/power change before measuring")
    ap.add_argument("--restore-profile", default="0,2",
                    help="min,max profile window restored on exit")
    ap.add_argument("--restore-dbm", type=int, default=1)
    ap.add_argument("--out", default="mcs_power_sweep.jsonl")
    args = ap.parse_args()

    rungs = [int(x) for x in args.rungs.split(",")]
    # "auto" = don't touch txpower; the craft's own §10.2 curve resolve (or
    # whatever is on the hardware) IS the power axis. One dwell per rung.
    powers = ([("auto")] if args.powers == "auto" else
              [int(x) for x in args.powers.split(",")])
    rlo, rhi = (int(x) for x in args.restore_profile.split(","))

    rows = []
    try:
        with open(args.out, "w") as f:
            for rung in rungs:
                craft_pin(args.craft, rung, rung)
                for dbm in powers:
                    if dbm != "auto":
                        craft_txpower(args.craft, args.ifname, dbm)
                    time.sleep(args.settle)
                    row = dwell_row(args, rung, dbm)
                    rows.append(row)
                    print(fmt_row(row), flush=True)
                    f.write(json.dumps(row) + "\n")
                    f.flush()
    finally:
        # Restore the operating point no matter how we exit.
        try:
            craft_pin(args.craft, rlo, rhi)
        except Exception as e:  # noqa: BLE001 - report, keep restoring
            print(f"RESTORE profile failed: {e}", file=sys.stderr)
        try:
            craft_txpower(args.craft, args.ifname, args.restore_dbm)
        except Exception as e:  # noqa: BLE001
            print(f"RESTORE txpower failed: {e}", file=sys.stderr)

    print(f"\n{len(rows)} dwells -> {args.out}")


if __name__ == "__main__":
    main()
