#!/bin/sh
# apply-mode.sh <mode-name> — apply one operating mode (docs/venc-mode-matrix.md
# §16) on the craft. This is the on-craft applier that waybeam-link forks when
# the hub POSTs /api/v1/mode (Pass 96).
#
# SUPPORTED ENTRY IS `POST /api/v1/mode` (the hub / control plane), NOT this
# script. Running it by hand is BENCH-ONLY: it works (it self-reasserts the
# encoder against the link at the end, step 6, Pass 103), but the link owns the
# active-mode label and the production flow goes through /api/v1/mode.
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
#
# Every path and both mechanisms below are env-overridable, because the crafts
# genuinely differ: the CV610 board ships no curl (busybox wget only) and cannot
# survive its init script's stop(). See VENC_RESTART and http_post().
set -u

NAME="${1:?usage: apply-mode.sh <mode-name>}"
MODES_DIR="${MODES_DIR:-/etc/waybeam-link/modes}"
VENC_CFG="${VENC_CFG:-/etc/waybeam.json}"
LINK_CFG="${LINK_CFG:-/etc/waybeam-link/craft.json}"
LINK_CTRL="${LINK_CTRL:-127.0.0.1:8091}"
JSON_CLI="${JSON_CLI:-json_cli}"
VENC_INIT="${VENC_INIT:-/etc/init.d/S95waybeam}"
VENC_CTRL="${VENC_CTRL:-127.0.0.1:80}"      # venc's own HTTP API
# How step 4 makes sensor.mode/size take effect: `init` (default) or `api`.
#
# `init` is right for the SigmaStar crafts, whose init script restarts the
# daemon and nothing else. The CV610 craft MUST set `api`: its S95waybeam
# stop() ends in `$LOADER stop`, which unloads the ~25 open_* MPP modules and
# takes the board off the network — a restart in flight would be
# unrecoverable. venc's own POST /api/v1/restart re-execs the daemon without
# touching the loader, which is the seam the CV610 init script's own header
# documents ("API restart forks a short-lived waybeam-resp helper").
VENC_RESTART="${VENC_RESTART:-init}"

# Per-craft overrides. §15.5 forks this applier with execl(cmd, cmd, name) —
# no shell, no environment — so a craft that needs different mechanisms has
# nowhere else to say so. Sourced AFTER the defaults and therefore
# authoritative, matching the PLATFORM_CONFIG pattern S95waybeam already uses
# on these boards. Bench override: APPLY_MODE_CONF=/dev/null.
APPLY_MODE_CONF="${APPLY_MODE_CONF:-$MODES_DIR/apply-mode.conf}"
# shellcheck disable=SC1090
[ -r "$APPLY_MODE_CONF" ] && . "$APPLY_MODE_CONF"

MODE_JSON="$MODES_DIR/$NAME.json"
[ -f "$MODE_JSON" ] || { echo "apply-mode: no such mode: $MODE_JSON" >&2; exit 2; }

# Reject anything but [A-Za-z0-9._-] — this name indexes a file and is passed
# around; keep it boring.
case "$NAME" in
  *[!A-Za-z0-9._-]*) echo "apply-mode: bad mode name: $NAME" >&2; exit 2 ;;
esac

# Checked HERE, not at step 4 where it is used: everything between writes the
# venc and link configs, so a bad value discovered later would leave the craft
# persisted into a mode it never restarted into.
case "$VENC_RESTART" in
  init|api) ;;
  *) echo "apply-mode: bad VENC_RESTART '$VENC_RESTART' (want init|api)" >&2
     exit 2 ;;
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

# One POST, whichever HTTP client the craft has. The CV610 board ships no curl
# at all (busybox 1.36.1 wget is the only client on it), and every call below is
# best-effort — the persisted config reproduces the mode on next start — so this
# returns the client's status and never aborts the apply.
# curl's -f is what makes the two clients agree. Without it curl exits 0 on an
# HTTP 4xx/5xx while wget exits nonzero, so the identical server rejection would
# print "live pin applied" on a curl craft and "WARN not applied" on a wget one.
http_post() {  # $1 = full URL, $2 = json body
  if command -v curl >/dev/null 2>&1; then
    curl -fsS --max-time 4 -X POST "$1" \
      -H 'Content-Type: application/json' -d "$2" >/dev/null 2>&1
  else
    wget -q -O /dev/null -T 4 --header 'Content-Type: application/json' \
      --post-data "$2" "$1" >/dev/null 2>&1
  fi
}

http_get() {  # $1 = full URL; only used as a liveness probe
  if command -v curl >/dev/null 2>&1; then
    curl -fsS --max-time 2 "$1" >/dev/null 2>&1
  else
    wget -q -O /dev/null -T 2 "$1" >/dev/null 2>&1
  fi
}

# §9.11 ladder toggle, LIVE through the link (Pass 99). Same lever as the
# over-air §11.7 FPS_LADDER command; no link/CSA restart. Best-effort.
link_fps() {  # $1 = true|false
  http_post "http://$LINK_CTRL/api/v1/link/fps" "{\"ladder\":$1}" \
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
http_post "http://$LINK_CTRL/api/v1/link/profile" "{\"min\":$MINP,\"max\":$MAXP}" \
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
#    Unlike every other HTTP call here this one is NOT best-effort: if the
#    encoder never restarts, the persisted sensor.mode/size never take effect
#    and the mode has silently not been applied.
case "$VENC_RESTART" in
  init)
    "$VENC_INIT" restart
    ;;
  api)
    # GET, not POST: waybeam_venc's httpd registers EVERY route as GET,
    # /api/v1/restart included (venc_api.c "venc_httpd_route(\"GET\", ...)"),
    # so a POST here 405s and the encoder never picks up the new
    # sensor.mode/size. Measured on .181 — the guard below caught it, but the
    # mode was silently not applied until this was a GET.
    if ! http_get "http://$VENC_CTRL/api/v1/restart"; then
      echo "apply-mode: venc API restart FAILED — mode not applied" >&2
      exit 3
    fi
    echo "apply-mode: venc re-exec requested via $VENC_CTRL"
    # Wait for the successor, do not guess at it. The POST returns while the
    # old process is still re-exec'ing, and step 6's reassert is what takes the
    # fresh encoder off its persisted-config bitrate — fire it into a dead
    # httpd and the craft sits at the seed. /api/v1/modes answers only once
    # bring-up reached init, which is exactly the readiness we need.
    i=0
    while [ "$i" -lt 30 ] && ! http_get "http://$VENC_CTRL/api/v1/modes"; do
      sleep 1
      i=$((i + 1))
    done
    if [ "$i" -ge 30 ]; then
      echo "apply-mode: WARN venc did not answer within 30s — the mode is" \
           "persisted but the encoder may be stranded at its config bitrate" >&2
    fi
    ;;
esac

# 5) VARIABLE: hand fps back to the ladder now that venc is up at the seed fps.
if [ "$FPS_MODE" = "variable" ]; then
  link_fps true
fi

# 6) Re-assert the encoder against the link (§15.5 Pass 103). The venc restart
#    in step 4 discarded the encoder's LIVE bitrate/caps/fps; the link's
#    write-on-change actuator still believes it holds them and would NOT
#    re-push, stranding the fresh encoder at its persisted-config bitrate
#    (worst on a same-band switch, where the pin — and so the derived bitrate —
#    did not change). This POST drops that cache so the link re-asserts on its
#    next tick. Best-effort: if the link is down the persisted config already
#    reproduces the mode on next start.
http_post "http://$LINK_CTRL/api/v1/venc/reassert" '{}' \
  && echo "apply-mode: link re-asserted encoder" \
  || echo "apply-mode: WARN venc/reassert not applied (link down?)" >&2

echo "apply-mode: $NAME applied"
