#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# P0a — §10.6 craft calibration regression.
#
# Pass 125 changed the bottom of every rung-0 ramp (the floor rule: a bad probe
# at min_qdb with no clean probe now ASCENDS instead of descending into a wall)
# and Pass 126/W9 changed what the addendum-4 blackout retreat records as the
# rung's overload bracket. §10.7 promised §10.6 is behaviourally unchanged
# ABOVE the first clean probe. This measures that promise against the last
# known-good artifact rather than asserting it.
#
# Deploys the new craft binary (with backup), restarts via the init script,
# waits for the link to re-latch, runs one full 8-rung campaign, and diffs.
set -u
. "$(dirname "$0")/lib.sh"
HW_PASS=0; HW_FAIL=0
BIN="$(dirname "$0")/../../build/ssc338q/waybeam-link"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$EVID/p0a-$STAMP"
mkdir -p "$OUT"

log "P0a: craft §10.6 regression vs baseline"
log "  baseline: $EVID/baseline-craft-artifact.json"

if [ "${SKIP_DEPLOY:-0}" != "1" ]; then
    # Stop FIRST: overwriting a running ELF is "Text file busy". SIGTERM via
    # the init script — never SIGKILL on SigmaStar, which leaves MI_SYS
    # zombies that only a power cycle clears.
    log "  stopping craft link"
    craft "/etc/init.d/S96waybeam-link stop 2>&1 | tail -2"
    log "  deploying $(basename "$BIN")"
    if ! deploy_bin "$CRAFT" "$BIN" /usr/bin/waybeam-link; then
        fail "deploy — restoring original and restarting"
        restore_bin "$CRAFT" /usr/bin/waybeam-link
        craft "/etc/init.d/S96waybeam-link start 2>&1 | tail -2"
        hw_summary P0a; exit 1
    fi
    log "  starting craft link"
    craft "/etc/init.d/S96waybeam-link start 2>&1 | tail -3"
fi

log "  waiting for the craft to re-latch a reporter (up to 60 s)"
i=0
while [ $i -lt 30 ]; do
    lat=$(capi "http://127.0.0.1:8091/api/v1/stats" 2>/dev/null |
          sed -n 's/.*"report_latch_holder":\([0-9]*\).*/\1/p')
    [ -n "${lat:-}" ] && [ "$lat" != "0" ] && break
    i=$((i+1)); sleep 2
done
[ -n "${lat:-}" ] && [ "$lat" != "0" ] && pass "report latch held by $lat" || fail "no report latch after 60 s"

# §10.6 is started by a §11.7 CALIBRATE campaign issued FROM THE GROUND — the
# craft's /api/v1/calibration is GET-only (the POST route is the ground's own
# §10.7). This is the same seam CalibSequencer drives for the downlink phase,
# so P0a exercises it too.
log "  issuing §11.7 CALIBRATE from the ground (the real operator path)"
r=$(gapi -X POST -d '{"cmd":"calibrate","arg":1}' "$G/vehicle/command")
echo "    $r"
case "$r" in
    *'"ok":true'*|*'"state"'*) pass "CALIBRATE accepted" ;;
    *) fail "CALIBRATE rejected: $r"; hw_summary P0a; exit 1 ;;
esac

# Spec sizes a full 8-rung run at the default dwells at ~2 min; the 600 s hard
# cap is a runaway backstop, not the expected duration. Poll for 4 min and
# report elapsed + rung so a stall reads as a stall, not as a slow test.
log "  waiting for the craft to finish (expect ~2 min)"
t0=$(date +%s); i=0; state=
while [ $i -lt 80 ]; do
    st=$(capi "http://127.0.0.1:8091/api/v1/calibration" 2>/dev/null)
    state=$(echo "$st" | sed -n 's/.*"state":"\([a-z_]*\)".*/\1/p')
    rung=$(echo "$st" | sed -n 's/.*"rung":\([0-9]*\).*/\1/p')
    el=$(( $(date +%s) - t0 ))
    case "$state" in
        running) printf '\r    t=%3ds  rung %s      ' "$el" "${rung:-?}" ;;
        done|failed) printf '\r    t=%3ds  %s        \n' "$el" "$state"; break ;;
        *) printf '\r    t=%3ds  state=%s   ' "$el" "${state:-<none>}" ;;
    esac
    i=$((i+1)); sleep 3
done
echo
check "§10.6 terminal state" "done" "${state:-timeout}"
# §10.6 (Pass 195): one artifact per adapter identity. Resolve the file from
# the craft rather than naming it — a hardcoded `artifact.json` still EXISTS on
# an upgraded craft (nothing removes it) while no longer being written, so this
# gate would compare the pre-upgrade file and pass regardless of what the run
# under test produced. Newest wins, so a re-run of the same unit is picked up.
CALIB_FILE=$(craft "ls -t /etc/waybeam-link/calibration/artifact-*.json 2>/dev/null | head -1")
if [ -z "$CALIB_FILE" ]; then
    log "  FAIL: no artifact-<identity>.json on the craft after a completed run"
    HW_FAIL=$((HW_FAIL+1))
    CALIB_FILE=/nonexistent
fi
log "  artifact: $CALIB_FILE"
craft "cat $CALIB_FILE" > "$OUT/artifact.json" 2>/dev/null
capi "http://127.0.0.1:8091/api/v1/calibration" > "$OUT/calibration-response.json" 2>/dev/null

python3 "$(dirname "$0")/compare_calib.py" \
    "$EVID/baseline-craft-artifact.json" "$OUT/artifact.json" || HW_FAIL=$((HW_FAIL+1))

log "  evidence: $OUT"
hw_summary "P0a"
