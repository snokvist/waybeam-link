#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Shared helpers for the Pass 125/126 §10.7 hardware campaign.
#
# Every bench process is killed from a FILE, never an interactive shell: a
# `pkill -f 'build/dev/waybeam-link'` typed at a prompt matches its own
# cmdline and kills the shell (exit 144). SIGTERM only — SIGKILL on SigmaStar
# leaves MI_SYS zombies that need a power cycle.
#
# All device output is filtered before it reaches a caller's stdout. Raw
# multi-thousand-line NDJSON is never printed; the analyzers summarize.

CRAFT=${CRAFT:-192.168.2.232}
GROUND=${GROUND:-192.168.2.199}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=6 -o StrictHostKeyChecking=no"
EVID=${EVID:-$(cd "$(dirname "$0")/../../docs/data/pass126-hw" && pwd)}

craft() { $SSH "root@$CRAFT" "$@"; }
ground() { $SSH "root@$GROUND" "$@"; }

# The craft/ground control servers bind loopback, so REST goes through ssh.
capi()  { craft  "curl -s --max-time 8 $*"; }
gapi()  { ground "curl -s --max-time 8 $*"; }

log()  { printf '%s  %s\n' "$(date -u +%H:%M:%S)" "$*"; }
fail() { printf '%s  FAIL: %s\n' "$(date -u +%H:%M:%S)" "$*"; HW_FAIL=$((HW_FAIL+1)); }
pass() { printf '%s  pass: %s\n' "$(date -u +%H:%M:%S)" "$*"; HW_PASS=$((HW_PASS+1)); }

# check <label> <expected> <actual>
check() {
    if [ "$2" = "$3" ]; then pass "$1 ($3)"; else fail "$1: want '$2' got '$3'"; fi
}
# contains <label> <needle> <haystack>
contains() {
    case "$3" in
        *"$2"*) pass "$1" ;;
        *) fail "$1: '$2' not in '$(echo "$3" | head -c 200)'" ;;
    esac
}

hw_summary() {
    printf '\n=== %s: %d passed, %d failed ===\n' "$1" "${HW_PASS:-0}" "${HW_FAIL:-0}"
    [ "${HW_FAIL:-0}" -eq 0 ]
}

# Deploy a binary without scp (dead on these targets) — cat-pipe, verify the
# backup exists BEFORE overwriting, and compare sizes after.
deploy_bin() {
    host=$1; src=$2; dst=$3
    want=$(stat -c %s "$src")
    $SSH "root@$host" "[ -f $dst ] && cp -a $dst $dst.bak-\$(date +%Y%m%d-%H%M%S) && ls -la $dst.bak-* | tail -1" || return 1
    $SSH "root@$host" "cat > $dst.new && chmod 755 $dst.new" < "$src" || return 1
    got=$($SSH "root@$host" "stat -c %s $dst.new")
    [ "$want" = "$got" ] || { echo "SIZE MISMATCH want=$want got=$got"; return 1; }
    $SSH "root@$host" "mv $dst.new $dst && ls -la $dst"
}
