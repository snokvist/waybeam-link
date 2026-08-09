# Known-good verification hardware

This is the reference kernel-monitor test rig as verified on 2026-07-22. Keep
the addresses, originators, channel, and interface names aligned with the
deployment assets in `deploy/` when using this setup for regression or flight
testing.

## Topology

| Node | Address / originator | Hardware and radio | Runtime |
|---|---|---|---|
| Vehicle | `192.168.2.232` / 17 | OpenIPC SSC338Q (`INFINITY6E SSC012B-S01A`), RTL8812EU `0bda:a81a`, `wlan0`, `8812eu` | `/usr/bin/waybeam-link`, `/etc/waybeam-link/craft.json`, `/etc/init.d/waybeam-link` |
| x86 ground | `192.168.2.242` / 9 | AMD Ryzen 5 7640HS, RTL8812EU `0bda:a81a` (`wlx84fc1450bcde`, `rtl88x2eu`) and RTL8812CU `0bda:c812` (`wlx40a5ef2f2308`, `rtl88x2cu`) | `/usr/local/bin/waybeam-link`, `/etc/waybeam-link/ground.json`, `waybeam-ground.service` (removed in Pass 164 — the node is driven manually, see `deploy/README.md`) |
| RK3566 ground | `192.168.2.199` / 10 | Radxa ZERO 3, RTL8812CU `0bda:c812`, `wlx40a5ef2f229b`, `rtl88x2cu` | `/usr/bin/waybeam-link`, `/etc/waybeam-link/ground.json`, `/etc/init.d/S49waybeam-link` (unit file removed from this repo in Pass 164) |
| Ethernet cache | `192.168.2.247` / 33 | Intel Celeron J4125 Debian host, MT7921 `14c3:7961` (`wlp3s0`, `mt7921e`) and RTL8812AU `0bda:8812` (`wlx200db0c4a76a`, `rtl88xxau_wfb`) | `/usr/local/bin/waybeam-link`, `/etc/waybeam-link/cache.json`, `waybeam-cache.service` (removed from this repo in Pass 164) |

All radios use channel 161, 5805 MHz, 20 MHz bandwidth, and Waybeam network
ID 0. The vehicle currently sends whole encoded frames from `venc_frame` with
profile/MCS 3. Both ground nodes write `venc_frame_out`. The RK3566 unit runs
`waybeam_hub` after the link and decodes that SHM stream through RK MPP to DRM
HDMI with schema-3 OSD.

The vehicle RF broadcast is received concurrently by both ground nodes and by
both cache adapters. The cache stores stream 0 and sends status and requested
repair symbols over Ethernet/UDP from `192.168.2.247:5801` to the requester's
source endpoint. The x86 and RK3566 grounds listen on `.242:5802` and
`.199:5802` respectively. Their distinct originators and sessions isolate the
two repair consumers. Originator 9 remains the preferred ground control node;
the RK3566 verification receiver does not become authoritative.

The cache radios are receive-only in this setup. RF-inserted cache replies are
not implemented yet.

## Monitor-frame FCS rule

Kernel monitor drivers do not agree on whether a captured 802.11 frame retains
its trailing four-byte FCS. Never remove four bytes unconditionally.

`RadiotapRx::fcs_at_end` must be derived from bit `0x10` of the radiotap FLAGS
field. Strip exactly four bytes only when that bit is set; strip none when it
is clear or FLAGS is absent. The BADFCS bit does not determine frame length.
Waybeam's declared wire length is then validated strictly against the remaining
payload.

This rig exercises both cases:

- The MT7921 on the cache omits the FCS and clears the radiotap FCS-at-end bit.
- The RTL8812CU on the x86 ground has been observed with radiotap FLAGS `0x90`
  and the four-byte FCS present.

Keep the paired FCS-present/FCS-absent cases in `radiotap_test` and run the full
test suite after any radiotap, monitor RX, BPF, or 802.11 parsing change. A live
check must also show increasing RX counts without malformed frames on at least
one adapter of each behavior.

## Verified startup behavior

On the 2026-07-22 cold boot, `waybeam-cache.service` initially started before
`192.168.2.247` had been assigned. Its unlimited restart policy retried four
times and became active as soon as the address appeared. Both radios then
received immediately, the cache reached 96 held blocks and health 1000, and
both grounds reported one fresh cache. This retry behavior is intentional and
must not be replaced with a one-shot interface condition.

The MT7921 monitor setup can log `mtu greater than device maximum`; the setup
script tolerates this and continues. Treat it as benign only if the interface
RX counter subsequently increases.

The RTL8812AU driver currently reports RSSI as `-128` even while its RX counter
increases. Use RX progress and kernel-drop counters, not that RSSI value alone,
to judge the AU cache path.

## Verification checklist

1. Confirm all four nodes are reachable and the expected interfaces exist.
2. Confirm every monitor interface is on 5805 MHz/20 MHz and each link/cache
   process is running exactly once.
3. Read the cache counters:

   ```sh
   ssh snokvist@192.168.2.247 \
     'systemctl is-active waybeam-cache; wget -qO- http://127.0.0.1:8091/api/v1/stats'
   ```

   Both `mt-cache` and `au-cache` RX counters must increase. Expect
   `blocks_held: 96`, `requests_rejected: 0`, and health at or above the
   configured `health_floor_permille` (800). Healthy short windows are normally
   near 1000 but can fluctuate slightly as block coverage changes.

4. Read both ground counters:

   ```sh
   curl -fsS http://127.0.0.1:8092/api/v1/stats
   ssh root@192.168.2.199 \
     'wget -qO- http://127.0.0.1:8092/api/v1/stats'
   ```

   Require `caches_fresh: 1`, advancing stream/frame counters, no malformed
   growth, and no active adapter marked `adapter_stalled` or `tx_wedged`.
   Visible video is not sufficient: one x86 diversity adapter can stall while
   the other continues delivering a clean picture. Restart and re-check the
   ground service if either configured x86 adapter is stalled.

5. Confirm the x86 SHM consumer and RK3566 `waybeam_hub` remain attached to
   `venc_frame_out`, with no sustained SHM backlog or full-drop growth.
6. Before flight, perform a props-off power-cycle test and an
   antenna-separated walking/range test while watching post-diversity loss,
   unrecoverable frames, cache repair latency, driver/kernel drops,
   temperature, and end-to-end latency.

This remains a video verification link. Flight control and failsafe must not
depend on the RF video path or the Ethernet cache.
