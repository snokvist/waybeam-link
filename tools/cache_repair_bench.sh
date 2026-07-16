#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# §14.3 cache-repair end-to-end bench (v1 IP transport), no radios needed:
#
#   frame_shm_feed produce ──> TX 17 ──udp-broadcast──┬─> aggregator 9 (lossy,
#                                                     │   cache.repair) ──>
#                                                     │   frame_shm_feed consume
#                                                     └─> cache node 33 (clean,
#                                                         cache.store)
#
# The aggregator takes rx_drop_permille synthetic loss; the spatially-"clean"
# cache node hears the same broadcast losslessly and answers CACHE_REQUESTs
# over localhost UDP. MODE=cache-only (default) points the aggregator's NACK
# return at a dead port so recovered_arq must stay 0 and every repaired block
# is attributable to the cache path; MODE=combined leaves vehicle ARQ live in
# parallel (§14.3 rule 8). The consumer verifies every delivered frame
# byte-exact (§6.3a reassembly), not just counts.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build/dev"}
LINK=${LINK:-"$BUILD/waybeam-link"}
FEED=${FEED:-"$BUILD/frame_shm_feed"}
FRAMES=${FRAMES:-300}
FPS=${FPS:-30}
P_BYTES=${P_BYTES:-12000}
IDR_EVERY=${IDR_EVERY:-30}
DROP=${DROP:-150}
MODE=${MODE:-cache-only}
MIN_DELIVERED_PCT=${MIN_DELIVERED_PCT:-85}
BASE_PORT=${BASE_PORT:-25200}
STATUS_INTERVAL_MS=${STATUS_INTERVAL_MS:-200}

if [[ "$MODE" != cache-only && "$MODE" != combined ]]; then
    echo "MODE must be cache-only or combined" >&2
    exit 2
fi
if [[ ! -x "$LINK" || ! -x "$FEED" ]]; then
    echo "bench binaries missing under $BUILD" >&2
    echo "run: cmake --preset dev && cmake --build --preset dev -j" >&2
    exit 1
fi

AIRP=$BASE_PORT              # shared broadcast data channel
DEADP=$((BASE_PORT + 1))     # nobody listens here
CACHEC=$((BASE_PORT + 2))    # cache.store listen
AGGC=$((BASE_PORT + 3))      # cache.repair listen (replies + status)
RETP=$AIRP
if [[ "$MODE" == cache-only ]]; then
    RETP=$DEADP              # NACKs go nowhere => recovered_arq must be 0
fi

IN_RING="wblink_cache_in_$$"
OUT_RING="wblink_cache_out_$$"
TMP=$(mktemp -d /tmp/wblink-cache-bench.XXXXXX)
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
cat >"$TMP/tx.json" <<EOF
{
  "node": {"originator": 17, "role": "tx", "preferred_originator": 9},
  "profile_table": "$TABLE",
  "streams": [{"stream_id": 0, "stream_type": "RTP", "dir": "in",
    "bind": {"kind": "frame-shm", "name": "$IN_RING"},
    "fec": {"scheme": "rlc256", "i_rate_permille": 250,
            "p_rate_permille": 100, "min_k": 3}}],
  "air": {"kind": "udp-broadcast", "tx": ["127.255.255.255:$AIRP"],
          "rx": ["0.0.0.0:$AIRP"], "pace_mbps": 100},
  "policy": {"select": {"min_profile": 0, "max_profile": 0}},
  "stats": {"hz": 5}
}
EOF
cat >"$TMP/agg.json" <<EOF
{
  "node": {"originator": 9, "role": "rx"},
  "profile_table": "$TABLE",
  "streams": [{"stream_id": 0, "stream_type": "RTP", "dir": "out",
    "originator": 17, "bind": {"kind": "frame-shm", "name": "$OUT_RING"}}],
  "air": {"kind": "udp-broadcast", "tx": ["127.255.255.255:$RETP"],
          "rx": ["0.0.0.0:$AIRP"], "pace_mbps": 100,
          "rx_drop_permille": $DROP},
  "policy": {"select": {"min_profile": 0, "max_profile": 0}},
  "cache": {"repair": {"enabled": true, "stream_id": 0,
    "listen": "127.0.0.1:$AGGC",
    "caches": [{"originator": 33, "endpoint": "127.0.0.1:$CACHEC"}]}},
  "stats": {"hz": 5}
}
EOF
cat >"$TMP/cache.json" <<EOF
{
  "node": {"originator": 33, "role": "rx"},
  "profile_table": "$TABLE",
  "air": {"kind": "udp-broadcast", "tx": ["127.255.255.255:$DEADP"],
          "rx": ["0.0.0.0:$AIRP"], "pace_mbps": 100},
  "policy": {"select": {"min_profile": 0, "max_profile": 0}},
  "cache": {"store": {"enabled": true, "listen": "127.0.0.1:$CACHEC",
    "stream_ids": [0], "status_to": ["127.0.0.1:$AGGC"],
    "status_interval_ms": $STATUS_INTERVAL_MS}},
  "stats": {"hz": 5}
}
EOF

(cd "$ROOT" && "$LINK" rx -c "$TMP/agg.json") \
    >"$TMP/agg.jsonl" 2>"$TMP/agg.log" &
pids+=($!)
(cd "$ROOT" && "$LINK" rx -c "$TMP/cache.json") \
    >"$TMP/cache.jsonl" 2>"$TMP/cache.log" &
pids+=($!)
(cd "$ROOT" && "$LINK" tx -c "$TMP/tx.json") \
    >"$TMP/tx.jsonl" 2>"$TMP/tx.log" &
pids+=($!)

MIN_FRAMES=$((FRAMES * MIN_DELIVERED_PCT / 100))
TOTAL_MS=$((FRAMES * 1000 / FPS + 20000))
"$FEED" consume "$OUT_RING" "$MIN_FRAMES" 2500 "$TOTAL_MS" \
    >"$TMP/consumer.log" 2>&1 &
consumer_pid=$!
pids+=("$consumer_pid")
sleep 0.5
"$FEED" produce "$IN_RING" "$FRAMES" "$FPS" "$P_BYTES" "$IDR_EVERY" \
    >"$TMP/producer.log" 2>&1

set +e
wait "$consumer_pid"
consumer_rc=$?
set -e
sleep 0.3
kill -TERM "${pids[0]}" "${pids[1]}" "${pids[2]}" 2>/dev/null || true
wait "${pids[0]}" "${pids[1]}" "${pids[2]}" 2>/dev/null || true

cat "$TMP/producer.log" "$TMP/consumer.log"
if (( consumer_rc != 0 )); then
    echo "consumer failed (delivered < ${MIN_DELIVERED_PCT}% or integrity)" >&2
    KEEP_TMP=1
    exit 1
fi

python3 - "$TMP/agg.jsonl" "$TMP/cache.jsonl" "$MODE" <<'PY'
import json
import sys

def last(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    if not rows:
        raise SystemExit(f"no stats in {path}")
    return rows[-1]

agg, cache, mode = last(sys.argv[1]), last(sys.argv[2]), sys.argv[3]
cr = agg["cache_repair"]
cs = cache["cache_store"]
aggs = agg["streams"][0]
assert cr["requests"] > 0, cr
assert cr["replies"] > 0, cr
assert cr["symbols_accepted"] > 0, cr
assert cr["blocks_repaired"] > 0, cr
assert cs["requests_answered"] > 0, cs
assert cs["symbols_sent"] > 0, cs
assert cs["health_permille"] > 900, cs   # the cache hears a clean broadcast
if mode == "cache-only":
    # NACK return is dead-ported: every repair is the cache path's (§14.3-8).
    assert aggs["recovered_arq"] == 0, aggs
print("cache stats: requests=%d replies=%d accepted=%d repaired_blocks=%d "
      "futile=%d suppressed=%d | store answered=%d sent=%d health=%d | "
      "agg fast=%d fec=%d arq=%d unrecoverable=%d" %
      (cr["requests"], cr["replies"], cr["symbols_accepted"],
       cr["blocks_repaired"], cr["blocks_futile"], cr["requests_suppressed"],
       cs["requests_answered"], cs["symbols_sent"], cs["health_permille"],
       aggs["frames_fast"], aggs["recovered_fec"], aggs["recovered_arq"],
       aggs["frames_unrecoverable"]))
PY
echo "cache repair bench ($MODE, drop=${DROP}permille): PASS"
