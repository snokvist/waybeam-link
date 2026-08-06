#!/usr/bin/env bash
# G4 device verification, take 2 — all three legs in one run.
#
# Take 1 could only ever show two of the three: hopping the craft to an empty
# channel does not silence it on a bench where the peer is a metre away and
# bleeds across 60 MHz. So silence the peer instead, and keep the retune as a
# same-channel no-op whose only job is to arm the §11.6 guard.
#
#   1. both up on 5805, matching net_id     -> craft hears the ground's 1 Hz beat
#   2. ground stopped                       -> craft RX genuinely flat
#   3. POST /api/v1/channel {5805}          -> arms the guard; flat for
#                                              rx_liveness_ms -> recover()
#   4. ground restarted                     -> craft must hear it again, which
#                                              is the actual claim: a restarted
#                                              RX loop still delivers
set -u
SP="$(cd "$(dirname "$0")" && pwd)"
REPO=/home/snokvist/dev/waybeam-coordination/waybeam-link
craft() { curl -s --max-time 5 localhost:8091/api/v1/stats; }
rx_of() { python3 -c 'import json,sys; print(sum(a["rx"] for a in json.load(sys.stdin)["adapters"]))'; }
ground_up() {
    (cd "$REPO" && sudo setsid "$SP/wl-g4" rx -c "$SP/rx.json" \
        >/dev/null 2>>"$SP/g4-ground.log" </dev/null &)
}
window() { local a b; a=$(craft | rx_of); sleep "$2"; b=$(craft | rx_of)
           echo "  $1: craft heard +$((b-a))"; }

cd "$REPO"
ground_up; sleep 7
sudo setsid "$SP/wl-g4" tx -c "$SP/tx-g4.json" >/dev/null 2>"$SP/g4-craft.log" </dev/null &
disown; sleep 8

window "1. peer up, before      " 6
sudo pkill -TERM -f 'wl-g4 rx -c'; sleep 3
window "2. peer stopped         " 6
curl -s --max-time 5 -X POST localhost:8091/api/v1/channel -d '{"mhz":5805}' >/dev/null
sleep 9
echo "  3. guard:"; grep -E "RX SILENT|recovery" "$SP/g4-craft.log" | tail -2 | sed 's/^/       /'
ground_up; sleep 8
window "4. peer back, after     " 6
