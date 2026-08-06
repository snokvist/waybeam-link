#!/usr/bin/env bash
# G1 device A/B — does a §15.5a scout sweep widen the §3.0 net_id filter on a
# devourer node?
#
# Two devourer nodes on this host, deliberately mismatched net_ids:
#   TX  8812CU (bus 5-1)  net_id 7, originator 17 — announces at 2 Hz
#   RX  8822EU (bus 1-1)  net_id 3, originator  9 — sweeps 5805
#
# A sweep is specified to hear ALL net_ids for its duration (ScoutEngine::start
# calls set_filter(nullopt)). If the setter is a no-op the RX node keeps its
# configured filter of 3 and never sees the craft, while the sweep still
# reports clean — the silent failure this closes.
#
# Success: wl-after discovers originator 17 at net_id 7; wl-before does not.
# The 8822EU is used as the EAR only: it is the H1 suspect unit whose 64-QAM
# TX is unreliable, and nothing here depends on its transmit path.
set -u
#
# Build the two arms first (the binaries are NOT committed):
#   git checkout main                 && cmake --build --preset x86-ground \
#     && cp build/x86-ground/waybeam-link <thisdir>/wl-before
#   git checkout <the fix>            && cmake --build --preset x86-ground \
#     && cp build/x86-ground/waybeam-link <thisdir>/wl-after

SP="$(cd "$(dirname "$0")" && pwd)"
REPO=/home/snokvist/dev/waybeam-coordination/waybeam-link
DWELL=${DWELL:-1500}

cleanup() {
    sudo pkill -TERM -f 'wl-(before|after) (tx|rx) -c' 2>/dev/null
    sleep 2
}
trap cleanup EXIT

run_arm() {
    local bin="$1" label="$2"
    cleanup
    (cd "$REPO" && sudo setsid "$SP/$bin" tx -c "$SP/tx.json" \
        >/dev/null 2>"$SP/$label-tx.log" </dev/null &)
    sleep 6
    (cd "$REPO" && sudo setsid "$SP/$bin" rx -c "$SP/rx.json" \
        >/dev/null 2>"$SP/$label-rx.log" </dev/null &)
    sleep 6
    for role in tx rx; do
        ps -eo args | grep -q "[w]l-.* $role -c" || {
            echo "  $label: $role failed to start"; tail -6 "$SP/$label-$role.log"; return 1; }
    done
    curl -s -X POST localhost:8099/api/v1/scout/start \
        -d "{\"channels\":[5805],\"dwell_ms\":$DWELL}" >/dev/null
    sleep 8
    local res
    res=$(curl -s localhost:8099/api/v1/scout/results)
    curl -s -X POST localhost:8099/api/v1/scout/stop >/dev/null
    LABEL="$label" python3 - "$res" <<'PY'
import json, os, sys
try:
    r = json.loads(sys.argv[1])
except Exception:
    print(f"  {os.environ['LABEL']}: no results ({sys.argv[1][:80]})"); raise SystemExit
cands = r.get("results") or r.get("candidates") or []
hit = [c for c in cands if c.get("originator") == 17]
print(f"  {os.environ['LABEL']:6}  candidates={len(cands)}  "
      f"craft(orig=17,net_id=7) {'FOUND ' + str(hit[0].get('net_id')) if hit else 'NOT FOUND'}")
if cands and not hit:
    print(f"          saw: {[(c.get('originator'), c.get('net_id')) for c in cands]}")
PY
    cleanup
}

sudo systemctl stop waybeam-ground 2>/dev/null
sleep 1
for d in rtl88x2cu:5-1:1.0 rtl88x2eu:1-1:1.0; do
    drv=${d%%:*}; port=${d#*:}
    echo "$port" | sudo tee "/sys/bus/usb/drivers/$drv/unbind" >/dev/null 2>&1
done
sleep 2
ls /sys/class/net | grep -qE 'wlx' && echo "WARN: a netdev is still bound" || echo "both adapters detached from the kernel"

for arm in before after; do
    run_arm "wl-$arm" "$arm"
done
