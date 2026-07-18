#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Bounded stdin -> file rotator for the vehicle-side `waybeam-link tx` stats
# stream.  The daemon emits one stats JSON line per stats.hz tick (5 Hz by
# default) to stdout with no size limit; when a bench pipes that stdout into a
# plain `> file` redirect on the ~45 MB SSC338Q tmpfs, a continuous run fills
# /tmp and wedges the vehicle.
#
# This helper *owns* the output file (it is the process holding the write fd),
# so it can truncate cleanly on rollover.  That is the crucial difference from
# a background trimmer racing against a redirected writer: truncating a file
# another process opened with `>` (no O_APPEND) leaves that writer's fd offset
# untouched, re-growing the file as a sparse hole and corrupting later reads.
# Here every line is appended with a fresh `>>` (O_APPEND) open, and rollover
# is a real truncate of the same inode we write, so the file stays <= cap with
# no holes and no fd skew.
#
# Usage: wblink-stats-rotate.sh <file> [cap-bytes]
# On rollover the file is truncated to empty (oldest data dropped); a bench
# only reads back the tail after a bounded run, so a generous cap keeps a full
# short run intact while bounding an unbounded continuous run.

set -eu

f=${1:?usage: wblink-stats-rotate.sh <file> [cap-bytes]}
cap=${2:-16777216}   # 16 MiB default

: >"$f"
sz=0
n=0

# IFS= / -r keep JSON lines byte-exact; the ${#line}+1 byte count avoids an
# extra stat() per line (we only stat implicitly via the periodic check).
while IFS= read -r line; do
	printf '%s\n' "$line" >>"$f"
	sz=$((sz + ${#line} + 1))
	n=$((n + 1))
	# Check every 100 lines (~20 s at 5 Hz) rather than per-line.
	if [ $((n % 100)) -eq 0 ] && [ "$sz" -gt "$cap" ]; then
		: >"$f"
		sz=0
	fi
done
