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

# Topology: the x86 host running this script IS the ground of the main pair
# (originator 9, holds the craft's §3.5 report latch, owns the designated
# `eu-uplink` role:"tx" adapter). The craft is the SSC338Q at .2.232
# (originator 17). The RK3566 at .2.199 is a spectator — a silent watcher that
# receives but never reports — and is deliberately NOT reconfigured by this
# campaign; it is an independent observer for P8/P9.
CRAFT=${CRAFT:-192.168.2.232}
WATCHER=${WATCHER:-192.168.2.199}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=6 -o StrictHostKeyChecking=no"
EVID=${EVID:-$(cd "$(dirname "$0")/../../docs/data/pass126-hw" && pwd)}
GBIN=${GBIN:-/usr/local/bin/waybeam-link}
GCFG=${GCFG:-/etc/waybeam-link/ground.json}

craft()   { $SSH "root@$CRAFT" "$@"; }
watcher() { $SSH "root@$WATCHER" "$@"; }
ground()  { sh -c "$*"; }             # local

# Both control servers bind loopback: the craft's over ssh, the ground's here.
capi() { craft "curl -s --max-time 8 $*"; }
gapi() { curl -s --max-time 8 "$@"; }
G=http://127.0.0.1:8092/api/v1

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

# Deploy a binary without scp (dead on these targets) — cat-pipe.
#
# The backup goes OFF-DEVICE, not beside the target. The SSC338Q rootfs is a
# 5.7 MB overlay with ~1.5 MB free; a 2.7 MB `cp` beside the binary fills it and
# the following write dies half-done, leaving a truncated binary and no space to
# repair it. Pull the original to $EVID, verify md5 against the device, and only
# then overwrite in place — an in-place truncate+write peaks at one copy.
deploy_bin() {
    host=$1; src=$2; dst=$3
    keep="$EVID/$(basename "$dst").orig-$host"
    if [ ! -s "$keep" ]; then
        $SSH "root@$host" "cat $dst" > "$keep" 2>/dev/null || { echo "backup pull failed"; return 1; }
        a=$(md5sum < "$keep" | cut -d' ' -f1)
        b=$($SSH "root@$host" "md5sum $dst" | cut -d' ' -f1)
        [ "$a" = "$b" ] || { echo "BACKUP MD5 MISMATCH $a != $b"; rm -f "$keep"; return 1; }
        echo "    rollback copy: $keep ($a)"
    else
        echo "    rollback copy already held: $keep"
    fi
    $SSH "root@$host" "cat > $dst && chmod 755 $dst" < "$src" || return 1
    want=$(md5sum < "$src" | cut -d' ' -f1)
    got=$($SSH "root@$host" "md5sum $dst" | cut -d' ' -f1)
    [ "$want" = "$got" ] || { echo "DEPLOY MD5 MISMATCH want=$want got=$got"; return 1; }
    echo "    deployed $dst ($got)"
}

# Restore a deployed binary from the off-device rollback copy.
restore_bin() {
    host=$1; dst=$2
    keep="$EVID/$(basename "$dst").orig-$host"
    [ -s "$keep" ] || { echo "no rollback copy at $keep"; return 1; }
    $SSH "root@$host" "cat > $dst && chmod 755 $dst" < "$keep"
    echo "    restored $dst from $keep"
}
