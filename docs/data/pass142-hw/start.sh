#!/usr/bin/env bash
# start.sh <arm>   — bring up the mismatched pair, nothing else.
set -u
SP="$(cd "$(dirname "$0")" && pwd)"
REPO=/home/snokvist/dev/waybeam-coordination/waybeam-link
sudo pkill -TERM -f 'wl-(lossy|after) (tx|rx) -c' 2>/dev/null
sleep 3
cd "$REPO"
sudo setsid "$SP/wl-$1" tx -c "$SP/tx.json" >/dev/null 2>"$SP/$1-tx.log" </dev/null &
disown; sleep 6
sudo setsid "$SP/wl-$1" rx -c "$SP/rx-anynet.json" >/dev/null 2>"$SP/$1-rx.log" </dev/null &
disown; sleep 7
curl -s --max-time 5 localhost:8099/api/v1/stats >/dev/null && echo "$1 pair up" || echo "$1 FAILED"
