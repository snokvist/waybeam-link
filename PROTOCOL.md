# waybeam-link — Protocol Specification (v0 draft)

A best-effort broadcast video tunnel with opt-in, importance-gated retransmission,
built on per-adapter receive diversity. Sits on top of OpenIPC **devourer** for
raw 802.11 monitor/injection; adds session, sequencing, dedup, gap-detection and
opportunistic ARQ. RTP is carried opaque end-to-end.

---

## 1. Scope and position in the stack

```
  ┌─────────────────────────────────────────────┐
  │ encoder / decoder (gstreamer, ffmpeg)        │  RTP/UDP on loopback
  ├─────────────────────────────────────────────┤
  │ waybeam-link                                 │  THIS SPEC
  │   TX: framer + resend ring + scheduler       │
  │   RX: merge/dedup + gap-detect + NACK gen    │
  ├─────────────────────────────────────────────┤
  │ devourer (vendored)                          │  raw 802.11 inject / monitor
  ├─────────────────────────────────────────────┤
  │ RTL8812AU/EU radio(s)                        │  same-channel only
  └─────────────────────────────────────────────┘
```

**Invariants carried down from design discussion (do not revisit):**

- RTP is opaque on the wire. The transport core never parses it. The *only*
  RTP/codec awareness lives in the RTP **profile** (framer classifier), isolated.
- Reliability is **not load-bearing.** Per-adapter diversity is the primary
  redundancy. ARQ is an opportunistic patch for the short correlated-fade band
  (~5–30 ms nulls/occlusions). Under saturation ARQ quietly does less and the
  link degrades toward pure diversity — the accepted floor.
- Single logical ground receiver: all local adapters feed **one** merge/NACK
  state machine → one NACK generator → no implosion, no suppression logic.
- No NACK authentication. No time synchronisation between TX and RX (all
  deadlines are RX-local wall-clock).
- Same-channel diversity only (single air radio). Decorrelation levers are
  spatial / antenna / polarisation, not frequency.
- Injected/broadcast frames get **no** 802.11 MAC ARQ. Application-level resend
  is the only retry logic; nothing underneath retries. CSMA carrier-sense
  contention still applies (the half-duplex cost).

---

## 2. Identifiers and session model

| id           | width | scope / meaning                                             |
|--------------|-------|-------------------------------------------------------------|
| `session_id` | u32   | Random per TX boot. Latch handle; namespaces seq/block.     |
| `stream_id`  | u8    | Instance index; disambiguates multiple streams in a session.|
| `stream_type`| u8    | Semantic kind of the stream → selects profile (§3.4, §4).   |
| `seq`        | u32   | Monotonic per (session,stream). Global order + dedup key.   |
| `block_id`   | u32   | Monotonic per (session,stream). One block = one RTP frame.  |

- **Discovery / latch:** RX in monitor mode passively observes DATA packets and
  enumerates `(session_id, stream_id, stream_type)` tuples present on the
  channel. It can route/subscribe by **type** ("all telemetry") or by a specific
  instance without ever parsing payload, then latch the desired stream(s). No
  handshake, no association. RX caches `(session_id, stream_id) → stream_type`.
- **Startup floor:** on latch, RX adopts the first-seen `seq` as its floor and
  never NACKs below it (no back-filling history on join).
- **Multi-stream:** distinct `stream_id` under one session, or distinct sessions.
  Each stream has its own seq/block namespace and its own profile.
- **Teardown:** implicit. If nothing is heard for a session within an idle
  timeout, RX drops its state. No explicit close on the wire.
- **seq width:** u32 — no wrap within a flight, so plain integer comparison; no
  serial-number arithmetic needed.

---

## 3. Wire header

Carried as the payload of the 802.11 data frame that devourer injects. devourer
owns radiotap + 802.11 MAC header; waybeam-link owns everything below.

### 3.1 Common prefix (all packet types)

| field        | width | notes                                             |
|--------------|-------|---------------------------------------------------|
| `magic`      | u8    | fixed sync byte, e.g. `0xWB` (protocol guard)     |
| `ver_type`   | u8    | high nibble = version, low nibble = packet type   |
| `session_id` | u32   |                                                   |
| `stream_id`  | u8    |                                                   |

`packet type`: `0x1 = DATA`, `0x2 = NACK`. (`0x3+` reserved: heartbeat, etc.)

### 3.2 DATA packet

| field         | width | notes                                                    |
|---------------|-------|----------------------------------------------------------|
| *common*      | —     | as §3.1                                                  |
| `stream_type` | u8    | semantic kind → profile selector (§3.4); on every packet |
| `seq`         | u32   | per-stream monotonic                                     |
| `block_id`    | u32   | per-stream monotonic; RTP frame boundary                 |
| `flags`       | u8    | see below                                                |
| `payload_len` | u16   | length of opaque payload                                 |
| `payload`     | var   | opaque RTP bytes (never parsed by core)                  |

`flags` bits:

| bit | name           | meaning                                                     |
|-----|----------------|-------------------------------------------------------------|
| 0   | `END_OF_BLOCK` | last packet of this block                                   |
| 1   | `ARQ`          | block is retransmit-eligible (importance / opt-in)          |
| 2   | `RETRANSMIT`   | this packet is itself a resend (stats/diagnostics only)     |
| 3–7 | reserved       | 0                                                           |

**Critical rule — redundant per-packet metadata:** `stream_type`, `block_id`,
`END_OF_BLOCK` membership, and the `ARQ` flag are stamped on **every** packet of
a block, not just the first. If the boundary packet is lost, a surviving packet of the next
block still reveals the boundary; if the first packet of an ARQ block is lost, a
survivor still tells RX the block is retransmit-eligible so the lost early seqs
can be NACKed.

### 3.3 NACK packet

| field         | width | notes                                                     |
|---------------|-------|-----------------------------------------------------------|
| *common*      | —     | as §3.1 (echoes `session_id`,`stream_id` being repaired)  |
| `base_seq`    | u32   | anchor for the bitmap                                     |
| `bitmap_len`  | u8    | number of bytes of bitmap following                      |
| `bitmap`      | var   | SACK-style; bit *i* set = `base_seq + i` is missing       |

- NACK references **seqs**, not blocks (missing units are packets).
- RX includes a missing seq **only if** its block is still live: `ARQ`-flagged,
  not superseded (§6), and not past deadline (§8). Non-ARQ and superseded seqs
  are never listed.
- No deadline field: TX applies its own resend deadline independently. No clocks
  cross the link.

---

### 3.4 Stream type registry

`stream_type` is a small registry; each value maps to exactly one profile (§4).

| value       | name        | profile                    | notes                          |
|-------------|-------------|----------------------------|--------------------------------|
| `0x00`      | UNKNOWN     | best-effort (default)      | unspecified / reserved         |
| `0x01`      | RTP         | RTP profile                | video; NAL classifier applies  |
| `0x02`      | TELEMETRY   | best-effort or app-defined | MAVLink/MSP-style, small pkts  |
| `0x03`      | CONTROL     | app-defined                | reserved for RC/command        |
| `0x10–0xEF` | user        | build-defined              | experimental / vendor          |
| `0xF0–0xFF` | reserved    | —                          |                                |

**Unknown-type rule (forward compatibility):** an RX that sees a `stream_type`
it does not recognise MUST treat the stream under the **best-effort default
profile** — deliver by diversity, never NACK, no supersession/deadline logic.
A new type on the air can therefore never make an older client misbehave.

---

## 4. Block model and profiles

A **block** is a set of packets sharing a `block_id`, delimited by
`END_OF_BLOCK`. For the RTP profile, one block = one RTP frame (marker/timestamp
boundary).

The active profile is selected on the wire by `stream_type` (§3.4) — both ends
apply the matching policy with no out-of-band agreement. A **profile** binds
payload semantics to these policies:

| policy          | RTP profile                                        | future bulk/file profile     |
|-----------------|----------------------------------------------------|------------------------------|
| boundary        | RTP frame (marker bit / timestamp change)          | fixed-size or app-defined    |
| supersession    | newer block ⇒ drop older incomplete (deadline)     | ignored (no deadline)        |
| ARQ eligibility | set per-block by codec classifier (§4.1)           | always, or app-defined       |
| deadline        | per-block wall-clock budget; longer for I-frames   | none (NACK until delivered)  |

The transport core is profile-agnostic: it carries `block_id` + `flags` and
executes whatever the active profile's policy object dictates.

### 4.1 RTP profile — ARQ classification (the bounded codec reach)

The RTP framer sets the `ARQ` flag per block using a shallow classifier that
reads **only the NAL unit type** from the start of the RTP payload:

- **H.264:** IDR / coded-slice-of-IDR (and parameter sets SPS/PPS) ⇒ `ARQ=1`.
  Non-IDR coded slices ⇒ `ARQ=0`.
- **H.265:** IDR_W_RADL / IDR_N_LP / CRA and VPS/SPS/PPS ⇒ `ARQ=1`; else `0`.
- Handle aggregation/fragmentation units (STAP/FU) enough to find the contained
  NAL type — no deeper parsing.

**Pure-agnostic fallback** (if NAL awareness is unwanted): classify by block
size — blocks above an adaptive size threshold are treated as important. Cruder,
occasional misclassification, but zero codec coupling. Selectable per build.

**Deadline coupling:** `ARQ`-important blocks may carry a longer retransmit
deadline than best-effort blocks, because a slightly-late I-frame can still
rescue the GOP that references it, whereas a late P-frame rescues only itself.

---

## 5. TX behaviour

### 5.1 Framer (per stream)
1. Read RTP/UDP datagram from loopback ingress socket.
2. Assign current `block_id`; detect RTP frame boundary → increment block_id and
   set `END_OF_BLOCK` on the last packet of the finished block.
3. Run profile classifier → set `ARQ` flag for the block.
4. Assign monotonic `seq`; build DATA header; hand frame to devourer for
   injection.
5. Push (seq → payload, block_id, flags, first-seen-tx-time) into the resend
   ring.

### 5.2 Resend ring
- Holds recently sent packets for a bounded window (~50 ms; size is generous,
  ~125 KB at 20 Mbps).
- Lookup by `seq` for NACK service.
- Eviction by age; a packet older than its deadline is dropped from the ring.

### 5.3 Scheduler / priority
- **Live packets strictly highest priority.** Retransmits are strictly lower.
- **Airtime cap:** retransmits limited to a hard fraction of downlink airtime per
  interval, so recovery can never starve live video.
- **Per-seq hold-down:** after resending seq *N*, suppress re-resending it for a
  short window so a second in-flight NACK (or one that crossed the resend) does
  not double-send.
- **Importance gate:** only blocks with `ARQ=1` are ever resent. A NACK for a
  non-ARQ seq is ignored (should not occur — RX must not list them).
- **Deadline gate:** never resend a seq past its deadline; drop instead.
- **Attempt cap:** bound retransmit attempts per seq; past the cap, give up.
- Mark every resend with `RETRANSMIT=1`.

---

## 6. RX behaviour (single merged state machine)

All adapters feed one pipeline. Per adapter, receive order == air order (no
intra-adapter reordering). Reordering exists **only across adapters** and is
host-side delivery jitter (USB URB timing, driver batching, scheduler) — not
propagation skew.

### 6.1 Ingest + dedup
- For each received DATA packet, key on (session,stream,seq).
- First copy wins; later duplicates from other adapters are counted and dropped.
- Maintain per-adapter "highest delivered seq" for the short-circuit below.

### 6.2 Gap detection with short-circuits (in priority order)
A missing seq is declared **lost** (→ NACK-eligible) as soon as *any* of:

1. **All-adapters-advanced (fast path):** every latched adapter has delivered a
   seq greater than the gap ⇒ none of them heard it ⇒ lost immediately, no wait.
2. **Block supersession (RTP profile):** a newer `block_id` has any received
   packet ⇒ every older incomplete block is past deadline ⇒ its missing seqs are
   lost, and are **not** NACKed (superseded). This simultaneously advances the
   delivery cursor past the hole (kills head-of-line blocking) and suppresses
   pointless NACKs.
3. **Dwell ceiling (rare backstop):** an adapter has gone silent (delivered
   nothing ≥ the gap), so 1 cannot fire, and the stream is still live so 2 has
   not fired. After a fixed dwell ms, declare lost. This is the only timer path
   and is almost never hit.

### 6.3 Delivery
- Deliver in-order, best-effort, out the loopback egress socket as untouched RTP.
- "Drop" means *stop recovering + advance cursor* — never withhold packets
  already held. Let the decoder's jitter buffer discard genuinely-late arrivals.

### 6.4 NACK generation
- For a lost seq that is `ARQ`-flagged, not superseded, and within deadline:
  add to the pending SACK set.
- Coalesce pending misses into a NACK bitmap anchored at `base_seq`.
- Send NACK via the **designated TX adapter** (one adapter appointed for uplink;
  its RX blind spot while transmitting is covered by the other diversity
  adapters, so ground half-duplex is free).
- Re-NACK policy: bounded retries with backoff; stop on receipt (RETRANSMIT
  arrives) or on deadline/supersession, whichever first.

---

## 7. Air-side uplink transport (v1 decision)

**Shared-channel, best-effort.** NACKs are injected back over the same wifi via
devourer; no separate backchannel, no ELRS. Accepted consequences:

- The air radio hears NACKs only in the gaps between its own injected packets →
  higher downlink duty cycle = smaller NACK window. Not fixed, just known.
- Under heavy loss NACKs spike when the channel is most saturated, so ARQ
  self-throttles and degrades toward pure diversity. Benign because ARQ is
  non-load-bearing.
- **Feedback self-congestion** is the one thing actively guarded: retransmit <
  live priority, airtime cap as a hard downlink fraction, resend attempt cap,
  per-interval bound. A burst needing more than a few repairs is past saving —
  let RTP concealment eat it.

The uplink is a **pluggable transport** in the design so a dedicated backchannel
(2nd air adapter / 900 MHz) can replace it later without touching the core.

---

## 8. Deadline and retry semantics

- Each block gets a first-seen wall-clock timestamp at RX (and at TX for its
  ring). Deadline = first-seen + budget(profile, importance).
- RX: never NACK a seq past deadline. TX: never resend past deadline.
- No clock crossing — TX and RX each apply their own local budget; they need not
  agree to the millisecond because the ring window and NACK window overlap
  generously.
- Importance-longer deadline for `ARQ` I-frame-class blocks (§4.1).

---

## 9. All-in-one binary and verification loop

A single portable binary **vendors devourer** and adds waybeam-link. Modes:

### 9.1 Modes
- `tx` — bind loopback UDP ingress (RTP in) → framer → inject via devourer.
- `rx` — open N adapters in monitor via devourer → merge/dedup/gap/NACK →
  loopback UDP egress (RTP out). One adapter flagged as designated NACK TX.
- `loopback` — TX and RX in one process for bench verification (below).

### 9.2 Bench verification (no flying)
Goal: prove protocol correctness and measure the three empirical knobs on real
hardware, deterministically.

1. **Source:** `gst videotestsrc → H.264/H.265 → RTP → UDP loopback` into `tx`.
   A moving test pattern with visible frame counter.
2. **Sink:** `rx` egress `→ UDP → gst → display`, plus decode-error overlay.
3. **Synthetic loss injector:** at RX ingest, before merge, drop packets by a
   configurable model:
   - uniform random `p`;
   - **burst** model (Gilbert-Elliott) to emulate correlated fades — the regime
     ARQ actually targets;
   - **per-adapter independent** drop to exercise diversity recovery;
   - **correlated across adapters** to confirm ARQ degrades gracefully (does
     *not* rescue, must not thrash).
4. **Counters (per run):** injected, received/adapter, deduped, recovered-by-
   NACK, dropped-superseded, dropped-past-deadline, NACKs sent, resends sent,
   double-send-suppressed, decode errors.
5. **Assertions for a passing run:**
   - independent per-adapter loss below single-adapter rate ⇒ near-zero decode
     errors from diversity alone, few/no NACKs;
   - moderate burst loss on ARQ (I-frame) blocks ⇒ recovered-by-NACK > 0, decode
     errors near zero, resend airtime under the cap;
   - non-ARQ (P-frame) loss ⇒ zero NACKs for those seqs, concealment handles it;
   - correlated-all-adapter fade ⇒ ARQ attempts bounded, no self-congestion
     collapse, recovers to clean stream after the fade.
6. **Knob extraction:** the injector + counters yield the three tuning values
   directly (§10).

### 9.3 Field bring-up
Same binary: `tx` on the craft (RTP from the real encoder), `rx` on the ground
with the real adapter set. Watch the same counters live.

---

## 10. Empirical knobs (measure, not design)

| knob                       | meaning                                             | how measured                              |
|----------------------------|-----------------------------------------------------|-------------------------------------------|
| dwell ceiling (ms)         | §6.2-3 backstop before declaring lost on silence    | cross-adapter delivery jitter histogram   |
| retransmit airtime fraction| hard cap on resend airtime vs downlink              | raise until live-video jitter appears      |
| deadline budget (ms)       | per-profile; longer for I-frame-class               | glass-to-glass budget minus pipeline delay |

---

## 11. Out of scope / accepted limitations

- No FEC (deliberately — diversity is the redundancy layer).
- No NACK authentication (spoof risk accepted).
- No time sync; no IP/ARP/AP/STA semantics on the wire.
- No frequency diversity (single air radio).
- No importance beyond the single ARQ bit (I-vs-P granularity only).
- Correlated fades that beat diversity also beat ARQ (retransmit rides the same
  faded channel) — ARQ's real win is the short-fade middle band, not the SNR
  edge.
- Multi-host spectator/DVR is out of scope; adding an independent second
  receiver reintroduces NAK-suppression and is a future extension.

---

## 12. Build order (suggested)

1. Wire header encode/decode + session model (§2–3).
2. TX framer with RTP boundary detection + resend ring (§5), classifier stubbed
   to size-heuristic first.
3. RX merge/dedup/gap-detector with both short-circuits (§6), NACK generation.
4. Air-side resend scheduler with priority/caps/hold-down (§5.3, §7).
5. NAL-type classifier for the RTP profile (§4.1).
6. `loopback` mode + synthetic-loss injector + counters (§9.2); extract knobs.
7. Field bring-up (§9.3).
## 13. Adaptive link layer

> Amends §3.1/§3.2 (packet types / DATA header), §5.3 (scheduler), §7 (uplink),
> §10 (knobs), §11 (scope). Every constant below is calibrated from the
> production controllers — `waybeam_wfb_ng/link_controller.c` @ `382d453` and
> `waybeam_venc/src/venc_api.c` @ `7441f76` — cited inline. These are
> on-air-validated values, not guesses; see `docs/groundwork.md` for the full
> extraction with file:line citations.

### 13.0 Objective

From OpenIPC adaptive-link's thesis (that doc is a philosophy paper —
energy-per-delivered-bit — not a wire spec; we take the objective, build the
mechanism): among operating points meeting the *delivered* loss/latency target,
prefer least **time-on-air** (highest workable MCS) at least TX power. Knob
priority, highest first: **MCS → TX power → RX chains → encoder bitrate**.

**Diversity/energy synergy (rationale):** the diversity fleet + ARQ (§6–7) mop
up marginal losses a single radio can't, so TX may ride a *higher* MCS at
*lower* power for equal *delivered* reliability. Diversity is not only
redundancy — it is *permission to run a lower-airtime, lower-energy operating
point*. Selection targets **post-diversity** delivered loss, never raw
per-adapter loss.

### 13.1 Control split (the load-bearing rule)

One closed loop, crossing the air once in the cheap direction:

- **RX reports RF metrics** (it has the merged diversity picture). It does **not**
  decide — it ships raw metrics up the backchannel (§7) at **~10 Hz** (matches
  wfb_ng `rx_ant` cadence, link_controller.c:7319).
- **TX runs the decision cascade and actuates.** MCS/power **and** encoder
  bitrate are decided **together at TX** from (a) the RX metrics and (b) a
  **TX-local backpressure signal** (venc output-queue fill — never crosses the
  air; wfb_ng carries this in its sidecar transport trailer, link_controller.c:
  135–150). RX feedback is *input*, never the decision.

**Why MCS+bitrate co-decide at TX:** they are sequenced against each other
(§13.5). Splitting them across the air races the two changes and reopens the
overshoot-into-lower-MCS loss burst. Co-locating makes the transition atomic and
local.

**Same-SoC actuation.** For craft (waybeam_venc → waybeam-link TX on one SoC),
the bitrate half of every transition is a **zero-air-latency local HTTP call**
(§13.6). Only the metrics — the slow, loss-tolerant half — cross the uplink.

| loop            | path                   | rate   | carries              |
|-----------------|------------------------|--------|----------------------|
| air feedback    | RX→TX over shared chan | ~10 Hz | RF metrics only      |
| local actuation | TX→radio, TX→venc HTTP | instant| MCS, power, kbps     |

**Decision law = rule cascade, first match wins** (mirrors wfb_ng
`selector_update`, link_controller.c:3605–3827 — NOT a weighted score):

1. **Reactive demote** — `smoothed_loss ≥ demote_per_milli` (**80‰ = 8%**).
   Suppressed under local backpressure (see §13.9). [3697, 5728]
2. **RSSI-floor demote** — `smoothed_rssi ≤ rssi_floor_dbm` (**−85 dBm**).
   RF guard-rail, independent of transport state. [3708, 5747]
3. **RSSI-fade demote** — `slope ≤ −rssi_fade_db_per_s` (**−10 dB/s**) AND
   `rssi ≤ rssi_fade_arm_dbm` (**−65 dBm**) for **3 consecutive ticks**.
   [3688, 5748–5749]
4. **Backpressure escape** — sustained local pressure ≥ `pressure_escape_s`
   (**2.0 s**) with clean RF ⇒ climb one rung per `down_cooldown_s` to drain the
   output ring (anti-death-spiral). [3720, 5718]
5. **Probe promote** — the only promote path (§13.4). [3761–3821]
6. **Hold.**

Demotes are rate-limited by `down_cooldown_s` (**0.2 s**) but never wait on
dwell. EWMA smoothing α = **0.3** for both RSSI and loss; slope EWMA α = 0.5.
[5703–5704, 5708]

### 13.2 `LINK_REPORT` packet (type `0x3`, RX→TX)

Reserved type `0x3` (§3.1). Common prefix, then the metrics the §13.1 cascade
consumes (superset of wfb_ng `rx_ant` fields, link_controller.c:7319–7349):

| field            | width | notes                                                 |
|------------------|-------|-------------------------------------------------------|
| *common*         | —     | §3.1; echoes `session_id`,`stream_id` scored          |
| `report_epoch`   | u16   | monotonic; TX watchdog + change-took-effect detect    |
| `rssi_best`      | i8    | best adapter, dBm                                     |
| `rssi_mean`      | i8    | fleet mean, dBm (TX derives slope from the series)    |
| `loss_pre_recov` | u16   | pre-diversity/ARQ loss, ‰ (0–1000)                    |
| `uniq`           | u32   | unique packets this interval (loss-ratio denominator) |
| `diversity`      | u32   | duplicate copies across adapters (decorrelation gauge)|
| `adapters`       | u8    | latched adapter count                                 |
| `probe_per`      | u16   | V+2 boundary-probe PER, ‰ (§13.4); 0xFFFF = no probe  |
| `recommended_prof`| u8   | RX hint; TX MAY override (final authority)            |

- Injected via the designated TX adapter (§6.4), same accounting as NACK.
- `diversity`/`adapters` let TX (and the bench, §9) compute **cross-adapter loss
  correlation** — the load-bearing measurement flagged in the §3 review.

#### 13.2.1 Downward echo (implicit negotiation)

Amend §3.2 DATA header — add `active_profile` (u8), the operating point TX is
currently on. This echo *is* the negotiation: stateless, loss-tolerant, rides
every packet. If RX recommended P but keeps seeing `active_profile=Q`, it learns
TX overrode for feasibility or its reports are being dropped — and adjusts
expectations with no retry protocol. (If DATA-header bytes are precious, carry it
on a periodic `0x4 = HEARTBEAT` instead; echo-on-DATA is more loss-robust.)

### 13.3 Profile table (pre-authored operating points)

Static table shared both ends; the air carries only the `u8` index. Seed rungs
from wfb_ng: video MCS **0–5**, with **+2 probe rungs (6–7)** reserved for the
boundary probe (link_controller.c:5711–5712).

```
profile[i] = {
  mcs, guard_interval, tx_power_cap,
  airtime_budget_frac,
  arq_deadline_ms[class],        // per §4.1 importance; I-frame class longer
  reserve_bps[stream_type],      // guaranteed floor for CONTROL/TELEMETRY
  bitrate_min_kbps,              // e.g. 2200 at MCS0 (wfb_ng fec.bitrate_min, 5649)
}
```

Diversity-primary means **no FEC in the base table** — note wfb_ng itself moved
loss-response OUT of FEC (its k/n is a *static* redundancy curve sized from frame
rate, NOT loss-reactive; Adaptive-n was removed) and INTO the MCS demote loop.
That validates waybeam-link's "loss ⇒ demote, not more parity" stance. Version
the table in `ver_type`; a diverged index degrades via the §3.4 unknown-type
rule.

### 13.4 Promote via V+2 boundary probe

wfb_ng promotes **only** after a boundary probe 2 rungs above current shows clean
PER (link_controller.c:3761–3821):

- **Promote** when `probe_per ≤ probe_clean_milli` (**20‰ = 2%**) AND no RSSI
  guard active AND flap-freeze clear AND `promote_dwell_s` (**0.5 s**) elapsed.
  [5719, 5731]
- **Pre-empt demote** when `probe_per ≥ probe_fail_milli` (**200‰ = 20%**) at the
  ceiling, to keep the +2 cushion. [5727]

**Divergence to resolve for waybeam-link:** wfb_ng runs the probe on a *separate
wfb stream*; the injection model has no such side stream. Two build options:
  (a) **v0 — RSSI-margin promote:** promote on `rssi ≥ next_rung_floor +
      rssi_floor_hyst_db` (**6 dB**, link_controller.c:5753) + dwell, no active
      probe. Simpler, slightly more conservative.
  (b) **v1 — active probe:** TX injects a short burst at MCS+1/+2, RX measures its
      PER and returns it in `probe_per`. Matches wfb_ng fidelity; costs a little
      airtime. Start (a), add (b) if promotes are too timid.

### 13.5 Transition sequencing (asymmetric, real timings)

MCS and bitrate never move together (link_controller.c:3033–3149):

- **Demote — bitrate LEADS:** drop encoder bitrate to the lower profile's budget
  first, wait `bitrate_lead_s` (**0.5 s**), *then* drop MCS. Prevents the encoder
  overshooting the new lower-capacity rung. [5654, 3108–3118]
- **Promote — bitrate LAGS:** raise MCS first, hold bitrate for `mcs_up_grace_s`
  (**0.25 s**), *then* raise it to the higher rung's budget. [5655, 3137–3149]
- Post-change **settle**: `mcs_settle_s` (**5.0 s**) suppresses further loss-loop
  reaction so a transition isn't mistaken for a fade. [5653]

### 13.6 Encoder actuation (venc, same SoC)

Live HTTP, `MUT_LIVE`, applied sub-ms with no pipeline reinit (verified against
waybeam_venc @ HEAD `7441f76`: venc_api.c:2312/407, star6e_controls.c:198):

```
GET /api/v1/set?video0.bitrate=<kbps>      Host 127.0.0.1:80
  → {"ok":true,"data":{"field":"video0.bitrate","value":<kbps>}}
GET /api/v1/dual/set?bitrate=<kbps>        # Star6E ch1 only; 501 on Maruko
```

- **Units = kbps.** Hard range **1000–200000** (venc_config.h:36–37), enforced by
  venc; default 8192. Profile `bitrate_min` is a *policy* floor ≥ 1000 (wfb_ng
  used 2200 at MCS0 — a controller policy, not venc's limit).
- venc internally does FPS-overshoot compensation (`rc_compensate_kbps`) and a
  rate-limited forced-IDR on each change — so bitrate steps coalesce cleanly.
- `rc_mode` (cbr/vbr/avbr/qvbr) is **restart-required**, not live — a profile
  selects a *bitrate* within a fixed RC mode, never switches CBR↔VBR on the fly.

**Invariant 1 — single bitrate authority (deployment, not a flag).** venc has
**no** `bitrate_enabled`/arbitration field; the API takes `video0.bitrate` from
any caller and **last-writer-wins**. So the authority rule is enforced by *not
running a competing writer* on the craft: (a) if waybeam-hub is present, set its
`venc.bitrate_enabled=false` (that flag lives in hub `mod_venc.c`, not venc);
(b) do not run wfb_ng `link_controller` — waybeam-link *replaces* it. This exact
two-writer fight (hub mod_venc vs link_controller, last-write-wins) is a
documented scar; waybeam-link must be the only process writing `video0.bitrate`.

**Invariant 2 — write only on change (flash wear).** Every `/set` **persists to
`/etc/waybeam.json`** on the overlay. Do NOT push bitrate at the 10 Hz feedback
rate — write only when the target actually changes (as the wfb_ng `bitrate_assert`
guard does), or the overlay flash takes needless wear.

**Low-bitrate coupling (optional).** At floor profiles the SVC-T resilience preset
oscillates 120↔24 fps (decoder drops enhancement frames under timing pressure).
If floor profiles are used, waybeam-link may also command
`video0.resilience=racing` (intra-refresh, no SVC-T) at the low end — but note
`resilience` is heavier than a bitrate tweak; treat as a coarse, hysteretic step.

### 13.7 Flap avoidance (three layers, wfb_ng defaults)

Lift wholesale rather than rediscover (link_controller.c:3558–3577, 3803–3815):

- **Soft reentry dwell:** re-promoting into the rung just demoted from, within
  `reentry_backoff_s` (**5.0 s**), requires `reentry_dwell_s` (**2.0 s**) instead
  of the normal 0.5 s. [5754–5755]
- **Hard flap-freeze:** `flap_freeze_count` (**3**) fast re-demotes from a rung
  within `flap_freeze_window_s` (**10.0 s**) pins the rung below for
  `flap_freeze_s` (**10.0 s**). This is the explicit **2-rung [min,min+1] trap**
  escape. [5766–5768]
- **min==max pin:** freezes adaptation at a fixed rung (bench / known-bad-link
  escape hatch). [5711–5712]

### 13.8 Fail-safe on lost feedback (new safety property)

`LINK_REPORT` rides the same lossy half-duplex uplink as NACKs. TX runs a
`report_epoch` watchdog: no epoch advance within `report_timeout_ms`
`[CAL: ~500 ms — a few missed 10 Hz reports]` ⇒ **fail toward degradation**:
hold, then step down toward a safe floor profile. **Never fail optimistic** — a
high MCS/bitrate held on a lost link is a black screen at range; a controlled
step-down is a clean fade. Mirrors the Android RX cold-start/latch watchdog
discipline.

### 13.9 Backpressure coupling (local, TX-side)

The venc output-queue fill / `in_pressure` flag is a **local TX signal**, not an
RX report (both live on the craft SoC). Its effect on the cascade
(link_controller.c:3720–3755):
- **Suppresses reactive-demote (rule 1)** — don't blame RF for a self-inflicted
  encoder overshoot.
- Does **NOT** suppress RSSI floor/fade rules (2,3) — real RF loss still demotes.
- After `pressure_escape_s` (**2.0 s**) sustained with clean RF, **climbs** MCS to
  drain the ring (rule 4, anti-death-spiral).

### 13.10 New knobs (append to §10)

| knob                  | meaning                              | seed / how measured             |
|-----------------------|--------------------------------------|---------------------------------|
| report cadence (Hz)   | LINK_REPORT rate                     | 10 Hz (wfb_ng rx_ant)           |
| report timeout (ms)   | §13.8 fail-safe trigger              | ~500 ms; uplink report-loss burst |
| demote_per_milli      | reactive-demote loss threshold       | 80‰                             |
| bitrate_lead_s        | demote bitrate-before-MCS gap        | 0.5 s                           |
| mcs_up_grace_s        | promote bitrate hold                 | 0.25 s                          |
| promote_dwell_s       | min hold before rate-up              | 0.5 s (2.0 s on reentry)        |
| probe_clean/fail ‰    | V+2 promote/pre-demote PER gates     | 20‰ / 200‰                      |
| NACK→RETRANSMIT (ms)  | (§3 review) ARQ round-trip fit       | measure — is ARQ in-deadline    |
| cross-adapter loss corr| (§3 review) diversity decorrelation | from diversity/adapters counts  |
```

## 14. Per-MCS TX power

Extends §13's operating-point model with a power dimension, so each profile
clears its MCS at the **minimum power that works** (the OpenIPC energy objective,
§13.0). Grounded in the stock Realtek `PHY_REG_PG.txt` power-by-rate format and
devourer's TX-power API; see `docs/groundwork.md` for citations.

### 14.1 Model — power is per-adapter and per-profile, not per-packet

Two things collapse the granularity, one thing expands it:

- **Not per-packet.** 8812AU = Jaguar1, 8812CU = Jaguar3 have **no per-packet TX
  power** in devourer — TXAGC is static until re-tuned (only Jaguar2 has a coarse
  per-packet radiotap path). Fine for us: each §13.3 profile pins one MCS, so
  power moves only at operating-point cadence, applied via
  `SetTxPowerOffsetQdb()` / `SetTxPowerIndexOverride()` at the MCS-change commit
  (§13.5), never per frame.
- **Per-adapter, NOT fleet-global.** Every physical adapter is a **separate
  devourer `IRtlDevice`** with its own efuse power calibration, antenna gain,
  thermal state, and role. Power is set on **each device individually**, and the
  correct value **differs per adapter**. So the power dimension is indexed by
  **(adapter × MCS)**, never one fleet-wide number. ("Global" earlier meant "not
  per-packet" — it is emphatically per-device, applied to each `IRtlDevice`
  in the fleet on its own.)

### 14.2 Where power lives — portable profile vs. local per-adapter table

Because absolute power is hardware-specific it **cannot** live in the on-air
profile (which is shared by index and must be portable across nodes/adapters):

- **On-air profile (§13.3)** carries only a power **intent** — a target level /
  index — alongside the MCS. Portable, hardware-agnostic.
- **Each TX node keeps a LOCAL per-adapter power map**, one per physical adapter
  (keyed by adapter identity / efuse), authored in the driver's proven
  `PHY_REG_PG.txt` row format:

```
#[v2][Exact]#
#[2.4G]A
[1]  <mcs_or_rate>  <value>     # per (adapter, band, rf-path, MCS) → power
...
0xffff
```

The controller resolves `(this adapter, profile.mcs, profile.level)` → an
absolute devourer setting and applies it to that adapter's device. The MCS axis
uses the driver rate-index enum (`MGN_MCS0=12 …`, groundwork §14); only the rungs
the profile table uses (HT MCS0–7) need populating.

### 14.2.1 No regulatory clamp — power is fully our responsibility

**devourer applies whatever value it is given.** `SetTxPowerIndexOverride(idx)` is
a *raw absolute* index and `SetTxPowerOffsetQdb(qdb)` an *uncapped* offset; neither
is limited to the efuse/regdomain ceiling. There is **no regulatory clamp in the
userspace driver**. Consequences:

- The per-adapter table holds **absolute, operator-authored** values, not
  "regulatory-safe trims." A mis-authored table **can and will** exceed legal /
  calibrated limits.
- Values **may intentionally exceed regulatory limits** for range — a deliberate
  operator choice and **their legal responsibility**, not a guard-railed default.
- `GetTxPowerCaps` / `GetTxPowerState` report each adapter's *hardware* range for
  reference and per-adapter baseline reads — they are **not** a safety limit.
- Recommend a per-node configurable `max_power` sanity ceiling in the controller
  (opt-in, off by default) so a fat-fingered table can't silently cook a PA or
  breach limits unless the operator explicitly raises it.

### 14.3 Actuation and sequencing

- On profile commit, for **each transmitting adapter** resolve
  `(adapter, profile.mcs, profile.level)` from that adapter's local power map
  (§14.2) and apply it to that adapter's `IRtlDevice` — `SetTxPowerOffsetQdb()`
  or `SetTxPowerIndexOverride()` — together with the MCS change, inside the §13.5
  sequenced transition (bitrate still leads on demote / lags on promote). Fleet
  members are set **individually**; values may differ.
- `ReApplyTxPower()` re-asserts that adapter's setting after any devourer re-tune
  that resets TXAGC (e.g. a channel change).
- Power is **not** a fast loop — it moves only with the operating point, at
  profile-change cadence, never per-frame.

### 14.4 Interaction with the probe (§13.4)

The V+2 boundary probe (if built, §13.4b) injects at MCS+1/+2 but at **each
adapter's own current power** (unchanged) — which is what you want: hold power
constant per adapter, vary MCS, measure whether the higher rung's PER is clean.
No per-rate power split within an adapter is needed (and none is available on
J1/J3).

### 14.5 Deployment note

`rtw_tx_pwr_by_rate` is a *kernel-driver* param, irrelevant to devourer (userspace
owns the radio). It matters only if a kernel driver is bound instead — documented
here so the two power paths are never confused. Under devourer, power authority is
`SetTxPowerOffsetQdb` + this table; under a bound kernel driver it would be
`rtw_tx_pwr_by_rate=1` + `PHY_REG_PG.txt`. waybeam-link uses the devourer path.
