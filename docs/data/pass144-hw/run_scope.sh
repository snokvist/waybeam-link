#!/usr/bin/env bash
# Pass 144 — does a sweep let a foreign-net_id craft into the §2 selector?
#
# Same mismatched pair as the G1 test: craft net_id 7, ground pinned to 3. The
# sweep widens node-wide, so the ground hears the craft. The question is what
# does with what it hears.
#
#   link.target_originator  — the §2 latch. Must stay 0: this ground is not
#                             paired with that craft and the sweep ends.
#   scout candidates        — must still list it. Scoping the selector must not
#                             cost discovery, which is the point of sweeping.
set -u
SP="$(cd "$(dirname "$0")" && pwd)"
REPO=/home/snokvist/dev/waybeam-coordination/waybeam-link

for arm in wide scoped; do
    sudo pkill -TERM -f 'wl-(wide|scoped) (tx|rx) -c' 2>/dev/null
    sleep 3
    cd "$REPO"
    sudo setsid "$SP/wl-$arm" tx -c "$SP/tx.json" >/dev/null 2>"$SP/s-$arm-tx.log" </dev/null &
    disown; sleep 6
    sudo setsid "$SP/wl-$arm" rx -c "$SP/rx.json" >/dev/null 2>"$SP/s-$arm-rx.log" </dev/null &
    disown; sleep 7

    # A craft with no feed airs only ANNOUNCE/heartbeat, and §2 admission
    # wants DATA — without this the hazard cannot appear in either arm.
    (cd "$REPO" && python3 tools/rtp_feed.py 30 3000 60 >/dev/null 2>&1 &)
    sleep 3
    curl -s --max-time 5 -X POST localhost:8099/api/v1/scout/start \
        -d '{"channels":[5805],"dwell_ms":9000}' >/dev/null
    sleep 9
    st=$(curl -s --max-time 5 localhost:8099/api/v1/stats)
    res=$(curl -s --max-time 5 localhost:8099/api/v1/scout/results)
    curl -s --max-time 5 -X POST localhost:8099/api/v1/scout/stop >/dev/null

    ARM="$arm" python3 - "$st" "$res" <<'PY'
import json, os, sys
st, res = (json.loads(x) for x in sys.argv[1:3])
cands = res.get("results") or res.get("candidates") or []
seen = [c for c in cands if c.get("originator") == 17]
print("  %-7s target_originator=%-4d  scout sees craft: %s"
      % (os.environ["ARM"], st["link"]["target_originator"],
         ("yes (net_id %s)" % seen[0].get("net_id")) if seen else "NO"))
PY
done
sudo pkill -TERM -f 'wl-(wide|scoped) (tx|rx) -c' 2>/dev/null
