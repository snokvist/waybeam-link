# waybeam-link

**A latency-first digital video link for drones and remote vehicles, built on
raw 802.11 injection.**

waybeam-link carries live video, telemetry and control between a vehicle and
one or more ground stations over ordinary USB WiFi adapters — with no access
point, no association, no TCP, and no kernel WiFi stack in the path. It is
designed around a single priority: **a frame that arrives late is worse than a
frame that never arrives at all.**

```
  camera ─► encoder ─► waybeam-link ─► ((( RF ))) ─► waybeam-link ─► decoder ─► screen
            (vehicle)                                  (ground)
```

See it run without owning a radio:

```sh
cmake --preset dev && cmake --build --preset dev -j
./build/dev/waybeam-link loopback -c examples/config.loopback.sample.json
```

> **Status: working on real hardware, not yet a shipping product.**
> The transport, the adaptive link layer, FEC, ARQ, the follow-me channel
> switch and the discovery/pairing path are implemented, unit-tested and
> verified end to end over real RF. Field range validation and the stability of
> the adaptive loop under sustained flight are still open. See
> [Maturity](#maturity) for the honest breakdown.

**Where this sits.** waybeam-link is the radio layer of the Waybeam FPV
ecosystem. It does not encode video and it does not decode it — it moves what
an encoder produces to wherever a decoder is waiting. On the vehicle it runs
next to `waybeam_venc` (the camera/encoder daemon); on the ground its output
feeds a decoder such as `waybeam-hub`, the Android ground station, or any
consumer of its shared-memory ring. It is usable on its own with any encoder
that can write whole frames into shared memory, or with a plain RTP source.

---

## Table of contents

- [Why this exists](#why-this-exists)
- [The design in one page](#the-design-in-one-page)
- [Architecture](#architecture)
  - [The node model](#the-node-model)
  - [The video path](#the-video-path)
  - [The return path](#the-return-path)
  - [The adaptive link layer](#the-adaptive-link-layer)
  - [Loss recovery: diversity, FEC, ARQ](#loss-recovery-diversity-fec-arq)
  - [Discovery, pairing and the follow-me channel switch](#discovery-pairing-and-the-follow-me-channel-switch)
  - [Operating modes](#operating-modes)
- [Hardware](#hardware)
- [Getting started](#getting-started)
- [Configuration](#configuration)
- [Control plane and observability](#control-plane-and-observability)
- [Using it as a library](#using-it-as-a-library)
- [Building](#building)
- [Maturity](#maturity)
- [Documentation map](#documentation-map)
- [Contributing](#contributing)
- [Licensing and credits](#licensing-and-credits)

---

## Why this exists

Analogue FPV is low-latency and degrades gracefully. Digital FPV is sharp but
usually degrades *catastrophically* — a WiFi link built on association,
retries and buffering will happily trade 300 ms of latency for a frame you no
longer care about.

Broadcast injection links solve this by throwing away the parts of 802.11 that
buy reliability at the cost of time: no association, no MAC-layer ARQ, no
rate-adaptation state machine hidden in a driver. waybeam-link belongs to that
family — the same lineage as [wfb-ng](https://github.com/svpcom/wfb-ng) and
OpenIPC — but it makes a different set of trades:

| | Typical injection link | waybeam-link |
|---|---|---|
| Primary redundancy | Forward error correction | **Multi-adapter receive diversity** |
| Repair of short fades | More FEC overhead | **Opportunistic, deadline-aware ARQ** |
| Unit of transport | Fixed-size packet blocks | **Whole encoded video frames** |
| Rate control | Radio and encoder tuned separately | **One controller owns MCS + TX power + encoder bitrate** |
| Kernel WiFi driver | Required (monitor mode) | **Not used** — userspace USB driver |
| Session model | Keyed, paired | **Open broadcast, anyone may watch** |

The result is a link where the *common* case — brief, correlated fades of
5–30 ms — is repaired by a targeted retransmission that still lands inside the
frame's display deadline, while the *hard* case — a real signal-strength cliff
— is absorbed by having more than one antenna listening rather than by paying
FEC overhead on every single frame forever.

**Deliberate non-goals.** waybeam-link is not encrypted, not authenticated on
the data path, and not a general-purpose network device. It broadcasts. It
assumes a clear-ish band, one vehicle at a time, and an operator who owns the
spectrum they are transmitting in. See §13 and §18 of
[`PROTOCOL.md`](PROTOCOL.md).

---

## The design in one page

Five ideas carry almost everything else:

**1. The vehicle has one radio. The ground has several.**
This is a fixed physical constraint, not a configuration choice. The vehicle
timeshares its single adapter between transmitting video (dominant) and
listening for the ground (opportunistic), which is why the return path is
best-effort *by physics*. The ground runs N adapters in one process and merges
them into a single stream — diversity is a ground-only capability.

**2. Video is transported as frames, not as packets.**
The encoder hands over a complete encoded frame through shared memory.
waybeam-link fragments it, protects it, transmits it, and reassembles a
**byte-identical** frame on the other side. Because the transport knows where
frame boundaries are, it can make per-frame decisions: how much redundancy this
frame deserves, whether it is worth retransmitting, and when it is too late to
bother.

**3. Everything is broadcast; nothing is negotiated.**
There is no handshake and no session setup. A vehicle transmits; any receiver
in range may decode it. Receivers *latch* onto a stream passively. Control
traffic (retransmission requests, link reports) is addressed to a stable
numeric `originator` ID rather than to a MAC address or a connection.

**4. The receiver measures; the transmitter decides.**
Ground stations continuously report what they are actually seeing — RSSI, SNR,
loss before and after diversity merge. The vehicle collects those reports and
runs a single control loop that picks the modulation rate, the transmit power,
*and* the encoder's bitrate together, as one coordinated operating point.

**5. Changing channel is a coordinated manoeuvre, not a reconnect.**
Because the vehicle has one radio and cannot be in two places at once, moving
to a new frequency is a "follow-me" switch: authenticated, scheduled against
the radio's own hardware clock, acknowledged before commit, and backed out
automatically if the vehicle fails to arrive.

---

## Architecture

<!-- DIAGRAM SLOT 1: System overview -->
<!-- See docs/diagram-brief.md, Figure 1 -->

### The node model

Every waybeam-link process is a **node** with a stable numeric identity and a
role. Nodes see each other's broadcasts; roles describe intent, not permission.

| Node | Radios | Transmits | Purpose |
|---|---|---|---|
| **Craft** (vehicle) | 1 | Video, telemetry | The camera platform. Runs alongside the encoder. |
| **Ground** (receiver) | N (1 must be TX-capable) | Return traffic only | The pilot's station. Merges diversity, owns pairing and control. |
| **Spectator** | N (all receive-only) | Nothing | A second screen. Watches without touching the link. |
| **Cache store** | N (all receive-only) | Nothing over RF | An additional listening post wired to the ground over Ethernet, extending coverage without adding a transmitter. |

Because the medium is broadcast, multiple ground nodes on the same channel are
first-class — a spectator adds no load and needs no permission. What *is*
arbitrated is repair: the vehicle serves retransmission requests to one latched
receiver at a time (first-latcher lock, with preemption by a preferred node), so
a room full of spectators cannot storm the return path.

### The video path

<!-- DIAGRAM SLOT 2: Video data path -->
<!-- See docs/diagram-brief.md, Figure 2 -->

```
                    VEHICLE                              GROUND
  ┌──────────┐   shared    ┌──────────────┐         ┌──────────────┐   shared   ┌─────────┐
  │ encoder  │─── memory ─►│ waybeam-link │         │ waybeam-link │── memory ─►│ decoder │
  │  (venc)  │   ring      │      TX      │         │      RX      │   ring     │         │
  └──────────┘             └──────┬───────┘         └──────▲───────┘            └─────────┘
                                  │                        │
                       fragment into source symbols   merge N adapters,
                       + GF(256) repair symbols       dedup, reassemble,
                                  │                   FEC-decode
                                  ▼                        │
                            ┌───────────┐   ((( RF )))  ┌──┴──┐ ┌─────┐ ┌─────┐
                            │  1 radio  │──────────────►│ rx1 │ │ rx2 │ │ rx3 │
                            └───────────┘               └─────┘ └─────┘ └─────┘
```

The encoder publishes each completed frame into a POSIX shared-memory ring.
waybeam-link picks it up in the same process tick, splits it into **source
symbols** sized to fit the current modulation's payload budget, generates
**GF(256) Reed–Solomon repair symbols** for it, and injects the lot. On the
ground, symbols arriving on any adapter feed one merged receive state machine
that deduplicates by sequence number, reassembles the frame, decodes FEC if
symbols were lost, and writes the result into an outgoing shared-memory ring for
the decoder.

The reconstructed frame is byte-identical to what the encoder produced — this is
verified continuously in the test suite and has been confirmed on hardware.

Video may also be ingested as ordinary **RTP over UDP** if you are feeding
waybeam-link from GStreamer or an existing pipeline. The transport treats RTP as
opaque; the only codec awareness anywhere in the system is a small classifier
that decides whether a given packet is important enough to be worth
retransmitting.

Alongside video, the same wire carries **telemetry**, **control** (RC uplink)
and **audio** stream types, each with its own delivery discipline.

### The return path

The vehicle can only hear the ground while its own radio is not transmitting.
waybeam-link therefore paces the return path against the radio's hardware
timestamp counter: the vehicle advertises quiet gaps, and the ground aims its
return traffic into them. Everything the ground sends upstream — retransmission
requests, link reports, channel-switch commands, RC and telemetry uplink —
shares that narrow, best-effort window, in a strict priority order.

This is why a ground station is expected to have one adapter appointed as the
**designated uplink transmitter**: while it is transmitting it is deaf, and its
diversity siblings cover the blind spot.

### The adaptive link layer

<!-- DIAGRAM SLOT 3: Adaptive control loop -->
<!-- See docs/diagram-brief.md, Figure 3 -->

```
   ground: measure                        vehicle: decide
   ┌────────────────────┐  link report   ┌──────────────────────┐
   │ RSSI / SNR         │───────────────►│  selector            │
   │ pre-diversity loss │                │   ├─ demote on loss  │
   │ post-diversity loss│                │   ├─ promote on margin│
   │ per-adapter health │                │   └─ freeze on flap  │
   └────────────────────┘                └──────────┬───────────┘
                                                    │  one operating point
                                    ┌───────────────┼───────────────┐
                                    ▼               ▼               ▼
                                  MCS          TX power        encoder bitrate
                            (modulation)    (per adapter)     (via local API)
```

A single controller on the vehicle owns all three knobs and moves them as a
coordinated sequence — bitrate down *before* modulation down, modulation up
*before* bitrate up — so the link never spends a moment asking the radio to
carry more than it can. Demotion is reactive (loss happened, react now);
promotion is conservative and gated on signal margin. A flap-freeze prevents
oscillation, and a fail-safe drops to the most robust operating point if reports
stop arriving at all.

**waybeam-link is the sole owner of the encoder's bitrate.** The encoder has no
arbitration — last writer wins — so nothing else in the system may write it
while waybeam-link is running. This is a hard deployment invariant.

### Loss recovery: diversity, FEC, ARQ

Three mechanisms, deliberately layered, each aimed at a different failure shape:

| Mechanism | Repairs | Cost | Role |
|---|---|---|---|
| **Receive diversity** | Uncorrelated per-antenna loss | Extra adapters | **Primary.** Load-bearing. |
| **FEC** (GF(256) RS) | Random symbol loss within a frame | Constant airtime overhead | Configurable per stream, higher rate on keyframes |
| **ARQ** (retransmission) | Short correlated fades, ~5–30 ms | Return-path airtime, only when needed | **Opportunistic.** Never load-bearing. |

The ordering matters. ARQ is explicitly *not* a reliability guarantee: it is
importance-gated (keyframes by default), deadline-aware (a repair that cannot
arrive in time is never sent), and it quietly does less as the channel
saturates. When the link is in real trouble, waybeam-link degrades toward pure
diversity — that is the designed floor, not a failure.

Measured during a real signal-strength fade: two adapters reduced 8.6 % loss
before the merge to 2.4 % after it, and 10 % FEC then recovered 599 source
symbols where ARQ recovered 52. In the same run a 37-second total blackout
proved that neither mechanism repairs a link that has genuinely gone away — and
the design does not pretend otherwise.

### Discovery, pairing and the follow-me channel switch

<!-- DIAGRAM SLOT 4: Discovery → claim → channel switch -->
<!-- See docs/diagram-brief.md, Figure 4 -->

A vehicle powers on and broadcasts a periodic announcement on its channel. A
ground station that does not know where the vehicle is **scouts**: it sweeps its
adapters across the allowed channel list, dwelling long enough on each to
actually hear an announcement, and reports what it found.

Pairing is a **claim**: the ground sends a token-keyed claim to the vehicle,
which binds to that originator as its command source. From then on the vehicle
accepts control traffic only from that ground station.

Changing frequency is the delicate part, because the vehicle has one radio and a
mistimed switch means a lost aircraft. The **follow-me channel switch** handles
it as a campaign:

1. Ground announces the target channel, authenticated by a shared key.
2. Vehicle acknowledges — the ground does not commit until it has.
3. Both sides schedule the switch against the radio's hardware clock so they
   arrive together.
4. If the vehicle does not appear on the new channel, it **backs out** to the
   old one automatically.
5. Once the link is proven on the new channel, it is committed and held.

This is the only cryptography in the system, and it is deliberately off the data
path.

### Operating modes

Rather than exposing modulation indices and frame rates to a pilot, each vehicle
ships a small catalog of named **operating modes** — a bundle of resolution,
frame rate and the modulation window the link is allowed to use. The user picks
along two axes:

| Axis | The pilot sees | What it actually sets |
|---|---|---|
| **Latency** | Low / Medium / High | Sensor mode and frame rate (100 / 60 / 30 fps) |
| **Range** | High / Medium / Low | The modulation band the link may select within |

Modes can be applied over HTTP on a bench, or over RF in flight. The ground
carries a fingerprint of the vehicle's catalog so that a mismatch is *detected*
rather than silently applying the wrong mode.

---

## Hardware

**Vehicle side** — one USB WiFi adapter, one SoC running the encoder.
Verified on SigmaStar SSC338Q and HiSilicon CV610 camera boards.

**Ground side** — one to three USB WiFi adapters on x86, ARM64 (RK3566), or
Android.

**Supported radios.** All four chips inject, receive, and accept transmit-power
control, through the vendored [OpenIPC
devourer](https://github.com/OpenIPC/devourer) userspace driver. The kernel
driver must be unloaded first — devourer talks to the USB device directly.

| Chip | Best used as | Worth knowing |
|---|---|---|
| **RTL8812AU** | The ground's transmit adapter | Lowest transmit-path latency by a wide margin — ~0.1 ms to air versus ~2 ms on the CU/EU generation. That matters because the return path has to fit in a narrow window. |
| **RTL8812CU** | A ground diversity ear | Fine as a receiver; avoid appointing it the uplink transmitter if an AU is available. |
| **RTL8812EU** | A vehicle adapter | Run it at 20 MHz — 40 MHz is broken on this part. |
| **RTL8733BU** | A vehicle adapter | No per-frame transmit-report path, so the link infers transmit health differently. Changes channel more slowly (~345 ms), which lengthens a channel sweep. |

Adapters are matched by USB bus path, or by the per-unit MAC burned into the
adapter's EFUSE — never by network interface name, which is not stable across
reboots or re-plugs.

---

## Getting started

### Without any radios

Everything but the RF hop runs on one machine. The built-in `loopback` mode
(shown at the top of this page) exercises the whole transport in a single
process. To run two *real* processes against a simulated air interface — one
transmitter, one receiver, with virtual diversity paths and a matched return
path:

```sh
./build/dev/waybeam-link tx -c examples/config.air-tx.sample.json &
./build/dev/waybeam-link rx -c examples/config.air-rx.sample.json
```

With GStreamer installed, `tools/frame_shm_udp_bench.sh` drives the complete
encode → transport → decode chain and validates the result frame by frame —
metadata, byte-exactness, timestamp monotonicity, FEC and ARQ counters.

### With radios

The kernel driver has to release the adapter first, because devourer opens the
USB device directly:

```sh
sudo rmmod <whatever module currently claims your adapter>   # e.g. 88x2cu, rtw88_8812au

sudo ./build/dev/waybeam-link tx -c examples/config.radio-tx.sample.json   # vehicle
sudo ./build/dev/waybeam-link rx -c examples/config.radio-rx.sample.json   # ground
```

Both ends must agree on channel, bandwidth and network ID. Run from the repo
root — the operating-point table is loaded by relative path.

> **Transmitting on 5 GHz is regulated.** You are responsible for operating
> within the rules of your jurisdiction, on frequencies you are licensed to
> use, at power levels you are permitted to radiate.

---

## Configuration

One JSON file per node, covering identity, adapters, streams, policy and
observability:

```json
{
  "node":    { "originator": 17, "net_id": 42, "role": "tx" },
  "adapters": [
    { "name": "craft", "bus": "1-1", "role": "tx", "channel": 5805, "bw": 20 }
  ],
  "air":     { "kind": "radio" },
  "streams": [
    { "stream_id": 0, "stream_type": "RTP", "dir": "in",
      "bind": { "kind": "frame-shm", "name": "venc_frame" },
      "fec":  { "scheme": "rlc256", "i_rate_permille": 250, "p_rate_permille": 100 } }
  ],
  "control": { "bind": "127.0.0.1:8091" },
  "stats":   { "hz": 5 }
}
```

Configs are **coupled across nodes** — network ID, channel, allowed channel
list and originator references have to agree fleet-wide. Validate before
deploying:

```sh
waybeam-link tx -c my-node.json --check --strict   # parses, binds, reports unknown keys
waybeam-link config-schema --json                  # the full declared key surface
```

`--strict` matters: the loader reads keys by name and silently ignores anything
it does not recognise, so a typo loads perfectly cleanly without doing what you
meant. `examples/` holds annotated samples; `deploy/` holds real configs read
back off flying hardware.

---

## Control plane and observability

Every node optionally exposes a small HTTP/1.0 REST surface, folded into the
main event loop — no threads, no locks, no authentication (bind it to localhost
or to a trusted network only).

```sh
curl -s http://127.0.0.1:8091/api/v1/stats | jq .link      # full statistics object
curl -s http://127.0.0.1:8091/api/v1/health                # terse link summary
curl -N http://127.0.0.1:8091/api/v1/stats/stream          # live SSE feed
```

Writes take effect immediately, without a restart:

| Endpoint | Effect |
|---|---|
| `POST /api/v1/link/profile` | Pin or unpin the modulation/bitrate operating point |
| `POST /api/v1/tx/power` | Set transmit power offset |
| `POST /api/v1/fec` | Change FEC rates per stream |
| `POST /api/v1/csa` | Trigger a follow-me channel switch |
| `POST /api/v1/scout/start` · `/quickconnect` | Discover and pair with a vehicle |
| `POST /api/v1/mode` · `GET /api/v1/modes` | Apply / enumerate operating modes |
| `POST /api/v1/vehicle/command` | Send a command to the paired vehicle over RF |
| `POST /api/v1/video/recover` | Request one keyframe to bootstrap a decoder |

Independently of REST, every node emits a newline-delimited JSON statistics
record at a configurable rate. `tools/link_monitor.py` — stdlib Python, no
dependencies — turns that stream into a live browser dashboard for a whole
fleet, with per-adapter signal, per-stream loss before and after diversity, FEC
and ARQ recovery counts, and return-path health.

```sh
python3 tools/link_monitor.py     # dashboard on :8099, stats intake on :9110
```

<!-- DIAGRAM SLOT 5: Control plane & observability -->
<!-- See docs/diagram-brief.md, Figure 5 -->

---

## Using it as a library

waybeam-link is structured so that other applications can embed the link rather
than shell out to a daemon. The Android ground station and the C-based
`waybeam-hub` daemon both do exactly this.

| Layer | Contains | Dependencies |
|---|---|---|
| `core/` | Wire format, receive engine, scheduler, FEC, adaptive selector, channel-switch logic. No sockets, no threads, no wall clock. | C++ standard library only |
| `io/` | Configuration, UDP and shared-memory bindings, the radio backend, statistics, encoder actuation | devourer, libusb |
| `node/` | Runnable node behaviour — a complete receiving or transmitting node you can start from your own process | `core` + `io` |

`node/` also exposes a **C ABI**, because the two real consumers are a C daemon
and an Android app reaching native code through JNI. On unrooted Android, USB
file descriptors come from the Java `UsbManager` and are handed in directly —
there is no device enumeration to do.

```cmake
find_package(wblink)              # gives wblink::core, installable standalone
# or
add_subdirectory(waybeam-link)    # gives wblink::core, wblink::io, wblink::node
```

The wire format is **vendored, not reimplemented**, into every consumer, so
there is exactly one implementation of the packet format in the ecosystem.

---

## Building

```sh
cmake --preset dev && cmake --build --preset dev -j    # x86 debug + sanitizers
ctest --preset dev                                     # the unit test suite
scripts/gates.sh                                       # everything CI runs
```

Cross-compilation presets exist for the SigmaStar SSC338Q (`ssc338q`), HiSilicon
CV610 (`cv610`), Rockchip RK3566 (`rk3566`), a multi-chip x86 ground build
(`x86-ground`), and Android arm64 (`android-arm64`). Toolchains the host does
not have are skipped loudly rather than silently passing.

`scripts/gates.sh` is the single source of truth for what "green" means — it
covers every preset, the full test suite, the library embedding and install
round-trips, and validation of every deployed config. CI runs the same script,
so local and CI cannot drift.

---

## Maturity

Being straightforward about what is proven and what is not:

**Verified on real hardware**
- End-to-end byte-identical video — encoder → RF → decoder, at ~90 fps, with
  zero decode errors.
- Retransmission round-trip P90 ≤ 4 ms at 65 % airtime, comfortably inside a
  keyframe's display deadline.
- Diversity gain and FEC recovery measured through a real signal-strength fade.
- Two different radio chips injecting and receiving in the same process.
- The follow-me channel switch, including the automatic back-out.
- Discovery, pairing, and operating-mode application over both HTTP and RF.

**Still open**
- Range-limited behaviour of the return path in flight.
- Long-run stability of the adaptive control loop under real flight dynamics.
- Several timing constants are bench seeds pending field re-derivation.

**Known limitations**
- No encryption or authentication on the data path — by design.
- One vehicle per channel; two vehicles need real spectral separation.
- The vehicle's return-path reception is best-effort and always will be.
- No 802.11 MAC-layer retries: application-level resend is the only retry.

**What is genuinely new here, and what is not.** Multi-adapter same-channel
receive diversity with deduplication is prior art; this is a clean, FEC-free
reimplementation of it rather than an invention. What is new is the rest: ARQ
that is frame-aligned, importance-gated and deadline-aware instead of a
reliability layer; the open broadcast passive-latch session model with
originator-addressed control; and putting modulation, transmit power and
encoder bitrate under one controller with a latency-first objective instead of
an airtime-first one.

---

## Documentation map

| Document | What it is |
|---|---|
| [`PROTOCOL.md`](PROTOCOL.md) | **The specification.** Wire format, state machines, configuration semantics. Normative — this is the contract a second implementation would be written against. |
| [`docs/bench-and-tools.md`](docs/bench-and-tools.md) | Bench harnesses, the fleet dashboard, replay and trace tooling, hardware bring-up notes |
| [`docs/build-order.md`](docs/build-order.md) | Build order and the de-risking bench gates |
| [`docs/findings.md`](docs/findings.md) | Dated measurement notes — anything still being characterised |
| [`docs/review-log.md`](docs/review-log.md) | Numbered log of every specification ruling and why it was made |
| [`docs/groundwork.md`](docs/groundwork.md) | Provenance of every constant, traced to its source |
| [`deploy/README.md`](deploy/README.md) | Real deployed node configurations |
| [`profiles/modes/README.md`](profiles/modes/README.md) | The operating-mode catalog format |
| [`CLAUDE.md`](CLAUDE.md) | Contributor conventions and the specification process |

---

## Contributing

Issues and pull requests are welcome. Two things are worth knowing before you
open one:

- **[`PROTOCOL.md`](PROTOCOL.md) is the contract.** Anything that changes a
  wire format, a state machine peers depend on, or configuration semantics is a
  specification change: the spec is amended first, in its own commit, and the
  reasoning is recorded in [`docs/review-log.md`](docs/review-log.md). Anything
  still being *measured* — thresholds, dwell times, seeds — is a configuration
  knob and a dated note in [`docs/findings.md`](docs/findings.md), not spec
  text.
- **`scripts/gates.sh` is what "green" means.** Run it before opening a PR. It
  builds every preset the host can build, runs the full test suite under
  sanitizers, and checks the library embedding and install paths. Toolchains
  you do not have are skipped loudly rather than passing silently.

[`CLAUDE.md`](CLAUDE.md) holds the longer version of both, plus the runtime and
bench gotchas that cost real debugging time to discover.

---

## Licensing and credits

GPL-2.0-or-later. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).

waybeam-link vendors and builds on:

- **[OpenIPC devourer](https://github.com/OpenIPC/devourer)** (GPL-2.0) — the
  userspace RTL8812/8733 driver that makes kernel-free injection possible.
- **[libusb](https://github.com/libusb/libusb)** (LGPL-2.1-or-later)
- **[nlohmann/json](https://github.com/nlohmann/json)** (MIT)

The per-adapter transmit-power tables reuse the row format from Realtek's
`PHY_REG_PG` driver data. The [OpenIPC](https://github.com/OpenIPC) and
[wfb-ng](https://github.com/svpcom/wfb-ng) projects established the broadcast
injection-link approach that this builds on.
