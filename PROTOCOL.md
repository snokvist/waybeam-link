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
| QoS Control | `0x00 0x00` for normal reports; TID 6 for urgent NACKs; Normal ACK policy |
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

**Urgent ARQ lane.** NACKs and DATA packets carrying `RETRANSMIT=1` use
QoS-Data TID 6 so monitor/devourer injection can select the voice access
category; the monitor socket also sets `SO_PRIORITY=6`. When the destination is
broadcast, radiotap keeps `NOACK`; an enabled unicast-return NACK keeps the
hardware-ACK radiotap above. Live DATA and LINK_REPORT traffic remain TID 0 / the
ordinary Data shape. This changes only 802.11 encapsulation, never the waybeam
wire packet inside it.

### 3.1 Common prefix (all packet types) — 11 bytes

| off | size | field | notes |
|---|---|---|---|
| 0 | 2 | `magic` | `0x5742` (bytes `57 42`) — protocol guard |
| 2 | 1 | `ver_type` | hi nibble = version (`0x0`), lo nibble = packet type |
| 3 | 2 | `originator` | sender node id (§2) |
| 5 | 2 | `destination` | advisory; `0x0000` = broadcast |
| 7 | 4 | `session_id` | sender boot nonce |

**Packet types** (low nibble): `0x1 DATA · 0x2 NACK · 0x3 LINK_REPORT ·
0x4 HEARTBEAT · 0x5 CSA · 0x6 RECOVERY_REQUEST · 0x7 JSCC_FEEDBACK ·
0x8 CACHE_STATUS · 0x9 CACHE_REQUEST · 0xA CACHE_REPLY · 0xB ANNOUNCE`. 11 of
16 used; the version nibble will not ship 16 wire-incompatible revisions, so
there is no type-budget scarcity. Future types (e.g. a dedicated FEC-repair
type, §14) take free slots.

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
| 1 | `ARQ` | important/IDR retransmit class with I-frame deadline |
| 2 | `RETRANSMIT` | this packet is itself a resend (stats/diagnostics) |
| 3 | `FEC_REPAIR` | packet is a FEC repair symbol (§14; an 11-byte subheader precedes payload) |
| 4 | `CSA_ARMED` | **craft→ground ARM ack** — craft has accepted the in-flight CSA campaign and will follow (§11.6) |
| 5 | `PFRAME_ARQ` | opt-in P-frame retransmit eligibility; retains the P-frame deadline |
| 6–7 | reserved | 0 |

**Redundant per-packet metadata (critical rule):** `stream_type`, `block_id`,
`END_OF_BLOCK` membership, the ARQ-class flag, `active_profile`, and `table_version`
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
  live: `ARQ`- or `PFRAME_ARQ`-flagged, not superseded (§6), within its class
  deadline (§8).
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
  **Enforcement point (Pass 41):** the filter runs at TX ingest, BEFORE the
  §9 selector and the §9.11 fps ladder consume the report. With
  `preferred_originator` configured, only that originator passes (its
  session follows reboots). Without it, the first reporter latches; the
  latch follows a same-originator session change (reboot, §2) and
  **re-latches to the next reporter only after `relatch_ms` of silence**
  (seed 4 × `report_timeout_ms`). Rejected reports are counted
  (§15.3 `reports_rejected`); the plausibility cross-check remains §17
  future work. An accepted session/source transition starts a new selector
  epoch and smoothing domain; state from the previous reporter MUST NOT make
  the newly accepted identity look stale.

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
LINK_REPORT, CSA, ANNOUNCE, or HEARTBEAT resets the one-second quiet interval, so
active traffic suppresses redundant keepalives. HEARTBEAT uses the node's current
`originator` and per-boot `session_id`, with broadcast destination `0`. A craft
that continuously emits ANNOUNCE (§3.12) at ≥1 Hz therefore **never separately
emits HEARTBEAT** — ANNOUNCE subsumes it as the presence beacon (Pass 62); the
HEARTBEAT path still serves non-announcing nodes (grounds, quiet rx).

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
The local `venc.recovery_enabled` permission is independent of
`venc.enabled`: the former authorizes only the rate-limited `/request/idr`
call, while the latter authorizes bitrate writes under §9.6. A deployment may
therefore provide decoder recovery without making waybeam-link a bitrate
authority. Both permissions default false.

### 3.10 JSCC_FEEDBACK packet (type `0x7`) — 37 bytes

An additive, per-stream RX→TX measurement packet for the §14.2 controller. It
does not replace `LINK_REPORT`: RF selection remains node/link scoped, while
repair demand and ARQ timing are properties of one received stream.

| off | size | field | notes |
|---|---:|---|---|
| 0 | 11 | *common* | sender = RX node that owns the estimator |
| 11 | 2 | `target_originator` | TX node that owns the source stream |
| 13 | 4 | `target_session` | exact current TX boot/session nonce |
| 17 | 1 | `target_stream_id` | source stream being measured |
| 18 | 4 | `feedback_epoch` | u32 monotonic per reporter |
| 22 | 2 | `repair_demand_permille` | causal predicted transmitted-repair demand normalized by `k` |
| 24 | 4 | `rtt_p95_us` | causal P95 NACK-to-retransmit RTT; 0 unless valid |
| 28 | 2 | `repair_samples` | bounded estimator sample count |
| 30 | 2 | `rtt_samples` | bounded RTT estimator sample count |
| 32 | 1 | `valid_flags` | bit 0 repair estimate present; bit 1 RTT estimate present; other bits 0 |
| 33 | 4 | `observed_block_id` | newest finalized block included in the repair estimator |

The repair field carries the estimator's normalized rate, not a symbol count,
because the RX does not know the next frame's `k`. The TX converts it causally
for frame `N` as `ceil(rate * k_N / 1000)`. `repair_samples` is diagnostic;
readiness is stated only by bit 0 and requires the authored estimator minimum.
An unrecoverable/censored block may raise the estimate but never turns a lower
bound into an exact sample (§14.2).

`rtt_p95_us` is derived only from arrivals explicitly marked `RETRANSMIT` that
fill a NACKed gap. Bit 1 remains clear until at least one RTT sample exists;
the TX independently requires `rtt_samples >= min_rtt_samples` from its authored
shadow configuration. Zero with bit 1 clear means unavailable, not zero
latency.

The receiver emits this packet at the existing report cadence after a matching
frame-SHM stream has latched. It uses the same return injection, quiet-gap, and
reporter/target filtering as NACK/LINK_REPORT. TX accepts it only for its own
exact `(originator, session_id, stream_id)`. The feedback cache is additionally
keyed by reporter `(prefix.originator, prefix.session_id)`, and
`feedback_epoch` is monotonic-forward only **within that reporter session**.
When the accepted reporter reboots and its session changes, TX replaces the
cached feedback before comparing the new session's epoch; an epoch reset across
receiver boots must not leave the JSCC controller permanently stale.
The packet is measurement-only: receipt updates a bounded cache and can never
directly alter FEC, ARQ, discard, MCS, or encoder state.

The runtime shadow treats feedback as usable only while both required validity
bits are set and its age is within the configured shadow timeout. Missing,
invalid, stale, wrong-session, or replayed feedback selects the configured
§14.1 fixed policy and reports the specific fallback state. It must never be
silently replaced by zero loss or zero RTT.

### 3.11 Cache packets (types `0x8`–`0xA`) — spatial cache repair (§14.3)

Three fixed-schema packets for the §14.3 Cache Controller. In v1 they travel
**only over the UDP/IP cache sockets (§14.3, §15.2)**, never injected on the
air path, but they carry the standard §3.1 header so a later RF binding needs
no re-numbering. The common prefix names the **sender** (aggregator or cache
node); the body names the **target stream** through the same target descriptor
as NACK/LINK_REPORT (§3.1 two-identity split). A cache node is an ordinary
waybeam-link node: its cache identity IS its `originator` (§2), and its
`session_id` is its own per-boot nonce.

#### CACHE_STATUS (type `0x8`) — 29 bytes

| off | size | field | notes |
|---|---:|---|---|
| 0 | 11 | *common* | sender = the cache node |
| 11 | 2 | `target_originator` | TX node whose stream is cached |
| 13 | 4 | `target_session` | exact TX boot/session nonce as latched |
| 17 | 1 | `target_stream_id` | |
| 18 | 4 | `oldest_block` | oldest `block_id` still retained |
| 22 | 4 | `newest_block` | newest `block_id` retained |
| 26 | 2 | `rx_health_permille` | 0–1000; rolling mean unique/`k` over the retention window |
| 28 | 1 | `capability_flags` | bit 0 = IP transport (always set in v1); other bits MUST be 0 |

Sent to each configured aggregator endpoint every `status_interval_ms` per
tracked stream **with a non-empty retention window** — an empty window is
silence, not a zero-filled status. Aggregators retain status independently per
cache and target stream identity; one tracked stream MUST NOT overwrite
another's eligibility state. `rx_health_permille > 1000` is a decode error.

#### CACHE_REQUEST (type `0x9`) — 32 bytes fixed + two bitmaps

| off | size | field | notes |
|---|---:|---|---|
| 0 | 11 | *common* | sender = the aggregator |
| 11 | 2 | `target_originator` | stream's TX node |
| 13 | 4 | `target_session` | |
| 17 | 1 | `target_stream_id` | |
| 18 | 2 | `target_cache` | `originator` of the ONE cache addressed |
| 20 | 4 | `request_id` | monotonic per aggregator boot |
| 24 | 4 | `block_id` | block being repaired |
| 28 | 2 | `window_len` | `k` (1–256), from the block's subheaders (§5.1a/§14.1) |
| 30 | 1 | `max_symbols` | ≥1; reply symbol allowance for this request |
| 31 | 1 | `repair_have_len` | bytes of the second bitmap (≤32) |
| 32 | ⌈k/8⌉ | `missing_sources` | bit *i* set ⇒ source symbol *i* absent from the merged block |
| 32+⌈k/8⌉ | var | `repair_have` | bit *r* set ⇒ repair symbol `repair_idx` *r* ALREADY held |

Total length MUST equal `32 + ⌈k/8⌉ + repair_have_len`. It is sent only after
the §14.3 local-collection phase closes below `k`. `max_symbols` is bounded by
the FEC deficit, the per-request `reply_limit`, and the remaining §14.3
per-block symbol cap.

#### CACHE_REPLY (type `0xA`) — 17 bytes fixed + one wrapped DATA packet

| off | size | field | notes |
|---|---:|---|---|
| 0 | 11 | *common* | sender = the answering cache |
| 11 | 4 | `request_id` | echoed from the request |
| 15 | 2 | `wrapped_len` | ≥ 26 |
| 17 | var | `wrapped` | ONE verbatim §3.2 DATA packet (header + subheader + chunk) as heard on the air |

One symbol per reply datagram; several reply datagrams may share a
`request_id`. The wrapped bytes are the cache's stored **original wire
packet**, unmodified — the aggregator revalidates it through the normal §3.1/
§3.2 decode before merging, so a cache cannot hand the reassembler anything a
radio could not have. Reply selection at the cache: requested missing sources
first (ascending index), then held repair symbols whose `repair_idx` bit is
clear in `repair_have` (ascending), stopping at `max_symbols`. A cache holding
none of the useful symbols stays silent.

### 3.12 ANNOUNCE packet (type `0xB`) — 30 bytes

A craft's **pairing beacon**: it advertises presence, claim state, and the
session **pairing token** so a ground can rendezvous and CSA-claim it with no
pre-shared config (§11.4a). Emitted at **1–2 Hz in both claimed and unclaimed
states** (unlike HEARTBEAT it is *not* suppressed by active DATA) — continued
emission while claimed is what lets a rebooted ground re-learn the token and
re-claim in place after the binding releases (§11.5a).

| off | size | field | notes |
|---|---:|---|---|
| 0 | 11 | *common* | §3.1 — sender = the craft; `destination` = `0` |
| 11 | 1 | `flags` | bit0 `claimed`; bit1 `psk_present`; bits 2–7 reserved (0) |
| 12 | 2 | `claimed_by` | `originator` of the binding ground, else `0` (advisory: UI/courtesy, not enforced) |
| 14 | 16 | `psk` | the session pairing token when `psk_present=1`; **all-zero** when `psk_present=0` (secret mode, `csa.psk` configured, §11.4a) |

- **Unauthenticated by design.** ANNOUNCE is an advertisement, not a control
  action: it carries no MAC. A forged ANNOUNCE with a bogus `psk` only wastes one
  claim attempt — the resulting CSA fails the craft's §11.4 MAC check and no
  claim occurs. This matches the §13 posture (see the added row).
- **`psk` is a token, not a secret.** In the default announced mode any RF-adjacent
  receiver can read it; takeover resistance comes from the §11.4a command-source
  **binding**, not from the token's confidentiality. A configured `csa.psk`
  selects secret mode: `psk_present=0` (16 zero bytes), a genuine secret (§11.4a).
- HEARTBEAT (§3.8) wire format is unchanged (exactly 11 bytes); ANNOUNCE never
  creates or refreshes per-stream RX state (like HEARTBEAT, it is node-scoped).
  Because ANNOUNCE resets the §3.8 quiet interval, a continuously-announcing craft
  **subsumes** its own HEARTBEAT — it emits ANNOUNCE only, not both (Pass 62).
- **Redaction (relaxed, Pass 63):** the ANNOUNCE `psk` with `psk_present=1` is the
  *announced session token* — public by construction (on the air, per-boot rotated
  on the craft), so it MAY be surfaced, logged, and cached; the ground caches it to
  key its §11 CSA (§15.5a). Only the operator-provisioned `csa.psk` secret stays
  redacted (the §15 config-dump `"(set, redacted)"` invariant). Secret mode never
  carries the secret in ANNOUNCE (16 zero bytes), so the ANNOUNCE `psk` field is
  never sensitive in either mode.

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

Frame-SHM ingress may opt into `arq_mode:"all-frames"`. IDRs keep the existing
`ARQ` flag and I-frame deadline; non-IDRs carry `PFRAME_ARQ` and retain the
active profile's P-frame deadline. The default `arq_mode:"idr-only"` preserves
the existing classifier. `PFRAME_ARQ` is a measurement/coverage mechanism, not
permission to extend latency or to actuate adaptive FEC. A receiver that does
not understand bit 5 ignores it and therefore fails safe as IDR-only ARQ.

**High-cadence ARQ cutoff (Pass 40, operator-ruled).** Above `arq_max_fps`
(config `policy.arq.arq_max_fps`, seed **100**; 0 disables the cutoff) the
frame period drops below ~10 ms — the lowest comfortable recovery window:
§6.3a zero-block retention finalizes an incomplete block the moment the next
frame arrives, so at 101–144 fps even the I-frame class has under 10 ms of
usable repair time at the receiver. While the operating cadence (the §9.6
cadence input, ladder-snapped) exceeds the cutoff, the frame-SHM TX stamps
**neither `ARQ` nor `PFRAME_ARQ`**, and §14.2 treats those frames as not
ARQ-capable. Suppressed classifications are counted
(§15.3 `arq_cutoff_frames`). Above the cutoff, recovery is FEC + diversity +
§14.3 cache repair only.

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
2. Set `ARQ` from `VencFrameMeta.flags` bit 0 (IDR ⇒ 1). With the explicit
   `all-frames` mode, set `PFRAME_ARQ` instead on non-IDRs, §4.1.
3. **Fragment** the blob into `k` **source symbols** of size
   `s = active_profile.max_payload − 26 − 11` (header + §14 repair subheader,
   so source and repair symbols are interchangeable for coding); the last
   symbol carries the tail (`< s`) and is zero-padded to `s` only for the FEC
   computation (§14), never on the wire. `k = ceil(blob_len / s)`. `s` is fixed
   for the life of the block (a frame is fragmented atomically under one
   profile); a later frame may use a different `s` after a profile change —
   the wire is self-describing (§3.2).
4. Emit the `k` source symbols as DATA packets in order (`FEC_REPAIR` unset),
   with `seq` monotonic across the whole block. When `r = 0`, put
   `END_OF_BLOCK` on the last source symbol. Each source DATA payload is a
   **4-byte source subheader**
   (`window_len u16 = k`, `sym_index u16 = i`) followed by the chunk. The
   subheader makes every source symbol self-describing: RX reassembly (§6.3a)
   knows each symbol's index and the block's `k` without inferring them from
   `seq` gaps — so a stream with no FEC (ARQ-only) can never mistake a
   leading-loss run for a complete frame.
5. Per the §14 adaptive policy, generate and emit `r` **repair symbols**
   (`FEC_REPAIR` set, 11-byte subheader §14) after the source symbols, same
   `block_id`. When `r > 0`, put `END_OF_BLOCK` on the final repair symbol so
   the block-close/quiet-gap edge follows the complete parity tail.
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
- **Eligibility gate:** only `ARQ=1` or `PFRAME_ARQ=1` blocks are ever resent.
- **Importance/deadline class:** `ARQ` uses the I-frame budget;
  `PFRAME_ARQ` uses the P-frame budget. The bits are mutually exclusive.
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
- For a `frame-shm` egress, feed that first copy directly to
  `FrameReassembler` after dedup, without waiting for the generic packet `seq`
  cursor. Source/repair equations are self-indexed and may arrive out of order;
  packet ordering remains active only for loss, deadline, and ARQ accounting.
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
- Deliver UDP egress in-order, best-effort, as untouched RTP. Frame-SHM symbols
  use the post-dedup early path in §6.1 and therefore cannot be head-of-line
  blocked behind a missing packet sequence.
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
6. Successful fast/FEC completion marks the block packet-complete in the merged
   RX engine. Every pending gap belonging to that block becomes FEC-satisfied,
   advances without a packet-drop charge, and is excluded from all later NACK
   construction. Cache-delivered completion uses the same edge.

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
- For a lost seq that is `ARQ`- or `PFRAME_ARQ`-flagged, not superseded, within
  its class deadline: add to the pending SACK set. Coalesce into one bitmap per
  return window (§7), anchored at `base_seq`.
- With quiet-gap pacing, construct the NACK bitmap only after the repair-tail
  `END_OF_BLOCK` has closed local FEC collection. A block completed by FEC
  before that edge contributes no NACK; ARQ is residual repair, not a race
  against parity still in flight. If that final EOB is itself lost, a rolling
  host-time fallback at the return-window midpoint after the most recently
  received DATA symbol releases pending NACKs; EOB loss must not suppress ARQ
  indefinitely.
- Send via the **designated uplink TX adapter**; its RX blind spot while
  transmitting is covered by the diversity siblings (ground half-duplex is free).
- Re-NACK: bounded retries with a default 6 ms per-attempt backoff; stop on
  RETRANSMIT receipt or on deadline/supersession.

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
- A backend without a live TSF read (kernel monitor mode) MUST NOT add the
  midpoint delay to a NACK after the repair-tail EOB has already arrived through
  the host/USB path: it submits that NACK immediately after FEC close. Periodic
  LINK_REPORTs remain normal-priority and wait for the next EOB midpoint; if no
  EOB arrives for 100 ms, they degrade to §7.1 opportunistic return.

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
Once authorized, a retransmit has queue priority over live video (802.11e TID 6
on monitor/devourer and the dedicated resend queue on UDP-air), but the airtime
cap remains a hard downlink fraction, with an attempt cap and per-interval bound.
A burst needing more than a few repairs is past saving — let RTP concealment eat
it. The uplink is a **pluggable transport** so a dedicated backchannel could
replace it later without touching the core.

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
GET /api/v1/set?video0.maxIBytes=<B>&video0.maxPBytes=<B>   # one live group
GET /api/v1/dual/set?bitrate=<kbps>            # Star6E ch1 only; 501 on Maruko
```

- **Units kbps**, hard range **1000–200000** (venc-enforced; default 8192).
  `bitrate_min` is a policy floor ≥ 1000.
- **Single bitrate authority (deployment rule, not a flag):** venc's API is
  last-writer-wins with no arbitration. waybeam-link MUST be the only writer of
  `video0.bitrate` (and, with frame caps enabled, of `video0.maxIBytes`/
  `maxPBytes`): if waybeam-hub is present set its `venc.bitrate_enabled=false`
  (that flag lives in hub `mod_venc`, not venc); do not run wfb_ng
  `link_controller` — waybeam-link replaces it.
- **Write only on change (flash wear):** every `/set` persists to
  `/etc/waybeam.json`; push bitrate only when the target actually changes, never
  at the 10 Hz report rate.
- **Low-bitrate coupling (optional):** at floor profiles the SVC-T preset
  oscillates fps; waybeam-link MAY command `video0.resilience=racing` at the low
  end (coarse, hysteretic — `resilience` is heavier than a bitrate tweak).

**Per-frame size caps — horizon actuation (Pass 37).** With `venc.enabled`
and a frame-shm ingress stream, waybeam-link additionally commands venc's
per-frame ceilings (`maxIBytes`/`maxPBytes`, FRAMEBITS_FIRST) so a burst
frame can never be encoded larger than the active rung can carry inside its
deadline. **A per-frame budget channel is ruled OUT OF SCOPE** (operator,
2026-07-16): venc control is HTTP with persist-on-set, and Salsify-style
next-frame commands would need a new transport. The caps are therefore
**horizon caps** — pure functions of slow inputs, recomputed only when an
input changes and pushed under the same write-on-change/holdoff rules as
bitrate, riding the same §9.5 transition moments:

- `frame_period_us` — a **windowed frames-per-ingress-second estimate**
  (frame count over a ~1 s window; the §15.3 last-gap `frame_interval_us`
  is batch-drain-skewed and unsuitable), **snapped to the nearest ladder
  fps** `{30, 45, 60, 75, 90, 100, 120, 144}` so cadence jitter cannot
  churn the caps; `venc.fps_hint` (seed 100, matching the preferred low-latency
  mode) until measured.
- `budget_bps` — the active rung's §9.5 derived bitrate target (already net
  of airtime fraction, table FEC overhead, and control/telemetry reserves).
- `maxP = budget_bps · frame_period_us / 8·10⁶ · 1000/(1000 + p_rate‰) ·
  p_headroom‰/1000` — the deadline-safe P ceiling: one frame period of rung
  budget, net of the stream's §14.1 P parity.
- `maxI = budget_bps · arq_deadline_iframe_ms / 8000 · 1000/(1000 + i_rate‰)
  · i_headroom‰/1000` — the I-class recoverable window (§4.1/§8), net of I
  parity: an I-frame is sized to what ARQ/FEC can still rescue, not to one
  frame period.
- Clamps: `maxI ≥ maxP`; if the I-class deadline/FEC ceiling is tighter than
  the independently derived P cap, lower `maxP` to `maxI` (never raise I past
  its recoverable ceiling). Both are floored at venc's own 4096-byte cap floor
  (never command sub-floor) and ceilinged at
  `min(cap_ceiling_bytes, s·⌊256000/(1000 + rate‰)⌋)` — the §14.1
  GF(256) eligibility bound at the rung's symbol size `s`.
- Headroom seeds 1000 ‰ (= exactly the deadline-safe ceiling); all seeds
  §17 RE-DERIVE. `venc.frame_caps=false` disables cap writes while keeping
  bitrate authority.

The doc-level actuator model (commanded / effective / pending): venc applies
a 2xx `/set` synchronously, so **commanded = applied** at HTTP success; the
encoder *output* settles over ~0.5–0.75 s. §15.3 exposes the commanded
values plus `venc_settling` (true within `venc.settle_ms`, seed 750, of the
last accepted change) so consumers can distinguish a pending transition
from steady state without a second wire field.

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

### 9.10 TX-wedge watchdog (backend TX-progress liveness)

The §6.5 watchdog is RX-side; this is its TX-side sibling, run by any node
whose TX adapter is a radio (§3.0). The failure mode is real and observed:
the RTL88x2 USB TX wedge — bulk-OUT keeps accepting frames (`tx_submitted`
advances) while nothing airs, and only a physical re-plug recovers the chip.

**Trigger — progress absence, never progress deficit (Pass 11; monitor
extension Pass 49).** Devourer uses per-frame CCX TX-status reports (Pass 8).
Those reports are lossy under load *by design*: the step-11 bench measured
healthy report return rates of 100% at ≤500 pps falling to ~25% at 4500 pps,
so any deficit threshold misfires exactly when the link is busiest. A healthy
chip returned *some* reports at every measured load. Kernel-monitor has no CCX
surface; it uses the TX adapter's monotonic Linux netdev `tx_packets` counter
as the completion-progress signal. This catches a measured CU failure where
AF_PACKET `send()` returned success 915 times while `tx_packets` did not move
and no RF frame aired.

The detector evaluates one verdict per `wedge_window_ms` (seed 1000) from the
backend's `(tx_submitted, tx_progress)` counter deltas (`tx_progress` = CCX
reports on devourer, netdev TX packets on kernel-monitor):

- `Δtx_progress > 0` → not wedged (any completion proves the TX path alive);
- `Δtx_progress == 0` and `Δtx_submitted >= wedge_min_submits` (seed 8) →
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

### 9.11 FPS ladder (Pass 39, corrected Pass 53 — frame-size preservation)

FPS is the slowest, most user-visible actuator (a change costs ~0.5–1 s and a
visible cadence discontinuity), so it sits OUTSIDE the §9.1 cascade as its own
loop with much larger hysteresis. The §9 selector first holds encoder bitrate
inside the PHY budget. The FPS ladder then preserves a useful frame-aligned FEC
block size: if the measured encoded P frames become too small, fewer frames per
second at the same bitrate give each frame more bytes/source symbols and hence
more absolute repair symbols at the configured FEC ratio. `maxIBytes` and
`maxPBytes` remain live ceilings, not promises that the encoder will fill a
frame to that size. Opt-in (`venc.fps_ladder.enabled`, default false; requires
`venc.enabled` — the ladder writes `video0.fps`).

- **Ladder:** the discrete §9.6 set `{30, 45, 60, 75, 90, 100, 120, 144}`
  clipped to the configured envelope. v1 operates in `[min, preferred]`:
  `preferred` is the recovery target and nominal low-latency operating point
  (seed **100 fps**); `max` is accepted in config for forward compatibility but
  v1 never commands above `preferred`. On start the ladder commands `preferred`
  once (aligning the encoder to the envelope).
- **Measurement:** each non-IDR frame at frame-SHM ingress contributes its
  Annex-B payload bytes (the 8-byte `VencFrameMeta` prefix is excluded) to a
  fixed EWMA. IDRs are excluded because their deliberately larger size would
  falsely prove that the steady P-frame block is healthy.
- **Reduce** (toward `min`) when the measured P-frame EWMA remains below
  `min_p_frame_bytes` (seed **10000**) for `reduce_after_ms` (seed 3000),
  rate-limited by `reduce_dwell_ms` (seed 4000) between steps. One neighboring
  ladder rung per step, never past `min`.
- **Restore** (toward `preferred`) only when the current EWMA predicts that the
  next-higher rung remains healthy at the same byte rate:
  `predicted_up = ewma_bytes * current_fps / next_fps`. It must remain at least
  `min_p_frame_bytes + restore_hysteresis_bytes` (hysteresis seed **1000**) for
  `restore_after_ms` (seed 8000). Restoration is deliberately slower than
  reduction because every FPS flap is visible.
- **Stale/transition hold.** No P-frame sample within `sample_timeout_ms` (seed
  500), or an active venc bitrate/cap settling window, clears accumulated
  reduce/restore evidence and commands no FPS change. Radio loss and selector
  floor state are not direct FPS triggers; they affect the ladder indirectly
  through the PHY-safe bitrate target and resulting encoded frame size.
- **Settling:** after a command the ladder freezes for `settle_ms`
  (seed 1500; venc's live fps apply + IDR recovery is ~0.5–1 s), discards the
  pre-change EWMA, and waits for new P-frame evidence.
- **Cap coupling:** while the ladder is enabled, the §9.6 cap cadence input
  is the **commanded ladder fps** (authoritative immediately), not the
  measured cadence. Both live `maxIBytes`/`maxPBytes` ceilings are recomputed
  after every FPS step; observed P-frame size, not the derived ceiling, closes
  the ladder loop.
- All values are §17 RE-DERIVE seeds; emergency reduction (bypassing dwell
  on persistent deadline misses) is deferred until bench data motivates it.

```json
"venc": { "fps_ladder": { "enabled": true, "min": 60, "preferred": 100,
  "max": 144, "min_p_frame_bytes": 10000,
  "restore_hysteresis_bytes": 1000, "sample_timeout_ms": 500,
  "reduce_after_ms": 3000, "reduce_dwell_ms": 4000,
  "restore_after_ms": 8000, "settle_ms": 1500 } }
```

`min ≤ preferred ≤ max`, and each must be a ladder member. §15.3 exposes the
last commanded fps as link `venc_fps` (0 = never commanded), the rounded EWMA
as `venc_p_frame_bytes`, the configured floor as
`venc_p_frame_target_bytes`, and the ladder state as `venc_fps_ladder_state`.

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
  **The resolve runs only in the tx-node selector commit (Pass 43):** a
  `power_map` on an **rx-node** adapter (including the designated uplink)
  would be silently loaded and never applied, so config load REJECTS it —
  explicit beats silent. Ground-uplink power control, if gate 4 shows return
  margin problems, is a separate future ruling.
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

### 11.4a Key provenance — announced session token vs. operator secret
The `csa_psk` HMAC key (§11.4) comes from one of two sources; **HMAC is always
applied** — there is no unauthenticated-CSA mode for craft/ground. **The source
is selected solely by whether `csa.psk` is configured** — present ⇒ secret,
absent ⇒ announced token (Pass 61). There is no separate mode toggle.
- **Announced session token (default — no `csa.psk` configured).** The craft
  **auto-generates a 16-byte token `P` at boot** (io/app entropy, alongside
  `session_id`; the pure `core` layer stays RNG/clock-free and merely *verifies*
  against a supplied key) and advertises it in ANNOUNCE (§3.12, `psk_present=1`).
  A ground learns `P` off the air and keys its CSA HMAC with it. This is
  **zero-config pairing**: the token is a rendezvous credential, readable by
  anyone RF-adjacent, **not a secret**. It raises the bar only against accidental
  cross-talk and a ground that never heard the beacon; deliberate takeover is
  bounded instead by the §11.5a binding.
- **Operator secret (`csa.psk` configured).** A provisioned `csa.psk` (on craft +
  ground, the classic §11.4 posture) selects secret mode and is **never
  announced** (ANNOUNCE carries `psk_present=0`, 16 zero bytes). This restores
  genuine cryptographic authentication of the switch. The binding rules (§11.5a)
  are identical in both modes.
- **All other §11.4 acceptance guards are unchanged in both modes** — nonce
  monotonicity per `(originator, session)`, `target_chan ∈ channel_allowlist`,
  and the `csa_min_interval_s` rate-limit still hold. An accepted CSA can never
  send a craft off its configured allowlist, whatever the key source.

### 11.5 State machine (follower)
```
IDLE ─valid+MAC'd CSA─▶ ARMED ─T_switch─▶ retune+ReApplyTxPower ─▶ VERIFY
 (home_chan or         (adaptive freeze on,                          │
  persisted chan,       watchdog paused)                             ├─ valid traffic
  power-on default)                                                  │  ≤verify_timeout_ms ─▶ COMMITTED
   ▲ stale/bad-MAC/replay → drop, stay IDLE                          │                        (bound §11.5a;
   └─ REVERT → prev_chan ◀─ no valid traffic (JUMP-FAILED backout) ◀─┘                         HOLD until reboot;
      then back to IDLE                                                                         freeze lifts after
                                                                                                csa_settle_s)
```
COMMITTED is terminal until reboot — it has no automatic outgoing edge (no
mid-flight revert). The only backout is VERIFY → `prev_chan` on a failed jump.
- **Jump-failed backout (kept):** in VERIFY, no valid traffic within
  `verify_timeout_ms` (**150 ms**, bench median 85 ms + margin) → revert to
  `prev_chan` and return to IDLE. This is the **only** automatic revert; it
  protects a switch that landed on a dead channel.
- **COMMITTED holds until reboot (Pass 59).** Once committed the craft keeps the
  channel through arbitrarily long command-source outages — a ground commonly
  runs a weaker TX than the craft and may be unheard for >60 s. The former
  mid-flight `rendezvous_timeout → home` revert is **removed**. The §9.8 adaptive
  fail-safe still applies but changes only bitrate, never channel.
- **`home_chan` is a power-on default only**, no longer a mid-flight rendezvous:
  a craft holding any channel stays findable via the §15.5 scout sweep. With
  `persist_channel` (§15.2) the craft instead boots onto its last-committed
  channel; claim/bind state (§11.5a) always resets on boot regardless.

### 11.5a Command-source binding lifecycle (claim / hold / release)
An accepted CSA (§11.4) both switches the channel and **binds** its issuer as the
craft's command source — the §11.4 "currently-latched command source." This
binding *is* the claim, and it governs who may switch the craft next:
- **Bootstrap (first claim).** When the craft is unclaimed there is no bound
  source yet; the first MAC-valid CSA is accepted and binds its issuer (§11.4
  bootstrap note). Thereafter the binding is required.
- **Sticky through link loss.** While bound, a CSA from any *other* issuer is
  rejected regardless of key knowledge (this, not token confidentiality, is what
  resists casual mid-flight takeover, §11.4a). The binding is NOT dropped on
  telemetry loss.
- **Release after `bind_release_s` (90 s) of command-source silence.** The
  binding stays fresh while the craft accepts **any** packet from the bound
  issuer — CSA, NACK, LINK_REPORT, or its 1 Hz HEARTBEAT (§3.8); the ground's
  keepalive alone holds it. Only after `bind_release_s` with nothing heard from
  that issuer does the binding release: the craft flips its ANNOUNCE back to
  unclaimed and re-opens for claim. **Release changes no channel** — the craft
  stays put; only claim eligibility re-opens. This lets a rebooted or returned
  ground re-claim the craft *in place* (the orphan case: ground reboots while the
  craft holds its channel).
- **Reboot resets claim/bind state**; the craft returns to `home_chan` (or the
  persisted channel) and announces unclaimed.
- ANNOUNCE `claimed`/`claimed_by` (§3.12) reflect this state (advisory only).

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
| Forged ANNOUNCE → bogus pairing token / false claim state | ANNOUNCE is unauthenticated advertisement; a wrong `psk` only wastes one claim attempt (the elicited CSA fails the craft's §11.4 MAC); `claimed_by` is advisory-only; takeover still bounded by the §11.5a binding | 3.12, 11.4a |
| Forged CACHE_REQUEST → cache amplification (≤48 B request elicits up to `reply_limit` full symbols) | exact-`target_cache` match + per-requester rate cap + `request_id` dedup window + per-request symbol clamp; v1 IP-only keeps it off the air entirely | 3.11, 14.3 |
| Forged CACHE_REPLY → junk symbol injection | accepted only for an outstanding `request_id`, from the addressed cache, for requested symbols, within allowance; wrapped packet revalidated via full §3.1/§3.2 decode + latched stream key (no worse than direct DATA injection, which is the accepted §13 posture) | 3.11, 14.3 |
| Forged CACHE_STATUS → registry poisoning / repair misdirection | caches are operator-provisioned static endpoints; status from any other endpoint is dropped (no on-air cache discovery in v1) | 14.3 |

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
  no-loss case without any FEC decode), then the `r` repair symbols (same
  `block_id`). `END_OF_BLOCK` is on the final repair when `r > 0`, otherwise on
  the final source. Thus the TSF quiet gap begins only after parity is on air.
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

**Air-only attribution is load-bearing against §14.3 parity offload (Pass 45,
correcting the Pass 42 inference).** Cache symbols merge into the same decode
state, but source/repair arrivals retain separate air-path attribution for the
estimator. A source supplied only by a cache remains an air-path loss, and a
cache repair does not advance `repairs_emitted_so_far`. The trailing-120
maximum with censored lower bounds therefore sees the same air demand whether
or not cache repair completes the block. The Pass 42 loss sweep remains a
useful system check, but was not a structural proof: futile high-deficit blocks
kept the maximum high while an all-cache-completable distribution could drain
it to zero. Changes to attribution or estimator shape REQUIRE both
`tools/cache_offload_bench.sh` and the deterministic 120-block regression.

The next Ethernet stage may run the pure decision on TX as a **non-enforcing
runtime shadow**. It consumes fresh §3.10 feedback plus TX-local facts: exact
frame `k`, metadata-derived ARQ class, the active profile deadline, configured
shadow FEC floor/cap, queued source-transmission airtime, resend airtime, and an
authored ARQ guard. Every input and the stable §14.2 reason are observable.
Unknown transport airtime or incomplete feedback makes the decision invalid and
selects §14.1 fallback; the implementation must not manufacture a PHY rate,
RTT, deadline, or guard.

For `kernel-monitor`, the commanded HT20 MCS/GI is a TX-local fact, but Linux
does not expose a reliable per-frame RF departure timestamp. An authored
`air.airtime_efficiency_permille` (range 1–1000, default **0/off**) may therefore
enable a conservative service-rate model:
`service_kbps = HT20_PHY_kbps(mcs, gi) * efficiency_permille / 1000`.
The estimate includes the current wire bytes, per-MPDU 802.11/FCS bytes, and
any socket outbound bytes reported by `SIOCOUTQ`; `include_pending=false` keeps
the deadline-priority resend estimate independent of the live queue. A zero or
invalid efficiency keeps `airtime_unavailable` fallback. This value is an
empirical transport-efficiency calibration, not the profile airtime budget and
not permission to infer one from the other. The initial monitor rig seed is
**600 permille**, matching the measured MCS3/SGI service envelope; deployments
must re-derive it under their driver, contention, and aggregation behavior.

Shadow configuration is optional and disabled when absent:

```json
"jscc_shadow": {
  "fec_floor_permille": 20,
  "fec_cap_permille": 400,
  "arq_guard_us": 500,
  "feedback_timeout_ms": 500,
  "min_rtt_samples": 20
}
```

All five values are operator-authored measurement inputs. There are no hidden
optimistic defaults. The cap is converted per frame and then clamped by the
GF(256) limit; it is independent of the active fixed §14.1 rate. Without
`enforce`, this block authorizes observation only.

**Enforcement (Pass 38, opt-in).** `"enforce": true` inside `jscc_shadow`
turns the per-frame decision from reported to ACTUATING, with per-frame
fail-safe — every rule below applies to one frame and resets on the next:

1. **Parity:** a VALID decision's `parity_symbols` replaces the fixed §14.1
   `repair_count` for that frame, still hard-clamped by GF(256) capacity
   (`k + r ≤ 256`) and still subject to the §14.1 `min_k` ARQ-only rule.
   Any named fallback (missing/stale feedback, unready estimator, missing
   airtime or deadline) selects the fixed §14.1 rate for that frame — an
   invalid decision can never zero out authored protection.
2. **Deadline discard:** a VALID decision with `discard=true`
   (`deadline_unreachable`) drops the frame at TX before spending airtime —
   the transient-overload guard: a visible frame drop is preferred over
   queueing stale video behind newer frames. A fallback frame always
   transmits; missing data never fails toward dropping.
3. **ARQ gate:** a VALID decision with `arq_eligible=false` clears
   **`PFRAME_ARQ` only** for that frame (suppressing NACKs that cannot be
   serviced in-deadline). The IDR `ARQ` bit is never removed — I-frame
   importance outlives one frame timing window, and the §5.3 deadline gate
   already bounds late resends.

**Flip criteria (operator guidance, not code):** enable `enforce` only after
a shadow soak on the same link class shows `jscc_valid_decisions ≥ 99%` of
`jscc_decision_frames`, `jscc_repair_underpredicted_blocks` growing at
< 1% of shadow blocks, and RTT readiness held throughout — measured on the
UDP-air harness first (§17 verification order), then radio/kernel-monitor
on the rig. Enforcement telemetry is additive (§15.3):
`jscc_enforced_frames` (valid decisions actuated) and
`jscc_discarded_frames` (rule-2 drops).

### 14.3 Spatial cache repair (Cache Controller — v1 IP transport only)

Per-adapter diversity (§6) cannot decorrelate a whole-site fade — the §17
gate-2 ρ→1 tail is exactly the case where every co-located adapter fades
together. A **cache** is a spatially separated waybeam-link RX node that
latches the same stream, retains the last `blocks` blocks of raw heard
symbols, and answers bounded repair requests from an **aggregator** (a
frame-shm egress RX node, §6.3a). Cache repair is a third repair source next
to diversity and vehicle ARQ; like ARQ it is opportunistic and **not
load-bearing** (§1) — when budgets or deadlines don't fit, the block drops
exactly as it does today.

**Transport (v1 ruling):** cache traffic runs over dedicated **UDP/IP
sockets** (Ethernet, fibre, or a routed side-link between ground sites) with
**operator-provisioned static endpoints** (§15.2) — no on-air discovery, no
RF injection. It therefore consumes zero Waybeam RF airtime and adds no new
on-air attack surface. An RF cache binding (addressed injection on the shared
channel) is reserved — the §3.11 formats are transport-agnostic — but is NOT
part of v1 and is not implemented before the §17 gate-2 vehicle verdict.

**Identity + merge (rulings):** symbols keep the §2/§6.1 merge identity
`(originator, session, stream, block_id, symbol index)`; the repair source is
metadata, never a second decode path. Cache-delivered symbols feed the §6.3a
reassembler **directly** — they bypass §6.1/§6.2 per-adapter dedup, gap
detection, and the §3.7 loss estimators (a cache is not an adapter and must
not inflate `diversity`/`adapters` or perturb pre-diversity loss). Merging is
idempotent by symbol index, and the §6.3a finalized watermark stands: **a late
reply never reopens an emitted or dropped block.**

**Repair window (ruling):** §6.3a zero-block retention is unchanged. The
repair window for block `B` ends at the earliest of its §8 deadline or the
arrival of any packet of a newer block (supersession). This is the one-frame
default; a longer playout-buffer variant is **rejected** (latency-first, §9.0).

**Local-collection close.** Cache repair for block `B` may begin only when the
merged local block is still `< k` unique symbols AND the earliest of the
following has passed (all RX-local wall-clock, ms granularity, evaluated at
event-loop cadence; seeds RE-DERIVE §17):

1. `END_OF_BLOCK` seen + `tail_grace_ms` (the tail proves the burst ended);
2. `max(first_symbol + min_collect_ms, last_new_symbol + local_quiet_ms)` —
   the `min_collect_ms` floor keeps a long run of missing middle symbols from
   being mistaken for end-of-burst;
3. `first_symbol + hard_close_ms` (delayed traffic must not extend collection).

A block with zero received symbols is undiscoverable inside the one-frame
window (§6.2) and is never cache-repaired. A gap on one adapter is never a
trigger; the trigger is an incomplete **merged** block after close.

**Decision rules (per block, aggregator side):**

1. The aggregator is the only cache-request authority; caches never initiate.
2. Only the cache named by `target_cache` may answer a request.
3. Per-block transmitted-symbol cap:
   `cap = min(⌈k · repair_fraction⌉, absolute_symbol_limit)`, counting
   **requested allowances** (the aggregator cannot observe symbols lost on the
   IP path; counting requests is the conservative side).
4. `deficit = k − unique`. `deficit > cap` ⇒ the block is futile for cache
   repair: no request is sent (vehicle ARQ is unaffected, rule 8).
5. At most `max_cache_attempts` caches are addressed per block, sequentially:
   the next attempt fires only if the deficit survives `request_timeout_ms`,
   and its `missing_sources`/`repair_have` bitmaps are recomputed from the
   current merged state.
6. Eligibility: status fresh (`≤ status_timeout_ms`), same
   `(target_originator, target_session, target_stream_id)` as the latched
   stream, `rx_health_permille ≥ health_floor_permille`, and
   `oldest_block ≤ block_id` — the **newest** bound is deliberately NOT
   enforced: a status snapshot is up to one `status_interval_ms` stale, so
   the newest blocks (exactly the ones needing repair) always lie beyond the
   last reported `newest_block`; a cache that truly lacks the block answers
   with silence (§3.11) at the cost of one bounded request. `newest_block`
   is diagnostic/lag telemetry only. Ranking among eligible caches: health,
   then status freshness, then config order.
7. Repair stops the moment the block reaches `k` (the reassembler emits), and
   every outstanding request for that block is retired immediately. A later
   reply is unknown and cannot inflate accepted-symbol telemetry.
8. **Ordering ruling (bounded cache lead):** cache replies are serviced before
   §6.4 NACK construction in each event-loop iteration. After a request is
   successfully submitted to a rule-6-eligible fresh cache, only the first
   NACK for that exact stream/block is held until
   `request_send + nack_grace_ms`; cache completion during the hold suppresses
   that NACK normally. The hold is clamped to the block deadline. It never
   applies to another block/stream, a re-NACK, an ineligible/stale cache, or a
   failed request send; normal ARQ resumes immediately at expiry. `0` disables
   the lead and restores fully parallel ordering. The seed is 3 ms (validated
   range 0..6 ms), derived from real monitor-RF collection plus localhost UDP
   cache timing: first accepted reply P95 2.845 ms and cache completion P95
   2.910 ms. This spends a bounded latency slice to avoid redundant vehicle
   RF resends while retaining ARQ as the deadline-protected fallback. An RF
   cache binding must re-derive the seed because it shares channel airtime.

**Cache-node rules (§13 hardening):** a cache answers a request only when
`target_cache` equals its own `originator` and the target stream is one it
tracks with the block in window; duplicate `request_id`s from the same
requester boot identity `(originator, session_id)` inside the dedup window are
ignored; per-requester requests are
rate-limited (`max_requests_per_s`); the aggregate reply for one request never
exceeds `min(max_symbols, reply_limit)`. The aggregator accepts a CACHE_REPLY
only for an outstanding `request_id` it issued, only from the addressed cache,
only for the requested block, only for symbols it asked for (a missing source,
or a repair not marked held), and only up to the request's allowance; the
wrapped packet must pass the full §3.1/§3.2 decode and match the latched
stream key. CACHE_STATUS is accepted only from configured cache endpoints.
For each stored `stream_id`, the cache accepts only `node.preferred_originator`
when configured; otherwise the first sender latches until restart. A new
session from that same originator replaces the retained window, but a different
originator cannot flush it.

Recommended seeds (config, §15.2; RE-DERIVE §17): `tail_grace_ms 1`,
`local_quiet_ms 2`, `min_collect_ms 4`, `hard_close_ms 8`,
`request_timeout_ms 4`, `repair_fraction_permille 200`,
`nack_grace_ms 3`,
`absolute_symbol_limit 8`, `max_cache_attempts 2`, `reply_limit 4`,
`health_floor_permille 800`, `status_timeout_ms 1500`,
`status_interval_ms 500`, retention `blocks 96`, `max_requests_per_s 400`.

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
- The §14.3 **cache sockets** (`cache.repair.listen` / `cache.store.listen`)
  are control-plane UDP sockets like the §15.5 REST bind — they carry only
  §3.11 packets and count against **neither** the ≤4-UDP stream pool nor the
  shm pool.

### 15.2 Config (JSON)
```json
{
  "node":  { "originator": 17, "role": "tx", "preferred_originator": 9,
             "net_id": null },
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
    "csa":    { "psk": "<optional; auto-generated + announced when absent, §11.4a>",
                "settle_s": 3.0, "verify_timeout_ms": 150,
                "min_interval_s": 5, "ack_timeout_ms": 1000,
                "bind_release_s": 90, "persist_channel": false,
                "home_chan": 5805, "channel_allowlist": [5745, 5805, 5825] }
  },
  "air":   { "kind": "radio", "ack_responder": false,
             "wedge_window_ms": 1000, "wedge_min_submits": 8 },
  "stats": { "hz": 1, "bind": { "kind": "udp", "send": "127.0.0.1:9110" } },
  "control": { "bind": "0.0.0.0:8091" },
  "scout": { "dwell_ms": 300, "channels": null },
  "venc": { "host": "127.0.0.1:80", "enabled": false,
            "recovery_enabled": true,
            "frame_caps": true, "fps_hint": 100,
            "i_headroom_permille": 1000, "p_headroom_permille": 1000,
            "cap_ceiling_bytes": 196608, "settle_ms": 750 }
}
```
- RX nodes use `"dir":"out"` streams (UDP `send` targets) and `role:"rx"` adapters
  (diversity = same `channel`; a scout may sit on a different channel on another
  adapter).
- Every policy constant is overridable → bench re-derivation (§9, §17) is config,
  not recompile.
- `csa.psk` is present only on craft + ground configs; it MUST be excluded from
  stats and logs. It is **optional** and is the **sole** key-provenance selector
  (§11.4a, Pass 61): absent selects the auto-generated announced session token
  (`psk_present=1`); present is the operator secret (`psk_present=0`, kept off the
  air). Redaction covers the ANNOUNCE `psk` field too.
- `node.net_id` (§3.0) is `0..255`, or `null`/absent to **auto-assign** (low byte
  of `originator`, or a random `1..255`); `0` is the unassigned default. It is an
  L2 RX partition (co-located systems / channel sharing), **not** access control
  (§13). `stamp` and `filter` net_id may diverge at runtime — a ground filters
  wide while scouting and narrows to a claimed craft's `net_id`, stamping that
  same value on its uplink so a strictly-filtering craft hears it (§15.5 scout).
- `csa.bind_release_s` (**90 s**) is the command-source binding release timeout
  (§11.5a); `csa.persist_channel` (default `false`) boots the craft onto its
  last-committed channel instead of `home_chan`. The former `rendezvous_timeout_s`
  is **removed** — COMMITTED now holds until reboot (§11.5, Pass 59).
- `scout` (ground/rx node, §15.5) configures the channel searcher: `dwell_ms`
  (per-channel listen, **300 ms** — a video-active craft is seen off DATA within
  a few hundred ms) and `channels` (`null` = sweep `csa.channel_allowlist`). A
  the **`role:"tx"` uplink adapter is the scout** (resolved as the backend's tx
  adapter, not assumed to be config index 0): a sweep roams the uplink while the
  diversity RX adapters hold the resting channel. A single-adapter ground has no
  spare ear, so its sweep is mode-exclusive with an active link; a two-adapter
  ground keeps hearing the resting channel on its diversity ear while it scouts.
- A **`frame-shm` stream** carries its own per-stream `fec` block (§14.1):
  ```json
  { "stream_id": 0, "stream_type": "RTP", "dir": "in",
    "bind": { "kind": "frame-shm", "name": "venc_frame" },
    "arq_mode": "idr-only",
    "fec": { "scheme": "rlc256", "i_rate_permille": 250,
             "p_rate_permille": 100, "min_k": 3 } }
  ```
  `scheme` `"none"` (default) fragments + ARQs but emits no repair symbols;
  `"rlc256"` enables §14.1. Rates are integer per-mille (project convention). On
  a `udp` stream the `fec` block is ignored (Framer path, §5.1).
- `arq_mode` is valid only on frame-SHM ingress and is either `"idr-only"`
  (default) or the opt-in `"all-frames"` experiment from §4.1.
- A frame-SHM ingress may additionally carry the optional `jscc_shadow` block
  from §14.2. It is rejected on UDP streams. Absence keeps only the fixed §14.1
  path and emits no controller decision shadow. `"enforce": true` inside the
  block activates §14.2 enforcement (Pass 38); default false = shadow-only.
  Enforcement requires that stream's `fec.scheme` to be `"rlc256"`.
- `venc.enabled` authorizes the §9.6 bitrate actuator and therefore requires
  single-writer ownership. `venc.recovery_enabled` independently authorizes
  only §3.9 decoder-recovery IDR requests. Neither permission is implied by the
  other, and both default false.
- The §14.3 Cache Controller is configured by an optional top-level `cache`
  object; both roles default off and a node may run either or both:
  ```json
  "cache": {
    "repair": { "enabled": true, "stream_id": 0, "listen": "0.0.0.0:5802",
      "caches": [ { "originator": 33, "endpoint": "192.168.1.33:5801" } ],
      "tail_grace_ms": 1, "local_quiet_ms": 2, "min_collect_ms": 4,
      "hard_close_ms": 8, "request_timeout_ms": 4, "nack_grace_ms": 3,
      "repair_fraction_permille": 200, "absolute_symbol_limit": 8,
      "max_cache_attempts": 2, "reply_limit": 4,
      "health_floor_permille": 800, "status_timeout_ms": 1500 },
    "store": { "enabled": true, "listen": "0.0.0.0:5801",
      "stream_ids": [0], "blocks": 96, "reply_limit": 4,
      "status_to": ["192.168.1.9:5802"], "status_interval_ms": 500,
      "max_requests_per_s": 400 }
  }
  ```
  `repair.enabled` requires a non-empty `caches` list and `listen`;
  `repair.stream_id` must name a `frame-shm` egress stream. `store.enabled`
  requires `listen`; `status_to` lists the aggregator endpoints (empty =
  answer requests but send no status — such a store is never eligible under
  §14.3 rule 6). Every value is a §17-overridable seed.

### 15.3 Streaming stats (newline-delimited JSON)
Emitted at `stats.hz` to stdout and/or the stats binding. Fields map 1:1 to the
§16.2 counters (the one exception, the derived `bpf_filtered` estimate, is called
out below) plus the state an operator needs to *see* a demote, a flap-freeze, a
table mismatch, phantom diversity, a stalled adapter, or a failing return path:
```json
{ "t_ms": 172834, "node": 17, "session": 2748291,
  "adapters": [ { "name": "wlan0", "rx": 10234, "dup": 812,
    "rssi_best": -58, "rssi_mean": -63, "snr": 22, "noise": -85,
    "tx_submitted": 540, "tx_failed": 2, "tx_timeout": 0,
    "drop": 0, "filtered": 0, "kernel_drop": 0, "bpf_filtered": 0, "tsf_fallback": 0,
    "tx_reports": 531, "tx_report_fails": 0,
    "adapter_stalled": false, "tx_wedged": false } ],
  "streams": [ { "stream_id": 0, "type": "RTP",
    "seq": 90233, "delivered": 89901, "uniq": 90100, "diversity": 178342,
    "loss_prediversity_milli": 41, "loss_postdiv_prearq_milli": 6,
    "recovered_arq": 220, "recovered_fec": 0,
    "fec_recovered_source_symbols": 0,
    "arq_recovered_source_symbols": 220,
    "arq_recovered_repair_symbols": 0,
    "frames_with_arq": 187, "frames_fec_only": 0,
    "frames_fec_after_arq": 0,
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
    "jscc_decision_frames": 89571, "jscc_valid_decisions": 89200,
    "jscc_fallback_decisions": 371, "jscc_decision_valid": true,
    "jscc_fallback": "none", "jscc_reason": "fec_and_arq",
    "jscc_input_k": 38, "jscc_input_predicted_symbols": 5,
    "jscc_input_floor_symbols": 1, "jscc_input_cap_symbols": 16,
    "jscc_input_deadline_us": 16667, "jscc_input_source_tx_us": 5210,
    "jscc_input_rtt_p95_us": 2000, "jscc_input_resend_us": 116,
    "jscc_input_guard_us": 500, "jscc_output_parity_symbols": 5,
    "jscc_output_remaining_us": 11457,
    "jscc_output_arq_eligible": true, "jscc_output_discard": false,
    "jscc_feedback_epoch": 1821, "jscc_feedback_age_ms": 42,
    "jscc_enforced_frames": 0, "jscc_discarded_frames": 0,
    "shm_full_drops": 0, "shm_oversize_drops": 0, "shm_bad_slots": 0,
    "dropped_superseded": 110, "dropped_deadline": 8,
    "nacks_sent": 18,
    "nack_rtt_hist": [0,2,7,6,2,1,0,0], "nack_rtt_max_ms": 34,
    "arq_rec_hist": [0,1,6,6,3,1,1,0], "arq_rec_max_ms": 61,
    "resends_sent": 230, "double_send_suppressed": 5,
    "source_symbols_sent": 4120300, "repair_symbols_sent": 358944,
    "fec_oversize_frames": 0, "idr_frames": 17, "arq_frames": 68342,
    "arq_cutoff_frames": 0,
    "decode_errors": 0, "active_profile": 4, "table_version": 178 } ],
  "arq_timing": {
    "eob_to_nack_build": { "samples": 18, "p95_us": 820, "max_us": 901 },
    "nack_build_to_inject": { "samples": 18, "p95_us": 4, "max_us": 7 },
    "nack_inject_to_retransmit": { "samples": 18, "p95_us": 2510, "max_us": 3100 },
    "nack_build_to_retransmit": { "samples": 18, "p95_us": 2514, "max_us": 3107 },
    "nack_receive_to_resend": { "samples": 18, "p95_us": 315, "max_us": 402 } },
  "return": { "reports_expected": 10, "reports_received": 9,
    "reports_rejected": 0,
    "return_window_hits": 7, "return_window_misses": 2,
    "unicast_sent": 0, "unicast_fallback": 0 },
  "link": { "target_originator": 9, "target_session": 183726,
    "profile": 4, "mcs": 4, "tx_power_qdb": 1800,
    "report_epoch": 1822, "report_age_ms": 40,
    "state": "HOLD", "flap_freeze": false, "csa_state": "IDLE",
    "venc_bitrate_kbps": 14000, "venc_max_i_bytes": 70000,
    "venc_max_p_bytes": 19444, "venc_pushes": 6, "venc_failures": 0,
    "venc_settling": false, "venc_fps": 90 } }
```
The `venc_*` link fields are the §9.6 actuator state: the last COMMANDED
bitrate and frame caps (0 = never pushed), cumulative pushes/failures, and
`venc_settling` — true within `venc.settle_ms` of the last accepted change
(the doc-model "pending transition"; commanded = applied at HTTP 2xx since
venc's `/set` is synchronous). Zero/false on nodes without `venc.enabled`.

`return_window_hits/misses` (TX-side) and `reports_expected/received` expose the
§7.2 optimisation's health directly, and `adapter_stalled` + the
`loss_prediversity` vs `loss_postdiv_prearq` pair expose phantom diversity and the
ρ decorrelation gauge — the two field-failure modes the design most fears.
`tx_wedged` is the §9.10 backend-progress verdict (TX adapter only): CCX
TX-status progress on devourer and Linux netdev `tx_packets` progress on
kernel-monitor. `tx_reports` itself remains CCX-only and stays 0 on monitor;
the monitor netdev counter is used internally rather than mislabelled as CCX.
`uniq`/`diversity` are the §17 gate-2 estimator inputs; the `nack_rtt_*` /
`arq_rec_*` histograms (cumulative, ms upper bounds 1,2,4,8,16,32,64,+inf) are
the §17 gate-3 estimator outputs.
The top-level `arq_timing` phase metrics are microsecond-domain, cumulative
sample count/max plus a bounded trailing-window P95. Ground populates
EOB→NACK-build, NACK-build→submission, submission→retransmit-arrival, and the
combined build→arrival; the vehicle populates NACK-receipt→resend-submission.
Each metric is host-local and therefore makes no cross-host clock assumption.

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

Recovery-method comparison uses the successful-frame attribution counters, not
`recovered_arq` versus `recovered_fec`: those legacy fields have different
units (`recovered_arq` is packet-sequence gaps filled; `recovered_fec` is whole
frames decoded). `fec_recovered_source_symbols` is the number of absent source
rows reconstructed by successful FEC decodes. `arq_recovered_source_symbols`
and `arq_recovered_repair_symbols` count unique rows first admitted with
`RETRANSMIT` that contributed to a subsequently delivered frame; duplicates and
rows belonging to lost frames do not count. `frames_with_arq` counts delivered
frames that used at least one such source or repair row. `frames_fec_only` and
`frames_fec_after_arq` partition `recovered_fec` into FEC decodes without and
with contributing retransmitted rows, respectively. On the all-source fast
path, queued repair rows do not contribute and therefore do not affect these
counters. Stats reset clears all six attribution counters.

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

The `jscc_decision_*`, `jscc_input_*`, `jscc_output_*`, and
`jscc_feedback_*` fields are TX-side §14.2 runtime-shadow telemetry. Frame and
valid/fallback counts are cumulative; the remaining fields describe the most
recent frame evaluation. `jscc_decision_valid=false` means §14.1 remained the
only decision and `jscc_fallback` names why: `feedback_missing`,
`feedback_stale`, `repair_not_ready`, `rtt_not_ready`,
`airtime_unavailable`, or `deadline_unavailable`. A valid decision reports
fallback `none`, one stable §14.2 reason, every numeric input, chosen parity,
remaining time, and ARQ/discard outputs. These outputs are hypothetical and do
not alter transmitted symbols. Fields are zero/empty on RX and on streams
without `jscc_shadow`.

`nack_rtt_samples` and `nack_rtt_p95_us` accompany the existing cumulative RTT
histogram on RX. They describe the bounded trailing sample window used in
§3.10; zero samples means the P95 is unavailable. Stats reset clears the RTT
window and therefore clears JSCC RTT readiness.

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
`bpf_filtered` is a **derived estimate** — the only §15.3 field that does *not*
map 1:1 to a §16.2 counter — of how many frames the kernel-monitor backend's
§3.0 BPF pre-filter rejected before the `recvmsg()` copy to userspace. It is
computed as the interface's sysfs `rx_packets` delta since socket open minus all
userspace-observed frames (`rx + filtered + drop + kernel_drop`), floored at 0.
Because `rx_packets` is a per-netdev driver counter incremented *below* the
packet-socket/BPF layer, this estimate also absorbs frames the driver delivered
to *other* sockets on the same monitor interface (e.g. a concurrent `tcpdump`)
and any driver-level accounting skew — it is a coarse health gauge of pre-filter
efficacy, **not** an exact filter-drop count. It is 0 on the `RadioAir` and
`UdpAir` backends, where the BPF pre-filter does not apply. The §3.0
`dot11_parse()` userspace check remains the correctness gate regardless.

A node with a §14.3 cache role enabled additionally emits the matching
top-level object (absent when the role is off, like `stats.bind`):

```json
"cache_repair": { "requests": 12, "replies": 11, "symbols_accepted": 18,
  "symbols_rejected": 0, "blocks_closed_deficit": 9, "blocks_repaired": 7,
  "blocks_futile": 1, "requests_suppressed": 2, "caches_fresh": 2,
  "nack_graces_armed": 10, "blocks_repaired_before_nack": 6,
  "request_to_first_reply": { "samples": 11, "p95_us": 1800,
    "max_us": 2400 },
  "request_to_completion": { "samples": 7, "p95_us": 2600,
    "max_us": 4100 } },
"cache_store": { "requests_received": 12, "requests_answered": 11,
  "requests_rejected": 1, "symbols_sent": 18, "status_sent": 240,
  "blocks_held": 96, "health_permille": 971 }
```

`blocks_repaired` counts blocks that reached `k` during a cache-reply merge
(completion attribution); `blocks_futile` counts §14.3 rule-4 skips;
`requests_suppressed` counts eligibility failures (stale/unhealthy/no window);
`nack_graces_armed` counts exact-block first-NACK holds successfully installed,
and `blocks_repaired_before_nack` counts cache-attributed completions for which
that block had emitted no NACK;
`caches_fresh` and `blocks_held`/`health_permille` are gauges. Stats reset
zeroes the counters and leaves the gauges live. Cache timing uses the
aggregator host's monotonic microsecond clock: `request_to_first_reply` starts
only after a request is successfully submitted and ends at its first accepted
reply; `request_to_completion` starts at the first successfully submitted
request for a block and ends only when a cache-reply merge completes that
block. `samples`/`max_us` are cumulative since reset and P95 is nearest-rank
over the trailing 512 samples. Requests or blocks crossing a stats reset do
not contribute partial intervals. No cross-host clock synchronization is
implied.

On frame-SHM TX ingress, `source_symbols_sent` and `repair_symbols_sent` are
the exact cumulative §14.1 symbols emitted by `FrameFramer`;
`fec_oversize_frames` counts frames sent source-only because `k+r` exceeded
GF(256) capacity; `idr_frames` counts frames whose VFRM metadata carried the
IDR flag; and `arq_frames` counts frames stamped with either ARQ-class flag.
They are zero on RX and non-frame-SHM streams. These counters are
the fixed-policy baseline for comparing hypothetical JSCC shadow parity; byte
or bitrate inference is not an acceptable substitute.

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
(never blocks). A waybeam-link egress producer sets the object mode to `0666`
after creation, independent of its process umask, so an unprivileged local
viewer can attach to a ring created by a privileged monitor-radio process.
The consumer needs read/write access because it owns `read_idx` and
`consumer_waiting`; read-only attachment is not compatible with this SPSC ABI.
All fields native-endian (same-host only).

**Slot payload** = 8-byte `VencFrameMeta` prefix + Annex-B frame bytes (NAL start
codes preserved):

| off | size | field | notes |
|---|---|---|---|
| 0 | 4 | `pts` | u32; encoder capture timestamp (SDK units), truncated |
| 4 | 1 | `codec` | u8; `0x01` = H.265 (only value emitted) |
| 5 | 1 | `flags` | u8; bit 0 = IDR, bit 1 = GDR active, bit 2 = SVC-T enhancement layer; other bits reserved 0 |
| 6 | 1 | `gdr_pos` | u8; zero-based GDR cycle position, 0 when inactive |
| 7 | 1 | `gdr_len` | u8; GDR cycle length, 0 when inactive; when active `gdr_pos < gdr_len` |
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
| `GET /api/v1/discovery` | bounded passive discovery: `{nodes:[], streams:[]}` from HEARTBEAT/ANNOUNCE/DATA observations |
| `GET /api/v1/scout/results` | current scout state: `{scanning, current_chan, channels:[], candidates:[]}` (§15.5a; ground/rx node) |

`GET /api/v1/discovery` is read-only and node-local. `nodes[]` contains
`{originator,session,last_seen_ms}` for HEARTBEAT, ANNOUNCE, or DATA senders;
ANNOUNCE (§3.12) senders additionally carry advisory `claimed`/`claimed_by` and a
`psk_present` bool (the announced token is public and may also be surfaced, Pass 63;
`/discovery` reports the bool, the claim path uses the cached token). `streams[]`
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
| `POST /api/v1/bench/rx-drop` | `{ "permille": 0 }` | UDP-air bench RX only: retune independent synthetic loss per listener (0–1000); 409 on RF backends |
| `POST /api/v1/scout/start` | `{ "channels":[…]?, "dwell_ms":??, "mode":"list"\|"quickconnect", "target":{"originator":N}? }` | begin a channel sweep (§15.5a; ground/rx node) |
| `POST /api/v1/scout/stop` | `{}` | end the sweep and hold the current channel |
| `POST /api/v1/scout/quickconnect` | `{ "originator":N, "target_chan":?? }` | claim a discovered craft onto `target_chan` (or the emptiest allowlisted channel) |

Endpoints act only where meaningful — `csa` on the issuer, `link/profile` and
`fec` on the TX, and `bench/rx-drop` only on UDP-air RX. An endpoint invoked in a mode where it does not apply returns
**409**; an unknown path **404**; a malformed or oversize body **400**. The
write knobs are exactly the §9/§11/§14 levers that were previously boot-time
JSON only; the profile pin is the operating-point (MCS + bitrate) lever, since
a profile bundles rate/power/MTU per §9.3. The `scout/*` endpoints (§15.5a) act
only on a ground/rx node and **409** elsewhere.

### 15.5a Ground scout (channel searcher)
A ground-side finder that sweeps channels to discover parked/flying craft and,
optionally, CSA-claim one (§11) — the inverse of the §11.5 follower. One sweep
engine backs two entry points; single- and dual-adapter grounds are both
supported (§15.2 `scout`).

- **Sweep + discovery.** `scout/start` retunes the **uplink (`role:"tx"`) adapter**
  across `channels` (or `csa.channel_allowlist`), dwelling `dwell_ms` per channel
  and aggregating the §15.5 passive-discovery view. For claim, the first heard
  candidate suffices — the §2 admission count (`N_admit`/`T_admit`) is the
  anti-flood gate for the *latch picker*, not a barrier to a deliberate operator
  claim. During a sweep the scout adapter ignores its `net_id` filter (hears all
  net_ids). On a two-adapter ground the diversity RX adapters stay on the resting
  channel, so an active link there survives the sweep; a single-adapter ground has
  no spare ear and drops any active link while scouting. **The survey (per-channel
  occupancy and candidates) is derived from the scout adapter's frames only**: a
  diversity ear parked on the resting channel hears the craft during every dwell, so
  counting its frames would attribute the craft to every swept channel and inflate
  occupancy. Frames from non-scout adapters are excluded from the survey.
- **Candidate** = `{originator, net_id, session, claimed, claimed_by, chan,
  psk_known}`. `psk_known` is a bool reporting whether the ground holds a usable
  CSA key for the craft: the **cached announced token** (the ground caches the
  ANNOUNCE `psk` from every beacon, Pass 63) or a configured `csa.psk` secret. The
  token is public and may be surfaced, but ownership is still *proven by
  connecting*, not read from the beacon: a MAC-valid CSA the craft follows is the
  proof.
- **Per-channel occupancy** is reported as a record whose field set is a superset
  aligned with the Realtek "Advanced Channel Scanning" survey so a future
  hardware backend is a field-fill, not a reshape. v1 fills only the
  packet-derivable fields from monitor RX over the dwell; units are **per-mille**
  and **dBm**:

  | field | v1 source | later (hardware ACS) |
  |---|---|---|
  | `wifi_util_permille` | decodable-frame airtime estimate | CLM Wi-Fi portion |
  | `util_permille` | = `wifi_util` (Wi-Fi only in v1) | total incl. non-Wi-Fi |
  | `interference_util_permille` | `null` (unmeasurable from packets) | CLM−Wi-Fi |
  | `noise_dbm` | RSSI-floor proxy (idle/min `DBM_ANTSIGNAL`) | NHM true noise floor |
  | `bss_count` | distinct BSSID/SA transmitters heard | ACS BSS count |
  | `quality_permille` / `availability_permille` | derived from the above | ACS direct |

  The candidate craft's own traffic is excluded from its channel's counts.
- **Quick-connect / claim.** `scout/quickconnect` (or `mode:"quickconnect"` with a
  `target`) claims a craft: set `stamp`+`filter` net_id to the craft's, retune
  **all** link adapters onto the craft's current channel (not just the uplink — so
  every diversity ear hears the campaign, and the §11.6 intra-process commit that
  already moves all adapters lands them together), `POST`-equivalent a §11 CSA to
  the chosen `target_chan` (or the emptiest allowlisted channel) keyed with the
  craft's `psk` (announced §11.4a, or configured secret), and confirm the §11.6
  `CSA_ARMED` ACK. A **failed** campaign (no `CSA_ARMED`) rolls every adapter and
  the net_id stamp/filter back to the resting state — the ground's operating
  channel (its configured channel, or the last committed target, tracked at
  runtime) — so a failed claim is a clean no-op rather than leaving a diversity ear
  stranded off the resting channel. Likewise the scout returns *all* adapters to the
  resting channel when a sweep ends or is stopped. On success the ground holds the
  target channel and does **not** auto-rescout on link loss (matching §11.5
  hold-until-reboot); re-scout is an explicit action.

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
| cache close timers (`tail_grace_ms`/`local_quiet_ms`/`min_collect_ms`/`hard_close_ms`) | §14.3 local-collection close | loss-position sweep at target fps on the Ethernet bench; close must beat next-block supersession with round-trip margin |
| frame-cap headrooms (`i/p_headroom_permille`, `cap_ceiling_bytes`, `fps_hint`) | §9.6 horizon caps | UDP-air actuation harness FIRST (fake venc, profile transitions — operator sequencing 2026-07-16), then the radio/kernel-monitor backends on the rig |
| FPS ladder frame floor/hysteresis/timers (`min_p_frame_bytes`, `restore_hysteresis_bytes`, `sample_timeout_ms`, `reduce_after/reduce_dwell/restore_after/settle_ms`) | §9.11 frame-size-preservation loop | UDP-air frame-size ladder harness first; flight calibration against direct frame-SHM cadence and visual output |
| `arq_max_fps` | §4.1 high-cadence ARQ cutoff | operator comfort floor 10 ms (2026-07-16); re-derive against gate-3 recovery latency at high fps |

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
- **Dynamic 20/40 MHz channel width — v1 is fleet-wide 20 MHz (operator
  ruling, Pass 40).** The craft is pinned by the 8812EU 40 MHz bug (§7.2),
  width is a fleet property under same-channel diversity (§1), and measured
  monitor-mode retunes (§11.2) rule out an "instant" width actuator. 40 MHz
  returns, if ever, as a CSA-shaped campaign behind a hardware verdict
  (review-log register R-D).
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
