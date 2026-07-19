#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Full-controller UDP soak: EVERYTHING on at once — unpinned §9 selector,
# §9.6 horizon caps, §14.2 enforcement, §9.11 fps ladder (SPEC seeds, not
# fast bench seeds), §14.3 cache repair — driven through a design-doc-style
# loss scenario schedule:
#
#   clean -> marginal -> burst -> fade -> interference -> outage -> recovery
#
# Asserts at the end: clean SIGTERM exits on every node (ASan/LSan silent),
# every stats line parses (schema stable across all controller states),
# bounded venc writes (flash-wear proxy) with zero failures, byte-exact
# delivery above a floor despite the outage, frame-size-driven fps reduction
# under sustained small frames, and FULL recovery (fps back at preferred, rung
# off the floor) by the end. SOAK_MULT stretches every phase for long runs
# (rig soaks); the default is a ~2.5 minute smoke soak.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build/dev"}
LINK=${LINK:-"$BUILD/waybeam-link"}
FEED=${FEED:-"$BUILD/frame_shm_feed"}
BASE_PORT=${BASE_PORT:-25900}
FPS=${FPS:-30}
SMALL_P_BYTES=${SMALL_P_BYTES:-7000}
LARGE_P_BYTES=${LARGE_P_BYTES:-16000}
SOAK_MULT=${SOAK_MULT:-1}
MIN_DELIVERED_PCT=${MIN_DELIVERED_PCT:-55}
MAX_VENC_PUSHES=${MAX_VENC_PUSHES:-200}

if [[ ! -x "$LINK" || ! -x "$FEED" ]]; then
    echo "bench binaries missing under $BUILD" >&2
    exit 1
fi

AIRP=$BASE_PORT
VENCP=$((BASE_PORT + 1))
CACHEC=$((BASE_PORT + 2))
AGGC=$((BASE_PORT + 3))
DEADP=$((BASE_PORT + 4))
CTRL=$((BASE_PORT + 5))
IN_RING="wblink_soak_in_$$"
OUT_RING="wblink_soak_out_$$"
TMP=$(mktemp -d /tmp/wblink-soak.XXXXXX)
pids=()
cleanup() {
    for pid in "${pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    if [[ ${KEEP_TMP:-0} == 1 ]]; then
        echo "soak artifacts: $TMP" >&2
    else
        rm -rf "$TMP"
    fi
}
trap cleanup EXIT INT TERM
TABLE="$ROOT/profiles/table.example.json"

cat >"$TMP/tx.json" <<EOF
{
  "node": {"originator": 17, "role": "tx", "preferred_originator": 9},
  "profile_table": "$TABLE",
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
  "venc": {"host": "127.0.0.1:$VENCP", "enabled": true, "fps_hint": $FPS,
           "fps_ladder": {"enabled": true, "min": 60, "preferred": 100,
                          "max": 144, "min_p_frame_bytes": 10000,
                          "restore_hysteresis_bytes": 1000}},
  "stats": {"hz": 5}
}
EOF
cat >"$TMP/agg.json" <<EOF
{
  "node": {"originator": 9, "role": "rx"},
  "profile_table": "$TABLE",
  "streams": [{"stream_id": 0, "stream_type": "RTP", "dir": "out",
    "originator": 17, "bind": {"kind": "frame-shm", "name": "$OUT_RING"}}],
  "air": {"kind": "udp-broadcast", "tx": ["127.255.255.255:$AIRP"],
          "rx": ["0.0.0.0:$AIRP"], "pace_mbps": 50, "rx_drop_permille": 10},
  "cache": {"repair": {"enabled": true, "stream_id": 0,
    "listen": "127.0.0.1:$AGGC",
    "caches": [{"originator": 33, "endpoint": "127.0.0.1:$CACHEC"}]}},
  "control": {"bind": "127.0.0.1:$CTRL"},
  "stats": {"hz": 5}
}
EOF
cat >"$TMP/cache.json" <<EOF
{
  "node": {"originator": 33, "role": "rx"},
  "profile_table": "$TABLE",
  "air": {"kind": "udp-broadcast", "tx": ["127.255.255.255:$DEADP"],
          "rx": ["0.0.0.0:$AIRP"], "pace_mbps": 50},
  "cache": {"store": {"enabled": true, "listen": "127.0.0.1:$CACHEC",
    "stream_ids": [0], "status_to": ["127.0.0.1:$AGGC"],
    "status_interval_ms": 500}},
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
(cd "$ROOT" && "$LINK" rx -c "$TMP/agg.json") \
    >"$TMP/agg.jsonl" 2>"$TMP/agg.log" &
agg_pid=$!
pids+=("$agg_pid")
(cd "$ROOT" && "$LINK" rx -c "$TMP/cache.json") \
    >"$TMP/cache.jsonl" 2>"$TMP/cache.log" &
cache_pid=$!
pids+=("$cache_pid")
(cd "$ROOT" && "$LINK" tx -c "$TMP/tx.json") \
    >"$TMP/tx.jsonl" 2>"$TMP/tx.log" &
tx_pid=$!
pids+=("$tx_pid")

# Phase schedule (seconds x SOAK_MULT) and its total for the feeder.
# Recovery is sized for three spec-seed ladder restores (8 s gate + settle
# each). The feeder independently changes P-frame size at the same phase
# boundaries; loss still exercises selector, JSCC, ARQ, FEC, and cache paths.
TOTAL_S=$((158 * SOAK_MULT))
"$FEED" consume "$OUT_RING" 0 8000 $(( (TOTAL_S + 30) * 1000 )) \
    >"$TMP/consumer.log" 2>&1 &
pids+=($!)
(
    "$FEED" produce "$IN_RING" $((20 * SOAK_MULT * FPS)) "$FPS" \
        "$LARGE_P_BYTES" "$FPS"
    "$FEED" produce "$IN_RING" $((93 * SOAK_MULT * FPS)) "$FPS" \
        "$SMALL_P_BYTES" "$FPS"
    "$FEED" produce "$IN_RING" $((45 * SOAK_MULT * FPS)) "$FPS" \
        "$LARGE_P_BYTES" "$FPS"
) >"$TMP/producer.log" 2>&1 &
pids+=($!)

drop() {
    curl -s -o /dev/null -X POST "http://127.0.0.1:$CTRL/api/v1/bench/rx-drop" \
        -H 'Content-Type: application/json' -d "{\"permille\": $1}" || true
}
p() { sleep $(($1 * SOAK_MULT)); }

echo "phase: clean";        drop 0;    p 20
echo "phase: marginal";     drop 80;   p 20
echo "phase: burst"
for _ in $(seq 1 $((4 * SOAK_MULT))); do
    drop 400; sleep 3; drop 20; sleep 3
done
echo "phase: fade"
for d in 60 150 250 350 250 150 60; do drop "$d"; p 3; done
echo "phase: interference"
for _ in $(seq 1 $((2 * SOAK_MULT))); do
    drop 300; sleep 5; drop 60; sleep 5
done
echo "phase: outage";       drop 1000; p 8
echo "phase: recovery";     drop 0;    p 45

kill -TERM "$tx_pid" "$agg_pid" "$cache_pid" 2>/dev/null || true
rc_tx=0; rc_agg=0; rc_cache=0
wait "$tx_pid"    || rc_tx=$?
wait "$agg_pid"   || rc_agg=$?
wait "$cache_pid" || rc_cache=$?
echo "exits: tx=$rc_tx agg=$rc_agg cache=$rc_cache"

python3 - "$TMP" "$TOTAL_S" "$FPS" "$MIN_DELIVERED_PCT" \
    "$MAX_VENC_PUSHES" "$rc_tx" "$rc_agg" "$rc_cache" <<'PY'
import json
import re
import sys
from urllib.parse import parse_qs, urlparse

(tmp, total_s, fps, min_pct, max_pushes,
 rc_tx, rc_agg, rc_cache) = (
    sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]),
    int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7]), int(sys.argv[8]))

# Clean SIGTERM exits: nonzero would be an ASan/LSan report or a crash.
assert (rc_tx, rc_agg, rc_cache) == (0, 0, 0), (rc_tx, rc_agg, rc_cache)

def parse_all(path):
    rows = []
    for i, line in enumerate(open(path, encoding="utf-8")):
        if not line.strip():
            continue
        rows.append(json.loads(line))  # any schema break throws here
    assert rows, path
    return rows

tx_rows = parse_all(f"{tmp}/tx.jsonl")
agg_rows = parse_all(f"{tmp}/agg.jsonl")
cache_rows = parse_all(f"{tmp}/cache.jsonl")

txs = tx_rows[-1]["streams"][0]
link = tx_rows[-1]["link"]
agg = agg_rows[-1]
cr = agg["cache_repair"]

# Actuator health: bounded writes, zero failures, recovered state.
assert link["venc_failures"] == 0, link
assert link["venc_pushes"] <= max_pushes, link["venc_pushes"]
assert link["venc_fps"] == 100, link
assert link["profile"] > 0, link  # off the floor after recovery
# The ladder reduced at least once under sustained exhaustion.
fps_writes = []
for line in open(f"{tmp}/venc.jsonl", encoding="utf-8"):
    q = parse_qs(urlparse(json.loads(line)["path"]).query)
    if "video0.fps" in q:
        fps_writes.append(int(q["video0.fps"][0]))
assert fps_writes and min(fps_writes) < 100, fps_writes
assert fps_writes[-1] == 100, fps_writes
# Enforcement ran and never wedged the stream.
assert txs["jscc_enforced_frames"] > 0, txs
assert txs["delivered"] > 0, txs
# The cache did real work across the lossy phases.
assert cr["requests"] > 0 and cr["blocks_repaired"] > 0, cr
# Byte-exact delivery above the floor despite the outage phase.
m = re.search(r"consumer frames=(\d+) bad=(\d+)",
              open(f"{tmp}/consumer.log", encoding="utf-8").read())
assert m, "no consumer summary"
frames, bad = int(m.group(1)), int(m.group(2))
assert bad == 0, f"integrity: {bad} bad frames"
need = total_s * fps * min_pct // 100
assert frames >= need, f"delivered {frames} < {need}"

print("soak: %ds, %d stats lines schema-clean | delivered %d/%d (%.0f%%) "
      "bad=0 | venc pushes=%d failures=0 fps=%s | enforced=%d "
      "cache repaired=%d futile=%d | final rung=%d fps=100"
      % (total_s, len(tx_rows) + len(agg_rows) + len(cache_rows),
         frames, total_s * fps, 100.0 * frames / (total_s * fps),
         link["venc_pushes"], fps_writes, txs["jscc_enforced_frames"],
         cr["blocks_repaired"], cr["blocks_futile"], link["profile"]))
PY
echo "controller soak: PASS"
