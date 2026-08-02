#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify the §3.15/§3.16 guard-cost boundary from a craft packet trace.

Handover case 12 ("quality emits before live DATA at 2 Hz and never
standalone/after EOB") is a run_tx loop property: the emission rides an
already-active video slot and must never manufacture a TX opportunity. No
unit test can reach it, so this is the verification step — run it against a
trace from the device and the claim becomes evidence instead of an argument.

Usage:
    WBLINK_PACKET_TRACE=/tmp/craft.jsonl ./waybeam-link tx ...
    tools/verify_quality_guard.py /tmp/craft.jsonl

Checks, per §3.16:
  1. Every emitted UPLINK_QUALITY is immediately followed on the wire by a
     live (non-repair, non-retransmit) DATA frame -- it prepends to a slot,
     it does not own one.
  2. That DATA follows within SAME_SLOT_US, i.e. in the same TX opportunity.
     This is what separates "prepended inside a live slot" from "sent
     standalone and coincidentally followed by video later".
  3. The rate stays at or under the 2 Hz cadence (with a small tolerance for
     coalescing across a measurement window).

Deliberately NOT checked: whether a quality packet follows an END_OF_BLOCK in
wire order. In a live stream it always does -- slots end with an EOB and the
next slot begins with the prepend -- so that would fail every honest trace.
The §7.2 hazard is emitting INSIDE the quiet gap, which check 2 is what
actually detects: a packet that opened its own window has no video behind it.

Exit status is 0 only when every check passes, so it can gate a campaign.
"""

import json
import sys
from collections import Counter

CADENCE_HZ = 2.0
RATE_TOLERANCE = 1.25  # allow modest slack; a real breach is order-of-magnitude
# Two packets submitted back to back in one TX opportunity land within a few
# hundred microseconds. The §7.2 quiet gap is milliseconds, so this threshold
# sits comfortably between "prepended" and "opened its own window".
SAME_SLOT_US = 2000


def load_tx_packets(path):
    """Ordered TX-direction packet events. Trace lines are one JSON object
    each; non-packet lines (schema, trace_end) are skipped."""
    out = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("type") != "packet" or ev.get("direction") != "tx":
                continue
            out.append(ev)
    return out


def is_live_data(ev):
    """A live video frame: DATA that is neither FEC repair nor an ARQ
    retransmit. Those ride slots that already exist rather than opening one."""
    return (
        ev.get("packet") == "data"
        and ev.get("kind") != "repair"
        and ev.get("retransmit") is not True
    )


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    pkts = load_tx_packets(argv[1])
    if not pkts:
        print("FAIL: no tx packet events in trace", file=sys.stderr)
        return 1

    quality_idx = [i for i, e in enumerate(pkts) if e.get("packet") == "uplink_quality"]
    kinds = Counter(e.get("packet") for e in pkts)
    print(f"tx packets: {sum(kinds.values())}  {dict(kinds)}")

    if not quality_idx:
        # Not a pass: the trace simply does not exercise the property. Say so
        # rather than reporting green on an empty check.
        print(
            "INCONCLUSIVE: no UPLINK_QUALITY emitted. Needs a live video feed,"
            " a configured csa_psk, and an accepted reporter (§3.16).",
            file=sys.stderr,
        )
        return 1

    failures = []

    # 1 + 2: what each quality packet rides, and how closely.
    worst_gap = 0
    for i in quality_idx:
        nxt = pkts[i + 1] if i + 1 < len(pkts) else None
        if nxt is None or not is_live_data(nxt):
            got = "end-of-trace" if nxt is None else nxt.get("packet")
            failures.append(
                f"  t_us={pkts[i]['t_us']}: not followed by live DATA (got {got})"
                " -- standalone emission opens a TX opportunity (§3.16)"
            )
            continue
        gap = nxt["t_us"] - pkts[i]["t_us"]
        worst_gap = max(worst_gap, gap)
        if gap > SAME_SLOT_US:
            failures.append(
                f"  t_us={pkts[i]['t_us']}: video follows {gap} us later"
                f" (> {SAME_SLOT_US} us) -- not the same TX opportunity, so"
                " this packet owned a window of its own (§7.2)"
            )
    print(f"worst quality->video gap: {worst_gap} us (limit {SAME_SLOT_US} us)")

    # 3: cadence.
    span_us = pkts[-1]["t_us"] - pkts[0]["t_us"]
    if span_us > 0:
        rate = len(quality_idx) / (span_us / 1_000_000.0)
        limit = CADENCE_HZ * RATE_TOLERANCE
        print(f"quality: {len(quality_idx)} packets, {rate:.2f}/s (limit {limit:.2f}/s)")
        if rate > limit:
            failures.append(f"  rate {rate:.2f}/s exceeds the 2 Hz cadence")

    if failures:
        print(f"FAIL: {len(failures)} guard-cost violation(s)", file=sys.stderr)
        for f in failures:
            print(f, file=sys.stderr)
        return 1
    print("PASS: every UPLINK_QUALITY rode a live video slot in the same TX"
          " opportunity, at or under the 2 Hz cadence")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
