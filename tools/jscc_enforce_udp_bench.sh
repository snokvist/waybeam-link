#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# §14.2 Pass-38 enforcement verification — UDP-air backend (§17 ordering:
# UDP first, then radio/kernel-monitor on the rig).
#
# Phases (one TX with jscc_shadow.enforce=true, one RX feeding JSCC_FEEDBACK):
#   A: pinned normal rung + synthetic loss  -> decisions become VALID and
#      actuate (jscc_enforced_frames grows, fallback "none", no discards)
#   D: live re-pin to a 1 ms-deadline rung  -> rule-2 deadline discards fire
#      (jscc_discarded_frames grows; the fail-safe oscillates by design once
#      the starved RX stops feeding back — that IS the §14.2 contract)
#   B: RX killed                            -> feedback stales; enforcement
#      stops while frames keep transmitting on the fixed §14.1 path
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build/dev"}
LINK=${LINK:-"$BUILD/waybeam-link"}
FEED=${FEED:-"$BUILD/frame_shm_feed"}
BASE_PORT=${BASE_PORT:-25400}
FPS=${FPS:-30}
P_BYTES=${P_BYTES:-12000}
DROP=${DROP:-100}

if [[ ! -x "$LINK" || ! -x "$FEED" ]]; then
    echo "bench binaries missing under $BUILD" >&2
    exit 1
fi

AIRP=$BASE_PORT
TXCTRL=$((BASE_PORT + 1))
RXCTRL=$((BASE_PORT + 2))
IN_RING="wblink_jscc_in_$$"
OUT_RING="wblink_jscc_out_$$"
TMP=$(mktemp -d /tmp/wblink-jscc-bench.XXXXXX)
pids=()
cleanup() {
    for pid in "${pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    if [[ ${KEEP_TMP:-0} == 1 ]]; then
        echo "bench artifacts: $TMP" >&2
    else
        rm -rf "$TMP"
    fi
}
trap cleanup EXIT INT TERM

# Custom table: rung 4 stays stock; rung 5 gets 1 ms deadlines so a live
# re-pin makes every frame deadline-unreachable at the paced serialization.
python3 - "$ROOT/profiles/table.example.json" "$TMP/table.json" <<'PY'
import json
import sys

table = json.load(open(sys.argv[1], encoding="utf-8"))
for p in table["profiles"]:
    if p["id"] == 5:
        p["arq_deadline_ms"] = {"iframe": 1, "pframe": 1}
json.dump(table, open(sys.argv[2], "w", encoding="utf-8"), indent=1)
PY

cat >"$TMP/tx.json" <<EOF
{
  "node": {"originator": 17, "role": "tx", "preferred_originator": 9},
  "profile_table": "$TMP/table.json",
  "streams": [{"stream_id": 0, "stream_type": "RTP", "dir": "in",
    "bind": {"kind": "frame-shm", "name": "$IN_RING"},
    "arq_mode": "all-frames",
    "fec": {"scheme": "rlc256", "i_rate_permille": 250,
            "p_rate_permille": 100, "min_k": 3},
    "jscc_shadow": {"fec_floor_permille": 20, "fec_cap_permille": 400,
      "arq_guard_us": 500, "feedback_timeout_ms": 1000,
      "min_rtt_samples": 3, "enforce": true}}],
  "air": {"kind": "udp-broadcast", "tx": ["127.255.255.255:$AIRP"],
          "rx": ["0.0.0.0:$AIRP"], "pace_mbps": 50},
  "policy": {"select": {"min_profile": 4, "max_profile": 4}},
  "control": {"bind": "127.0.0.1:$TXCTRL"},
  "stats": {"hz": 5}
}
EOF
cat >"$TMP/rx.json" <<EOF
{
  "node": {"originator": 9, "role": "rx"},
  "profile_table": "$TMP/table.json",
  "streams": [{"stream_id": 0, "stream_type": "RTP", "dir": "out",
    "originator": 17, "bind": {"kind": "frame-shm", "name": "$OUT_RING"}}],
  "air": {"kind": "udp-broadcast", "tx": ["127.255.255.255:$AIRP"],
          "rx": ["0.0.0.0:$AIRP"], "pace_mbps": 50,
          "rx_drop_permille": $DROP},
  "control": {"bind": "127.0.0.1:$RXCTRL"},
  "stats": {"hz": 5}
}
EOF

(cd "$ROOT" && "$LINK" rx -c "$TMP/rx.json") \
    >"$TMP/rx.jsonl" 2>"$TMP/rx.log" &
rx_pid=$!
pids+=("$rx_pid")
(cd "$ROOT" && "$LINK" tx -c "$TMP/tx.json") \
    >"$TMP/tx.jsonl" 2>"$TMP/tx.log" &
tx_pid=$!
pids+=("$tx_pid")
"$FEED" consume "$OUT_RING" 0 5000 60000 >"$TMP/consumer.log" 2>&1 &
pids+=($!)
"$FEED" produce "$IN_RING" $((30 * FPS)) "$FPS" "$P_BYTES" "$FPS" \
    >"$TMP/producer.log" 2>&1 &
pids+=($!)

snap() { cp "$TMP/tx.jsonl" "$TMP/snap-$1.jsonl"; }

sleep 10                    # phase A: readiness + enforcement
snap A
curl -s -o /dev/null -X POST "http://127.0.0.1:$TXCTRL/api/v1/link/profile" \
    -H 'Content-Type: application/json' -d '{"min": 5, "max": 5}'
sleep 6                     # phase D: 1 ms deadlines => rule-2 discards
snap D
kill -TERM "$rx_pid" 2>/dev/null || true
sleep 2.5                   # feedback stales (timeout 1000 ms)
snap B1
sleep 4
snap B2
kill -TERM "$tx_pid" 2>/dev/null || true
wait "$tx_pid" 2>/dev/null || true

python3 - "$TMP" <<'PY'
import json
import sys

def last(path):
    rows = [json.loads(l) for l in open(path, encoding="utf-8") if l.strip()]
    assert rows, path
    return rows[-1]["streams"][0]

tmp = sys.argv[1]
a = last(f"{tmp}/snap-A.jsonl")
d = last(f"{tmp}/snap-D.jsonl")
b1 = last(f"{tmp}/snap-B1.jsonl")
b2 = last(f"{tmp}/snap-B2.jsonl")

# Phase A: decisions valid and actuating on the normal rung; no discards.
assert a["jscc_valid_decisions"] > 0, a
assert a["jscc_enforced_frames"] > 0, a
assert a["jscc_discarded_frames"] == 0, a
assert a["jscc_fallback"] == "none", a
assert a["repair_symbols_sent"] > 0, a

# Phase D: the 1 ms rung makes valid decisions discard (rule 2). The
# fail-safe oscillation (starved RX -> stale feedback -> fixed path) means
# not every frame discards — growth is the assertion.
assert d["jscc_discarded_frames"] > 0, d
assert d["jscc_enforced_frames"] >= a["jscc_enforced_frames"], (a, d)

# Phase B: RX dead -> feedback stales -> enforcement stops, frames continue
# on the fixed §14.1 path.
assert b2["jscc_enforced_frames"] == b1["jscc_enforced_frames"], (b1, b2)
assert b2["jscc_discarded_frames"] == b1["jscc_discarded_frames"], (b1, b2)
assert b2["delivered"] > b1["delivered"], (b1, b2)
assert b2["jscc_fallback"] in ("feedback_stale", "feedback_missing"), b2

print("enforce: A valid=%d enforced=%d parity=%d | D discards=%d | "
      "B fallback=%s frames+%d"
      % (a["jscc_valid_decisions"], a["jscc_enforced_frames"],
         a["repair_symbols_sent"], d["jscc_discarded_frames"],
         b2["jscc_fallback"], b2["delivered"] - b1["delivered"]))
PY
echo "jscc enforcement UDP bench: PASS"
