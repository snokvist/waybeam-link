#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""D5 on hardware: decode LINK_REPORT report_epoch straight off the air.

§3.5 says `report_epoch` advances once per EMITTED report, and §10.7's loss
identity divides by the craft's delta of exactly this field. Pass 126 moved the
increment from build-time to the radio call so a report the radio never took
burns no number.

What that must look like on the air, from a single observer:

  * strictly increasing (never repeats except in a §7.2 redundancy copy, which
    is a byte-identical repeat of the SAME frame and so carries the SAME epoch)
  * gaps are allowed and expected — this observer is not the craft and simply
    misses frames — but a gap must not be systematic
  * a redundancy copy repeats an epoch rather than consuming a new one

Reads `tcpdump -e -nn -x` on stdin. LINK_REPORT is type 3, 39 bytes, with
report_epoch at §3.5 wire offset 18 (pinned by encode_link_report).

tcpdump parses our 3-byte header (magic 'WB' + ver_type) as an 802.2 LLC
dsap/ssap/ctrl and dumps only what follows, so the hex starts at waybeam
offset 3 and the frame type is the "ctrl 0x.." field on the text line.
"""
import collections
import re
import sys

HEXLINE = re.compile(r"^\s+0x([0-9a-f]{4}):\s+((?:[0-9a-f]{4}\s*)+)")
CTRL = re.compile(r"ctrl 0x([0-9a-f]{2})")
PAYLOAD_OFF = 3   # bytes of ours that tcpdump ate as LLC
EPOCH_OFF = 18    # §3.5 report_epoch


def frames(stream):
    """Yield (ver_type, payload) with payload starting at waybeam offset 3."""
    cur, vt = None, None
    for line in stream:
        m = HEXLINE.match(line)
        if m:
            off = int(m.group(1), 16)
            data = bytes.fromhex(m.group(2).replace(" ", ""))
            if off == 0:
                if cur is not None:
                    yield vt, cur
                cur = bytearray(data)
            elif cur is not None:
                cur.extend(data)
        else:
            c = CTRL.search(line)
            if c:
                if cur is not None:
                    yield vt, cur
                    cur = None
                vt = int(c.group(1), 16)
    if cur is not None:
        yield vt, cur


def main():
    epochs = []
    for vt, p in frames(sys.stdin):
        if vt is None or (vt & 0x0F) != 3:   # LINK_REPORT
            continue
        if len(p) + PAYLOAD_OFF < 39:
            continue
        orig = int.from_bytes(p[3 - PAYLOAD_OFF:5 - PAYLOAD_OFF], "big")
        e0 = EPOCH_OFF - PAYLOAD_OFF
        epoch = int.from_bytes(p[e0:e0 + 4], "big")
        epochs.append((orig, epoch))

    if not epochs:
        print("  INCONCLUSIVE: no LINK_REPORT frames decoded")
        return 1

    by_orig = collections.defaultdict(list)
    for o, e in epochs:
        by_orig[o].append(e)

    rc = 0
    for orig, seq in sorted(by_orig.items()):
        uniq = sorted(set(seq))
        span = uniq[-1] - uniq[0] + 1
        dups = len(seq) - len(uniq)
        backward = sum(1 for a, b in zip(seq, seq[1:]) if b < a)
        deltas = collections.Counter(
            b - a for a, b in zip(uniq, uniq[1:]))
        print(f"  originator {orig}: {len(seq)} reports, "
              f"{len(uniq)} distinct epochs")
        print(f"    range {uniq[0]}..{uniq[-1]} (span {span}), "
              f"observed {len(uniq)} = {100*len(uniq)/span:.1f}% of the span")
        print(f"    repeats (§7.2 redundancy copies): {dups}")
        print(f"    backward steps: {backward}")
        top = ", ".join(f"+{d}x{n}" for d, n in deltas.most_common(5))
        print(f"    epoch deltas between distinct reports: {top}")
        if backward:
            print("    FAIL: report_epoch went BACKWARD — not monotonic")
            rc = 1
        # A stamped-but-never-sent epoch shows up as a permanent hole. This
        # observer drops frames too, so a hole is only evidence in aggregate:
        # flag when the observer sees a high delivery rate yet the epoch span
        # runs far ahead of the reports actually observed.
        if len(uniq) >= 50 and len(uniq) / span < 0.5:
            print(f"    NOTE: only {100*len(uniq)/span:.0f}% of the epoch span "
                  f"was observed — either this observer is lossy or epochs "
                  f"are being burned without transmission (D5)")
    return rc


if __name__ == "__main__":
    sys.exit(main())
