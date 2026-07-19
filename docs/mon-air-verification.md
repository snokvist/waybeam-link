# Kernel-monitor backend + core verification (2026-07-11; updated 2026-07-19)

Bench report for the `kernel-monitor` air backend (`MonAir`, PR #15) and the
core ARQ/broadcast verification it enabled. Companion to `docs/step11-bench.md`.

## Why the pivot

The vendored **devourer** userspace driver cannot transmit 16-QAM+ (MCS4+) on
the RTL8822E in **UNII-3** — including **5805 MHz, the §4.1 gate channel** — even
with the correct per-channel TXAGC values (a deep IQK/TXGAPK/PA-cal gap). The
**Linux kernel driver transmits clean MCS7 at 5805** on the same hardware
(measured: kernel `rtl88x2eu` monitor injection → MCS7, EVM −53). devourer only
exists to make the craft *portable* (a SigmaStar with no kernel WiFi driver); it
is not required to validate the product. So we added a second air backend that
injects/receives raw 802.11 through the kernel driver in monitor mode via
AF_PACKET (the wfb-ng path) — no devourer, no libusb.

## The backend (`air.kind: "kernel-monitor"`)

`MonAir` sits behind the same `create/inject/poll_once` + control-plane shape as
`UdpAir`/`RadioAir` (`app/main.cpp` `AirBackend`). It is **pure POSIX and
compiled unconditionally** — a `WBLINK_RADIO=OFF` build (no devourer/libusb, 298
KB stripped ARMv7) still has a real RF path. The PHY rate rides in a per-packet
radiotap **MCS** field (kernel injection has no out-of-band `SetTxMode`); RX
radiotap yields `DBM_ANTSIGNAL` (RSSI) and `TSFT`. The §3.0 on-air frame is
unchanged — either backend interoperates with either on the RX side (PROTOCOL.md
§3.0 Pass 13). Bring monitor interfaces up with `scripts/mon-up.sh` (mirrors
`S99wfb`: monitor / ch161 / MTU 4052 / txpower auto).

## Results

### Gate 1 — injector + monitor siblings (RF-proven)
`MonAir` runs one AF_PACKET RX thread per adapter plus a TX injector on the
craft's single radio. Desk first-light (EU `rtl88x2eu` craft → CU `rtl88x2cu`
ground, both monitor @ ch161/5805, adapter-doctor HEALTHY before+after):

| MCS | ground rx | delivered | post-div loss | rssi |
|---|---|---|---|---|
| 0 | 2399/2400 | 2397 | 0 | −24 |
| 7 | 2393 | **2391 / 2400 (99.6%)** | 0.2% | −23 |

The same 8822e that gave devourer **0 MCS7 frames at 5805** delivers **99.6%**.

### Vehicle end-to-end (real craft)
298 KB devourer-free binary on `.201`; craft = `wlan0` / kernel `8812eu`
monitor; desk = 2-way diversity ground (CU uplink + EU div). Real craft → desk:

| MCS | delivered | post-div loss |
|---|---|---|
| 0 | 3598/3599 | 0 |
| 7 | **3598 / 3599 (99.97%)** | 0 |

Whole chain proven: vehicle craft → kernel monitor TX → desk diversity RX →
merge → stream egress.

### Gate 2 — physical N=2 walk fade (RF-proven, PASS)

The deferred vehicle-range test ran on channel 161/HT20 with the standard
`/usr/bin/waybeam` producer, MCS5, 10% GF(256) RLC on both frame classes, and
two ground monitor receivers. `wlx40a5ef2f2308` was the designated return TX;
`wlx40a5ef2f229b` was RX-only. The guarded measurement window was 179.4 s:

| measure | result |
|---|---:|
| transmitted / completed frames | 5,378 / 3,491 |
| pre-diversity / post-diversity-pre-ARQ loss | 86‰ / 24‰ |
| FEC / ARQ recovered source symbols | 599 / 52 |
| partially observed unrecoverable frames | 232 |
| both-receiver blackout | 45.7 s total; 37.1 s longest continuous |
| scheduled return reports received | 1,438 / 1,461 (98.4%) |

Diversity removed about 72% of the aggregate pre-diversity loss, and FEC
accounted for 92% of explicit source-symbol recovery once diversity was
exhausted. That is the intended recovery order: diversity primary, light FEC
for the residual, and ARQ as the last opportunistic repair. At the range edge,
however, `2308` was the sole receiving path for 14.0 s while `229b` was sole for
only 0.6 s. The paths therefore help materially but are not independent or
balanced at the tail.

The 37.1 s interval with no packets on either adapter is the expected
same-site correlated-fade limit: 33% static parity would spend airtime without
repairing it, and ARQ cannot cross the same absent channel. The measured
operating ruling is 10% base FEC for N=2 monitor mode; an adaptive 15–20% edge
range may be evaluated, but static 33% is not justified by this result.

### Gate 3 — NACK→RETRANSMIT round-trip (RF-proven, PASS)
Live vehicle→desk link, all-IDR feed (every loss is ARQ-class), 15% synthetic
ground loss to force gaps. Ground NACKs over RF → craft resends (802) → ground
recovers; `tools/gate3_rtt.py` over the run:

| distribution | P50 | P90 | max | vs deadline |
|---|---|---|---|---|
| `nack_rtt` (link round-trip) | ≤8 ms | ≤8 ms | 16 ms | 100% inside 50 ms |
| **`arq_rec` (recovery latency)** | ≤8 ms | **≤8 ms** | 23 ms | **100% inside, `dropped_deadline=0`** |

The single-radio craft return path turns a NACK around in ~4–8 ms — comfortably
inside the §14 I-frame deadline (1–2 frame periods ≈ 16–33 ms @ 60 fps). **ARQ
is in-deadline.**

### ARQ/broadcast + §14 regime (loopback, deterministic GE bursts)
ARQ accounting verified consistent (TX-stream `resends_sent`, RX-stream
`nacks_sent`/`recovered_arq`). Correlation sweep, all-IDR, GE burst loss:

| ρ | delivered % | post-div loss | NACKs | ARQ recovered | residual (glass-visible) |
|---|---|---|---|---|---|
| 0.10 | 99.83 | 6‰ | 10 | 9 | 3 |
| 0.40 | 99.56 | 15‰ | 25 | 23 | 8 |
| 0.70 | 98.72 | 40‰ | 61 | 53 | 23 |
| 0.90 | 96.89 | 62‰ | 80 | 63 | 56 |
| 0.95 | 96.33 | 70‰ | 88 | 66 | 66 |

Quantifies §14: **low ρ → diversity + ARQ ≈ lossless (no-FEC stands); ρ→1 →
diversity collapses, ARQ recovery saturates** (recovered plateaus ~63–66 while
NACKs keep climbing), and the residual undelivered tail grows ~22× (3 → 66) —
the only thing a GF(256) RLC would target. (XOR/GF(2) "lightweight" FEC is
rejected by §14 regardless.)

## Remaining

- **Gate 4** — return-window fit + adaptive-loop stability under a saturating
  injector (quiet-gap vs opportunistic; §9.8 damped step pair).
- **Stationary sequence complete (Pass 48):** 9,000-frame N=2 decoder/EOS
  soak passed; `229b` failover/failback works when the CU interface receives a
  full monitor reinitialization; `229b` return TX is a driver/device silent-TX
  failure and remains RX-only.
- **UDP/IP cache timing/order verified (Pass 50):** first-reply/completion P95
  was 2.845/2.910 ms. A targeted 3 ms first-NACK lead cut NACK packets 22.5%
  and vehicle resends 21.3% in clean matched decoder runs. Any monitor-radio
  cache proposal must re-derive this seed because replies then share RF
  airtime.
- Hardening follow-ups from PR-#15 review: radiotap FCS-flag check, own-TX
  loopback suppression (kernel ≥4.20 / BPF), moved-from asserts.
