# MVP flight-test deployment

Current four-node kernel-monitor verification topology:

| Node | Address | RF role | Service/config |
|---|---|---|---|
| Vehicle | `192.168.2.232` | RTL8812EU TX/RX, channel 161 HT20 | `vehicle-waybeam-link.init`, `vehicle-192.168.2.232.json` |
| x86 ground | `192.168.2.242` | RTL8812EU return TX/RX + RTL8812CU RX | `waybeam-ground.service`, `ground-192.168.2.242.json` |
| RK3566 ground | `192.168.2.199` | RTL8812CU **passive spectator** (Pass 74) — RX only, no uplink; frame-SHM to MPP/DRM | `waybeam-ground-rk.init`, `ground-192.168.2.199.json` |
| Cache | `192.168.2.247` | MT7921 + RTL8812AU receive-only | `waybeam-cache.service`, `cache-192.168.2.247.json` |

**Ground uplink adapter choice (issue #99, findings.md 2026-08-07):** where
a ground rig has both generations, its `role:"tx"` adapter should be the
**Jaguar1 (8812AU)** — submit→air p99 is 101 µs there vs 2.2 ms on Jaguar3
(8822/8812CU) against the §7.2 ±1000 µs return-window budget, and a
Jaguar3 uplink's deferral tail is *correlated* return loss inside a window.
Jaguar3 units stay diversity ears.

The cache belongs to the x86 ground receiver (originator 9). That receiver owns
vehicle discovery/pairing and sends the committed vehicle originator, channel,
bandwidth, and net ID to the cache over Ethernet. The cache accepts assignments
only from `192.168.2.242:5802`, retunes both RF ears, clears an old vehicle's
window, and resumes status/repair service. Its RTL8812AU is receive diversity;
RF-inserted cache replies are not implemented in this deployment.

The RK3566 remains a useful second-view test receiver. It may consume repair
status for the same vehicle, but it does not own or retarget the cache.

The vehicle uses whole-frame `venc_frame` SHM input, adaptive MCS 1–5
(`select.min_profile 1 / max_profile 5`, since #47), 30% IDR RLC, and 20%
P-frame RLC. Both grounds write `venc_frame_out`; the RK3566 ground feeds it
to `waybeam_hub` for MPP/DRM display.

See [the verification hardware reference](../docs/verification-hardware.md)
for exact hardware, drivers, originators, FCS handling requirements, cold-boot
behavior, and the reusable test checklist.

## Operation

Ground and cache services are enabled at boot. The vehicle now also autostarts
`waybeam-link` at boot via `S96waybeam-link` (the craft rootfs init script,
which lives outside this repo). Complete the flight checklist below before
relying on it; drive it manually with:

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
- Verify channel 161 is permitted at the test site. NOTE: the vehicle TX power
  is **not** configured or enforced by waybeam-link — the config sets no power,
  kernel-monitor power actuation is a documented no-op, and `mon-up.sh` sets
  `txpower auto`. Any regulatory power limit must be met at the adapter/driver,
  not assumed from this repo.
- CSA key mode: no deploy config sets `csa.psk`, so the fleet runs
  **announced-token mode** (Pass 61/63) — the per-boot CSA token is public on
  the ANNOUNCE beacon, and the only takeover defence is the §11.5a sticky
  binding (90 s release after the claiming ground goes silent). For a stronger
  guarantee, provision a shared `csa.psk` secret on the craft **and** every
  ground together (a mismatch fails closed, Pass 85). After any craft reboot,
  re-scout and re-claim before relying on channel switching (the token
  regenerates each boot).
- Perform a props-off power-cycle/restart test, then an antenna-separated
  walking/range test with packet loss, unrecoverable frames, driver drops,
  temperature, and end-to-end latency observed.
- Treat this as a video link only. Flight control and failsafe must not depend
  on the FPV video or Ethernet cache path.
