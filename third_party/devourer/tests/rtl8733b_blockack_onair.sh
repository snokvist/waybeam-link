#!/usr/bin/env bash
# Does the RTL8733B immediate-response gate return a BlockAck for a received
# A-MPDU? A Jaguar2 TX forms normal-ack-policy A-MPDUs and a third adapter
# witnesses aggregate structure, copies per payload counter, and the response
# control frames themselves.
#
# This deliberately does not use tx.report: per-frame CCX accounting is not a
# valid retry oracle under A-MPDU (docs/aggregation.md). The air-side witness
# must first prove that both phases contain real aggregates (paggr plus bursts
# of >=4 MPDUs sharing one RX TSF), then show that arming the RTL8733B both
# collapses retry copies toward one and produces addressed 0x94 BlockAck frames
# with nonzero bitmaps. The active-but-unarmed control must approach
# 1 + RETRY_LIMIT copies and produce no such BlockAck.
#
# Three distinct adapters are required. Defaults match the local bench:
#   TX      0bda:b812  RTL8822B/88x2BU, Jaguar2
#   RESP    0bda:f72b  RTL8733B under test
#   WITNESS 0bda:8812  RTL8812AU, Jaguar1
#
#   sudo bash tests/rtl8733b_blockack_onair.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-/tmp/rtl8733b-blockack}

TX_VID=${TX_VID:-0x0bda}; TX_PID=${TX_PID:-0xb812}
RESP_VID=${RESP_VID:-0x0bda}; RESP_PID=${RESP_PID:-0xf72b}
WIT_VID=${WIT_VID:-0x0bda}; WIT_PID=${WIT_PID:-0x8812}
CH=${CH:-36}; RATE=${RATE:-MCS3}; SECS=${SECS:-14}
RETRY_LIMIT=${RETRY_LIMIT:-12}; RESP_MAC=${RESP_MAC:-02:12:34:56:78:9a}
if [ -z "${TX_SA:-}" ]; then
  run_id=$$
  printf -v TX_SA '02:ba:33:%02x:%02x:%02x' \
    $(((run_id >> 16) & 255)) $(((run_id >> 8) & 255)) $((run_id & 255))
fi

if ! [[ "$SECS" =~ ^[0-9]+$ && "$RETRY_LIMIT" =~ ^[0-9]+$ ]] ||
   [ "$SECS" -lt 10 ] || [ "$RETRY_LIMIT" -lt 5 ] ||
   [ "$RETRY_LIMIT" -gt 63 ]; then
  echo "ABORT: require SECS>=10 and RETRY_LIMIT=5..63" >&2
  exit 2
fi
min_secs=$((RETRY_LIMIT + 1))
if [ "$min_secs" -lt 10 ]; then min_secs=10; fi
if [ "$SECS" -lt "$min_secs" ]; then
  echo "ABORT: SECS=$SECS is too short for the 1000-payload off-arm floor" >&2
  echo "       at RETRY_LIMIT=$RETRY_LIMIT; require at least $min_secs" >&2
  exit 2
fi
if [ "$TX_VID:$TX_PID" = "$RESP_VID:$RESP_PID" ] ||
   [ "$TX_VID:$TX_PID" = "$WIT_VID:$WIT_PID" ] ||
   [ "$RESP_VID:$RESP_PID" = "$WIT_VID:$WIT_PID" ]; then
  echo "ABORT: TX, responder, and witness must be distinct adapters" >&2
  exit 2
fi

# pkill/pgrep use EREs. Escape every ERE metacharacter so a build directory
# containing characters such as '+', '?', '(' or '|' remains an exact prefix.
ESC_BUILD=$(printf '%s' "$BUILD" | sed 's#[][\\.^$*+?(){}|/]#\\&#g')
cleanup() {
  sudo pkill -9 -f "^$ESC_BUILD/rxdemo" 2>/dev/null
  sudo pkill -9 -f "^$ESC_BUILD/txdemo" 2>/dev/null
  return 0
}
trap cleanup EXIT
mkdir -p "$OUT"
RESULTS="$OUT/results.jsonl"; : >"$RESULTS"

wait_rx() {
  local err="$1" waited=0
  until grep -qE 'async ring of .* URBs submitted' "$err"; do
    sleep 1; waited=$((waited + 1))
    if [ "$waited" -ge 25 ]; then
      echo "ABORT: RX process never became ready: $err" >&2
      tail -8 "$err" >&2
      exit 1
    fi
  done
}

stop_receivers() {
  sudo pkill -INT -f "^$ESC_BUILD/rxdemo" 2>/dev/null || true
  for _ in 1 2 3 4 5; do
    if ! pgrep -f "^$ESC_BUILD/rxdemo" >/dev/null; then
      return 0
    fi
    sleep 1
  done
  sudo pkill -9 -f "^$ESC_BUILD/rxdemo" 2>/dev/null || true
  return 1
}

# Establish a known-passive RTL8733B before the responder-off control. Merely
# killing a previous responder process is not such a guarantee: SIGKILL cannot
# run Halmac8733bMac::stop(). A normal no-responder session plus its verified
# stop-time clear makes the negative arm independent of prior bench state.
force_responder_passive() {
  cleanup; sleep 2
  # Redirects intentionally belong to the invoking user, not root.
  # shellcheck disable=SC2024
  sudo env DEVOURER_VID="$RESP_VID" DEVOURER_PID="$RESP_PID" \
    DEVOURER_CHANNEL="$CH" DEVOURER_LOG_LEVEL=info "$BUILD/rxdemo" \
    >"$OUT/resp_passive.jsonl" 2>"$OUT/resp_passive.err" &
  wait_rx "$OUT/resp_passive.err"
  if ! stop_receivers; then
    echo "ABORT: passive-reset receiver did not stop gracefully" >&2
    exit 1
  fi
  if grep -qE 'ACK responder disarm (did not latch|failed)' \
       "$OUT/resp_passive.err"; then
    echo "ABORT: passive-reset ACK gate clear was not verified" >&2
    tail -8 "$OUT/resp_passive.err" >&2
    exit 1
  fi
  sleep 2
}

run_phase() {
  local phase="$1" responder="$2"
  local resp_pid wit_pid
  local -a resp_env=()
  cleanup; sleep 2
  if [ "$responder" -eq 1 ]; then
    resp_env+=("DEVOURER_ACK_RESPONDER=$RESP_MAC")
  fi
  # Both arms keep the same RTL8733B process fully initialized and receiving;
  # only DEVOURER_ACK_RESPONDER differs. A powered-down off arm would confound
  # the gate with the entire active MAC/RX state.
  # shellcheck disable=SC2024
  sudo env DEVOURER_VID="$RESP_VID" DEVOURER_PID="$RESP_PID" \
    DEVOURER_CHANNEL="$CH" "${resp_env[@]}" DEVOURER_LOG_LEVEL=info \
    "$BUILD/rxdemo" >"$OUT/resp_$phase.jsonl" 2>"$OUT/resp_$phase.err" &
  resp_pid=$!
  if [ "$responder" -eq 1 ]; then
    waited=0
    until grep -q "hardware ACK responder armed for $RESP_MAC" \
                 "$OUT/resp_$phase.err"; do
      sleep 1; waited=$((waited + 1))
      if [ "$waited" -ge 25 ]; then
        echo "ABORT: RTL8733B responder never armed in phase=$phase" >&2
        tail -8 "$OUT/resp_$phase.err" >&2
        exit 1
      fi
    done
  fi
  wait_rx "$OUT/resp_$phase.err"
  if ! kill -0 "$resp_pid" 2>/dev/null; then
    echo "ABORT: RTL8733B responder process exited during phase=$phase init" >&2
    tail -8 "$OUT/resp_$phase.err" >&2
    exit 1
  fi

  # shellcheck disable=SC2024
  sudo env DEVOURER_VID="$WIT_VID" DEVOURER_PID="$WIT_PID" \
    DEVOURER_CHANNEL="$CH" DEVOURER_RX_PCTR=1 \
    DEVOURER_RX_AGG_SA="$TX_SA" DEVOURER_RX_CONTROL=1 \
    DEVOURER_LOG_LEVEL=info \
    "$BUILD/rxdemo" >"$OUT/wit_$phase.jsonl" 2>"$OUT/wit_$phase.err" &
  wit_pid=$!
  wait_rx "$OUT/wit_$phase.err"
  if ! kill -0 "$wit_pid" 2>/dev/null; then
    echo "ABORT: witness process exited during phase=$phase init" >&2
    tail -8 "$OUT/wit_$phase.err" >&2
    exit 1
  fi
  sleep 2

  # no_ack=0 keeps normal acknowledgement policy and the configured retry
  # limit. QSEL 0 and the gapless feed let the Jaguar2 MAC form A-MPDUs. Keep
  # one host feeder: on this bench a two-thread feed can fill the no-BA queue
  # without presenting a measurable negative arm to the independent witness.
  # shellcheck disable=SC2024
  sudo env DEVOURER_VID="$TX_VID" DEVOURER_PID="$TX_PID" \
    DEVOURER_CHANNEL="$CH" DEVOURER_TX_QOS_DATA=1 \
    DEVOURER_TX_RA="$RESP_MAC" DEVOURER_TX_SA="$TX_SA" \
    DEVOURER_TX_RATE="$RATE" DEVOURER_TX_PAYLOAD_BYTES=200 \
    DEVOURER_TX_GAP_US=0 DEVOURER_TX_RETRY_LIMIT="$RETRY_LIMIT" \
    DEVOURER_TX_AMPDU_MODE=0/16/7/0/20 \
    DEVOURER_LOG_LEVEL=warn timeout -s INT "$SECS" "$BUILD/txdemo" \
    >"$OUT/tx_$phase.jsonl" 2>"$OUT/tx_$phase.err" || true
  if ! kill -0 "$resp_pid" 2>/dev/null; then
    echo "ABORT: RTL8733B responder exited during phase=$phase TX" >&2
    tail -8 "$OUT/resp_$phase.err" >&2
    exit 1
  fi
  if ! kill -0 "$wit_pid" 2>/dev/null; then
    echo "ABORT: witness exited during phase=$phase TX" >&2
    tail -8 "$OUT/wit_$phase.err" >&2
    exit 1
  fi
  sleep 2
  if ! stop_receivers; then
    echo "ABORT: phase=$phase receivers did not stop gracefully" >&2
    cleanup
    exit 1
  fi
  if grep -qE 'ACK responder disarm (did not latch|failed)|ACK responder disarm register write failed' \
       "$OUT/resp_$phase.err" 2>/dev/null; then
    echo "ABORT: phase=$phase responder disarm was not verified" >&2
    tail -8 "$OUT/resp_$phase.err" >&2
    cleanup
    exit 1
  fi
  cleanup

  sent=$(grep '"ev":"tx.stats"' "$OUT/tx_$phase.jsonl" | tail -1 |
         sed -n 's/.*"submitted":\([0-9]*\).*/\1/p')
  sent=${sent:-0}
  if [ "$sent" -lt 500 ]; then
    echo "ABORT: phase=$phase submitted only $sent frames" >&2
    tail -8 "$OUT/tx_$phase.err" >&2
    exit 1
  fi

  if ! python3 - "$phase" "$sent" "$OUT/wit_$phase.jsonl" \
       "$TX_SA" "$RESP_MAC" \
       >>"$RESULTS" <<'PY'
import collections, json, sys
phase, sent, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
tx_sa = sys.argv[4].replace(":", "").lower()
resp_mac = sys.argv[5].replace(":", "").lower()
events = []
copies = collections.Counter()
blockacks = []
for line in open(path, errors="replace"):
    if line.startswith('{"ev":"rx.blockack"'):
        try:
            ba = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (not ba.get("crc") and ba.get("ra", "").lower() == tx_sa and
                ba.get("ta", "").lower() == resp_mac):
            blockacks.append(ba)
        continue
    if not line.startswith('{"ev":"rx.seq"'):
        continue
    try:
        ev = json.loads(line)
    except json.JSONDecodeError:
        continue
    if ev.get("crc"):
        continue
    events.append(ev)
    copies[int(ev["pctr"])] += 1
if len(copies) < 1000:
    raise SystemExit(f"ABORT: witness recorded only {len(copies)} unique "
                     "clean payload counters; require 1000")

bursts = []
run = 1
for prev, cur in zip(events, events[1:]):
    if cur.get("tsfl") == prev.get("tsfl"):
        run += 1
    else:
        bursts.append(run)
        run = 1
bursts.append(run)
aggregated = sum(bool(e.get("paggr")) for e in events)
total = len(events)
nonzero_bitmaps = sum(ba.get("bitmap", "0" * 16) != "0" * 16
                      for ba in blockacks)
compressed = sum(bool(int(ba.get("ctrl", 0)) & 0x4) for ba in blockacks)
compressed_nonzero = sum(bool(int(ba.get("ctrl", 0)) & 0x4) and
                         ba.get("bitmap", "0" * 16) != "0" * 16
                         for ba in blockacks)
print(json.dumps({"ev": "rtl8733b.blockack", "phase": phase,
                  "sent": sent, "observed_frames": len(copies),
                  "copies": total,
                  "copies_per_observed_frame": round(total / len(copies), 3),
                  "paggr_ratio": round(aggregated / total, 3),
                  "mean_burst": round(sum(bursts) / len(bursts), 3),
                  "max_burst": max(bursts),
                  "blockacks": len(blockacks),
                  "compressed_blockacks": compressed,
                  "nonzero_bitmaps": nonzero_bitmaps,
                  "compressed_nonzero_bitmaps": compressed_nonzero}))
PY
  then
    exit 1
  fi
  tail -1 "$RESULTS"
}

force_responder_passive
run_phase off 0
run_phase on 1

python3 - "$RESULTS" "$RETRY_LIMIT" "$RATE" <<'PY'
import json, sys
rows = {r["phase"]: r for r in map(json.loads, open(sys.argv[1]))}
limit, rate = int(sys.argv[2]), sys.argv[3]
on, off = rows["on"], rows["off"]
on_c, off_c = on["copies_per_observed_frame"], off["copies_per_observed_frame"]
structure = all(r["paggr_ratio"] >= 0.50 and r["max_burst"] >= 4
                for r in (on, off))
closure = (on_c <= 1.25 and
           0.60 * (limit + 1) <= off_c <= 1.15 * (limit + 1) and
           off_c >= 5 * on_c)
control = (on["blockacks"] >= 100 and
           on["compressed_blockacks"] >= 100 and
           on["nonzero_bitmaps"] >= 100 and
           on["compressed_nonzero_bitmaps"] >= 100 and
           off["blockacks"] == 0)
ok = structure and closure and control
print(json.dumps({"ev": "rtl8733b.blockack.verdict", "ok": ok,
                  "rate": rate, "aggregation_proven": structure,
                  "control_frames_proven": control,
                  "on_copies": on_c, "off_copies": off_c,
                  "on_paggr": on["paggr_ratio"],
                  "off_paggr": off["paggr_ratio"],
                  "on_max_burst": on["max_burst"],
                  "off_max_burst": off["max_burst"],
                  "on_blockacks": on["blockacks"],
                  "off_blockacks": off["blockacks"],
                  "on_compressed_blockacks": on["compressed_blockacks"],
                  "on_nonzero_bitmaps": on["nonzero_bitmaps"],
                  "on_compressed_nonzero_bitmaps": on["compressed_nonzero_bitmaps"],
                  "on_observed": on["observed_frames"],
                  "off_observed": off["observed_frames"]}))
raise SystemExit(0 if ok else 1)
PY
