#!/usr/bin/env bash
# G1 control arm — the widen must be temporary.
#
# A filter that stayed open after the sweep would pass this repo's discovery
# test and still be a regression: the node would accept every net_id's traffic
# forever. Same two mismatched nodes as run_g1.sh (TX net_id 7, RX pinned 3),
# sampling the RX ear's counters in three windows:
#
#   idle    — before any sweep: frames must land in rx_filtered, not rx_frames
#   sweep   — filter wide: rx_frames advances
#   after   — sweep stopped: back to rx_filtered only
set -u

SP="$(cd "$(dirname "$0")" && pwd)"
REPO=/home/snokvist/dev/waybeam-coordination/waybeam-link

cleanup() { sudo pkill -TERM -f 'wl-(before|after) (tx|rx) -c' 2>/dev/null; sleep 2; }
trap cleanup EXIT
cleanup

(cd "$REPO" && sudo setsid "$SP/wl-after" tx -c "$SP/tx.json" >/dev/null 2>"$SP/r-tx.log" </dev/null &)
sleep 6
(cd "$REPO" && sudo setsid "$SP/wl-after" rx -c "$SP/rx-anynet.json" >/dev/null 2>"$SP/r-rx.log" </dev/null &)
sleep 6

sample() { curl -s localhost:8099/api/v1/stats; }

window() {  # label, seconds, body
    local label="$1" secs="$2"
    local a b
    a=$(sample); sleep "$secs"; b=$(sample)
    LABEL="$label" python3 - "$a" "$b" <<'PY'
import json, os, sys
a, b = (json.loads(x) for x in sys.argv[1:3])
def s(d, k): return sum(x.get(k, 0) for x in d["adapters"])
print(f"  {os.environ['LABEL']:8} rx_frames +{s(b,'rx')-s(a,'rx'):5d}   "
      f"filtered  +{s(b,'filtered')-s(a,'filtered'):5d}")
PY
}

window idle 6
curl -s -X POST localhost:8099/api/v1/scout/start -d '{"channels":[5805],"dwell_ms":6000}' >/dev/null
window sweep 6
curl -s -X POST localhost:8099/api/v1/scout/stop >/dev/null
sleep 1
window restored 6
