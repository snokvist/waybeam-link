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
  boundary). devourer owns radiotap; the 802.11 MAC header is pinned in §3.0;
  waybeam-link owns everything below.
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

### 3.0 On-air 802.11 encapsulation (the devourer boundary)

Every waybeam-link packet rides as the frame body of a **pinned 802.11 data
frame** injected through devourer (radiotap prefix rate-less `TX_FLAGS`-only;
modulation comes from the adapter's committed `SetTxMode`, §9.5/§10.4). The
MAC header is normative — all implementations must emit and filter exactly
this shape (Pass-7 ruling):

| field | value |
|---|---|
| Frame Control | `0x08 0x00` — Data (not QoS), ToDS=0, FromDS=0, no flags |
| Duration | `0` |
| addr1 (DA) | `ff:ff:ff:ff:ff:ff` — broadcast, so the frame can never solicit an 802.11 MAC ACK (§1 no-MAC-ARQ invariant) |
| addr2 (SA) | `56:42:NN:OO:OO:AA` — locally-administered **unicast**; `NN` = `net_id` u8, `OO:OO` = `originator` u16 BE (§2), `AA` = sender adapter index (diagnostics only) |
| addr3 (BSSID) | `56:42:4c:4b:00:00` — fixed `"VBLK"` tag |
| Sequence Control | injector-incremented per frame (fragment 0) |

The SA first octet is `0x56`, not the payload magic's `0x57`: `0x57` has the
I/G bit set, making it a **group address** — nonconforming as a transmitter
address and structurally unable to ever solicit an ACK. `0x56` keeps the
locally-administered bit and clears I/G, so every waybeam-link SA is a valid
unicast TA (Pass-8 ruling; keeps the hardware ACK-responder door open for the
uplink, §7.2 note). The payload `magic` (§3.1) remains `0x57 0x42`.

The frame body that follows is the raw waybeam-link packet (§3.1 onward) — no
LLC/SNAP, no encryption. The FCS is the radio's. Monitor-mode RX delivers the
MPDU **with the 4-byte FCS appended** (devourer's monitor bring-up keeps
`APP_FCS`; the chip has already validated it — CRC-error frames are dropped
at the driver boundary): receivers strip the trailer before the length-exact
§3.1 parse. Bench-verified on 8812CU (step 11).

**Backend-agnostic frame; two rate-carrying mechanisms (Pass 13).** The 802.11
frame, SA filter, and FCS handling above are identical across air backends —
only *how the PHY rate is selected* differs. The **devourer** backend uses the
rate-less `TX_FLAGS`-only radiotap prefix and commits the rate out-of-band via
`SetTxMode` (§9.5/§10.4). The **kernel-monitor** backend (`air.kind
"kernel-monitor"`: AF_PACKET raw injection through the Linux driver in monitor
mode, no devourer) has no such out-of-band control, so it carries the rate in a
**per-packet radiotap MCS field** (13-byte HT radiotap: `TX_FLAGS | MCS`,
`known = BW|MCS|GI|FEC|STBC`). RX radiotap is parsed for `DBM_ANTSIGNAL` (RSSI)
and `TSFT` (the §7.2 per-adapter TSF); the monitor netdev exposes no
per-adapter TSF *read*, so the craft/ground fall back to host time (§7.2). The
on-air bytes a receiver sees are unchanged — either backend interoperates with
either on the RX side.

**RX filter** (in priority order, cheapest first): type/subtype == Data &&
`SA[0..1] == 56:42` (&& `SA[2] == net_id` **when the node configures one**;
an unconfigured node accepts any `net_id`) && payload `magic` + full header
validation (§3.1). `net_id` is node-local config, stamped by every TX
(default `0`); it exists so co-located waybeam-link systems can partition
their RX paths at L2 — it is **not** access control (§13 applies).

**Hardware-ACKed unicast return (Pass 12; the §7.2 hybrid's wire shape —
bench knob, both halves default off).** When the ground enables
`return.unicast` and the craft arms `air.ack_responder`, ground→craft
returns (**NACK / LINK_REPORT only** — CSA campaign copies stay broadcast,
§11) are injected as **QoS-Data** instead of the pinned broadcast frame:

| field | value |
|---|---|
| Frame Control | `0x88 0x00` — QoS-Data, ToDS=0 FromDS=0 |
| addr1 (RA) | the target craft's §3.0 SA **as last heard** (latched per originator from accepted frames — exact match with the MACID the craft's ACK responder armed, adapter-idx byte included) |
| addr2 (SA) / addr3 | own §3.0 SA / `"VBLK"` BSSID, unchanged |
| QoS Control | `0x00 0x00` — TID 0, Normal ACK policy |
| radiotap | the same rate-less prefix with `TX_FLAGS = 0` (the frame *expects* an ACK) |

The injecting chip hardware-retransmits an unACKed unicast frame (devourer
descriptor retry limit 12, Jaguar1 and Jaguar3 alike) — SIFS-timed hardware
ARQ on the return path for zero extra return-path bytes. Pinned
consequences:

- **Receivers always accept both shapes.** The RX filter additionally
  accepts QoS-Data whose FC1 is clear **except the Retry bit** (hardware
  retransmissions set it); the frame body then starts at offset 26 (after
  the QoS Control field). The knobs gate only what a node *sends/arms*, so
  the A/B halves deploy independently.
- A lost ACK can deliver the same return twice; NACK/LINK_REPORT handling
  is already idempotent (§5.3 per-seq hold-down, §9.1 monotonic
  `report_epoch`).
- No latched SA for the target yet → that return falls back to broadcast
  (counted, §15.3 `unicast_fallback`).
- Arming the responder turns a passive monitor into an ACK transmitter —
  acceptable on the *craft* (it transmits anyway); ground diversity
  adapters never arm.
- Downlink DATA stays broadcast unconditionally (Pass 8 rejected hardware
  ARQ for the video path; the §1 no-MAC-ARQ invariant stands there).

### 3.1 Common prefix (all packet types) — 11 bytes

| off | size | field | notes |
|---|---|---|---|
| 0 | 2 | `magic` | `0x5742` (bytes `57 42`) — protocol guard |
| 2 | 1 | `ver_type` | hi nibble = version (`0x0`), lo nibble = packet type |
| 3 | 2 | `originator` | sender node id (§2) |
| 5 | 2 | `destination` | advisory; `0x0000` = broadcast |
| 7 | 4 | `session_id` | sender boot nonce |

**Packet types** (low nibble): `0x1 DATA · 0x2 NACK · 0x3 LINK_REPORT ·
0x4 HEARTBEAT · 0x5 CSA · 0x6 RECOVERY_REQUEST`. 6 of 16 used; the version nibble will not ship 16
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
| 3 | `FEC_REPAIR` | packet is a FEC repair symbol (§14; an 11-byte subheader precedes payload) |
| 4 | `CSA_ARMED` | **craft→ground ARM ack** — craft has accepted the in-flight CSA campaign and will follow (§11.6) |
| 5–7 | reserved | 0 |

**Redundant per-packet metadata (critical rule):** `stream_type`, `block_id`,
`END_OF_BLOCK` membership, the `ARQ` flag, `active_profile`, and `table_version`
are stamped on **every** packet of a block, not just the first. A surviving
packet of a block reveals the block's boundary, ARQ-eligibility, and the TX's
operating point/table even if the first packet was lost.

**Header overhead:** 26 B header. The **usable MPDU is profile-driven**
(§9.3 `max_payload`), not fixed: standard rungs seed ~1450 B (⇒ **1424 B max
payload**, ~1.8%); Realtek jumbo/A-MSDU rungs reach ~3993 B (⇒ ~3967 B
payload). The wire `payload_len` (u16) is self-describing, so an RX decodes
whatever budget the TX used — including across a mid-stream profile change.
`kMaxDataPayload` is the **absolute ceiling** (4096) for buffer sizing only;
the effective per-frame budget is `active_profile.max_payload`. Adaptive MTU
is essential for large IDR frames on the SHM path (§5.1a/§14): a 512 KB frame
is k≈370 source symbols at 1400 B but only k≈132 at 3967 B.

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

**Concrete definition (normative, operator-ruled 2026-07-10):** the hash is
**CRC-8/DVB-S2** — polynomial `0xD5`, init `0x00`, no reflection, no final XOR
(the same CRC-8 the ecosystem's CRSF stack uses) — computed over the following
**canonical binary serialization** of the profile table:

- `count` u8 — number of profiles;
- for each profile, **sorted ascending by `id`** (duplicate `id`s are a config
  error), the fields in this exact order, big-endian, 25 bytes per profile:
  `id` u8 · `mcs` u8 · `gi` u8 (0=long, 1=short) · `tx_power_level` u8 ·
  `airtime_budget_permille` u16 · `fec_scheme` u8 (0=none, 1=rlc256,
  2=rlc256_iframe, 3=tetrys_reactive) · `fec_overhead_permille` u16 ·
  `arq_deadline_iframe_ms` u16 · `arq_deadline_pframe_ms` u16 ·
  `bitrate_min_kbps` u32 · `reserve_control_bps` u32 · `reserve_telemetry_bps` u32;
- `floor_profile` u8.

Fractional JSON fields (`airtime_budget_frac`, `fec_overhead_frac`) are scaled to
integer per-mille with `llround(frac × 1000)` before hashing. The hash is thus
invariant to JSON formatting, key order, and comments, and changes on any semantic
change to any profile field.

### 3.7 The `loss_postdiv_prearq` semantics (do not confuse with wfb_ng)

This field is **post-diversity, pre-ARQ delivered loss** — the loss remaining
after the merged RX state machine has combined all adapters, before ARQ repairs.
It is **not** wfb_ng's `rx_ant` "pre-diversity" loss. The distinction is
load-bearing: waybeam-link has **no FEC underneath**, so this number is close to
true delivered video loss, and the adaptive demote threshold (§9.1) is tuned
against it accordingly (~20‰, not wfb_ng's pre-FEC 80‰). The stats output (§15)
additionally exposes raw `loss_prediversity` for ρ analysis; the two must never be
conflated in code.

**Pre-diversity estimator (operator-approved implementation pass 2026-07-12):**
after a stream latches, RX maintains one sequence-opportunity tracker per
adapter over original DATA only (`RETRANSMIT=0`). A forward sequence advance by
`d` adds `d` expected opportunities and one received opportunity; bounded
out-of-order arrivals fill previously missing opportunities exactly once.
Duplicates and retransmits add neither expected nor received opportunities.
`loss_prediversity_milli = 1000 * sum(expected-received) / sum(expected)` across
the stream's adapters. Trackers begin at each adapter's first post-latch packet,
so late adapter startup is not counted as loss. The missing set is bounded by
the existing plausible-forward clamp. Stats reset zeros estimator totals while
preserving each adapter's current sequence anchor.

### 3.8 HEARTBEAT packet (type `0x4`) — 11 bytes

The common prefix (§3.1) alone; there is no body (operator-ruled 2026-07-10). A
presence/keepalive frame: it refreshes the sender's `(originator, session_id)`
liveness against the §2 idle teardown and gives quiet nodes (e.g. a ground node
between NACKs, or a node waiting at a rendezvous channel, §11.5) something to be
discovered by. It carries no stream fields — HEARTBEAT never creates or refreshes
*per-stream* RX state. Exactly 11 bytes; any other length is a decode error.

**Emission cadence (operator-ruled 2026-07-12):** every node emits HEARTBEAT at
**1 Hz while otherwise quiet**. Any successfully submitted DATA, NACK,
LINK_REPORT, CSA, or HEARTBEAT resets the one-second quiet interval, so active
traffic suppresses redundant keepalives. HEARTBEAT uses the node's current
`originator` and per-boot `session_id`, with broadcast destination `0`.

### 3.9 RECOVERY_REQUEST packet (type `0x6`) — 18 bytes

| off | size | field | notes |
|---|---:|---|---|
| 0 | 11 | *common* | sender = RX node requesting decoder bootstrap |
| 11 | 2 | `target_originator` | TX node whose encoder owns the stream |
| 13 | 4 | `target_session` | exact current TX boot/session nonce |
| 17 | 1 | `target_stream_id` | RTP stream requiring a random-access picture |

An RX emits this return when a local decoder is newly attached or reset while
the encoded stream remains live. A GDR stream can carry VPS/SPS/PPS indefinitely
without an IRAP picture, so parameter sets alone do not guarantee that a fresh
decoder can display. The matching TX requests one IDR/CRA from its encoder and
otherwise leaves the steady-state GDR policy unchanged.

The TX accepts a request only when `target_originator` and `target_session`
match itself and `target_stream_id` names a configured RTP ingress. Requests
are rate-limited to one encoder actuation per second; duplicates and forged
floods inside that window are harmless. The packet is best-effort and may be
repeated by the local controller after one second if decoder output has not
resumed. It uses the same designated return adapter and quiet-gap scheduling as
NACK and LINK_REPORT. This is recovery signalling, not a periodic-IDR policy.

---

## 4. Block model and profiles

A **block** = packets sharing a `block_id`, delimited by `END_OF_BLOCK`. For the
RTP profile, one block = one RTP frame (marker/timestamp boundary). The active
profile is selected on the wire by `stream_type`; both ends apply the matching
policy with no out-of-band agreement.

**Frame-SHM ingress (§5.1a, §15.4):** when a stream is fed by a `frame-shm`
binding, the block boundary is **direct, not inferred** — waybeam-link ingests
one whole encoded frame per SHM slot, so one frame = one `block_id` by
construction (no marker-bit / timestamp inference). The block model itself is
unchanged (a block is still packets sharing `block_id`, delimited by
`END_OF_BLOCK`); only the boundary *source* differs.

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

**Frame-SHM direct classifier:** on a `frame-shm` stream the encoder has already
classified the frame — the SHM slot's `VencFrameMeta.flags` bit 0 marks IDR
(§15.4). `FrameFramer` sets the block `ARQ` from that flag directly; **no NAL
parsing on the link side**. This is the authoritative form of the §4.1
classifier (the encoder's own IDR decision), not an approximation of it.

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

**No fragmentation (invariant, UDP/RTP ingress).** On a UDP/RTP-ingested stream
each ingress datagram MUST fit one MPDU payload. Configure the encoder's RTP
payloader `mtu` at or under the active profile's `max_payload` budget (§9.3;
standard rungs ~1400, jumbo rungs up to ~3960). H.264/H.265 payloaders already
fragment NALs to MTU — this is a config assertion, not new code. A runtime
datagram larger than the payload budget is **dropped with a stat**
(`oversize_ingress`), never silently truncated.

**The invariant is relaxed for `frame-shm` ingress** — there the ingress unit is
a whole frame (up to 512 KB) and `FrameFramer` (§5.1a) *is* the fragmenter.

### 5.1a FrameFramer (per `frame-shm` stream)
Replaces §5.1's Framer when the ingress binding kind is `frame-shm` (§15.4).
The ingress unit is one whole encoded frame carrying an 8-byte `VencFrameMeta`
prefix (§15.4); the `[VencFrameMeta][Annex-B]` blob is treated as an **opaque
payload** — FrameFramer parses only the metadata prefix, never the NAL bytes.

1. Read one frame blob from the SHM binding. Assign it a fresh `block_id`
   (one frame = one block, §4).
2. Set `ARQ` from `VencFrameMeta.flags` bit 0 (IDR ⇒ 1), §4.1.
3. **Fragment** the blob into `k` **source symbols** of size
   `s = active_profile.max_payload − 26 − 11` (header + §14 repair subheader,
   so source and repair symbols are interchangeable for coding); the last
   symbol carries the tail (`< s`) and is zero-padded to `s` only for the FEC
   computation (§14), never on the wire. `k = ceil(blob_len / s)`. `s` is fixed
   for the life of the block (a frame is fragmented atomically under one
   profile); a later frame may use a different `s` after a profile change —
   the wire is self-describing (§3.2).
4. Emit the `k` source symbols as DATA packets in order (`FEC_REPAIR` unset),
   `seq` monotonic across the whole block, `END_OF_BLOCK` on the last **source**
   symbol. Each source DATA payload is a **4-byte source subheader**
   (`window_len u16 = k`, `sym_index u16 = i`) followed by the chunk. The
   subheader makes every source symbol self-describing: RX reassembly (§6.3a)
   knows each symbol's index and the block's `k` without inferring them from
   `seq` gaps — so a stream with no FEC (ARQ-only) can never mistake a
   leading-loss run for a complete frame.
5. Per the §14 adaptive policy, generate and emit `r` **repair symbols**
   (`FEC_REPAIR` set, 11-byte subheader §14) after the source symbols, same
   `block_id`.
6. Push every emitted symbol into the resend ring (§5.2) as normal.

Both subheaders are deducted from the rung `max_payload` when sizing the coded
symbol: `s = max_payload − 26 − 11` (the 11-byte repair subheader binds; the
4-byte source subheader is smaller, so a source packet `26 + 4 + chunk ≤
max_payload`). Redundant per-packet metadata (§3.2) is stamped on every symbol,
so a surviving symbol reveals the block's boundary, ARQ-eligibility, `k`, index,
and operating point.

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

### 6.3a Frame reassembly + SHM egress (`frame-shm` out streams)
When the egress binding is `frame-shm` (§15.4), the RX node reassembles whole
frames instead of forwarding per-packet payloads:

1. `FrameReassembler` collects a block's source + repair symbols by `block_id`.
2. **All `k` source symbols present** (fast path): concatenate their payloads in
   `seq` order → the `[VencFrameMeta][Annex-B]` blob → write one SHM slot. No
   FEC decode.
3. **Sources missing but ≥ `k` total** (source + repair): GF(256) decode (§14.1)
   recovers the missing source symbols, trim to `frame_len`, then egress.
4. **< `k` after deadline / on supersession (§6.2):** frame lost, **nothing is
   egressed** — a partial frame is useless to the decoder (no partial slots).
5. The egressed slot is **byte-identical** to the producer's original slot
   (§15.4) — the metadata prefix rides through transparently; the RX never
   parses the Annex-B payload.

There is no completed-frame reorder buffer. On observing any packet from block
`N`, every incomplete block older than `N` is finalized as superseded. Once
block `N` is released, older blocks can therefore neither delay it nor appear
after it; any late symbol for an older finalized block is ignored. This is the
frame-SHM form of §6.2's latency-first rule and the retention window is zero
blocks, not a jitter-buffer allowance.

This is §5.3 Option A. (Option B — RTP re-packetization for a decoder that cannot
consume SHM — is out of scope for v1; a `udp` egress on a `frame-shm`-ingested
stream is rejected at config load, since the wire payloads are frame *fragments*,
not RTP packets.)

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
RX rejects any DATA `seq`/`block_id` or NACK `base_seq` that jumps implausibly
far ahead. Real monotonic traffic never jumps by millions; this single check
neutralises forged far-future `block_id` video-flush, garbage NACK bitmaps, and
discovery cursor poisoning. Two clamps with distinct references (amended after
the step-4 build surfaced a deep-fade death spiral in the original single-cursor
wording):

- **seq clamp** — `seq` must be within `fwd_clamp_pkts` (seed 256) ahead of the
  delivery **cursor**.
- **block clamp** — `block_id` must be within `fwd_clamp_blocks` (seed 4) ahead
  of **`max_block`, the newest legitimately heard block — NOT the last
  *delivered* block.** In a deep fade the cursor advances by deadline-skips
  without delivering anything; a delivered-block reference freezes during the
  fade and then clamp-rejects the entire recovering stream forever. Ratcheting
  `max_block` costs an attacker one accepted in-clamp packet per `+K` step —
  that bounded creep is the accepted residual of this defence.

**Sustained-clamp resync (escape hatch).** If *every* packet of a latched
stream clamp-rejects continuously for `clamp_resync_ms` (seed 500 ms), the
stream is desynced by a real outage (the TX ran further ahead than the clamp
allows), not under attack: adopt the next packet as a fresh floor with §2
startup-floor semantics — drop all held packets, gap state, block state, and
per-adapter cursors; count it as `resyncs` in the §15 stats. Any packet
accepted inside the window resets it. Security posture: without the clamp, one
forged packet flushes video; with clamp + resync, a forger must sustain an
unbroken flood that out-crowds *all* legitimate traffic for the full window —
at which point it is a jammer, which no sequence-number defence survives
anyway.

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

**Hardware-ACK hybrid (Pass 12; gate-4 A/B slot from Pass 8):** the return
path — §7.1 opportunistic or §7.2 paced alike — can be switched to
hardware-ACKed unicast: the craft arms its chip's ACK responder
(`air.ack_responder`), the ground sends returns as unicast QoS-Data
(`return.unicast`) and gets SIFS-timed hardware retries on them. Wire shape
and consequences are pinned in §3.0. Both knobs default off; the A/B
against plain broadcast returns is a §17 bench slot.

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
  max_payload,             // u16 air MTU budget (§3.2); DATA payload ceiling on
                           //   this rung. Standard rungs ~1424; Realtek jumbo/
                           //   A-MSDU rungs up to ~3967. Drives FrameFramer's
                           //   source-symbol size s (§5.1a). Absent ⇒ 1424.
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
- **`next_rung_floor` provenance (Pass-6 ruling):** per-rung RSSI floors are
  **node-local policy**, NOT part of the hashed §9.3 wire table — they encode
  this receiver's antenna/LNA reality, and adding them to the table would break
  `table_version` for what is local tuning. Config:
  `policy.select.rung_rssi_floor_dbm`, one dBm value per rung index, seeds
  `[-88, -85, -83, -80, -77, -73, -71, -70]` (typical HT20 RX sensitivity +
  margin; §17-overridable, bench re-derivable per rung).
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
- **The "budget" (Pass-6 ruling — derived, not configured):** each rung's
  bitrate target is computed, all-integer, from the profile itself:
  `HT20 PHY rate(mcs, gi) × airtime_budget_permille/1000 ×
  (1000 − fec_overhead_permille)/1000 − reserve_control_bps −
  reserve_telemetry_bps`, floored at `bitrate_min_kbps`. PHY rates (kbps,
  20 MHz, long GI): `{6500, 13000, 19500, 26000, 39000, 52000, 58500, 65000}`
  for MCS0–7; short GI = ×10/9. No separate per-rung bitrate field exists —
  the table's airtime fraction IS the bitrate policy.

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
- **`min==max` pin:** freezes adaptation at the pinned rung (bench /
  known-bad-link). A runtime re-pin (§15.5 `POST /api/v1/link/profile`) **snaps**
  the operating point to that rung immediately, in either direction — it is a
  select-and-hold, not a freeze-in-place. (Config-time pins already land there via
  the boot clamp; the runtime path clamps in `evaluate()` on the next tick.)

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

### 9.10 TX-wedge watchdog (CCX-report liveness)

The §6.5 watchdog is RX-side; this is its TX-side sibling, run by any node
whose TX adapter is a radio (§3.0). The failure mode is real and observed:
the RTL88x2 USB TX wedge — bulk-OUT keeps accepting frames (`tx_submitted`
advances) while nothing airs, and only a physical re-plug recovers the chip.

**Trigger — report absence, never report deficit (Pass 11).** The per-frame
CCX TX-status reports (Pass 8) are lossy under load *by design*: the step-11
bench measured healthy report return rates of 100% at ≤500 pps falling to
~25% at 4500 pps, so any deficit threshold misfires exactly when the link is
busiest. A healthy chip returned *some* reports at every measured load. The
detector therefore evaluates one verdict per `wedge_window_ms` (seed 1000)
from the `(tx_submitted, tx_reports)` counter deltas:

- `Δtx_reports > 0` → not wedged (any report proves the TX path alive);
- `Δtx_reports == 0` and `Δtx_submitted >= wedge_min_submits` (seed 8) →
  **wedged**;
- too few submissions to judge → hold the previous verdict (an idle TX is
  not evidence either way).

**Action (v1) — observability only.** The verdict is surfaced per adapter as
`tx_wedged` (§15.3) and logged on every transition; it deliberately does NOT
actuate §9. A craft TX wedge stops video and returns together, so the §9.8
`report_epoch` watchdog already fails the selector toward the floor, and
recovery requires a physical re-plug regardless. Coupling the detector into
adaptation (or an automatic USB reset) is deferred until the detector itself
passes bench validation: silent across a healthy 500–4500 pps sweep, fires
within one window of an induced wedge (§17 knob table).

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
- **Level→absolute law (Pass-6 ruling):** the authored per-MCS curve **IS
  level 4** (the baseline intent). The controller computes
  `absolute_qdb = curve[mcs] + (tx_power_level − 4) × 8 qdb` (one level step =
  2 dB), then applies the §10.3 `max_power_qdb` ceiling if configured. One
  curve row to author per adapter; levels are a portable, monotonic offset
  around it — no per-level tables.

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
- **Primitive (normative, operator-ruled 2026-07-11):** `HMAC` is
  **HMAC-SHA-256** (RFC 2104 over FIPS 180-4 SHA-256). The MAC input is
  **bytes 0..27 of the encoded CSA packet** (everything before the `csa_mac`
  field, §11.1). `trunc(·, 4)` = the **leftmost 4 bytes** of the 32-byte tag,
  read **big-endian** into the `csa_mac` u32. `csa_psk` is the **raw bytes of
  the config string** (any length; RFC 2104 keying handles it — no derivation
  step).
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

## 14. FEC (frame-aligned GF(256) RLC — built, default-off, rate bench-gated)

**Mechanism built, enablement bench-gated.** The GF(256) frame-aligned RLC codec
and its wire form are pinned and implemented for the `frame-shm` path (§14.1,
§5.1a); it defaults **off** (`fec.scheme="none"`) and the base profile table
ships `fec_scheme=none`. Whether to *enable* forward parity, and at what rate,
stays a **choice gated on a measurement**, not an assumption. The design
rationale that selected GF(256) RLC over the alternatives:

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

**Wire form:** repairs are ordinary DATA packets with `data_flags.FEC_REPAIR`
set and an **11-byte subheader** before the coded payload:

| off | size | field | notes |
|---|---|---|---|
| 0 | 1 | `repair_idx` | u8; index of this repair symbol within the block |
| 1 | 2 | `window_len` | u16; `k` = number of source symbols in the block |
| 3 | 4 | `window_base_seq` | u32; `seq` of the block's first source symbol |
| 7 | 4 | `frame_len` | u32; total source-blob length (bytes) — lets RX strip the last symbol's zero-padding after a decode |

`block_id` = the block repaired. `window_len` is **u16** (widened from the
earlier u8 sketch): a 512 KB frame at a 1400 B rung is k≈370 > 255. `frame_len`
is required because a last source symbol recovered via FEC arrives full-size
(`s` bytes) with no length marker — RX trims `k·s − frame_len` padding bytes.
RX reconstructs when total received symbols (source + repair) for the block ≥
`k`. **Deterministic-latency is a *target to validate* (≤1–2 frame periods),
not an RFC-given guarantee** (§17 gate 4).

### 14.1 Frame-aligned GF(256) RLC (the built scheme, `fec.scheme="rlc256"`)
The scheme selected for the `frame-shm` path (§5.1a). The **codec and wire form
are pinned here and implemented**; whether to *enable* it and at what **rate**
stays bench-gated on the §17 gate-2 ρ measurement (still pending) — the rate is
config (`fec.i_rate_permille` / `fec.p_rate_permille`), not a recompile.

- **Per-frame block coding** (not sliding-window): each frame is an independent
  FEC block. `k` source symbols (§5.1a) + `r` repair symbols; every symbol is
  `s` bytes for the coding (last source symbol zero-padded to `s`, padding never
  on the wire).
- **Systematic MDS (Cauchy Reed–Solomon over GF(256)):** source symbols are
  transmitted unmodified (zero encode cost on the no-loss path). Each repair
  symbol is `repair[j] = Σ_i c[j][i]·source[i]`, where the coefficient row
  `c[j][*]` is a **Cauchy** row — `c[j][i] = 1 / (x_j ⊕ y_i)` over GF(256) with
  `x_j` (repair) and `y_i` (source) drawn from disjoint fixed element sets. Both
  ends reconstruct the row from the repair subheader `(repair_idx, window_len=k)`
  alone — no coefficients on the wire. The Cauchy structure is **MDS**: any `k`
  of the `k+r` symbols are linearly independent, so recovery is *guaranteed*
  (not probabilistic).
- **Capacity cap `k + r ≤ 256`** (GF(256) has 256 distinct elements). Adaptive
  MTU (§3.2) keeps this satisfied for large frames — a 512 KB IDR is k≈132 at a
  jumbo rung (leaving ≥124 for repair) but k≈370 at 1400 B. If a frame's
  `k + r_target > 256` at the current MTU, FrameFramer **disables FEC for that
  frame** (`r = 0`, ARQ-only) and raises a stat (`fec_oversize_k`); the source
  symbols still ship. This is the concrete reason the SHM path pairs with
  adaptive/jumbo MTU.
- **RX decode:** with all `k` source symbols → deliver by concatenation, no
  decode. With ≥ `k` total symbols (any source/repair mix) → GF(256) Gaussian
  elimination on the received rows recovers the missing source symbols
  (guaranteed by MDS). With < `k` after the block deadline / on supersession →
  frame lost (§6.2), no partial delivery.
- **Emission order:** all `k` source symbols first (source-first delivers the
  no-loss case without any FEC decode), `END_OF_BLOCK` on the last source
  symbol, then the `r` repair symbols (same `block_id`).
- **Adaptive per-frame policy** (rates provisional, gate-2-derived):

  | condition | repair count | rationale |
  |---|---|---|
  | `k ≤ fec.min_k` (seed 3) | `r = 0` (ARQ-only) | at k=3 one repair = 33% overhead; NACK→RETRANSMIT recovers within deadline (§17 gate 3). |
  | P-frame, `k > min_k` | `r = ceil(k · p_rate)`, seed `p_rate` 0.10 | P-frames are expendable (supersession §6.2); light parity for the short burst diversity misses. |
  | IDR frame | `r = ceil(k · i_rate)`, seed `i_rate` 0.25 | IDR loss is catastrophic (whole GOP until next IDR); heavier parity justified. |

- **Priority:** a frame's repair symbols are that frame's **live data**, emitted
  immediately after its source symbols at the same live priority (§5.3), *not*
  demoted to retransmit priority.

### 14.2 JSCC inner decision contract

The controller's per-frame decision is a pure, deterministic calculation. It
does not consume instantaneous RSSI and does not assume independent packet loss.
Its loss estimator supplies `predicted_loss_symbols`, a conservative frame-loss
quantile fitted from a defined observation window or replay trace. Until the RF
burst model is measured, this value is an explicit input rather than a hidden
binomial calculation.

Inputs are `k`, `predicted_loss_symbols`, configured `fec_floor_symbols` and
`fec_cap_symbols`, frame `deadline_us`, elapsed time, estimated remaining source
TX airtime, P95 return RTT, estimated resend airtime, ARQ guard time, and whether
the frame is ARQ-capable. The decision is:

1. If elapsed time plus remaining source TX airtime exceeds the frame deadline,
   discard before spending more airtime (`deadline_unreachable`). Equality is
   allowed and is not a miss.
2. Otherwise choose
   `m = min(max(predicted_loss_symbols, fec_floor_symbols), fec_cap_symbols,
   256-k)`. If `k>256`, `m=0` and capacity is limited. A clamp below the
   predicted count is reported as `fec_capacity_limited`; it does not by itself
   discard a frame that may still arrive intact.
3. ARQ is eligible only for an ARQ-capable frame when the time remaining after
   original source transmission is at least
   `rtt_p95_us + resend_airtime_us + arq_guard_us`. Equality is eligible.
4. The reason code is stable and mutually exclusive:
   `deadline_unreachable`, `fec_capacity_limited`, `fec_and_arq`, `fec_only`,
   `arq_only`, or `unprotected`. Numeric telemetry may map these names to a
   local enum, but the names are the diagnostic contract.

The estimator, airtime model, and outer/middle loop are separate components.
This contract only allocates protection for the current frame. The existing
fixed-rate §14.1 policy remains the runtime fallback until a measured estimator
feeds this decision; loss of controller input therefore preserves the authored
configuration rather than silently selecting optimistic protection.

Before runtime actuation, frame-SHM RX runs a **shadow-only** estimator over
post-diversity source-symbol loss. For block `N`, prediction is calculated only
from finalized blocks before `N`; block `N` is observed after its outcome is
fixed. The initial diagnostic is the nearest-rank P95 of the trailing 120
blocks, requires 20 samples, and predicts zero during cold start. It changes no
wire field, parity count, ARQ gate, or deadline. Its purpose is to expose
underprediction and adaptation lag in §15.3 while fixed §14.1 protection stays
authoritative. These seeds are not an adopted RF loss model.

A second protection-aware shadow estimates **transmitted repair demand**, not
only missing source symbols. For a recovered block this is one plus the highest
repair index needed to supply `k` received equations, so repair-packet loss is
included. An unrecovered block reports the lower bound `repairs_emitted_so_far
+ missing_equations`; that observation is marked censored and must not be
treated as an exact sample. Demand is normalized to permille of `k`, estimated
as the trailing-120 maximum, then converted back to symbols for the next
block. It requires 20 samples and uses a 100-permille cold-start rate. This
candidate is also shadow-only and does not supersede the original source-loss
telemetry.

---

## 15. I/O bindings, configuration & observability

### 15.1 Binding model
- Pools per node: **≤1 shm** (in XOR out), **≤1 unix socket** (in XOR out),
  **≤4 UDP** (each independently in or out). UDP and the **`frame-shm`** shm
  kind (§15.4) are live; the unix socket remains v1-reserved.
- The `frame-shm` binding counts against the **shm pool (≤1)**, not the ≤4-UDP
  pool. `bind.kind:"frame-shm"` + `bind.name:"<ring>"` (POSIX SHM object). An
  ingress `frame-shm` routes the stream through `FrameFramer` (§5.1a); an egress
  `frame-shm` receives whole reassembled frames (§6.3a).
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
    "return": { "guard_us": 300, "return_window_us": 2000,
                "unicast": false },
    "csa":    { "psk": "<operator-provisioned; craft+ground only>",
                "settle_s": 3.0, "verify_timeout_ms": 150,
                "min_interval_s": 5, "ack_timeout_ms": 1000,
                "rendezvous_timeout_s": 5, "home_chan": 5745,
                "channel_allowlist": [5745, 5805, 5825] }
  },
  "air":   { "kind": "radio", "ack_responder": false,
             "wedge_window_ms": 1000, "wedge_min_submits": 8 },
  "stats": { "hz": 1, "bind": { "kind": "udp", "send": "127.0.0.1:9110" } },
  "control": { "bind": "0.0.0.0:8091" }
}
```
- RX nodes use `"dir":"out"` streams (UDP `send` targets) and `role:"rx"` adapters
  (diversity = same `channel`; a scout may sit on a different channel on another
  adapter).
- Every policy constant is overridable → bench re-derivation (§9, §17) is config,
  not recompile.
- `csa.psk` is present only on craft + ground configs; it MUST be excluded from
  stats and logs.
- A **`frame-shm` stream** carries its own per-stream `fec` block (§14.1):
  ```json
  { "stream_id": 0, "stream_type": "RTP", "dir": "in",
    "bind": { "kind": "frame-shm", "name": "venc_frame" },
    "fec": { "scheme": "rlc256", "i_rate_permille": 250,
             "p_rate_permille": 100, "min_k": 3 } }
  ```
  `scheme` `"none"` (default) fragments + ARQs but emits no repair symbols;
  `"rlc256"` enables §14.1. Rates are integer per-mille (project convention). On
  a `udp` stream the `fec` block is ignored (Framer path, §5.1).

### 15.3 Streaming stats (newline-delimited JSON)
Emitted at `stats.hz` to stdout and/or the stats binding. Fields map 1:1 to the
§16.2 counters plus the state an operator needs to *see* a demote, a flap-freeze, a
table mismatch, phantom diversity, a stalled adapter, or a failing return path:
```json
{ "t_ms": 172834, "node": 17, "session": 2748291,
  "adapters": [ { "name": "wlan0", "rx": 10234, "dup": 812,
    "rssi_best": -58, "rssi_mean": -63, "snr": 22, "noise": -85,
    "tx_submitted": 540, "tx_failed": 2, "tx_timeout": 0,
    "drop": 0, "filtered": 0, "kernel_drop": 0, "tsf_fallback": 0,
    "tx_reports": 531, "tx_report_fails": 0,
    "adapter_stalled": false, "tx_wedged": false } ],
  "streams": [ { "stream_id": 0, "type": "RTP",
    "seq": 90233, "delivered": 89901, "uniq": 90100, "diversity": 178342,
    "loss_prediversity_milli": 41, "loss_postdiv_prearq_milli": 6,
    "recovered_arq": 220, "recovered_fec": 0,
    "frame_count": 89571, "frame_bytes": 5872391040,
    "frame_size_last": 65432, "frame_size_min": 8120,
    "frame_size_max": 241810, "frame_interval_us": 11106,
    "frame_jitter_us": 184,
    "frames_fast": 89571, "frames_unrecoverable": 0, "malformed": 0,
    "jscc_shadow_blocks": 89681, "jscc_predicted_loss_symbols": 3,
    "jscc_observed_loss_symbols": 1, "jscc_underpredicted_blocks": 72,
    "jscc_predicted_parity_symbols": 271044,
    "jscc_predicted_repair_symbols": 4,
    "jscc_observed_repair_symbols": 3,
    "jscc_repair_underpredicted_blocks": 18,
    "jscc_repair_demand_censored_blocks": 2,
    "jscc_repair_predicted_parity_symbols": 358121,
    "shm_full_drops": 0, "shm_oversize_drops": 0, "shm_bad_slots": 0,
    "dropped_superseded": 110, "dropped_deadline": 8,
    "nacks_sent": 18,
    "nack_rtt_hist": [0,2,7,6,2,1,0,0], "nack_rtt_max_ms": 34,
    "arq_rec_hist": [0,1,6,6,3,1,1,0], "arq_rec_max_ms": 61,
    "resends_sent": 230, "double_send_suppressed": 5,
    "decode_errors": 0, "active_profile": 4, "table_version": 178 } ],
  "return": { "reports_expected": 10, "reports_received": 9,
    "return_window_hits": 7, "return_window_misses": 2,
    "unicast_sent": 0, "unicast_fallback": 0 },
  "link": { "target_originator": 9, "target_session": 183726,
    "profile": 4, "mcs": 4, "tx_power_qdb": 1800,
    "report_epoch": 1822, "report_age_ms": 40,
    "state": "HOLD", "flap_freeze": false, "csa_state": "IDLE" } }
```
`return_window_hits/misses` (TX-side) and `reports_expected/received` expose the
§7.2 optimisation's health directly, and `adapter_stalled` + the
`loss_prediversity` vs `loss_postdiv_prearq` pair expose phantom diversity and the
ρ decorrelation gauge — the two field-failure modes the design most fears.
`tx_wedged` is the §9.10 CCX-liveness verdict (TX adapter only, meaningful on
the radio backend).
`uniq`/`diversity` are the §17 gate-2 estimator inputs; the `nack_rtt_*` /
`arq_rec_*` histograms (cumulative, ms upper bounds 1,2,4,8,16,32,64,+inf) are
the §17 gate-3 estimator outputs.

On a **`frame-shm` binding**, `frame_count` and `frame_bytes` count successful
whole-frame transfers at the local SHM boundary: consumer `read_frame()` on TX
ingress, producer `write_frame()` on RX egress. `frame_size_last`,
`frame_size_min`, and `frame_size_max` are the successful slot payload sizes in
bytes since start/reset (`frame_size_min` is 0 before the first frame).
`frame_interval_us` is the monotonic-host-time gap between the two most recent
successful transfers. `frame_jitter_us` is an integer EWMA of the absolute
change between consecutive intervals, updated as `J += (|D| - J) / 16`; both
timing fields are 0 until enough frames have arrived. These fields are 0 on UDP
bindings. They measure local frame-boundary cadence, not RTP packet jitter and
not encoder PTS cadence. Stats reset clears the counters, extrema, and timing
history together.

On a **`frame-shm` egress** stream (§6.3a) the per-frame reassembler counters
map onto these fields directly: `recovered_fec` = frames rebuilt from repair
symbols, `frames_fast` = frames delivered all-source with no decode,
`frames_unrecoverable` = frames finalized below `k` (no way to decode),
`decode_errors` = FEC decode returned an error, `malformed` = symbols rejected
before decode, and `dropped_superseded`/`dropped_deadline` = frames dropped by
supersession / past their deadline. On a UDP (RTP/telemetry) stream the
per-frame fields (`frames_fast`, `frames_unrecoverable`, `malformed`) stay 0.
On frame-SHM ingress, `malformed` counts whole frames rejected by FrameFramer;
RX-only reassembly outcome fields remain 0.

The `jscc_*` fields are receiver-side, diagnostic-only shadow state from
§14.2. `jscc_shadow_blocks` counts finalized blocks observed by the estimator;
`jscc_predicted_loss_symbols` and `jscc_observed_loss_symbols` are the most
recent causal prediction and result; `jscc_underpredicted_blocks` counts
results greater than their prior prediction; and
`jscc_predicted_parity_symbols` sums hypothetical predicted parity. They are
zero on TX ingress and UDP streams. Stats reset clears both the estimator
window and these counters so a new observation generation has an unambiguous
cold start. None of these values changes active §14.1 FEC.

The `jscc_*repair*` fields are the separate protection-aware shadow.
`jscc_predicted_repair_symbols` and `jscc_observed_repair_symbols` are its
latest causal prediction and transmitted-repair demand;
`jscc_repair_underpredicted_blocks` counts exact or lower-bound observations
above their prior prediction; `jscc_repair_demand_censored_blocks` counts
unrecovered lower-bound observations; and
`jscc_repair_predicted_parity_symbols` is cumulative hypothetical parity. A
censored observation is evidence of insufficient protection, not an exact
demand measurement. Reset clears this estimator and its counters too.

`shm_full_drops`, `shm_oversize_drops`, and `shm_bad_slots` expose local ring
backpressure/ABI failures separately from air/frame-reassembly loss. They are 0
on UDP bindings. On frame-SHM egress they come from the producer ring; on
frame-SHM ingress `shm_bad_slots` comes from the consumer ring. Adapter
`kernel_drop` is the Linux socket's `SO_RXQ_OVFL` cumulative receive-queue loss
(UDP/kernel socket backends; 0 where unavailable). It is distinct from `drop`,
which remains backend/synthetic queue loss.
`filtered` is the backend's cumulative count of structurally rejected or
self-originated receive frames. It is 0 where filtering occurs below an
observable boundary.

### 15.4 `frame-shm` binding — venc_frame_ring slot format
The `frame-shm` binding attaches to (ingress) or creates (egress) a POSIX
shared-memory ring produced by the waybeam encoder (`waybeam_venc`
`venc_frame_ring`, canonical header `waybeam_venc/include/venc_frame_ring.h`).
One slot = one whole encoded frame. waybeam-link treats a slot's payload as
**opaque** (§1 "RTP is opaque on the wire"): it fragments/reassembles the bytes
and reads only the 8-byte metadata prefix.

**Ring:** POSIX SHM object `/<bind.name>` (default `venc_frame`). Producer-owned
(creates `O_EXCL`, `shm_unlink`s stale + on teardown). SPSC (single producer,
single consumer), lock-free, futex consumer-wake. Header magic `0x5646524D`
("VFRM"), version 1; default geometry 16 slots × 512 KB (~8 MB). Free-running
`write_idx`/`read_idx`; on a full ring the producer **drops and keeps running**
(never blocks). All fields native-endian (same-host only).

**Slot payload** = 8-byte `VencFrameMeta` prefix + Annex-B frame bytes (NAL start
codes preserved):

| off | size | field | notes |
|---|---|---|---|
| 0 | 4 | `pts` | u32; encoder capture timestamp (SDK units), truncated |
| 4 | 1 | `codec` | u8; `0x01` = H.265 (only value emitted) |
| 5 | 1 | `flags` | u8; bit 0 = IDR frame; other bits reserved 0 |
| 6 | 2 | `reserved` | u16; must be 0 |
| 8 | N | frame | raw Annex-B (start codes + NAL units) |

The whole `[VencFrameMeta][Annex-B]` blob is the FrameFramer source-blob (§5.1a);
on egress (§6.3a) the reassembled blob is written back byte-identical. The
metadata (`pts`, `codec`, IDR `flags`) therefore rides TX→RX transparently inside
the opaque payload — no DATA-header change, no re-derivation. FrameFramer reads
`flags` bit 0 for §4.1 ARQ; nothing else parses the blob.

### 15.5 Control plane (REST/HTTP)
Optional, config-gated: `"control": { "bind": "<addr>:<port>" }` (absent = off;
default port `8091`). A minimal **HTTP/1.0** server folded into the single
event loop — **no threads, no locks**: the listen socket and any in-flight
connections are polled with a 0 ms timeout once per tick, each connection
serves **one** bounded request (headers + body ≤ 8 KiB) and is closed, except
the SSE stream which is held open. A slow or partial request is **dropped, not
awaited** — the flight loop never blocks on a client.

Auth posture matches the data plane (§13, no-auth): the control port rides a
**trusted same-host/LAN** network. Bind `127.0.0.1` to keep it host-local
(SSH/tunnel to reach it); bind a routable address only on a trusted net.
`csa.psk` and any secret are **never** echoed by `GET /info`. **This control
plane supersedes the ground CSA stdin trigger, which is removed** — `POST
/api/v1/csa` is now the only campaign trigger.

**Read** (idempotent, present on every node):

| Method + path | Returns |
|---|---|
| `GET /api/v1/stats` | the current §15.3 snapshot as one JSON object (no trailing newline) |
| `GET /api/v1/stats/stream` | `text/event-stream`; one §15.3 object per `stats.hz` tick |
| `GET /api/v1/info` | static identity: `role`, `node`, `session`, `table_version`, `streams[]`, `adapters[]`, `build` |
| `GET /api/v1/health` | terse `{ state, mcs, profile, rssi_best, loss_milli, fps }` |
| `GET /api/v1/discovery` | bounded passive discovery: `{nodes:[], streams:[]}` from HEARTBEAT/DATA observations |

`GET /api/v1/discovery` is read-only and node-local. `nodes[]` contains
`{originator,session,last_seen_ms}` for HEARTBEAT or DATA senders. `streams[]`
contains DATA-derived candidates and active latches as
`{originator,session,stream_id,stream_type,packet_count,first_seen_ms,last_seen_ms,latched}`.
Times are monotonic node-local millisecond stamps and are comparable only within
one node's responses. HEARTBEAT never fabricates a stream entry. Both lists are
bounded by the §13 discovery cap, refreshed by matching traffic, and aged out
after the existing discovery/idle windows; the endpoint does not alter latch
selection or admission state.

**Write** (live; `200 { "ok": true, … }` on success, `4xx { "ok": false, "error": "…" }`
otherwise). Every write is **MUT_LIVE** — applied in-loop, no restart:

| Method + path | Body | Effect |
|---|---|---|
| `POST /api/v1/csa` | `{ "mhz": 5805, "class": 0 }` | start a §11 CSA campaign (issuer/ground node) |
| `POST /api/v1/link/profile` | `{ "min": 3, "max": 3 }` | §9.7 profile pin; `min==max` freezes the operating point, `{ "max": 255 }` unpins (TX node) |
| `POST /api/v1/fec` | `{ "stream_id": 0, "i_permille": 250, "p_permille": 100, "min_k": 3 }` | retune a `frame-shm` stream's §14.1 FEC rates (TX node) |
| `POST /api/v1/stats/reset` | `{}` | zero the cumulative counters — a clean measurement window |
| `POST /api/v1/video/recover` | `{ "stream_id": 0 }` (optional with one latch) | RX emits one §3.9 recovery request for a latched RTP stream |

Endpoints act only where meaningful — `csa` on the issuer, `link/profile` and
`fec` on the TX. An endpoint invoked in a mode where it does not apply returns
**409**; an unknown path **404**; a malformed or oversize body **400**. The
write knobs are exactly the §9/§11/§14 levers that were previously boot-time
JSON only; the profile pin is the operating-point (MCS + bitrate) lever, since
a profile bundles rate/power/MTU per §9.3.

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

### 16.3 UDP broadcast/sniffer air backend

`air.kind: "udp-broadcast"` is the RF-broadcast bench analogue. It uses the
existing `air.tx` and `air.rx` arrays and requires exactly one TX endpoint plus
one or more RX endpoints. `tx[0]` is an IPv4 broadcast destination; every RX
entry is a virtual adapter bound to the shared listen address/port. Entries MAY
repeat the same endpoint: Linux broadcast fanout gives each `SO_REUSEADDR`
socket its own observation, which exercises the normal pre-diversity merge.
Multiple nodes may bind the same channel. Every injected air frame is sent once;
every local listener receives and passively filters the channel rather than
owning a point-to-point route.

The backend enables `SO_BROADCAST` on TX and shared-address binding on RX. Before
delivery to the core it validates the complete waybeam wire packet (§3), rejects
malformed/non-waybeam datagrams, and rejects packets whose common-prefix
`originator` equals the local node. These filtered packets increment the
adapter's `filtered` counter, never `rx` or synthetic `drop`. Valid packets from
all other originators, including HEARTBEAT and return traffic, are delivered
unchanged. Thus a node may transmit and sniff the same channel without consuming
its own looped-back traffic. Ordinary `air.kind: "udp"` retains its existing
multi-target/multi-listener point-to-point simulation semantics and performs no
new filtering.

Repeated local RX bindings model independent receiver delivery paths only when
the bench applies per-adapter synthetic loss; without injected loss they receive
identical copies. This backend is Linux/IPv4 bench tooling, not a claim that UDP
broadcast models RF timing, RSSI, collision, capture, or half-duplex behavior. Loopback use SHOULD
send to `127.255.255.255:<port>` and listen on `0.0.0.0:<port>`; subnet broadcast
may be used for a multi-host LAN bench.

An optional positive `air.pace_mbps` serializes broadcast datagrams at that
payload bit rate using a monotonic next-send deadline. `0` (default) is unpaced.
Pacing is strongly recommended for frame-SHM video benches: without RF
serialization a complete encoded frame is emitted as a host-speed burst and may
overflow the receiver's UDP queue even when the intended channel loss is zero.
This pacing models serialization only; it does not model PHY overhead or
contention.

Accepted retransmissions that have passed the §5.3 deadline, attempt, hold-down,
and airtime-budget gates use a separate deadline-priority lane. At each
serialization opportunity this lane is drained before queued live packets. This
does not exempt retransmissions from the §5.3 airtime cap; it only prevents an
already-authorized recovery from waiting behind a complete encoded-frame burst
in a host-side pacing FIFO. Air receive readiness MUST also wake a transmitter
that is waiting for local stream ingress, so NACK handling does not inherit the
local-ingress polling interval.

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
| `wedge_window_ms` / `wedge_min_submits` | §9.10 TX-wedge watchdog | silent across a healthy 500–4500 pps sweep; fires within one window of an induced USB wedge |

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
   *Estimator (ground-side, ms-domain):* each NACKed seq is anchored at its
   **first** and its **most recent** NACK build; a gap-filling arrival carrying
   `RETRANSMIT=1` yields two samples — **round-trip** (most-recent-NACK →
   arrival; the §5 freshness-gate input) and **recovery** (first-NACK →
   arrival; the quantity compared against the I-frame deadline, since a lost
   NACK's re-NACK backoff is real recovery latency). Late originals without
   the flag close the gap but never sample. Exposed as cumulative
   power-of-two-ms histograms in stream stats (`nack_rtt_*`, `arq_rec_*`).
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
