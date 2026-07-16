#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# R-B / Pass 42 verification: §14.2 enforcement x §14.3 cache repair on ONE
# stream must not let TX parity silently offload onto the cache path.
#
# Two runs at identical synthetic loss, both with jscc_shadow.enforce=true:
#   OFF: no cache            -> the air-path parity baseline
#   ON:  cache repair active -> parity must stay at the baseline (air-only
#        estimators, Pass 42) while the cache still repairs residual blocks
#
# The checker compares the TX repair/source symbol ratio between runs: with
# pre-Pass-42 estimators the ON ratio collapses (cache-completed blocks mask
# air loss, feedback demand drops, the enforcing TX under-protects); with
# air-only estimators the two ratios agree within tolerance.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build/dev"}
LINK=${LINK:-"$BUILD/waybeam-link"}
FEED=${FEED:-"$BUILD/frame_shm_feed"}
BASE_PORT=${BASE_PORT:-25600}
DROP=${DROP:-150}
FPS=${FPS:-30}
P_BYTES=${P_BYTES:-12000}
RUN_S=${RUN_S:-20}
TOLERANCE_PCT=${TOLERANCE_PCT:-30}

if [[ ! -x "$LINK" || ! -x "$FEED" ]]; then
    echo "bench binaries missing under $BUILD" >&2
    exit 1
fi

TMP=$(mktemp -d /tmp/wblink-offload-bench.XXXXXX)
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

run_one() {
    local tag=$1 with_cache=$2 base=$3
    local airp=$base cachec=$((base + 2)) aggc=$((base + 3)) deadp=$((base + 4))
    local in_ring="wblink_ofl_in_${$}_$tag"
    local out_ring="wblink_ofl_out_${$}_$tag"
    local cache_block=""
    if [[ "$with_cache" == 1 ]]; then
        cache_block=",
  \"cache\": {\"repair\": {\"enabled\": true, \"stream_id\": 0,
    \"listen\": \"127.0.0.1:$aggc\",
    \"caches\": [{\"originator\": 33, \"endpoint\": \"127.0.0.1:$cachec\"}]}}"
    fi
    cat >"$TMP/tx-$tag.json" <<EOF
{
  "node": {"originator": 17, "role": "tx", "preferred_originator": 9},
  "profile_table": "$TABLE",
  "streams": [{"stream_id": 0, "stream_type": "RTP", "dir": "in",
    "bind": {"kind": "frame-shm", "name": "$in_ring"},
    "arq_mode": "all-frames",
    "fec": {"scheme": "rlc256", "i_rate_permille": 250,
            "p_rate_permille": 100, "min_k": 3},
    "jscc_shadow": {"fec_floor_permille": 20, "fec_cap_permille": 400,
      "arq_guard_us": 500, "feedback_timeout_ms": 1000,
      "min_rtt_samples": 3, "enforce": true}}],
  "air": {"kind": "udp-broadcast", "tx": ["127.255.255.255:$airp"],
          "rx": ["0.0.0.0:$airp"], "pace_mbps": 50},
  "policy": {"select": {"min_profile": 4, "max_profile": 4}},
  "stats": {"hz": 5}
}
EOF
    cat >"$TMP/agg-$tag.json" <<EOF
{
  "node": {"originator": 9, "role": "rx"},
  "profile_table": "$TABLE",
  "streams": [{"stream_id": 0, "stream_type": "RTP", "dir": "out",
    "originator": 17, "bind": {"kind": "frame-shm", "name": "$out_ring"}}],
  "air": {"kind": "udp-broadcast", "tx": ["127.255.255.255:$airp"],
          "rx": ["0.0.0.0:$airp"], "pace_mbps": 50,
          "rx_drop_permille": $DROP}$cache_block,
  "stats": {"hz": 5}
}
EOF
    cat >"$TMP/cache-$tag.json" <<EOF
{
  "node": {"originator": 33, "role": "rx"},
  "profile_table": "$TABLE",
  "air": {"kind": "udp-broadcast", "tx": ["127.255.255.255:$deadp"],
          "rx": ["0.0.0.0:$airp"], "pace_mbps": 50},
  "cache": {"store": {"enabled": true, "listen": "127.0.0.1:$cachec",
    "stream_ids": [0], "status_to": ["127.0.0.1:$aggc"],
    "status_interval_ms": 200}},
  "stats": {"hz": 5}
}
EOF
    local run_pids=()
    (cd "$ROOT" && "$LINK" rx -c "$TMP/agg-$tag.json") \
        >"$TMP/agg-$tag.jsonl" 2>"$TMP/agg-$tag.log" &
    run_pids+=($!)
    if [[ "$with_cache" == 1 ]]; then
        (cd "$ROOT" && "$LINK" rx -c "$TMP/cache-$tag.json") \
            >"$TMP/cache-$tag.jsonl" 2>"$TMP/cache-$tag.log" &
        run_pids+=($!)
    fi
    (cd "$ROOT" && "$LINK" tx -c "$TMP/tx-$tag.json") \
        >"$TMP/tx-$tag.jsonl" 2>"$TMP/tx-$tag.log" &
    run_pids+=($!)
    pids+=("${run_pids[@]}")
    "$FEED" consume "$out_ring" 0 5000 $(( (RUN_S + 15) * 1000 )) \
        >"$TMP/consumer-$tag.log" 2>&1 &
    pids+=($!)
    "$FEED" produce "$in_ring" $((RUN_S * FPS)) "$FPS" "$P_BYTES" "$FPS" \
        >"$TMP/producer-$tag.log" 2>&1
    sleep 1
    for pid in "${run_pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    wait "${run_pids[@]}" 2>/dev/null || true
}

run_one off 0 "$BASE_PORT"
run_one on 1 $((BASE_PORT + 20))

python3 - "$TMP" "$TOLERANCE_PCT" <<'PY'
import json
import sys

def last(path, key="streams"):
    rows = [json.loads(l) for l in open(path, encoding="utf-8") if l.strip()]
    assert rows, path
    return rows[-1]

tmp, tol_pct = sys.argv[1], int(sys.argv[2])
off = last(f"{tmp}/tx-off.jsonl")["streams"][0]
on = last(f"{tmp}/tx-on.jsonl")["streams"][0]
agg_on = last(f"{tmp}/agg-on.jsonl")

def ratio(s):
    assert s["source_symbols_sent"] > 0, s
    return s["repair_symbols_sent"] / s["source_symbols_sent"]

r_off, r_on = ratio(off), ratio(on)
# Both runs must actually be enforcing (valid decisions actuating).
assert off["jscc_enforced_frames"] > 0, off
assert on["jscc_enforced_frames"] > 0, on
# The cache must be doing real work in the ON run.
cr = agg_on["cache_repair"]
assert cr["blocks_repaired"] > 0, cr
# Pass 42 invariant: air-only estimators keep TX parity at the air-path
# baseline — the cache must not absorb the protection budget.
gap_pct = abs(r_on - r_off) * 100.0 / max(r_off, 1e-9)
print("offload: parity ratio off=%.3f on=%.3f gap=%.0f%% | "
      "cache repaired=%d futile=%d | enforced off=%d on=%d"
      % (r_off, r_on, gap_pct, cr["blocks_repaired"], cr["blocks_futile"],
         off["jscc_enforced_frames"], on["jscc_enforced_frames"]))
assert gap_pct <= tol_pct, (
    f"parity offloaded onto the cache: ratio {r_off:.3f} -> {r_on:.3f} "
    f"({gap_pct:.0f}% > {tol_pct}%)")
PY
echo "cache offload bench: PASS"
