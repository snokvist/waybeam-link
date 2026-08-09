#!/bin/bash
# B10 runtime verification (#109): does a FRAME actually reach the callback
# sink? Entirely localhost over the `udp` dev air backend — no radio, no
# hardware, no second repo.
#
#   tools/frame_sink_bench.sh
#
# Chain: frame_shm_feed (deterministic [VencFrameMeta][pattern] producer)
#        -> waybeam-link tx, frame-shm ingest, FEC over the udp air backend
#        -> frame_sink_probe, whose egress is a FrameSink callback.
# Pass = the probe's frame/byte totals equal the producer's, exactly.
#
# THE PAIRING IS THE TRAP. The first attempt used config.air-tx.sample.json,
# which ingests RTP datagrams, against the frame-shm RX. The RX then received
# every datagram (delivered 10799) and emitted zero frames, which looks exactly
# like a broken sink. It is not: whole-frame egress needs a TX that FRAMED the
# input, so the frame-shm tx config is the only correct partner. The control
# that settles it is stock `rx` mode — it reports frame_count 0 on the same
# feed, exonerating the sink.
set -u
cd /home/snokvist/dev/waybeam-coordination/waybeam-link
L="${TMPDIR:-/tmp}/wblink-sink-bench"
mkdir -p "$L"
: > "$L/v_tx.log"; : > "$L/v_probe.log"; : > "$L/v_feed.log"

# Producer first: it CREATES the ring the TX attaches to.
setsid ./build/dev/frame_shm_feed produce venc_frame 180 30 20000 30 \
    >"$L/v_feed.log" 2>&1 </dev/null & FEED=$!
sleep 1
setsid ./build/dev/waybeam-link tx -c examples/config.frame-shm-tx.sample.json \
    >"$L/v_tx.log" 2>&1 </dev/null & TX=$!
setsid ./build/dev/frame_sink_probe examples/config.frame-shm-rx.sample.json \
    >"$L/v_probe.log" 2>&1 </dev/null & PROBE=$!
sleep 9
for p in "$FEED" "$PROBE" "$TX"; do kill -TERM "$p" 2>/dev/null; done
sleep 2
for p in "$PROBE" "$TX"; do kill -TERM "$p" 2>/dev/null; done
sleep 1
echo "=== producer ==="; grep -E "producer" "$L/v_feed.log" | tail -2
echo "=== tx stream ==="; tail -1 "$L/v_tx.log" | python3 -c "
import sys,json; d=json.loads(sys.stdin.readline()); s=d['streams'][0]
print({k:s[k] for k in ('delivered','frame_count','source_symbols_sent','idr_frames')}, 'link=',d['link']['state'])" 2>/dev/null
echo "=== probe ==="; grep -E "FIRST FRAME|summary|probe:   stream|NO FRAMES" "$L/v_probe.log" | tail -8
echo "=== probe 15.3 stream line ==="; grep '^{' "$L/v_probe.log" | tail -1 | python3 -c "
import sys,json
ln=sys.stdin.readline()
if not ln.strip(): print('(no stats line)'); raise SystemExit
d=json.loads(ln); s=d['streams'][0]
print({k:s[k] for k in ('delivered','uniq','frame_count','frame_bytes','frame_size_last','frame_interval_us','frame_jitter_us')})" 2>/dev/null
