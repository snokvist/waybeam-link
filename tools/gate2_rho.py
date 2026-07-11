#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Gate-2 (§17) windowed cross-adapter loss correlation, ground-side only.

Feed it the ground node's stats JSONL (2+ adapters). Windows self-align on
the ground's own view of the craft seq (streams[0].seq), so craft/ground
start-time skew cannot contaminate the series. Per active window:

  per-adapter loss  = 1 - (adapter rx delta / seq delta)
  joint pre-ARQ loss = 1 - (uniq delta / seq delta)   [post-diversity]

Reports mean + P95 per adapter, the joint loss vs the independence
prediction (product of per-adapter means), and Pearson rho of the two
per-adapter miss-rate series. Gate: joint P95 << single-adapter P95.

Bench validation (2026-07-11, x86 + 8812AU/8812CU, air.rx_drop_permille=150):
measured per-adapter 15.3%/14.0%, joint 2.10% vs independence 2.07%,
rho -0.36 (n=59) -- machinery exact on known-independent loss.

Usage: gate2_rho.py <ground-stats.jsonl> [min-seq-delta-per-window=30]
"""
import json
import statistics
import sys


def main(path, min_dseq=30):
    rows = [json.loads(l) for l in open(path) if l.strip().startswith("{")]
    rows = [r for r in rows if r.get("streams")]
    if len(rows) < 10:
        print("not enough stats lines with a latched stream")
        return 1
    names = [a["name"] for a in rows[0]["adapters"]]
    win = []
    for i in range(1, len(rows)):
        dseq = rows[i]["streams"][0]["seq"] - rows[i - 1]["streams"][0]["seq"]
        if dseq < min_dseq:
            continue
        m = {}
        for a0, a1 in zip(rows[i - 1]["adapters"], rows[i]["adapters"]):
            m[a1["name"]] = max(0, dseq - (a1["rx"] - a0["rx"]))
        duniq = rows[i]["streams"][0]["uniq"] - rows[i - 1]["streams"][0]["uniq"]
        win.append((dseq, m, max(0, dseq - duniq)))
    if len(win) < 5:
        print("not enough active windows")
        return 1

    def p95(v):
        return sorted(v)[int(0.95 * len(v))]

    series = {n: [w[1][n] / w[0] for w in win] for n in names}
    joint = [w[2] / w[0] for w in win]
    print(f"active windows: {len(win)}")
    for n in names:
        print(f"loss {n}: mean {statistics.mean(series[n]):.3f}  "
              f"P95 {p95(series[n]):.3f}")
    print(f"joint post-div pre-ARQ: mean {statistics.mean(joint):.4f}  "
          f"P95 {p95(joint):.4f}")
    indep = 1.0
    for n in names:
        indep *= statistics.mean(series[n])
    print(f"independence predicts (mean product): {indep:.4f}")
    if len(names) == 2:
        a, b = names
        if len(set(series[a])) > 1 and len(set(series[b])) > 1:
            print(f"pearson rho: "
                  f"{statistics.correlation(series[a], series[b]):+.3f}")
    last = rows[-1]["streams"][0]
    print(f"totals: uniq={last['uniq']} diversity={last['diversity']} "
          f"nacks={last['nacks_sent']} recovered_arq={last['recovered_arq']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1],
                  int(sys.argv[2]) if len(sys.argv) > 2 else 30))
