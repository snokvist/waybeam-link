# MVP flight-test deployment

Current three-node kernel-monitor topology:

| Node | Address | RF role | Service/config |
|---|---|---|---|
| Vehicle | `192.168.2.232` | RTL8812EU TX/RX, channel 161 HT20 | `vehicle-waybeam-link.init`, `vehicle-192.168.2.232.json` |
| Ground | `192.168.2.242` | RTL8812EU return TX/RX + RTL8812CU RX | `waybeam-ground.service`, `ground-192.168.2.242.json` |
| Cache | `192.168.2.247` | MT7921 + RTL8812AU receive-only | `waybeam-cache.service`, `cache-192.168.2.247.json` |

The cache stores vehicle DATA received over RF. Cache status, requests, and
replies use Ethernet/UDP. Its RTL8812AU is receive diversity now; RF-inserted
cache replies are not implemented in this deployment.

The vehicle uses whole-frame `venc_frame` SHM input, profile/MCS 3, 30% IDR
RLC, and 20% P-frame RLC. The ground writes `venc_frame_out` for the viewer.

## Operation

Ground and cache services are enabled at boot. The vehicle link is deliberately
not linked into `rcS` until the flight checklist below is complete:

```sh
ssh root@192.168.2.232 '/etc/init.d/waybeam-link start'
ssh root@192.168.2.232 '/etc/init.d/waybeam-link status'
ssh root@192.168.2.232 '/etc/init.d/waybeam-link stop'
```

Ground statistics are available at `http://127.0.0.1:8092/api/v1/stats`.
Vehicle and cache statistics bind to `127.0.0.1:8091` on their hosts.

## Before flight

- Reserve or statically configure `.242` and `.247`; both are currently DHCP
  clients and the cache endpoints require these exact addresses.
- Make the SHM viewer persistent or start it explicitly before every test.
- Verify channel 161 and the configured 27 dBm vehicle TX power are permitted
  at the test site.
- Perform a props-off power-cycle/restart test, then an antenna-separated
  walking/range test with packet loss, unrecoverable frames, driver drops,
  temperature, and end-to-end latency observed.
- Treat this as a video link only. Flight control and failsafe must not depend
  on the FPV video or Ethernet cache path.
