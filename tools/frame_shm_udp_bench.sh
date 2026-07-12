#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build/release"}
LINK=${LINK:-"$BUILD/waybeam-link"}
GST=${GST:-"$BUILD/frame_shm_gst_bench"}
FRAMES=${FRAMES:-90}
WARMUP_FRAMES=${WARMUP_FRAMES:-3}
BITRATES=${BITRATES:-"1000 4000 8000"}
RX_DROP_PERMILLE=${RX_DROP_PERMILLE:-0}

if [[ ! -x "$LINK" || ! -x "$GST" ]]; then
    echo "bench binaries missing under $BUILD" >&2
    echo "run: cmake --preset release && cmake --build --preset release -j" >&2
    exit 1
fi

TMP=$(mktemp -d /tmp/wblink-frame-shm-bench.XXXXXX)
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

run_one() {
    local bitrate=$1
    local index=$2
    local data0=$((24000 + index * 10))
    local data1=$((data0 + 1))
    local ret=$((data0 + 2))
    local in_ring="wblink_bench_in_${$}_${index}"
    local out_ring="wblink_bench_out_${$}_${index}"
    local total_frames=$((FRAMES + WARMUP_FRAMES))
    local tx_cfg="$TMP/tx-${index}.json"
    local rx_cfg="$TMP/rx-${index}.json"

    cat >"$tx_cfg" <<EOF
{
  "node": {"originator":17,"role":"tx","preferred_originator":9},
  "profile_table":"profiles/table.example.json",
  "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
    "bind":{"kind":"frame-shm","name":"$in_ring"},
    "fec":{"scheme":"rlc256","i_rate_permille":250,
           "p_rate_permille":100,"min_k":3}}],
  "air":{"kind":"udp","tx":["127.0.0.1:$data0","127.0.0.1:$data1"],
         "rx":["127.0.0.1:$ret"]},
  "policy":{"select":{"min_profile":0,"max_profile":0}},
  "stats":{"hz":5}
}
EOF
    cat >"$rx_cfg" <<EOF
{
  "node":{"originator":9,"role":"rx"},
  "profile_table":"profiles/table.example.json",
  "streams":[{"stream_id":0,"stream_type":"RTP","dir":"out",
    "originator":17,"bind":{"kind":"frame-shm","name":"$out_ring"}}],
  "air":{"kind":"udp","rx":["127.0.0.1:$data0","127.0.0.1:$data1"],
         "tx":["127.0.0.1:$ret"],"rx_drop_permille":$RX_DROP_PERMILLE},
  "policy":{"select":{"min_profile":0,"max_profile":0}},
  "stats":{"hz":5}
}
EOF

    (cd "$ROOT" && "$LINK" rx -c "$rx_cfg") \
        >"$TMP/rx-${index}.jsonl" 2>"$TMP/rx-${index}.log" &
    local rx_pid=$!
    pids+=("$rx_pid")
    (cd "$ROOT" && "$LINK" tx -c "$tx_cfg") \
        >"$TMP/tx-${index}.jsonl" 2>"$TMP/tx-${index}.log" &
    local tx_pid=$!
    pids+=("$tx_pid")

    "$GST" consume "$out_ring" "$FRAMES" 30000 \
        >"$TMP/consumer-${index}.log" 2>&1 &
    local consumer_pid=$!
    pids+=("$consumer_pid")
    sleep 0.3
    "$GST" produce "$in_ring" "$bitrate" "$total_frames" \
        >"$TMP/producer-${index}.log" 2>&1
    wait "$consumer_pid"
    sleep 0.3
    kill -TERM "$tx_pid" "$rx_pid" 2>/dev/null || true
    wait "$tx_pid" "$rx_pid" 2>/dev/null || true

    python3 - "$TMP/tx-${index}.jsonl" "$TMP/rx-${index}.jsonl" \
              "$FRAMES" "$total_frames" "$RX_DROP_PERMILLE" <<'PY'
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

tx, rx = last(sys.argv[1]), last(sys.argv[2])
expected, generated, loss = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
txs = tx["streams"][0]
rxs = rx["streams"][0]
assert txs["delivered"] == generated, ("TX frames", txs["delivered"], generated)
assert sum(a["tx_submitted"] for a in tx["adapters"]) > 0
assert len(rx["adapters"]) == 2, rx["adapters"]
assert all(a["rx"] + a["drop"] > 0 for a in rx["adapters"])
if loss == 0:
    assert rxs["frames_fast"] + rxs["recovered_fec"] >= expected, rxs
    assert rxs["decode_errors"] == 0 and rxs["malformed"] == 0, rxs
print("stats tx_frames=%d rx_fast=%d rx_fec=%d loss_milli=%d nacks=%d resends=%d" %
      (txs["delivered"], rxs["frames_fast"], rxs["recovered_fec"],
       rxs["loss_postdiv_prearq_milli"], rxs["nacks_sent"],
       txs["resends_sent"]))
PY
    echo "bitrate=${bitrate}kbps drop=${RX_DROP_PERMILLE}permille"
    cat "$TMP/producer-${index}.log"
    cat "$TMP/consumer-${index}.log"
}

i=0
for bitrate in $BITRATES; do
    run_one "$bitrate" "$i"
    i=$((i + 1))
done
echo "frame-shm UDP bench: PASS"
