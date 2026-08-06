#!/usr/bin/env bash
set -u
SP="$(cd "$(dirname "$0")" && pwd)"
REPO=/home/snokvist/dev/waybeam-coordination/waybeam-link
sudo pkill -TERM -f 'wl-(wide|scoped) (tx|rx) -c' 2>/dev/null; sleep 3
cd "$REPO"
sudo setsid "$SP/wl-wide" tx -c "$SP/tx.json" >/dev/null 2>"$SP/d-tx.log" </dev/null &
disown; sleep 6
sudo setsid "$SP/wl-wide" rx -c "$SP/rx.json" >/dev/null 2>"$SP/d-rx.log" </dev/null &
disown; sleep 7
(cd "$REPO" && python3 tools/rtp_feed.py 30 3000 60 >/dev/null 2>&1 &)
sleep 3
curl -s --max-time 5 -X POST localhost:8099/api/v1/scout/start -d '{"channels":[5805],"dwell_ms":9000}' >/dev/null
sleep 9
echo "--- craft ---"; curl -s --max-time 5 localhost:8091/api/v1/stats 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print('tx_submitted', sum(a['tx_submitted'] for a in d['adapters']))" 2>/dev/null || echo "craft has no control port"
echo "--- ground ---"; curl -s --max-time 5 localhost:8099/api/v1/stats | python3 -c "
import json,sys
d=json.load(sys.stdin)
print('adapters rx  ', sum(a['rx'] for a in d['adapters']), ' filtered', sum(a['filtered'] for a in d['adapters']))
print('link         ', {k:d['link'][k] for k in ('target_originator','state','selector_state_valid')})
print('streams      ', [(s.get('stream_id'), s.get('delivered', s.get('rx_packets'))) for s in d.get('streams',[])])"
sudo pkill -TERM -f 'wl-(wide|scoped) (tx|rx) -c' 2>/dev/null
