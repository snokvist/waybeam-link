#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Gate-3 (§17) NACK→RETRANSMIT latency report from ground stats JSONL.

Reads the run's first and last stats lines with a latched stream and
diffs the cumulative histograms, so a long-running node can be analyzed
over just the captured segment. Two distributions per the §17 estimator:

  nack_rtt  most-recent-NACK → RETRANSMIT arrival (pure link round-trip,
            the §5 freshness-gate input)
  arq_rec   first-NACK → arrival (recovery latency — THE gate-3 number,
            compare its P90 against the I-frame deadline)

Buckets are ms upper bounds 1,2,4,8,16,32,64,+inf; percentiles resolve to
a bucket's upper bound (histogram resolution is the estimator's stated
precision). Pass the I-frame deadline to get the verdict line.

Usage: gate3_rtt.py <ground-stats.jsonl> [iframe-deadline-ms=50]
"""
import json
import sys

BOUNDS = [1, 2, 4, 8, 16, 32, 64, None]  # None = +inf


def label(i):
    return f"<={BOUNDS[i]}ms" if BOUNDS[i] else ">64ms"


def pct(hist, q):
    total = sum(hist)
    if total == 0:
        return None
    acc = 0
    for i, n in enumerate(hist):
        acc += n
        if acc >= q * total:
            return i
    return len(hist) - 1


def report(name, hist, max_ms, deadline_ms):
    total = sum(hist)
    print(f"{name}: {total} samples, max {max_ms} ms")
    if total == 0:
        return
    print("  " + "  ".join(f"{label(i)}:{n}" for i, n in enumerate(hist)
                           if n))
    for q in (0.5, 0.9):
        i = pct(hist, q)
        print(f"  P{int(q * 100)} {label(i)}")
    if deadline_ms is not None:
        in_deadline = sum(n for i, n in enumerate(hist)
                          if BOUNDS[i] and BOUNDS[i] <= deadline_ms)
        # Buckets straddling the deadline are indeterminate at histogram
        # resolution — count them separately, not as passes.
        print(f"  vs {deadline_ms} ms deadline: {in_deadline}/{total} "
              f"provably inside ({100 * in_deadline / total:.1f}%)")


def main(path, deadline_ms=50):
    rows = [json.loads(l) for l in open(path) if l.strip().startswith("{")]
    rows = [r for r in rows if r.get("streams")]
    if len(rows) < 2:
        print("not enough stats lines with a latched stream")
        return 1
    first, last = rows[0]["streams"][0], rows[-1]["streams"][0]
    for key, mx in (("nack_rtt", "nack_rtt_max_ms"),
                    ("arq_rec", "arq_rec_max_ms")):
        hist = [b - a for a, b in zip(first[f"{key}_hist"],
                                      last[f"{key}_hist"])]
        report(key, hist, last[mx], deadline_ms)
    print(f"totals: nacks={last['nacks_sent']} "
          f"recovered_arq={last['recovered_arq']} "
          f"dropped_deadline={last['dropped_deadline']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1],
                  int(sys.argv[2]) if len(sys.argv) > 2 else 50))
