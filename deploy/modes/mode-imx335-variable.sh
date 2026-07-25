#!/bin/sh
# imx335-variable — thin wrapper. See deploy/modes/apply-mode.sh and
# docs/venc-mode-matrix.md §16.6 for what this mode is. VFR, live-view only.
exec "$(dirname "$0")/apply-mode.sh" "imx335-variable" "$@"
