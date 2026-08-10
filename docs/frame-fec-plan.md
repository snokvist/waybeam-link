# Frame-aligned FEC with SHM ingress — implementation plan

> **STATUS: IMPLEMENTED (PR #17, review-log Pass 15).** This plan is now built.
> The spec is pinned in PROTOCOL.md (§5.1a FrameFramer, §6.3a reassembly,
> §14.1 GF(256) Cauchy-RS MDS, §15.4 slot format) and the code is in
> `core/{gf256,rlc,frame_framer,frame_reassembler}.cpp`, `io/frame_shm.cpp`,
> and the app tx/rx loops. Deviations from this original plan, as ruled during
> implementation: the codec is **systematic Cauchy-RS MDS** (guaranteed
> recovery, k+r≤256 cap) not generic random RLC; the repair subheader grew to
> **11 bytes** (u16 `window_len` + u32 `frame_len`); source symbols carry a
> **4-byte self-describing subheader** (k, index); the DATA payload budget is
> **profile-driven (adaptive MTU)**, not fixed 1424. The §17 gate-2 vehicle
> walk now supports a 100/100-per-mille N=2 monitor campaign profile; the
> normative adaptive seeds remain independently configurable. Sections below
> are the original planning text, kept for provenance.

Plan for frame-sized FEC block emissions with ARQ fallback for small frames.
This document was a planning artifact — the PROTOCOL.md amendment and code
landed together in PR #17, per project law (`CLAUDE.md` "spec amendments commit
FIRST").

---

## 1. Background

The current pipeline:

1. **waybeam** (the encoder) fragments each encoded frame into 1400 B FU-A RTP
   packets and sends them over UDP to waybeam-link.
2. **waybeam-link** wraps each RTP datagram 1:1 into a DATA frame (§5.1
   no-fragmentation invariant) and injects it over the air.
3. The **Framer** (§5.1) infers block (frame) boundaries by parsing RTP marker
   bits and timestamps — indirect knowledge of frame boundaries.
4. **FEC was gated** per §14 on cross-adapter loss correlation rho (§17 gate
   2). The physical MCS5/N=2 vehicle walk completed 2026-07-19: diversity
   reduced 86‰ pre-diversity loss to 24‰ post-diversity loss, 10% GF(256) FEC
   recovered 599 source symbols, and ARQ recovered 52. Light FEC is therefore
   retained for the residual correlated tail; static 33% is not justified.

### Why change

- The Framer's marker-bit inference is fragile: it works, but the block
  boundary is a derived property of the RTP stream, not a first-class signal.
- FEC must be frame-aligned (the decoder needs a complete frame or nothing; a
  partial frame is useless). With per-RTP-packet ingress, the Framer must
  buffer and re-derive the frame boundary before it can build FEC blocks —
  duplicating work the encoder already did.
- A full-frame SHM handoff gives waybeam-link direct, authoritative knowledge
  of frame boundaries, frame type (IDR vs P), and frame size — all inputs to
  the FEC policy — without any RTP/NAL parsing on the link side.

---

## 2. Proposed change — SHM frame ingress

**waybeam** emits full encoded frames over a new `venc_frame_ring` SHM ring
(Annex B format, one slot = one frame, 512 KB slot ceiling).

### 2.1 SHM slot format

Each SHM slot contains:

| offset | size | field | encoding |
|--------|------|-------|----------|
| 0 | 4 | `timestamp` | u32, RTP-style 90 kHz clock |
| 4 | 1 | `codec` | u8: 0 = H.264, 1 = H.265 |
| 5 | 1 | `flags` | u8: bit 0 = IDR, bit 1 = GDR, bit 2 = SVC-T enhancement |
| 6 | 1 | `gdr_pos` | u8, zero-based GDR cycle position |
| 7 | 1 | `gdr_len` | u8, GDR cycle length; zero when inactive |
| 8 | N | NAL data | raw Annex B (start codes + NAL units) |

Total slot: 8-byte metadata header + up to (512 KB − 8) bytes of NAL data.

### 2.2 waybeam-link SHM consumer

waybeam-link reads full frames from SHM, classifies (IDR from metadata flag),
assigns `block_id` directly (one frame = one block). No RTP parsing, no
marker-bit inference.

---

## 3. Frame-aligned FEC design

### 3.1 Source symbol construction

Frame data split into **k** source symbols of size:

```
s = kMaxDataPayload − 6 = 1424 − 6 = 1418 bytes
```

The 6-byte deduction is the FEC subheader (§14 wire form) reserved in every
source+repair symbol so all symbols fit in a single DATA packet.

**k varies per frame:**
- 30 KB P-frame: k = ceil(30000 / 1418) = 22
- 150 KB IDR: k = ceil(150000 / 1418) = 106

The last source symbol is zero-padded to **s** bytes for the FEC computation
(the padding is not transmitted — the receiver knows the frame length from the
source symbols' aggregate payload or from an explicit length field in the first
source symbol).

### 3.2 Coding scheme — GF(256) systematic Random Linear Coding (RLC)

Per-frame block coding (not sliding-window — each frame is an independent FEC
block):

1. **k** source symbols form the message.
2. **r** repair symbols generated: `r = ceil(k * fec_rate)`, where `fec_rate`
   depends on frame type (§3.3).
3. Coefficients seeded deterministically from `(block_id, repair_idx)` — both
   ends can reconstruct the coefficient vector without transmitting it.
4. Each repair symbol = random GF(256) linear combination of all k source
   symbols: `repair[j] = sum_i( c[j][i] * source[i] )` over GF(256).
5. Systematic: source symbols are transmitted unmodified (no encoding cost on
   the common no-loss path).

### 3.3 Per-frame adaptive FEC policy

Three regimes, selected per frame:

| condition | FEC | rationale |
|-----------|-----|-----------|
| k <= `fec_min_k` (seed 3) | **ARQ-only**, no repair symbols | For k=3, one repair = 33% overhead; NACK->RETRANSMIT at 2-5 ms RTT (§2 gate-3 result) recovers within the frame deadline. Overhead not justified. |
| P-frame, k > `fec_min_k` | r = ceil(k * `fec_p_rate`) | Seed `fec_p_rate` 0.10-0.15 (10-15% overhead). P-frames are expendable (supersession, §6.2); light parity covers the short-burst gap diversity misses. |
| IDR frame | r = ceil(k * `fec_i_rate`) | Seed `fec_i_rate` 0.25-0.30 (25-30% overhead). IDR loss is catastrophic (entire GOP lost until next IDR); heavier parity justified. |

The 2026-07-19 real-fade measurement establishes 10% as the current N=2
monitor campaign base rate. A future enforcing controller may test 15–20% at
the edge. This does not make 33% a useful static rate: the dominant physical
failure was a 37.1 s all-receiver blackout that parity cannot cross.

---

## 4. Wire format

Reuses the §14 wire form already spec'd in PROTOCOL.md:

### 4.1 Source symbols

Normal DATA packets. Payload = chunk of frame data. `data_flags.FEC_REPAIR`
**not** set. With no repair rows the last source symbol has `END_OF_BLOCK` set;
otherwise the flag moves to the final repair row so block close follows parity.

All packets in a frame share the same `block_id`.

### 4.2 Repair symbols

DATA packets with `data_flags.FEC_REPAIR` set. 6-byte subheader before the
coded payload:

| offset | size | field |
|--------|------|-------|
| 0 | 1 | `repair_idx` u8 |
| 1 | 1 | `window_len` u8 (= k, number of source symbols) |
| 2 | 4 | `window_base_seq` u32 (seq of the first source symbol in this block) |

Followed by 1418 bytes of coded repair data.

### 4.3 Emission order

Within a block: all **k** source symbols first (in order), then all **r**
repair symbols. Source-first lets the common no-loss case deliver without any
FEC decode. Repair symbols are appended after `END_OF_BLOCK`.

---

## 5. RX side

### 5.1 Collection

Collect source + repair symbols per `block_id` into a per-block reassembly
buffer.

### 5.2 Delivery decision

Three outcomes, in priority order:

1. **Common case (no loss):** all k source symbols arrive -> deliver
   immediately, concatenate payloads, zero FEC computation.
2. **Loss with recovery:** fewer than k source symbols, but >= k total symbols
   (source + repair) -> GF(256) Gaussian elimination to recover the missing
   source symbols, then deliver.
3. **Unrecoverable:** < k total symbols after the block deadline expires ->
   frame lost, superseded by the next block (§6.2 supersession).

### 5.3 Delivery options

**Option A (preferred): SHM egress to decoder.** Write the reassembled frame
into the same `venc_frame_ring` format (8-byte metadata header + Annex B NAL
data). The decoder reads full frames from SHM — symmetric with ingress.

**Option B (compatibility): RTP re-packetization.** Scan the reassembled
Annex B data for NAL boundaries, FU-A fragment each NAL to <= 1400 B, emit as
RTP/UDP to the decoder's existing UDP listener. This is the fallback when the
decoder cannot consume SHM (e.g., Android MediaCodec with an existing RTP
pipeline).

Option A is preferred: it avoids the re-packetization overhead, the RTP
sequence-number synthesis, and the risk of FU-A fragmentation bugs in a
format the link layer shouldn't need to understand.

---

## 6. FrameFramer — core/ replacement for SHM streams

`FrameFramer` replaces `Framer` (§5.1) when the ingress binding is
`frame-shm`. Lives in `core/` (pure protocol logic, no I/O).

### 6.1 Inputs

- Full frame data (Annex B NAL bytes)
- Metadata: timestamp, codec, IDR flag (from SHM slot header, §2.1)

### 6.2 Responsibilities

| step | FrameFramer | Framer (current, §5.1) |
|------|-------------|------------------------|
| block boundary | **direct**: one frame = one `block_id` | inferred from RTP marker bit / timestamp change |
| ARQ classification | **direct**: metadata IDR flag | NAL-type parsing (§4.1) |
| fragmentation | frame -> k source symbols of size s | none (1:1 datagram -> DATA) |
| FEC coding | generate r repair symbols (§3.2) | n/a |
| `seq` assignment | monotonic across all symbols in the block | monotonic per datagram |
| emission | k source + r repair DATA packets per frame | 1 DATA packet per datagram |

### 6.3 No-FEC passthrough

When `fec.scheme` is `"none"` or when k <= `fec_min_k`, FrameFramer still
fragments the frame into source symbols (it must — a full frame does not fit a
single MPDU) but emits zero repair symbols. ARQ (NACK/RETRANSMIT) operates on
the individual source symbols as normal.

### 6.4 Interaction with existing §5.1 invariant

The §5.1 no-fragmentation invariant ("each ingress datagram MUST fit one MPDU
payload") applies to the RTP/UDP ingress path where the encoder's payloader
already fragments NALs to MTU. For SHM streams, FrameFramer *is* the
fragmenter — the invariant is relaxed: the ingress unit is a full frame (up to
512 KB), and FrameFramer fragments it into source symbols that each fit one
MPDU. The spec amendment will add this as an explicit alternative path in §5.1.

---

## 7. Config shape (v1 SHM binding)

```json
{
  "streams": [{
    "stream_id": 0, "stream_type": "RTP", "dir": "in",
    "bind": { "kind": "frame-shm", "name": "venc_frame" },
    "fec": {
      "scheme": "rlc256",
      "i_rate_permille": 250,
      "p_rate_permille": 100,
      "min_k": 3
    }
  }]
}
```

- `bind.kind`: `"frame-shm"` selects the venc_frame_ring SHM consumer and
  routes through FrameFramer instead of Framer.
- `bind.name`: SHM ring name (maps to the POSIX shared-memory object name).
- `fec.scheme`: `"rlc256"` enables GF(256) frame-aligned RLC; `"none"` disables
  FEC (FrameFramer still fragments, ARQ still operates).
- `fec.i_rate_permille` / `fec.p_rate_permille`: FEC overhead per-mille for
  IDR / P-frames. Integer per-mille, not float — matches the project convention
  (§9.3 profile values are per-mille).
- `fec.min_k`: frames with k <= this value skip FEC (ARQ-only). Seed 3.

When `bind.kind` is `"udp"`, the `fec` block is ignored and the existing
Framer path is used unchanged. FEC is only supported on SHM-ingested streams.

---

## 8. Implementation sequencing

Four steps, each independently PR-able:

### Step 1: waybeam `venc_frame_ring` + new output mode

**Repo:** `waybeam_venc` (separate repo, not waybeam-link).

- Implement the `venc_frame_ring` SHM producer in waybeam.
- Slot format per §2.1.
- waybeam continues to emit RTP/UDP in parallel (dual-output) so the existing
  pipeline is unbroken during the transition.

### Step 2: waybeam-link SHM ingress + FrameFramer (source-only, no FEC)

**Repo:** waybeam-link.

- SHM consumer (`io/`): open the named SHM ring, poll for new frames.
- FrameFramer (`core/`): fragment frames into source symbols, assign block_id,
  set ARQ from metadata IDR flag, emit DATA packets. Zero repair symbols.
- Config: `bind.kind "frame-shm"`, `fec.scheme "none"`.
- **Validates the full-frame SHM pipeline end-to-end** (waybeam -> SHM ->
  waybeam-link -> air -> RX -> delivery) without FEC complexity.
- Gate: loopback + RF bench must match the existing RTP/UDP path's delivery
  rate and latency.

### Step 3: GF(256) FEC codec

**Repo:** waybeam-link, `core/`.

- Standalone GF(256) arithmetic: multiplication via log/exp tables (512 B) or
  direct 64 KB mul table (see §9 memory budget).
- RLC encoder: given k source symbols, produce r repair symbols with
  deterministic coefficients seeded from (block_id, repair_idx).
- RLC decoder: given >= k symbols (any mix of source + repair), Gaussian
  elimination to recover missing source symbols.
- Unit-tested (`tests/fec_test.cpp`): roundtrip encode-decode, erasure
  patterns (random, burst, boundary), k=1 / k=3 / k=106 / k=max.
- ARM-benchmarked: encoding + decoding latency on SSC338Q at representative k
  values. Must fit within the frame deadline (16.7 ms at 60 fps, minus TX
  airtime).

### Step 4: Frame-aligned FEC integration

**Repo:** waybeam-link.

- Wire FEC codec into FrameFramer TX (step 2) + RX reassembly.
- Implement the per-frame adaptive policy (§3.3): min_k gate, per-type rates.
- Bench against §17 gates: delivery rate with synthetic loss at the rho levels
  measured in gate 2 must exceed the no-FEC baseline.
- Config: `fec.scheme "rlc256"` with rate seeds from bench.

---

## 9. Memory budget (ARM — SSC338Q)

| component | size | notes |
|-----------|------|-------|
| TX: venc_frame_ring consumer buffer | ~512 KB | one full slot (the ring itself is in SHM, not process memory) |
| RX: per in-flight block symbol buffers | ~400 KB worst case | at 60 fps, <= 2 blocks in flight; worst case 2 x max_frame (~200 KB IDR) |
| GF(256) mul table (option A) | 64 KB | 256x256 direct lookup — fast, cache-friendly |
| GF(256) log/exp tables (option B) | 512 B | 2x256 tables, mul = exp[log[a]+log[b]] — smaller, one extra lookup per multiply |
| Coefficient scratch per block | k bytes | one row of coefficients per repair symbol; generated on the fly from the seed, not stored |

Total headroom: ~1 MB TX + RX combined, well within the SSC338Q's available
DRAM. Option B (log/exp) is preferred unless the bench shows the extra lookup
is a bottleneck at high k.

---

## 10. Spec sections affected

The following PROTOCOL.md sections will need amendment when implementation
begins. Listed here for planning; the actual amendment will be a separate
spec-first commit per project law.

- **§14 FEC** — currently "deferred, bench-gated candidate." Amendment:
  concrete scheme selection (GF(256) frame-aligned RLC), wire subheader pinned
  (already sketched in §14, to be formalized), per-frame adaptive policy
  (min_k gate, per-type rates), emission order (source-first, repair after
  END_OF_BLOCK).

- **§15.1 Binding model** — currently "v0 = UDP only (shm/unix are v1)."
  Amendment: SHM binding becomes real (v1 -> live), `frame-shm` kind added
  with its config shape and the venc_frame_ring slot format.

- **§4 Block model** — currently "one block = one RTP frame (marker/timestamp
  boundary)." Amendment: FrameFramer alternative path where block_id is
  assigned from frame count directly, not marker-bit inference. The block model
  itself is unchanged (a block is still packets sharing a block_id, delimited
  by END_OF_BLOCK).

- **§5.1 Framer** — currently "no fragmentation (invariant)." Amendment:
  invariant relaxed for SHM streams — FrameFramer fragments frames into source
  symbols. The invariant still holds for UDP/RTP ingress (the encoder's
  payloader owns fragmentation there).

---

## 11. Open questions (for operator ruling when implementation begins)

- [ ] **RX egress path:** Option A (SHM) vs Option B (RTP re-packetization) —
      depends on whether the waybeam decoder can consume SHM. If yes, Option A
      is strictly better. If the Android path needs RTP, Option B is a
      compatibility shim, not the primary path.
- [x] **FEC operating point after gate-2 vehicle verdict:** the physical N=2
      MCS5 walk used 100/100 per-mille. It delivered the intended recovery
      order (diversity, then 599 FEC versus 52 ARQ source-symbol recoveries),
      while the 37.1 s joint blackout showed why static 33% cannot solve the
      edge. Keep 10% as the monitor campaign base; evaluate 15–20% only as an
      adaptive edge action. The normative 250/100 seeds remain configurable
      for importance-weighted production policy rather than being silently
      rewritten by this campaign result.
      **Provenance (2026-08-10):** "the monitor campaign" is literal — that
      campaign ran on the `kernel-monitor` backend deleted in Pass 164. **RULED TRANSFERABLE (operator, 2026-08-10).**
      ρ is geometric, so the verdict and this 10% base carry to devourer
      unchanged (`docs/findings.md` 2026-08-10).
- [ ] **Repair symbol scheduling vs live priority:** §14 says parity is
      live-priority (budgeted from encoder bitrate). With frame-aligned FEC,
      repair symbols for the current frame are emitted immediately after the
      source symbols — they ARE the current frame's live data, not deferred
      parity. Confirm this matches the §5.3 scheduler's priority model (repair
      symbols = same priority as their frame's source symbols, not demoted to
      retransmit priority).
- [ ] **Sliding-window vs per-frame:** this plan uses per-frame block coding
      (each frame is an independent FEC block). A sliding-window (Tetrys-style)
      codec would allow inter-frame recovery but adds complexity and latency.
      The §14 spec text mentions sliding-window RLC/Tetrys — confirm per-frame
      is the right simplification for v1.
