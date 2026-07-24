# MVP flight-test deployment

Current four-node kernel-monitor verification topology:

| Node | Address | RF role | Service/config |
|---|---|---|---|
| Vehicle | `192.168.2.232` | RTL8812EU TX/RX, channel 161 HT20 | `vehicle-waybeam-link.init`, `vehicle-192.168.2.232.json` |
| x86 ground | `192.168.2.242` | RTL8812EU return TX/RX + RTL8812CU RX | `waybeam-ground.service`, `ground-192.168.2.242.json` |
| RK3566 ground | `192.168.2.199` | RTL8812CU **passive spectator** (Pass 74) — RX only, no uplink; frame-SHM to MPP/DRM | `waybeam-ground-rk.init`, `ground-192.168.2.199.json` |
| Cache | `192.168.2.247` | MT7921 + RTL8812AU receive-only | `waybeam-cache.service`, `cache-192.168.2.247.json` |

The cache belongs to the x86 ground receiver (originator 9). That receiver owns
vehicle discovery/pairing and sends the committed vehicle originator, channel,
bandwidth, and net ID to the cache over Ethernet. The cache accepts assignments
only from `192.168.2.242:5802`, retunes both RF ears, clears an old vehicle's
window, and resumes status/repair service. Its RTL8812AU is receive diversity;
RF-inserted cache replies are not implemented in this deployment.

The RK3566 remains a useful second-view test receiver. It may consume repair
status for the same vehicle, but it does not own or retarget the cache.

The vehicle uses whole-frame `venc_frame` SHM input, **adaptive MCS1–5**
(`select.min_profile 1 / max_profile 5` since #47 — it was pinned at 3), 30%
IDR RLC, and 20% P-frame RLC. Both grounds write `venc_frame_out`; the RK3566
ground feeds it to `waybeam_hub` for MPP/DRM display.

See [the verification hardware reference](../docs/verification-hardware.md)
for exact hardware, drivers, originators, FCS handling requirements, cold-boot
behavior, and the reusable test checklist.

## Operation

Ground and cache services are enabled at boot. The vehicle link is **also
boot-enabled** since 2026-07-23 — `/etc/init.d/S96waybeam-link` on the craft,
starting after `S95waybeam` (venc, which feeds `venc_frame`). That init script
lives on the device, not in this repo. To drive it by hand:

```sh
ssh root@192.168.2.232 '/etc/init.d/waybeam-link start'
ssh root@192.168.2.232 '/etc/init.d/waybeam-link status'
ssh root@192.168.2.232 '/etc/init.d/waybeam-link stop'
```

Ground statistics are available at `http://127.0.0.1:8092/api/v1/stats`.
Vehicle and cache statistics bind to `127.0.0.1:8091` on their hosts.

## Discover and pair a vehicle

The production path is receiver 1 → vehicle + receiver 1 → cache. Start a
three-channel survey on the x86 ground, inspect candidates, then quick-connect:

```sh
ssh snokvist@192.168.2.242 'curl -sS -X POST http://127.0.0.1:8092/api/v1/scout/start -H "Content-Type: application/json" -d '\''{"mode":"list"}'\'''
ssh snokvist@192.168.2.242 'curl -sS http://127.0.0.1:8092/api/v1/scout/results'
ssh snokvist@192.168.2.242 'curl -sS -X POST http://127.0.0.1:8092/api/v1/scout/quickconnect -H "Content-Type: application/json" -d '\''{"originator":17}'\'''
```

Pairing is complete only after the CSA campaign commits. Observe the selected
vehicle and cache readiness independently:

```sh
ssh snokvist@192.168.2.242 'curl -sS http://127.0.0.1:8092/api/v1/link/selection'
ssh snokvist@192.168.2.247 'curl -sS http://127.0.0.1:8091/api/v1/cache/assignment'
```

`link/selection` progresses through `configured` → `claiming` → `verifying` →
`committed`. `caches_following` reaches `caches_configured` after the cache has
retuned and emitted fresh status for the selected vehicle. Video does not wait
for cache readiness; a stopped/rebooted cache is reassigned automatically at
the configured 500 ms retry cadence.

## Before flight

- Reserve or statically configure `.242`, `.199`, and `.247`; the cache
  endpoints require these exact addresses.
- Keep the cache controller endpoint paired with receiver originator 9 and
  `192.168.2.242:5802`; changing either requires updating both deployments.
- Make the SHM viewer persistent or start it explicitly before every test.
- Verify channel 161 is permitted
  at the test site.
- Perform a props-off power-cycle/restart test, then an antenna-separated
  walking/range test with packet loss, unrecoverable frames, driver drops,
  temperature, and end-to-end latency observed.
- Treat this as a video link only. Flight control and failsafe must not depend
  on the FPV video or Ethernet cache path.
