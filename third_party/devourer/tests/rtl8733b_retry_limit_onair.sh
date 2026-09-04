#!/usr/bin/env bash
# rtl8733b_retry_limit_onair.sh — is DeviceConfig::tx::retry_limit a LIVE
# actuator on the RTL8733B, or just an encoded descriptor field?
#
# The 8733B has no working CCX / tx.report path (no C2H reports were observed —
# src/rtl8733b/CLAUDE.md), so the TX side cannot be its own witness the way
# tests/ack_txreport_matrix.sh judges the Jaguars. This bench judges from the
# AIR instead: the DUT sends unicast QoS-Data to a MAC that nobody owns, so no
# ACK ever comes back and the MAC must exhaust its descriptor retry limit on
# every frame. A passive monitor counts how many times each submitted frame
# actually aired.
#
#   retry_limit = N  ->  airings/frame ~= 1 + N   (1 original + N retries)
#
# That is a DOSE-RESPONSE, not an A/B: a single on/off pair could be explained
# by ambient conditions, three or more levels on a straight line cannot.
#
# COUNTING NOTE: the witness emits one `rx.seq` event per received copy, keyed
# by a test-specific unicast SA and carrying txdemo's payload counter. This is
# deliberately not the sampled `rx.txhit` stream: counting those events would
# undercount by 100x, while reading their cumulative field would quantize every
# arm by up to 99 airings. Each arm gets a fresh witness and log.
#
#   sudo bash tests/rtl8733b_retry_limit_onair.sh
#   ARMS="0 3 12" FRAMES=3000 CH=36 sudo bash tests/rtl8733b_retry_limit_onair.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD=${BUILD:-$ROOT/build}

DUT_VID=${DUT_VID:-0x0bda}; DUT_PID=${DUT_PID:-0xf72b}   # RTL8731BU/8733BU
WIT_VID=${WIT_VID:-0x0bda}; WIT_PID=${WIT_PID:-0x8812}   # RTL8812AU witness
CH=${CH:-6}
ARMS=${ARMS:-"0 3 12 0 12"}
FRAMES=${FRAMES:-1500}
RATE=${RATE:-MCS0}
GAP_US=${GAP_US:-3000}
PWR_QDB=${PWR_QDB:-12}
# RA must be UNICAST (so an ACK is expected) and unowned (so none ever comes).
RA=${RA:-02:de:ad:be:ef:01}
if [ -z "${TX_SA:-}" ]; then
  run_id=$$
  printf -v TX_SA '02:73:33:%02x:%02x:%02x' \
    $(((run_id >> 16) & 255)) $(((run_id >> 8) & 255)) $((run_id & 255))
fi
OUT=${OUT:-/tmp/rtl8733b_retry}

# A capability verdict requires an actual dose-response: baseline zero plus at
# least two distinct nonzero doses, with one dose >=3. Without this guard,
# ARMS=0 could certify tx_retry_limit_ok after proving only that frames air.
read -r -a raw_arm_values <<< "$ARMS"
arm_values=()
declare -A arm_seen=()
have_zero=0; max_arm=-1
for arm_text in "${raw_arm_values[@]}"; do
  if ! [[ "$arm_text" =~ ^[0-9]+$ ]]; then
    echo "ABORT: retry arm '$arm_text' is outside the descriptor range 0..63" >&2
    exit 2
  fi
  arm=$((10#$arm_text))
  if [ "$arm" -gt 63 ]; then
    echo "ABORT: retry arm '$arm_text' is outside the descriptor range 0..63" >&2
    exit 2
  fi
  arm_values+=("$arm")
  arm_seen[$arm]=1
  [ "$arm" -eq 0 ] && have_zero=1
  [ "$arm" -gt "$max_arm" ] && max_arm=$arm
done
if [ "${#arm_seen[@]}" -lt 3 ] || [ "$have_zero" -ne 1 ] ||
   [ "$max_arm" -lt 3 ]; then
  echo "ABORT: ARMS must contain baseline 0 and at least two distinct" >&2
  echo "       nonzero retry levels (one >=3); got: '$ARMS'" >&2
  exit 2
fi

# Fixed statistical floor, not an overrideable tuning knob.
FRAMES_MIN=1000
if ! [[ "$FRAMES" =~ ^[0-9]+$ ]] || [ "$FRAMES" -lt "$FRAMES_MIN" ]; then
  echo "ABORT: FRAMES=$FRAMES is invalid or below the validity floor $FRAMES_MIN" >&2
  exit 2
fi

# Kill only this tree's demos; do not terminate unrelated bench sessions.
# pkill uses an ERE. Escape every ERE metacharacter so a build directory
# containing characters such as '+', '?', '(' or '|' remains an exact prefix.
ESC_BUILD=$(printf '%s' "$BUILD" | sed 's#[][\\.^$*+?(){}|/]#\\&#g')
KILL() {
  sudo pkill -9 -f "^$ESC_BUILD/rxdemo" 2>/dev/null
  sudo pkill -9 -f "^$ESC_BUILD/txdemo" 2>/dev/null
  return 0
}
trap KILL EXIT
mkdir -p "$OUT"; RESULTS="$OUT/results.jsonl"; : >"$RESULTS"

idx=0
for arm in "${arm_values[@]}"; do
  idx=$((idx+1))
  # Per-ARM-INDEX filenames, not per-retry-value: a ladder repeats values
  # (0/3/12/0/12) and reusing the value as the name lets a slow-dying witness
  # from the earlier arm append into the next one's log.
  tag="$(printf '%02d_r%s' "$idx" "$arm")"
  KILL; sleep 3   # USB release after a -9 is not instantaneous
  # Redirects intentionally belong to the invoking user, not root.
  # shellcheck disable=SC2024
  sudo env DEVOURER_VID="$WIT_VID" DEVOURER_PID="$WIT_PID" \
       DEVOURER_CHANNEL="$CH" DEVOURER_RX_PCTR=1 \
       DEVOURER_RX_AGG_SA="$TX_SA" \
       DEVOURER_LOG_LEVEL=info \
       "$BUILD/rxdemo" >"$OUT/wit_$tag.jsonl" 2>"$OUT/wit_$tag.err" &
  waited=0
  until grep -qE "async ring of .* URBs submitted|Listening air" "$OUT/wit_$tag.err"; do
    sleep 1; waited=$((waited+1))
    if [ "$waited" -ge 25 ]; then
      echo "ABORT: witness never reached RX for arm=$arm (#$idx)" >&2
      tail -5 "$OUT/wit_$tag.err" >&2; exit 1
    fi
  done
  sleep 2
  # shellcheck disable=SC2024
  sudo env DEVOURER_VID="$DUT_VID" DEVOURER_PID="$DUT_PID" \
       DEVOURER_CHANNEL="$CH" DEVOURER_TX_QOS_DATA=1 \
       DEVOURER_TX_RA="$RA" DEVOURER_TX_SA="$TX_SA" \
       DEVOURER_TX_RATE="$RATE" DEVOURER_TX_PAYLOAD_BYTES=200 \
       DEVOURER_TX_GAP_US="$GAP_US" DEVOURER_TX_FRAMES="$FRAMES" \
       DEVOURER_TX_RETRY_LIMIT="$arm" DEVOURER_TX_PWR_OFFSET_QDB="$PWR_QDB" \
       DEVOURER_LOG_LEVEL=warn \
       timeout -s INT 90 "$BUILD/txdemo" >"$OUT/tx_$tag.jsonl" 2>"$OUT/tx_$tag.err" || true
  sleep 3
  sent=$(grep '"ev":"tx.stats"' "$OUT/tx_$tag.jsonl" | tail -1 |
         sed -n 's/.*"submitted":\([0-9]*\).*/\1/p'); sent=${sent:-0}
  hits=$(python3 - "$OUT/wit_$tag.jsonl" <<'PY'
import json, sys

hits = 0
for line in open(sys.argv[1], errors="replace"):
    if not line.startswith('{"ev":"rx.seq"'):
        continue
    try:
        event = json.loads(line)
    except json.JSONDecodeError:
        continue
    if not event.get("crc"):
        hits += 1
print(hits)
PY
  )
  KILL
  # A cell that did not run is NOT a measurement of zero. An arm whose DUT
  # never opened (sent=0) or whose witness heard nothing at all (hits=0) is a
  # harness failure, and reporting it as 0.0 airings/frame would read exactly
  # like a dead retry engine — the same false-verdict shape that made an
  # 8821AU look broken in tests/ack_txreport_matrix.sh. Abort instead.
  if [ "$sent" -eq 0 ] || [ "$hits" -eq 0 ]; then
    echo "ABORT: arm=$arm (#$idx) did not run — submitted=$sent airings=$hits" >&2
    echo "       (DUT or witness failed to open; this is not a zero result)" >&2
    tail -5 "$OUT/tx_$tag.err" >&2
    exit 1
  fi
  if [ "$sent" -ne "$FRAMES" ] || [ "$sent" -lt "$FRAMES_MIN" ]; then
    echo "ABORT: arm=$arm (#$idx) submitted $sent of $FRAMES requested;" >&2
    echo "       partial runs do not meet the $FRAMES_MIN-frame validity floor" >&2
    exit 1
  fi
  python3 - "$arm" "$sent" "$hits" >>"$RESULTS" <<'PY'
import json, sys
arm, sent, hits = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
per = round(hits / sent, 2) if sent else 0.0
print(json.dumps({"ev": "retry.arm", "retry_limit": arm, "submitted": sent,
                  "airings": hits, "airings_per_frame": per,
                  "expected": 1 + arm}))
PY
  tail -1 "$RESULTS"
done

echo "==== VERDICT ===="
python3 - "$RESULTS" <<'PY'
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
ok = True
for r in rows:
    exp, got = r["expected"], r["airings_per_frame"]
    # The SA filter excludes ambient traffic; the witness can still lose
    # copies, so the floor is what matters. 0.6*expected separates every
    # adjacent level in the required 0/3/12-style ladder.
    good = 0.6 * exp <= got <= 1.15 * exp
    ok &= good
    print(f"  retry={r['retry_limit']:>2}  expected~{exp:>2}  measured={got:>5}"
          f"  {'OK' if good else 'FAIL'}")
print(json.dumps({"ev": "retry.verdict", "tx_retry_limit_ok": bool(ok),
                  "arms": len(rows)}))
sys.exit(0 if ok else 1)
PY
