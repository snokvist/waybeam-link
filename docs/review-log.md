# Review log

This spec is reviewed several times before any code is written. Log each pass
here: date, reviewer, what changed, open questions.

## Pass 1 — 2026-07-10 — initial adversarial review + grounding

- Reviewed the v0 draft (§1–12) against the ecosystem and physics.
- **Findings folded in:**
  - §3 risk-3 (uncapped live bitrate) → resolved by the adaptive quota (§13.4):
    live video is now hard-bounded by airtime minus the ARQ reserve.
  - Diversity-correlation and NACK-round-trip flagged as the two premise-critical
    measurements → promoted to build gates (docs/build-order.md).
- **Adaptive link layer (§13) added**, calibrated from production controllers.
  Grounding overturned three assumptions (see groundwork.md): no composite quality
  score (rule cascade), promote is a V+2 probe, venc has no bitrate-authority flag.
- **Per-MCS TX power (§14) added** [pending — see groundwork.md TX-power section].

## Pass 2 — 2026-07-10 — TX-power corrections (operator)

- **Per-adapter, not fleet-global.** Each diversity adapter is a separate devourer
  `IRtlDevice` with its own efuse power calibration/antenna/role — power is set per
  device and differs per adapter. §14 reworked: power indexed by (adapter × MCS);
  absolute values live in a node-local per-adapter table; the on-air profile
  carries only a portable power *level/intent*. `tx_power_qdb` → `tx_power_level`
  in the example profile.
- **No regulatory clamp.** devourer's `SetTxPowerIndexOverride` is a raw absolute
  index (uncapped); the earlier "efuse owns the ceiling / regulatory-safe by
  construction" claim was WRONG and is removed (§14.2.1). Power is fully the
  operator's responsibility and may intentionally exceed regulatory limits;
  recommended opt-in `max_power` sanity ceiling in the controller.

## Pass 3 — 2026-07-10 — adversarial review + latency-first redesign

Five investigators (3× prior-art/code/spec grounding, 2× Opus red-team + design-synth),
all grounded in verified source. Full arbitration in **`docs/findings-pass3.md`** —
proposal awaiting operator sign-off; **nothing written into PROTOCOL.md yet**.

- **Operator decisions locked:** first-latcher-lock ARQ; `originator` = config node ID;
  FEC = evaluate-then-decide; findings-doc-first.
- **8 audit blockers ruled** (findings Part A): demote 80‰ RE-DERIVE (was pre-FEC;
  no FEC now → ~20‰ delivered); single-RX invariant REWRITE (multi-RX first-class);
  three-ID model (`originator` u16 / `session_id` u32 / `destination` u16); two-identity
  control packets (sender in prefix, target descriptor in body); `active_profile` +
  `table_version` added to DATA; §13.0 objective inverted → robustness-first.
- **Pivotal ruling — deterministic return telemetry (findings Part C):** NOT achievable
  as a guarantee (no per-packet TX departure timestamp; can't reserve airtime; 8812EU
  self-RX desense trap). Resolved: **best-effort opportunistic return is the shipping
  baseline**; the TSF-anchored EOB-quiet-gap is an opportunistic optimization behind a
  bench gate; graceful vehicle-only fallback on lost feedback. Recommend a dedicated
  craft 2nd RX adapter.
- **SWFEC (Part F):** red-team killed the XOR-only middle option (recovers the case
  diversity already handles, fails the burst that motivates FEC). Real choice is binary
  no-FEC vs GF(256) RLC, **gated on measured ρ**.
- **Follow-me CSA (Part E):** lift our `vehicle/csa`, TSF-anchor T_switch, paired
  ReApplyTxPower, adaptive freeze during VERIFY, two-tier straggler rendezvous + home
  channel, anti-replay; forged-CSA = CRITICAL fleet-blackout → scoped 4-byte MAC
  proposed.
- **No-auth hardening (Part G):** plausible-forward-window clamp + global-per-seq
  hold-down + `preferred_originator` + contested-only lock release neutralize the
  injection class at near-zero cost.
- **I/O layer (Part H):** JSON config + stats + shm/unix/4×UDP binding model; v0=UDP.

### Prior open questions — resolved
- Promote mechanism → **RSSI-margin v0, active probe deferred** (injection has no wfb
  side-stream; findings Part D).
- `active_profile` echo → **on DATA header** (chosen; + `table_version`).
- ARQ scope after gate → **decided jointly with SWFEC**, both bench-gated on ρ and
  return-RTT (Part F).
- Per-MCS TX-power granularity → unchanged (§14 per-adapter global offset stands).
- Bidirectional RC uplink → subsumed by the symmetric originator model (any node TXes).
- Repo vendoring → unchanged (still private-phase; revisit at first consumer).

## Pass 3b — 2026-07-10 — Fable adversarial review of findings-pass3.md

Fifth-pass skeptic; confirmed N=3 adapters in one process (per-adapter libusb context +
thread, RX-only) against `Waybeam-android/wifi/.../wifi_jni.cpp`. **Gate 1 resolved/
rescoped** → residual unknown is one injector + N monitors in-process. 4 must-fix items
folded into findings Pass 3b: (BLOCKER) return-path incoherence + floor-oscillation under
single-adapter craft → damp + escalate the 2nd-adapter revisit (now software-cheap);
(MAJOR) forged LINK_REPORT defeats "never fail optimistic" → latch-filter + plausibility;
(MAJOR) pilot-starvation contradiction → preference = preemption; (MAJOR) craft-misses-CSA
strand → ground commits only after craft ACK on the strong downlink + issuer reverts on
no-video. Plus: loss_pre_recov semantic-flip rename, table_version→content-hash, big-endian
declared, CSA MAC covers common prefix, csa_psk trust boundary = craft+ground, home_chan
config-pinned (CSA 34→32 B), ρ→P95 estimator + two hybrid FEC options added to the bench.

## Pass 4 — 2026-07-10 — implementation-gap rulings (operator)

First implementation session (§19 steps 1–2) surfaced two spec gaps; operator
ruled, folded into PROTOCOL.md in the same change:

- **HEARTBEAT (type 0x4) body was undefined** → ruled **prefix-only, 11 bytes**
  (new §3.8). Presence/keepalive; never creates per-stream RX state.
- **`table_version` hash was underspecified** (§3.6 named CRC-8 but pinned neither
  polynomial nor canonical form) → ruled **CRC-8/DVB-S2 (poly 0xD5, init 0x00)**
  over a defined **canonical binary serialization** (profiles sorted by id, fixed
  field order, big-endian, fractions scaled to integer per-mille). Matches the
  ecosystem's CRSF CRC-8.
- Amendment policy: spec commit lands first, code follows in the same PR.
- Portability constraint recorded: reference implementation must build for
  SigmaStar SSC338Q (32-bit ARMv7, OpenIPC gnueabihf gcc 13.3) and x86 host.

## Pass 5 — 2026-07-10 — §6.6 deep-fade amendment (from the step-4 build)

The steps 3–6 loopback bench (§16.2 scenario d, 95 % correlated fade) proved the
original single-cursor clamp wording death-spirals: during a deep fade the
delivery cursor advances by deadline-skips without delivering, so a
delivered-block clamp reference freezes and clamp-rejects the entire recovering
stream forever. Amended §6.6 (flagged in PR #3, merged; spec updated here):

- block clamp references **`max_block`** (newest legitimately heard block), not
  the last delivered block; attacker residual = one accepted in-clamp packet per
  `+K` ratchet step.
- **sustained-clamp resync** (`clamp_resync_ms`, seed 500 ms): if every packet
  clamp-rejects for the whole window, adopt the next packet as a fresh floor
  (§2 startup-floor semantics, `resyncs` stat). A forger must sustain an
  unbroken flood for the full window to force a flush.

## Pass 6 — 2026-07-10 — step-8 implementation-gap rulings (operator)

Planning the §9/§10 build surfaced three spec gaps; operator ruled, folded into
PROTOCOL.md spec-commit-first (same PR as the step-8 code):

- **§9.4 `next_rung_floor` was unsourced** → ruled **node-local config array**
  `policy.select.rung_rssi_floor_dbm` (one dBm floor per rung, §17-overridable,
  seeds from typical HT20 RX sensitivity + margin). Deliberately NOT in the
  hashed §9.3 wire table: receiver-local physics, and a table field would break
  `table_version` for local tuning.
- **§10.2 `tx_power_level` → absolute mapping was undefined** → ruled **curve +
  level offset**: the authored per-adapter per-MCS curve IS level 4;
  `absolute_qdb = curve[mcs] + (level − 4) × 8 qdb` (2 dB/step), then the
  opt-in `max_power_qdb` ceiling.
- **§9.5 "budget" had no definition** → documented as **derived, not
  configured**: HT20 PHY rate × airtime fraction × (1 − FEC overhead) −
  reserves, floored at `bitrate_min_kbps`, integer math.

## Pass 7 — 2026-07-10 — step-9 encapsulation ruling (operator)

Planning the devourer backend surfaced that §0 said "devourer owns radiotap +
the 802.11 MAC header" without ever pinning the MAC header's content or the
RX-side frame filter — wire interop territory. Operator ruled **pinned data
frame + SA-prefix filter**, folded in as new **§3.0**:

- 24-byte non-QoS Data frame, ToDS/FromDS=0, Duration 0, DA=broadcast (a
  broadcast DA can never solicit an 802.11 MAC ACK — keeps the §1 no-MAC-ARQ
  invariant structural, not behavioral).
- SA = locally-administered `57:42:<net_id>:<originator u16 BE>:<adapter idx>`;
  BSSID = fixed `57:42:4c:4b:00:00` ("WBLK"). Frame body = raw waybeam-link
  packet, no LLC/SNAP.
- RX filter: Data && SA prefix `57:42` (&& `net_id` equality only when the
  node configures one) && payload magic + header validation. `net_id` is a
  new optional node-local config so co-located systems partition at L2;
  explicitly not access control.
- Rejected alternative: broadcast-everything + payload-magic-only filter —
  simpler spec text but no L2 discriminator and no co-location separation.

Same pass, scope ruling: step 9 is **code-only** (vendor devourer, radio
backend, §7.2 quiet-gap pacer; verified by tests + x86 ASan + SSC338Q cross).
Hardware bring-up and gates 1–4 stay at step 11.

## Pass 8 — upstream devourer aggregation/HW-ACK review + §3.0 SA amendment (2026-07-11)

Reviewed upstream devourer 3025e2d ("Packet aggregation and hardware ACKs in
userspace", PR #239; four off-by-default capabilities) against our design.
Operator ruled **no pivot**: broadcast + importance-gated NACK-ARQ stands —
hardware ARQ retries indiscriminately (head-of-line on the craft's single
radio), and its failure mode (designated ACKer fades → every frame re-airs to
the retry limit) is an airtime storm exactly when the link is marginal.
A-MPDU rejected for v1 (0.8–3 ms aggregate-fill pacing = latency adder; needs
QoS-Data; Jaguar1/8812AU collapses under the deep feed it requires). USB TX
aggregation rejected for the data path (shallow feed by design).

Adopted:
- **§3.0 SA amendment (spec change this pass)**: SA/BSSID first octet
  `0x57 → 0x56`. Upstream's docs surfaced that `0x57` has the I/G bit set —
  our SA was a **group address**, nonconforming as a TA and permanently
  unable to solicit an ACK. `0x56` = locally-administered unicast. BSSID tag
  becomes "VBLK". Payload magic stays `0x57 0x42`.
- **Re-vendor at 3025e2d** (own PR): all knobs off-by-default byte-identical;
  buys per-frame TX-status CCX reports — a TX-side wedge detector /
  queue-latency sensor for §9 that costs zero return-path bytes. Exclude
  upstream `reference/` (vendored kernel-driver submodules).
- **Step-11 bench item**: hardware-ACK'd **uplink** hybrid — craft arms
  `SetAckResponder` so ground→craft returns (NACK/LINK_REPORT, one shot per
  §7.2 window today) get SIFS-timed hardware ARQ; downlink video stays
  broadcast. Bench slot, not a redesign.

## Pass 9 — §11.4 csa_mac primitive pinned (2026-07-11)

Step-10 planning surfaced that §11.4 said "HMAC" without pinning the hash.
Operator ruled **HMAC-SHA-256**, leftmost 4 tag bytes big-endian, MAC input =
encoded CSA bytes 0..27, `csa_psk` = raw config-string bytes (RFC 2104 keying,
no derivation). Rejected: SipHash-2-4 (needs a PSK→128-bit derivation rule and
isn't an HMAC construction) and keyed BLAKE2s (less legible in embedded
ecosystems). CSA is rare, so CPU cost was irrelevant; legibility and
vendorability (Android core reuse) decided it.

## Pass 10 — §17 gate-3 estimator pinned (2026-07-11)

Gate 3 said "NACK→RETRANSMIT round-trip P90" without pinning the anchor when
re-NACKs occur. Pinned **both** anchors as distinct observables: *round-trip*
(most-recent NACK → RETRANSMIT arrival — the pure link RTT the §5
freshness-priority gate consumes) and *recovery* (first NACK → arrival — the
gate-3 headline vs the I-frame deadline, because a lost NACK's backoff wait is
real recovery latency the deadline doesn't forgive). Samples are gated on the
`RETRANSMIT=1` flag so a late original copy never contaminates the series
(`recovered_arq` still counts it). Cumulative power-of-two-ms histograms
(≤1,2,4,8,16,32,64,>64) in stream stats; offline percentiles from JSONL
deltas (`tools/gate3_rtt.py`), matching the gate-2 analyzer pattern.

## Pass 11 — §9.10 TX-wedge watchdog pinned (2026-07-11)

The Pass-8 bench slot said "reports stall while `tx_submitted` advances"
without pinning what "stall" means. The step-11 saturation series
(docs/step11-bench.md §2, incidental 1) showed the obvious reading — a
report-count *deficit* — is wrong: healthy CCX return rates fall from 100%
at ≤500 pps to ~25% at 4500 pps under normal report-channel contention.
Pinned the trigger as report **absence**: one verdict per `wedge_window_ms`
(seed 1000) — `Δtx_reports == 0` while `Δtx_submitted >= wedge_min_submits`
(seed 8) sets `tx_wedged`; any report clears it; an idle window holds the
previous verdict. Action is **observability only** in v1 (`tx_wedged` in
§15.3 + a transition log line): a craft TX wedge already trips the §9.8
report-epoch fail-safe (video and returns die together), recovery needs a
physical re-plug regardless, and coupling an unvalidated detector into
adaptation risks false-positive degradation. Also brought the §15.3 adapter
sample back in sync with the emitted schema (`drop`, `tsf_fallback`,
`tx_reports`, `tx_report_fails` had drifted out of the sample).

## Pass 12 — §3.0 unicast HW-ACK return shape pinned (2026-07-11)

The Pass-8 bench slot ("craft arms `SetAckResponder`, ground returns as
unicast QoS-Data") left the wire shape unpinned. Pinned: FC `0x88 0x00`,
addr1 = the craft SA **as last heard** (latched per originator — exact
match with the armed MACID, adapter-idx byte included), QoS Control TID 0 /
Normal ACK, radiotap `TX_FLAGS = 0`. RX-filter amendment: QoS-Data accepted
with the Retry bit masked (hardware retransmissions set it), body at offset
26 — receivers always accept both shapes so the knob halves
(`return.unicast` ground, `air.ack_responder` craft) deploy independently
for the A/B. Duplicate deliveries from a lost ACK ride the existing
idempotent NACK/report handling. Scope: NACK + LINK_REPORT only; CSA
campaign copies and all DATA stay broadcast (Pass 8 rejected hardware ARQ
for the video path). No vendored changes needed: both chip families already
hardware-retry unicast injects (descriptor limit 12) and implement
`SetAckResponder`; an unlatched target falls back to broadcast, counted as
`unicast_fallback` in §15.3.

## Pass 13 — §3.0 kernel-monitor backend + per-packet radiotap MCS (2026-07-11)

Added a second air backend (`air.kind "kernel-monitor"`) that injects/receives
raw 802.11 through the Linux driver in monitor mode (AF_PACKET, no devourer),
after the devourer 8822e 16-QAM+ TX proved dead in UNII-3 (5805, the gate
channel) — the kernel driver transmits clean MCS7 there. **Spec impact is
minimal and additive:** the on-air frame, SA filter, and FCS handling are
unchanged; only the *rate-carrying mechanism* is generalized (§3.0 "Pass 13"
paragraph). devourer keeps its rate-less `TX_FLAGS`-only radiotap + out-of-band
`SetTxMode`; the kernel-monitor backend carries the PHY rate in a per-packet
radiotap **MCS** field (13-byte HT radiotap). RX radiotap yields `DBM_ANTSIGNAL`
(RSSI) and `TSFT` (§7.2 per-adapter TSF); no per-adapter TSF *read* on a monitor
netdev → host-time fallback (already a supported §7.2 path). CSA-over-monitor
retune and precise per-rate power actuation are deferred (channel + `txpower
auto` fixed at monitor bring-up; the 8812eu per-rate TXAGC curve owns power).
The backend is pure POSIX → compiled unconditionally (a `WBLINK_RADIO=OFF`
build still has a real RF path). devourer stays vendored + selectable.

## Pass 14 — §14/§15.1 frame-aligned FEC plan + SHM ingress shape (2026-07-11)

Planning pass — no code, no PROTOCOL.md amendment. Spec amendment will be a
separate commit when implementation begins, per project law.

Designed the full-frame SHM ingress + frame-aligned FEC pipeline
(`docs/frame-fec-plan.md`):

- **venc_frame_ring SHM format:** 8-byte metadata header (timestamp u32, codec
  u8, flags u8 [bit 0 = IDR], reserved u16) + raw Annex B NAL data, one slot
  per frame, 512 KB slot ceiling. waybeam produces; waybeam-link consumes.
- **FrameFramer (core/ replacement for Framer on SHM streams):** block_id
  assigned directly from frame count (no marker-bit inference), ARQ
  classification from metadata IDR flag (no NAL parsing), frame fragmented into
  k source symbols of size s = kMaxDataPayload − 6 = 1418 B.
- **GF(256) systematic RLC per frame:** k varies per frame (22 for 30 KB
  P-frame, 106 for 150 KB IDR); r = ceil(k * rate) repair symbols;
  coefficients seeded deterministically from (block_id, repair_idx).
- **Per-frame adaptive policy:** k <= fec_min_k (seed 3) -> ARQ-only (overhead
  not justified at small k, gate-3 RTT recovers within deadline); P-frame
  fec_p_rate seed 0.10-0.15; IDR fec_i_rate seed 0.25-0.30.
- **Wire format:** reuses existing §14 sketch (source = normal DATA, repair =
  DATA + FEC_REPAIR flag + 6-byte subheader). Source-first emission order.
- **RX:** no-loss fast path (all k sources -> deliver, no decode); loss
  recovery via Gaussian elimination when >= k total symbols; unrecoverable
  when < k.
- **Config shape:** `bind.kind "frame-shm"`, `fec.scheme "rlc256"`,
  `i_rate_permille`/`p_rate_permille`/`min_k` per stream.
- **4-step implementation sequencing:** (1) waybeam venc_frame_ring (separate
  repo), (2) waybeam-link SHM ingress + FrameFramer source-only, (3) standalone
  GF(256) codec, (4) FEC integration. Steps 2-4 in waybeam-link.

**Spec sections to amend (when implementation begins):** §14 (concrete FEC
scheme), §15.1 (SHM binding live), §4 (FrameFramer block model), §5.1
(no-fragmentation invariant relaxed for SHM).

**Decisions deferred to operator:** RX egress path (SHM vs RTP
re-packetization), FEC rate seeds (pending gate-2 vehicle rho), repair symbol
priority vs §5.3 scheduler, per-frame vs sliding-window confirmation.

## Pass 15 — §5.1a/§6.3a/§14.1/§15.4 frame-SHM ingress+egress + GF(256) RLC pinned (2026-07-11)

Implementation begins (PR #17). Spec amended FIRST (this commit); code follows.
The Pass 14 plan is now pinned into PROTOCOL.md, with **four operator rulings**
resolving gaps the plan left open:

- **Ruling — metadata transport:** the 8-byte `VencFrameMeta` is **prepended
  into the opaque payload**, not carried in the DATA header. The block payload =
  `[VencFrameMeta][Annex-B]`, byte-identical to the venc slot (§15.4). RX egress
  writes it back verbatim; the DATA header is untouched; "RTP opaque on the wire"
  (§1) holds. FrameFramer reads only `flags` bit 0 (§4.1).
- **Ruling — `window_len` u8 → u16 + new `frame_len` u32:** the §14 repair
  subheader grows 6 B → **11 B** (`repair_idx u8, window_len u16,
  window_base_seq u32, frame_len u32`). u8 `window_len` overflowed (512 KB at a
  1400 B rung ⇒ k≈370 > 255); `frame_len` is required to strip a FEC-recovered
  last symbol's zero-padding. Source-symbol size becomes
  `s = max_payload − 26 − 11`.
- **Ruling — adaptive MTU (§3.2/§9.3):** the DATA payload budget is
  **profile-driven** (`Profile.max_payload`, u16), not the fixed 1424 constant,
  which becomes the absolute buffer ceiling (4096). Standard rungs ~1424; Realtek
  jumbo/A-MSDU rungs up to ~3967 — essential for large IDRs (512 KB ⇒ k≈370 at
  1400 vs k≈132 at 3967). The wire `payload_len` is self-describing, so RX
  decodes whatever `s` TX chose (even across a mid-stream profile change); `s` is
  fixed per FEC block. Dynamic switching rides the existing §9 profile selector —
  no separate MTU control loop.
- **Ruling — RX egress = SHM only (Option A):** §5.3 Option B (RTP
  re-packetization) is out of scope for v1; a `udp` egress on a `frame-shm`
  stream is rejected at config load (wire payloads are frame fragments, not RTP).

**Amended:** §3.2 (profile-driven payload budget + 11-B FEC subheader ref), §4 +
§4.1 (frame block boundary + IDR-flag classifier), §5.1 (invariant relaxed) +
new §5.1a (FrameFramer), new §6.3a (frame reassembly + SHM egress), §9.3
(`max_payload`), §14 (built/default-off) + new §14.1 (GF(256) RLC scheme, wire,
adaptive policy), §15.1 (`frame-shm` live), §15.2 (per-stream `fec` block), new
§15.4 (venc_frame_ring slot format).

**Still deferred to bench (not blocking):** FEC rate seeds (gate-2 ρ pending;
`i_rate` 250 / `p_rate` 100 per-mille / `min_k` 3 as config seeds); per-profile
`max_payload` seeds (bench-derived); on-device jumbo MPDU injection verification
on real Realtek radios (loopback/bench unaffected).

## Pass 16 — §15.5 REST control plane + frame-shm stats mapping (2026-07-12)

Implementation begins (PR B, feature/rest-control-plane). Spec amended FIRST
(this commit); code follows. Two operator decisions taken up front:

- **Decision — control scope = core write set.** The REST plane exposes the
  read/observability surface (`GET /stats`, `/stats/stream` SSE, `/info`,
  `/health`) plus the four live knobs that were previously boot-time JSON only:
  `POST /csa` (replacing the stdin trigger, now **removed**), `POST /link/profile`
  (§9.7 pin = the MCS+bitrate operating-point lever), `POST /fec` (§14.1 rate
  retune), `POST /stats/reset`. A TX-power override is deferred: it fights the
  §9/§10 per-tick selector power write and needs an override-latch — a later pass.
- **Decision — LAN-bindable, no auth.** Matches the §13 data-plane posture and
  venc's own same-SoC `/api/v1/set`. `control.bind` is operator-chosen
  (`127.0.0.1` for host-local, a routable addr on a trusted net); no token. The
  server is single-threaded, folded into the event loop (0 ms-timeout poll per
  tick, one bounded request/conn, dropped-not-awaited), so it cannot stall the
  flight path. Secrets (`csa.psk`) are never echoed by `/info`.

Also pinned: the **frame-shm stats gap**. On a `frame-shm` egress the RX path
runs `FrameReassembler`, whose per-frame counters were not surfaced — `recovered_fec`
read 0 even when FEC recovered frames. §15.3 now maps the reassembler counters
onto the stream fields (`recovered_fec`←`frames_fec`, `decode_errors`←`decode_failures`,
`dropped_superseded/deadline`←`frames_superseded/deadline`) and adds three
additive fields — `frames_fast`, `frames_unrecoverable`, `malformed` — which
stay 0 on UDP streams.

**Amended:** new §15.5 (control plane: endpoints, JSON, MUT class, bind/no-auth,
stdin-trigger removal); §15.2 (`control` config block); §15.3 (three new stream
fields + frame-shm mapping prose).

**Deferred (not blocking):** TX-power override knob (override-latch design);
per-instance direct-to-REST dashboard mode (the UDP-push fleet bridge ships now
in `tools/link_monitor.py`, PR #18).

**Addendum — runtime pin snap (found in E2E-over-UDP validation).** Exercising
`POST /link/profile {min:1,max:1}` against a live udp-air pair showed the link
report `state=PINNED` but the operating point staying put (`profile` unchanged)
instead of snapping to the pinned rung. Root cause: `Selector::evaluate()`'s
`min==max` branch set `state_="PINNED"` and returned **without** moving `rung_`;
config-time pins worked only because the ctor clamps `rung_`, but the §15.5
runtime path (`set_profile_pin`) relied on `evaluate()`, which never re-clamped.
Fix: the pin branch now snaps `rung_` to the pinned rung and emits the commit
(direction-agnostic, no flap bookkeeping — the pin overrides adaptation). §9.7
clarified to state the snap explicitly; `selector_test` gains a runtime-repin
case (down, up, idempotent no-commit, unpin-resumes). Re-verified live: pin
drove `active_profile` 6→1 with `mcs`/bitrate following, both TX and RX panes
reflecting profile 1. Also confirmed in the same run: full REST read surface,
`/fec`→400 on a UDP stream, `/csa`→409 on TX, `/nope`→404, `/stats/reset` zeroing
counters, native SSE, and the fleet monitor aggregating both instances over UDP.
The two-target udp-air `tx:[a,b]` split-fanout under-reports `delivered` vs the
wire (dev-backend artifact, not a stats bug: single-adapter and real diversity
both account exactly — verified `delivered==uniq==wire` on a 1-adapter run).

## Pass 17 — quiet HEARTBEAT + passive discovery API (2026-07-12)

Operator approved the missing application behavior for the already-pinned §3.8
HEARTBEAT wire type and the discovery surface identified by the transport audit:

- Every node emits HEARTBEAT at 1 Hz only while otherwise quiet. Any successfully
  submitted packet resets the quiet interval; active links add no keepalive load.
- HEARTBEAT refreshes bounded node presence only. It cannot create stream state
  because its normative 11-byte body has no stream fields.
- DATA continues to populate admission candidates and latches. New read-only
  `GET /api/v1/discovery` returns bounded `nodes[]` plus DATA-derived `streams[]`
  with identity, counts, monotonic first/last-seen stamps, and latch state.
- Discovery introspection is observational: it never changes admission, latch
  selection, teardown, or output routing.

**Amended:** §3.8 (1 Hz quiet-only emission rule); §15.5 (discovery endpoint and
bounded JSON contract). Code follows in separate commits.

## Pass 18 — pre-diversity + local-drop observability (2026-07-12)

The approved transport-audit sequence resolves the advertised-but-zero
`loss_prediversity` gauge and separates local host backpressure from air loss:

- Pre-diversity loss is a post-latch, per-stream/per-adapter original-DATA
  sequence-opportunity estimator. Bounded reorder fills missing opportunities;
  duplicates and RETRANSMIT packets are excluded. Adapter totals are aggregated
  only at stats formatting time.
- Stats reset zeros estimator totals without moving sequence anchors.
- Stream stats add frame-SHM full/oversize/bad-slot counters. Adapter stats add
  Linux `SO_RXQ_OVFL` kernel receive-queue loss as `kernel_drop`; it is not
  folded into synthetic/backend `drop`.

**Amended:** §3.7 (normative estimator); §15.3 (additive local-drop fields and
mapping). Code follows separately.

## Pass 19 — UDP broadcast/sniffer bench backend (2026-07-12)

The approved transport-audit follow-up adds a dedicated RF-broadcast analogue
without changing point-to-point UDP-air behavior:

- `air.kind "udp-broadcast"` is one shared IPv4 broadcast channel, expressed as
  exactly one TX destination plus one shared RX listen endpoint.
- TX enables `SO_BROADCAST`; multiple nodes may share the RX port.
- RX validates complete waybeam packets and filters the local originator before
  core delivery, matching the RF backends' passive-sniff/self-filter boundary.
- Adapter stats add `filtered`; malformed/self frames are counted separately
  from accepted and synthetic-dropped traffic. Kernel-monitor's existing
  internal filter counter maps to the same field.
- Optional `pace_mbps` serializes broadcast datagrams so frame-sized host bursts
  do not create accidental UDP queue loss on systems with a small `rmem_max`.

**Amended:** §15.3 (`filtered` adapter field); §16.3 (backend config and receive
semantics). Code follows in a separate commit.

## Pass 20 — JSCC architecture review + live Ethernet harness (2026-07-12)

Reviewed `docs/waybeam-jscc-controller.md` against the implemented frame-SHM,
FEC, ARQ, deadline, stats, and venc-control paths. This pass makes no protocol
ruling and does not amend `PROTOCOL.md`; it separates existing mechanisms from
controller-roadmap requirements in `docs/jscc-controller-review.md`.

Corrections/findings: the implemented Cauchy cap is `k+r<=256`, source and
repair symbols already carry `k`, current deadlines are relative to first RX
observation rather than absolute display slots, and there is no per-frame
encoder QP/size actuator (the separate `GET /request/idr` actuator is available).
The live encoder produced about 90 fps despite a configured 144 fps; the first
passive GDR baseline contained no IDR markers (a later run naturally contained
one, and the explicit `GET /request/idr` actuator is available).

Added a repeatable SSC338Q-to-x86 Ethernet bench using the encoder-owned
`venc_frame` ring, two UDP diversity observations, frame reassembly/FEC, an x86
egress ring, and real GStreamer H.265 decode. The runner persistently deploys
the cross-build and craft config, restores the encoder config after each run,
and records per-frame CSV plus link stats. Initial 300-frame baseline passed at zero loss;
with 10% independent loss per path, post-diversity loss measured 1.0%, 16 frames
were FEC-recovered, and none were unrecoverable.

## Pass 21 — frame-SHM size/cadence observability (2026-07-12)

Live JSCC bench review found that frame byte size and arrival cadence existed
only in the finite consumer trace, while TX-side frame-SHM activity was not
visible in streaming stats. The common measurement boundary is the local SHM
ring: successful consumer reads on TX ingress and successful producer writes on
RX egress. Stream stats add cumulative count/bytes, last/min/max frame size,
latest inter-frame interval, and a 1/16 EWMA of interval variation. Failed ring
operations stay in their existing full/oversize/bad-slot counters.

The monitor review also found that its long-lived bridge retained superseded
sessions indefinitely, report age was graphed on the delivery scale without
explaining its expected sawtooth, and TX streams displayed RX-only reassembly
outcomes. Those are presentation/retention defects, not wire behavior.

**Amended:** §15.3 (additive frame boundary telemetry and ingress/egress
mapping). Code follows in separate commits.

## Pass 22 — explicit decoder-generation recovery (2026-07-12)

Live VFRM inspection showed that a fresh Radeon decoder could continuously drain
the current ring yet never display after a pipeline disconnect/reconnect. Across
roughly 800 post-reset access units the stream carried trailing slices and
periodic VPS/SPS/PPS, but no NAL type 16–23 random-access picture and no IDR
metadata flag. Restarting the Ethernet bench restarted venc and happened to
bootstrap the decoder, proving that ring consumption is not decoder readiness.

VFRM v1 intentionally has no consumer-generation signal, and Radeon keeps
draining while its local display pipeline is disabled. The SHM producer
therefore cannot infer a decoder reset from `read_idx`, inode, epoch, or futex
state. Periodic IDRs would undermine the GDR frame-size invariant. The protocol
adds an explicit, best-effort RECOVERY_REQUEST return and a ground-local REST
trigger; the matched TX rate-limits requests and calls venc's existing IDR
actuator once. Steady-state GDR remains unchanged.

**Amended:** §3.1/§3.9 (RECOVERY_REQUEST) and §15.5
(`POST /api/v1/video/recover`). Code follows in a separate commit.

## Pass 23 — UDP broadcast virtual diversity (2026-07-12)

The live Ethernet load investigation found that the JSCC harness still used
ordinary UDP with two TX destinations. That sends and copies every air frame
twice on the constrained vehicle, contrary to the intended RF-broadcast model.
Pass 19 allowed multiple nodes to share the broadcast channel but constrained
each process to one RX socket, so one ground process could not expose two
pre-diversity observations.

The operator ruled that the vehicle sends each air frame once to an IPv4
broadcast channel while the ground opens multiple passive listeners. A
udp-broadcast config therefore has exactly one TX endpoint and one or more RX
endpoints; repeated RX endpoints are explicitly valid virtual adapters. Linux
broadcast fanout delivers one copy to each `SO_REUSEADDR` socket. Independent
synthetic loss remains per adapter; without injected loss the observations are
identical and deduplicate normally.

**Amended:** §16.3 (multi-listener virtual diversity). Code follows in a
separate commit.

## Pass 24 — deterministic JSCC inner decision (2026-07-12)

The approved JSCC roadmap needs a per-frame protection decision without
prematurely fitting independent-loss math to unmeasured RF bursts. The inner
controller therefore accepts a conservative predicted loss-symbol count from a
separate estimator, clamps parity against configured and GF(256) capacity, and
reports when the requested protection cannot fit. ARQ is eligible only when
P95 RTT plus resend airtime and guard fit after original source transmission.
Deadline discard is reserved for a frame whose original transmission cannot
finish by its deadline; statistical under-protection alone does not prove that
the frame is doomed.

The decision is pure, uses injected timing, and has stable reason names for
replay and diagnostics. Existing fixed FEC rates remain the fail-safe runtime
fallback until the measured estimator is connected.

**Amended:** §14.2 (inner-controller inputs, clamps, ARQ/deadline gates, and
reason-code contract). Code follows separately.

## Pass 25 — release supersedes every older incomplete frame (2026-07-12)

The operator confirmed the wfb_ng-style latency rule: once the receiver has a
newer available frame/block, waiting for any older incomplete frame is invalid.
The protocol already said a newer block supersedes older incomplete blocks, but
`FrameReassembler` retained a two-block window. Ordered RX delivery prevented
late output in common cases, yet stale reassembly state and counters survived
until a later block or deadline.

Frame-SHM reassembly now has a zero-block retention window. Seeing block `N`
finalizes every older incomplete block as superseded; releasing `N` can never
be delayed by, or followed by, an older frame. Late symbols for finalized blocks
remain ignored.

**Amended:** §6.3a (zero-retention release/supersession rule). Code follows
separately.

## Pass 26 — expedite authorized ARQ retransmissions (2026-07-13)

Ethernet gate-3 testing separated sub-millisecond network propagation from
application recovery latency. Two avoidable host-side waits remained: the TX
main loop could sleep on local stream ingress while a return packet was ready,
and the UDP serialization model appended an authorized retransmission behind a
whole queued encoded-frame burst. The latter can consume most or all of a
short JSCC deadline even though the §5.3 scheduler has already accepted,
budgeted, and freshness-sorted the resend.

Authorized retransmissions now use a deadline-priority serialization lane, and
air-return readiness participates in the TX ingress wait. The §5.3 airtime cap,
per-requester partition, attempt cap, deadline gate, global hold-down, and
freshness ordering remain unchanged. This is bounded priority for packets the
scheduler already admitted, not an unlimited ARQ fast path.

**Amended:** §16.3 (paced retransmit priority and return-path wakeup). Code
follows separately.

## Pass 27 — causal JSCC estimator shadow telemetry (2026-07-13)

Offline loss-matrix replay showed that an empirical quantile can save parity,
but also that estimator lag creates deadline failures under changing and
correlated loss. Runtime adaptive FEC is therefore not authorized. The next
measurement stage observes post-diversity source-symbol loss at frame-SHM RX
with a causal trailing-window P95: every block's prediction is fixed before
that block contributes an observation.

The initial 120-block window, 20-sample threshold, and zero cold start are
diagnostic seeds, not an RF model. Additive stats expose the latest prediction
and observation, cumulative underprediction, hypothetical parity, and sample
count. Reset starts a new estimator generation. Fixed §14.1 parity remains the
only runtime authority.

**Amended:** §14.2 (shadow estimator ordering and non-enforcement); §15.3
(additive `jscc_*` receiver stream fields). Code follows separately.

## Pass 28 — protection-aware JSCC shadow (2026-07-13)

The first runtime shadow predicts missing source symbols. That is useful loss
telemetry but is not sufficient for parity allocation because repair packets
can also be lost, and raw symbol counts scale with block size. Deterministic
replay now measures transmitted repair demand, normalizes it by `k`, and marks
unrecoverable observations as censored lower bounds.

A 120-block maximum with a 10% cold-start rate matched fixed FEC under steady
15% independent loss while selecting less parity, but added one failure under
incremental loss. It remains non-enforcing. Additive fields expose this second
shadow independently; the existing source-loss fields retain their meaning.

**Amended:** §14.2 (repair-demand definition, normalization, censoring, and
shadow-only seeds); §15.3 (additive protection-shadow fields). Code follows
separately.

## Pass 29 — exact frame-FEC transmission counters (2026-07-13)

Protection shadow telemetry reports hypothetical parity, but live stats did
not export `FrameFramer`'s existing source/repair counters. Comparing against
an inferred bitrate would conflate frame size, metadata, headers, and IDR rate.
Additive TX stream fields now expose the exact emitted source and repair symbol
counts, capacity-disabled frames, and IDR-classified frames. No transmission
behavior changes.

**Amended:** §15.3 (additive frame-SHM TX symbol and classification counters).
Code follows separately.

## Pass 30 — live UDP loss-ramp control (2026-07-13)

Restarting the Ethernet receiver to change synthetic loss resets the causal
estimator and cannot test adaptation lag. A bench-only control endpoint may
therefore retune `udp`/`udp-broadcast` synthetic RX loss in-process. It is
bounded to 0–1000 permille, rejects non-UDP backends, changes no persistent
configuration, and is reset to zero after scripted ramps. Monitor and Devourer
behavior is explicitly untouched.

**Amended:** §15.5 (UDP-air bench synthetic-loss control). Code follows
separately.

## Pass 31 — explicit JSCC feedback and truthful TX shadow (2026-07-13)

The protection-aware estimator lives on RX, while exact `k`, frame class,
transport queueing, and FEC actuation live on TX. A per-frame controller cannot
be wired honestly by joining independent one-second stats snapshots or by
assuming missing RTT/airtime values. Enlarging the fixed v0 `LINK_REPORT` would
also be wire-incompatible under the same version.

Add a separate fixed-size `JSCC_FEEDBACK` packet carrying normalized causal
repair demand, measured P95 ARQ RTT, readiness, estimator sample count, and the
newest observed block. TX caches only exact-target, monotonic feedback. The
first runtime integration is non-enforcing: it combines fresh feedback with
TX-local frame/deadline/airtime inputs and reports the pure §14.2 decision.
Missing or stale input explicitly selects authored fixed-policy fallback; zero
must never stand in for an unavailable measurement. Shadow floor, cap, guard,
freshness, and RTT sample threshold are operator-authored and have no implicit
optimistic defaults.

**Amended:** §3.1/§3.10 (new additive packet); §14.2 (runtime shadow validity
and optional configuration); §15.2 (frame-SHM scope). Code follows separately.

## Pass 32 — make RTT readiness independently checkable (2026-07-13)

Codec integration exposed that Pass 31 carried repair sample count but not RTT
sample count. Because `min_rtt_samples` is authored on TX, an RX-only readiness
bit cannot prove the TX threshold was met. Add `rtt_samples` and define the RTT
valid bit as "estimate present"; TX applies its own configured minimum. This
widens only the new, not-yet-deployed additive packet from 35 to 37 bytes.

**Amended:** §3.10 (`rtt_samples`, offsets, size, and readiness semantics). Code
follows separately.

## Pass 33 — auditable runtime-shadow telemetry (2026-07-13)

A non-enforcing controller is useful only if an operator can prove which inputs
produced each latest decision and why fallback was selected. Add TX stream
stats for decision counts, validity, named fallback/reason, all §14.2 numeric
inputs, outputs, and feedback freshness. Add RX rolling RTT sample count/P95 so
the feedback readiness input is independently visible. All fields are additive
and zero/empty where the shadow does not apply; no actuator is authorized.

**Amended:** §15.3 (runtime decision-shadow and rolling RTT telemetry). Code
follows separately.

## Pass 34 — separate decoder recovery from bitrate ownership (2026-07-13)

The Ethernet shadow bench proved that `RECOVERY_REQUEST` reached vehicle TX but
all requests failed when `venc.enabled=false`. That flag currently gates both
the persistent bitrate writer and the independent `/request/idr` actuator. A
bench must not claim bitrate ownership merely to bootstrap a decoder or gather
ARQ RTT samples.

Add a separate, default-off `venc.recovery_enabled` permission. It authorizes
only the existing rate-limited IDR request; `venc.enabled` retains its existing
bitrate-write meaning. The two permissions are independent so the real encoder
settings remain operator-owned during Ethernet measurements.

**Amended:** §3.9 (independent recovery permission); §15.2 (configuration and
defaults). Code follows separately.

## Pass 35 — bounded P-frame ARQ experiment (2026-07-13)

Repeated high-parity replay rejected a reactive fixed-FEC guard because no
causal repair-demand estimator can protect the first frame of an unseen upward
loss step. The existing pre-diversity metric is lifetime-based, and its 100 ms
report cadence is not a frame-leading signal. Ethernet measurement instead
showed a 1 ms P95 NACK-to-retransmit loop, inside the 16 ms P-frame deadline.

Add an opt-in frame-SHM `arq_mode:"all-frames"` experiment. A new
`PFRAME_ARQ` DATA flag makes non-IDR frames retransmit-eligible without granting
the longer IDR deadline; the existing `ARQ` flag retains its importance and
I-frame deadline meaning. Unknown receivers ignore the additive bit and fail
safe as IDR-only. Default behavior remains unchanged, fixed FEC remains
authoritative, and `arq_frames` makes the experiment auditable.

**Amended:** §3.2/§3.3 (flag and NACK eligibility); §4.1/§5.1a/§5.3/§6.4
(classification, deadline, and resend semantics); §15.2/§15.3 (configuration
and telemetry). Code follows separately.

## Pass 36 — §3.11/§14.3 spatial cache repair, v1 IP transport (2026-07-16)

Co-located adapters cannot decorrelate a whole-site fade; the gate-2 ρ→1 tail
is precisely where diversity and ARQ fail together (§18). The operator adopted
the Cache Controller V3.1 concept (external design doc + reference harness)
as a spec extension, with these rulings pinned against V3.1 where waybeam-link
constraints bind:

1. **v1 transport is UDP/IP only** (Ethernet/routed side-link between ground
   sites), operator-provisioned static endpoints, no on-air discovery. The RF
   cache binding is reserved but deferred until after the gate-2 vehicle
   verdict — the §3.11 formats are transport-agnostic so RF later costs no
   re-numbering.
2. **A cache is an ordinary waybeam-link node**: cache identity = `originator`,
   epoch = the target's `session_id`; §3.11 bodies reuse the NACK/LINK_REPORT
   target-descriptor split. No new identity space.
3. **CACHE_REPLY wraps verbatim §3.2 DATA packets** (one per datagram). The
   aggregator revalidates through the normal decode; a cache cannot inject
   anything a radio could not, and no second decode path exists.
4. **Cache symbols feed the §6.3a reassembler directly**, bypassing §6.1/§6.2
   per-adapter state and the §3.7 estimators — a cache is not an adapter and
   must not inflate `diversity` or perturb pre-diversity loss.
5. **Zero-block retention stands**; the repair window ends at next-block
   arrival or the §8 deadline. V3.1's longer-playout-buffer variant rejected.
6. **Cache repair runs in parallel with vehicle ARQ**, deviating from V3.1's
   serial cache-before-vehicle order: IP repair spends no RF airtime, so the
   airtime argument does not bind, and latency-first (§9.0) argues against
   delaying the measured 1 ms Ethernet / 2–4 ms RF NACK loop behind a cache
   phase. §5.3/§6.4/§12 are untouched; dedup + existing budgets absorb the
   redundancy. Revisit only with an RF cache transport.
7. **Local close is implementable, not airtime-modeled**: earliest of
   EOB+`tail_grace_ms`, quiet timeout floored by `min_collect_ms`, or
   `hard_close_ms` — ms-granularity seeds, all §17 RE-DERIVE (V3.1's
   µs-precision expected-burst-end anchor is unimplementable at the current
   event-loop cadence and adds no safety the floor doesn't).
8. **Budgets count requested allowances** (IP loss is unobservable at the
   aggregator; requests are the conservative ledger): per-block cap
   `min(⌈k·fraction⌉, absolute_limit)`, futility skip above it (cache phase
   only — vehicle ARQ unaffected), ≤2 sequential attempts, per-request
   `reply_limit`.
9. **§13 additions**: request amplification (rate cap + `request_id` dedup +
   exact-target match), reply forgery (outstanding-request + subset +
   allowance + full revalidation), status poisoning (static endpoints only).

**Amended:** §3.1 (types 0x8–0xA), §3.11 (wire formats), §13 (three rows),
§14.3 (controller rules + seeds), §15.1 (cache sockets are control-plane),
§15.2 (`cache` config), §15.3 (`cache_repair`/`cache_store` stats), §17 (close
timer knob row). Code follows separately in the same PR.

**In-pass correction (first Ethernet bench, same unmerged PR):** rule 6
originally required `block_id ≤ newest_block`. The bench showed that bound
suppresses most legitimate requests — a status snapshot is one interval
stale, so the blocks needing repair are always past the last reported
`newest_block` (72 of 86 closed-deficit blocks suppressed in the first run).
Eligibility now enforces only the `oldest_block` bound; `newest_block` is
lag telemetry. This matches the reference harness, which filtered on the
oldest bound alone.

## Pass 37 — §9.6 actuation contract v2: horizon frame caps (2026-07-16)

The FPV frame-size/radio adaptation design doc (operator-supplied) asks for
Salsify-style control: the encoder is told a maximum size for the next frame.
The venc side landed the actuator (`maxIBytes`/`maxPBytes` + FRAMEBITS_FIRST,
venc PR #181 + clamp fix). Rulings for the link side:

1. **A per-frame budget channel is OUT OF SCOPE** (operator). venc control is
   HTTP with persist-on-set; per-frame commands would need a new transport.
   The way around it: caps are pure functions of SLOW inputs (rung budget,
   snapped frame cadence, I-frame deadline, FEC rates), so recomputing them
   only when an input changes loses nothing a per-frame channel would win —
   the per-frame *enforcement* already lives inside venc's rate controller.
2. **Cap formulas** (§9.6): maxP = one frame period of rung budget net of P
   parity; maxI = the I-class recoverable deadline (§4.1/§8) of rung budget
   net of I parity — an I-frame is sized to what ARQ/FEC can still rescue.
   Cadence is the measured frame-shm interval snapped to the ladder fps set
   (jitter must not churn caps); headrooms seed 1000‰, RE-DERIVE.
3. **Caps ride the §9.5 transition** under the same write-on-change/holdoff
   discipline as bitrate; `venc.frame_caps=false` opts out without giving up
   bitrate authority. Ceiling = min(configured ceiling, §14.1 GF(256)
   eligibility at the rung symbol size).
4. **Actuator-state telemetry** (§15.3): commanded bitrate/caps + pushes/
   failures + `venc_settling` (within `venc.settle_ms` of the last accepted
   change) — the design doc's commanded/effective/pending model collapsed to
   one boolean because venc's `/set` applies synchronously.
5. **Verification order** (operator): a full actuation harness on the
   UDP-air backend FIRST (fake venc endpoint, loss-driven rung transitions),
   before the radio and kernel-monitor backends are verified on the rig.

**Amended:** §9.6 (horizon caps + actuator model), §15.2 (`venc` knobs),
§15.3 (link `venc_*` fields), §17 (knob row + UDP-first note). Code follows
separately in the same PR.

## Pass 38 — §14.2 enforcement gate: shadow becomes actuating (2026-07-16)

The Pass 24–35 shadow program existed to justify exactly this flip with
data. The operator's controller design (frame-size/radio adaptation doc) and
the agreed sequencing make §14.2 the per-frame inner loop of the JSCC
controller; horizon caps (Pass 37) bound what the encoder may produce, and
this pass makes the per-frame protection decision live. Rulings:

1. **Opt-in and per-frame fail-safe.** `jscc_shadow.enforce=true` actuates a
   VALID decision's parity/discard/ARQ outputs for that one frame; any named
   fallback selects the fixed §14.1 rate for that frame. Missing data can
   therefore never zero out authored protection, and disabling the flag
   restores pure shadow with no other behavior change.
2. **Discard only on a valid decision** (`deadline_unreachable`): the
   transient-overload guard drops the frame at TX rather than queueing stale
   video. A fallback frame always transmits.
3. **The ARQ gate touches `PFRAME_ARQ` only.** The IDR `ARQ` bit is never
   removed by per-frame timing — importance outlives one window and §5.3
   already deadline-gates resends.
4. **Flip criteria stated as shadow telemetry** (≥99% valid decisions,
   <1% repair underprediction, RTT readiness held), verified UDP-first per
   the §17 ordering.

**Amended:** §14.2 (enforcement rules + flip criteria), §15.2 (`enforce`
flag), §15.3 (`jscc_enforced_frames`/`jscc_discarded_frames`). Code follows
separately in the same PR.

## Pass 39 — §9.11 FPS ladder (2026-07-16)

Third step of the controller sequencing: FPS as the last-resort actuator,
outside the §9.1 cascade. Rulings:

1. **Envelope semantics:** v1 operates in `[min, preferred]`; `preferred` is
   the recovery target and v1 never commands above it (`max` reserved for a
   future ruling on above-preferred cadence). Values must be §9.6 ladder
   members.
2. **Reduce trigger is radio-loop exhaustion**, made measurable: floor rung
   held AND smoothed report loss ≥ `distress_milli` for `reduce_after_ms` —
   "the cascade has nothing left", not any single bad frame or FEC block.
3. **Asymmetric hysteresis** per the design doc: restore needs off-floor +
   low loss for ~2.7× the reduce window, plus per-step dwell and a settle
   freeze after each command.
4. **Stale feedback holds** — the rung fail-safe (§9.8) owns degradation on
   silence; an fps stutter requires positive evidence.
5. **Cap coupling:** the commanded ladder fps becomes the §9.6 cadence input
   while the ladder is enabled — measurement lags a change by ~1 s and would
   brief the caps wrong across every transition.

**Amended:** §9.11 (new section), §15.2 (`venc.fps_ladder`), §15.3 (link
`venc_fps`), §17 (knob row). Code follows separately in the same PR.

## Pass 40 — 20 MHz lock + high-cadence ARQ cutoff (2026-07-16)

Two operator rulings from the pending register (see `docs/followup-plan.md`
for the working order):

1. **R-D resolved: v1 is fleet-wide 20 MHz.** Dynamic 20/40 width is out of
   scope for this version; 40 MHz is revisited later, and only as a
   CSA-shaped campaign behind a hardware verdict for the deployed chips.
   Recorded in §18; no code change (nothing implemented 40 MHz-dynamically).
2. **No ARQ above 100 fps (101–144).** Operator: "10 ms is the lowest
   comfortable window." Architecturally consistent: §6.3a zero retention
   supersedes an incomplete block at next-frame arrival, so above 100 fps
   even the I-frame class has <10 ms of usable receiver-side repair time.
   The frame-SHM TX stamps neither `ARQ` nor `PFRAME_ARQ` while the §9.6
   cadence input exceeds `policy.arq.arq_max_fps` (seed 100; 0 disables),
   §14.2 marks those frames not ARQ-capable, and `arq_cutoff_frames` counts
   the suppressions. Above the cutoff, recovery is FEC + diversity + cache.

**Amended:** §4.1 (cutoff), §15.3 (`arq_cutoff_frames`), §17 (knob row),
§18 (width lock). Code follows separately in the same PR.

## Pass 41 — §3.5 acceptance filter enforced at TX ingest (R-A) (2026-07-16)

The register's R-A gap: §3.5 rules the acceptance filter, but the TX event
loop forwarded any target-matching LINK_REPORT to the selector — and since
Pass 39 to the fps ladder. Pinned enforcement semantics:

1. Filter at ingest, before selector AND ladder consume the report.
2. `preferred_originator` configured ⇒ only that originator passes; its
   session follows reboots (§2 per-boot nonce of the same tail number).
3. Unconfigured ⇒ first-latcher per target; a same-originator session change
   follows immediately (reboot), a DIFFERENT originator takes the latch only
   after `relatch_ms` of latched-reporter silence (seed 4 ×
   `report_timeout_ms` — the §9.8 watchdog fires first, so the fail-safe
   still owns the gap).
4. Rejections counted (`reports_rejected`); the §3.5 plausibility
   cross-check stays future bench work.

**Amended:** §3.5 (enforcement point + relatch seed), §15.3
(`reports_rejected`). Code follows separately in the same PR.

## Pass 42 — R-B resolved by measurement: no parity offload (2026-07-16)

The feared §14.2 × §14.3 loop — cache-completed blocks masking air loss,
feedback demand dropping, the enforcing TX under-protecting — does NOT
materialize. `tools/cache_offload_bench.sh` (enforce ON, cache OFF vs ON at
identical loss, paced udp-broadcast):

| drop | parity ratio OFF | parity ratio ON | cache repaired |
|---:|---:|---:|---:|
| 150 ‰ | 0.426 | 0.421 (−1 %) | 35 |
| 80 ‰ | 0.301 | 0.259 (−14 %) | 8 |
| 50 ‰ | 0.207 | 0.234 (+13 %) | 8 |

The gap flips sign across runs — noise, not offload. The structural reason
is pinned in §14.2: the demand estimator is a trailing-window MAXIMUM with
censored lower bounds, and the blocks that set the max are exactly the ones
the cache cannot complete (deficit beyond the §14.3 cap ⇒ censored high
samples). Cache masking of shallow blocks therefore cannot pull TX
protection down. No code change; the bench stays as the regression guard,
and any future estimator-shape change must re-run it (adopting air-only
observation only if the offload then appears).

**Amended:** §14.2 (immunity property pinned + re-check requirement).

## Pass 43 — §10 rx-node power_map rejected at config load (2026-07-16)

The register's §10 item: the power resolve runs only in the tx-node selector
commit, so a `power_map` on an rx-node adapter (including the ground's
designated uplink TX adapter) was silently loaded and never applied — a
2026-07-11 desk run swept it as a no-op. Ruling: config load rejects it with
an explanatory error; explicit beats silent. Bringing the ground return
uplink under real power control remains a separate future ruling, taken only
if gate 4 shows return-margin problems.

**Amended:** §10.2 (enforcement note). Code follows separately in the same
PR.

## Pass 44 — kernel BPF pre-filter efficacy gauge (2026-07-18)

The kernel-monitor backend attaches a classic BPF program (`SO_ATTACH_FILTER`)
to each adapter socket that mirrors the §3.0 RX filter's cheapest checks
(frame-control, SA prefix `56:42`, optional `net_id`, payload magic `57:42`),
rejecting ambient WiFi in the kernel before the `recvmsg()` copy. `dot11_parse()`
stays the correctness gate; the BPF is a pure performance optimization and is
non-fatal on attach failure. That part changes no wire bytes and needs no ruling.

The observability question did: an operator cannot tune the filter without seeing
how much it rejects, but a per-socket true drop count was not plumbed. Ruling —
ship an honestly-labeled **estimate** rather than block on exact instrumentation.
`bpf_filtered` is derived from the interface's sysfs `rx_packets` delta minus all
userspace-observed frames, floored at 0. Because `rx_packets` is counted below
the packet-socket/BPF layer, the estimate also absorbs frames delivered to other
sockets on the same monitor interface and driver accounting skew — so §15.3 marks
it the sole field that does **not** map 1:1 to a §16.2 counter and names it a
coarse gauge, not an exact count. It is 0 on `RadioAir`/`UdpAir` (BPF N/A).

**Amended:** §15.3 (the `bpf_filtered` derived-estimate field and the 1:1-mapping
exception). Code follows separately.

## Pass 45 — full PR accuracy audit corrections (2026-07-18)

A spec-to-implementation audit before radio/multi-node testing found three
false-ready paths despite the green UDP/device matrix:

1. The malformed-`k` hardening incorrectly applied the GF(256) `k≤256` bound
   to source-only frames, contradicting §14.1's explicit oversized-block
   fallback. Repair/cache bitmaps remain bounded, while source-only reassembly
   now accepts the full wire `u16` range subject to producer geometry and the
   configured frame-size ceiling.
2. Pass 41's gate followed reporter reboot/re-latch, but Selector retained its
   independent first-wins source and epoch. Accepted identity transitions now
   reset the selector epoch/smoothing domain; replay rejection also gates the
   FPS ladder.
3. Pass 42's max-with-censoring argument was not structural. A deterministic
   all-cache-completable window drove demand from 100‰ to zero because cache
   sources were counted as air arrivals. Reassembly now tracks air attribution
   separately from unified decode state, and the regression holds demand at
   100‰ across cache completion.

The same pass scopes cache status per target stream, cache request dedup per
aggregator boot identity, rejects JSCC enforcement without `rlc256`, and makes
the normative `maxI≥maxP` cap invariant executable by lowering P when needed.

**Amended:** §3.5, §3.11, §9.6, §14.2, §14.3, §15.2 and the follow-up register.

## Pass 46 — monitor ARQ priority and phase-timing gate (2026-07-19)

The first N=2 monitor runs exposed a long retry tail rather than an RF airtime
limit. Monitor mode has no live TSF read, so host-delayed EOB arrival was being
delayed by the quiet-gap midpoint a second time; NACKs also shared a FIFO with
periodic reports, vehicle radio readiness followed an unbounded SHM drain, and
only UDP-air implemented the §16.3 priority lane.

The corrected path sends monitor NACKs immediately after repair-tail/FEC close,
keeps reports in a separate normal queue, services return-radio readiness before
live ingress, bounds SHM work to one frame per ring/iteration, drains accepted
resends in the NACK callback, and uses QoS TID 6 for monitor/devourer NACKs and
retransmits. The default re-NACK step is now 6 ms. Host-local microsecond phase
telemetry separates EOB→build, build→submission, ground submission→retransmit
arrival, combined build→arrival, and vehicle receipt→resend submission.

Guarded channel-161/HT20 verification used the standard `/usr/bin/waybeam`
producer, MCS 5, two ground monitor receivers, and an independent synthetic
100‰ receive drop on each ground adapter to force measurable residual loss:

- 10% configured FEC: 600/600 valid frames, zero unrecoverable; FEC recovered
  340 source symbols and ARQ recovered 5 source symbols across 5 frames.
  NACK-build→retransmit P95/max was 2.510 ms; vehicle
  NACK-receipt→resend-submission P95/max was 315 µs.
- 33% configured FEC: 600/600 valid frames, zero unrecoverable and zero ARQ;
  5,164 repair symbols versus 1,840 at 10% (2.8× repair traffic).
- Final-binary zero-FEC ARQ stress: 43 retransmitted symbols; combined
  NACK-build→retransmit P95 2.114 ms and max 4.164 ms, while vehicle
  receipt→submission P95/max was 190/273 µs. All 43 RTT samples stayed below
  the 6 ms target (stream RTT P95/max 3/4 ms).

This establishes the code/turnaround portion of the <6 ms ARQ gate and supports
10% as the present N=2 operating point, with ARQ as residual repair. The drop
was synthetic and independent, so real spatial correlation/range and the
devourer backend remain follow-up RF tests. The vehicle config and
`S95waybeam` AP service were restored after both runs.

**Amended:** §3.0, §6.4, §7.2–§7.4, §15.3, §16.3 and §17 gate 3.

## Pass 47 — physical N=2 walk fade closes gate 2 (2026-07-19)

A guarded 179.4 s vehicle walk exercised the final SSC338Q binary through the
real kernel-monitor path on channel 161/HT20. The standard `/usr/bin/waybeam`
producer ran at MCS5 with 10% GF(256) RLC. Ground used two monitor receivers:
`wlx40a5ef2f229b` RX-only and `wlx40a5ef2f2308` as RX plus the designated
return transmitter.

- The vehicle transmitted 5,378 frames and ground completed 3,491. Aggregate
  pre-diversity loss was 86‰ and post-diversity/pre-ARQ loss 24‰, so the second
  path removed about 72% of otherwise-lost symbols.
- FEC recovered 599 source symbols; ARQ recovered 52. Thus FEC supplied 92% of
  explicit post-diversity source-symbol recovery, leaving ARQ in its intended
  last-resort role. There were 232 partially observed unrecoverable frames.
- Both ground receivers were silent for 45.7 s total, including a 37.1 s
  continuous blackout. `2308` alone carried 14.0 s of marginal reception;
  `229b` alone carried only 0.6 s. Diversity is useful but becomes correlated
  and asymmetric at the range edge.
- Of the return reports scheduled after the measurement reset, 1,438/1,461
  (98.4%) reached the vehicle. NACK build→radio submission P95 was 608 µs and
  vehicle receipt→resend submission P95 was 926 µs. Full
  build→retransmission P95 rose to 6.525 ms at the RF cliff (core RTT P95 5 ms,
  max 8 ms), versus the earlier 2–3 ms stationary result. The local priority
  lane remains sub-millisecond; the edge tail is RF availability.
- The SHM trace had no PTS regression, malformed frame, ring overflow, or
  protocol decode error. Because the validator was terminated during guarded
  cleanup rather than allowed to reach EOS, the next stationary soak must
  obtain the explicit decoder EOS verdict.

Ruling: keep 10% as the N=2 monitor base rate. An adaptive 15–20% edge range
may be evaluated, but static 33% is not justified: it costs 2.8× the repair
traffic seen at 10% in Pass 46 and cannot repair the measured whole-site
blackout. Stationary follow-up order is (1) long N=2/MCS5/FEC10 decode soak,
(2) controlled receiver disable/re-enable, (3) diagnose the `229b` return-TX
failure, and (4) independent ARQ-cache verification over UDP/IP before any
monitor-radio cache proposal.

No protocol amendment: this pass supplies the real-RF evidence required by
§17 gate 2 and selects an operating point within the existing §14 mechanism.
Raw local evidence is retained under
`artifacts/pr26-monitor-diversity-20260719/realrf-walkfade-n2-fec10/`.

## Pass 48 — stationary soak, failover, return-TX isolation, and IP cache (2026-07-19)

The post-walk stationary sequence used the same final SSC338Q binary,
channel 161/HT20, MCS5, 10% GF(256), and the vehicle's current standard
`/usr/bin/waybeam` producer (90 fps in the restored operator configuration).

**N=2 soak.** The explicit decoder gate completed 9,000/9,000 frames including
101 IDRs with `bad_meta=0`, `bad_annexb=0`, `pts_regress=0`, and `decode=ok`.
At the consumer boundary there were no SHM/kernel drops, malformed packets,
adapter stalls, or wedges. FEC recovered 123 source symbols and ARQ 10; six
incomplete frames were superseded without upsetting the decoder. Core NACK RTT
P95/max was 5/5 ms. The apparent SHM-full count in the later shutdown snapshot
was created only after the finite validator exited while the producer remained
live; the boundary snapshot is the measurement.

**Receiver failover/failback.** Administratively disabling RX-only `229b`
made the liveness watchdog mark it stalled while `2308` continued both video
RX and return TX without a restart. The expected N=1 penalty appeared (19–21‰
post-diversity loss, heavy FEC/ARQ use). A bare `ip link up` left the CU driver
`NO-CARRIER`, at −100 dBm TX power, and packet-silent. Reapplying the complete
monitor sequence (down → monitor type → up → MTU/channel) restored RX;
waybeam-link cleared `adapter_stalled` automatically. A post-rejoin 900-frame
decoder run passed with both adapters receiving, nine FEC source recoveries,
zero ARQ, and zero new unrecoverable frames.

**`229b` return-TX isolation.** With `229b` designated TX, 915 AF_PACKET sends
returned success and `tx_failed` stayed zero, but Linux `tx_packets/tx_bytes`
remained exactly 1,854/114,691, the adjacent `2308` captured zero stamped
returns, and the vehicle RX counter stayed zero. The same-session `2308`
control advanced netdev counters by 200 packets/12,600 bytes, was captured by
`229b` (20 captured, 39 filter hits), and reached the vehicle (272 RX frames;
103/146 scheduled reports received during the sampled window). This is below
the waybeam wire/core: keep `229b` RX-only. Kernel-monitor still needs an
honest silent-TX/wedge gauge because successful `send()` alone cannot detect
this driver/device failure.

**Independent cache over UDP/IP, real RF collection.** A clean `229b` monitor
process acted as cache node 33 while the `2308` aggregator took a deterministic
150‰ post-radio receive drop. Cache status/request/reply used localhost UDP/IP;
vehicle ARQ remained live in parallel as §14.3 specifies. At the finite
consumer boundary the cache accepted 1,715 symbols, repaired 774 blocks,
rejected zero replies, and reported 992‰ health. The matched no-cache control
used the same radio/drop seed and nearly identical receiver windows:

| 150‰ N=1 stress | cache over UDP/IP | no cache |
|---|---:|---:|
| completed receiver frames in snapshot | 4,318 | 4,339 |
| FEC-recovered source symbols | 3,587 | 3,516 |
| ARQ-recovered source symbols | 1,010 | 1,645 |
| unrecoverable/superseded frames | **119** | **534** |
| vehicle resends / transmitted frames | 4,147 / 4,536 | 4,461 / 4,864 |
| full NACK-build→retransmit P95 | 21.751 ms | 19.977 ms |

Both finite consumers completed 4,500 byte-clean decoded frames. Cache reduced
unrecoverable frames by 77.7%, proving the real monitor-receiver → UDP/IP cache
→ aggregator path. It did not materially reduce resend load (0.914 versus
0.917 resend/frame), because cache requests and vehicle NACKs deliberately run
in parallel. The 20 ms ARQ tail is the forced 15% N=1 overload regime, not the
stationary N=2 operating point.

Ruling: UDP/IP cache transport is verified. Do not infer that the reserved RF
cache binding is ready: it would consume shared airtime and invalidates the
v1 parallel-ordering rationale. Before proposing it, add cache request/reply
latency telemetry and decide from measurement whether a fresh-cache-only,
strictly bounded grace before the first vehicle NACK is worth the latency.

No protocol amendment in this pass. Evidence is retained under:

- `artifacts/pr26-monitor-diversity-20260719/stationary-soak-n2-mcs5-fec10/`
- `artifacts/pr26-monitor-diversity-20260719/stationary-failover-229b/`
- `artifacts/pr26-monitor-diversity-20260719/stationary-returntx-229b-diagnosis/`
- `artifacts/pr26-monitor-diversity-20260719/stationary-cache-udpip-realrf/`
- `artifacts/pr26-monitor-diversity-20260719/stationary-cache-control-realrf/`

## Pass 49 — kernel-monitor silent-TX observability ruling (2026-07-19)

Pass 48 proved that AF_PACKET submission success is not a TX-liveness signal:
`229b` accepted 915 sends while its netdev TX counters remained frozen and no
adjacent receiver or vehicle observed a frame. Kernel-monitor nevertheless
hardcoded `tx_wedged=false`; the existing §9.10 watchdog was devourer-only
because its progress source was CCX TX reports.

Ruling: reuse the existing absence-only watchdog and its seeds, substituting
the TX interface's monotonic Linux `tx_packets` counter as kernel-monitor's
completion-progress source. Any progress clears the verdict; zero progress
with at least `wedge_min_submits` during a full window sets it. The action stays
observability-only—no automatic role swap, USB reset, or adaptation coupling.
The public `tx_reports` field remains honestly CCX-only and stays zero on
kernel-monitor; only `tx_wedged` consumes the netdev progress internally.

**Amended:** §9.10 and §15.3. Code follows separately.

Implementation reuses `TxWedge` unchanged at the policy/state-machine level.
`MonAir` supplies cumulative `(tx_submitted, netdev tx_packets)` while
devourer continues to supply `(tx_submitted, CCX tx_reports)`. Native build and
43/43 tests passed; the SSC338Q cross-build passed. The real same-session A/B
then produced the required verdict: silent `229b` reached 77 submissions and
`tx_wedged=true` after one window, while healthy `2308` reached 77 submissions
and stayed false. `tx_reports` remained zero on both monitor adapters.
Evidence:
`artifacts/pr26-monitor-diversity-20260719/stationary-txwedge-fix-final/`.

## Pass 50 — cache timing and bounded first-NACK lead (2026-07-19)

The aggregator now measures successful request submission→first accepted
reply and first request→cache-attributed block completion with its local
monotonic microsecond clock. Both publish cumulative sample/max values and a
trailing-512 nearest-rank P95. Completion also retires all requests for the
block, so later replies are rejected rather than inflating acceptance counts.

Real monitor-RF collection with the independent `229b` cache and localhost
UDP/IP control path measured 300 first replies and 195 completions in the
clean decoder window: P95 was 2.845 ms and 2.910 ms respectively (max 4.577
ms and 3.414 ms). The consumer decoded 900/900 frames with 11 IDRs, clean
metadata/Annex-B/PTS, and EOS. A longer timing-only sample agreed: first-reply
P95 2.828 ms across 1,404 samples and completion P95 2.792 ms across 878.

That evidence sets a 3 ms default (`nack_grace_ms`, validated 0..6; zero
restores parallel ordering). Cache replies and new requests are serviced
before NACK construction. Only a successfully sent request to an eligible
fresh cache can defer the exact block's first NACK; the hold is deadline-
clamped and cannot affect another stream/block or any re-NACK.

The matched 150‰ N=1 real-RF A/B used a warm-up drain and two 1,800-frame
decoded windows:

| metric | grace 0 ms | grace 3 ms | change |
|---|---:|---:|---:|
| decoder result | 1,800 clean | 1,800 clean | pass/pass |
| NACK packets | 623 | 483 | **−22.5%** |
| vehicle resends | 1,779 | 1,400 | **−21.3%** |
| deadline drops | 0 | 0 | unchanged |
| unrecoverable/superseded | 22 | 37 | not paired |

The last row is deliberately not used as an A/B verdict: changing resend
traffic changes which subsequent packets consume the deterministic synthetic
drop RNG, so the runs do not lose identical source symbols. Clean decode and
zero deadline drops establish that the bounded lead remains functional; the
NACK/resend counters directly establish the RF-work reduction.

The final bookkeeping audit made `blocks_repaired_before_nack` durable at
block scope even after individual gaps are filled. Its final semantics are
unit-tested; the earlier hardware samples used gap-local history and are
therefore deliberately not quoted here.

Evidence:

- `artifacts/pr26-monitor-diversity-20260719/stationary-cache-timing-realrf-decode/`
- `artifacts/pr26-monitor-diversity-20260719/stationary-cache-grace0-realrf/`
- `artifacts/pr26-monitor-diversity-20260719/stationary-cache-grace3-realrf/`

## Pass 51 — unprivileged frame-SHM egress attachment (2026-07-19)

The normal ground deployment runs waybeam-link as root because the
kernel-monitor backend requires raw packet access, while the local video viewer
runs as the desktop user. The previous producer mode `0600` therefore made the
otherwise-compatible `venc_frame_out` ring inaccessible without a manual
permission change.

Operator ruling: a waybeam-link frame-SHM egress producer publishes its POSIX
SHM object as mode `0666`, explicitly applied after creation so the producer's
umask cannot narrow it. Although the payload is described as viewer-readable,
the SPSC consumer must also update `read_idx` and `consumer_waiting`, so this ABI
cannot support a read-only consumer mapping. The ring remains same-host,
single-consumer, and producer-owned; no wire-format or layout change is made.

**Amended:** §15.4 (egress object access mode and consumer write requirement).

Implementation verification covered the real privilege split, not only the
unit test. A root kernel-monitor RX created `/venc_frame_out` as `0666`; the
already-running unprivileged radeon-vrx process attached directly and drained
it without a helper or manual permission change. The restrictive-umask unit
test, all 43 native sanitizer suites, and the SSC338Q cross-build also passed.

The live visual check separated stress behavior from egress behavior. With the
intentional 150‰ N=1 post-radio drop, a matched 10.4 s interval produced 89.93
fps at the vehicle but only 89.10 fps at ground, with ten unrecoverable frames
and visible jitter/artifacts; SHM itself had zero full/bad-slot deltas. After
restarting only the ground aggregator without synthetic loss, a 15.3 s interval
measured 89.97 fps at the vehicle and 89.94 fps at SHM egress, zero
unrecoverable frames, zero SHM full/bad-slot drops, 18 FEC-only frames, and two
ARQ-assisted frames. The operator reported perfect visual cadence. The small
clean ARQ sample measured 3.78 ms P95 NACK-build→retransmit, below the 6 ms
usefulness target; it is a clean-path confirmation, not a loaded-tail claim.

## Pass 52 — controller regressions and kernel-monitor actuation (2026-07-19)

All latest-code UDP controller gates were rerun after the cache ordering and
SHM-access changes:

- Cache-only and combined cache+vehicle-ARQ delivered byte-exact output at
  150‰ loss (280/300 and 278/300 frames respectively).
- Cache parity-offload stayed invariant: repair/source ratio 0.429 both with
  and without the cache, while the cache path remained active.
- The actuation checker accepted 47 formula-exact writes across eight rungs;
  JSCC enforcement exercised 230 valid/enforced frames, 175 deadline discards,
  and the feedback-stale fallback; the FPS ladder produced
  `90→75→60→75→90` with dwell and cap coupling intact.
- The 158 s all-controller soak exited every process cleanly, parsed 2,372
  schema-valid stats rows, delivered 3,031/4,740 byte-exact frames through its
  outage schedule, made 131 venc pushes with zero failures, enforced 3,019
  frames, repaired 196 blocks from cache, and restored rung 7 / 90 fps.

Kernel-monitor actuation was then verified without exposing the vehicle venc
API to controller-cadence writes. Vehicle HTTP actuation went over Ethernet to
a host fake-venc endpoint, while video and feedback used the physical 8812EU
TX→8812CU RX monitor link. Because the real encoder remained at 8,192 kbps, the
valid fake-actuator envelope was profiles 3..5; a preliminary 0..5 attempt was
discarded because fake demotion claimed 3,804 kbps without actually reducing
the source, violating §9.5's bitrate-before-MCS premise and overloading MCS0.

In the valid bounded run, a 15.45 s clean window held profile 5: 1,390 source
frames and 1,388 SHM frames, zero unrecoverable/ring drops, 14 FEC-only frames,
one ARQ-assisted frame, fresh reports, zero steady-state venc writes, and zero
actuation failures. NACK-build→retransmit P95 was 5.195 ms across 17 samples,
inside the 6 ms usefulness target.

A 200‰ ground synthetic-loss phase demoted 5→3 while reports stayed fresh. In
10.45 s it generated 496 NACKs and 1,153 vehicle resends; 338 frames used FEC,
184 used ARQ, and 251 remained unrecoverable. Ground NACK-build→inject stayed
56 us P95, but build→retransmit rose to 22.809 ms and the vehicle's local
NACK-receive→resend rose to 16.039 ms. This is ARQ-queue saturation when ARQ is
forced to be the primary repair layer, not RF propagation delay. It confirms
the operational rule: diversity/FEC/cache must remove bulk loss before ARQ;
the <6 ms gate applies at residual load, not an artificial 20% ARQ workload.

Returning to the clean receiver restored profile 5, fresh reports, and a final
10.25 s zero-unrecoverable/zero-ring-drop window with no steady actuator writes.
The vehicle was then restored to its original config hash and AP service.

Evidence:

- `artifacts/pr26-monitor-diversity-20260719/controller-monitor-bounded/`

## Pass 53 — FPS ladder preserves frame-aligned FEC block size (2026-07-19)

Operator correction: FPS is not a direct last-resort response to selector-floor
loss. The selector first chooses a bitrate that fits the PHY. If the resulting
encoded P frames become too small for useful frame-aligned FEC blocks, the FPS
ladder lowers cadence while retaining the bitrate target, increasing bytes,
source symbols, and absolute repair symbols per frame. The nominal/preferred
low-latency mode is 100 fps.

The live `video0.maxIBytes` and `video0.maxPBytes` fields are fully wired, but
they are ceilings rather than guarantees of realized frame size. Therefore the
closed-loop input is measured non-IDR Annex-B size at frame-SHM ingress; IDRs
are excluded. The initial seeds are a 10,000-byte floor, 1,000-byte restoration
hysteresis applied to the next-rung size prediction, and the existing slow
reduce/restore dwell. Stale samples and active bitrate/cap settling hold.

This replaces Pass 39's floor-rung/loss trigger. Cap coupling remains: every
FPS step immediately re-derives the live I/P ceilings, but only observed
P-frame size supplies positive evidence. The v1 ceiling remains `preferred`;
`max` stays forward-reserved.

**Amended:** §9.11 (intent, measurement, reduce/restore predicates, 100 fps
nominal, configuration, and observability), §9.6/15.2 (`fps_hint` seed 100),
and §17 (frame-size-driven calibration knobs and harness).

## Pass 54 — Frame-size FPS implementation and UDP verification (2026-07-19)

The Pass 53 ruling is implemented at frame-SHM ingress. Non-IDR Annex-B bytes
feed the ladder EWMA; the 8-byte metadata prefix and IDR frames are excluded.
The controller now holds during stale input or venc bitrate/cap settling,
clears pre-step evidence, predicts the next-rung frame size for restoration,
and exports the observed EWMA, target, and state in link stats. The default
cadence hint and preferred ladder mode are both 100 fps.

The focused UDP-air bench drove independent 16 KB / 7 KB / 16 KB P-frame
phases with a pinned PHY profile. It observed the exact write-on-change
sequence `[100, 90, 75, 60, 75, 90, 100]`, adjacent-rung dwell compliance,
zero venc failures, and cap writes coupled to each FPS transition. This proves
the ladder responds to encoded frame size without radio loss or selector-floor
state as a trigger.

The full 158 s all-controller soak repeated the same ladder sequence while
simultaneously exercising JSCC enforcement, selector transitions, FEC/ARQ,
and UDP cache repair across clean, marginal, burst, fade, interference, outage,
and recovery phases. Results: 2,372 schema-clean stats lines, 3,035/4,740
frames delivered (64%) with zero byte-integrity errors, 2,987 JSCC-enforced
frames, 149 cache-repaired blocks, 138 bounded venc pushes with zero failures,
and full recovery to profile 7 / 100 fps. All 43 dev sanitizer suites passed;
the SSC338Q cross-build also completed successfully.

**Verified:** §9.11 frame-size control and observability on UDP-air. The next
gate is real venc/radio actuation with direct `venc_frame_out` visual cadence.

## Pass 55 — JSCC feedback epoch is reporter-session scoped (2026-07-19)

The first monitor-radio enforcement run found a receiver-restart lockout. A
ground restart correctly changed its common-prefix session and reset its
per-reporter `feedback_epoch`, but TX compared the new epoch with the cached
epoch from the previous receiver session. Every new feedback packet was then
rejected as non-forward: LINK_REPORT and ARQ remained active while JSCC stayed
in `feedback_stale` indefinitely.

The wire contract already defines `feedback_epoch` as monotonic **per
reporter**. The cache key is therefore reporter `(originator, session_id)`, not
originator alone. An accepted reporter-session change replaces cached feedback
before epoch comparison; replay protection remains unchanged within a session.
JSCC feedback also passes the same preferred/first-latched reporter gate as
LINK_REPORT.

**Amended:** §3.10 (reporter-session cache identity and reboot behavior).

## Pass 56 — Authored kernel-monitor airtime service model (2026-07-19)

After the Pass 55 fix, monitor-radio feedback became fresh and RTT-ready, but
JSCC correctly remained in `airtime_unavailable`: only paced UDP had an
authored serialization model. Enabling enforcement by silently borrowing the
encoder bitrate or raw PHY rate would be optimistic and violates the fallback
contract.

Kernel-monitor may now opt into an explicit effective-service calibration.
The model multiplies the locally commanded HT20 MCS/GI rate by
`air.airtime_efficiency_permille`; zero (the default) preserves fallback. It
includes known MPDU overhead and available socket-outbound bytes, while the
priority resend estimate excludes the live queue. The MCS3/SGI stationary rig
carried the 15–20 Mbit/s target inside a 28.9 Mbit/s PHY envelope, supporting a
conservative initial 600-permille test value. This is a rig calibration, not a
new universal constant.

**Amended:** §14.2 (kernel-monitor airtime input and fail-safe configuration).

## Pass 57 — Monitor JSCC enforcement and real-venc FPS ladder (2026-07-19)

The vehicle ran the SSC338Q cross-build on its 8812EU monitor interface; ground
used the 2308 CU for return TX and 229b CU as the second diversity RX. The
opposite return assignment immediately reproduced the known 229b TX-wedge
state, so all accepted measurements use the healthy swapped assignment.

**Real venc FPS ladder.** With a 200-permille N=1 synthetic receive loss phase,
the selector held profile 0 / 3,804 kbit/s. Live venc writes produced the exact
adjacent sequence `[100, 90, 75, 60]`; at the floor the measured P-frame EWMA
was 7,828 B and venc reported 60 fps with `maxPBytes=7,203`. Replacing that
receiver with clean N=2 promoted to profile 3 / 17,236 kbit/s. The same running
TX then restored `[60, 75, 90, 100]`; final P-frame EWMA was 20,270 B,
`maxPBytes=19,586`, ladder state `HOLD`, and venc itself reported 100 fps.
The complete command sequence was `[100, 90, 75, 60, 75, 90, 100]` with zero
HTTP failures; the final 61.6 s clean N=2 receiver session recorded one
unrecoverable frame and neither adapter wedged.

**Monitor JSCC enforcement.** A 600-permille authored monitor service model at
profile 3 made the previously missing transport input explicit. Under the N=1
200-permille calibration phase, the final TX snapshot contained 8,457 valid and
enforced decisions versus 51 cold-start fallbacks, zero JSCC discards, fresh
feedback, 6,000 us measured RTT, 7,190 us source service, 667 us resend
service, five selected parity symbols, and ARQ eligible with 10,810 us
remaining after source transmission. The stable reason was
`fec_capacity_limited`, truthfully reporting predicted demand of six symbols
against the configured five-symbol cap.

Restarting the active ground receiver reset its feedback epoch from 346 to
152. With TX left running, decisions remained valid and enforced count grew
from 3,111 to 6,683, proving the Pass 55 reporter-session reset on real radio;
the old permanent `feedback_stale` lockout did not recur.

After both tests, the vehicle was restored byte-for-byte to config SHA256
`3636ad2b...`, normal `/usr/bin/waybeam` through `S95waybeam`, and AP channel
149/40 MHz. The verified Pass 56 `waybeam-link` binary remains deployed at
SHA256 `b9b0fd93...`; all bench processes were stopped.

**Verified:** JSCC per-frame enforcement and the frame-size-driven 100 fps
ladder on kernel-monitor radio. The next visual/RF work should use clean N=2
and residual natural loss; the 200-permille N=1 phase was deliberately a
controller-input calibration, not a quality target.

## Pass 58 — Session pairing token + ANNOUNCE packet (2026-07-20)

Multi-vehicle channel-sharing and a ground "scout" want zero-config rendezvous:
a ground should find a parked craft and CSA-claim it without pre-shared keys,
while co-located craft stay separable. Two identity axes already exist —
`originator` (§2) and the L2 `net_id` (§3.0); this pass adds the pairing
credential and the beacon that carries it. (See `docs/scout-design.md` for the
full flow; operator-ruled 2026-07-20.)

New packet type `0xB` **ANNOUNCE** (§3.12), fixed 30 bytes: common prefix +
`flags` (`claimed`, `psk_present`) + advisory `claimed_by` + a 16-byte session
pairing token. It is emitted at 1–2 Hz whether claimed or not (so a rebooted
ground can re-learn the token and re-claim in place, §11.5a/Pass 59) and is
**unauthenticated** — an advertisement, not a control action. A forged ANNOUNCE
only wastes one claim attempt because the elicited CSA still fails the §11.4 MAC.

CSA key provenance is split (§11.4a) with **HMAC always applied** — the earlier
"psk=none / skip-HMAC" idea is withdrawn. Default `psk_announce=true`: absent
`csa.psk` ⇒ the craft auto-generates a 16-byte token at boot (io/app entropy;
`core` stays RNG/clock-free and only verifies) and announces it. The announced
token is a **rendezvous credential, not a secret** — deliberate takeover is
bounded by the §11.5a binding, not token confidentiality. `psk_announce=false`
keeps an operator secret off the air (`psk_present=0`) and restores genuine
authentication. Nonce/allowlist/rate-limit guards are unchanged in both modes,
so an accepted CSA can never leave the craft's allowlist. The token inherits the
`csa.psk` redaction invariant (§3.12, §15.2).

**Amended:** §3.1 (type registry, 11/16), §3.12 (new), §11.4a (new), §13
(forged-ANNOUNCE row), §15.2 (`node.net_id` auto, `node.psk_announce`, optional
`csa.psk`).

## Pass 59 — CSA follower holds until reboot; command-source binding lifecycle (2026-07-20)

Field asymmetry drove this: a ground commonly runs a weaker TX than the craft,
so the craft can lose accepted ground telemetry for long stretches (>60 s). The
prior §11.5 `rendezvous_timeout` (5 s) would revert a healthy, committed link to
`home_chan` mid-flight — exactly the spurious channel-hop we must not do.
(Operator-ruled 2026-07-20; see `docs/scout-design.md`.)

COMMITTED is now **terminal until reboot**: the mid-flight `rendezvous_timeout →
home` revert is removed. The `verify_timeout_ms` (150 ms) revert is **kept** but
scoped to its real job — a *jump that landed on a dead channel* backs out to
`prev_chan`; it is not a mid-flight watchdog. `home_chan` is demoted to a
power-on default (the §15.5 scout sweeps all channels, so a craft holding any
channel stays findable), with an optional `persist_channel` to boot onto the
last-committed channel instead.

New §11.5a **command-source binding lifecycle**: an accepted CSA binds its issuer
as the craft's command source (this *is* the claim). The binding is sticky
through link loss and resists other issuers regardless of key knowledge — this,
not token confidentiality (§11.4a), is the takeover defence. It releases only
after `bind_release_s` (**90 s**) of command-source silence, and **release changes
no channel** — the craft stays put and merely re-opens for claim (continues
ANNOUNCE), so a rebooted/returned ground re-claims in place (the orphan case).
Reboot always resets claim/bind state.

**Amended:** §11.5 (state machine, hold-until-reboot), §11.5a (new), §15.2
(`csa.bind_release_s`, `csa.persist_channel`; `rendezvous_timeout_s` removed;
example `home_chan` 5805).

## Pass 60 — Ground scout, channel occupancy, and channel persistence (2026-07-20)

The claim/hold model (Passes 58–59) needs a finder on the other end: a ground
that sweeps channels, lists discovered craft with per-channel occupancy, and can
CSA-claim one — the inverse of the §11.5 follower. This is an io/app feature over
the existing §15.5 passive discovery and §11 CSA primitives; **no additional wire
change**. (Operator-ruled 2026-07-20; see `docs/scout-design.md`.)

New §15.5a defines one sweep engine with two entry points (list / quickconnect),
control-plane endpoints `scout/{start,stop,results,quickconnect}` (ground/rx
only, 409 elsewhere), and the §15.2 `scout` config (`dwell_ms` 300, `channels`
null = allowlist). A claim needs only the first heard candidate — the §2
admission count is anti-flood for the latch picker, not a barrier to a deliberate
operator claim. During a sweep the scout adapter ignores its `net_id` filter; a
single-adapter ground drops any active link while scouting.

Ownership is **proven by connecting**, not read from the beacon: `psk_known` is a
bool and the ANNOUNCE token is never echoed (§15 redaction). Per-channel
occupancy is reported as a superset **aligned with the Realtek "Advanced Channel
Scanning" survey** (`libc0607/rtl88x2eu-20230815`: quality/availability/
utilization/Wi-Fi-util/interference-util/noise dBm/BSS count); v1 fills only the
packet-derivable fields (Wi-Fi utilization, RSSI-floor noise proxy, transmitter
count) so a later hardware-ACS backend is a field-fill, not a reshape. Persistence
(`csa.persist_channel`, Pass 59) is surfaced here as the boot-channel choice.

**Amended:** §15.2 (`scout` block), §15.5 (Read/Write endpoint rows), §15.5a
(new).

## Pass 61 — Key-provenance mode is a pure function of `csa.psk` presence (2026-07-20)

Implementing the Pass 58 ANNOUNCE emit forced the four `psk_announce` × `csa.psk`
combinations to resolve. Two operator rulings (2026-07-20): a configured `csa.psk`
**always wins → secret mode** (even under the `psk_announce=true` default); and
`psk_announce=false` with **no** `csa.psk` **falls back to the announced token**
(never fails to boot). Together these make the mode a pure function of `csa.psk`
presence — `psk_announce` no longer gates anything in any combination. Rather than
ship an inert config knob (operator-ruled), **`psk_announce` is removed**:

- **Selector:** `csa.psk` configured ⇒ secret (`psk_present=0`, key = `csa.psk`);
  absent ⇒ announced (`psk_present=1`, key = the auto-generated 16-byte `P`).
  There is no separate toggle.
- The `node.psk_announce` field (added in the Pass 60 config plumbing) is deleted
  from `NodeCfg`, the parser, and `config_test`; `docs/scout-design.md` follows.

**Amended:** §3.12 (`psk` note), §11.4a (mode selection reworded, selector
sentence added), §15.2 (config example + `csa.psk` note; `node.psk_announce`
removed). No wire change — ANNOUNCE bytes and `flags` semantics are unchanged.

## Pass 62 — ANNOUNCE subsumes HEARTBEAT; discovery is an ANNOUNCE presence source (2026-07-20)

On-device bring-up of the Pass 58 emit (craft on ch161, ground monitor capture)
showed the always-on ANNOUNCE fully suppresses the craft's HEARTBEAT: ANNOUNCE's
inject resets the same §3.8 one-second quiet interval, so an idle announcing craft
emits **only** ANNOUNCE (0 HEARTBEAT observed at 2 Hz). Since ANNOUNCE carries the
same `(originator, session_id)` presence as HEARTBEAT — and more (claim state,
token) — this is correct behaviour, not a regression (operator-ruled 2026-07-20:
**ANNOUNCE supersedes HEARTBEAT**). Two reconciliations:

- **§3.8 quiet-interval reset list** now includes ANNOUNCE, so a craft announcing
  at ≥1 Hz never separately emits HEARTBEAT. HEARTBEAT's wire format and its role
  for non-announcing nodes (grounds, quiet rx) are unchanged.
- **§15.5 passive discovery** accepts ANNOUNCE as a node-presence source alongside
  HEARTBEAT/DATA (otherwise an idle craft would vanish from `/discovery`). ANNOUNCE
  senders additionally contribute advisory `claimed`/`claimed_by` to their node
  record; the token is never surfaced (§15 redaction).

**Amended:** §3.8 (reset list), §3.12 (HEARTBEAT-unchanged bullet reworded), §15.5
(`/discovery` `nodes[]` source + claim fields). No wire change.

## Pass 63 — The announced token is public: cache/log/surface, don't redact (2026-07-20)

Building the ground claim path (§15.5a) raised how the ground obtains the CSA key
in announced mode. Operator ruling (2026-07-20): **the ground caches the token
from every received ANNOUNCE and applies the matching one at link time**, and the
token is **no longer redacted** — it is on the air by construction and per-boot
rotated on the craft, so logging/surfacing it leaks nothing an RF-adjacent
listener doesn't already have. Redaction now scopes to the *operator secret* only:

- The ANNOUNCE `psk` with `psk_present=1` is the announced session token — public;
  it MAY be surfaced, logged, and cached (the ground does exactly this to key its
  §11 CSA). The prior "never echoed / MUST NOT print" language is withdrawn for it.
- The operator-provisioned `csa.psk` (secret mode) stays redacted (the §15
  config-dump `"(set, redacted)"` invariant is unchanged). It is never carried in
  ANNOUNCE (secret mode sends 16 zero bytes), so the ANNOUNCE `psk` field is never
  sensitive in either mode.
- The ground keys its `CsaIssuer` from the cached token (announced) or the
  configured `csa.psk` (secret); `psk_known` reports whether a usable key exists.

Also settled for the same feature (code-only, no wire/spec change): scout
candidates carry `net_id` (already parsed in `Dot11Rx`, surfaced via `AirRxMeta`);
monitor channel retune is implemented via `iw`/nl80211 (the ssc338q SDK lacks
libnl-3, so `iw dev … set freq` is the portable path), lifting the code-comment
"CSA/scout over monitor deferred" — §11.5/§15.5a already assume retune works.

**Amended:** §3.12 (redaction bullet), §15.5 (`/discovery` token note), §15.5a
(candidate `psk_known`/token-cache wording). No wire change.

## Pass 64 — Multi-adapter ground scout: the uplink roams, a claim moves every ear (2026-07-20)

Bringing up a two-adapter ground (one uplink + one diversity RX) exposed that the
scout/claim path was single-adapter only: the sweep and the claim's get-onto-the-
craft pre-step both retuned adapter index 0, and the §15.2 wording ("a two-adapter
ground dedicates a scout adapter") never said *which* adapter. Two operator rulings
(2026-07-20):

- **The uplink (`role:"tx"`) adapter is the scout.** A sweep roams the tx adapter
  only; the diversity RX adapters hold the resting channel, so a multi-adapter
  ground keeps hearing an active link on that channel while it scouts (a
  single-adapter ground has no spare ear and still drops the link for the sweep,
  unchanged). "The tx adapter" is resolved from the backend (`tx_index()`), not
  assumed to be config index 0 — the sample config lists it first, but the code no
  longer depends on that.
- **A claim retunes every link adapter, not just the uplink.** The quickconnect
  pre-step now moves all adapters onto the craft's current channel (so every
  diversity ear hears the campaign and the §11.6 commit), matching the existing
  intra-process commit/revert which already retunes all adapters (§11.6). An
  aborted or reverted campaign therefore leaves every adapter together on the
  craft's channel — positioned for an explicit retry, not split across channels.

Implementation-only for the code side (backend `tx_index()` accessor; scout sweep
and claim pre-step rewired); the wording rulings amend §15.2 (scout adapter is the
uplink) and §15.5a (sweep-roams-uplink, claim-retunes-all). No wire change.

## Pass 65 — Scout survey is scout-adapter-only; a failed claim rolls back to rest (2026-07-20)

On-device verification of Pass 64 (two-adapter ground) surfaced two multi-adapter
interactions the single-adapter design never exercised. Operator rulings
(2026-07-20):

- **The scout survey (occupancy + candidates) is derived from the scout adapter's
  frames only.** A diversity RX adapter parked on the resting channel hears the
  claimed/idle craft during *every* dwell, so aggregating all adapters recorded the
  craft as a candidate on every swept channel (with a bogus `chan`) and inflated
  occupancy. `ScoutEngine::on_frame` now ignores frames whose `adapter_id` is not
  the scout (uplink) adapter — the survey reflects the channel the scout is actually
  tuned to. Single-adapter grounds are unchanged (the scout adapter is the only one).
- **A failed claim rolls all adapters + the net_id stamp/filter back to the resting
  state, and the scout returns *all* adapters to rest on completion.** Pass 64 left
  an aborted claim's adapters on the craft's channel ("positioned for retry"); that
  left a diversity ear stranded off the resting channel, and the scout's rest only
  moved the uplink, so a re-scout could split the ears. Superseded: an aborted claim
  now retunes every adapter back to the ground's operating channel (its configured
  channel, or the last committed target) and restores the resting net_id; the scout
  returns every adapter to that channel when a sweep ends or is stopped. The
  operating channel is tracked at runtime (updated on a successful §11.6 commit).

This supersedes the Pass 64 §15.5a "aborted campaign leaves every adapter on the
craft's channel" wording. Implementation-only otherwise (scout gains a retune-all
hook + scout-adapter index; run_rx tracks the operating channel and rolls back on
abort). No wire change.

## Pass 66 — A candidate's channel is where it was heard most, not last (2026-07-20)

Verifying Pass 65 exposed a residual survey artifact independent of the multi-adapter
work: on a channel change the scout adapter drains frames buffered from the *previous*
channel into the first part of the new dwell (kernel socket + the io/ RX deque), so a
craft can be recorded on an adjacent swept channel it isn't on. `candidate_for` took
the *last* channel a craft appeared on, which a settling leak can make wrong depending
on scan order.

Operator ruling (2026-07-20): **a candidate's channel is the swept channel it was
heard on with the most frames, not the last.** The craft's true channel is heard for
the entire dwell (hundreds of DATA frames, or every ANNOUNCE), while a settling leak is
a handful of buffered frames — so max-frame-count is robust regardless of scan order.
Rejected the alternatives (time-based settle-guard, post-retune queue flush,
channel-generation stamping): all chase *occupancy* accuracy at the cost of dwell time
or io/ RX-path changes, whereas the only decision that must be correct — the channel a
claim retunes to — is fixed by evidence-weighting alone. Occupancy stays best-effort v1
(a small airtime bump on an adjacent channel is tolerated); a settle-guard/flush can be
revisited if occupancy precision is ever needed.

Amends §15.5a (candidate `chan` selection). Implementation-only: `ScoutEngine` counts
frames per originator per channel and `candidate_for` returns the max. No wire change.

## Pass 67 — Receiver-owned vehicle selection and cache following (2026-07-22)

The first complete production topology is one vehicle, one receiver, and one
Ethernet ARQ cache. Review of the implemented scout showed three independent
states: quick-connect moved the RF channel/net_id, RX stream wants remained
pinned to their boot-configured originator, and the cache remained pinned to
its own `node.preferred_originator`. A generic RX-side CSA spectator could also
move another receiver or cache without changing either pin, creating a partial
switch rather than a usable link.

Operator rulings (2026-07-22):

- **The receiver owns the cache.** The cache is statically linked to exactly one
  controller receiver. The receiver discovers/pairs with the vehicle and assigns
  its committed vehicle tuple to the cache; the cache never discovers or chooses
  a vehicle independently. Multi-receiver cache arbitration and simultaneous
  multi-vehicle storage are outside the production MVP.
- **Claim commit is the subscription boundary.** On issuer commit the receiver
  changes the local RX sender pin, clears old stream/reassembly/cache-controller
  state, and begins cache-follow retries. Issuer verification revert restores
  the previous tuple. A claim that never commits changes neither subscription
  nor cache assignment.
- **Cache following is explicit and restart-healing.** A new Ethernet-only
  CACHE_ASSIGN packet carries vehicle originator/channel/bandwidth/net_id and a
  per-controller-session epoch. The cache accepts it only from the configured
  controller originator and exact UDP endpoint, retunes before changing the
  logical store target, and clears the old vehicle window. The receiver retries
  until fresh matching CACHE_STATUS proves readiness; it also assigns its static
  startup selection, healing cache restarts without another vehicle claim.
- **Cache availability never gates video.** Pairing/selection succeeds when the
  vehicle campaign succeeds. An absent cache is observable and retried, while
  RF video and vehicle ARQ continue normally.

This adds packet type `0xC` (§3.13), amends §14.3 cache ownership, §15.2 config,
and §15.5a claim commit semantics. The cache-control packet stays on the
Ethernet cache socket and is never RF-injected.

## Pass 68 — Remote vehicle commands ride the CSA trust machinery (2026-07-22, ruled; hardware-verified 2026-07-23)

**Operator ruling (2026-07-22): approved as proposed, conditional on an
adversarial review before deployment.** The review ran, its resolutions are
folded into the sections below, and the implementation was verified on the
production rig (vehicle 17 / x86 ground 9, channel 161): quickconnect claim →
`ARQ off` acked over RF with `arq_frames` frozen while IDRs kept flowing →
`ARQ on` restore → `SELECTOR` freeze/run pinning and releasing profile 3 →
`FPS_LADDER on` echoed `REJECTED` (venc disabled on the craft, the intended
unconfigured-actuator path) → 409 with an unbound craft → ground-local
`POST /api/v1/arq` gate observable in §15.3. Random-seeded nonces confirmed
on the wire. Incidental find: the deployed craft config had drifted (no `csa`
block ⇒ fail-closed empty allowlist rejecting every campaign); re-synced from
`deploy/vehicle-192.168.2.232.json`.

The ground-station MVP needs runtime toggles for craft-side knobs that are
boot-config only (ARQ, selector adaptation, FPS ladder) — the hub's ADAPTIVE
menu has no lever to pull. Prior operator rulings framing this pass
(2026-07-22): the channel rides the §11 CSA path and its HMAC PSK machinery;
**every command is an enable/disable or an enum of at most 5 choices** — no
free-form numerics; v1 commands are ARQ on/off, MCS-adaptation
freeze/unfreeze, FPS-ladder on/off, with venc set-commands reserved for v2; a
ground-local ARQ-off REST endpoint ships alongside.

Proposed design (each point is a decision the operator should confirm or
overrule):

- **New packet type `0xD` VEHICLE_CMD (§3.14), 23 bytes**, reusing §11
  wholesale: the §11.4 HMAC primitive and §11.4a key provenance (no separate
  command key), a dedicated `cmd_nonce` counter (NOT shared with `csa_nonce` —
  interleaved CSA and command campaigns must not invalidate each other), and
  the §11.5a binding as the authorization: **bound-issuer-only, no bootstrap**.
  Unlike CSA, a command can never establish a claim; unbound/non-bound senders
  get a silent drop (an echo would be a probe oracle).
- **ACK = the craft echoes the packet back** (`cmd_flags` bit0), re-MAC'd,
  on the strong diversity-received downlink — chosen over a `CSA_ARMED`-style
  DATA flag because a flag cannot carry *which* nonce/command it acknowledges,
  and over a separate ACK type because the echo is free and self-describing.
  `REJECTED` (bit1) distinguishes "understood, won't do" (unknown `cmd_id`,
  bad arg, unconfigured actuator) from silence.
- **Idempotent retry via duplicate-nonce re-echo** (the §3.13 CACHE_ASSIGN
  pattern): `cmd_nonce == last_applied` re-echoes without re-applying — a
  retried campaign means a lost echo, not a failed command.
- **Campaign shape:** 3 copies @ 20 ms; echo 2 copies; `ack_timeout_ms` 1000
  (csa_ack_timeout's reasoning); `retry_cap` 3 campaigns on the same nonce,
  then `timeout`. Craft-side rate limit `min_interval_ms` 250 (commands are
  mild, unlike the 5 s CSA limit — a menu should feel responsive). All §17
  RE-DERIVE seeds under `policy.cmd`.
- **v1 semantics:** `ARQ off` clears `ARQ`/`PFRAME_ARQ` stamping AND NACK
  service — receivers then never NACK by construction (§6.4) and cache
  nack-graces never arm; FEC and §3.9 recovery unaffected. `SELECTOR freeze`
  = the existing §9.7 `min==max` pin at the current rung (same lever as
  §15.5 `/link/profile`, last writer wins); `run` restores the boot envelope.
  `FPS_LADDER off` stops the §9.11 loop where it is (no snap to `preferred`);
  `on` re-enables with cleared evidence. All command state is
  **craft-session-volatile** (reboot restores boot config; survives binding
  release and channel moves — a re-claiming ground reads state from §15.3,
  not memory).
- **REST (§15.5):** `POST /api/v1/vehicle/command` on the issuer (409 when
  unbound or elsewhere), polled via `GET /api/v1/vehicle/command`
  (`pending|acked|rejected|timeout`); `POST /api/v1/arq` = RX-local NACK
  emission gate (§6.4), unilateral, craft untouched.
- **§13 row:** forged VEHICLE_CMD worst case is bounded to reboot-resettable
  settings — never channel or power.

Adds §3.14 and §11.7, amends §3.1 (registry, 13 of 16), §6.4, §13, §15.2
(`policy.cmd`), §15.3 (`cmd_*`/`vcmd_*`/`arq_rx_enabled` link fields), §15.5.
Spec-only; implementation follows in the same PR after the ruling.

**Review hardening (2026-07-22, adversarial pass on the draft).** An
independent review of the draft found the wire arithmetic sound but the
echo/acceptance semantics under-specified. Resolutions folded into the same
sections, still under the same pending ruling:

- **Echo acceptance is now explicit (was: "matching echo", undefined).** The
  issuer accepts an echo only on MAC + `ACK` + bound-craft sender + exact
  nonce/cmd/arg match. Token-mode honesty note added: a forged echo can
  misreport an outcome; the bound is the §13 row and the recourse is
  re-issuing the idempotent command (the §11.6 `CSA_ARMED` posture).
- **Duplicate re-echo echoes the STORED tuple, never the incoming packet** —
  the craft retains `(nonce, cmd_id, cmd_arg, rejected)` per issuer domain; a
  same-nonce packet with different fields is a silent drop (else a buggy or
  malicious retry could mint an ACK for a command never applied). The re-echo
  path keeps the full guard set minus nonce monotonicity, and bursts at most
  once per nonce per `min_interval_ms`.
- **`cmd_nonce` starts at a random 32-bit value per issuer session** —
  a rebooted issuer restarting at 1 would let a recorded session-A echo
  satisfy session-B's first campaign (false "acked"). Random start kills the
  cross-session replay for free; the craft guard was already per-domain.
- **No over-air state readback.** The draft's "ground reads craft state from
  stats" claimed a wire path that does not exist; replaced with the honest
  mechanism — a re-claiming ground re-issues the idempotent commands. The
  `cmd_*` fields are craft-local §15.3 observability (§15.3 now actually
  amended; the draft referenced fields it never added).
- **Binding identity pinned to the issuer's originator** (the §11.5a latch as
  implemented): a rebooted ground commands its craft immediately in a fresh
  nonce domain, no 90 s dead window. The issuer-side "bound" predicate behind
  the REST 409 is its own committed selection this session; craft-side truth
  surfaces as `timeout`.
- **Smaller gaps closed:** `GET /vehicle/command` gains `idle`; `POST` 409s
  while a campaign is pending; issuer paces starts by
  `min_interval + copy_interval` so consecutive menu commands don't eat the
  craft rate-limit; `cmd_arg > 4` is a structural decode error vs. in-range
  invalid = consumed + `REJECTED` (was ambiguous across §3.14/§11.7);
  `FPS_LADDER` "configured" = ladder ran at boot; `ARQ on` over an all-off
  boot config is an acked no-op; `SELECTOR` lands at the next selector
  evaluation, i.e. after any §11.3 freeze; every echo sets `ACK` (`REJECTED`
  is additive); the ARQ-off "no NACKs" claim is bounded by in-flight flagged
  blocks draining (one deadline window).

## Pass 69 — CSA cross-channel rendezvous: pre-position + beacon (ruled 2026-07-23)

**Operator ruling (2026-07-23): options A + B + D from the open question
below; spec amendment first, then implementation, then rig verification.**
Option C (a larger `verify_timeout_ms`) was NOT taken — the window stays
150 ms; the margins are now structural, not padded.

- **A — issuer pre-positions (§11.6).** The issuer commits (retunes all its
  ears to `target_chan`) immediately upon seeing `CSA_ARMED`, not at
  T_switch. After the copies are out and the craft has ACKed, the issuer has
  no further business on the old channel. With class-1 dt = 500 ms and the
  ACK arriving ~60–100 ms into the campaign, the issuer now lands ~250–350 ms
  before the craft moves, instead of 117–184 ms after.
- **B — rendezvous beacon (§11.1/§11.4/§11.5/§11.6).** From landing until
  video-verify success or deadline, the issuer re-injects the accepted
  campaign packet with `csa_seq = 0`, `dt_to_switch_ms = 0`, MAC recomputed,
  at the §11.2 copy spacing. `dt = 0` is now a normative §11.4 accept guard
  (never arms), which makes the beacon un-forgeable-into-an-arm by
  construction. The craft's VERIFY confirms on the first beacon matching the
  armed campaign's `(originator, session, csa_nonce)`; in every other state a
  zero-dt CSA is dropped with no side effects (no §11.5a binding refresh — a
  recorded beacon must not hold the binding alive). New §13 row.
- **Deadline anchor (§11.6).** Issuer revert-on-no-video deadline =
  `max(T_switch, landing) + verify_timeout_ms` — pre-positioning must not
  shrink the window in which the craft can legitimately appear (it does not
  move before T_switch). Previously the deadline was anchored at the commit
  tick and had already lapsed inside the blocking retune (the observed
  false-commit-or-revert coin flip).
- **D — io hygiene.** (i) On the commit retune the issuer flushes RX backlog
  captured before the retune completed (kernel socket + process queues), so
  video-verify counts only genuine `target_chan` traffic — closes the Pass 66
  stale-drain false commit on the issuer side. (ii) `retune_all` failures are
  logged at every call site (they were silently ignored). The "parallel/
  async retunes" half of D was NOT implemented: with pre-positioning the
  landing latency is no longer on the critical path, and threading the
  retunes would buy nothing but complexity.
- **Timeline after the fix (measured driver costs, class 1):** claim t0 →
  copies t0..t0+80 ms → `CSA_ARMED` ~t0+100 ms → issuer lands ≤t0+300 ms,
  beacons every copy-spacing → craft retunes at t0+500 ms (+~30 ms set-freq)
  → first beacon heard within one spacing → COMMITTED ~t0+550 ms; issuer
  hears craft video ~t0+540 ms → COMMITTED. Both margins >100 ms against a
  150 ms window.
- **HARDWARE-VERIFIED 2026-07-23** on the x86 ground (EU uplink + CU
  diversity) + SSC338Q craft + Ethernet cache rig. Five cross-channel moves
  committed on all three nodes with video flowing and the cache following
  each epoch: quickconnect 5805→5825 (class 1: claim→landing 265 ms, beacons
  at 20 ms spacing bridging the craft's arrival), `/csa` 5825→5745 and
  5745→5805 (class 0, dt 126/147 ms), and menu-path (`/menu/exec`) jumps
  5805→5825→5805 on the final clean build. Same-channel claims and the
  Pass 65 abort-rollback (4× — stale-candidate claims on the wrong channel
  abort cleanly, link undisturbed) also exercised. **One observation raised
  for a possible hardening ruling:** the issuer stops beaconing on
  `note_craft_video`, which fires the instant the craft lands — in every
  verified move the craft's confirm actually rode the ground's first
  post-arrival report inside the window, never a beacon. Robust in practice
  (pre-positioning keeps the issuer present with a warm report path), but
  the "guaranteed signal in the craft's window" would be literal only with a
  beacon tail running until T_switch + t_revert_ms regardless of early
  video-verify success. Not implemented — needs an operator ruling if
  wanted.
- **Beacon tail RULED (operator, 2026-07-23, same day):** the observation
  above is accepted — the issuer keeps beaconing until its verify deadline
  (= the close of the craft's window) regardless of early video-verify;
  video success is latched and the campaign closes, success or revert, only
  at the deadline (`kSuccess` / `kRevert`). Cost: ≤ ~8 extra 32-byte frames
  per campaign. The §15.5 `selection_state` flip to `committed` accordingly
  moves from first-craft-video to the campaign close, ~150–500 ms later.
  (Correction, review pass 2: the flip is NOT purely observability — the
  `vehicle_command` 409 gate and the claim-busy `set_psk` rejection key on
  it/`active()`, so REST commands are refused for the tail duration. All
  transient and retryable; accepted.)
- **Adversarial review pass 2 (2026-07-23, operator-directed) — two
  confirmed HIGHs fixed, one MEDIUM implemented, one design gap raised:**
  - **H1 (fixed):** the issuer's verify deadline was computed at the tick
    that *emitted* kCommit — but the engine cannot tick during the app's
    blocking serialized retunes, so "landing" was really "commit tick" and
    a slow-enough multi-adapter retune (3 ears × 65–117 ms, class 0) would
    open the window already expired: instant kRevert, zero beacons — the
    exact pre-Pass-69 failure re-introduced. The two-adapter bench passed
    only because 180 ms < the 300 ms class-0 budget. Deadline (and first
    beacon) now computed lazily at the first tick IN kVerify, which is
    landing by construction. §11.6 now defines "landing" normatively.
  - **H2 (fixed):** `video_seen_` could latch before T_switch — the craft
    cannot legitimately be on the target yet, so any such frame is a stale
    ear (failed per-adapter retune keeps hearing the craft on the old
    channel) or RF bleed — producing a false `kSuccess` that discards the
    revert state and defeats both the failed-retune recovery and the
    forged-`CSA_ARMED` backstop. `note_craft_video` now ignores anything
    before `T_switch`, and a failed commit retune abandons the campaign
    outright (`note_commit_failed` + restore previous selection) instead
    of verifying with untrusted ears. §11.6 amended accordingly.
  - **M1 (implemented):** the §11.6 flush MUST was MonAir-only; RadioAir
    (devourer bench backend) now carries the same generation-stamped
    flush (its reachable backlog is the process queue — devourer's USB
    pipeline depth is ~ms and out of scope, like driver-internal buffers
    on MonAir).
  - **L2 (hardened):** the MonAir drain loop now survives EINTR and
    zero-length reads.
  - Tests added: late-landing window (H1), pre-T_switch video ignored
    (H2), commit-retune-failure abandon, beacon leaves the §11.4
    rate-limit anchor untouched, spectator (empty-PSK) beacon confirm.
  - **H1b (found by the post-review verification sweep, fixed):** the
    FOLLOWER had the same anchor defect as H1 — its verify deadline was
    set at the tick that ordered its retune, so the craft's own blocking
    `iw` set-freq (+ any post-retune RX dead-time) burned the window from
    the inside. On a class-0 campaign (150 ms dt) the craft's window
    closed ~40 ms after the issuer's landing; a 5825→5745 `/csa` jump
    reverted+unbound the craft while the issuer (legitimately hearing the
    craft's verify-window video on-target, post-T_switch) confirmed — a
    strand the pre-review build dodged only by racing. §11.5 now opens
    the follower window at its landing, mirroring §11.6. Verified by
    repeated class-0 jumps in the final sweep.

## Pass 70 — M2 feed-stall confirm asymmetry: accepted + documented (ruled 2026-07-23)

**Operator ruling (2026-07-23): option (a) — accept and document.** The
Pass 69 design gives the two campaign ends different confirm evidence (craft:
rendezvous beacon; issuer: craft video). A craft input-feed stall between
`CSA_ARMED` and the verify deadline therefore splits the fleet — craft
COMMITs on `target_chan`, issuer reverts to `prev_chan` — until an explicit
re-scout + re-claim (§15.5a; §11.5a binding self-releases). Alternatives were
rejected because each weakens a ruled protection: (b) issuer counting craft
HEARTBEAT/ANNOUNCE hollows out the forged-`CSA_ARMED` revert-on-no-video
backstop (those frames are unauthenticated); (c) craft delaying COMMIT past
the beacon reopens the §11.6 rendezvous gap Pass 69 closed. Exposure: one
campaign window (~the verify timeout) on a craft whose encoder already
stopped feeding — a failed link by definition; recovery is the standard
re-scout/quickconnect. Spec: accepted-asymmetry paragraph added to §11.6
(after the forged-`CSA_ARMED` backstop). No code change.

## Open questions for the next pass

- [x] **RESOLVED (Pass 70, ruled accept+document 2026-07-23)** — see the
      Pass 70 entry; original analysis kept below for the record.
      **Asymmetric confirm split on a mid-campaign feed stall (Pass 69
      review pass 2, M2).** The craft's confirm signal is issuer presence
      (the beacon — guaranteed), but the issuer's confirm is craft *video*
      only. If the craft's RTP feed stalls after `CSA_ARMED` (a TX node
      sends nothing without a feed), the craft confirms on a beacon and
      COMMITs (terminal until reboot) while the issuer reverts on no-video
      → ground on `prev_chan`, craft on `target_chan` until a re-scout/
      re-claim (recoverable; binding releases after 90 s). Pre-Pass-69 both
      sides reverted and reconverged. Counting the craft's unauthenticated
      HEARTBEAT/ANNOUNCE as issuer-side confirmation would close it but
      weakens the forged-`CSA_ARMED` backstop (those frames are forgeable).
      Options: (a) accept + document (the window is one campaign, ~650 ms,
      and a real craft venc always feeds); (b) issuer counts any
      MAC-checkable craft frame (none exists today — would need an
      authenticated craft beacon); (c) craft delays COMMIT until it hears
      non-beacon issuer traffic (weakens the guaranteed-confirm just
      ruled). Recommendation: (a). Needs an operator ruling.
- [x] **RESOLVED (Pass 69, ruled A+B+D 2026-07-23)** — see the Pass 69 entry;
      original analysis kept below for the record.
      **Cross-channel claim rendezvous gap (root cause PROVEN 2026-07-23,
      instrumented rerun after Pass 68; supersedes the earlier retune-latency
      wording — the operator's "iw is 5–10 ms" objection was correct).**
      A timestamping `iw` interposer on both ends showed, per T_switch-anchored
      timeline: the `iw`/nl80211 shell-out itself is ~1.5 ms; the *vendor
      drivers' cross-channel set-freq* is the real cost (rtl88x2eu x86
      **117 ms**, rtl88x2cu **65 ms**, craft 8812eu-on-SSC338Q **30 ms**), and
      same-channel set-freq is free (why every same-channel claim works).
      Mechanism, deterministic not probabilistic: §11.6 has the issuer begin
      its own retune AT T_switch — the same instant as the craft — serialized
      per adapter and blocking the event loop (ground lands EU T+117 ms,
      CU T+184 ms). The craft lands at T+30 ms and listens until its
      `t_revert_ms` = 150 ms deadline, but the issuer transmits NOTHING on the
      target until its report machinery wakes (craft EOB + 10 Hz reporter +
      §7.2 return window), so the craft cannot possibly confirm and reverts +
      unbinds. Meanwhile the ~30 ms EU-landing overlap still carries genuine
      craft video (~1000 pkt/s), which satisfies `note_craft_video` — the
      issuer commits (its own 150 ms verify deadline had already lapsed inside
      the blocking retune; Pass 66 stale-buffer drain is a second false-commit
      path). Asymmetric strand every time. Fix options needing an operator
      ruling (spec §11.6/§11.5): (A) issuer pre-positions — retune to target
      as soon as all copies are out and CSA_ARMED is seen, instead of waiting
      for T_switch; (B) issuer transmits immediately on landing — inject a few
      authenticated issuer-present frames (e.g. forced LINK_REPORTs, campaign
      timing, quiet-gap-exempt) so the craft's `note_valid_rx` can confirm;
      (C) larger `verify_timeout_ms`/`t_revert_ms` for kernel-monitor rigs
      (needs ≥ ~400 ms to cover measured worst case); (D) io-level hygiene —
      parallel/async retunes, flush stale RX queues at the commit retune, log
      `retune_all` failures. Recommended combination: A + B (+ D flush), with
      C only as margin. Same-channel claims remain safe and verified; the hub
      menu's "connect best" pins `target_chan` to the craft's current channel
      until this is ruled and fixed.
- [ ] **`bpf_filtered` precision follow-up** — if the coarse sysfs estimate proves
      too noisy in bench (concurrent sniffers, multi-socket rigs), replace it with an
      exact per-socket count via `PACKET_STATISTICS`/`tp_drops` and drop the §16.2
      1:1 exception. Deferred until a bench shows the estimate is inadequate.

Standing constraints (not revisitable):

- [ ] **Ruling 3 is FIXED** — vehicle is permanently single-adapter;
      diversity is ground-RX-only. Craft return path is best-effort by physics; the
      quiet-gap fit + floor-oscillation damping are **resolved empirically at gate 4**,
      not designed further on paper.
- [ ] Run the four bench gates (gate 1 now RX-proven; gate 4 return-window-fit is the one
      that gates the craft return path under single-adapter). The Pass 36–39 UDP
      harnesses (`cache_repair`/`actuation`/`jscc_enforce`/`fps_ladder`) re-run
      against the radio and kernel-monitor backends as part of the same rig
      campaign (§17 UDP-first ruling, 2026-07-16).

Pending operator rulings, with recommendations (2026-07-16 register):

- [x] **R-A — RESOLVED (Pass 41, integration corrected Pass 45).** Preferred/
      first-latched filtering runs before selector and FPS ladder; accepted
      reboot/re-latch identities reset selector epoch/smoothing state.
- [x] **R-B — RESOLVED (Pass 45 correction).** Unified cache decode now keeps
      explicit air-only attribution for the repair-demand estimator, verified
      by both the combined bench and an all-cache-completable 120-block guard.
- [x] **R-C — MEASURED (2026-07-16), zero retention stands.** 150 ‰ sweep:
      repair success 100 % @90 fps, 82 % @120, 62 % @144; unrecoverable
      5.7/5.3/8.9 %. Latency-first keeps the pin; revisit only on 144 fps
      cache-primary flight data (Pass 40 makes the cache the sole repair
      path above 100 fps).
      Zero-block retention ends cache repair at next-block arrival; at
      120 fps the post-close window is ~2–5 ms. *Recommendation:* keep zero
      retention as pinned; run the Ethernet cache bench at a 90/120/144 fps
      sweep, and only if a material fraction of cache replies lose the
      supersession race, add an OPT-IN per-stream `max_blocks_ahead=1`
      (+1 frame latency) for cache-enabled streams. Prior: unnecessary at
      ≤90 fps.
- [x] **R-D — RULED (Pass 40): v1 locked to fleet-wide 20 MHz.** The design
      doc's "2–5 ms invisible switch" does not survive this stack: the craft
      is pinned 20 MHz (8812EU 40 MHz bug, §7.2), width is a FLEET property
      under same-channel diversity (§1) so a change is CSA-shaped (§11
      already carries `target_bw`), and measured monitor-mode retunes run to
      ~277 ms cross-band (§11.2). *Recommendation:* no per-link width
      actuator. If capacity beyond MCS7@20 MHz is ever needed, design it as
      a CSA campaign parameter, gated on (a) a hardware verdict for the
      deployed chips under injection at 40 MHz, (b) measured retune times,
      (c) a gate-4-class bench. Until then width stays a deployment choice
      (the matrix tool already models both).
- [ ] **R-E: venc volatile writes (flash wear at controller cadence).**
      Every venc `/set` persists to `/etc/waybeam.json`; write-on-change
      helps, but bitrate+caps+fps transitions on an oscillating link still
      wear flash. *Recommendation (venc-repo change):* add a volatile apply
      (e.g. `persist=false`) to the /set contract; waybeam-link then uses
      volatile sets for controller-driven fields plus a rare persisted
      baseline. Wants doing before long-duration flight soaks.
- [x] **R-F — CLOSED (operator ruling 2026-07-16): not load-bearing here.**
      Everything late is dropped BEFORE the SHM egress boundary (§6.3a
      supersession, §8 deadlines, Pass 38 TX discard) and those outcomes are
      already §15.3 telemetry; beyond the ring the link can neither see nor
      act except by dropping, which it already does. Reopen only if
      rig/flight shows consumer-side latency the existing counters cannot
      explain. The §9.11 emergency-reduction deferral stands.
- [x] **R-G — CLOSED with R-F** (needed R-F's evidence). `max` stays a
      reserved config field; §9.11 v1 semantics unchanged.
- [ ] **R-H: RF cache transport ordering — defer until proposed.** Pass 50
      gives the UDP/IP cache a measured 3 ms first-NACK lead. An RF cache
      reply spends shared airtime, so the grace and priority lane must be
      re-derived for that binding. Formats are transport-agnostic; do not
      assume the UDP/IP seed transfers unchanged.
- [x] **§10 ground-uplink power scope — RESOLVED (Pass 43): rejected at
      config load.** (2026-07-11 desk §4.6 run): the
      §10 power curve is applied only on the tx-node selector commit, so the
      ground's designated uplink TX adapter transmits returns at devourer's
      efuse-default power regardless of any `power_map` — sweeping it is a
      no-op. *Recommendation:* config-load warning/rejection for a
      `power_map` on an rx-node uplink now (explicit beats silent); bring
      the return uplink under power control only if gate 4 shows return
      margin problems.
- [ ] **JSCC enforcement production flip (per link class, rig data).** The
      Pass 38 criteria (≥99% valid decisions, <1% repair underprediction,
      RTT readiness held) are UDP-proven; the flip itself should follow a
      radio-backend shadow soak during the gate campaigns.
      *Recommendation:* flip P-frames first (`arq_mode all-frames` +
      `enforce`), confirm discard behavior visually on the bench before any
      flight use.
