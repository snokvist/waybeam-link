#!/bin/bash
# §6.3b Phase D runner: TX + RX waybeam-link over udp-air on loopback, a real
# multi-slice HEVC elementary stream through the frame-SHM rings, synthetic
# RX symbol loss, conceal on/off. Reads the RX §15.3 NDJSON tail back out.
# usage: udp_air_run.sh <drop_permille> <slice-skip|off> <tag>
# env: WBLINK_REPO WBLINK_BIN WBLINK_FEED CONCEAL_WORK CONCEAL_ES CONCEAL_FPS
set -u
DROP=$1; MODE=$2; TAG=$3
REPO=${WBLINK_REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
SP=${CONCEAL_WORK:-/tmp/spatial_conceal}
BIN=${WBLINK_BIN:-$REPO/build/dev/waybeam-link}
FEED=${WBLINK_FEED:-$REPO/build/dev/frame_shm_feed}
OUT=$SP/run_$TAG
mkdir -p $OUT

cat > $OUT/tx.json <<EOF
{
  "node": { "originator": 17, "role": "tx", "preferred_originator": 0 },
  "profile_table": "profiles/table.example.json",
  "streams": [
    { "stream_id": 0, "stream_type": "RTP", "dir": "in",
      "bind": { "kind": "frame-shm", "name": "venc_frame" },
      "fec": { "scheme": "rlc256", "i_rate_permille": 250, "p_rate_permille": 100, "min_k": 3 } }
  ],
  "air": { "kind": "udp", "tx": ["127.0.0.1:5801"], "rx": ["127.0.0.1:5810"] },
  "stats": { "hz": 1 }
}
EOF

cat > $OUT/rx.json <<EOF
{
  "node": { "originator": 9, "role": "rx", "preferred_originator": 17 },
  "profile_table": "profiles/table.example.json",
  "streams": [
    { "stream_id": 0, "stream_type": "RTP", "dir": "out", "originator": 17,
      "bind": { "kind": "frame-shm", "name": "venc_frame_out" },
      "conceal": { "mode": "$MODE", "freeze_frame": true } }
  ],
  "air": { "kind": "udp", "tx": ["127.0.0.1:5810"], "rx": ["127.0.0.1:5801"],
           "rx_drop_permille": $DROP },
  "stats": { "hz": 1 }
}
EOF

rm -f /dev/shm/venc_frame /dev/shm/venc_frame_out
cd $REPO
$BIN rx -c $OUT/rx.json > $OUT/rx_stats.ndjson 2> $OUT/rx.log &
RXPID=$!
sleep 1
$BIN tx -c $OUT/tx.json > $OUT/tx_stats.ndjson 2> $OUT/tx.log &
TXPID=$!
sleep 1
$FEED dump venc_frame_out $OUT/egress.265 3000 40000 > $OUT/dump.txt 2>&1 &
DUMPPID=$!
$FEED play venc_frame ${CONCEAL_ES:-$SP/long.265} ${CONCEAL_FPS:-100} 1 2000 > $OUT/play.txt 2>&1
sleep 3
kill -TERM $TXPID 2>/dev/null
wait $DUMPPID 2>/dev/null
kill -TERM $RXPID 2>/dev/null
wait $TXPID $RXPID 2>/dev/null

cat $OUT/play.txt $OUT/dump.txt
python3 - "$OUT/rx_stats.ndjson" <<'PYEOF'
import json, sys
last = None
for line in open(sys.argv[1]):
    line = line.strip()
    if line.startswith('{'):
        try: last = json.loads(line)
        except Exception: pass
if last:
    for s in last.get('streams', []):
        keys = ['frames_fast','recovered_fec','frames_salvaged','frames_frozen',
                'salvage_failed','slices_synthesized','frames_unrecoverable',
                'dropped_deadline','dropped_superseded','malformed','frame_count']
        print('rx-stats:', {k: s.get(k) for k in keys if k in s})
PYEOF
