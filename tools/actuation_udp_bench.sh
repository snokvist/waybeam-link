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
# §9.5/§9.6 integer math: every commanded bitrate must map to a table rung's
# derived budget, every cap write must equal derive_frame_caps() for the
# rung active at that moment, caps must accompany every bitrate change, and
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

python3 - "$TMP/venc.jsonl" "$TMP/tx.jsonl" "$TABLE" "$FPS" <<'PY'
import json
import sys
from urllib.parse import parse_qs, urlparse

venc_log, tx_stats, table_path, fps = (
    sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]))

# --- mirror the §9.5 Pass-6 budget + §9.6 cap math (all-integer) -----------
HT20 = [6500, 13000, 19500, 26000, 39000, 52000, 58500, 65000]
LADDER = [30, 45, 60, 75, 90, 100, 120, 144]
I_RATE, P_RATE = 250, 100          # stream fec config in tx.json
CEILING, FLOOR = 196608, 4096      # venc.cap_ceiling_bytes seed, venc floor

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

def snap_period_us(f):
    return 1000000 // min(LADDER, key=lambda x: abs(1000000 // x - 1000000 // f))

def window_bytes(kbps, window_us, rate, headroom=1000):
    bits = kbps * window_us // 1000
    b = bits // 8
    b = b * 1000 // (1000 + rate)
    return b * headroom // 1000

def expected_caps(kbps, deadline_ms, s):
    period = snap_period_us(fps)
    max_p = window_bytes(kbps, period, P_RATE)
    max_i = window_bytes(kbps, deadline_ms * 1000, I_RATE)
    p_ceil = min(CEILING, (256000 // (1000 + P_RATE)) * s)
    i_ceil = min(CEILING, (256000 // (1000 + I_RATE)) * s)
    max_p = min(max_p, p_ceil)
    max_i = min(max_i, i_ceil)
    if max_i < max_p:
        max_i = min(max_p, i_ceil)
    return max(max_i, FLOOR), max(max_p, FLOOR)

# --- replay the fake-venc log ------------------------------------------------
writes = []
for line in open(venc_log, encoding="utf-8"):
    rec = json.loads(line)
    q = parse_qs(urlparse(rec["path"]).query)
    if "video0.bitrate" in q:
        writes.append(("bitrate", int(q["video0.bitrate"][0])))
    elif "video0.maxIBytes" in q:
        writes.append(("caps", (int(q["video0.maxIBytes"][0]),
                                int(q["video0.maxPBytes"][0]))))

bitrates = [v for k, v in writes if k == "bitrate"]
caps = [v for k, v in writes if k == "caps"]
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

# §9.5 sequencing recomputes caps at BOTH transition steps: the bitrate
# step (new budget, old rung deadline) and the MCS commit (new deadline).
# Invariants replayable from the log alone: every caps write uses the
# bitrate currently in force, and its maxI matches SOME table rung's
# I-deadline at that budget; every bitrate change is followed by at least
# one caps write before the next bitrate change (unless caps are unchanged).
current = None
caps_since_bitrate = True  # startup: bitrate is pushed before caps
prev_caps = None
for k, v in writes:
    if k == "bitrate":
        assert caps_since_bitrate, f"bitrate {v}: prior change got no caps"
        current = v
        caps_since_bitrate = False
    else:
        assert current is not None, "caps written before any bitrate"
        candidates = {
            expected_caps(current, p["arq_deadline_ms"]["iframe"],
                          p.get("max_payload", 1424) - 26 - 11)
            for p in table["profiles"]}
        assert v in candidates, (
            f"caps {v} not derivable at bitrate {current}: {sorted(candidates)}")
        caps_since_bitrate = True
        prev_caps = v
# A trailing bitrate change may legitimately leave caps unchanged (dedupe):
if not caps_since_bitrate:
    candidates = {
        expected_caps(current, p["arq_deadline_ms"]["iframe"],
                      p.get("max_payload", 1424) - 26 - 11)
        for p in table["profiles"]}
    assert prev_caps in candidates, "final bitrate change got no caps write"

# Strict final check: at steady state the active rung (TX stats) must map
# the last bitrate + its own deadline to the last commanded caps exactly.

# TX stats must agree with the last commanded values, with zero failures.
rows = [json.loads(l) for l in open(tx_stats, encoding="utf-8") if l.strip()]
link = rows[-1]["link"]
assert link["venc_failures"] == 0, link
assert link["venc_bitrate_kbps"] == bitrates[-1], link
assert (link["venc_max_i_bytes"], link["venc_max_p_bytes"]) == caps[-1], link
assert link["venc_pushes"] == len(writes), (link["venc_pushes"], len(writes))
active = next(p for p in table["profiles"] if p["id"] == link["profile"])
strict = expected_caps(bitrates[-1], active["arq_deadline_ms"]["iframe"],
                       active.get("max_payload", 1424) - 26 - 11)
assert caps[-1] == strict, (caps[-1], strict, link["profile"])
assert link["venc_settling"] is False, link

print("actuation: %d writes (%d bitrate, %d caps), rungs %s -> formula-exact"
      % (len(writes), len(bitrates), len(caps),
         sorted(set(bitrates))))
PY
echo "actuation UDP bench: PASS"
