#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# §9.6 Pass-37 actuation verification harness — UDP-air backend (the §17
# ruling: prove the contract here FIRST, then verify the radio and
# the radio backend on the rig).
#
#   frame_shm_feed ──> TX 17 (adaptive selector, venc actuation ──> fake_venc)
#                        │ udp air
#                        v
#                      RX 9 (LINK_REPORTs; REST loss ramp drives transitions)
#
# Three phases: clean (selector promotes), loss ramp (demotes), clean again
# (re-promotes). The checker then replays the fake-venc log against the
# §9.5 integer math: every commanded bitrate must map to a table rung's
# derived budget, and
# write-on-change must hold (no duplicate consecutive writes).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build/dev"}
LINK=${LINK:-"$BUILD/waybeam-link"}
FEED=${FEED:-"$BUILD/frame_shm_feed"}
BASE_PORT=${BASE_PORT:-25300}
PHASE_S=${PHASE_S:-6}
DROP=${DROP:-200}
FPS=${FPS:-30}
P_BYTES=${P_BYTES:-12000}

if [[ ! -x "$LINK" || ! -x "$FEED" ]]; then
    echo "bench binaries missing under $BUILD" >&2
    exit 1
fi

D0=$BASE_PORT             # video downlink
RET=$((BASE_PORT + 1))    # NACK/LINK_REPORT return
VENCP=$((BASE_PORT + 2))  # fake venc HTTP
CTRL=$((BASE_PORT + 3))   # RX REST (loss ramp)
IN_RING="wblink_act_in_$$"
OUT_RING="wblink_act_out_$$"
TMP=$(mktemp -d /tmp/wblink-act-bench.XXXXXX)
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
# Fast transition seeds so all three phases fit a short run — every value is
# a §17-overridable config knob, not a code change.
SELECT='{"mcs_settle_s": 0.5, "promote_dwell_s": 0.3, "down_cooldown_s": 0.2,
         "bitrate_lead_s": 0.2, "mcs_up_grace_s": 0.1}'
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
  "venc": {"host": "127.0.0.1:$VENCP", "enabled": true, "fps_hint": $FPS},
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
for _ in $(seq 1 40); do  # wait for the endpoint before the TX's first push
    curl -s -o /dev/null "http://127.0.0.1:$VENCP/ping" && break
    sleep 0.05
done
: >"$TMP/venc.jsonl"  # drop the readiness probe from the log
(cd "$ROOT" && "$LINK" rx -c "$TMP/rx.json") \
    >"$TMP/rx.jsonl" 2>"$TMP/rx.log" &
pids+=($!)
(cd "$ROOT" && "$LINK" tx -c "$TMP/tx.json") \
    >"$TMP/tx.jsonl" 2>"$TMP/tx.log" &
pids+=($!)
"$FEED" consume "$OUT_RING" 0 5000 $(( (3 * PHASE_S + 25) * 1000 )) \
    >"$TMP/consumer.log" 2>&1 &
pids+=($!)

TOTAL_FRAMES=$((3 * PHASE_S * FPS))
"$FEED" produce "$IN_RING" "$TOTAL_FRAMES" "$FPS" "$P_BYTES" "$FPS" \
    >"$TMP/producer.log" 2>&1 &
pids+=($!)

ramp() {
    curl -s -o /dev/null -X POST "http://127.0.0.1:$CTRL/api/v1/bench/rx-drop" \
        -H 'Content-Type: application/json' -d "{\"permille\": $1}" || true
}
sleep $((PHASE_S + 2))   # phase A: clean — promote toward high rungs
ramp "$DROP"
sleep "$PHASE_S"         # phase B: lossy — demote toward the floor
ramp 0
sleep "$PHASE_S"         # phase C: clean — re-promote
sleep 2
kill -TERM "${pids[1]}" "${pids[2]}" 2>/dev/null || true
wait "${pids[1]}" "${pids[2]}" 2>/dev/null || true

python3 - "$TMP/venc.jsonl" "$TMP/tx.jsonl" "$TABLE" <<'PY'
import json
import sys
from urllib.parse import parse_qs, urlparse

venc_log, tx_stats, table_path = sys.argv[1], sys.argv[2], sys.argv[3]

# --- mirror the §9.5 Pass-6 budget math (all-integer) ---------------------
HT20 = [6500, 13000, 19500, 26000, 39000, 52000, 58500, 65000]

table = json.load(open(table_path, encoding="utf-8"))
def permille(frac):
    return round(frac * 1000)
rungs = {}
for p in table["profiles"]:
    kbps = HT20[p["mcs"]]
    if p.get("guard_interval", "long") == "short":
        kbps = kbps * 10 // 9
    kbps = kbps * permille(p["airtime_budget_frac"]) // 1000
    kbps = kbps * (1000 - permille(p.get("fec_overhead_frac", 0.0))) // 1000
    reserve = (p["reserve_bps"]["control"] + p["reserve_bps"]["telemetry"]) // 1000
    kbps = max(kbps - reserve if kbps > reserve else 0, p["bitrate_min_kbps"])
    rungs[kbps] = p

# --- replay the fake-venc log ------------------------------------------------
writes = []
for line in open(venc_log, encoding="utf-8"):
    rec = json.loads(line)
    q = parse_qs(urlparse(rec["path"]).query)
    if "video0.bitrate" in q:
        writes.append(("bitrate", int(q["video0.bitrate"][0])))
bitrates = [v for k, v in writes if k == "bitrate"]
assert len(set(bitrates)) >= 3, f"too few rung transitions: {bitrates}"
assert any(b < a for a, b in zip(bitrates, bitrates[1:])), "no demote seen"
assert any(b > a for a, b in zip(bitrates, bitrates[1:])), "no promote seen"
for v in bitrates:
    assert v in rungs, f"commanded bitrate {v} is not a rung budget"

# Write-on-change: no consecutive duplicate writes of the same kind+value.
last = {}
for k, v in writes:
    assert last.get(k) != v, f"duplicate {k} write: {v}"
    last[k] = v

# §9.6 Pass 112's caps-then-bitrate ordering check is gone with the caps
# themselves: the actuator now issues bitrate, fps and idr only, so there is
# no second write kind to order against. What survives is the §9.5 rung
# grammar above plus the stats agreement below.
# TX stats must agree with the last commanded values, with zero failures.
rows = [json.loads(l) for l in open(tx_stats, encoding="utf-8") if l.strip()]
link = rows[-1]["link"]
assert link["venc_failures"] == 0, link
assert link["venc_bitrate_kbps"] == bitrates[-1], link
assert link["venc_pushes"] == len(writes), (link["venc_pushes"], len(writes))
# §9.6 volatile-first regression control (Pass 192): fake_venc answers 200 on
# /api/v1/live/set, so a healthy actuator never reaches the persisting /set.
# A non-zero count here means the link wrote the encoder's config file.
assert link["venc_persisted_writes"] == 0, link
assert link["venc_live_fallback"] is False, link
assert link["venc_settling"] is False, link

print("actuation: %d writes (%d bitrate), rungs %s -> formula-exact"
      % (len(writes), len(bitrates),
         sorted(set(bitrates))))
PY
echo "actuation UDP bench: PASS"
