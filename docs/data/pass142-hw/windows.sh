#!/usr/bin/env bash
# Drive the three windows against an ALREADY-RUNNING pair. No process
# management here — that is what kept hanging.
set -u
S=/tmp/wb-stats
w() {
    curl -s --max-time 5 localhost:8099/api/v1/stats > $S.a
    sleep "$2"
    curl -s --max-time 5 localhost:8099/api/v1/stats > $S.b
    LABEL="$1" python3 <<'PY'
import json, os
a=json.load(open("/tmp/wb-stats.a")); b=json.load(open("/tmp/wb-stats.b"))
s=lambda d,k: sum(x.get(k,0) for x in d["adapters"])
lab=os.environ["LABEL"]
print("  %-9s accepted +%4d   filtered +%4d" %
      (lab, s(b,"rx")-s(a,"rx"), s(b,"filtered")-s(a,"filtered")))
PY
}
w idle 6
curl -s --max-time 5 -X POST localhost:8099/api/v1/scout/start -d '{"channels":[5805],"dwell_ms":6000}' >/dev/null
w sweep 6
curl -s --max-time 5 -X POST localhost:8099/api/v1/scout/stop >/dev/null
sleep 1
w restored 6
