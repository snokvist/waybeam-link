#!/usr/bin/env bash
# Does an RTL8733B soliciting TX recognize a real hardware ACK and stop its
# autonomous retry loop? A third adapter witnesses copies per payload counter:
# responder ON must collapse each frame toward one airing; responder OFF must
# drive the same descriptor to 1 + RETRY_LIMIT airings.
#
# Three adapters are required: RTL8733B DUT, an ACK-capable responder, and an
# independent passive witness. RATE=11M CH=6 exercises the CCK path.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-/tmp/rtl8733b-arq-tx}

DUT_VID=${DUT_VID:-0x0bda}; DUT_PID=${DUT_PID:-0xf72b}
RESP_VID=${RESP_VID:-0x0bda}; RESP_PID=${RESP_PID:-0x8812}
WIT_VID=${WIT_VID:-0x0bda}; WIT_PID=${WIT_PID:-0xc812}
CH=${CH:-36}; RATE=${RATE:-MCS3}; FRAMES=${FRAMES:-1000}
GAP_US=${GAP_US:-5000}; RETRY_LIMIT=${RETRY_LIMIT:-12}
RESP_MAC=${RESP_MAC:-02:12:34:56:78:9a}
if [ -z "${TX_SA:-}" ]; then
  run_id=$$
  printf -v TX_SA '02:73:33:%02x:%02x:%02x' \
    $(((run_id >> 16) & 255)) $(((run_id >> 8) & 255)) $((run_id & 255))
fi

if ! [[ "$FRAMES" =~ ^[0-9]+$ && "$RETRY_LIMIT" =~ ^[0-9]+$ ]] ||
   [ "$FRAMES" -lt 1000 ] || [ "$RETRY_LIMIT" -lt 5 ] ||
   [ "$RETRY_LIMIT" -gt 63 ]; then
  echo "ABORT: require FRAMES>=1000 and RETRY_LIMIT=5..63" >&2
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

wait_rx() {
  local err="$1" waited=0
  until grep -qE 'async ring of .* URBs submitted|Listening air' "$err"; do
    sleep 1; waited=$((waited + 1))
    if [ "$waited" -ge 25 ]; then
      echo "ABORT: RX process never became ready: $err" >&2
      tail -8 "$err" >&2
      exit 1
    fi
  done
}

run_phase() {
  local phase="$1" responder="$2"
  cleanup; sleep 2
  if [ "$responder" -eq 1 ]; then
    # Redirects intentionally belong to the invoking user, not root.
    # shellcheck disable=SC2024
    sudo env DEVOURER_VID="$RESP_VID" DEVOURER_PID="$RESP_PID" \
      DEVOURER_CHANNEL="$CH" DEVOURER_ACK_RESPONDER="$RESP_MAC" \
      DEVOURER_LOG_LEVEL=info "$BUILD/rxdemo" \
      >"$OUT/resp_$phase.jsonl" 2>"$OUT/resp_$phase.err" &
    waited=0
    until grep -q "hardware ACK responder armed for $RESP_MAC" \
                 "$OUT/resp_$phase.err"; do
      sleep 1; waited=$((waited + 1))
      if [ "$waited" -ge 25 ]; then
        echo "ABORT: responder never armed in phase=$phase" >&2
        tail -8 "$OUT/resp_$phase.err" >&2
        exit 1
      fi
    done
  fi

  # shellcheck disable=SC2024
  sudo env DEVOURER_VID="$WIT_VID" DEVOURER_PID="$WIT_PID" \
    DEVOURER_CHANNEL="$CH" DEVOURER_RX_PCTR=1 \
    DEVOURER_RX_AGG_SA="$TX_SA" DEVOURER_LOG_LEVEL=info \
    "$BUILD/rxdemo" >"$OUT/wit_$phase.jsonl" 2>"$OUT/wit_$phase.err" &
  wait_rx "$OUT/wit_$phase.err"
  sleep 2

  # shellcheck disable=SC2024
  sudo env DEVOURER_VID="$DUT_VID" DEVOURER_PID="$DUT_PID" \
    DEVOURER_CHANNEL="$CH" DEVOURER_TX_QOS_DATA=1 \
    DEVOURER_TX_RA="$RESP_MAC" DEVOURER_TX_SA="$TX_SA" \
    DEVOURER_TX_RATE="$RATE" DEVOURER_TX_PAYLOAD_BYTES=200 \
    DEVOURER_TX_GAP_US="$GAP_US" DEVOURER_TX_FRAMES="$FRAMES" \
    DEVOURER_TX_RETRY_LIMIT="$RETRY_LIMIT" \
    DEVOURER_TX_PWR_OFFSET_QDB=12 DEVOURER_LOG_LEVEL=warn \
    timeout -s INT 90 "$BUILD/txdemo" \
    >"$OUT/tx_$phase.jsonl" 2>"$OUT/tx_$phase.err" || true
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
  if [ "$sent" -ne "$FRAMES" ]; then
    echo "ABORT: phase=$phase submitted $sent of $FRAMES frames" >&2
    exit 1
  fi

  python3 - "$phase" "$sent" "$OUT/wit_$phase.jsonl" >>"$RESULTS" <<'PY'
import collections, json, sys
phase, sent, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
copies = collections.Counter()
for line in open(path, errors="replace"):
    if not line.startswith('{"ev":"rx.seq"'):
        continue
    try:
        ev = json.loads(line)
    except json.JSONDecodeError:
        continue
    if not ev.get("crc"):
        copies[int(ev["pctr"])] += 1
if not copies:
    raise SystemExit("ABORT: witness recorded no payload counters")
total = sum(copies.values())
print(json.dumps({"ev": "rtl8733b.arq_tx", "phase": phase,
                  "sent": sent, "observed_frames": len(copies),
                  "coverage": round(len(copies) / sent, 3),
                  "copies": total,
                  "copies_per_observed_frame": round(total / len(copies), 3)}))
PY
  tail -1 "$RESULTS"
}

run_phase on 1
run_phase off 0

python3 - "$RESULTS" "$RETRY_LIMIT" "$RATE" <<'PY'
import json, sys
rows = {r["phase"]: r for r in map(json.loads, open(sys.argv[1]))}
limit = int(sys.argv[2])
rate = sys.argv[3]
on, off = rows["on"], rows["off"]
on_c = on["copies_per_observed_frame"]
off_c = off["copies_per_observed_frame"]
ok = (on["coverage"] >= 0.60 and off["coverage"] >= 0.60 and
      on_c <= 1.25 and 0.60 * (limit + 1) <= off_c <= 1.15 * (limit + 1) and
      off_c >= 5 * on_c)
print(json.dumps({"ev": "rtl8733b.arq_tx.verdict", "ok": ok,
                  "rate": rate, "on_copies": on_c, "off_copies": off_c,
                  "on_coverage": on["coverage"],
                  "off_coverage": off["coverage"]}))
raise SystemExit(0 if ok else 1)
PY
