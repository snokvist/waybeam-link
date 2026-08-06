#!/usr/bin/env bash
# Pass 144 — does a claim taken mid-sweep survive the sweep?
#
# do_claim never stopped the scout. Left running, the sweep finishes seconds
# later and its rest() restores the resting channel and net_id filter over the
# claim, with the campaign already in flight.
#
# Sweep with a long dwell so it is still running when the claim lands, then
# read the state twice: right after the claim, and again after the sweep would
# have finished. The second read is the one that matters.
set -u
SP="$(cd "$(dirname "$0")" && pwd)"
REPO=/home/snokvist/dev/waybeam-coordination/waybeam-link
G=localhost:8099

state() {  # label
    local sc sel
    sc=$(curl -s --max-time 5 $G/api/v1/scout/results)
    sel=$(curl -s --max-time 5 $G/api/v1/link/selection)
    LABEL="$1" python3 - "$sc" "$sel" <<'PY'
import json, os, sys
sc, sel = (json.loads(x) for x in sys.argv[1:3])
print("  %-26s scanning=%-5s  selection: originator=%s chan=%s net_id=%s state=%s"
      % (os.environ["LABEL"], str(sc.get("scanning")),
         sel.get("originator"), sel.get("chan"), sel.get("net_id"),
         sel.get("state")))
PY
}

for arm in "$@"; do
    echo "--- $arm ---"
    sudo pkill -TERM -f 'wl-(scoped|claimstop) (tx|rx) -c' 2>/dev/null; sleep 3
    cd "$REPO"
    sudo setsid "$SP/wl-$arm" tx -c "$SP/tx.json" >/dev/null 2>"$SP/c-$arm-tx.log" </dev/null &
    disown; sleep 6
    sudo setsid "$SP/wl-$arm" rx -c "$SP/rx.json" >/dev/null 2>"$SP/c-$arm-rx.log" </dev/null &
    disown; sleep 8

    # Candidates only exist once a channel's dwell FINALIZES, so a
    # single-channel sweep leaves almost no window. Sweep three channels with
    # the craft on the middle one: by the time channel 3 is dwelling, the
    # craft is claimable and the sweep is still running.
    curl -s --max-time 5 -X POST $G/api/v1/scout/start \
        -d '{"channels":[5745,5805,5825],"dwell_ms":6000}' >/dev/null
    sleep 14
    state "mid-sweep, pre-claim"
    r=$(curl -s --max-time 8 -X POST $G/api/v1/scout/quickconnect \
        -d '{"originator":17,"target_chan":5805}')
    echo "       claim -> ${r:0:90}"
    sleep 2
    state "just after claim"
    sleep 12   # the third dwell expires and the sweep rests
    state "after sweep would end"
done
sudo pkill -TERM -f 'wl-(scoped|claimstop) (tx|rx) -c' 2>/dev/null
