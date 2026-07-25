#!/bin/sh
# imx335-100fps-lowrange — thin wrapper. See deploy/modes/apply-mode.sh and
# docs/venc-mode-matrix.md §16 for what this mode is.
exec "$(dirname "$0")/apply-mode.sh" "imx335-100fps-lowrange" "$@"
