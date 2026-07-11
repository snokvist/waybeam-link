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
> campaigns, craft follower with TSF-anchored switch + auto-revert + home
> rendezvous, ground issuer with commit-after-CSA_ARMED, §11.3 selector
> freeze; ground trigger = stdin `csa <mhz> [class]`) — are implemented and
> tested (`ctest --preset dev`, ASan+UBSan; SSC338Q cross-verified via
> `cmake --preset ssc338q`).
> Step 11 (field bring-up + the §17 bench gates) has **run** on the x86 bench
> (2× RTL8812CU + 1× RTL8812AU): the §3.0 on-air encapsulation is
> field-verified (monitor RX delivers the MPDU with a +4-byte FCS trailer,
> stripped before parse); gate 1 **PASSED** (injector + monitor siblings mix
> in one process, both chip families, per-frame CCX `tx.report` live); gate 3
> **PASSED** (NACK→RETRANSMIT recovery P90 ≤4 ms at 65% airtime, well inside
> the 40 ms I-frame deadline; ARQ ceases past saturation by design); gate 2
> machinery is validated and **desk-partial** (single-adapter synthetic fade
> behaves textbook-independent; the correlated-fade verdict is deferred to
> vehicle range testing); gate 4 observables are live (return-window
> paced-hit ratio 97%→77% across load). See `docs/step11-bench.md` for the
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
for gate-2 machinery validation.

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
