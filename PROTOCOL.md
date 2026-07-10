# waybeam-link — Protocol Specification (v1)

A best-effort broadcast video + telemetry link for monitor/injection WiFi
(RTL8812AU/CU/EU via OpenIPC **devourer**), designed **latency-first and
robustness-first**; bandwidth efficiency is subordinate. Broadcast, **no
encryption / no authentication / no MAC layer on the data path** (one scoped
exception: the channel-switch command, §11). Multi-adapter **receive diversity**
is the primary redundancy; opportunistic importance-gated **ARQ** patches the
short correlated-fade band. RTP is carried opaque end-to-end.

> This v1 supersedes the v0 draft. It folds in the Pass-3 adversarial review
> (`docs/findings-pass3.md`) and the operator rulings recorded there. Calibration
> provenance for every inherited constant is in `docs/groundwork.md`; the review
> trail is in `docs/review-log.md`.

---

## 0. Conventions

- **Endianness:** every multi-byte field is **big-endian (network order)**. The
  magic is transmitted as the two bytes `0x57 0x42` (ASCII `"WB"`).
- **Byte offsets** in the header tables are relative to the start of the
  waybeam-link header (i.e. the first byte after devourer's 802.11 MAC payload
  boundary). devourer owns radiotap + the 802.11 MAC header; waybeam-link owns
  everything below.
- **Units:** loss in per-mille ‰ (0–1000); TX power in quarter-dB `qdb`; time in
  ms unless a field name says `_us`; bitrate in kbps.
- **"Craft"** = the air vehicle (single radio, dominant video producer).
  **"Ground"** = a receiving node (may have multiple diversity adapters).
- **RE-DERIVE / bench-gated** constants are seeds transplanted from wfb_ng or
  first-principles estimates; they are marked and must be confirmed empirically
  (§17). They are not authoritative flight values.

---

## 1. Scope and position in the stack

```
  ┌─────────────────────────────────────────────┐
  │ producers / consumers                        │  RTP video, MAVLink, telemetry…
  │ (gstreamer, ffmpeg, FC, apps)                │  via shm / unix / UDP (§15)
  ├─────────────────────────────────────────────┤
  │ waybeam-link                                 │  THIS SPEC
  │   TX: framer + resend ring + scheduler       │
  │       + adaptive selector + CSA              │
  │   RX: merge/dedup + gap-detect + NACK gen    │
  │       + metric reporter                      │
  ├─────────────────────────────────────────────┤
  │ devourer (vendored)                          │  raw 802.11 inject / monitor
  ├─────────────────────────────────────────────┤
  │ RTL8812AU/CU/EU radio(s)                      │  craft: 1 · ground: N (diversity)
  └─────────────────────────────────────────────┘
```

**Hardware topology (fixed constraints — do not design around alternatives):**

- **The craft has exactly one radio, permanently.** Diversity is a
  **ground-only** capability. The craft therefore timeshares its single adapter
  between TX-video (dominant) and RX-return, and can only hear return traffic
  when its own radio is idle (§7). There is no second-craft-adapter escape; the
  craft return path is best-effort by physics and the design accommodates that,
  it does not remove it.
- **Ground may run N adapters in one process.** Proven at N=3 (per-adapter
  `libusb_context` + per-adapter RX thread, RX-only; `Waybeam-android
  :wifi/wifi_jni.cpp`). One ground adapter is appointed the **designated uplink
  TX** for NACK/LINK_REPORT/CSA; its RX blind spot while transmitting is covered
  by the diversity siblings.

**Invariants (carried down from design; do not silently revisit):**

- RTP is opaque on the wire. The transport core never parses it. The only
  RTP/codec awareness lives in the RTP **profile** (framer classifier, §4.1).
- Reliability is **not load-bearing.** Per-adapter diversity is the primary
  redundancy. ARQ is opportunistic for the short correlated-fade band (~5–30 ms;
  **bench-gated §17**). Under saturation ARQ quietly does less and the link
  degrades toward pure diversity — the accepted floor.
- **One merged RX state machine per RX node** → one NACK generator per node → no
  intra-node implosion. Multiple RX *nodes* on-air are first-class (§12); ARQ
  service across nodes is arbitrated by the TX (first-latcher lock, §12).
- No data-path authentication. No time synchronisation between TX and RX; all
  deadlines are RX-local wall-clock; all cross-node timing anchors on the
  hardware TSF the radio latches per frame (§7).
- Same-channel diversity (the craft has one channel at a time). Frequency changes
  happen only as a **coordinated follow-me switch** (§11), never per-packet.
- Injected/broadcast frames get **no** 802.11 MAC ARQ. Application-level resend is
  the only retry. CSMA carrier-sense still applies (the half-duplex cost).

---

## 2. Identifiers and session model

Three orthogonal identities, never conflated:

| id | width | scope / meaning |
|---|---|---|
| `originator` | u16 | **Config-assigned stable node ID** (operator-set, like a tail number). Identifies **who transmitted this frame**. On every packet, in the common prefix. High byte MAY encode a squad, low byte a craft — operator convention. |
| `destination` | u16 | Advisory filter naming an intended recipient node. `0x0000` = broadcast/any. **Non-strict:** an RX MAY still process a frame not addressed to it (semi-anarchy). Not access control. |
| `session_id` | u32 | Random per-boot nonce **of the sender**. Namespaces that originator's seq/block spaces and doubles as a reboot/anti-replay epoch. |
| `stream_id` | u8 | Instance index; disambiguates multiple streams from one originator. |
| `stream_type` | u8 | Semantic kind → selects the profile (§3.4, §4). |
| `seq` | u32 | Monotonic per `(originator, session, stream)`. Global order + dedup key. No wrap within a flight → plain integer comparison. |
| `block_id` | u32 | Monotonic per `(originator, session, stream)`. One block = one RTP frame. |

- **Discovery / latch:** an RX in monitor mode passively enumerates
  `(originator, session_id, stream_id, stream_type)` tuples on the channel. It
  routes/subscribes by type ("all telemetry") or by a specific instance without
  parsing payload, then latches. No handshake, no association. **All per-stream
  RX state — the discovery cache, the startup floor, delivery cursors, deadline
  and supersession state — is keyed by the full `(originator, session_id,
  stream_id)`.** (`originator` is *additive* to `session_id`: two nodes may
  randomly pick the same session nonce; `originator` disambiguates.)
- **Discovery admission control (anti-flood, §13):** a tuple enters the operator's
  latch-picker only after **N_admit=3 packets over T_admit=1 s**; unqualified
  tuples are LRU-aged out. A one-shot forged packet never qualifies.
- **stream_type resolution:** the cache tracks the type seen on *sustained /
  most-recent* traffic, not strict first-seen, so a single early forged packet
  cannot pin a misclassification.
- **Startup floor:** on latch, RX adopts the first-seen `seq` as its floor and
  never NACKs below it (no back-filling history on join).
- **Teardown:** implicit. Nothing heard for a session within an idle timeout →
  RX drops its state. No explicit close on the wire.

---

## 3. Wire header

### 3.1 Common prefix (all packet types) — 11 bytes

| off | size | field | notes |
|---|---|---|---|
| 0 | 2 | `magic` | `0x5742` (bytes `57 42`) — protocol guard |
| 2 | 1 | `ver_type` | hi nibble = version (`0x0`), lo nibble = packet type |
| 3 | 2 | `originator` | sender node id (§2) |
| 5 | 2 | `destination` | advisory; `0x0000` = broadcast |
| 7 | 4 | `session_id` | sender boot nonce |

**Packet types** (low nibble): `0x1 DATA · 0x2 NACK · 0x3 LINK_REPORT ·
0x4 HEARTBEAT · 0x5 CSA`. 5 of 16 used; the version nibble will not ship 16
wire-incompatible revisions, so there is no type-budget scarcity. Future types
(e.g. a dedicated FEC-repair type, §14) take free slots.

**Common prefix describes the SENDER.** Control packets (NACK, LINK_REPORT) name
the stream they concern via a **target descriptor** in their body (§3.3, §3.5) —
that is a *different* identity from the sender in the prefix. This two-identity
split is what lets any node send control traffic about any other node's stream.

### 3.2 DATA packet — 26-byte header

| off | size | field | notes |
|---|---|---|---|
| 0 | 11 | *common* | §3.1 |
| 11 | 1 | `stream_id` | |
| 12 | 1 | `stream_type` | profile selector (§3.4) |
| 13 | 4 | `seq` | per-stream monotonic |
| 17 | 4 | `block_id` | per-stream monotonic; RTP frame boundary |
| 21 | 1 | `data_flags` | see below |
| 22 | 1 | `active_profile` | operating point TX is currently on (§9) |
| 23 | 1 | `table_version` | profile-table content hash (§3.6) |
| 24 | 2 | `payload_len` | length of opaque payload |
| 26 | var | `payload` | opaque RTP bytes (never parsed by core) |

`data_flags` bits:

| bit | name | meaning |
|---|---|---|
| 0 | `END_OF_BLOCK` | last packet of this block |
| 1 | `ARQ` | block is retransmit-eligible (importance / opt-in) |
| 2 | `RETRANSMIT` | this packet is itself a resend (stats/diagnostics) |
| 3 | `FEC_REPAIR` | packet is a FEC repair symbol (§14; a 6-byte subheader precedes payload) |
| 4 | `CSA_ARMED` | **craft→ground ARM ack** — craft has accepted the in-flight CSA campaign and will follow (§11.6) |
| 5–7 | reserved | 0 |

**Redundant per-packet metadata (critical rule):** `stream_type`, `block_id`,
`END_OF_BLOCK` membership, the `ARQ` flag, `active_profile`, and `table_version`
are stamped on **every** packet of a block, not just the first. A surviving
packet of a block reveals the block's boundary, ARQ-eligibility, and the TX's
operating point/table even if the first packet was lost.

**Header overhead:** 26 B on a ~1450 B usable MPDU → **1424 B max payload**
(~1.8%).

### 3.3 NACK packet — 23-byte fixed + bitmap

| off | size | field | notes |
|---|---|---|---|
| 0 | 11 | *common* | §3.1 — sender = the RX node asking |
| 11 | 2 | `target_originator` | u16 — TX node whose stream is repaired |
| 13 | 4 | `target_session` | u32 |
| 17 | 1 | `target_stream_id` | u8 |
| 18 | 4 | `base_seq` | anchor for the bitmap |
| 22 | 1 | `bitmap_len` | bytes of bitmap following |
| 23 | var | `bitmap` | SACK-style; bit *i* set ⇒ `base_seq + i` missing |

- References **seqs**, not blocks. RX lists a missing seq only if its block is
  live: `ARQ`-flagged, not superseded (§6), within deadline (§8).
- No deadline field; TX applies its own resend deadline (no clocks cross).
- `target_stream_type` is omitted — TX resolves it from `(session, stream_id)`.

### 3.4 Stream-type registry

| value | name | profile | notes |
|---|---|---|---|
| `0x00` | UNKNOWN | best-effort (default) | unspecified / reserved |
| `0x01` | RTP | RTP profile | video; NAL classifier applies (§4.1) |
| `0x02` | TELEMETRY | best-effort / app-defined | MAVLink/MSP-style small packets |
| `0x03` | CONTROL | app-defined | RC / command (uplink) |
| `0x10–0xEF` | user | build-defined | experimental / vendor |
| `0xF0–0xFF` | reserved | — | |

**Unknown-type / table-mismatch rule (forward compatibility):** an RX that sees a
`stream_type` it does not recognise, **or a `table_version` that does not match
its own local profile table (§3.6)**, MUST treat that stream under the
**best-effort default profile** — deliver by diversity, never NACK, no
supersession/deadline/adaptive logic — and raise a stat. A new type or a diverged
table can therefore never make an older/mismatched client misbehave.

### 3.5 LINK_REPORT packet (type `0x3`, RX→TX) — 39 bytes

| off | size | field | notes |
|---|---|---|---|
| 0 | 11 | *common* | §3.1 — sender = the reporting RX node |
| 11 | 2 | `target_originator` | u16 — TX node being scored |
| 13 | 4 | `target_session` | u32 |
| 17 | 1 | `target_stream_id` | u8; `0xFF` = node-scope (RF health is per-link) |
| 18 | 4 | `report_epoch` | u32 monotonic; TX watchdog + change-took-effect (u32 = 13.6 y @10 Hz; no wrap in a flight) |
| 22 | 1 | `table_version` | table RX scored against (§3.6) |
| 23 | 1 | `rssi_best` | i8, dBm — best adapter |
| 24 | 1 | `rssi_mean` | i8, dBm — fleet mean (TX derives slope from the series) |
| 25 | 2 | `loss_postdiv_prearq` | u16, ‰ — **post-diversity, pre-ARQ** delivered loss (see §3.7) |
| 27 | 4 | `uniq` | u32 — unique packets this interval (loss denominator) |
| 31 | 4 | `diversity` | u32 — duplicate copies across adapters (decorrelation gauge) |
| 35 | 1 | `adapters` | u8 — latched, *non-stalled* adapter count (§6.5) |
| 36 | 2 | `probe_per` | u16, ‰ — promote-probe PER; `0xFFFF` = no probe |
| 38 | 1 | `recommended_prof` | u8 — RX hint; TX has final authority |

- Injected via the designated uplink TX adapter (§6.4), same accounting as NACK.
- `diversity`/`adapters` let TX and the bench compute **cross-adapter loss
  correlation ρ** (§17 gate 2).
- **Acceptance filter (anti-spoof, §13):** TX accepts a LINK_REPORT only from the
  **currently latched / preferred `(originator, session_id)`** for that target,
  and cross-checks plausibility (reported loss vs TX-observed NACK behaviour).
  Conflicting concurrent reports for one target ⇒ **fail toward degradation**
  (§9.8), never toward the optimistic one.

### 3.6 `table_version` — content hash, not a counter

`table_version` is an **8-bit content hash (CRC-8) of the canonical serialization
of the profile table** (§9.3), not a hand-bumped generation number. A manual
counter detects only *lineage* skew; two independently edited tables both labelled
"3" would reproduce the exact silent-divergence hole this field exists to close
(same `active_profile` index → different MCS/power/deadline on each end). The hash
detects *content* skew: any table difference changes the hash, and an RX compares
the DATA/LINK_REPORT `table_version` against its own local table's hash. Mismatch
→ best-effort default profile (§3.4). Collision risk (1/256) is acceptable for a
liveness check; widen to CRC-16 in `ver_type` version 1 if field data shows
collisions.

### 3.7 The `loss_postdiv_prearq` semantics (do not confuse with wfb_ng)

This field is **post-diversity, pre-ARQ delivered loss** — the loss remaining
after the merged RX state machine has combined all adapters, before ARQ repairs.
It is **not** wfb_ng's `rx_ant` "pre-diversity" loss. The distinction is
load-bearing: waybeam-link has **no FEC underneath**, so this number is close to
true delivered video loss, and the adaptive demote threshold (§9.1) is tuned
against it accordingly (~20‰, not wfb_ng's pre-FEC 80‰). The stats output (§15)
additionally exposes raw `loss_prediversity` for ρ analysis; the two must never be
conflated in code.

---

## 4. Block model and profiles

A **block** = packets sharing a `block_id`, delimited by `END_OF_BLOCK`. For the
RTP profile, one block = one RTP frame (marker/timestamp boundary). The active
profile is selected on the wire by `stream_type`; both ends apply the matching
policy with no out-of-band agreement.

| policy | RTP profile | future bulk/file profile |
|---|---|---|
| boundary | RTP frame (marker / timestamp change) | fixed-size or app-defined |
| supersession | newer block ⇒ drop older incomplete (deadline) | ignored (no deadline) |
| ARQ eligibility | per-block by codec classifier (§4.1) | always, or app-defined |
| deadline | per-block wall-clock budget; longer for I-frames | none |

The core is profile-agnostic: it carries `block_id` + `data_flags` and executes
the active profile's policy.

### 4.1 RTP profile — ARQ classification

The RTP framer sets `ARQ` per block from a shallow classifier reading **only the
NAL unit type**:

- **H.264:** IDR / coded-slice-of-IDR + parameter sets (SPS/PPS) ⇒ `ARQ=1`;
  non-IDR coded slices ⇒ `ARQ=0`.
- **H.265:** IDR_W_RADL / IDR_N_LP / CRA + VPS/SPS/PPS ⇒ `ARQ=1`; else `0`.
- Handle STAP/FU aggregation/fragmentation only enough to find the contained NAL
  type — no deeper parsing.

**Pure-agnostic fallback:** classify by block size (above an adaptive threshold ⇒
important). Cruder, zero codec coupling, selectable per build.

**Deadline coupling:** `ARQ`-important blocks carry a longer retransmit deadline
than best-effort blocks (a slightly-late I-frame still rescues its GOP).

---

## 5. TX behaviour

### 5.1 Framer (per stream)
1. Read a datagram from the ingress binding (§15).
2. Assign `block_id`; detect RTP frame boundary → increment `block_id`, set
   `END_OF_BLOCK` on the finished block's last packet.
3. Run the profile classifier → set `ARQ`.
4. Assign monotonic `seq`; stamp `active_profile` + `table_version`; build the
   DATA header; hand to devourer for injection.
5. Push `(seq → payload, block_id, flags, first-seen-tx-time)` into the resend
   ring.

**No fragmentation (invariant).** Each ingress datagram MUST fit one MPDU
payload. Configure the encoder's RTP payloader `mtu = 1400` (24 B margin under
the 1424 B DATA payload budget). H.264/H.265 payloaders already fragment NALs to
MTU — this is a config assertion, not new code. A runtime datagram larger than
the payload budget is **dropped with a stat** (`oversize_ingress`), never
silently truncated.

### 5.2 Resend ring
- Recently sent packets for a bounded window (~50 ms, **bench-gated §17**; ~125 KB
  at 20 Mbps). Lookup by `seq` for NACK service. Eviction by age; a packet older
  than its deadline is dropped.

### 5.3 Scheduler / priority
- **Live packets strictly highest priority.** Retransmits strictly lower.
- **Airtime cap:** retransmits capped at a hard fraction of downlink airtime per
  interval, **partitioned per originator** (§12) so one requester cannot drain
  the pool.
- **Global per-seq hold-down (load-bearing anti-amplification, §13):** after
  resending seq *N*, suppress re-resending it for a window **keyed by seq
  globally** (not per requester). 1000 NACKs for seq *N* ⇒ **one** resend.
- **Freshness-priority within the budget:** serve resends by deadline-remaining
  (most-recoverable first), not FIFO; proactively drop any seq whose remaining
  deadline < one measured NACK round-trip (§17 gate 3) — it cannot arrive in time.
- **Importance gate:** only `ARQ=1` blocks are ever resent.
- **Deadline gate:** never resend past deadline.
- **Attempt cap:** bounded resend attempts per seq.
- Mark every resend `RETRANSMIT=1`.

---

## 6. RX behaviour (one merged state machine per node)

All local adapters feed one pipeline. Per adapter, receive order == air order (no
intra-adapter reorder). Reorder exists only across adapters (host-side delivery
jitter: URB timing, driver batching, scheduler), not propagation skew.

### 6.1 Ingest + dedup
- Key each DATA packet on `(originator, session, stream, seq)`. First copy wins;
  later duplicates from other adapters are counted (→ `diversity`) and dropped.
- Maintain per-adapter "highest delivered seq" for the short-circuit below.

### 6.2 Gap detection with short-circuits (priority order)
A missing seq is declared **lost** as soon as *any* of:
1. **All-adapters-advanced (fast path):** every latched, non-stalled adapter has
   delivered a seq greater than the gap ⇒ none heard it ⇒ lost immediately.
2. **Block supersession (RTP profile):** a newer `block_id` has any received
   packet ⇒ older incomplete blocks are past deadline ⇒ their missing seqs are
   lost and **not** NACKed (superseded). Advances the delivery cursor past the
   hole (kills head-of-line blocking) and suppresses pointless NACKs.
   **Guarded by the plausible-forward clamp (§6.6)** so a forged far-future
   `block_id` cannot force a flush.
3. **Dwell ceiling (rare backstop):** an adapter went silent (delivered nothing ≥
   the gap) so 1 can't fire, and the stream is live so 2 hasn't. After a fixed
   dwell (**bench-gated §17**), declare lost. The only timer path; almost never
   hit.

### 6.3 Delivery
- Deliver in-order, best-effort, out the egress binding (§15) as untouched RTP.
- "Drop" = *stop recovering + advance cursor*, never withhold already-held
  packets; the decoder's jitter buffer discards genuine late arrivals.

### 6.4 NACK generation
- For a lost seq that is `ARQ`-flagged, not superseded, within deadline: add to
  the pending SACK set. Coalesce into one bitmap per return window (§7), anchored
  at `base_seq`.
- Send via the **designated uplink TX adapter**; its RX blind spot while
  transmitting is covered by the diversity siblings (ground half-duplex is free).
- Re-NACK: bounded retries with backoff; stop on RETRANSMIT receipt or on
  deadline/supersession.

### 6.5 Adapter liveness watchdog (anti-phantom-diversity)
An adapter delivering **zero** frames for `stall_timeout` (seed 200 ms) while
siblings deliver is marked **stalled** and **excluded** from the `adapters` /
`diversity` counts and from the §6.2-1 fast path. Rationale: a stalled-but-latched
adapter (e.g. the RTL88x2 USB TX-wedge that needs a physical re-plug) would
otherwise inflate the redundancy the TX trusts, making it ride a higher-MCS /
lower-power point on phantom diversity → real delivered loss spikes. The stall is
surfaced as `adapter_stalled` in the stats (§15).

### 6.6 Plausible-forward clamp (load-bearing injection defence, §13)
RX rejects any DATA `seq`/`block_id` or NACK `base_seq` that jumps more than
`fwd_clamp_K` (seed: a few blocks / one resend-ring) ahead of the current cursor.
Real monotonic traffic never jumps by millions; this single check neutralises
forged far-future `block_id` video-flush, garbage NACK bitmaps, and discovery
cursor poisoning.

---

## 7. Air-side uplink transport + return-telemetry contract

**Shared-channel, best-effort.** NACK / LINK_REPORT / CSA-ack traffic is injected
back over the same WiFi via devourer; no separate backchannel. The return path is
**best-effort by physics** and the design does not pretend otherwise — devourer
gives no per-packet TX-departure timestamp, a TX cannot *reserve* airtime without
TDMA, and (critically) **the craft, single-radio, is RX-deaf while it transmits.**

### 7.1 Baseline — opportunistic return (always on, ships first)
RX injects NACK/LINK_REPORT on its designated uplink adapter using devourer's
CCA, with **no timing contract** (the wfb_ng-proven model). This is the baseline
the protocol operates on; everything below is an optimisation layered over it.

### 7.2 Optimisation — TSF-anchored quiet-gap (the craft's primary return path)
Because the craft can only hear returns when its own radio is idle, the quiet gap
is **not optional for the craft** — it is the craft's principal opportunity to
receive. The mechanism, using only RX-local hardware TSF (no clock crossing):

- When ground RX receives the craft's `END_OF_BLOCK`, its hardware `tsfl` latches
  the arrival instant. Propagation is ~µs, so **that receive-TSF and the craft's
  real send-of-EOB are the same physical instant.** Ground schedules its return
  at `rx_tsfl(EOB) + guard_us + return_window_us/2`, aiming for the middle of a
  gap the craft paces after each EOB.
- The craft, after injecting an EOB, **paces a quiet window** `[EOB + guard_us,
  EOB + guard_us + return_window_us]` in which it does not inject video (skipped
  only if the next block is already airtime-critical), and listens.
- **`guard_us` MUST cover `max(ground turnaround, craft TX→RX AGC/PLL settle)`** —
  any radio is briefly unable to receive cleanly for the moment its receiver
  AGC/PLL settle after a transmit; that turnaround is a normal physical cost, not a
  chip defect. (8812EU concurrent RX+TX is proven in the field on wfb_ng; the one
  known real limitation is a **40 MHz-bandwidth bug** → run the craft at **20 MHz**,
  which also matches the intra-band fast-retune path in §11.) The exact settle time
  is measured at §17 gate 4. Seeds: `guard_us = 300`, `return_window_us = 2000`.

**Crossover (state it, don't hide it):** at high fps + saturated bitrate the idle
gap shrinks below `return_window_us` and the contract degrades to §7.1
best-effort. This optimisation, its window fit, and whether the damped adaptive
loop (§9) holds a stable operating point rather than oscillating at the floor, are
**resolved empirically at §17 gate 4** — not designed further on paper.

### 7.3 Cadence
- **NACK:** event-driven on loss declaration, coalesced to one bitmap per return
  window, rate-limited by the global per-seq hold-down (§5.3).
- **LINK_REPORT:** periodic **10 Hz** floor (bench-gated) **+ immediate** on a
  step change (RSSI-floor breach, loss spike), to cut reaction latency.

### 7.4 Self-congestion guard
Retransmit < live priority; airtime cap as a hard downlink fraction; resend
attempt cap; per-interval bound. A burst needing more than a few repairs is past
saving — let RTP concealment eat it. The uplink is a **pluggable transport** so a
dedicated backchannel could replace it later without touching the core.

---

## 8. Deadline and retry semantics

- Each block gets a first-seen wall-clock timestamp at RX (and at TX for its
  ring). Deadline = first-seen + budget(profile, importance).
- RX never NACKs past deadline; TX never resends past deadline.
- No clock crossing — each side applies its own local budget; the ring window and
  NACK window overlap by design (the overlap size is `return_window_us` +
  ring-age, a *sized* quantity measured at §17 gate 3, not an assumption).
- Importance-longer deadline for `ARQ` I-frame-class blocks (§4.1).

---

## 9. Adaptive link layer (link selection)

> Amends §3.2 (DATA `active_profile`), §5.3 (scheduler), §7 (return), §15 (stats).
> Constants trace to `docs/groundwork.md`; those marked RE-DERIVE are wfb_ng seeds
> whose original tuning assumed a FEC-protected stream and must be re-derived for
> waybeam-link's no-FEC, latency-first regime.

### 9.0 Objective (latency/robustness-first)

> Among operating points whose **delivered** (post-diversity, post-ARQ) loss and
> latency meet the target, choose the one **most likely to deliver the next frame
> intact within its deadline** — maximum robustness margin. Airtime/energy is a
> **subordinate tiebreak** only among points statistically equivalent on
> robustness.

Formally, for feasible candidate rung *i*: pick `argmax margin(i)`, where
`margin(i)` combines `(loss_target − predicted_delivered_loss(i))` with deadline
slack; tiebreak by lower airtime (higher MCS), then lower `tx_power_qdb`.
Diversity is "permission to run a lower-airtime point" — but only *after*
robustness is satisfied, never at its expense.

### 9.1 Control split + decision cascade

- **RX reports RF metrics** at ~10 Hz (§7.3). It does not decide.
- **TX runs the cascade and actuates.** MCS/power and encoder bitrate are decided
  **together at TX** from (a) RX metrics and (b) a **TX-local backpressure signal**
  (venc output-queue fill; never crosses the air). Co-locating makes the
  MCS↔bitrate transition (§9.5) atomic and local.

**Decision law = rule cascade, first match wins:**
1. **Reactive demote** — `smoothed(loss_postdiv_prearq) ≥ demote_milli`.
   **Seed 20‰ (2%), RE-DERIVE.** (Not wfb_ng's 80‰: that was pre-FEC loss an FEC
   layer then absorbed; there is no FEC here, so react ~4× earlier.) Suppressed
   under local backpressure (§9.9).
2. **RSSI-floor demote** — `smoothed_rssi ≤ rssi_floor_dbm` (**−85 dBm**). KEEP.
3. **RSSI-fade demote** — `slope ≤ −rssi_fade_db_per_s` (**−10 dB/s**) AND
   `rssi ≤ rssi_fade_arm_dbm` (**−65 dBm**) for 3 ticks. KEEP.
4. **Backpressure escape** — sustained local pressure ≥ `pressure_escape_s`
   (**2.0 s**) with clean RF ⇒ climb one rung per `down_cooldown_s` (anti-death-
   spiral). KEEP.
5. **Promote** — the §9.4 path. 6. **Hold.**

Demotes rate-limited by `down_cooldown_s` (**0.2 s**), never waiting on dwell.
EWMA α = **0.3** (RSSI/loss, RE-DERIVE), slope α = 0.5.

### 9.2 Max-probability fallback rung (Minstrel-derived, lightweight)
Maintain **one** success-EWMA per rung, fed by `loss_postdiv_prearq`. The
**max-probability rung** = highest recent delivery probability; under multi-rung
stress the cascade demotes **toward it**, not blind `min−1`. Reject all Minstrel
A-MPDU/aggregation/sampling machinery (no MAC-ACK referent under injection). Age
unvisited-rung EWMAs toward a **physics prior** (lower MCS ⇒ higher delivery
probability) so staleness degrades to the safe ordering, not an arbitrary pick.
One EWMA per rung is the entire new state footprint.

### 9.3 Profile table (pre-authored operating points)
Static table shared both ends; the air carries only the `u8 active_profile` index
+ the `table_version` hash (§3.6). Each entry:

```
profile[i] = {
  id, mcs, guard_interval,
  tx_power_level,          // PORTABLE power intent (§10); NOT an absolute value
  airtime_budget_frac,
  arq_deadline_ms[class],  // per §4.1 importance; I-frame class longer
  reserve_bps[stream_type],// guaranteed floor for CONTROL / TELEMETRY
  bitrate_min_kbps,        // policy floor ≥ venc hard floor 1000 (§9.6)
  fec_scheme, fec_overhead_frac,  // §14; scheme=none in the base table
}
```

Video rungs seed MCS **0–5**; rungs **6–7** are normal high rungs the promote path
may reach (the wfb_ng "reserved for probe" tag is dropped — see §9.4).

### 9.4 Promote path (v0 = RSSI-margin; active probe deferred)
wfb_ng promoted only after a boundary probe on a **separate wfb stream**; the
injection model has no such side stream, so that mechanism is **dropped for v0**.

- **v0 — RSSI-margin promote:** promote when `rssi ≥ next_rung_floor +
  rssi_floor_hyst_db` (**6 dB**) AND no RSSI guard active AND flap-freeze clear AND
  `promote_dwell_s` (**0.5 s**) elapsed. Self-contained, no probe.
- **v1 (optional) — active probe:** TX injects a short burst at the next rung, RX
  returns its PER in `probe_per`. Add only if v0 promotes prove too timid.

### 9.5 Transition sequencing (asymmetric)
MCS and bitrate never move together:
- **Demote — bitrate LEADS:** drop encoder bitrate to the lower budget first, wait
  `bitrate_lead_s` (**0.5 s**), then drop MCS. Prevents encoder overshoot into the
  lower-capacity rung.
- **Promote — bitrate LAGS:** raise MCS first, hold bitrate `mcs_up_grace_s`
  (**0.25 s**), then raise it.
- **Settle:** `mcs_settle_s` (**5.0 s**, RE-DERIVE — no-FEC loss is spikier)
  suppresses further loss-loop reaction so a transition isn't read as a fade.

### 9.6 Encoder actuation (venc, same SoC)
Live HTTP, `MUT_LIVE`, sub-ms, no reinit:

```
GET /api/v1/set?video0.bitrate=<kbps>          Host 127.0.0.1:80
GET /api/v1/dual/set?bitrate=<kbps>            # Star6E ch1 only; 501 on Maruko
```

- **Units kbps**, hard range **1000–200000** (venc-enforced; default 8192).
  `bitrate_min` is a policy floor ≥ 1000.
- **Single bitrate authority (deployment rule, not a flag):** venc's API is
  last-writer-wins with no arbitration. waybeam-link MUST be the only writer of
  `video0.bitrate`: if waybeam-hub is present set its `venc.bitrate_enabled=false`
  (that flag lives in hub `mod_venc`, not venc); do not run wfb_ng
  `link_controller` — waybeam-link replaces it.
- **Write only on change (flash wear):** every `/set` persists to
  `/etc/waybeam.json`; push bitrate only when the target actually changes, never
  at the 10 Hz report rate.
- **Low-bitrate coupling (optional):** at floor profiles the SVC-T preset
  oscillates fps; waybeam-link MAY command `video0.resilience=racing` at the low
  end (coarse, hysteretic — `resilience` is heavier than a bitrate tweak).

### 9.7 Flap avoidance (three layers)
- **Soft reentry:** re-promoting into a just-demoted rung within
  `reentry_backoff_s` (**5.0 s**) needs `reentry_dwell_s` (**2.0 s**) instead of
  0.5 s.
- **Hard flap-freeze:** `flap_freeze_count` (**3**) fast re-demotes within
  `flap_freeze_window_s` (**10 s**) pins the rung below for `flap_freeze_s`
  (**10 s**). Flap is *worse* without FEC (each flap = a visible glitch) → this is
  more valuable here, not less.
- **`min==max` pin:** freezes adaptation at a rung (bench / known-bad-link).

### 9.8 Fail-safe on lost feedback
TX runs a `report_epoch` watchdog. No fresh, monotonic-forward epoch within
`report_timeout_ms` (**~500 ms**) ⇒ **fail toward degradation**: hold, then step
toward a safe floor profile. **Never fail optimistic** — a high MCS held on a lost
link is a black screen at range. Under the single-adapter craft (§1), return
starvation is the *normal* high-duty state, so this step-down/promote pair MUST be
**hysteretic + min-dwell damped** and **observable** (§15) to avoid the
floor-oscillation limit cycle (§17 gate 4). The craft additionally runs
**vehicle-only heuristics** from signals it owns (last-heard return RSSI/CFO,
`TxStats.last_was_timeout` = local FIFO backpressure, venc queue fill) — noting
these are *local* and cannot see a remote fade, so they conservatively bias toward
degradation, not toward holding a high rung.

### 9.9 Backpressure coupling (local, TX-side)
The venc output-queue fill is a local TX signal, not an RX report. It **suppresses
reactive-demote (rule 1)** (don't blame RF for encoder overshoot), does **not**
suppress RSSI rules (2,3), and after `pressure_escape_s` climbs to drain the ring
(rule 4).

---

## 10. Per-adapter TX power

Extends §9's operating points with a power dimension so each profile clears its
MCS at the minimum workable power. Grounded in devourer's TX-power API and the
stock Realtek `PHY_REG_PG.txt` power-by-rate format (`docs/groundwork.md §14`).

### 10.1 Model — per-adapter, per-profile, not per-packet
- **Not per-packet.** 8812AU (Jaguar1) and 8812CU/EU (Jaguar3) have no per-packet
  TX power in devourer; power moves only at operating-point cadence via
  `SetTxPowerOffsetQdb()` / `SetTxPowerIndexOverride()` at the MCS-change commit
  (§9.5), never per frame.
- **Per-adapter, NOT fleet-global.** Each physical adapter is a separate devourer
  `IRtlDevice` with its own efuse calibration / antenna / role; power is set on
  each device individually and the correct value differs per adapter. The power
  dimension is indexed by **(adapter × MCS)**. (This matters only where a node has
  multiple TX adapters — i.e. ground; the craft has one.)

### 10.2 Where power lives — portable intent vs. node-local absolutes
- **On-air profile (§9.3)** carries only `tx_power_level` — a portable power
  **intent/index**, hardware-agnostic. **This is the single canonical name;**
  `tx_power_cap`/`tx_power_qdb` from earlier drafts are dead.
- **Each TX node keeps a LOCAL per-adapter power map**, one per physical adapter,
  in the `PHY_REG_PG.txt` row format, holding the **absolute** `qdb` values. The
  controller resolves `(this adapter, profile.mcs, profile.tx_power_level)` → an
  absolute `SetTxPowerOffsetQdb` value and applies it to that adapter's device.

### 10.3 No regulatory clamp — operator responsibility
devourer applies whatever value it is given; `SetTxPowerIndexOverride` is a raw
absolute index and `SetTxPowerOffsetQdb` an uncapped offset. There is **no
regulatory clamp** in the userspace driver. The per-adapter map holds absolute,
operator-authored values that **may intentionally exceed regulatory limits** (a
deliberate operator choice and legal responsibility). `GetTxPowerCaps/State` is a
hardware-range reference, not a safety limit. **Recommend an opt-in per-node
`max_power_qdb` sanity ceiling** in the controller (off by default) so a
mis-authored table cannot silently cook a PA.

### 10.4 Actuation
On profile commit, for each transmitting adapter resolve and apply its power
inside the §9.5 sequenced transition. After **any** devourer retune that resets
TXAGC (a channel change, §11) call **`ReApplyTxPower()`** to re-assert the
adapter's setting. Power is not a fast loop — profile-change cadence only.

---

## 11. Follow-me channel switch (CSA)

A coordinated, announced channel change. **Ground leads** (issues the command);
craft and spectators follow. Lifted from the bench-proven `waybeam_wfb_ng/
vehicle/csa` (JSON `csa_commit`, N=5 decrementing-dt copies resolving to one
absolute switch time), folded into the wire model and hardened.

### 11.1 CSA packet (type `0x5`) — 32 bytes

| off | size | field | notes |
|---|---|---|---|
| 0 | 11 | *common* | §3.1 — sender = the issuing (ground) node |
| 11 | 4 | `csa_nonce` | u32 campaign id; strictly increasing per `(originator, session)` |
| 15 | 1 | `csa_seq` | copy counter N..1 within the campaign |
| 16 | 2 | `target_chan` | center freq MHz (band-agnostic) |
| 18 | 1 | `target_bw` | 0=20, 1=40, 2=80 |
| 19 | 1 | `retune_class` | 0 = fast intra-band, 1 = cross-band |
| 20 | 2 | `dt_to_switch_ms` | **decrements across copies** → one absolute T_switch |
| 22 | 2 | `t_revert_ms` | follower auto-revert budget |
| 24 | 2 | `prev_chan` | revert target |
| 26 | 1 | `prev_bw` | |
| 27 | 1 | `power_intent` | profile power level to `ReApplyTxPower` post-switch |
| 28 | 4 | `csa_mac` | `trunc(HMAC(csa_psk, bytes[0..27]), 4)` — see §11.4 |

`home_chan` is **config-pinned per node** (§15), not carried on the wire — a
static rendezvous channel cannot be redirected by a forged/accepted campaign.

### 11.2 Timing + retune budget
- **N=5 copies @ 20 ms** (100 ms campaign). Each copy's `dt_to_switch_ms`
  decrements so all resolve to the same absolute instant.
- A follower anchors on the **hardware TSF of the copy it received**:
  `target_tsf = rx_tsfl + dt_to_switch_ms·1000` (µs) — TSF, not host clock, so no
  host-jitter smear (re-derive the bench's 0–5 ms precision against TSF).
- `dt_to_switch_ms` (copy 1) ≥ campaign span + max retune for the class + margin:
  **class 0 (fast intra-band, `FastRetune`, ~0.5–2.5 ms) ⇒ 150 ms**; **class 1
  (cross-band, full `SetMonitorChannel`, up to ~277 ms on 8812AU) ⇒ 500 ms**.
- On entering COMMITTED after any retune, every transmitting adapter calls
  **`ReApplyTxPower()`** (§10.4) — `FastRetune` skips TXAGC re-apply.

### 11.3 Adaptive-layer CSA freeze
On **ARMED**, the §9 cascade enters a **freeze** for `csa_settle_s` (**3.0 s**):
no demote/promote, **and the `report_epoch` watchdog (§9.8) is paused** — the
retune blackout + re-acquire silence would otherwise trip a spurious demote and
fail-safe step-down for a healthy switch.

### 11.4 Authentication + anti-replay (the one scoped-auth exception)
CSA is rare and catastrophic (one forged switch blacks out the fleet), so — unlike
the data path — it is authenticated:
- **`csa_mac` = `trunc(HMAC(csa_psk, common_prefix ‖ all CSA fields), 4)`.** The
  MAC covers the **common prefix too**, so a valid body cannot be re-wrapped under
  a fresh `session_id` after a reboot. Forgery without the PSK is dead.
- **Anti-replay:** accept only if `csa_mac` verifies AND `csa_nonce >
  last_applied[(originator,session)]` AND the issuer is the currently-latched
  command source AND `target_chan` ∈ the node's config **channel allowlist** AND
  no CSA accepted within `csa_min_interval_s` (**5 s** rate-limit).
- **`csa_psk` trust boundary = craft + ground only** (§15). Spectators may follow
  CSAs **unauthenticated** — their divergence is self-harm only (they strand
  themselves, not the fleet). A MAC-valid CSA MAY *establish* the craft's session
  binding for the issuer (bootstrap before the craft has cached the ground
  session).

### 11.5 State machine (follower)
```
IDLE ──valid+MAC'd CSA──▶ ARMED ──T_switch──▶ retune + ReApplyTxPower ──▶ VERIFY
        (adaptive freeze on,                                               │
         watchdog paused)                          valid traffic ≤150 ms──┤
        │                                                                 ▼
        │  stale / bad-MAC / replay → drop, stay IDLE            COMMITTED (freeze
        │                                                        lifts after csa_settle_s)
        ▼                                                                 │ no valid
  (never saw CSA)                                                         ▼ traffic
        └────────────────── link lost > rendezvous_timeout ──▶ REVERT → prev_chan
                            (5 s)                              (ReApplyTxPower)
                                    │                                     │
                                    ▼ prev_chan also dead                 │
                            retune HOME_CHAN (config), passive listen ◀───┘
```
- **Short auto-revert:** switched but no valid traffic within `verify_timeout_ms`
  (**150 ms**, bench median 85 ms + margin) → revert to `prev_chan`.
- **Long rendezvous:** never saw the CSA, or lost link `rendezvous_timeout` (5 s)
  after revert → retune to config `home_chan` and listen.

### 11.6 Ground-commits-after-craft-ACK (strand-proof, exploits ground-leads)
Ground leading + the craft→ground downlink being the **strong, diversity-received
direction** lets us make the strand class *never happen* rather than recover after:
- Ground sends the CSA campaign (this is the ARM announcement).
- The craft, on accepting the campaign, sets **`data_flags.CSA_ARMED`** on its
  outgoing video packets (§3.2 bit 4) — an implicit, diversity-carried ACK that it
  will follow the current campaign.
- **Ground commits (switches its own RX) only after it sees `CSA_ARMED`** from the
  craft. If it does not within `csa_ack_timeout` (**1 s**), ground **aborts the
  campaign and stays** on the current channel — no strand.
- **Issuer revert-on-no-video:** if ground did commit (craft ACKed) but then sees
  no craft video on `target_chan` within `verify_timeout_ms`, ground reverts to
  `prev_chan` (an issuer abandoning a failed campaign is not "unasked revert").
- **Intra-process atomic switch (ground fleet):** if the ground *process* accepted
  the CSA (any adapter heard it), it fires all local `FastRetune` calls at
  T_switch — a straggler adapter follows because a sibling heard it.

Because a forged `CSA_ARMED` (unauthenticated DATA flag) could make ground commit
to a switch the real craft won't follow, the **issuer revert-on-no-video is the
backstop** for that case: no craft video on the new channel → ground returns.

---

## 12. Multi-receiver semi-anarchy & ARQ arbitration

Any node may view any stream and NACK it. The TX arbitrates who it *repairs*:

- **First-latcher lock with preferred preemption.** ARQ service is locked to one
  requesting `originator` at a time. A NACK from the config `preferred_originator`
  (the operator's own ground station) **preempts the lock immediately and
  unconditionally** — this fixes the structural bias that the *worst* link NACKs
  first (more loss ⇒ earlier gap declaration), which would otherwise capture ARQ
  for the worst node on the air.
- **Contested-only release (among non-preferred).** A non-preferred holder's lock
  releases only when `(holder silent ≥ release_timeout) AND another originator is
  actively NACKing`. If nobody else wants it, the lock parks (costs nothing —
  silence ⇒ no resends), avoiding clean-stretch thrash. The preferred node ignores
  this rule entirely (it preempts).
- **Per-originator resend budget.** The §5.3 airtime cap is partitioned per
  requesting originator; the preferred node's share is fenced. The lock is a
  **tiebreak within the budget partition**, not a second exclusive mechanism —
  the two do not double-count.
- Resends are **broadcast** (no unicast on this link), so a RETRANSMIT serves every
  receiver that needed it; a losing NACKer quiesces on RETRANSMIT receipt (§6.4).

Multi-host spectator/DVR is thus supported (any node RX + optionally NACK within
its budget), without per-node NAK-suppression signalling — the TX-side budget +
lock is the arbitration.

---

## 13. Security model (no-auth, hardened)

waybeam-link is **unauthenticated on the data path by design** — anyone RF-adjacent
can view, spoof, or inject. That is accepted. The goal of this section is to make
spoofing/injection **unrewarding at near-zero cost**, without smuggling in TDMA,
data-path crypto, or heavy state. Threats and mitigations:

| threat | mitigation | §ref |
|---|---|---|
| Forged far-future `block_id` → video flush | plausible-forward clamp | 6.6 |
| NACK amplification (max-bitmap flood) | global per-seq hold-down + bitmap sanity clamp (reject popcount > one block, or `base_seq` outside the ring) + per-originator budget | 5.3, 12 |
| Discovery-cache flood / mislabel | admission control (N over T) + sustained-traffic type resolution + forward clamp | 2 |
| First-latcher captured by worst/malicious node | `preferred_originator` preemption | 12 |
| Preferred-ID spoof (plaintext) | honest/accepted; damage clamped to that ID's fenced budget + hold-down; spoof extracts no more than a real NACKer | 12 |
| **Forged optimistic LINK_REPORT** (defeats "never fail optimistic") | accept only from latched/preferred `(originator,session)`; plausibility cross-check; conflicting reports ⇒ fail toward degradation | 3.5, 9.8 |
| Replayed control frames (NACK/report) | monotonic wrap-aware discipline on `seq`, `report_epoch` (u32), `csa_nonce` | 3.5, 11.4 |
| **Forged CSA → fleet blackout** (CRITICAL) | **4-byte HMAC on CSA only** + nonce anti-replay + channel allowlist + rate-limit + config-pinned home-channel | 11.4 |

The CSA MAC is the sole cryptographic element and touches only the rare
channel-switch control action, never the bandwidth-carrying data path. Key
distribution (`csa_psk`) is operator-provisioned to craft + ground (§15).

---

## 14. FEC (deferred — bench-gated candidate)

**Decision deliberately deferred.** waybeam-link ships **no FEC** initially
(diversity + ARQ + RTP concealment + short GOP). Whether to add forward parity is
a **binary choice gated on a measurement**, not an assumption:

- **Do NOT use XOR-only (GF(2)) sliding-window FEC.** It recovers one loss per
  window — exactly the isolated-loss case diversity already handles — and fails
  the 5–30 ms correlated burst that is the only reason to want FEC (a burst loses a
  contiguous run of ~8–48 packets). It is the worst of both worlds and is rejected.
- **The real choice is: no-FEC vs. a proper GF(256) sliding-window (RLC/Tetrys)
  codec** sized so `n−k ≥ burst length`. No license-clean embedded C
  implementation exists (OpenFEC's sliding-window support is proprietary;
  swif-codec is research-grade) → a GF(256) RLC would be hand-rolled.
- **Gate on cross-adapter loss correlation ρ (§17 gate 2), measured as a
  windowed P95, not a one-shot mean** — ρ is geometry/attitude-dependent (a
  banking turn is the operative ρ→1 transient). Low ρ ⇒ diversity carries the
  fade, no-FEC stands. ρ→1 in the tail ⇒ diversity collapses toward single-adapter
  loss and ARQ self-throttles exactly when needed ⇒ forward parity justified.

**Two hybrids to include in the bench comparison** (they sit between the poles and
reduce dependence on the fragile craft return path, §7):
- **(a) FEC on ARQ-class (I-frame) blocks only** — parity spent only where loss is
  catastrophic; reuses the §4.1 classifier; shrinks the always-on cost.
- **(b) Tetrys-style reactive coded repair** — on a NACK, send *one* GF(256) coded
  repair covering the block instead of *k* retransmits; ARQ-shaped (no always-on
  parity), each repair fixes any single additional loss, cutting round trips.

**If any FEC is adopted it must be budgeted, not bolted on:** parity is
**live-priority** (only useful in-deadline) — the *recommended* default debits it
from encoder bitrate via `fec_overhead_frac` (§9.3):
`bitrate_budget_eff = capacity(profile) × (1 − fec_overhead_frac − arq_reserve_frac)`,
folded into the §9.5 atomic transition. (A middle priority class — parity above
retransmits, shed first under *local* backpressure but held through *RF* fades —
is an allowed alternative to the permanent bitrate tax.)

**Wire form (if built):** repairs are ordinary DATA packets with
`data_flags.FEC_REPAIR` set and a 6-byte subheader before the payload:
`repair_idx u8 · window_len u8 · window_base_seq u32`. `block_id` = the block
repaired. RX reconstructs when losses within `[window_base_seq, +window_len)` ≤ the
scheme's recoverable count. **Deterministic-latency is a *target to validate*
(≤1–2 frame periods), not an RFC-given guarantee** (§17 gate 4).

---

## 15. I/O bindings, configuration & observability

### 15.1 Binding model
- Pools per node: **≤1 shm** (in XOR out), **≤1 unix socket** (in XOR out),
  **≤4 UDP** (each independently in or out). **v0 = UDP only** (shm/unix are v1).
- Every `stream_id` maps to **exactly one** binding; a binding is in *xor* out,
  never both. Enforced at config load.
- Control packets (NACK/LINK_REPORT/CSA) never touch a binding — the core consumes
  them.

### 15.2 Config (JSON)
```json
{
  "node":  { "originator": 17, "role": "tx", "preferred_originator": 9 },
  "profile_table": "/etc/waybeam-link/profiles.json",
  "adapters": [
    { "name": "wlan0", "bus": "1-1.2", "role": "tx",
      "channel": 5805, "bw": 20,
      "power_map": "/etc/waybeam-link/power.wlan0.txt",
      "max_power_qdb": 2000 },
    { "name": "wlan1", "bus": "1-1.3", "role": "rx", "channel": 5805, "bw": 20 }
  ],
  "streams": [
    { "stream_id": 0, "stream_type": "RTP", "dir": "in",
      "bind": { "kind": "udp", "listen": "127.0.0.1:5600" } },
    { "stream_id": 1, "stream_type": "TELEMETRY", "dir": "in",
      "bind": { "kind": "udp", "listen": "127.0.0.1:14650" } }
  ],
  "policy": {
    "report_hz": 10, "report_timeout_ms": 500,
    "select": { "demote_milli": 20, "rssi_floor_dbm": -85,
                "rssi_fade_db_per_s": 10, "rssi_fade_arm_dbm": -65,
                "promote_rssi_hyst_db": 6, "promote_dwell_s": 0.5,
                "mcs_settle_s": 5.0, "down_cooldown_s": 0.2,
                "ewma_alpha": 0.3 },
    "arq":    { "airtime_frac": 0.15, "attempt_cap": 3, "holddown_ms": 20,
                "fwd_clamp_blocks": 4 },
    "fec":    { "scheme": "none", "overhead_frac": 0.0 },
    "return": { "guard_us": 300, "return_window_us": 2000 },
    "csa":    { "psk": "<operator-provisioned; craft+ground only>",
                "settle_s": 3.0, "verify_timeout_ms": 150,
                "min_interval_s": 5, "ack_timeout_ms": 1000,
                "rendezvous_timeout_s": 5, "home_chan": 5745,
                "channel_allowlist": [5745, 5805, 5825] }
  },
  "stats": { "hz": 1, "bind": { "kind": "udp", "send": "127.0.0.1:9110" } }
}
```
- RX nodes use `"dir":"out"` streams (UDP `send` targets) and `role:"rx"` adapters
  (diversity = same `channel`; a scout may sit on a different channel on another
  adapter).
- Every policy constant is overridable → bench re-derivation (§9, §17) is config,
  not recompile.
- `csa.psk` is present only on craft + ground configs; it MUST be excluded from
  stats and logs.

### 15.3 Streaming stats (newline-delimited JSON)
Emitted at `stats.hz` to stdout and/or the stats binding. Fields map 1:1 to the
§16.2 counters plus the state an operator needs to *see* a demote, a flap-freeze, a
table mismatch, phantom diversity, a stalled adapter, or a failing return path:
```json
{ "t_ms": 172834, "node": 17, "session": 2748291,
  "adapters": [ { "name": "wlan0", "rx": 10234, "dup": 812,
    "rssi_best": -58, "rssi_mean": -63, "snr": 22, "noise": -85,
    "tx_submitted": 540, "tx_failed": 2, "tx_timeout": 0,
    "adapter_stalled": false } ],
  "streams": [ { "stream_id": 0, "type": "RTP",
    "seq": 90233, "delivered": 89901,
    "loss_prediversity_milli": 41, "loss_postdiv_prearq_milli": 6,
    "recovered_arq": 220, "recovered_fec": 0,
    "dropped_superseded": 110, "dropped_deadline": 8,
    "nacks_sent": 18, "resends_sent": 230, "double_send_suppressed": 5,
    "decode_errors": 0, "active_profile": 4, "table_version": 178 } ],
  "return": { "reports_expected": 10, "reports_received": 9,
    "return_window_hits": 7, "return_window_misses": 2 },
  "link": { "target_originator": 9, "target_session": 183726,
    "profile": 4, "mcs": 4, "tx_power_qdb": 1800,
    "report_epoch": 1822, "report_age_ms": 40,
    "state": "HOLD", "flap_freeze": false, "csa_state": "IDLE" } }
```
`return_window_hits/misses` (TX-side) and `reports_expected/received` expose the
§7.2 optimisation's health directly, and `adapter_stalled` + the
`loss_prediversity` vs `loss_postdiv_prearq` pair expose phantom diversity and the
ρ decorrelation gauge — the two field-failure modes the design most fears.

---

## 16. All-in-one binary, modes & bench verification

A single portable binary vendors devourer and adds waybeam-link.

### 16.1 Modes
- `tx` — bind ingress (RTP in) → framer → inject via devourer.
- `rx` — open N adapters in monitor → merge/dedup/gap/NACK → egress (RTP out); one
  adapter is the designated uplink TX.
- `loopback` — TX and RX in one process for bench verification (below).

### 16.2 Bench verification (no flying)
- **Source:** `gst videotestsrc → H.264/H.265 → RTP → UDP` into `tx`; moving test
  pattern with a visible frame counter.
- **Sink:** `rx` egress → UDP → gst → display, plus a decode-error overlay.
- **Synthetic loss injector** at RX ingest: uniform `p`; **Gilbert-Elliott burst**
  (the ARQ target regime); **per-adapter independent** (exercises diversity);
  **correlated across adapters** (confirms graceful ARQ degradation).
- **Counters (per run):** injected, received/adapter, deduped, recovered-by-NACK,
  recovered-by-FEC, dropped-superseded, dropped-past-deadline, NACKs sent, resends
  sent, double-send-suppressed, decode errors (mirror §15.3).
- **Passing assertions:** independent per-adapter loss ⇒ near-zero decode errors
  from diversity alone, few/no NACKs; moderate burst on ARQ blocks ⇒
  recovered-by-NACK > 0, decode errors ≈ 0, resend airtime under cap; non-ARQ loss
  ⇒ zero NACKs for those seqs; correlated-all-adapter fade ⇒ ARQ bounded, no
  self-congestion collapse, recovers after the fade.

`loopback` mode has **no hardware TSF** (§9.2 host clock only) and cannot validate
the §7.2 quiet-gap or §11 TSF anchoring — those need real radios (§17).

---

## 17. Empirical knobs & bench gates

**Knobs to measure, not design** (all overridable via §15 config):

| knob | meaning | how measured |
|---|---|---|
| `demote_milli` | reactive-demote on delivered loss | raise until decode errors clear at target range |
| dwell ceiling | §6.2-3 backstop | cross-adapter delivery-jitter histogram |
| retransmit airtime frac | resend cap vs downlink | raise until live-video jitter appears |
| deadline budget (per class) | glass-to-glass minus pipeline | measured pipeline delay |
| `guard_us` / `return_window_us` | §7.2 quiet gap | craft TX→RX settle + ground turnaround + return airtime |
| EWMA α, `mcs_settle_s` | §9 smoothing/settle | no-FEC loss spikiness |

**Bench gates (must pass before the dependent design is trusted):**

1. **One injecting `IRtlDevice` + N monitoring siblings in one process**
   (per-adapter `libusb_context` + thread). *Multi-adapter RX is already proven at
   N=3 (RX-only) in Waybeam-android `:wifi`; the residual unknown is the injector +
   monitors mix* — ground's designated-NACK-TX among RX siblings, and the craft's
   single-adapter TX+RX-return (devourer Jaguar3 `enable_with_tx` must be set
   before `InitWrite`; measure the actual TX→RX settle time; 8812EU RX+TX is
   field-proven on wfb_ng at 20 MHz — its 40 MHz bug is out of scope). *Hardware-
   required.*
2. **Cross-adapter loss correlation ρ** (windowed **P95**, estimator defined over
   `diversity`/`adapters`) — decides no-FEC vs GF(256) RLC (§14). *Real-RF geometry
   required.*
3. **NACK→RETRANSMIT round-trip P90** vs the I-frame deadline on the saturated
   uplink — decides whether ARQ is ever in-deadline. *Hardware-required.*
4. **Return-window fit + adaptive-loop stability** — at target fps/bitrate, does
   the §7.2 quiet gap beat pure-opportunistic return, and does the §9.8 damped
   step-down/promote pair hold a stable operating point rather than oscillate at
   the floor? **This gate governs the single-adapter craft return path (§1, §7).**
   *Real-RF, saturating injector, TSF-capable RX required.*

---

## 18. Out of scope / accepted limitations

- No data-path authentication (spoof/injection accepted; hardened §13, not
  prevented). The CSA MAC (§11.4) is the sole exception.
- No TDMA; no per-packet frequency hopping; no time sync (TSF is the only shared
  anchor); no IP/ARP/AP/STA semantics on the wire.
- The craft has one radio — its return-reception is best-effort by physics (§7).
- No importance beyond the single `ARQ` bit (I-vs-P granularity).
- Correlated fades that beat diversity also beat ARQ (both ride the same faded
  channel) — the win is the short-fade middle band, not the SNR edge.
- FEC is unresolved by design (§14), pending the ρ measurement.

---

## 19. Build order

1. Wire header encode/decode + session model + big-endian codec (§2–3), including
   `table_version` hashing and the plausible-forward clamp.
2. I/O binding layer + JSON config + JSON stats (§15), UDP-only.
3. TX framer with RTP boundary detection + resend ring (§5), classifier stubbed to
   size-heuristic first.
4. RX merge/dedup/gap-detector with both short-circuits + liveness watchdog (§6),
   NACK generation.
5. Air-side resend scheduler with priority/caps/hold-down + first-latcher lock +
   per-originator budget (§5.3, §12).
6. `loopback` mode + synthetic-loss injector + counters (§16); extract the
   loopback-measurable knobs.
7. NAL-type classifier for the RTP profile (§4.1).
8. Adaptive selector: metric reporter (RX) + decision cascade + venc actuation +
   flap/fail-safe (§9); per-adapter TX power (§10).
9. Return-telemetry TSF quiet-gap optimisation (§7.2) — gated on §17 gate 1/4
   hardware.
10. Follow-me CSA (§11) — gated on gate 1.
11. Field bring-up; run bench gates 1–4 (§17). FEC (§14) only if gate 2 says so.
