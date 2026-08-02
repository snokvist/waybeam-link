#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# P3 — ten consecutive §10.7 uplink calibrations.
#
# Gate: 10/10 complete, placement spread no greater than one seek step.
# Per run the campaign wants: duration, samples/dwell, placement, RSSI, loss,
# bracket, liveness timeouts, report delivery, last_rx_mcs, restore result.
# All of that is already emitted — the dwell trace on stderr and the REST
# response — so this collects rather than re-derives it.
set -u
G=http://127.0.0.1:8092/api/v1
N=${N:-10}
OUT=${OUT:-docs/data/pass126-hw/p3-soak}
mkdir -p "$OUT"

echo "run,secs,state,fail,placement_qdb,rssi,loss,fp,reports,quality_rssi,rx_mcs,radio_dbm" \
    > "$OUT/summary.csv"

for i in $(seq 1 "$N"); do
    # Radio power before the run — the restore check compares against it.
    pre=$(iw dev wlx84fc1450bcde info | sed -n 's/.*txpower \([0-9.]*\).*/\1/p')
    t0=$(date +%s)
    start=$(curl -s --max-time 8 -X POST -d '{"action":"start"}' "$G/calibration")
    case "$start" in
        *'"ok":true'*) ;;
        *) echo "$i,,REFUSED,$start,,,,,,,," >> "$OUT/summary.csv"
           printf 'run %2d  REFUSED: %s\n' "$i" "$start"; sleep 5; continue ;;
    esac

    state=; for _ in $(seq 1 100); do
        r=$(curl -s --max-time 4 "$G/calibration")
        state=$(printf '%s' "$r" | sed -n 's/.*"state":"\([a-z_]*\)".*/\1/p')
        case "$state" in done|failed) break ;; esac
        sleep 3
    done
    secs=$(( $(date +%s) - t0 ))
    printf '%s' "$r" > "$OUT/run-$i.json"

    eval "$(printf '%s' "$r" | python3 -c '
import json,sys
d=json.load(sys.stdin); q=d.get("quality",{}) or {}
a=(d.get("artifact") or {}).get("placements") or [{}]
p=a[0]
def g(v): return "" if v is None else v
print("fail=%s;place=%s;rssi=%s;loss=%s;fp=%s;reports=%s;qrssi=%s;rxmcs=%s" % (
    g(d.get("fail_reason")), g(p.get("placement_qdb")), g(p.get("placement_rssi_dbm")),
    g(p.get("placement_loss_milli")), g(d.get("fingerprint")),
    g(q.get("reports_received")), g(q.get("rssi_mean")), g(q.get("rx_mcs"))))')"

    post=$(iw dev wlx84fc1450bcde info | sed -n 's/.*txpower \([0-9.]*\).*/\1/p')
    echo "$i,$secs,$state,$fail,$place,$rssi,$loss,$fp,$reports,$qrssi,$rxmcs,$post" \
        >> "$OUT/summary.csv"
    printf 'run %2d  %3ds  %-6s place=%-4s rssi=%-4s loss=%-3s fp=%-3s radio %s->%s dBm %s\n' \
        "$i" "$secs" "$state" "${place:--}" "${rssi:--}" "${loss:--}" "${fp:--}" \
        "$pre" "$post" "${fail:+FAIL:$fail}"
    sleep 6
done

echo
python3 - "$OUT/summary.csv" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
done = [r for r in rows if r["state"] == "done"]
pl = sorted({int(r["placement_qdb"]) for r in done if r["placement_qdb"]})
print(f"  completed      {len(done)}/{len(rows)}")
if pl:
    print(f"  placements     {pl}  spread {pl[-1]-pl[0]} qdb "
          f"({(pl[-1]-pl[0])/4:.1f} dB, one step = 16 qdb / 4 dB)")
    print(f"  GATE spread<=1 step: {'PASS' if pl[-1]-pl[0] <= 16 else 'FAIL'}")
if done:
    d = [int(r["secs"]) for r in done]
    l = [int(r["loss"]) for r in done if r["loss"]]
    print(f"  duration       {min(d)}-{max(d)}s (mean {sum(d)//len(d)}s)")
    print(f"  placement loss {min(l)}-{max(l)}permille" if l else "")
for r in rows:
    if r["state"] != "done":
        print(f"  run {r['run']}: {r['state']} {r['fail']}")
PY
