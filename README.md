# waybeam-link

A best-effort, **latency-first** broadcast video + telemetry link for
monitor/injection WiFi (RTL8812AU/CU/EU) with per-adapter receive **diversity** as
the primary redundancy, opt-in importance-gated **ARQ** as an opportunistic patch,
an **adaptive link layer** (per-MCS rate/power + encoder-bitrate control), and a
coordinated **follow-me channel switch** — built on OpenIPC **devourer** for raw
802.11 monitor/injection.

> **Status: IMPLEMENTATION IN PROGRESS — build-order steps 1–10 built.**
> Wire codec, I/O/config/stats, TX framer + resend ring, merged RX engine,
> resend scheduler + arbitration, the loopback bench + udp-air dev backend,
> the §4.1 NAL-type ARQ classifier, the §9/§10 adaptive layer (RX metric
> reporter, TX decision cascade with sequenced MCS↔bitrate transitions,
> flap-freeze + fail-safe, venc bitrate actuation, per-adapter TX-power
> resolve), and the **radio path** — vendored devourer behind the §3.0
> pinned encapsulation (`air.kind: "radio"`, per-adapter RX threads, real
> RSSI/TSF, `SetTxMode` + `SetTxPowerOffsetQdb` at profile commit) with the
> §7.2 TSF quiet-gap pacer, and the **follow-me CSA** (§11: HMAC-SHA-256'd
> campaigns, craft follower with TSF-anchored switch + jump-failed backout +
> committed channel hold, ground issuer with commit-after-CSA_ARMED, §11.3 selector
> freeze; ground trigger = `POST /api/v1/csa`, §15.5), plus the **ground scout
> + quickconnect path** (§15.5a: ANNOUNCE discovery, channel sweep, token-keyed
> claim, multi-adapter uplink scouting with all-ear retune/rollback, and
> heard-most candidate-channel selection), plus **receiver-owned Ethernet cache
> following** (claim commit replaces the RX sender subscription and repeatedly
> assigns the same vehicle/channel/net-id tuple to the receiver's cache until
> matching status proves readiness) — are implemented and tested
> (`ctest --preset dev`, ASan+UBSan; SSC338Q cross-verified via
> `cmake --preset ssc338q`).
> Step 11 (field bring-up + the §17 bench gates) has **run** on the x86 bench
> (2× RTL8812CU + 1× RTL8812AU): the §3.0 on-air encapsulation is
> field-verified (monitor RX delivers the MPDU with a +4-byte FCS trailer,
> stripped before parse); gate 1 **PASSED** (injector + monitor siblings mix
> in one process, both chip families, per-frame CCX `tx.report` live); gate 3
> **PASSED** (NACK→RETRANSMIT recovery P90 ≤4 ms at 65% airtime, well inside
> the 40 ms I-frame deadline; ARQ ceases past saturation by design); gate 2 is
> **RF-proven by a real MCS5 walk fade**: N=2 reduced 86‰ pre-diversity loss to
> 24‰ post-diversity loss, then 10% GF(256) FEC recovered 599 source symbols
> versus 52 by ARQ. A 37.1 s whole-link blackout confirmed that neither FEC nor
> same-channel ARQ can repair the correlated SNR-edge tail. Gate 4 observables
> and the clean desk A/B are live: the 300/2000 µs guard/window seeds stand,
> while range-sensitive return-path and adaptive-loop stability remain to be
> validated. See `docs/step11-bench.md` for the
> full bench report and the remaining-work plan. Try it without hardware:
> `./build/dev/waybeam-link loopback -c examples/config.loopback.sample.json`
> or the two-process udp-air pair (`examples/config.air-{tx,rx}.sample.json`);
> with radios, `examples/config.radio-{tx,rx}.sample.json`.

## What this is

- **Broadcast, no data-path auth**, semi-anarchy: any node may view and NACK any
  stream; the TX arbitrates who it *repairs* (first-latcher lock with
  preferred-node preemption, §12).
- A **per-node merged RX state machine** combining that node's adapters (diversity
  by packet-seq dedup). Multiple RX *nodes* on-air are first-class. **The craft has
  one radio; diversity is ground-only.**
- Nodes addressed by a **stable `originator` ID**; NACK/LINK_REPORT are ordinary
  originator-addressed control packets carrying a target descriptor.
- **RTP carried opaque** end-to-end; the only codec awareness is an isolated RTP
  profile (NAL classifier) setting one importance bit.
- An **adaptive controller** (RX reports / TX decides) driving MCS, per-adapter TX
  power, and the encoder's bitrate via a local same-SoC API call.
- **Follow-me channel switch** (§11): ground-led CSA, TSF-anchored, authenticated
  by a 4-byte MAC (the sole crypto, off the data path), strand-proofed by a
  craft-ACK-before-commit handshake.

## What is genuinely new vs. borrowed (be honest about this)

The **transport core** — same-channel multi-adapter RX diversity + dedup +
injection — already exists in `waybeam_wfb_ng`. The genuinely novel contributions
here are:
1. **opportunistic ARQ** (instead of FEC) targeting the short correlated-fade band,
2. the **opaque passive-latch session model** with **originator-addressed
   semi-anarchy** (any node views/NACKs; TX-side first-latcher arbitration), and
3. a clean, minimal, **FEC-free** reimplementation decoupled from the wfb_ng/wfb
   pipeline (the reason the Android devourer path was built in the first place).

The **adaptive link layer (§9–10)** is **lifted and adapted** from the production
`waybeam_wfb_ng` `link_controller` — its control discipline (reactive-demote,
MCS↔bitrate sequencing, flap-freeze) and its constants are reused as seeds. But the
objective is **inverted to latency/robustness-first** (not energy/airtime-first),
the demote threshold is **re-derived for the no-FEC regime**, and the probe-promote
is **replaced by RSSI-margin promote** (no wfb probe side-stream under injection).
Several seed constants are marked RE-DERIVE and settled on the bench, not on paper.

## Layout

```
PROTOCOL.md            Canonical protocol spec (§1–19). Single source of truth.
docs/
  findings-pass3.md    The adversarial-review arbitration (Parts A–I) + operator
                       rulings + Pass-3b; the raw material PROTOCOL.md v1 folds in.
  groundwork.md        Calibration source-of-truth: every adaptive/TX-power constant
                       traced to link_controller.c / venc_api.c / the rtl88x2 driver
                       and devourer, with file:line citations + corrections.
  build-order.md       Build order (§19) + the four de-risking bench gates (§17).
  review-log.md        Running log of review passes (1, 2, 3, 3b).
profiles/
  table.example.json   The §9.3 operating-point table (data, not code).
```

## Bench tools

Scripts under `tools/` support the §17 bench gates:

- `tools/rtp_feed.py <duration-s> <pps> [fps=60]` — frame-structured synthetic
  H.265 RTP to `127.0.0.1:5600`: `<fps>` access units/s, per-AU M-bit on the
  last packet, one IDR (NAL 19) per second. The saturation feeder for gates 3/4.
- `tools/gate2_rho.py <ground-stats.jsonl> [min-seq-delta=30]` — the §17 gate-2
  windowed cross-adapter loss correlation, ground-side self-aligned on the
  stream's own seq deltas (immune to craft/ground start-time skew). Reports
  per-adapter mean/P95 loss, joint post-diversity loss vs. the independence
  product, and Pearson ρ between adapters.
- `tools/gate3_rtt.py <ground-stats.jsonl> [iframe-deadline-ms=50]` — the §17
  gate-3 NACK→RETRANSMIT latency report, diffed from the run's cumulative
  `nack_rtt_*`/`arq_rec_*` histograms and segment-aware across stream
  relatches. Reports P50/P90 plus the share provably inside the I-frame
  deadline.

Bench knob: `air.rx_drop_permille` (per-adapter independent synthetic RX
drop; bench-only, default off) — used to manufacture known-independent loss
for gate-2 machinery validation and to exercise FEC recovery. Honored by all
air backends (`udp`, `udp-broadcast`, `radio`).

### Real-video frame-SHM/UDP bench

On a host with GStreamer development packages plus `x265enc`, `h265parse`, and
`avdec_h265`, the native build adds `frame_shm_gst_bench`. The orchestrator
runs two real `waybeam-link` processes with a paired UDP return path and two
virtual diversity adapters:

```text
GStreamer H.265 -> frame-SHM -> TX -> UDP-air x2 -> RX -> frame-SHM
                                                       -> H.265 decoder
```

```sh
cmake --preset release
cmake --build --preset release -j
tools/frame_shm_udp_bench.sh
RX_DROP_PERMILLE=100 BITRATES=4000 tools/frame_shm_udp_bench.sh
```

The clean sweep defaults to 1/4/8 Mbit/s. It checks frame metadata, Annex-B,
PTS monotonicity, decoder EOS, frame counts, both UDP adapter counters, FEC,
ARQ, malformed/decode outcomes, and SHM producer drops. `FRAMES`, `BITRATES`,
`WARMUP_FRAMES`, `RX_DROP_PERMILLE`, and `BUILD` are overridable. Set
`KEEP_TMP=1` to retain configs, logs, and stats JSONL after a failure.
Synthetic `x265enc` uses periodic IDRs and deliberately fails on a 512 KB SHM
oversize. Stress-only runs may set `ALLOW_PRODUCER_OVERSIZE=1`; this accepts
only an oversize-only producer result with no full-ring drops and enough usable
frames for the consumer. Normal runs continue to fail on every oversize.

ARQ stress uses one listener (fully correlated/single-path loss), disables FEC,
and permits the consumer to finish with fewer frames while requiring the actual
NACK/retransmit/recovery counters to advance:

```sh
FRAMES=300 BITRATES=8000 AIR_KIND=udp-broadcast RX_LISTENERS=1 \
RX_DROP_PERMILLE=20 FEC_SCHEME=none ALLOW_FRAME_LOSS=1 EXPECT_ARQ=1 \
CONSUMER_TIMEOUT_MS=15000 ARQ_IFRAME_DEADLINE_MS=16 \
ARQ_PFRAME_DEADLINE_MS=16 tools/frame_shm_udp_bench.sh
```

Deadline overrides generate a temporary matched profile table for both local
processes. They do not edit `profiles/table.example.json`.

### Fleet monitor (live dashboard)

`tools/link_monitor.py` — a stdlib-only bridge that turns the §15.3 stats
NDJSON into a browser dashboard for human link evaluation. Point each
instance's stats egress at the monitor host (every instance may share one
port; snapshots key by `(src-ip, node, session)`):

```json
"stats": { "hz": 5, "bind": { "kind": "udp", "send": "<monitor-ip>:9110" } }
```

Then:

```
python3 tools/link_monitor.py          # HTTP :8099, UDP :9110
python3 tools/link_monitor.py --label 192.168.2.201=vehicle --label 192.168.2.242=ground
```

Open `http://localhost:8099/`. Each instance gets a card: link state /
profile / MCS / tx-power / CSA, per-adapter RSSI/SNR/tx-fail/wedged, per-stream
delivered-rate / pre+post loss / ARQ + FEC recovery / superseded+deadline
drops / decode-errors / NACK-RTT, and return-path health — all SSE-live with
staleness dots. The bridge never touches the binaries; it only consumes the
stats push (`GET /api/instances` for a JSON snapshot, `GET /api/stream` for the
SSE feed).

The dashboard separates those groups into tabs and keeps five minutes of
browser-side trend history. Hover an underlined label for its precise meaning.
Frame-SHM streams report successful local frame transfers, cumulative bytes,
last/min/max frame size, latest arrival interval, and smoothed arrival jitter.
The finite bench additionally writes its per-frame trace and summary to
`frames.csv` and `summary.json`. It converts that data to the versioned
`controller-trace.jsonl` contract and deterministically replays it into
`controller-decisions.jsonl`. The initial deadline is excess inter-arrival time
over the run's median cadence; it is not cross-host one-way latency. VFRM PTS
is retained as an opaque SDK correlation value and is not treated as
milliseconds. Override the provisional 16 ms budget with `JSCC_DEADLINE_MS=25`.
Finite runs also enable the capped UDP packet-event observer and emit
`{tx,rx}-packets.jsonl`, `controller-packet-trace.jsonl`, and
`controller-packet-decisions.jsonl`. `PACKET_TRACE_MAX` defaults to 75,000
events per process; `trace_end.events_dropped` makes an incomplete capture
explicit. `controller-matrix.json` contains the standard nine recorded/loss
scenarios crossed with four FEC/ARQ/deadline ablations.
Vehicle packet traces default to the SD card at
`/mnt/mmcblk0p1/waybeam-link-traces` and are removed after retrieval. Override
the location with `REMOTE_TRACE_DIR`; avoid `/tmp` for extended captures because
it is a small RAM-backed filesystem on the SSC338Q.
`FEC_I_RATE_PERMILLE`, `FEC_P_RATE_PERMILLE`, and `FEC_MIN_K` override only the
generated/deployed waybeam-link bench config. They never edit venc settings;
set both rates to zero for an ARQ-only hardware run.

## Frame-SHM video transport (PROTOCOL.md §5.1a/§6.3a/§14.1/§15.4)

The low-latency video path. The encoder (`waybeam_venc`) publishes whole encoded
frames into a POSIX shared-memory ring; waybeam-link ingests them, fragments each
into source symbols + GF(256) Cauchy-RS repair symbols (§14.1), injects over the
air, and on the ground reassembles + FEC-decodes back into a **byte-identical**
SHM slot for the decoder. SHM is same-host on each end; the air hop is either
real devourer RF injection or the udp-air bench sim.

```
venc(frame-shm://venc_frame) → wl tx (FrameFramer+FEC) → AIR → wl rx (reassemble+FEC) → frame-shm(venc_frame_out) → decoder
```

FEC is transparent to the RX (it decodes whatever the TX emits); `fec.scheme
"none"` fragments + ARQs without repair symbols. Both ends must share
`node.net_id` on the radio path. Adaptive MTU: symbols are sized from the
active profile's `max_payload` (jumbo rungs keep large IDRs under the GF(256)
k+r≤256 cap). Example configs: `examples/config.frame-shm-{tx,rx}.sample.json`.

### Vehicle / craft side — TX (frame-shm ingress)

Point venc at the ring, then restart it:
`json_cli -s .outgoing.server '"frame-shm://venc_frame"' -i /etc/waybeam.json`.
TX stream + FEC:

```json
"streams": [
  { "stream_id": 0, "stream_type": "RTP", "dir": "in",
    "bind": { "kind": "frame-shm", "name": "venc_frame" },
    "fec": { "scheme": "rlc256", "i_rate_permille": 250, "p_rate_permille": 100, "min_k": 3 } }
]
```

**(a) RF injection** — `air.kind "radio"` (devourer/libusb) is the verified craft
path for the 8812EU: the kernel driver is unbound so libusb owns the raw USB
device and injects directly (the `rtl88x2eu` driver does **not** inject via
mac80211 monitor — `iw set monitor` leaves `tx_packets=0`). Use the bench init
script `/etc/init.d/waybeam-link {start|stop}`, which does `adapter stop` +
`rmmod 8812eu` before launching:

```json
"adapters": [{ "name": "eu-craft", "bus": "", "role": "tx", "channel": 5805, "bw": 20,
               "power_map": "/etc/waybeam-link/power.craft.floor.txt", "max_power_qdb": -40 }],
"air": { "kind": "radio" },
"policy": { "select": { "min_profile": 0, "max_profile": 0 } }
```

Craft runs 20 MHz, MCS pinned low at 5805 (§10, 8812EU sub-band limits).
`air.kind "kernel-monitor"` — AF_PACKET injection through a mac80211 monitor
netdev — was **retired in Pass 164**; devourer is the only RF backend, and a
config carrying the old value is rejected with a message that says so.

**(b) UDP sim (no radios)** — DATA fanned out over ethernet to the ground:

```json
"air": { "kind": "udp", "tx": ["<ground-ip>:5801"], "rx": ["0.0.0.0:5810"] }
```

Run (repo root as cwd for `profiles/`): `waybeam-link tx -c <tx>.json`.

### Ground / air side — RX (frame-shm egress)

```json
"streams": [
  { "stream_id": 0, "stream_type": "RTP", "dir": "out", "originator": 17,
    "bind": { "kind": "frame-shm", "name": "venc_frame_out" } }
]
```

**(a) Devourer RX** — one or more adapters on the craft's channel (extra
`role":"rx"` adapters add diversity; one `role":"tx"` carries NACK/LINK_REPORT
returns). Adapters are matched by USB `bus` — or by `mac`, the per-unit EFUSE
identity (§15.2 Pass 154) — never by netdev name:

```json
"adapters": [{ "name": "wlan1", "bus": "5-1", "role": "rx", "channel": 5805, "bw": 20 }],
"air": { "kind": "radio" }
```

A dedicated `cache.store` node with no media streams is one exception to the
designated-uplink rule, and a `node.spectator` display node is the other: all
of their adapters may be `role:"rx"` (§3.11 Pass 162 `allow_rx_only`).
Such a node is RF receive-only; cache status, requests, and replies use its
configured UDP/IP endpoints.
For production MVP it is linked to one receiver with
`cache.store.controller`; that receiver is the only vehicle-selection authority.
The cache never scouts or chooses a vehicle and accepts CACHE_ASSIGN only from
the configured receiver originator and exact UDP source endpoint.

**(b) UDP sim** — mirror the TX targets:

```json
"air": { "kind": "udp", "rx": ["0.0.0.0:5801"], "tx": ["<craft-ip>:5810"] }
```

For UDP benches with ARQ, generate the reciprocal pair from one topology file
so node identities, diversity endpoints, return injection, and preferred peers
cannot drift independently:

```sh
tools/expand_arq_topology.py examples/topology.frame-shm-udp.sample.json \
  --out-dir /tmp/waybeam-pair
```

The expander writes `tx.json` and `rx.json`. `udp.downlink_ports` defines the
virtual diversity paths and `udp.return_port` defines the matched NACK/report
path. It rejects duplicate identities, duplicate downlink ports, and collisions
between the forward and return paths.
Once the named SHM producer exists, validate each generated node with
`waybeam-link tx -c /tmp/waybeam-pair/tx.json --check` and the corresponding
`rx` command.

For a closer RF-broadcast analogue on one Linux host, put both nodes on one
shared loopback broadcast channel:

```json
"air": {
  "kind": "udp-broadcast",
  "tx": ["127.255.255.255:5801"],
  "rx": ["0.0.0.0:5801"],
  "pace_mbps": 10
}
```

Each node receives foreign waybeam packets from the shared channel and filters
its own originator before the socket queue. `pace_mbps` prevents encoded-frame
bursts from becoming accidental host queue loss. Exercise the full frame-SHM
video chain with `AIR_KIND=udp-broadcast tools/frame_shm_udp_bench.sh`.

Run: `waybeam-link rx -c <rx>.json`.

### Verify

Read the ground egress ring with any `venc_frame_ring` consumer, e.g.
`waybeam_venc/tools/frame_shm_consumer_test venc_frame_out <seconds>` — it
validates `VencFrameMeta`, Annex-B start codes, IDR flags, and pts monotonicity
(exit 0 = PASS). Add `air.rx_drop_permille` to the RX config to exercise FEC
recovery under synthetic loss.

**Verified end-to-end** (Star6E .201 → x86 ground): venc `frame-shm://venc_frame`
→ craft `radio` inject (8812EU, MCS0, 5805) → ground `radio` RX → reassemble
→ `venc_frame_out`, read back **byte-clean** (bad_meta=0, bad_startcode=0,
pts_regress=0) at ~90 fps, `decode_errors=0`. Also proven over the udp-air sim
(same, at full bitrate) and the in-process `frame_shm_loopback_test` (FEC recovery
byte-exact).

For a repeatable live-encoder Ethernet run, use
`tools/jscc_ethernet_bench.sh`. It temporarily switches the craft encoder to
frame-SHM, simulates two ground diversity observations over UDP, validates the
reconstructed stream with GStreamer, records per-frame size/arrival data, and
restores the craft configuration on exit. See `docs/jscc-controller-review.md`.

```sh
cmake --build --preset release -j
cmake --build --preset ssc338q -j
tools/jscc_ethernet_bench.sh start
```

The dedicated Ethernet bench leaves encoder bitrate control manual by default,
so changes made through venc remain stable. Decoder-recovery IDR requests stay
enabled independently and do not write encoder configuration. Set
`VENC_CONTROL_ENABLED=1` only when testing waybeam-link's adaptive bitrate
actuator instead.
On the dual-core SSC338Q it also defaults the TX/FEC main thread to CPU 1 and
the SHM readiness thread to CPU 0; override with `VEHICLE_MAIN_CPU` and
`VEHICLE_SHM_CPU` when testing scheduler placement.

The detached bench keeps running after the command returns. View both nodes at
`http://192.168.2.242:8099/`; inspect it with
`tools/jscc_ethernet_bench.sh status`, and stop both endpoints plus restore the
encoder with `tools/jscc_ethernet_bench.sh stop`. For a foreground finite
recorded run, use `FRAMES=1440 tools/jscc_ethernet_bench.sh finite`.

Continuous `start` defaults to leaving the ground egress ring for an external
decoder such as Radeon-VRX:

```sh
tools/jscc_ethernet_bench.sh stop
tools/jscc_ethernet_bench.sh start
tools/jscc_ethernet_bench.sh status
# After Radeon-VRX creates/recreates its decoder pipeline:
tools/jscc_ethernet_bench.sh recover-video
# In-process UDP loss ramp; estimator state is preserved and loss resets to 0:
RAMP_DWELL_S=4 tools/jscc_ethernet_bench.sh loss-ramp
```

For the opt-in P-frame ARQ Ethernet experiment, restart with
`ARQ_MODE=all-frames tools/jscc_ethernet_bench.sh start`. This stamps
non-IDRs retransmit-eligible while retaining the P-frame deadline; the default
is `idr-only`.

The stable application SHM name is `venc_frame_out` (POSIX object
`/venc_frame_out`). `status` prints the active name and consumer mode. To run
the built-in continuous validator instead, stop any external decoder and use
`BENCH_CONSUMER=gst tools/jscc_ethernet_bench.sh start`. The harness rejects a
detected second consumer because the venc frame ring is strictly
single-consumer. Foreground `finite` runs always use the GStreamer trace
consumer.

Replay an artifact with a different relative deadline without rerunning the
encoder:

```sh
python3 tools/jscc_replay.py replay \
  artifacts/jscc-ethernet-*/controller-trace.jsonl \
  --deadline-ms 25 --output /tmp/jscc-decisions.jsonl
```

Each frame records its encoded size, raw SDK PTS/arrival cadence, calculated
symbol size, `k`, target/emitted parity, and whether `k+m <= 256`. Per-frame
path delivery and RTT are explicitly `null` until packet-event and uplink
tracing are added; replay does not infer data the Ethernet frame trace cannot
observe.

Packet-event traces group the existing DATA/NACK wire records by frame block.
They retain source/repair symbol identity, both virtual listener outcomes,
synthetic drops, retransmissions, NACK timing, and final replay outcome. Replay
can replace recorded delivery with deterministic failure models and pin FEC,
ARQ, or deadline discard for ablation:

```sh
python3 tools/jscc_replay.py replay controller-packet-trace.jsonl \
  --loss-model burst --loss-period 100 --burst-length 12 \
  --path-correlation correlated --fec on --arq eligible \
  --rtt-ms 4 --deadline-ms 16
```

Loss models are `recorded`, `none`, `burst`, `incremental`, `low-frequency`,
and `high-frequency`. Synthetic diversity defaults to independent paths;
`--path-correlation correlated` applies the same loss decision to every path.
All timing remains receiver-host-relative. Kernel queue drops are cumulative
stats and cannot be reconstructed as individual packet events.

`recover-video` sends a receiver-to-transmitter recovery request for the
latched RTP stream. The vehicle then requests one IDR from venc; steady-state
GDR remains unchanged. Invoke it only after the replacement decoder pipeline is
PLAYING and accepting SHM buffers. VFRM v1 has no consumer-generation field, so
waybeam-link cannot infer a Radeon-only disconnect/reconnect from the ring while
Radeon continues advancing `read_idx`.

## REST control plane (PROTOCOL.md §15.5)

Every mode (`tx` / `rx` / `loopback`) exposes an optional HTTP/1.0 control
surface — config-gated, folded into the single event loop (no threads/locks),
no auth (bind `127.0.0.1` for host-local, a routable addr on a trusted net):

```json
"control": { "bind": "0.0.0.0:8091" }
```

**Read** (any node): `GET /api/v1/stats` (the §15.3 object), `…/stats/stream`
(SSE, one object per stats tick), `…/info` (identity), `…/health` (terse
`{state,mcs,profile,rssi_best,loss_milli,…}`), and `…/discovery` (bounded
HEARTBEAT-derived nodes plus DATA-derived stream candidates/latches). **Write**
(live, no restart):

| Endpoint | Body | Where |
|---|---|---|
| `POST /api/v1/csa` | `{"mhz":5805,"class":0}` | rx / ground (replaces the old stdin trigger) |
| `POST /api/v1/link/profile` | `{"min":3,"max":3}` | tx (`min==max` pins the MCS+bitrate operating point; `{"max":255}` unpins) |
| `POST /api/v1/fec` | `{"stream_id":0,"i_permille":250,"p_permille":100,"min_k":3}` | tx (frame-shm streams) |
| `POST /api/v1/video/recover` | `{"stream_id":0}` (optional with one latch) | rx / ground; request one decoder-bootstrap IDR from the matched TX |
| `POST /api/v1/stats/reset` | `{}` | any (fresh measurement window) |

A write that doesn't apply to the running mode returns `409`; a malformed body
`400`. Examples:

```
curl -s http://127.0.0.1:8091/api/v1/stats | jq .link
curl -s http://127.0.0.1:8091/api/v1/link/profile -d '{"min":2,"max":2}'   # pin MCS2
curl -s http://127.0.0.1:8091/api/v1/csa          -d '{"mhz":5745}'         # switch channel
curl -N http://127.0.0.1:8091/api/v1/stats/stream                          # live SSE
```

The `tools/link_monitor.py` fleet dashboard rides the UDP stats push and needs
no `control` block; with `control` enabled you can additionally point tooling
straight at each instance's `GET /api/v1/stats/stream`.

## Deployment invariant (must hold before this can drive a craft)

waybeam-link must be the **sole writer** of the encoder's bitrate. venc has no
arbitration flag — last writer wins. On the craft: disable waybeam-hub's
`venc.bitrate_enabled` if hub is present, and do **not** run wfb_ng
`link_controller` (waybeam-link replaces it). See PROTOCOL.md §9.6. The craft runs
**20 MHz** (8812EU 40 MHz bug).

## Licensing

GPL-2.0-or-later (it will vendor devourer, GPL-2.0). See `LICENSE` / `NOTICE`.

## Consumers (future)

The wire codec (`core/`, once written) is vendored — not reimplemented — into
consumers so there is **one** implementation of the packet format. First target:
`Waybeam-android` `:wifi` (which already vendors devourer/libusb the same way).
This avoids the CRSF-style "N independent implementations must stay byte-identical"
drift trap.
