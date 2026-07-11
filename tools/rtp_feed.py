#!/usr/bin/env python3
"""Rate-parametrized synthetic H.265 RTP feeder (gate 3/4 saturation runs).

Frame-structured: <fps> access units per second, each of <pps>/<fps>
packets; the LAST packet of each AU carries the M bit (EOB path fires at
the real video rate, unlike rtp-feed.py's fixed every-30th marking).
One IDR AU per second (NAL 19 on its first packet), the rest NAL 1.

Usage: rtp-feed2.py <duration-s> <pps> [fps=60]
"""
import socket
import struct
import sys
import time

DST = ("127.0.0.1", 5600)
PAYLOAD = 1188  # + 12 RTP header = 1200 B

dur = float(sys.argv[1])
pps = int(sys.argv[2])
fps = int(sys.argv[3]) if len(sys.argv) > 3 else 60
per_au = max(1, pps // fps)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
seq = 0
ts = 0
au_pkt = 0
au_idx = 0
ssrc = 0x57424C4B
deadline = time.monotonic()
sent = 0
end = deadline + dur
while time.monotonic() < end:
    last_of_au = au_pkt == per_au - 1
    marker = 0x80 if last_of_au else 0
    hdr = struct.pack("!BBHII", 0x80, marker | 98, seq & 0xFFFF, ts, ssrc)
    nal_type = 19 if (au_pkt == 0 and au_idx % fps == 0) else 1
    body = bytes([nal_type << 1, 0x01]) + bytes(PAYLOAD - 2)
    sock.sendto(hdr + body, DST)
    seq += 1
    sent += 1
    if last_of_au:
        ts += 90000 // fps
        au_pkt = 0
        au_idx += 1
    else:
        au_pkt += 1
    deadline += 1.0 / pps
    slack = deadline - time.monotonic()
    if slack > 0:
        time.sleep(slack)
print(f"rtp-feed2: sent {sent} packets ({pps} pps, {fps} fps)",
      file=sys.stderr)
