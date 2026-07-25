#!/bin/sh
# apply-mode.sh <mode-name> — apply one operating mode (docs/venc-mode-matrix.md
# §16) on the craft. This is the on-craft applier that waybeam-link forks when
# the hub POSTs /api/v1/mode (Pass 96); it can also be run by hand.
#
# What a "mode" is: three venc fields (sensor.mode, video0.size, video0.fps —
# sensor.mode and video0.size are restart_required) plus the link range pin
# (policy.select.min_profile/max_profile). All five live in one file,
# modes/<name>.json, so the matrix has a single source of truth.
#
# The range pin is applied LIVE through the link's own /api/v1/link/profile
# endpoint, so the link and CSA never restart and no re-pair (B9) is needed.
# Only venc restarts, which briefly interrupts video.
#
# Persistence: the venc fields and the link pin are also written to disk
# (json_cli, style-preserving) so a reboot reproduces the mode. The link is the
# authority for the active-mode label; venc.active_mode is written to craft.json
# for the same reason.
set -u

NAME="${1:?usage: apply-mode.sh <mode-name>}"
MODES_DIR="${MODES_DIR:-/etc/waybeam-link/modes}"
VENC_CFG="${VENC_CFG:-/etc/waybeam.json}"
LINK_CFG="${LINK_CFG:-/etc/waybeam-link/craft.json}"
LINK_CTRL="${LINK_CTRL:-127.0.0.1:8091}"
JSON_CLI="${JSON_CLI:-json_cli}"
VENC_INIT="${VENC_INIT:-/etc/init.d/S95waybeam}"

MODE_JSON="$MODES_DIR/$NAME.json"
[ -f "$MODE_JSON" ] || { echo "apply-mode: no such mode: $MODE_JSON" >&2; exit 2; }

# Reject anything but [A-Za-z0-9._-] — this name indexes a file and is passed
# around; keep it boring.
case "$NAME" in
  *[!A-Za-z0-9._-]*) echo "apply-mode: bad mode name: $NAME" >&2; exit 2 ;;
esac

get() { "$JSON_CLI" -g "$1" -i "$MODE_JSON" --raw 2>/dev/null; }

SENSOR_MODE=$(get .venc.sensor.mode)
FPS=$(get .venc.video0.fps)
SIZE=$(get .venc.video0.size)
RESILIENCE=$(get .venc.video0.resilience)   # GDR intra-refresh vs GOP; optional
MINP=$(get .link.policy.select.min_profile)
MAXP=$(get .link.policy.select.max_profile)
FPS_MODE=$(get .link.policy.fps_mode)        # static|variable; §9.11 Pass 99
[ -n "$FPS_MODE" ] || FPS_MODE=static        # default: fps pinned, recordable

for v in "$SENSOR_MODE" "$FPS" "$SIZE" "$MINP" "$MAXP"; do
  [ -n "$v" ] || { echo "apply-mode: $MODE_JSON missing a field" >&2; exit 2; }
done

echo "apply-mode: $NAME -> sensor.mode=$SENSOR_MODE video0=${SIZE}@${FPS} resilience=${RESILIENCE:-default} MCS ${MINP}-${MAXP} fps=${FPS_MODE}"

# §9.11 ladder toggle, LIVE through the link (Pass 99). Same lever as the
# over-air §11.7 FPS_LADDER command; no link/CSA restart. Best-effort.
link_fps() {  # $1 = true|false
  curl -sS --max-time 4 -X POST "http://$LINK_CTRL/api/v1/link/fps" \
    -H 'Content-Type: application/json' -d "{\"ladder\":$1}" >/dev/null 2>&1 \
    && echo "apply-mode: live fps ladder=$1" \
    || echo "apply-mode: WARN fps ladder=$1 not applied (link down?)" >&2
}

# STATIC: stop the ladder BEFORE the venc restart pins video0.fps, else a
# still-running loop would fight the restart. VARIABLE: turn it on AFTER, so it
# resumes from a known rung with cleared evidence (§9.11 settle).
if [ "$FPS_MODE" = "static" ]; then
  link_fps false
fi

# 1) Range pin, LIVE through the link (no restart). Best-effort: if the link is
#    down this is a no-op and the persisted pin below takes effect on next start.
curl -sS --max-time 4 -X POST "http://$LINK_CTRL/api/v1/link/profile" \
  -H 'Content-Type: application/json' \
  -d "{\"min\":$MINP,\"max\":$MAXP}" >/dev/null 2>&1 \
  && echo "apply-mode: live pin applied ${MINP}-${MAXP}" \
  || echo "apply-mode: WARN live pin not applied (link down?) — persisted only" >&2

# 2) Persist the pin + active-mode label into the link config for reboot.
"$JSON_CLI" -s .policy.select.min_profile "$MINP" -i "$LINK_CFG"
"$JSON_CLI" -s .policy.select.max_profile "$MAXP" -i "$LINK_CFG"
"$JSON_CLI" -s .venc.active_mode "\"$NAME\"" -i "$LINK_CFG"

# 2b) Persist the §9.11 boot run-state so a reboot reproduces the mode without
#     the applier re-running (Pass 99). enabled=true iff fps_mode=variable.
if [ "$FPS_MODE" = "variable" ]; then
  "$JSON_CLI" -s .venc.fps_ladder.enabled true -i "$LINK_CFG"
else
  "$JSON_CLI" -s .venc.fps_ladder.enabled false -i "$LINK_CFG"
fi

# 3) Persist the venc fields. sensor.mode/size/resilience are restart_required.
#    For a variable mode video0.fps is just the boot seed; the ladder floats it.
"$JSON_CLI" -s .sensor.mode "$SENSOR_MODE" -i "$VENC_CFG"
"$JSON_CLI" -s .video0.size "\"$SIZE\"" -i "$VENC_CFG"
"$JSON_CLI" -s .video0.fps "$FPS" -i "$VENC_CFG"
[ -n "$RESILIENCE" ] && "$JSON_CLI" -s .video0.resilience "\"$RESILIENCE\"" -i "$VENC_CFG"

# 4) Restart venc so sensor.mode/size take effect. The link keeps running and
#    re-acquires the new video stream; it owns bitrate/fps live from here.
"$VENC_INIT" restart

# 5) VARIABLE: hand fps back to the ladder now that venc is up at the seed fps.
if [ "$FPS_MODE" = "variable" ]; then
  link_fps true
fi

echo "apply-mode: $NAME applied"
