# waybeam-link

A best-effort, **latency-first** broadcast video + telemetry link for
monitor/injection WiFi (RTL8812AU/CU/EU) with per-adapter receive **diversity** as
the primary redundancy, opt-in importance-gated **ARQ** as an opportunistic patch,
an **adaptive link layer** (per-MCS rate/power + encoder-bitrate control), and a
coordinated **follow-me channel switch** — built on OpenIPC **devourer** for raw
802.11 monitor/injection.

> **Status: IMPLEMENTATION IN PROGRESS — build-order steps 1–7 built.**
> Wire codec, I/O/config/stats, TX framer + resend ring, merged RX engine,
> resend scheduler + arbitration, the loopback bench + udp-air dev backend,
> and the §4.1 NAL-type ARQ classifier (H.264/H.265, per-stream
> `"classifier"` knob) are implemented and tested (`ctest --preset dev`,
> ASan+UBSan; SSC338Q cross-verified via `cmake --preset ssc338q`). The radio
> path (devourer) and the adaptive/CSA layers are steps 8–11 — see
> `docs/build-order.md` and the §17 bench gates. Try it without hardware:
> `./build/dev/waybeam-link loopback -c examples/config.loopback.sample.json`
> or the two-process udp-air pair (`examples/config.air-{tx,rx}.sample.json`).

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
