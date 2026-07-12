#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Live SSC338Q venc frame-SHM -> UDP-air -> x86 frame-SHM/decode bench.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build/release"}
LINK=${LINK:-"$BUILD/waybeam-link"}
GST=${GST:-"$BUILD/frame_shm_gst_bench"}
CRAFT=${CRAFT:-root@192.168.2.201}
CRAFT_IP=${CRAFT_IP:-192.168.2.201}
GROUND_IP=${GROUND_IP:-192.168.2.242}
FRAMES=${FRAMES:-720}
TIMEOUT_MS=${TIMEOUT_MS:-30000}
LIVE=${LIVE:-1}
RX_DROP_PERMILLE=${RX_DROP_PERMILLE:-0}
BENCH_CONSUMER=${BENCH_CONSUMER:-external}
ARTIFACTS=${ARTIFACTS:-"$ROOT/artifacts/jscc-ethernet-$(date +%Y%m%d-%H%M%S)"}
REMOTE_INSTALL=/usr/bin/waybeam-link
REMOTE_CFG=/etc/waybeam-link/jscc-ethernet.json
REMOTE_STATS=/tmp/waybeam-link-jscc-ethernet.jsonl
REMOTE_ERR=/tmp/waybeam-link-jscc-ethernet.err
REMOTE_PID=/tmp/waybeam-link-jscc-ethernet.pid
REMOTE_BACKUP=/tmp/waybeam-jscc-ethernet.json.backup
OUT_RING=${OUT_RING:-wblink_jscc_out}
STATE_DIR=${STATE_DIR:-/tmp/waybeam-jscc-ethernet}
SUPERVISOR_PID="$STATE_DIR/supervisor.pid"
SUPERVISOR_LOG="$STATE_DIR/supervisor.log"
SYSTEMD_UNIT=waybeam-jscc-ethernet.service
RUNTIME_INFO="$STATE_DIR/runtime.env"
GROUND_PID=
CONSUMER_PID=
MONITOR_PID=
REMOTE_CHANGED=0
CLEANED=0

fail() { echo "jscc ethernet bench: $*" >&2; exit 1; }

assert_single_consumer() {
    local maps pid
    local -a consumers=()
    for maps in /proc/[0-9]*/maps; do
        pid=${maps#/proc/}
        pid=${pid%/maps}
        [[ "$pid" == "$GROUND_PID" ]] && continue
        if grep -Fq "/dev/shm/$OUT_RING" "$maps" 2>/dev/null; then
            consumers+=("$pid:$(cat "/proc/$pid/comm" 2>/dev/null || echo unknown)")
        fi
    done
    if (( ${#consumers[@]} > 1 )); then
        fail "frame-shm '$OUT_RING' has multiple consumers: ${consumers[*]}"
    fi
}

remote() {
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$CRAFT" "$@"
}

remote_put() {
    local source=$1
    local target=$2
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$CRAFT" "cat >'$target'" <"$source"
}

remote_get() {
    local source=$1
    local target=$2
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$CRAFT" "cat '$source'" >"$target"
}

supervisor_running() {
    if systemctl --user is-active --quiet "$SYSTEMD_UNIT" 2>/dev/null; then
        return 0
    fi
    [[ -s "$SUPERVISOR_PID" ]] && kill -0 "$(cat "$SUPERVISOR_PID")" 2>/dev/null
}

reconcile_remote() {
    stop_remote_link
    remote 'if [ -f /tmp/waybeam-jscc-ethernet.json.backup ]; then
        cp /tmp/waybeam-jscc-ethernet.json.backup /etc/waybeam.json
        /etc/init.d/S95waybeam restart
        rm -f /tmp/waybeam-jscc-ethernet.json.backup
    fi' || true
}

start_supervisor() {
    mkdir -p "$STATE_DIR"
    if supervisor_running; then
        status_supervisor
        return 0
    fi
    rm -f "$SUPERVISOR_PID" "$SUPERVISOR_LOG"
    local pid
    local script
    script=$(readlink -f "$0")
    if systemctl --user show-environment >/dev/null 2>&1; then
        systemd-run --user --quiet --unit "$SYSTEMD_UNIT" --collect \
            --property=Type=exec --working-directory="$ROOT" \
            /usr/bin/env OUT_RING="$OUT_RING" BENCH_CONSUMER="$BENCH_CONSUMER" \
            RX_DROP_PERMILLE="$RX_DROP_PERMILLE" \
            /bin/bash -c "exec '$script' run >>'$SUPERVISOR_LOG' 2>&1"
        pid=$(systemctl --user show -p MainPID --value "$SYSTEMD_UNIT")
    else
        nohup "$script" run >"$SUPERVISOR_LOG" 2>&1 </dev/null &
        pid=$!
        echo "$pid" >"$SUPERVISOR_PID"
    fi
    for _ in {1..600}; do
        if ! supervisor_running; then
            echo "JSCC Ethernet bench failed to start:" >&2
            tail -40 "$SUPERVISOR_LOG" >&2 || true
            rm -f "$SUPERVISOR_PID"
            return 1
        fi
        if grep -q "JSCC Ethernet bench is running" "$SUPERVISOR_LOG" 2>/dev/null; then
            echo "JSCC Ethernet bench started (pid $pid)"
            echo "live dashboard: http://$GROUND_IP:8099/"
            [[ -f "$RUNTIME_INFO" ]] && sed -n 's/^/  /p' "$RUNTIME_INFO"
            return 0
        fi
        sleep 0.1
    done
    echo "JSCC Ethernet bench startup timed out after 60 seconds; see $SUPERVISOR_LOG" >&2
    stop_supervisor || true
    return 1
}

stop_supervisor() {
    if ! supervisor_running; then
        rm -f "$SUPERVISOR_PID"
        reconcile_remote
        echo "JSCC Ethernet bench is not running"
        return 0
    fi
    local pid=0
    if systemctl --user is-active --quiet "$SYSTEMD_UNIT" 2>/dev/null; then
        systemctl --user stop "$SYSTEMD_UNIT"
    else
        pid=$(cat "$SUPERVISOR_PID")
        kill -TERM "$pid"
    fi
    for _ in {1..200}; do
        if ! supervisor_running; then
            rm -f "$SUPERVISOR_PID"
            reconcile_remote
            echo "JSCC Ethernet bench stopped"
            return 0
        fi
        sleep 0.1
    done
    echo "JSCC Ethernet bench did not stop cleanly; see $SUPERVISOR_LOG" >&2
    return 1
}

status_supervisor() {
    if supervisor_running; then
        local pid
        if systemctl --user is-active --quiet "$SYSTEMD_UNIT" 2>/dev/null; then
            pid=$(systemctl --user show -p MainPID --value "$SYSTEMD_UNIT")
        else
            pid=$(cat "$SUPERVISOR_PID")
        fi
        echo "JSCC Ethernet bench running (pid $pid)"
        echo "live dashboard: http://$GROUND_IP:8099/"
        [[ -f "$RUNTIME_INFO" ]] && sed -n 's/^/  /p' "$RUNTIME_INFO"
    else
        echo "JSCC Ethernet bench stopped"
        return 1
    fi
}

stop_remote_link() {
    # shellcheck disable=SC2016
    remote 'if [ -s /tmp/waybeam-link-jscc-ethernet.pid ]; then
        p=$(cat /tmp/waybeam-link-jscc-ethernet.pid)
        kill -TERM "$p" 2>/dev/null || true
        i=0; while kill -0 "$p" 2>/dev/null && [ "$i" -lt 50 ]; do
            sleep 0.1; i=$((i + 1))
        done
        rm -f /tmp/waybeam-link-jscc-ethernet.pid
    fi' || true
}

restore_remote() {
    stop_remote_link
    if [[ "$REMOTE_CHANGED" == 1 ]]; then
        remote 'if [ -f /tmp/waybeam-jscc-ethernet.json.backup ]; then
            cp /tmp/waybeam-jscc-ethernet.json.backup /etc/waybeam.json
            /etc/init.d/S95waybeam restart
            rm -f /tmp/waybeam-jscc-ethernet.json.backup
        fi' || true
    fi
}

cleanup() {
    if [[ "$CLEANED" == 1 ]]; then return; fi
    CLEANED=1
    if [[ -n "$CONSUMER_PID" ]]; then kill -TERM "$CONSUMER_PID" 2>/dev/null || true; fi
    if [[ -n "$GROUND_PID" ]]; then kill -TERM "$GROUND_PID" 2>/dev/null || true; fi
    if [[ -n "$MONITOR_PID" ]]; then kill -TERM "$MONITOR_PID" 2>/dev/null || true; fi
    wait 2>/dev/null || true
    restore_remote
}

on_signal() {
    echo "stopping JSCC Ethernet bench" >&2
    cleanup
    exit 130
}
COMMAND=${1:-start}
case "$COMMAND" in
    start) start_supervisor; exit $? ;;
    stop) stop_supervisor; exit $? ;;
    status) status_supervisor; exit $? ;;
    finite) LIVE=0 ;;
    run) LIVE=1 ;;
    *) echo "usage: $0 [start|stop|status|run|finite]" >&2; exit 2 ;;
esac

trap cleanup EXIT
trap on_signal INT TERM

[[ -x "$LINK" ]] || fail "missing $LINK (build the release preset)"
[[ "$BENCH_CONSUMER" == gst || "$BENCH_CONSUMER" == external ]] || \
    fail "BENCH_CONSUMER must be gst or external"
if [[ "$BENCH_CONSUMER" == gst || "$LIVE" == 0 ]]; then
    [[ -x "$GST" ]] || fail "missing $GST (GStreamer development packages are required)"
fi
[[ -x "$ROOT/build/ssc338q/waybeam-link" ]] || \
    fail "missing build/ssc338q/waybeam-link (build the ssc338q preset)"
[[ "$FRAMES" =~ ^[1-9][0-9]*$ ]] || fail "FRAMES must be positive"
[[ "$TIMEOUT_MS" =~ ^[1-9][0-9]*$ ]] || fail "TIMEOUT_MS must be positive"
[[ "$LIVE" == 0 || "$LIVE" == 1 ]] || fail "LIVE must be 0 or 1"
[[ "$RX_DROP_PERMILLE" =~ ^([0-9]{1,3}|1000)$ ]] || fail "RX_DROP_PERMILLE must be 0..1000"
mkdir -p "$ARTIFACTS"
printf 'frame_shm=%s\nconsumer=%s\nartifacts=%s\n' \
    "$OUT_RING" "$BENCH_CONSUMER" "$ARTIFACTS" >"$RUNTIME_INFO"

if ! curl -fsS --max-time 1 http://127.0.0.1:8099/api/instances >/dev/null 2>&1; then
    python3 "$ROOT/tools/link_monitor.py" \
        --label "$CRAFT_IP=vehicle" --label "$GROUND_IP=ground" \
        >"$ARTIFACTS/monitor.log" 2>&1 &
    MONITOR_PID=$!
    sleep 0.2
fi
echo "live dashboard: http://$GROUND_IP:8099/"

remote 'test -x /usr/bin/waybeam-link && test -x /usr/bin/json_cli &&
        test -x /etc/init.d/S95waybeam && test -f /etc/waybeam.json' \
    || fail "craft prerequisites missing"

cat >"$ARTIFACTS/tx.json" <<EOF
{
  "node":{"originator":17,"role":"tx","preferred_originator":9},
  "profile_table":"/etc/waybeam-link/table.example.json",
  "streams":[{"stream_id":0,"stream_type":"RTP","dir":"in",
    "bind":{"kind":"frame-shm","name":"venc_frame"},
    "fec":{"scheme":"rlc256","i_rate_permille":250,
           "p_rate_permille":100,"min_k":3}}],
  "air":{"kind":"udp","tx":["$GROUND_IP:5801","$GROUND_IP:5802"],
         "rx":["0.0.0.0:5810"]},
  "policy":{"select":{"min_profile":0,"max_profile":0}},
  "stats":{"hz":5,"bind":{"kind":"udp","send":"$GROUND_IP:9110"}}
}
EOF
cat >"$ARTIFACTS/rx.json" <<EOF
{
  "node":{"originator":9,"role":"rx","preferred_originator":17},
  "profile_table":"profiles/table.example.json",
  "streams":[{"stream_id":0,"stream_type":"RTP","dir":"out",
    "originator":17,"bind":{"kind":"frame-shm","name":"$OUT_RING"}}],
  "air":{"kind":"udp","rx":["0.0.0.0:5801","0.0.0.0:5802"],
         "tx":["$CRAFT_IP:5810"],"rx_drop_permille":$RX_DROP_PERMILLE},
  "policy":{"select":{"min_profile":0,"max_profile":0}},
  "stats":{"hz":5,"bind":{"kind":"udp","send":"127.0.0.1:9110"}}
}
EOF

(cd "$ROOT" && "$LINK" rx -c "$ARTIFACTS/rx.json") \
    >"$ARTIFACTS/rx-stats.jsonl" 2>"$ARTIFACTS/rx.err" &
GROUND_PID=$!
if [[ "$LIVE" == 1 && "$BENCH_CONSUMER" == gst ]]; then
    "$GST" consume "$OUT_RING" 4294967295 4294967295 \
        >"$ARTIFACTS/consumer.log" 2>&1 &
    CONSUMER_PID=$!
elif [[ "$LIVE" == 0 ]]; then
    "$GST" consume-trace "$OUT_RING" "$FRAMES" "$TIMEOUT_MS" \
        "$ARTIFACTS/frames.csv" >"$ARTIFACTS/consumer.log" 2>&1 &
    CONSUMER_PID=$!
fi

remote_put "$ROOT/build/ssc338q/waybeam-link" /tmp/waybeam-link-jscc-dev.new
remote_put "$ARTIFACTS/tx.json" /tmp/waybeam-link-jscc-ethernet.json.new
remote "mkdir -p /etc/waybeam-link &&
        cp /tmp/waybeam-link-jscc-dev.new $REMOTE_INSTALL && chmod 0755 $REMOTE_INSTALL &&
        rm -f /tmp/waybeam-link-jscc-dev.new &&
        cp /tmp/waybeam-link-jscc-ethernet.json.new $REMOTE_CFG && chmod 0644 $REMOTE_CFG"
remote "cp /etc/waybeam.json $REMOTE_BACKUP"
REMOTE_CHANGED=1
remote "/usr/bin/json_cli -s .outgoing.server '\"frame-shm://venc_frame\"' -i /etc/waybeam.json &&
        /etc/init.d/S95waybeam restart"
remote "rm -f $REMOTE_STATS $REMOTE_ERR; setsid $REMOTE_INSTALL tx -c $REMOTE_CFG \
        >$REMOTE_STATS 2>$REMOTE_ERR </dev/null & echo \$! >$REMOTE_PID"

# The venc ring ABI has one shared read_idx. Two consumers do not fan out;
# they steal alternating frames from each other. Catch accidental viewer +
# GStreamer attachment before declaring the continuous bench ready.
sleep 0.5
assert_single_consumer

if [[ "$LIVE" == 1 ]]; then
    echo "JSCC Ethernet bench is running"
    set +e
    if [[ "$BENCH_CONSUMER" == gst ]]; then
        wait "$CONSUMER_PID"
        consumer_rc=$?
    else
        wait "$GROUND_PID"
        consumer_rc=$?
    fi
    set -e
    CONSUMER_PID=
    GROUND_PID=
    fail "continuous bench exited unexpectedly (status $consumer_rc; see $ARTIFACTS)"
fi

set +e
wait "$CONSUMER_PID"
consumer_rc=$?
set -e
CONSUMER_PID=
# Allow the 5 Hz emitters to publish counters for the final delivered frame.
sleep 0.3
kill -TERM "$GROUND_PID" 2>/dev/null || true
wait "$GROUND_PID" 2>/dev/null || true
GROUND_PID=
stop_remote_link
remote_get "$REMOTE_STATS" "$ARTIFACTS/tx-stats.jsonl"
remote_get "$REMOTE_ERR" "$ARTIFACTS/tx.err"

python3 - "$ARTIFACTS" "$FRAMES" "$RX_DROP_PERMILLE" <<'PY'
import csv
import json
import pathlib
import statistics
import sys

root = pathlib.Path(sys.argv[1])
expected = int(sys.argv[2])
injected_loss = int(sys.argv[3])

def last(path):
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    if not rows:
        raise SystemExit(f"no stats snapshots in {path}")
    return rows[-1]

tx = last(root / "tx-stats.jsonl")
rx = last(root / "rx-stats.jsonl")
frames = list(csv.DictReader((root / "frames.csv").open(encoding="utf-8")))
if len(frames) != expected:
    raise SystemExit(f"received {len(frames)} frames, expected {expected}")
arrivals = [int(row["arrival_ns"]) for row in frames]
sizes = [int(row["bytes"]) for row in frames]
idrs = sum(int(row["idr"]) for row in frames)
gaps_ms = [(b - a) / 1e6 for a, b in zip(arrivals, arrivals[1:])]
stream = rx["streams"][0]
if stream["decode_errors"] or stream["malformed"]:
    raise SystemExit(f"reassembly errors: {stream}")
if injected_loss == 0 and any(a["kernel_drop"] for a in tx["adapters"] + rx["adapters"]):
    raise SystemExit("unexpected UDP socket queue drops")
summary = {
    "frames": len(frames),
    "bytes_min": min(sizes),
    "bytes_mean": round(statistics.fmean(sizes), 1),
    "bytes_p95": sorted(sizes)[max(0, int(len(sizes) * .95) - 1)],
    "bytes_max": max(sizes),
    "idr_marked": idrs,
    "arrival_gap_ms_mean": round(statistics.fmean(gaps_ms), 3) if gaps_ms else 0,
    "arrival_gap_ms_p95": round(sorted(gaps_ms)[max(0, int(len(gaps_ms) * .95) - 1)], 3) if gaps_ms else 0,
    "frames_fast": stream["frames_fast"],
    "frames_fec": stream["recovered_fec"],
    "frames_unrecoverable": stream["frames_unrecoverable"],
    "loss_postdiv_prearq_milli": stream["loss_postdiv_prearq_milli"],
    "nacks_sent": stream["nacks_sent"],
    "resends_sent": tx["streams"][0]["resends_sent"],
}
(root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
print(json.dumps(summary, indent=2))
PY

[[ "$consumer_rc" == 0 ]] || fail "GStreamer validation failed (see $ARTIFACTS/consumer.log)"
echo "jscc ethernet bench: PASS ($ARTIFACTS)"
