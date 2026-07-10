# waybeam-link

A best-effort broadcast video tunnel with per-adapter receive **diversity** as
the primary redundancy layer, opt-in importance-gated **ARQ** as an opportunistic
patch, and an **adaptive link layer** (per-MCS rate/power + encoder-bitrate
control) — built on OpenIPC **devourer** for raw 802.11 monitor/injection.

> **Status: SPECIFICATION + GROUNDWORK ONLY. No implementation yet.**
> This repo is under deliberate multi-pass review before any code is written.
> Do not start `core/` until the spec is signed off.

## What this is

- A **single logical ground receiver** merging N same-channel adapters into one
  dedup/gap/NACK state machine (diversity combining by packet-seq dedup).
- **RTP carried opaque** end-to-end; the transport core never parses it. The only
  codec awareness is an isolated RTP profile (NAL classifier) that sets one
  importance bit.
- An **adaptive controller** that scores the RF link from RX feedback and drives
  MCS, TX power (per-rate), and the encoder's bitrate — the last one via a local
  same-SoC API call when TX and the encoder share a board.

## What is genuinely new vs. borrowed (be honest about this)

The **transport core** — same-channel multi-adapter RX diversity + dedup +
injection — already exists in `waybeam_wfb_ng`. The genuinely novel contributions
here are:
1. **opportunistic ARQ** (instead of FEC) targeting the short correlated-fade band,
2. the **opaque passive-latch session model** (discover/subscribe by stream type
   with no handshake, no association), and
3. a clean, minimal, **FEC-free** reimplementation decoupled from the wfb_ng/wfb
   pipeline (the reason the Android devourer path was built in the first place).

The **adaptive link layer (§13–14)** is **lifted and adapted** from the production
`waybeam_wfb_ng` `link_controller` — its control discipline (reactive-demote /
probe-promote, MCS↔bitrate sequencing, flap-freeze) and its on-air-validated
constants are reused. Its *mechanism* (FEC coupling, probe side-stream, SHM
backpressure) is adapted to the inject+diversity+ARQ transport. **The adaptive
layer is not new science; the diversity+ARQ+latch transport is the new part.**

## Layout

```
PROTOCOL.md          Canonical protocol spec (§1–14). Single source of truth.
docs/
  groundwork.md      Calibration source-of-truth: every §13–14 constant traced
                     to link_controller.c / venc_api.c / the rtl88x2 driver,
                     with file:line citations and the adversarial corrections.
  build-order.md     Suggested build order + the two de-risking measurements
                     that must pass before the ARQ/adaptive machinery is trusted.
  review-log.md      Running log of review passes (this repo is reviewed several
                     times before code starts).
profiles/
  table.example.json The §13.3 operating-point table (data, not code).
```

## Deployment invariant (must hold before this can drive a craft)

waybeam-link must be the **sole writer** of the encoder's bitrate. venc has no
arbitration flag — last writer wins. On the craft: disable waybeam-hub's
`venc.bitrate_enabled` if hub is present, and do **not** run wfb_ng
`link_controller` (waybeam-link replaces it). See PROTOCOL.md §13.6.

## Licensing

GPL-2.0-or-later (it will vendor devourer, GPL-2.0). See `LICENSE` / `NOTICE`.

## Consumers (future)

The wire codec (`core/`, once written) is vendored — not reimplemented — into
consumers so there is **one** implementation of the packet format. First target:
`Waybeam-android` `:wifi` (which already vendors devourer/libusb the same way).
This avoids the CRSF-style "N independent implementations must stay byte-identical"
drift trap.
