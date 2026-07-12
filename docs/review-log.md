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

## Open questions for the next pass

- [ ] **Ruling 3 is FIXED, not revisitable** — vehicle is permanently single-adapter;
      diversity is ground-RX-only. Craft return path is best-effort by physics; the
      quiet-gap fit + floor-oscillation damping are **resolved empirically at gate 4**,
      not designed further on paper.
- [ ] Run the four bench gates (gate 1 now RX-proven; gate 4 return-window-fit is the one
      that gates the craft return path under single-adapter).
- [ ] **§10 ground-uplink power scope** (surfaced by the 2026-07-11 desk §4.6 run): the
      §10 power curve is applied only on the tx-node selector commit, so the ground's
      designated uplink TX adapter (role tx) transmits returns at devourer's efuse-default
      power regardless of any `power_map` — sweeping it is a no-op. Decide: bring the ground
      return uplink under power-curve control, or reject/ignore a `power_map` on an rx-node
      uplink (currently it's silently loaded but never applied). Not a spec ruling yet —
      raised for the next pass.
