#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# §9.11 Pass-53 FPS-ladder verification — UDP-air backend (§17 ordering:
# UDP first, then radio on the rig).
#
# Phases: sufficiently large P frames (hold preferred) -> undersized P frames
# (step 100 -> 90 -> 75 -> 60 with dwell) -> large P frames (predictive,
# hysteretic restore to 100). Link loss is deliberately absent: frame size,
# not selector state or bitrate inference, is the §9.11 control signal.
# The fake venc records every video0.fps write; the checker asserts the
# envelope, adjacency, dwell spacing, write-on-change, and the final
# recovery to preferred — and that caps writes follow fps changes (§9.11
# cap coupling).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build/dev"}
LINK=${LINK:-"$BUILD/waybeam-link"}
FEED=${FEED:-"$BUILD/frame_shm_feed"}
BASE_PORT=${BASE_PORT:-25500}
FPS=${FPS:-100}
SMALL_P_BYTES=${SMALL_P_BYTES:-7000}
LARGE_P_BYTES=${LARGE_P_BYTES:-16000}

if [[ ! -x "$LINK" || ! -x "$FEED" ]]; then
    echo "bench binaries missing under $BUILD" >&2
    exit 1
fi

D0=$BASE_PORT
RET=$((BASE_PORT + 1))
VENCP=$((BASE_PORT + 2))
CTRL=$((BASE_PORT + 3))
IN_RING="wblink_fps_in_$$"
OUT_RING="wblink_fps_out_$$"
TMP=$(mktemp -d /tmp/wblink-fps-bench.XXXXXX)
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

TABLE="$ROOT/profiles/table.example.json"
SELECT='{"mcs_settle_s": 0.5, "promote_dwell_s": 0.3, "down_cooldown_s": 0.2,
         "bitrate_lead_s": 0.2, "mcs_up_grace_s": 0.1,
         "min_profile": 5, "max_profile": 5}'
LADDER='{"enabled": true, "min": 60, "preferred": 100, "max": 144,
         "min_p_frame_bytes": 10000, "restore_hysteresis_bytes": 1000,
         "sample_timeout_ms": 500,
         "reduce_after_ms": 1200, "reduce_dwell_ms": 1500,
         "restore_after_ms": 2500, "settle_ms": 500}'
cat >"$TMP/tx.json" <<EOF
{
  "node": {"originator": 17, "role": "tx", "preferred_originator": 9},
  "profile_table": "$TABLE",
  "streams": [{"stream_id": 0, "stream_type": "RTP", "dir": "in",
    "bind": {"kind": "frame-shm", "name": "$IN_RING"},
    "fec": {"scheme": "rlc256", "i_rate_permille": 250,
            "p_rate_permille": 100, "min_k": 3}}],
  "air": {"kind": "udp", "tx": ["127.0.0.1:$D0"], "rx": ["127.0.0.1:$RET"]},
  "policy": {"select": $SELECT},
  "venc": {"host": "127.0.0.1:$VENCP", "enabled": true, "fps_hint": $FPS,
           "fps_ladder": $LADDER},
  "stats": {"hz": 5}
}
EOF
cat >"$TMP/rx.json" <<EOF
{
  "node": {"originator": 9, "role": "rx"},
  "profile_table": "$TABLE",
  "streams": [{"stream_id": 0, "stream_type": "RTP", "dir": "out",
    "originator": 17, "bind": {"kind": "frame-shm", "name": "$OUT_RING"}}],
  "air": {"kind": "udp", "rx": ["127.0.0.1:$D0"], "tx": ["127.0.0.1:$RET"]},
  "control": {"bind": "127.0.0.1:$CTRL"},
  "loopback": {"rssi_dbm": -50},
  "stats": {"hz": 5}
}
EOF

python3 "$ROOT/tools/fake_venc.py" "$VENCP" "$TMP/venc.jsonl" &
pids+=($!)
for _ in $(seq 1 40); do
    curl -s -o /dev/null "http://127.0.0.1:$VENCP/ping" && break
    sleep 0.05
done
: >"$TMP/venc.jsonl"
(cd "$ROOT" && "$LINK" rx -c "$TMP/rx.json") \
    >"$TMP/rx.jsonl" 2>"$TMP/rx.log" &
pids+=($!)
(cd "$ROOT" && "$LINK" tx -c "$TMP/tx.json") \
    >"$TMP/tx.jsonl" 2>"$TMP/tx.log" &
tx_pid=$!
pids+=("$tx_pid")
"$FEED" consume "$OUT_RING" 0 5000 70000 >"$TMP/consumer.log" 2>&1 &
pids+=($!)
feed_phase() {
    local seconds=$1
    local bytes=$2
    "$FEED" produce "$IN_RING" $((seconds * FPS)) "$FPS" "$bytes" "$FPS"
}
feed_phase 6 "$LARGE_P_BYTES" >"$TMP/producer.log" 2>&1
feed_phase 12 "$SMALL_P_BYTES" >>"$TMP/producer.log" 2>&1
feed_phase 16 "$LARGE_P_BYTES" >>"$TMP/producer.log" 2>&1
kill -TERM "$tx_pid" 2>/dev/null || true
wait "$tx_pid" 2>/dev/null || true

python3 - "$TMP/venc.jsonl" "$TMP/tx.jsonl" "${FAIL_FIRST:-0}" <<'PY'
import json
import sys
from urllib.parse import parse_qs, urlparse

fps_writes = []   # (t, fps)
for line in open(sys.argv[1], encoding="utf-8"):
    rec = json.loads(line)
    q = parse_qs(urlparse(rec["path"]).query)
    if "video0.fps" in q:
        fps_writes.append((rec["t"], int(q["video0.fps"][0])))

seq = [f for _, f in fps_writes]
assert seq, "no fps writes at all"
assert seq[0] == 100, f"first command must be preferred: {seq}"
assert all(f in (60, 75, 90, 100) for f in seq), f"envelope violated: {seq}"
assert all(a != b for a, b in zip(seq, seq[1:])), f"duplicate write: {seq}"
assert 60 in seq, f"never reached min under exhaustion: {seq}"
assert seq[-1] == 100, f"did not restore to preferred: {seq}"
lowest = seq.index(60)
assert seq[:lowest + 1] == [100, 90, 75, 60], f"reduction not stepwise: {seq}"
assert seq[lowest:] == [60, 75, 90, 100], f"restore not stepwise: {seq}"
# Dwell: consecutive reductions at least reduce_dwell (1.5 s) apart; restores
# at least restore_after (2.5 s) after the previous change.
times = [t for t, _ in fps_writes]
assert times[2] - times[1] >= 1.4, f"reduce dwell violated: {times}"
assert times[3] - times[2] >= 1.4, f"reduce dwell violated: {times}"
assert times[lowest + 1] - times[lowest] >= 2.4, f"restore gate violated: {times}"
rows = [json.loads(l) for l in open(sys.argv[2], encoding="utf-8") if l.strip()]
link = rows[-1]["link"]
assert link["venc_fps"] == 100, link
assert link["venc_p_frame_target_bytes"] == 10000, link
assert link["venc_failures"] == int(sys.argv[3]), link
print("fps ladder: %s (frame-size driven, dwell ok)"
      % seq)
PY
echo "fps ladder UDP bench: PASS"
