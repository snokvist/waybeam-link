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

## Pass 71 — §11.7 v2 venc commands: preset-indexing, ladder exclusivity, documented persistence (ruled 2026-07-23)

**Operator rulings (2026-07-23), four questions raised when v2 was scoped:**

1. **fps select vs the §9.11 ladder: REJECTED while the ladder runs.** A
   `FPS_SELECT` arriving while `cmd_fps_ladder` is true is consumed +
   `REJECTED`; the issuer must send `FPS_LADDER` off (`0x03`) first. No
   implicit ladder stop — that would change state the issuer didn't command
   and cannot read back (Pass 68: no over-air readback). Single-owner
   semantics for `video0.fps` are preserved. A selection made while a
   configured ladder is *off* updates the ladder's current-rung model so a
   later re-enable resumes from the selected rung (least surprise).
2. **Arg encoding: config preset-indexing.** The Pass 68 ≤5-choice bound vs
   the 8-rung ladder (and open-ended resolution/framing spaces) is resolved
   by `venc.command_presets` — up to 5 deployment-chosen values per command,
   `cmd_arg` indexes the list, unset index ⇒ consumed + `REJECTED`. Uniform
   across all three commands, no venc values baked into the spec, and the
   ground reads presets from deployment config, never over the air. `fps`
   presets must be §9.11 ladder members (cap-coupling assumes rungs).
   Rejected alternatives: fixed spec enums (can't cover the ladder, freezes
   venc strings into the wire spec); envelope-relative positions (fps-only —
   resolution/framing would need a second encoding anyway).
3. **Persistence: documented, not fought.** Venc's persist-on-set contract
   (Pass 37) means a v2 command's encoder effect survives reboot, in tension
   with §11.7 volatility and the §13 "settings a reboot resets" bound. Ruled:
   **document the asymmetry** — §11.7 gains a persistence-exception
   paragraph, the §13 row now bounds the v2 worst case to "an encoder preset
   from the operator's own deployment allowlist — never channel or power".
   Forged-command exposure is already gated by HMAC + bound-issuer; the
   alternatives (venc no-persist variant = R-E's recommendation, or
   boot-baseline restore machinery) add a venc-repo dependency or new
   failure modes for a benign property. R-E stays open for the
   *controller-cadence* flash-wear concern; command-driven presets stay
   persisted even if R-E lands.
4. **Scope: all three commands spec'd now, implementation staged.**
   `FPS_SELECT` (`0x04`) is implemented immediately (reuses the §9.11
   `set_fps` actuator); `RESOLUTION` (`0x05`) and `FRAMING` (`0x06`) are
   fully specified but answer `REJECTED` until the venc-side HTTP knobs
   exist (venc-repo dependency) — the unconfigured-actuator pattern makes
   staging spec-legal with zero drift.

Spec: §11.7 registry rows `0x04`–`0x06` + preset-encoding paragraph +
persistence exception; §3.14 untouched (no version field needed — unknown-id
forward compatibility carries v2); §15.2 `venc.command_presets`; §15.3
`cmd_fps_select`/`cmd_resolution_select`/`cmd_framing_select` (1-based
applied index, 0 = none this session); §15.5 name enum extended; §13 row
amended. New link stats fields ⇒ stats golden updated in the same PR.

## Pass 72 — scout candidate-resolving dwell extension (ruled 2026-07-23)

**Problem (operator, 2026-07-23): discovery takes a variable 1–4 sweeps —
bad UX.** Root cause is arithmetic, not flakiness: a *candidate* requires an
ANNOUNCE decode (DATA marks the channel occupied but names no claimable
craft — the §15.2 "seen off DATA" rationale was imprecise), and the craft
announces every 500 ms (§3.12 permits ≥1 Hz) while the scout dwells 300 ms —
p≈0.6 of catching an ANNOUNCE per visit to the craft's channel.

**Ruling: adaptive dwell extension.** Base `dwell_ms` unchanged (300 ms —
empty channels stay fast); when a dwell elapses with decodable waybeam frames
heard but no candidate resolved, the dwell extends until the first ANNOUNCE
decodes, bounded at `dwell_ms` + 1200 ms total (≥ one worst-case 1 Hz
announce period + margin; two periods at the reference 2 Hz cadence). The
extension ends at the first resolved candidate; occupancy switches to the
actual elapsed dwell as its airtime denominator so extension doesn't skew
`wifi_util_permille`. Rejected alternatives: flat `dwell_ms` ≥ 1100 ms
(deterministic but every sweep pays 7 × 1.1 s on empty air); hub-side
auto-retry (hides, not fixes, the variance — still probabilistic per sweep);
faster announce cadence (spends craft airtime forever to speed a rare
discovery event, and 2 Hz is already the §3.12 fast edge).

Spec: §15.5a new dwell-extension bullet; §15.2 `scout.dwell_ms` bullet
re-worded (base dwell + corrected rationale). Known accepted edge: a channel
carrying only non-announcing waybeam traffic (another ground's uplink, no
craft) burns the full extension once per sweep.

## Pass 73 — venc volatile actuation: /api/v1/live/set, all link writes live-only (ruled 2026-07-23, closes R-E)

**Problem (operator, 2026-07-23):** venc persists every value-changing `/set`
to `/etc/waybeam.json`; adaptive operation (§9.5 rung moves → §9.6
bitrate/caps, §9.11 ladder fps) can write hundreds of times per flight —
flash wear (R-E). Persisting adaptive transients is also semantically wrong:
a craft should not boot into its last emergency rung.

**Rulings (three questions, operator 2026-07-23):**

1. **API shape: new venc endpoint `/api/v1/live/set`** — same field set as
   `/set`, applies to the running config, never saves. Old venc builds 404
   the unknown route, so waybeam-link self-detects: a 404 re-sends the push
   on `/set` (no lost actuation), and the fallback **latches only when that
   `/set` re-send succeeds**, re-probing `/live/set` at most every 10 min
   thereafter. The latch condition was hardened during bench verify
   (2026-07-23): venc's httpd binds seconds before its routes register at
   pipeline bring-up/respawn, and the first-cut latch-on-any-404 wrongly
   latched during a venc restart — silently reintroducing persist-on-set
   until the next link restart (caught as unexplained `Config saved` lines
   correlating with commands). Rejected: `persist=0` param on `/set` — old
   builds 400 unknown params as "unknown config field", which would need a
   fleet config flag instead of self-detection. Venc-side: `live/set`
   serves MUT_LIVE fields only (bitrate, caps, fps — everything waybeam-link
   actuates); restart-class fields are rejected there because a respawn
   reloads from disk, which would silently discard a volatile value.
   Revisit when RESOLUTION/FRAMING (§11.7 v2 staged) unstage.
2. **Policy: ALL waybeam-link venc writes go volatile** — controller-driven
   §9.6/§9.11 AND operator §11.7 v2 commands. Supersedes the Pass 71
   command-persistence exception: encoder effect now matches §11.7
   volatility (reboot = boot config), matching the `cmd_*_select`
   session-reset semantics, and removes the restore-venc-after-bench chore.
   Rejected: adaptive-only volatile (keeps the Pass 71 asymmetry and the
   bench chore for a persistence nobody relies on — re-establishment after
   reboot is by re-issuing idempotent commands, §11.7).
3. **Persistence is an operator act only**: venc's own UI/config `/set`
   path keeps persist-on-set; waybeam-link never writes it.

Spec: §9.6 endpoint block + volatile-first bullet (write-on-change bullet
retained, re-scoped); §9.6 Pass 37 rationale sentence annotated; §11.7
persistence paragraph replaced (Pass 71 asymmetry now only the documented
pre-live-venc fallback mode); §13 forged-VEHICLE_CMD bound re-worded; §15.3
`venc_*`/`cmd_*_select` notes updated. R-E closed. Venc-repo change lands as
its own PR in waybeam_venc (persist flag threaded through the set pipeline +
route registration; identical response shapes).

## Pass 74 — Passive spectator RX (analog-video model): no-uplink display node (ruled 2026-07-24)

**Problem (operator, 2026-07-24):** the kernel-monitor backend requires
exactly one `role:"tx"` uplink adapter for any node with delivery streams —
only a store-cache with *no* streams is exempt (`allow_rx_only =
cache.store.enabled && streams.empty()`). This refuses a **passive display
receiver**: a node that just tunes in and watches a broadcast the way an
analog FPV monitor does. Yet §2 (passive latch, "no handshake, no
association") and §13.x ("multi-host spectator/DVR … any node RX + *optionally*
NACK") already sanction exactly this — TX is optional for a receiver. The
backend invariant was over-broad; unencrypted waybeam-link is meant to be
analog-video-like, and passive RX is core MVP, not an add-on.

**Ruling (operator 2026-07-24):** a node opts into passive-spectator via
`node.spectator: true` (fail-closed: without the flag a streams node with no
uplink still errors, so an ordinary ground that lost its uplink is not
silently downgraded). A spectator:

1. **Runs with zero `role:"tx"` adapters** — passive display, **FEC +
   diversity best-effort, NO ARQ / NACK / LINK_REPORT** (the §6/§9 return and
   §3.9 recovery paths already no-op with no tx adapter — `inject`/
   `inject_return` are guarded on `has_tx`). This is the analog-like tradeoff:
   a lost frame is a lost frame.
2. **Scouts by roaming the backend tx adapter when present, else config
   index 0** (its sole / first RX adapter). A single-adapter spectator's sweep
   is mode-exclusive with its view — the analog "channel search" — as §15.2
   already notes for a single-adapter ground.
3. **Selects a feed by a passive tune + latch, never a §11 claim.**
   quickconnect on a spectator retunes all ears to the scouted feed's channel
   and net_id; §2 first-latch / `preferred_originator` picks up the stream. No
   issuer campaign — a spectator has no uplink for one, and the `csa_psk`
   trust boundary stays craft + ground.
4. **Does not follow CSA channel moves** (no `csa_psk` to validate a
   campaign). Recovery from a craft channel-hop is a **re-scout** — the
   spectator re-acquires by discovery, not by following.

**Rejected:** auto-detecting spectator from "zero tx adapters" (silently drops
ARQ on a real ground that merely misconfigured its uplink — the opt-in flag
fails closed); unauthenticated CSA-follow for spectators (would let any node
yank every spectator across channels — re-scout is safe and needs no secret).

Spec: §2 passive-latch note (spectator = the no-uplink case); §15.2
`node.spectator` bullet + scout-adapter resolution reworded (tx adapter, else
config index 0); §15.5 select = passive tune for a spectator. Known accepted
edge: multiple feeds on one channel — a spectator first-latches one; explicit
per-originator select among co-channel feeds is deferred (config
`preferred_originator` pins one meanwhile).

## Pass 75 — venc encoder-capability bitrate ceiling (§9.6, ruled 2026-07-24)

**Problem (operator, 2026-07-24):** the §9.5 per-rung derived bitrate has only
a **floor** (`bitrate_min_kbps`) — no ceiling. On a strong link the top rungs
derive well above what the SoC encoder + pipeline can sustain (on the seed
table at 600‰ airtime: MCS4 ≈ 25.9 Mbps, MCS5 ≈ 34.6 Mbps), and waybeam-link
as the sole §9.6 authority will command it. An SSC338Q-class SoC cannot
sustain much beyond ~25 Mbps of encode + framing + injection. Capping airtime
would also work but conflates two different limits (channel occupancy vs CPU);
the operator ruled 65 % PHY airtime is fine and the real constraint is encoder
CPU.

**Ruling (operator 2026-07-24):** add `venc.max_bitrate_kbps` (default `0` =
unlimited), an **encoder-capability ceiling** independent of the rung. The
§9.5-derived bitrate is clamped to `min(derived, max_bitrate_kbps)` before
§9.6 actuation, `venc_bitrate_kbps` reporting, and §9.6/§9.11 cap coupling —
so everything downstream sees one clamped value. Rung-independent: the
selector still climbs MCS for link robustness, but above the rung where
`derived == ceiling` the extra PHY capacity becomes airtime margin, not video
bits (MCS4 and MCS5 both command 25 000 at a 25 Mbps ceiling). The ceiling
must be ≥ the venc hard floor 1000 (rejected at config load otherwise) and
should exceed the table's `bitrate_min_kbps` to be meaningful; it never lowers
the per-profile floor below its own value in normal (`max ≥ floor`) configs.

**Rejected:** lowering the table `airtime_budget_frac` to cap the top rung
(operator wants 65 % PHY; and it's a fleet-wide table-hash change conflating
occupancy with CPU); a `max_profile` cap (blunt — bounds the rung, not the
bitrate, and forfeits the higher rung's link robustness).

Spec: §9.6 `venc.max_bitrate_kbps` bullet (encoder ceiling on the §9.5 derived
target); §15.2 venc config note. Wired as `SelectorPolicy.max_bitrate_kbps`.

## Pass 76 — §15.3 link stats gains `channel` (operating RF center MHz) (ruled 2026-07-23)

_(Renumbered from Pass 74 → 76 on 2026-07-24: this ruling was authored in a
parallel session dated 2026-07-23 but merged after Passes 74/75 landed, so it
takes the next free number. Ruling content unchanged.)_

**Problem (operator, 2026-07-23):** the ground station's OSD needs to show
which channel the link is on and whether a §11 follow-me switch is in flight.
`csa_state` was already in the §15.3 `link` block (so "switch ongoing" was
observable), but the current operating channel was **not** in the stats stream
at all — it lived only in the `/api/v1/info` config dump (`adapters[].channel`,
static) and the scout's `/api/v1/scout/results` (`current_chan`). A consumer
that already scrapes `/api/v1/stats` would have to add a second, differently-
shaped scrape just to render the channel next to the CSA state.

**Ruling (operator 2026-07-23):** add a single `channel` field (uint16, RF
center MHz) to the §15.3 `link` block, immediately after `csa_state` (the two
are the follow-me pair). Semantics:

- It is the **rx node's live committed channel** — sourced from the rx loop's
  `operating_chan`, which is seeded from `adapters[0].channel_mhz` and updated
  as §11 CSA campaigns commit (`app/main.cpp`). This is the authoritative
  runtime value, not the static config, so it stays correct across switches.
- `channel` is `0` on nodes that do not track a runtime operating channel
  (tx / loopback emit_stats call sites pass 0). The ground station runs the rx
  loop, so the node a hub scrapes reports the real channel.
- No new endpoint, no second scrape: `csa_state` + `channel` together answer
  "which channel, and is a switch in flight" from the one stats line.

Rejected alternatives: (a) surfacing the per-adapter config channel from
`/api/v1/info` — static, goes stale the moment CSA commits; (b) a hub-side
second scrape of scout `current_chan` — scout is a distinct subsystem and not
always active. The golden §15.3 schema test (`tests/stats_test.cpp`) is updated
in lockstep (fixed field order is contract).
## Pass 77 — AUDIO stream type (§3.4): vehicle→ground Opus carriage (ruled 2026-07-24)

_(Renumbered from Pass 76 → 77 on 2026-07-24: authored in a parallel session
before Pass 76 (link.channel) merged; takes the next free number. Ruling
content unchanged.)_

**Problem (operator, 2026-07-24):** the vehicle already produces audio —
waybeam_venc captures + Opus-encodes at **50 Hz** (20 ms frames, RFC 7587,
`src/star6e_audio.c`) and emits them to a dedicated UDP port that, in the
co-located topology, already lands on `127.0.0.1`. There was no way to carry
that stream vehicle→ground over the link. The transport is already
stream-generic (multiplexes by §3.4 `stream_type`; the §15.2 sample runs RTP +
TELEMETRY side by side today), so the only gap was a registry name — but adding
one is a §3.4 amendment, and how audio should be carried (reliability class,
which type value) is a ruling, not an inference.

**Ruling (operator 2026-07-24):**

1. **`0x04 AUDIO` is a canonical stream type**, not a vendor `0x10–0xEF` value.
   Audio is first-class MVP media alongside RTP/TELEMETRY/CONTROL and deserves a
   named config token (`"AUDIO"`) and a stable registry slot, not a per-build
   number. Carried **opaque** end-to-end exactly like RTP — the core never
   parses Opus/RTP.
2. **ARQ stays OFF for audio (best-effort, diversity-only).** Audio takes the
   existing non-RTP framer path: one ingress datagram = one block, `END_OF_BLOCK`
   set, `ARQ=0` (no NAL/size classifier). Rationale: Opus PLC conceals an
   occasional dropped 20 ms frame, per-adapter diversity is the redundancy
   (§1), and keeping 50 blocks/s of audio **out of the §5.3 resend airtime
   budget** protects the I-frame ARQ pool it would otherwise compete with. FEC
   is the lever if audio ever needs hardening, not ARQ.
3. **No core/framer/scheduler change.** The wire codec treats `stream_type` as
   an opaque u8 and RX latches wants by `stream_type` (`core/src/rx.cpp`); the
   whole change is the registry name (`stream_type::kAudio` in
   `core/include/wblink/types.h`, `"AUDIO"` in `io/src/config.cpp`
   `parse_stream_type`) plus example configs. A distinct type value is
   **load-bearing**: RX out-stream wants are matched by `stream_type`, so reusing
   `TELEMETRY` would collide the audio and telemetry egress wants.

**Quiet-gap interaction (accepted, bench-observe not redesign).** A non-RTP
stream sets `END_OF_BLOCK` on every datagram, so an always-on 50 Hz audio stream
adds **~50 §7.2 listen windows/s** interleaved with video's EOB pacing on the
single-radio craft. The operator accepts this — 50 pps is expected to sit inside
the headroom, and the pacer already skips the gap when the next block is
airtime-critical. Flagged as a §17 gate-4 observation (does audio-EOB pacing
measurably erode video airtime or perturb the return-window fit), **not** a
redesign; revisit only if the rig shows it.

**Rejected:** reusing `TELEMETRY` (RX want-matching keys on `stream_type` —
audio and telemetry egress would collide); a vendor `0x10+` value (audio is core
media, wants a named token and a stable slot); ARQ-on audio (competes with
I-frames for the §5.3 resend airtime; a lost 20 ms frame is PLC-concealable and
past-deadline before a repair would land at 50 Hz).

Spec: §3.4 `0x04 AUDIO` row. Wired as `stream_type::kAudio` + the `"AUDIO"`
config token; example `config.radio-tx/rx.sample.json` gain a second UDP stream
on `127.0.0.1:5601` (matching waybeam_venc's default `audioPort`).

## Pass 78 — §7.2 paced-stream EOB semantics + LINK_REPORT redundancy (ruled 2026-07-24)

**Problem (bench, 2026-07-24, measured on the rig at MCS5/25 Mbps/100 fps
with the Pass 77 AUDIO stream live):** with 50 Hz audio flowing the adaptive
link never holds — full-range rung flapping (8–24 profile changes per ~3 min,
excursions to MCS1, §9.8 watchdog trips in some phase alignments) — while the
identical link with audio off sits pinned at MCS5 with zero changes. A/B
report-heard ratio barely moves (≈40–48% both ways); the damage is episodic
multi-report deaf spells. Root cause, two code sites:

1. **Craft** (`send_raw` → `note_eob_sent`): every EOB — and every 20 ms
   audio datagram is a one-datagram block with EOB — re-armed the §7.2 quiet
   gap, including *mid-flush*. Held video stalled behind each audio EOB, the
   backlog inflated until the `skip_backlog` airtime-critical override fired,
   and the craft then transmitted straight through its own listen windows —
   deaf at exactly the midpoints the ground aims for. The 50 vs 100 Hz phase
   drift makes it episodic, not constant.
2. **Ground** (rx loop EOB anchor): the pending return deadline was
   re-anchored on *every* EOB heard, audio included, so the report aim-point
   churned with audio's unrelated cadence.

**Ruling (operator 2026-07-24): A + B, both adopted.**

- **A — paced-stream semantics:** EOB pacing/anchoring keys on the RTP video
  stream ONLY. Non-video EOBs neither open craft listen windows nor re-anchor
  ground returns. Non-video injection stays *gated* by an open gap (held
  datagrams flush back-to-back after the window) — audio never transmits into
  a listen window, and never re-arms one. This restores the pre-audio pacing
  contract exactly; audio rides between gaps at a ≤`window_us` queueing cost,
  irrelevant at 50 Hz.
- **B — report redundancy:** `return.report_redundancy` (seed 2, 1 disables;
  RE-DERIVE §17). An anchored LINK_REPORT batch is re-sent once at the *next*
  return window — spread across two listen gaps, never back-to-back within
  one (deafness is correlated inside a window). Blind fallback batches are
  not repeated. TX-side `reports_received` now counts only selector-fresh
  epochs so duplicates/replays cannot inflate the §15.3 heard-ratio (this is
  a deliberate stats-semantics tightening).
- **C — hardware-ACKed unicast returns (Pass 12)** stays a §17 gate-4 slot
  and is confirmed **devourer-gated**: kernel monitor can send unicast
  QoS-Data but cannot arm the craft-side ACK responder, so the full hybrid
  needs the devourer backend on the craft.

**Also noted (baseline, pre-existing):** even audio-off, only ~45% of ground
reports are heard at 100 fps/MCS5 — the §7.2 crossover the spec already
flags. B mitigates; C is the structural fix candidate.

Spec: §7.2 paced-stream + report-redundancy paragraphs; §15.2 `return`
gains `report_redundancy`. Wired as `frame_is_paced_eob()` at the craft
pacer and ground anchor, `ReturnPolicy::report_redundancy`, and the
ground-side repeat queue.

## Pass 79 — LINK_REPORT is video-stream-only (§7.3): per-stream loss must not steer selection (ruled 2026-07-24)

**Problem (bench, found by the Pass 78 benchmark):** with the return path
healthy (67% unique reports heard, ages p90 175 ms, zero watchdog trips) the
selector STILL flapped — instant 5→1 rung drops with perfectly fresh reports,
one every ~10–40 s, immediately re-climbing. Root cause is pre-existing and
merely exposed by Pass 77 audio: `Reporter::build()` emits **one LINK_REPORT
per latched stream** (audio included) and the §3.5 gate filters by originator
only, so the audio stream's report feeds `Selector::on_report`. The
denominators make a low-rate stream's loss fraction explosive: a ~100 ms
report period holds ~5 audio datagrams, so ONE lost audio packet reports
200‰ against `demote_milli = 20` — while the identical RF burst is ~3‰ on
video. Measured ~1.5‰ audio loss at 50 pps ⇒ one such event every ~13 s,
matching the drop cadence exactly. Latent for ANY non-video stream (a
TELEMETRY stream reports the same way); the bench never ran one over RF
before audio.

**Ruling (operator 2026-07-24):** selection feedback keys on the RTP video
stream only.

1. **Reporter emits LINK_REPORTs for RTP streams only.** Fix at the source:
   halves report airtime back to pre-audio, keeps the §9.8 epoch counter
   meaningful (every epoch is a selector-relevant report).
2. **TX-side defensive filter:** a report whose `target_stream_id` is not one
   of the node's RTP streams is ignored before the gate/selector — a new
   craft stays stable against an old ground during mixed-version windows.
3. Non-video loss remains observable in the RX's local §15.3 stream stats;
   it just no longer steers the link. If a future consumer wants remote
   non-video loss telemetry, that is a new field/type, not a selector input.

**Also ruled (operator):** re-verify on a second channel (149 / 5745 MHz) to
exclude channel/antenna effects from the benchmark conclusions.

Spec: §7.3 video-stream-reports-only bullet. Wired in `Reporter::build()`
(skip non-RTP streams) and the craft report intake (stream-id filter).

## Pass 80 — craft post-retune RX-liveness guard (§11.6): CSA must not strand the fleet (ruled 2026-07-24)

**Problem (bench, found during the Pass 79 cross-channel verification):** a
§11 CSA 5805→5745 left the craft 8812EU **half-retuned**: TX kept airing on
5745 (both grounds decoded ~100 fps for minutes) while the RX chain went
completely deaf (adapter rx counter frozen, zero reports for 4+ minutes,
§9.8 FAILSAFE at the floor) and `iw` later showed the radio back on the
origin channel. A deaf craft cannot hear the return-CSA, so the fleet was
**stranded** on 5745 until an operator-side craft link restart (full monitor
bring-up) recovered it. A restart-based native bring-up on the same channel
is flawless — the trigger is specifically the in-place `iw set freq` retune
path on the RTL88x2 family (same family as the known ground-side EU
txpower/reinit quirk). Note the earlier fleet-sanity CSA round trip
succeeded, so the wedge is intermittent — worse for follow-me in flight,
and the §17 motivation for the 10× soak below.

**Ruling (operator 2026-07-24, "10x csa retune verification run with the
liveness check"):** adopt the craft post-retune RX-liveness guard:

1. After ANY §11 retune (commit or revert) the craft records its adapter RX
   counter and arms `csa.rx_liveness_ms` (seed **750 ms**, `0` disables;
   RE-DERIVE §17). The issuer's zero-dt rendezvous beacons blanket the
   verify window, so total silence for the deadline ⇒ half-applied retune.
2. Recovery is **one** full monitor re-init of the adapter (link down →
   monitor type → link up → `iw set freq` — exactly the bring-up sequence),
   loud in the log, one-shot per retune. The §11.5 machine is untouched —
   the guard only restores the radio the machine already assumes it has.
3. Scope: kernel-monitor backend (where the wedge is observed); the devourer
   backend keeps its own §11.5a fast-retune path and is out of scope until
   it exhibits the failure.
4. Verification bar: a **10× alternating CSA soak** (5805↔5745) with per-hop
   checks — selection committed, craft RX advancing, report age fresh,
   video on both receivers, audio stream advancing — plus recovery-fire
   count from the craft log.

Spec: §11.6 guard bullet; §15.2 `csa.rx_liveness_ms`. Wired as
`MonAir::recover()` (bring-up sequence via forked `ip`/`iw`) + the craft
CSA-action hook in `app/main.cpp`.

## Pass 81 — §2.1/§6.6: the no-wrap assertion is not a memory-safety premise (ruled 2026-07-24)

**Problem (pre-flight audit 2026-07-24, reproduced):** `RxEngine::note_gaps`
bounded its enumeration loop on the §6.6 clamp invariant
(`max_seq - cursor <= fwd_clamp_pkts`), which does not survive the u32 `seq`
wrap. `advance_cursor` does a bare `++s.cursor`, and `plausible_forward()`
deliberately has no modular arithmetic and treats every backward candidate as
plausible, so nothing stops the cursor crossing `0xFFFFFFFF` while `max_seq`
stays behind it. **Four chosen packets** (`0xFFFFFFFD/FE/FF`, then `0x0`) —
satisfying §2 admission on the attacker's own invented tuple, no jamming, no
prior state — then run the loop over the full u32 space allocating a `Gap` node
per iteration: the ground's video pipeline hangs and OOMs. Also reachable
against an already-latched stream via the §6.6 sustained-clamp resync escape.

**Ruling (operator 2026-07-24, "i agree with all"):** §2's no-wrap property
describes a *legitimate* sender's counter and is a valid basis for **correctness**
(plain integer comparison in §6.6). It is **not** a bound on what values RX can be
made to hold — the §2 startup floor and the §6.6 resync both adopt a floor from
the air. Therefore no RX loop bound, allocation size or buffer index may derive
from it; such bounds must hold independently, against the config-bounded
`fwd_clamp_pkts`/`fwd_clamp_blocks`. A cursor that wraps past `max_seq` is a
desync and re-floors with §2 startup-floor semantics, counted as `resyncs`.

**Explicitly NOT adopted:** switching `plausible_forward()` to modular
arithmetic. The existing comment's reasoning stands — modular comparison makes a
forged far-*backward* seq look forward-plausible and reopens the video-flush hole
§6.6 exists to close. Bound the loop; keep the clamp non-modular.

Spec: new §2.1; §6.6 bounded-gap-enumeration bullet. Wired as an unconditional
`fwd_clamp_pkts` cap in `RxEngine::note_gaps()` plus a wrap re-floor in
`RxEngine::on_data()`. Test debt closed alongside: there was no adversarial
harness on `RxEngine::on_data` at all (`wire_fuzz_test` stops at `decode()`),
which is why this survived the full 46-suite gate.

## Pass 82 — §3.6 canonical serialization is 27 bytes, not 25 (ruled 2026-07-24)

**Problem:** §3.6 normatively pins the `table_version` hash input at "25 bytes
per profile", ending at `reserve_telemetry_bps`. The shipped implementation
hashes **27** — `max_payload` u16 appended at offset 25
(`kCanonicalProfileSize = 27`). The §9.3 `max_payload` ruling amended §3.2/§9.3
but not §3.6, where the hash is normatively defined, leaving the wire hash
undocumented. Any implementation written against §3.6 v1 computes a different
byte for the same table → permanent §3.4 best-effort on both ends → Pass 87.

**Ruling (operator 2026-07-24):** amend the **spec** to 27 bytes; the
implementation is correct and does not change. `max_payload` is a semantic
profile field, and §3.6's own stated invariant — the hash changes on any semantic
change to any profile field — requires its inclusion. Rotating `table_version`
across the deployed fleet (craft `.232`, grounds `.242`/`.199`) would drop every
node to §3.4 best-effort for no benefit.

**Follow-up (test debt):** `tests/table_hash_test.cpp` pins `0x41` for the
example table, which locks in the *implementation*, not the *spec* — this class
of divergence is invisible to CI by construction. A hand-serialized fixture
derived from the §3.6 byte layout, with its expected CRC-8, now cross-checks the
two.

Spec: §3.6 field list + amendment note. No code change.

## Pass 83 — §9.7 `min_profile`/`max_profile` are profile ids (ruled 2026-07-24)

**Problem:** `SelectorPolicy` documented these as "indexes into the ladder by id"
— self-contradictory — and `Selector::clamp_rung()` used them as raw indices into
`table_->profiles` in **file order**, while `floor_profile` in the same struct
resolves by `id`. §9.7 never defined the pin's semantics; code picked silently.
Invisible today only because `profiles/table.example.json` happens to have
`id == index == mcs` for 0–7 ascending.

**Ruling (operator 2026-07-24):** they are profile **ids**, resolved like
`floor_profile`, independent of JSON order. A configured id absent from the table
is a config error rejected at load with the offending value named — never a
silent clamp to a neighbouring rung. `{"max": 255}` still unpins by saturating
above the highest present id.

Rationale: `floor_profile` already carried id semantics in the same struct;
operators author these as MCS-bearing ids (the vehicle's "MCS1-5"); an id
survives a table reordering that an index does not. **Blast radius on the
deployed fleet: none** — the shipped table resolves identically under both
readings, which is exactly why it was fixed now rather than after someone
authors a table with a gap, a non-zero base, or out-of-order entries.

Spec: §9.7 pin-semantics paragraph. Wired in `Selector::clamp_rung()` +
config-load validation.

## Pass 84 — §9.8 fail-safe descends to `floor_profile`, unclamped by the §9.7 pin (ruled 2026-07-24)

**Problem:** the §9.8 lost-feedback descent target ran through `clamp_rung()`, so
`min_profile` bounded it. The vehicle config's deliberate `min_profile: 1`
(#47, adaptive MCS1-5 — a sound airtime choice) therefore *also* removed MCS0,
the table's `floor_profile`, from the fail-safe. The most robust rung was
unavailable on the one path that runs when the link is worst and the craft is
furthest away.

**Ruling (operator 2026-07-24):** separate the two concepts. §9.7 `min_profile`
is an **adaptation envelope** — how low the selector may *choose* to go while it
can see feedback. §3.6 `floor_profile` is a **safety floor** — where the link goes
when feedback is *gone*. The pin is not consulted during `FAILSAFE`; it resumes
governing on the first fresh `report_epoch`. A craft with `min_profile: 1` /
`floor_profile: 0` thus adapts within MCS1–5 and still descends to MCS0 on lost
feedback.

Conflating them means an operator cannot tune airtime efficiency without silently
weakening the fail-safe — the "never fail optimistic" violation §9.8 opens by
forbidding. Rejected alternative: setting `min_profile: 0` on the vehicle, which
fixes this flight and leaves the coupling as a trap for the next config.

Spec: §9.8 descent-target table. Wired as an unclamped `floor_profile` target in
`Selector::evaluate()`'s failsafe branch.

## Pass 85 — §11.4a an absent CSA key is a fault, not a mode (ruled 2026-07-24)

**Problem:** §11.4a states "HMAC is always applied — there is no
unauthenticated-CSA mode for craft/ground", but `CsaFollower::verify()` made the
MAC check conditional on `!policy_.psk.empty()`. `CsaFollower` cannot distinguish
craft/ground from spectator, so an empty key silently meant *accept any CSA whose
target is in my allowlist*. A craft whose announced token failed to populate — a
missed ANNOUNCE, a config typo, the degraded-entropy path — retunes off-channel
on a forged frame mid-flight. `VcmdCraft::on_cmd()` already fails closed on the
same condition; the two disagreed.

**Ruling (operator 2026-07-24):** for craft/ground both key sources are
exhaustive (secret configured, or announced token self-generated at boot), so an
empty key is always a bug and MUST fail closed — reject, count, stay put. The
permission to follow unauthenticated belongs to the **role** and MUST be an
explicit policy input (`allow_unauthenticated`, true only for a §15.2
`node.spectator`, Pass 74), never inferred from a zero-length key: a security
posture that is a side effect of an empty container is one refactor away from
silently inverting, and gave craft, ground and spectator one code path with three
different intended outcomes.

Spec: §11.4a absent-key paragraph. Wired as `CsaPolicy::allow_unauthenticated`
(default **false**) + `node.spectator` config wiring + a `csa_unauth_rejected`
counter.

## Pass 86 — §11.5 `t_revert_ms` may shorten the VERIFY window, never lengthen it (ruled 2026-07-24)

**Problem:** the follower's VERIFY window preferred the wire's
`campaign_.t_revert_ms` when non-zero, falling back to the node-local
`verify_timeout_ms`. §11.5 specifies the local 150 ms and §11.1 calls the wire
field the "follower auto-revert budget"; precedence was undefined. The field is
u16, so one frame setting 65535 strands a follower **65 s** on a dead channel —
and until Pass 85, an empty-key node accepts that frame from anyone.

**Ruling (operator 2026-07-24):** the effective window is
`min(t_revert_ms > 0 ? t_revert_ms : verify_timeout_ms, verify_timeout_ms)`. An
issuer that knows its campaign is fast may tighten the fleet's revert; it can
never extend how long a follower sits on a channel it cannot hear. How long a
node is willing to be deaf is the node's own decision.

Spec: §11.5 precedence bullet. Wired in `CsaFollower`'s VERIFY deadline.

## Pass 87 — §3.4 best-effort must keep ratcheting `max_block` (ruled 2026-07-24)

**Problem (pre-flight audit, reproduced):** `RxEngine::on_data()` updated
`s->max_block` only inside `if (!s->best_effort)`, while the §6.6 block clamp
reads it unconditionally. Once best-effort latched on a `table_version` mismatch
(sticky, never cleared), `max_block` froze; `fwd_clamp_blocks` (4) blocks later
every packet clamp-rejected until the 500 ms resync escape fired and flushed all
held/gap/block state — then repeated forever. Measured over 200 blocks at 30 fps:
`delivered=50/200, clamp_rejected=160, resyncs=9`. §3.4 promises "deliver by
diversity" as a *graceful* degradation; instead the single most likely field
misconfiguration — a profile-table skew, exactly what `table_version` exists to
detect — turned a healthy link into ~7 fps of shredded video.

**Ruling (operator 2026-07-24):** `max_block` (and `last_delivered_block`) are
§6.6 clamp state, not §3.4 profile state, and ratchet unconditionally. Only the
`BlockInfo` deadline/supersession bookkeeping belongs inside the best-effort
guard. This is the same failure §6.6 already documents for a delivered-block
reference ("freezes during the fade and then clamp-rejects the entire recovering
stream forever") — best-effort reintroduced it by a different route.

`tests/rx_test.cpp` covered best-effort with `block_id = 1` on every packet, so
the freeze was invisible; multi-block best-effort coverage added.

Spec: §3.4 clamp-state note. Wired in `RxEngine::on_data()`.

## Pass 88 — deployment hardening carried with the above (no spec change)

Not rulings — unambiguous defects found in the same audit, recorded so the fixes
are traceable to it:

- **`SIGPIPE` was never ignored.** `main()` installed SIGINT/SIGTERM only, and
  `MSG_NOSIGNAL` appeared exactly once in the tree (`control_server.cpp`). A venc
  restart across the `venc_http` `send()`, or a log reader exiting on the §15.3
  stdout NDJSON, terminated the flight process. Now `SIG_IGN` at startup +
  `MSG_NOSIGNAL` on the venc send.
- **`MonAir::recover()` did not mirror `mon-up.sh`** despite saying so: it omitted
  `iw set monitor otherbss` (foreign-BSS admission) and `iw set txpower auto` —
  precisely the two steps the known RTL88x2 quirks depend on, including the
  ground-side EU `-100 dBm`-after-reinit case (Pass 48). The Pass 80 guard could
  therefore "recover" a radio that was still deaf or mute. Sequence corrected;
  a post-recovery liveness re-check now runs rather than one-shot-and-hope.
- **`decode_data()` enforced no `kMaxDataPayload` ceiling** — checked on TX
  ingress and in FEC subheaders, never on the RX decode path.
- **`RxCore::reset_stats()` never called `reporter_.reset_link()`**, so the §3.5
  LINK_REPORT loss window underflowed to 0‰ for one window after
  `POST /api/v1/stats/reset` — the optimistic direction §3.5 forbids.
- **`cmd.copies` / `cmd.retry_cap` were unvalidated `uint8_t`**: `0` underflowed
  through `--` to 255, turning a 3-copy VEHICLE_CMD into a 256-copy transmit
  storm.
- **`waybeam-ground.service` gated on `ConditionPathExists`**, which makes systemd
  *skip* rather than fail the unit, so `Restart=on-failure` never applied — late
  USB enumeration meant no ground receiver, silently, with no retry.
- **No respawn on craft or RK ground.** A craft process death in the air was
  terminal — there is no second link. Both inits gained a supervisor loop
  (start → wait → respawn with a 2 s backoff, ended by an explicit stop flag so
  `stop` cannot race a respawn); the RK init also gained a bounded interface
  wait, the same late-enumeration failure as the systemd unit. Both verified in
  a sandbox: start, induced crash → respawn, clean stop with zero leftovers.
- **`nack_grace_ms: 0`** in both deploy configs, discarding the Pass 50 measured
  default of 3 ms (−22.5% NACK packets, −21.3% vehicle resends).
- **`.199` carried `role: "tx"` on `wlx40a5ef2f229b`**, the adapter Pass 48/49
  isolated as a silent transmitter and ruled RX-only — either a dead return path
  or a second uncoordinated uplink colliding with `.242` in the craft's single
  §7.2 EOB listen gap. Set to `node.spectator: true`, the flag Pass 74 created
  for this node shape and which was unused anywhere in `deploy/`.

### Pass 89 — §11.6 video-verify requires a COMMITTED craft; §11.5 window re-sized on the tail

**Trigger.** Hop 4 of the first Pass 80 soak (2026-07-24) split the fleet: the
issuer logged `csa: campaign confirmed -> 5745 MHz` while the craft logged
`csa: IDLE -> 5805 MHz`. Ground held the new channel, craft reverted to the old
one, and the ground reported success. Recovery needed an operator re-scout plus
re-claim — impossible airborne. Reproduced on the pre-Passes-81–88 binary, so
pre-existing, not a regression.

**Diagnosis.** The two ends confirmed on different evidence. The craft
(`CsaFollower`) commits only on hearing the issuer within its §11.5 window. The
issuer (`CsaIssuer`) declared success on `video_seen_` — *any* craft DATA on
`target_chan` after T_switch. But the craft transmits throughout its own VERIFY
window, before deciding, and `CSA_ARMED` was cleared at the switch
(`app/main.cpp:3155`, "the ACK window is over"), so committed video and
still-deciding video were byte-identical to the issuer.

**Measurement (the operator asked for numbers before a ruling).** Both ends were
instrumented on a bench branch and run with a 3000 ms window so nothing
reverted, over 8 alternating 5805↔5745 hops:

- **A** — craft landing → heard the issuer (ms): 45.5, 49.8, 57.6, 58.6, 73.1,
  76.2, 79.0, **143.0**. Median 66, max **143.0**.
- **B** — issuer landing → first craft video (ms): ≤ **1.2** on every hop, with
  the issuer's deadline anchored at `now` (never `switch_at`) on all 8.

Two conclusions fell out:

1. The 150 ms `verify_timeout_ms` was sized from a *median* ("bench median
   85 ms + margin"). The craft reverts on the **tail**. Max 143.0 vs a 150 ms
   window is 4.7% margin — the config was marginal on its own terms, before any
   fix. → **500 ms**, ~3.5× measured max, inside the 750 ms RX-liveness guard.
2. B ≈ 0 means the **craft lands first and waits**; the craft's commit is
   *caused by* the issuer's arrival, so `issuer_landing ≈ craft_landing + A`.
   The craft's commit signal therefore appears at the very start of the
   issuer's window, not in a race with its end. This **retracts** the concern
   raised when the directions were first put to the operator — that requiring a
   craft-commit signal could invert the race and needed asymmetric window
   sizing. It does not. Equal windows are correct.

**Rulings.**

- §11.6: video-verify MUST latch only on a craft DATA frame on `target_chan`
  after T_switch **with `CSA_ARMED` clear**. A set bit means "arrived, still
  deciding" and does not satisfy verify.
- §3.2 bit 4: `CSA_ARMED` lifetime extends to the whole campaign — set on
  accept, cleared on reaching COMMITTED. The pre-T_switch ARM-ack use is
  unaffected; the issuer latches `armed_seen_` before the switch and the bit
  simply persists past it.
- §11.5: `verify_timeout_ms` default 150 → **500 ms**, on both ends. Re-measure
  whenever the issuer's adapter count or retune path changes, since A is
  dominated by the issuer's landing delay.

**Failure mode after the fix.** A campaign the craft does not confirm ends with
*both* ends reverting to `prev_chan`, instead of the ground holding a channel
the craft has left. Recoverable in the air.

### Pass 90 — §11.2 campaign copies: retransmit-until-ACK, released in the quiet gap

**Trigger.** After Pass 89 removed the fleet-split failure mode, hops still
failed intermittently (~1 in 5) on the bench. Pass 89's own hypothesis — that
the issuer needed the craft's §11.6 RX-liveness guard — was **tested and
refuted**, twice over:

1. *Timing.* `rx_liveness_ms` is 750 ms, armed at the retune, while the issuer
   pre-positions (commits before T_switch, Pass 69) and the craft's window is
   500 ms from its own landing. A ground-side guard could not fire until
   ≥250 ms after the craft had already reverted.
2. *Mechanism.* Sampling both ground adapters at 20 Hz through a failing hop
   showed channel and txpower **completely static** — `5805/19.00` and
   `5805/25.00` throughout. The issuer never retuned, because it never reached
   `kCommit`. There was no half-applied retune and no deaf radio to recover.

**Diagnosis.** Every rejection path in `CsaFollower::on_csa` was instrumented.
On the failing hop the craft logged *nothing* — not a rejection, not an accept.
`on_csa` was never called: the campaign was never received.

Measured from the craft's own counters over 188.6 s: rx ≈ **15.1 frames/s**
against tx ≈ **2901 frames/s**. Single radio, RX-deaf while transmitting
(§7.2). A campaign was `kCopies = 5` at `kCopySpacingUs = 20000` — an **80 ms**
burst — after which `kAwaitAck` sent nothing further for the remainder of a 1 s
`ack_timeout`. Taking the observed ~20% hop-failure rate as ~27% per-copy loss,
that is entirely consistent with five blind copies inside one 80 ms window.

The decisive asymmetry: LINK_REPORTs are **gap-scheduled** (`report_ret_held`,
released at the TSF-anchored return deadline) and arrive at essentially the
rate they are sent — `report_hz` 10 against 15.1 rx/s total. CSA copies were
explicitly excluded, with the comment *"campaign timing: never
quiet-gap-held"* (`app/main.cpp:4237`). The one message the campaign depends on
was the only one denied the mechanism that makes delivery to this craft work.

**Operator ruling (2026-07-24).** Do both:

- §11.2: copies repeat at the copy spacing **until `CSA_ARMED` or T_switch**,
  rather than a fixed burst of five. `CSA_ARMED` is already the ACK, so this is
  retransmit-until-acked with a natural stop condition — no new wire state.
- §11.2: copies are **released in the craft's §7.2 quiet gap**, like every
  other ground→craft message.

**Consequence that had to be ruled with it.** `dt_to_switch_ms` is relative to
the copy's own transmission and the follower anchors on that copy's *receive*
TSF, so a copy held for the gap MUST have `dt_to_switch_ms` and `csa_mac`
recomputed at the instant of transmission. Releasing a pre-stamped copy would
put the follower's T_switch late by the hold time — desynchronising the exact
instant the campaign exists to agree on. A copy that cannot be re-stamped
before T_switch is dropped rather than sent stale.

The original exemption predates per-copy `dt` stamping; with it, a gap-delayed
copy is as correct as a prompt one.

**Scope.** This is delivery, not semantics: Pass 89's commit-proof rule and the
500 ms window are unaffected and still required. Pass 89 made failure *safe*
(both ends stay together); Pass 90 makes it *rare*.

**Addendum — ack-lead cutoff (found by re-soaking the merge candidate).**
The first Pass 90 implementation ran copies right up to T_switch, which the
pre-Pass-90 code could never do (its burst ended at 80 ms of a 150 ms budget).
Re-soaking after a late review fix produced one failure in six, and the craft
log named the cause exactly: eight accepted campaigns at dt 465 / 470 / 146 /
140 / 109 / 107 / 106 ms all committed, and the **only** revert was a
**dt = 23 ms** acceptance. A craft accepting that late has ~3 frames to
advertise `CSA_ARMED` before leaving the old channel, so the issuer cannot
reliably pre-position and the jump is uncoordinated.

Ruling: **no copy inside the last 50 ms before T_switch**, applied to emission
and to the quiet-gap re-stamp alike (a hold can push a legal copy past the
deadline). 50 ms ≈ 7 craft frames at the measured ~7.4 ms interval.

This forces an honest correction to the pass's own framing: at **class 0** the
150 ms budget minus the cutoff leaves room for little more than the original
burst, so the measured delivery gain there comes from **quiet-gap scheduling**,
not from extra copies. The retransmission has real room only at class 1. The
tests were retargeted accordingly rather than left asserting a benefit that
class 0 does not actually get.

Two process notes, both worth repeating: the RX-liveness hypothesis was
refuted by measurement before being built, and this cutoff defect was caught
only because the candidate was re-soaked *after* a late code change — the
earlier 20/20 no longer covered the code being merged.

**Status: the cutoff is NOT yet validated, and Pass 80 must be re-run before
this merges.** Stated plainly because the numbers above are easy to misread:

- The **20/20** result belongs to commit `d00ca67` (Pass 90 implementation,
  class 0, no cutoff). It is a real result for that build and nothing else.
- With the cutoff, class 0 gave **1/2** before the run was stopped. Two hops
  is not evidence either way, but it is certainly not confirmation.
- The cutoff shrinks the class-0 copy window from 0–150 ms to 0–100 ms, so it
  trades the late-accept hazard for fewer delivery opportunities. At a 150 ms
  budget these two requirements are in direct conflict and 150 ms cannot
  satisfy both.

**Class 1 is not the workaround.** Tried on the bench (2/3) and it introduced a
*new* split, the inverse of B8: the issuer pre-positions ~490 ms before
T_switch, sits on the target seeing no craft video, and its deadline —
`max(T_switch, landing) + verify_timeout` — gives the craft only 500 ms after
T_switch to finish a class-1 retune that §11.2 itself budgets at up to 277 ms,
hear the issuer, commit, and emit a CSA_ARMED-clear frame. Observed: craft
COMMITTED on 5805 while the issuer reverted to 5745.

**Open question for the next pass — needs an operator ruling.** The likely
resolution is to widen the **class-0 dt budget** (150 ms → ~300 ms) so one
budget holds both a full copy window and the 50 ms ack lead, instead of
forcing a choice between them. That is a §11.2 constant and therefore not
picked here.

### Pass 91 — §11.2 class-0 dt budget 150 → 300 ms

**Ruling (operator, 2026-07-24)**, resolving the open question Pass 90 left.

Pass 90 changed what the class-0 budget has to pay for. It used to size only
the retune it precedes (`FastRetune`, ~0.5–2.5 ms — 150 ms was almost all
margin). It must now hold two things at once:

- a **copy window** long enough to deliver a campaign to a craft that is
  RX-deaf while transmitting (the Pass 90 root cause), and
- the **50 ms ack-lead cutoff** before T_switch (the Pass 90 addendum), so a
  late acceptance still leaves the craft time to get `CSA_ARMED` back to the
  issuer before it departs.

At 150 ms these conflict: the cutoff leaves a 100 ms copy window, barely more
than the pre-Pass-90 burst that measured ~1 campaign lost in 5. The bench
showed the conflict directly — 20/20 with copies running to T_switch and no
ack lead, then 1/2 with the ack lead added at the same budget.

**300 ms leaves a 250 ms copy window (~12 gap-scheduled copies) with the ack
lead intact.** Both requirements are satisfied at once instead of traded.

**Why not just issue class-1 campaigns.** Tried on the bench and rejected:
class 1's 500 ms budget makes the issuer pre-position ~490 ms before T_switch,
where it sits on the target seeing no craft video while its deadline —
`max(T_switch, landing) + verify_timeout` — leaves the craft only
`verify_timeout` after T_switch to finish a retune §11.2 budgets at up to
277 ms, hear the issuer, commit, and emit a `CSA_ARMED`-clear frame. Observed:
craft COMMITTED on 5805 while the issuer reverted to 5745 — the inverse of the
split Pass 89 closed. A longer budget is not automatically a safer one; the
issuer's pre-position window grows with it.

**Cost.** A class-0 campaign now takes 300 ms rather than 150 ms from trigger
to switch. That is dead time on the old channel, not an outage, and it buys
the delivery margin the whole §11 path depends on.

### Pass 92 — §11.5/§11.6 the verify window: a shadowed default and a mis-anchored deadline

**Trigger.** After Passes 89–91 the Pass 80 soak still failed roughly one hop in
three or four, always the same way and always *safely*: the craft armed,
switched, heard nothing inside its window, reverted, and the ground followed it
back. Delivery was demonstrably fixed (the craft armed on every campaign, min
accepted `dt` 235 ms), so the remaining fault was in the verify phase.

Pass 89 had sized `verify_timeout_ms` from eight hops (median 66, max 143 ms)
and flagged that as thin. It was thin — but it was not the fault.

**Defect 1 — the 500 ms window was never running.** `CsaParams::verify_timeout_ms`
(core) and `CsaPolicy::verify_timeout_ms` (§15.2 config) both carried a literal
default, and `csa_params()` copies the config value over the engine's
unconditionally. Pass 89 raised only the core literal. **Every binary built
between Pass 89 and Pass 92 ran a 150 ms window**, and every soak that believed
it was verifying 500 ms was measuring 150. The Pass 89 sizing analysis was
correct and had simply never reached a radio.

One knob, two seed values, one of which silently wins: the config default now
derives from the engine's (`kCsaVerifyTimeoutMsDefault`) rather than restating
it, so the drift cannot recur.

**Defect 2 — the two ends measure the same budget from different events.**
Both windows are `verify_timeout_ms` long, but:

- the **follower's** opens at *its* landing (§11.5, Pass 69 H1b) — T_switch plus
  its own retune cost;
- the **issuer's** opens at `max(T_switch, its own landing)`, and §11.6
  pre-positioning means it has usually landed *before* T_switch.

So the issuer stops beaconing, gives up and reverts a full craft-retune-cost
before the craft does. The tail of the craft's window has no issuer on air.

**Measurement.** Both engines instrumented on `bench/csa-verify-window`, both
windows opened to 3000 ms and `rx_liveness_ms` disabled so nothing reverted and
nothing was rescued. 27 alternating 5805↔5745 hops, desk range, `release`
builds, craft `.232` / ground `.242`:

| quantity (ms) | min | median | p90 | max |
|---|---|---|---|---|
| craft tick lateness at T_switch | 0.0 | 1.6 | 8.6 | 20.4 |
| craft retune cost (T_switch → landing) | 34.8 | 48.7 | 57.3 | 67.9 |
| **A** — craft landing → heard the issuer | 7.7 | 33.8 | 83.7 | **132.8** |
| issuer landing, before T_switch | −2.0 | 37.4 | 39.0 | 234.7 |
| issuer: T_switch → craft ARMED frame (craft landing, observed) | 11.4 | 44.7 | 54.5 | 70.5 |
| **issuer: T_switch → craft CLEAR frame** (commit proof) | 76.7 | **115.6** | **145.2** | **196.9** |
| issuer: craft landing → craft CLEAR frame | 30.9 | 72.6 | 120.2 | 151.7 |

The bolded row is the quantity the issuer's real 150 ms window had to cover.
Median 115.6 passes; p90 145.2 is on the line; max 196.9 fails. That *is* the
observed failure rate, and it explains why the failures were roughly one in
three rather than rare.

Two incidental confirmations, both consistent with the Pass 90 root cause: the
craft heard **0–2** ground frames during an entire ~250 ms ARMED window (it is
RX-deaf while transmitting), and 10 of 27 campaigns were confirmed by a
rendezvous beacon rather than ordinary return traffic — the beacon is doing
real work, not decorating.

**Rulings.**

- §11.5: the Pass 89 **500 ms** stands, now sourced from a single
  `kCsaVerifyTimeoutMsDefault` that the §15.2 config default derives from.
  Against the re-measured A (max 132.8) that is 3.8×.
- §11.6: the issuer's verify deadline **re-anchors once** on the first
  `CSA_ARMED`-set craft frame on `target_chan` after T_switch — the craft's
  landing as the issuer can observe it. Both ends then run the same budget from
  the same event. Against the re-anchored quantity (craft landing → clear
  frame, max 151.7) 500 ms is 3.3×.
- §15.2: `verify_timeout_ms` **must be less than** `rx_liveness_ms` when the
  guard is enabled; config load rejects otherwise. A verify window that outlives
  the RX-liveness deadline lets a monitor re-init fire mid-switch.

**Why re-anchoring rather than a landing-allowance constant.** The alternative
was `issuer_deadline = max(T_switch, landing) + verify_timeout_ms +
kLandingAllowance`, which needs a bench-derived number for the craft's retune
cost — a fourth constant of exactly the kind this Pass exists to clean up, and
one that would go stale on different craft hardware. Re-anchoring removes a
number instead of adding one, and self-calibrates.

**Ordering property (preserved).** The issuer observes the craft's landing one
frame *after* it happens, so the issuer's deadline sits microseconds after the
follower's. On a failed campaign the craft reverts first and the issuer follows
— the Pass 89 safety property, unchanged. There is no interval in which the
issuer has abandoned a craft that is still listening.

**Retraction.** Pass 89's "no asymmetric window sizing is required" is
withdrawn (§11.6 marked). Its supporting measurement — issuer → first craft
video ≤ 1.2 ms — timed a `CSA_ARMED`-*set* frame, under the semantics that same
ruling then replaced. Under the commit-proof rule the issuer must wait for the
*clear* frame, which arrives median 115.6 ms after T_switch. The asymmetry is
real; equal windows are still correct, but only once both ends anchor on the
same event.



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
- [x] **R-E — CLOSED (Pass 73, ruled 2026-07-23).** Venc grew
      `/api/v1/live/set` (volatile apply, MUT_LIVE fields only); ALL
      waybeam-link actuation — controller-driven §9.6/§9.11 writes AND §11.7
      v2 commands — now goes volatile, with a self-healing 404 fallback to
      the persisting `/set` for pre-live venc builds. No persisted baseline
      write: persistence is exclusively an operator act via venc's own UI.
      Original analysis kept below for the record.
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

---

### Pass 94 — §14.1 the `min_k` ARQ-only gate must be conditioned on ARQ eligibility

**Problem (derived 2026-07-24, out of B11).** `FrameFramer::repair_count()`
returns `r = 0` unconditionally for `k ≤ fec.min_k`. §14.1's own rationale for
that branch is *"NACK→RETRANSMIT recovers within deadline (§17 gate 3)"* — it
is an optimisation that trades parity for ARQ. But the branch never checked
whether the frame *has* ARQ. Under `arq_mode: idr-only` — the craft's deployed
setting and the §4.1 default for the P class — a P-frame has none, so the
branch grants neither FEC nor ARQ and the frame ships bare.

With `min_k: 3` and `s = 1387 B`, that is every P-frame under **4161 B**. At
the §9.8 fail-safe floor rung, `derived_bitrate / fps` lands squarely there.
This is B11.

**Evidence.** Two independent derivations that agree; full tables in
`docs/venc-mode-matrix.md` §11.

- *Offline*, driving the real `FrameFramer` and `FrameReassembler` over
  Bernoulli loss — same `ceil()`, same gate, same GF(256) decode. A **17×
  cliff at one byte**: at p = 2 %, 4161 B → 5.785 % unrecoverable, 4162 B →
  0.340 %. Above the cliff the curve is a decreasing *sawtooth*, not monotone
  (k=5 is worse than k=4: `ceil(k·0.2)` gives both r=1 while k=5 has one more
  symbol to lose), with local maxima recurring at k = 5, 10, 15.
- *On hardware*, craft pinned MCS5, `venc.enabled: false`, ground
  `air.rx_drop_permille: 141` per adapter giving a measured 2.1–2.3 %
  effective post-diversity loss, 98.7 fps, bitrate swept to walk k = 2…15.
  `min_k` 3 → 1 via the live `POST /api/v1/fec`, both arms back-to-back on the
  same link: **mean 0.393 % → 0.123 %, a 3.2× reduction, better at 9 of 10
  points** (the exception is 0.000 % vs 0.057 %, both at the noise floor).

The improvement persists at k ≥ 4 where the gate should not bite, because
`B/frame` is a *mean* and the operative quantity is the distribution: real
P-frames vary enough that a tail of every operating point falls under the
gate. Re-running offline with a realistic spread (cv = 0.6) reproduces the
ratio structure — 4.7× at k=2 decaying to 2.0× at k=12 — against the
hardware's 3.2× mean.

**Ruling (operator 2026-07-24, "in the scope of this PR i would say they land
and we try out the model").** Condition the gate on ARQ eligibility rather than
lowering `min_k`. A frame is ARQ-eligible when it would carry `ARQ` or
`PFRAME_ARQ` (§5.1a); above the §4.1 cadence cutoff nothing is, and the gate
goes inert for every class. This keeps `min_k` doing the job it was designed
for — don't spend parity where ARQ covers it — and stops it being a protection
hole. It subsumes the `min_k: 1` config workaround and is strictly better.

Spec: §14.1 policy table + a new conditional-gate bullet.

**Consequence beyond the fix.** With the gate closed there is no frame size
below which protection collapses, so §9.11's `min_p_frame_bytes` (seed 10000,
a standing §17 RE-DERIVE) can be **retired rather than derived** — the §17
derivation it always owed returns "no useful block target". That in turn
retracts the mode-matrix range axis built on it: fps does *not* force an MCS
floor, and the binding constraint is bits per pixel instead
(`docs/venc-mode-matrix.md` §16.0).

---

### Pass 95 — §9.5 `fec_overhead_frac` MUST be non-zero wherever `rlc256` runs

**Problem (found alongside Pass 94).** `core/src/selector.cpp:31` debits
`fec_overhead_permille` from the §9.5 derived bitrate — the only place parity
airtime is accounted for. `profiles/table.example.json` ships
`fec_overhead_frac: 0.0` on **all eight rungs**, while `craft.json` runs
`"scheme": "rlc256"` at 200 ‰ P / 300 ‰ IDR. So §9.5 derives the encoder
target as if there were no parity, and §14.1 then adds parity on top of it.

`docs/findings-pass3.md:286` called this exactly: *"If FEC is adopted it must
be budgeted, not bolted on."* FEC was adopted; it was bolted on.

**Evidence.** Measured live on the craft at MCS5: 183 672 repair / 847 295
source symbols = **21.7 % by symbol count, 22.2 % by wire bytes** — ≈ 180 ‰ of
capacity. Across the hardware bitrate sweep the ratio ran **8.8 % of source at
k≈19 up to 29.1 % at k≈4**, because `r = ceil(k · rate)` inflates as `k` falls
and `k` falls with the rung. Corrected §9.5 bitrates are ~18 % below the
shipped table's on every rung (rung 0: 3804 → 3102), and the
`venc.max_bitrate_kbps` clamp stops binding at rung 4.

**Ruling (operator 2026-07-24).** Non-zero, and **graduated, not flat** — a
flat value under-budgets the low rungs, which is precisely where the parity
ratio is highest and where B11 bites. Authored values MUST be monotonically
non-increasing with rung index. Seeded from the measurement:

| rungs | `fec_overhead_frac` | operating k |
|---|---|---|
| 0–1 | 0.25 | k ≈ 2–4, `ceil()` inflation worst |
| 2–3 | 0.20 | k ≈ 5–10 |
| 4–7 | 0.18 | k ≫ 10, approaches the 200 ‰ `p_rate` asymptote |

Runtime derivation from the framer's own source/repair counters is the better
long-term answer and is explicitly **out of scope**: it closes a loop
(bitrate → frame size → k → overhead → bitrate) that wants its own bench.

**Fleet consequence.** `fec_overhead_permille` is inside the §3.6 CRC-8
content hash (`core/src/table.cpp:42`), so this moves `table_version` off
**0x41**. Both ends must be redeployed together; a mismatched pair does not
agree on the table. The pin in `tests/table_hash_test.cpp` moves with it.

Spec: §9.3 field comment + two §9.5 bullets.

---

### Pass 96 — §15.5 `POST /api/v1/mode`: the link owns the user-facing operating mode

**Context.** `docs/venc-mode-matrix.md` §16 defines nine user-facing operating
modes (three fps × three range bands) over the IMX335. The user picks latency
and range in the hub menu and never sees a frame size, MCS rung or fps number;
everything else is derived. The open question was *where the mode lives* — the
operator's ruling: *"the call for each mode should be api reachable from
waybeam-link … so user can change mode from the waybeam_hub menu but link still
owns the mode setting."*

**Ruling (operator 2026-07-25).** The link is the control authority for the
operating mode. A new `POST /api/v1/mode {name}` is the single entry point the
hub calls; `GET /api/v1/mode` returns the active label. The link owns the label
(persisted as `venc.active_mode`, restored at boot).

**Why it is the one non-MUT_LIVE write.** A mode bundles three venc fields
(`sensor.mode`, `video0.size` — both `restart_required` — and `video0.fps`)
with the §9.7 range pin. Sensor mode and resolution cannot be applied in-loop,
so the link does not try: it forks a **detached** on-craft applier
(`venc.mode_apply_cmd`) that persists both configs and restarts venc. Two
properties made this safe to add to the flight binary:

- **The range pin is applied *live*** by the applier through the existing
  `POST /api/v1/link/profile`, so the **link and CSA never restart** — no
  §15.5a re-pair (issue B9), only a brief video drop from the venc restart.
  This is why the mode change is not a link restart despite touching
  restart_required venc fields.
- **The fork is injection-proof and non-blocking.** The name is charset-limited
  to `[A-Za-z0-9._-]` and passed as **argv, never a shell**; the applier is
  double-forked + `setsid` so the grandchild reparents to init (no zombie,
  nothing to wait on, flight loop never blocks). Precedent: `RadioAir` already
  forks `execvp("iw", …)` for channel retunes.

**Single source of truth.** The matrix lives only in `profiles/modes/*.json`
(deployed to `/etc/waybeam-link/modes/`). The applier reads the five fields
from `modes/<name>.json`; the link does not duplicate the matrix, it only holds
the active label and forks. So the link stays generic and the nine JSON files
are authoritative.

Spec: §15.5 read + write tables, the MUT_LIVE-exception note, and the
`POST /api/v1/mode` paragraph. Wired as `ControlHandlers::mode_get`/`mode_set`,
`VencCfg::active_mode`/`mode_apply_cmd`, `spawn_mode_applier()`, and
`deploy/modes/apply-mode.sh` + nine wrappers. Applying a mode is the operator's
chosen mechanism: set the JSON fields via `json_cli`, restart venc.

---

### Pass 97 — §9.7 the `min==max` pin snap must re-derive the rung bitrate

**Found on hardware (2026-07-25), running the MCS0 mode test.** With the craft
live-pinned to MCS0 (`POST /api/v1/link/profile {min:0,max:0}`), the ground
delivered **1.2 fps at 98.4 % unrecoverable** — at −12 dBm, so not RF. Root
cause: the craft kept commanding **10303 kbps into a 2829 kbps MCS0 link**, a
3.6× oversubscription; frames could not clear the airtime and missed deadline.

`Selector::evaluate()`'s §9.7 PINNED branch (`core/src/selector.cpp`) set
`a.commit` (profile/MCS/GI/power) when the pinned rung changed, but never set
`a.bitrate_kbps` / `bitrate_kbps_`. `kBoot` and `start_demote` both re-derive
the bitrate with the commit; the live-pin snap forgot to. So venc stayed at
whatever rung the selector last derived (here rung 2's 10303) while the MCS
dropped to 0.

**This is distinct from B11.** B11 (Pass 94) was the FEC gap on small frames,
and the same MCS0 test confirmed it is fixed: at MCS0 with the bitrate corrected
to 2829, the craft emitted **34.8 % parity** on k≈3 frames and the ground
delivered **99.8 fps at 0.20 % unrecoverable** — versus the 3.34 % B11 baseline.
Two separate MCS0 failures with one symptom ("MCS0 is unusable"); Pass 94 fixed
the FEC one, Pass 97 fixes the bitrate one.

**Scope of the bug.** The band-pinned matrix modes (`min < max`, e.g. 0–2) do
**not** hit this branch — they adapt within the band via demote/promote, which
re-derive correctly. The §9.8 fail-safe descent likewise uses `start_demote`.
Only a live `min==max` pin to a rung other than the current one was affected:
bench pins, and any future single-rung "lock" mode.

**Fix.** The PINNED snap re-derives `clamp_bitrate_kbps(derive_bitrate_kbps(p))`
alongside the commit, direction-agnostic (covers a pin up as well). Regression:
`selector_test` now asserts the pin emits the target rung's bitrate, down and up.

Spec: §9.7 pin-scope paragraph. No table change — `table_version` unaffected,
so this is a binary-only (craft TX) redeploy, not a lockstep one.

---

### Pass 98 — §14.1 minimum repair floor `min_r` (small-frame burst protection)

**Motivation (operator, 2026-07-25, out of the MCS0 test).** MCS0 works after
Pass 94 + 97 but is choppier than large-frame operating points. Root cause is
not more packet loss (MCS0 is the *most* robust modulation) but that small
frames convert each lost packet into a lost *frame* far more often: at the seed
`p_rate` 200 ‰, `r = ceil(k·0.2)` yields **r = 1 for every k ≤ 4**, so a small
P-frame is a single loss from death, and `ceil(1·rate) = 1` for any rate ≤ 1000,
so a bumped rate cannot help k = 1 at all.

**Ruling (operator).** Add a minimum repair floor, and treat it as a general
burst-protection win, not an MCS0 patch. `r = max(ceil(k·rate), min_r)`, seed
`min_r = 2`.

**Why a floor rather than a rate bump.** Both were on the table. A global
`p_rate` bump costs airtime at *every* rung — including the high rungs where
frames are large and loss is already ~0, wasting video bitrate — and still
cannot reach k = 1. The floor adds symbols *only* to small frames (which are
small in absolute bytes), never lowers the rate-derived count on large frames,
and is the only lever for k = 1. So the floor strictly dominates a rate bump
for this problem, and `p_rate` stays 200 ‰.

**Measured (offline, real `FrameFramer`/`FrameReassembler`, 2 % loss).**

| frame | k | r=ceil (was) | with min_r=2 | unrec was → now |
|---|---|---|---|---|
| ≤700 B | 1 | 1 | 2 | ~1.9 % → 0.005 % |
| 1500 B | 2 | 1 | 2 | 0.128 % → 0.000 % |
| 2800 B | 3 | 1 | 2 | 0.233 % → 0.015 % |
| 5600 B | 5 | 1 | 2 | 0.595 % → 0.025 % |
| ~28 KB | 20 | 4 | 4 | unchanged (ceil dominates) |

**Bounds and interactions.** The floor applies only after the §14.1 `min_k`
ARQ-only gate (Pass 94) has passed and only when the class rate is non-zero, so
it never resurrects an ARQ-covered small frame nor forces FEC onto a disabled
class. It sits under the GF(256) `k + r ≤ 256` cap. `min_r = 0` restores the
pure rate formula.

**Not a table change.** `min_r` is per-stream FEC config (`streams[].fec.min_r`,
live via `POST /api/v1/fec`), not the §9.3 profile table — `table_version`
unaffected, craft-only (TX) binary redeploy.

Spec: §14.1 policy bullets, the config example, and the `/api/v1/fec` row.
Wired through `FrameFecConfig::min_r`, `set_fec_rates`, `StreamFecCfg::min_r`,
and the control-plane `fec` handler (fifth arg).

---

## Pass 99 — §9.11/§11.7 fps behaviour is a mode property; craft-local ladder toggle (2026-07-25)

**Context.** PR #53 makes fps a user-facing mode axis (§16,
`docs/venc-mode-matrix.md`): nine static-fps modes (recordable, CFR) plus one
variable-fps mode (§16.6 — 1280×720, fps free 30–100, MCS band 0–5, *"maximum
range, no recording"*). The variable mode **is** the §9.11 ladder running; the
nine static modes are the ladder held still. Operator ruling (2026-07-25):
*"this is now just down to a mode policy selection … all the modes have static
FPS selections, except the special VARIABLE FPS mode"*, and *"I agree with
(2a)"* — the link stays the **sole owner** of fps behaviour; mode-select pokes
it **locally**, no ground round-trip.

**Problem.** Two gaps blocked shipping the variable mode as a *switchable* mode:

1. **Construct-gating.** §9.11 instantiated the ladder object only when
   `venc.fps_ladder.enabled` was true at boot, and `FPS_LADDER on` (§11.7
   `0x03`) is documented to *toggle a running loop, it cannot conjure one* — so a
   craft that booted static could never become variable without a **link
   restart**, and a link restart drops CSA (B9 re-pair). Switching modes must
   never restart the link.
2. **No craft-local lever.** The only on/off path was the §11.7 VEHICLE_CMD
   campaign, which is issuer→craft over the air. The Pass 96 mode mechanism is
   craft-local (hub → `POST /api/v1/mode` → forked applier). There was no
   craft-local way for the applier to set the ladder state.

**Ruling (operator, option (2a)).**

- **Decouple construct from run.** The ladder object is instantiated whenever
  `venc.enabled` (it is a cheap POD controller). `venc.fps_ladder.enabled` now
  sets only the **boot run-state** (`TxCore::cmd_fps_enabled_`), not whether the
  object exists. `FPS_LADDER on`/`off` therefore works in **both** directions on
  any venc craft, with no link restart and no B9. "Configured" in the §11.7
  `0x03` row now means `venc.enabled`, not `fps_ladder.enabled`.
- **Add a craft-local toggle.** New `POST /api/v1/link/fps {"ladder": bool}`
  (§15.5, TX/craft only, null hook → 409). It routes through the *same* §11.7
  `apply_command(FPS_LADDER, …)` transition as the over-air path, so local and
  remote are identical (same `resume()` settle semantics, same select-clear).
  **MUT_LIVE** — no restart.
- **fps behaviour becomes a mode field.** Each mode JSON gains
  `link.policy.fps_mode` (`"static"` default | `"variable"`). `apply-mode.sh`
  reads it: static → `POST … {"ladder":false}` **before** the venc restart that
  pins `video0.fps`; variable → `POST … {"ladder":true}` **after** the venc
  restart. The ladder span (`min`/`preferred`) stays a fleet constant in
  `craft.json` (`venc.fps_ladder`, seed `min 30`, `preferred 100`) — per-mode
  spans are a construct-time parameter and are deferred.

**Why the boot run-state still lives in config.** After a reboot the link must
reproduce the persisted mode without the applier re-running. `apply-mode.sh`
persists `venc.fps_ladder.enabled` alongside the venc fields, so a variable-mode
craft boots variable and a static-mode craft boots static; the construct-always
change only makes the *runtime* transition restart-free.

**Ordering matters in the applier.** Static path turns the ladder **off first**
so the loop stops commanding `video0.fps`, *then* restarts venc at the pinned
fps — otherwise a still-running ladder would fight the restart. Variable path
restarts venc (to seed `preferred`) *then* turns the ladder **on**, so the
ladder resumes from a known rung with cleared evidence.

**Recording caveat (design doc §16.6).** The variable mode is VFR — it breaks
CFR muxers, and with `resilience=range` the GDR intra-refresh period is
frame-indexed, so a moving fps stretches/compresses the refresh interval. The
variable mode is **live-view only**; a time-based (not frame-indexed) refresh
for it is noted as follow-up.

**Not a table change.** Everything here is per-craft link config + a live
control toggle. `table_version` unaffected; **craft-only (TX) binary redeploy**
— ground is untouched.

Spec: §9.11 (instantiate-vs-run split), §11.7 `0x03` row ("Configured" =
`venc.enabled`), §15.5 (`POST /api/v1/link/fps` row + MUT_LIVE note). Wired
through `TxCore::apply_command` (unchanged transition), the always-construct
change in the `TxCore` ctor + `cmd_fps_enabled_` boot-init, `ControlHandlers::
link_fps`, `apply-mode.sh`, and `link.policy.fps_mode` in the mode JSONs.

---

## Pass 100 — §9.7 range re-pin clamps into [min,max] immediately (2026-07-25)

**Context.** Verifying the Pass 99 mode workflow on the craft, switching from a
high-band mode (`imx335-30fps-lowrange`, band 2-5, sitting at MCS5) to a
low-band mode (`imx335-100fps-highrange`, band 0-2) left the craft at **MCS5 for
30 s+** on the pristine bench link. The applier persisted `max_profile: 2` and
applied the live pin, and promotion was correctly capped at `max` — but the
selector never demoted DOWN to the new ceiling because a clean link produces no
loss/§9.8 demote trigger.

**Spec gap.** §9.7 states a `min==max` pin **snaps** the operating point in
either direction, but is **silent on the range case** (`min < max`) when a
runtime re-pin lowers `max` below the current rung. `evaluate()` handled
`lo == hi` (snap) and then fell through to the adaptation rules, which only
demote on a trigger. So the envelope's `max` was a soft ceiling on the way down.

**Ruling (operator).** Extend the §9.7 snap to the range case: a runtime re-pin
whose new envelope **excludes** the current rung clamps the operating point INTO
`[min, max]` on the next `evaluate()`.
- **Down-clamp** (current `> max`): unconditional — it is a demote, always safe,
  and is the case that bit. Commits MCS **and** bitrate together (the Pass 97
  discipline), so no oversubscription.
- **Up-clamp** (current `< min`): a promotion, so it **defers to §9.8 while
  feedback is stale** — a raised `min` must not pull the rung UP on a lost link
  ("never fail optimistic"; §9.8/Pass 84 deliberately keeps `floor_profile`
  below `min_profile` as the safety floor). It fires only with fresh feedback.

**Why immediate matters.** "High range" is a user picking robustness *because
they are about to need it* (flying far). Waiting for the first loss to demote
defeats the point — by then the high MCS may already be a black screen at range.

**Mechanics.** New block in `Selector::evaluate()` right after the `lo == hi`
PINNED branch, before the §9.8 failsafe check. Direct snap (`rung_ = target`,
`last_change_ms_ = now_ms`, commit + bitrate, `state_ = "REPIN"`, return),
mirroring the PINNED branch — the pin overrides adaptation, no flap bookkeeping.
`evaluate()` runs only in the `kIdle` phase, so there is never an in-flight
transition to race.

**Not a table change, not Pass 99.** Pure §9/§9.7 selector logic in `core/`;
`table_version` unaffected, craft-only (TX) redeploy. Independent of the fps
ladder — it just happened to surface while exercising the mode workflow.

Spec: §9.7 (new range-clamp bullet). Wired through `Selector::evaluate()`; test
in `selector_test` (down-clamp snaps; up-clamp gated on feedback).

## Pass 101 — §15.3 `rx_dead`: dead RX adapter distinguished from a quiet one (2026-07-25)

**Context.** Pre-flight audit item **B3** (`docs/preflight-open-issues.md`): a
dead RX adapter is silent. The devourer `RadioAir` per-adapter RX thread catches
a USB exception, prints one stderr line, and exits — after which `rx_frames`
merely stops advancing, which is **indistinguishable from a quiet channel**. With
diversity this stays hidden until the *second* ear dies. The §6.5 `adapter_stalled`
watchdog fires on the zero-frame heuristic, but it too cannot tell a dead ear from
a quiet one.

**Ruling (operator).** Add a per-adapter §15.3 boolean `rx_dead` — a *definitive*
RX-loop-terminated signal — alongside `adapter_stalled`. (The two sibling B3/E
counters raised in the same batch — a §11.4 MAC-reject count and a TSF-clamp count
— were **declined**; only this flag was approved.)

**Semantics.**
- `rx_dead = true` only when a backend *knows* an RX loop has terminated. The
  `RadioAir` RX thread sets a per-adapter atomic in its exception handler; the
  §15.3 emit reads it. This is the ground-diversity backend, which is exactly
  where B3's "second adapter dies" scenario lives.
- Kernel-monitor (`MonAir`) has no thread-exit death path — its recv loop
  persists (now throttled on hard errors, B1/B3 Tier-1) — so it leaves `rx_dead`
  `false` and relies on the §6.5 stall watchdog.
- Observability **only**: the §6.5 phantom-diversity exclusion still keys on the
  stall verdict, not on `rx_dead`. Nothing in the selector or RX fast path reads
  it; it exists for the OSD/hub to surface a hard failure.

**Placement.** Emitted between `adapter_stalled` and `tx_wedged` in the
per-adapter object — grouped with the other liveness verdict. Golden schema
(`stats_test`) and the §15.3 sample updated in lockstep.

Spec: §6.5 (heuristic-vs-definitive paragraph), §15.3 (sample + field). Wired:
`AdapterStats.rx_dead` → `stats.cpp` emit; `RadioAir::Impl::Adapter` atomic set in
the RX-thread catch → `AdapterCounters.rx_dead` → `main.cpp` radio mapping.

## Pass 102 — §9.8 fail-safe floors at `min_profile`, not below it (2026-07-25, supersedes Pass 84)

**Context.** Pre-flight audit item **A3** (`docs/preflight-open-issues.md`) asked
whether a §9.7 `min==max` pin should yield to the §9.8 fail-safe. Working it with
the operator reframed the question around the **operating-mode harness** landed in
PR #53 (`docs/venc-mode-matrix.md` §16): the user-facing **Range** axis is exactly
`select.min_profile`/`max_profile` — High = MCS 0-2, Medium = 1-4, Low = 2-5 (the
nine `profiles/modes/*.json`) — and each mode's resolution + fps are co-designed so
the §16.1 bpp floor is cleared **at the band's lowest rung and no lower**.

**The defect this exposed.** `floor_profile` is a table-global constant (`0` in
`table.example.json`), so **every** mode's §9.8 fail-safe descends to MCS0 (Pass
84: descent target is `floor_profile`, *unclamped* by `min_profile`). A Range-Low
mode (band 2-5, co-designed at e.g. 1920×1080/100 fps) fading to MCS0 blows its
bpp floor — a blocked-up screen — and the deployed `min 1 / max 5` craft gets
dumped to MCS0 on any lost-feedback fade. The operator's words: *"we don't want
the mode mechanics fighting each other."*

**Ruling (operator).** The §9.8 fail-safe descends no lower than
**`max(min_profile, floor_profile)`**. In the mode harness `min_profile` is the
band's **verified operating floor**, not an airtime-efficiency knob, so pushing
below it lands on a rung the mode never verified. `floor_profile` remains the
table's **absolute** floor and still binds when it sits *above* `min_profile`
(`max(...)`), but it can no longer drag the fail-safe *below* the mode band.

**This supersedes Pass 84**, and deliberately so — it reverses that ruling's
*premise*, not its values. Pass 84 read `min_profile` as a pure airtime envelope
and drove the fail-safe past it to keep MCS0 (the most robust rung) available on
the worst-case path. B11 + the mode harness make `min_profile` a co-designed
floor, so below it is no longer "more robust" — it is a bpp violation. Still
"never fail optimistic": the fail-safe only ever degrades on lost feedback, now to
the mode's verified floor instead of past it.

**The `min==max` pin falls out, no special case.** A pin's band floor *is* the pin
(`min == max`), so `max(min_profile, floor_profile)` never sits below it — the pin
freezes adaptation outright, `FAILSAFE` included, exactly as PR #53 (Pass 97)
leaves it (the PINNED branch still returns before the fail-safe and re-derives the
rung's §9.5 bitrate on snap). So A3's original "should the pin yield?" question
dissolves: the general rule holds it. **The earlier A3 direction (pin yields to a
sub-pin fail-safe) is withdrawn** in favour of this.

**Mechanics.** One line in `Selector::evaluate()`: the §9.8 descent floor
`floor_rung()` → `std::max(lo, floor_rung())`, where `lo = clamp_rung(0)` is
already the `min_profile` rung. No table change, craft-only (TX) redeploy, no
`table_version` bump. Independent of the fps ladder.

Spec: §9.8 (rewritten descent-floor section + table). Wired through
`Selector::evaluate()`; `selector_test` updated (`min 1/max 5` now floors at MCS1,
new Range-Low band-floor case) — the min==max pin tests are unchanged and still
pass.

## Pass 103 — §15.5 `POST /api/v1/venc/reassert`: mode-switch bitrate stranding (2026-07-25)

**Symptom (operator, on hardware).** Applying `imx335-60fps-highrange` by hand
left venc at a near-zero bitrate — heavy pixelation — even though the link
reported a healthy commanded bitrate. The craft baseline was already a Range-High
`0-2` mode and the applied mode is also `0-2`, so the pin never changed.

**Root cause.** The §9.6 venc actuator drives bitrate over the **volatile**
`/api/v1/live/set` path (Pass 73) with **write-on-change**: a setter is a no-op
when the target equals the last *successfully pushed* value (`last_`). A mode
apply restarts venc (`sensor.mode`/`video0.size` are `restart_required`). The
fresh encoder boots at its **persisted** `video0.bitrate` — the live value died
with the old process — but the actuator's `last_` still holds the pre-restart
value, so it believes the encoder is already correct and **never re-pushes**. The
encoder is stranded at whatever stale/low bitrate is on flash. Same for
`last_caps_`/`last_fps_`. It bites hardest on a **same-band switch** (only
fps/resolution change, pin and derived bitrate unchanged → nothing the actuator
thinks is new), which is exactly the operator's case. Both entry paths were
affected — the hand-run applier *and* `POST /api/v1/mode` (which forks the same
applier and did not invalidate the actuator).

**Ruling.** A venc restart is a cache-invalidation event for the link's actuator.
The §16 applier's **final step** (after the venc restart) POSTs a new §15.5
endpoint `POST /api/v1/venc/reassert`, which drops the actuator's write-on-change
cache (`last_`/`last_caps_`/`last_fps_`) and re-probes the volatile path, so the
next tick re-asserts bitrate + frame-caps + fps onto the fresh encoder. The
endpoint is MUT_LIVE (it only clears in-loop cache; the re-push rides the normal
per-tick reconcile, which already tolerates the encoder 404ing during bring-up,
Pass 73). `POST /api/v1/mode` is affirmed as the **one supported** mode-apply
entry; the on-craft applier is what it forks and self-reasserts, so a hand-run
(bench-only) heals too.

**Mechanics.** `VencActuator::invalidate()` (io) resets the three `last_*`,
clears `live_fallback_`/reprobe, and clears the retry holdoff. `tx.reassert_venc()`
+ `h.venc_reassert` + a `control_server` route expose it; `apply-mode.sh` gains
the notify curl as its last step plus a bench-only banner. Pure add otherwise —
no `table_version` bump, craft-only (TX) redeploy.

Spec: §15.5 (new Write-table row + the §16 applier paragraph gains the
stranding-gap explanation). Wired through `VencActuator::invalidate()` +
`venc_actuator_test`; verified on the fleet with the new mode harness
(`tools/mode_harness.py`) red→green.

## Pass 104 — §15.5 `GET /api/v1/modes`: the operating-mode catalog (2026-07-25)

**Context.** Wiring the waybeam-hub mode menu onto the link's control plane, the
first gap: the link exposes `POST /api/v1/mode` (apply) and `GET /api/v1/mode`
(the *active* label) but **no way to enumerate the selectable modes**. `GET
/api/v1/modes` returned `unknown path`. The 10 modes exist only as
`modes/imx335-*.json` files on the craft, so a menu had to either read those
files itself or ship a hardcoded catalog — both drift from the craft the moment
a mode is added or its parameters change.

**Ruling (operator).** Add `GET /api/v1/modes` to the link and keep the link the
**single source of truth** for which modes exist and their parameters; the hub
renders, it does not catalog. Raised as an explicit gap before implementing (the
two rejected alternatives — hub-side catalog, or the hub reading craft files
directly — both re-introduce the drift the mode matrix was built to avoid).

**Shape.** `{active, apply_configured, modes:[{name, fps, resolution, mcs_min,
mcs_max, fps_mode}]}`. Per-mode facts come straight from each mode file
(`.venc.video0.fps`/`.size`, `.link.policy.select.min_profile`/`max_profile`,
`.link.policy.fps_mode`), so the wire is the raw latency/range/resolution facts,
**not** UI vocabulary — the caller maps `mcs_min`/`mcs_max` to any "High/Med/Low"
label and lays out the latency×range grid. `resolution` is the derived §16
resolution (`video0.size`). Name-sorted; malformed/partial files skipped, never
fatal.

**Where the modes live.** New `venc.modes_dir`. When unset it defaults to the
directory containing `mode_apply_cmd` (the §16 layout co-locates the applier and
the mode files), so an already-deployed craft serves the catalog with no config
change. Off a TX node the endpoint **409**s (like `/api/v1/mode`); on a craft
with no readable dir it returns an empty `modes[]` — a misconfigured craft is
distinguishable from a wrong-node 409.

**Mechanics.** New io helper `modes_catalog_json(dir, active, apply_configured)`
(POSIX `dirent` enumerate + nlohmann parse — no `std::filesystem`, to stay clean
on the ARMv7 cross toolchain); `h.modes_list` hook + a `control_server` route
mirroring `/api/v1/mode`; wired on both the TX and loopback control paths. Pure
add — no `table_version` bump, read-only, craft-only (TX) surface. New unit
`modes_test` (valid + partial + non-JSON dir entries → catalog).

Spec: §15.5 (new Read-table row + a catalog paragraph after the `POST
/api/v1/mode` discussion). First step of the hub mode-menu integration; the
ground→craft transport (craft-local hub proxy) is a separate, later piece.

## Pass 105 — §11.7 `0x07` MODE: over-air operating-mode select (2026-07-25)

**Context.** The hub mode menu (Pass 104 gave it the catalog) needs a way for the
**ground** to actually apply a mode on a bound craft. `POST /api/v1/mode` (§15.5)
exists but is loopback/LAN-local to the craft — issuing it from the ground would
mean an unauthenticated HTTP POST to the craft's network-facing hub `:8060`, a
mode-change hole anyone on the RF-adjacent LAN could drive. The operator ruling:
the apply must ride the **PSK-guarded RF uplink** like every other vehicle
command (§11.7), not HTTP.

**The gap that had to be raised (not inferred).** The natural encoding — `cmd_arg`
= index into the name-sorted §15.5 catalog — collides with §3.14's hard
`cmd_arg` `0..4` structural cap (Pass 68: "no free-form numerics"), enforced in
the wire codec on **both encode and decode** (`>4` = structural decode error,
silent drop). The catalog already has **10** `imx335-*.json` modes, so an index
of 5..9 cannot ride the wire at all. "Index the catalog, no wire change" is not
achievable for 10 modes. Raised to the operator with three options: (A) widen the
wire for MODE, (B) a ≤5 `command_presets.mode` subset (v2 pattern, zero wire
change, caps at 5), (C) curate the catalog down to ≤5.

**Ruling (operator).** **Widen the wire for MODE (option A).** `cmd_arg` is
physically a `u8`; the byte layout is unchanged, but the structural gate becomes
`cmd_id`-dependent: `0x07` MODE decodes/encodes the full `0..255`, while
`0x01`–`0x06` (and unknown ids) keep the `0..4` cap and the `>4`-is-a-decode-error
rule. This overturns the Pass 68 ≤5-choice bound **for MODE only** — justified
because a mode is still *one* enum choice, just over an inherently open-ended set
that is already enumerable and single-sourced on the craft (`GET /api/v1/modes`).
The two other roads were rejected: the preset subset (B) would make half the
modes unreachable and re-introduce a config list the ground must learn separately
from the catalog it already reads; catalog curation (C) throws away modes.

**Semantics.** `0x07 MODE`, `cmd_arg` = index into the **name-sorted** catalog
(the exact `GET /api/v1/modes` order). The craft resolves the index against the
*same* enumeration+sort `modes_catalog_json` is built from, so ground and craft
agree on which ordinal is which mode with no mode identity crossing the air —
only its position. Apply forks the §16 applier (`venc.mode_apply_cmd`), the
identical path `POST /api/v1/mode` takes (so the Pass 103 self-reassert heals the
venc restart either way). `REJECTED` when the craft has no `mode_apply_cmd` (not
a mode-actuating node) or `arg` ≥ the catalog length (index past the end — a
range error, consumed + echoed, not a structural drop). Craft-session volatile
like all §11.7 state (reboot restores the boot `active_mode`); a mode restarts
venc and re-bands the §9.7 selector envelope, so it is a **pre-flight** action —
never a channel or power change (the §13 bound holds).

**Mechanics.** `vcmd_id::kMode = 0x07` (core). The `cmd_arg > kVcmdMaxArg` gate is
relaxed to `cmd_id != kMode && cmd_arg > kVcmdMaxArg` in the three enforcement
sites — `wire.cpp` decode + encode, and `VcmdIssuer::start` (core). New io helper
`mode_name_at(dir, index)` shares the enumeration+filter+name-sort with
`modes_catalog_json` (refactored a common `collect_modes(dir)`), so the craft's
index→name map is byte-for-byte the catalog's ordering. `app/main.cpp`: the
`apply_cmd` lambda intercepts `kMode` (catalog lookup → `spawn_mode_applier` →
optimistic `active_mode` update), `vcmd_id_for`/`vcmd_name_for` gain `"mode"`,
and the ground `POST /api/v1/vehicle/command` route allows `arg` `0..255` for
`cmd:"mode"` (the ground can't range-check locally — the catalog is on the craft
— so an over-range index is the craft's `REJECTED` to give). Tests: `wire_test`
(MODE wide-arg round-trip; non-MODE `>4` still a decode error), `vehicle_cmd_test`
(issuer accepts a wide MODE arg; craft applies/REJECTs by range), `modes_test`
(`mode_name_at` ordering matches the catalog). Craft-only (TX) + ground-issuer
change; no `table_version` bump (VEHICLE_CMD is not a §7 table).

Spec: §3.14 (`cmd_arg` row — the `cmd_id`-dependent structural gate), §11.7
(intro caveat, the `0x07 MODE` registry row, the "MODE takes the other road"
paragraph), §13 (the forged-VEHICLE_CMD row extended to mode switches). Second
step of the hub mode-menu integration; the hub-side proxy + WebUI menu follow.

## Pass 106 — §3.9 latch-triggered decoder recovery (2026-07-26)

**Context.** A fresh cold start (craft + ground, both power-cycled) produced
audio but no video. Every counter read healthy: the stream latched, the frame
reassembler delivered, the `venc_frame_out` egress ring advanced. Manually
writing `video0.bitrate=1000` restored the picture instantly. The reason it
worked is that a *value-changing* live bitrate write rebinds the SigmaStar rate
controller, which emits an IRAP as a side effect — the operator had accidentally
requested a keyframe.

**Root cause — two independent faults, one symptom.** First, the consumer:
waybeam-hub's frame-SHM ingress hard-gates on `VFRM_FLAG_IDR` before it will
push a single access unit into its decoder ("drop deltas until the first IDR so
we never seed a decoder mid-GOP"). Under the §16 matrix modes, which all run
`resilience=range`, that flag is **never set on any frame**, so the gate never
opens and the decoder is starved — this was never a decoder-conformance problem,
the decoders never received a byte. Second, the trigger: the hub already POSTs
`/api/v1/video/recover` (§15.5), but only once, at ring **attach**. The ground
link creates the egress ring at boot, before anything is latched, so that POST
lands on `"no matching latched RTP stream"` and is never retried. Both faults
had to be present; either alone is survivable.

**Why the existing §3.9 wording did not cover it.** §3.9 already anticipated
exactly this stream shape ("a GDR stream can carry VPS/SPS/PPS indefinitely
without an IRAP picture"), but scoped emission to "a local decoder is newly
attached or reset" — a condition the link **cannot observe**. That is the same
wall Pass 22 hit: VFRM v1 has no consumer generation, and a consumer draining
the ring with its display pipeline down looks identical to one that is decoding.
The trigger was specified against a signal that does not exist at this layer, so
in practice nothing ever fired it automatically.

**Ruling (operator, 2026-07-26).** Add **fresh latch** as a §3.9 emission
trigger, defaulting **on**. A first latch is the one bootstrap-relevant moment
the link genuinely can detect, and on a GDR craft it is the only moment at which
an IRAP is guaranteed absent from everything the consumer will ever see. Placed
on the link rather than the hub deliberately: the link is the only component
that knows when a stream latched, which is precisely what the hub's attach-time
POST was racing. Default-on was ruled explicitly — the failure mode is silent
(healthy counters, black screen), and the cost is one 18-byte return per latched
stream against a TX gate that already rate-limits to one actuation per second.

**Bounded, because the stop condition is unobservable.** §3.9's existing
"repeated … if decoder output has not resumed" is unimplementable for the same
reason the original trigger was. The repeat is therefore capped at 5 attempts at
1 Hz, with one early exit the link *can* observe: on frame-SHM egress, an
IRAP-flagged frame written to the egress ring means the consumer now has
something to start from. On RTP egress the link does not parse the payload, so
the attempt bound is the only stop. Without the bound, a craft with
`venc.recovery_enabled` false would draw a return every second for the whole
flight for a request it will never honour.

**Scope note — this does not make the hub gate correct.** The link-side trigger
fixes the cold-boot race, but a node with no uplink cannot use it at all: a §2
passive spectator (Pass 74) emits no returns by construction, so for a spectator
joining a GDR stream an IRAP will never arrive on request. The consumer-side
bounded gate bypass in waybeam-hub is therefore not redundant with this pass —
it is the only mechanism that class of node has. Both land together.

**Mechanics.** New `node.recovery_on_latch` (default true, RX). `RxNode` tracks
latched RTP `StreamKey`s and drives a small per-stream attempt schedule from the
rx loop, reusing the existing `request_recovery` encode + designated-return
inject path (so quiet-gap scheduling and the spectator no-op both come for
free). No wire change, no `table_version` bump, no new §15.3 field — emissions
log to stderr like the TX-side actuation does, keeping the stats golden file
untouched.

Spec: §3.9 (fresh-latch trigger paragraph + the bounded-repeat paragraph
replacing the unobservable stop condition), §15.2 (`node.recovery_on_latch` and
its independence from the craft-side `venc.recovery_enabled` permission).

### Pass 106 addendum — bench verification (2026-07-26)

Deployed to the live fleet (craft .232, x86 ground .242, RK3566 spectator .199)
and measured. Three things the desk analysis had right, and one it had wrong.

**The stream, measured.** A passive census of `/dev/shm/venc_frame_out` over
120 s (11 996 frames) on `imx335-100fps-highrange`: **zero** `VFRM_FLAG_IDR`
frames and **zero** IRAP NALs (16–23). `VFRM_FLAG_GDR` on 100 % of frames,
`gdr_len=18` (~180 ms refresh at 100 fps), 20 % also `ENHANCE`. VPS/SPS/PPS
repeat about every 2 s. **No SEI NALs at all**, so no `recovery_point` SEI —
the clean "decoder honours the recovery point and starts mid-GDR by itself"
path does not exist on this encoder. A consumer must be seeded on an IRAP or
be forced to start dirty. This is Pass 22's observation, now quantified.

**`/request/idr` works; §3.9 end-to-end is ~50 ms.** Ground
`POST /api/v1/video/recover` → RF → craft → venc → IDR visible in the ground's
egress ring measured **41–62 ms, mean 49 ms, 6/6** — one `flags=0x01` frame
carrying NAL 19 `IDR_W_RADL` with `gdr_len=0`. The recovery mechanism was never
broken; nothing was pulling it.

**What the desk analysis got wrong: the §3.9 rate gate creates a consumer dead
zone.** The one-actuation-per-second limit is correct as a defence against
forged floods, but it interacts badly with a consumer that re-arms its own
start-point gate. The link emits its latch request at t=0 and the IDR lands at
t≈50 ms — *before* the hub has finished building its pipeline and attached.
Attach skips to the ring's live edge (deliberately: draining a backlog into an
appsink with `drop=TRUE` discards the seed frame anyway), so that IDR is
stepped over. The consumer's own request then arrives inside the craft's 1 s
window and is suppressed — `request_idr()` returns true without queueing — so
the next obtainable IDR is ~1 s away. Any consumer-side timeout below one
second therefore fires in the gap between the IDR it threw away and the one it
is not yet allowed to ask for. The hub's bypass was retuned 500 → 1500 ms.
Nothing in §3.9 changes: the rate gate is right, and the lesson is that the
*consumer's* patience must exceed it. Recorded here because the next component
to consume a §3.9-backed stream will hit exactly this.

**Verified.** x86 ground cold start: 1 frame gated, 0 bypasses — seeded on a
real IDR. RK3566 spectator: 151 frames gated (1.5 s), 1 bypass, 3 recover POSTs
that cannot be answered because a spectator emits no returns (§2 Pass 74) — the
node class for which the consumer-side bypass is the only mechanism, confirmed
black-forever without it. Spectator emitted **zero** latch requests, per the
construction-time gate. No regressions: both grounds' adapters, FEC, ARQ and
audio nominal; craft `GET /api/v1/mode(s)` 200; RX-node `/api/v1/mode(s)` 409
per Pass 104.

**Not deployed to the craft.** Pass 106 is RX-side and a no-op for TX, and the
craft overlay has ~1.5 MB free against a 2.6 MB binary — the swap is a brick
risk with no behavioural gain. The craft stays on its current build.

## Pass 107 — §15.3 frame-SHM ingress backpressure is not observable (2026-07-26)

**The defect.** `StreamStats::shm_full_drops` was filled unconditionally from
`FrameShmRing::Stats::full_drops`, but that counter is only ever incremented in
`write_frame()` — the **producer** path. On a `tx` node the frame-SHM binding is
an *ingress* ring that we attach to as a consumer, so the field was structurally
incapable of being non-zero. It nevertheless shipped in every stats line and on
the `:8099` dashboard, named exactly as though it monitored the condition an
operator most wants to see on the vehicle: venc dropping whole frames because
our drain is too slow. Same for `shm_oversize_drops`.

This is worse than a missing metric. A hardcoded zero under a plausible name is
read as evidence, and the venc producer's drops break the H.265 reference chain
— the failure it appeared to rule out is precisely the one that was invisible.

**Why it cannot be fixed by reading harder.** venc_frame_ring is SPSC and its
`full_drops`/`oversize_drops` live in the producer's process-local
`venc_frame_ring_t.stats`, not in the §15.4 shared header. There is nothing in
the mapping to read. The prose in §15.3 was already careful here — it said only
`shm_bad_slots` comes from the consumer ring on ingress — but it stated it as a
sourcing note rather than as a prohibition, and the code did not follow it.

**Ruling.** §15.3 now carries an explicit per-direction observability table and
the sentence a future reader needs: a reader MUST NOT interpret
`shm_full_drops == 0` on an ingress stream as "the producer is not dropping".
Ingress gets a counter that is real today, with no producer change and no ABI
change: **`shm_ring_full`**, incremented in `read_frame()` whenever the consumer
observes `write_idx - read_idx == slot_count`.

It is deliberately not called a drop count. It is a *leading* indicator — at
that instant the producer's next write is dropped unless our read frees the slot
first — so it can read non-zero while the producer has dropped nothing. That
asymmetry is the honest one to have: it over-warns rather than under-warns, and
under-warning is what this pass exists to stop.

**Alternative considered and deferred:** carve the producer's true `full_drops`
into the 52 free bytes of the venc_frame_ring header's producer-owned cache line
(offsets 76+), which is offset-compatible with every existing consumer. That is
a cross-repo change owned by
`waybeam-coordination/specs/cross/2026-07-26-shm-egress-backpressure/` and
requires a venc release; this pass is the part that needs no coordination. When
that lands, `shm_full_drops` becomes real on ingress and the table in §15.3 is
updated — `shm_ring_full` stays, because a full ring matters whether or not the
producer chose to drop.

**Scope.** Egress is untouched and stays correct: on a `rx` node we create the
ring, so all three producer counters are real there.
## Pass 108 — §15.5 a §2 latch binds the craft; §15.5 mode `catalog_fingerprint` (2026-07-26)

**Context.** A ground-menu audit began from one reported symptom: CHANNEL →
"jump 5180" had *never* worked from the on-screen menu, returning `400 {"error":
"no live CSA key for craft (rebooted? re-scout)"}`. The craft had not rebooted
and a re-scout was not the fix.

**Root cause.** `active_selection` is constructed once, at startup, from
`rx.selected_originator().value_or(0)` — before any packet has arrived, so **0**,
with `selection_state = "configured"`. Its only writer, `apply_selection()`, is
called from exactly three sites, all inside CSA campaign outcome handling
(commit and the two rollbacks). **A §2 latch never touches it.** A ground that
boots, hears its craft, and latches is fully operational for video and telemetry
while remaining `originator: 0` for its entire uptime.

Everything gated on the selection tuple is therefore dead on such a ground:

| surface | gate | result |
|---|---|---|
| `POST /api/v1/csa` | `discovery.token_for(active_selection.originator)` | `token_for(0)` → nullopt → 400 |
| `POST /api/v1/vehicle/command` | `selection_state != "committed"` | 409 "no bound craft" |
| §14.3 cache assignment | `assign_caches()` early-returns on `originator == 0` | cache configured, never follows |

Measured on all three fleet grounds simultaneously, in normal operation:
`{"state":"configured","originator":0,"channel":5805,"caches_configured":1,
"caches_following":0}` — while each one's own `/api/v1/discovery` held the craft
with a fresh cached token (`originator:17, psk_present:true`). The key was
present the whole time; only the originator to look it up by was zero.

The reachable-by-menu blast radius: all seven CHANNEL jumps, six of eight
ADAPTIVE items, and the Pass 105 over-air MODE apply. The only way any of them
worked was scout → quickconnect — tearing a healthy link down and re-forming it
to earn the right to command over a link that was already up.

**Ruling (operator, R1-a).** A §2 latch binds. When the latch picker selects an
originator and the selection is still `configured`, the receiver adopts it and
the state becomes `latched`; `latched` is a valid target for `/csa` and for
§14.3 cache-follow.

**Where the ruling stops, found on hardware.** The first implementation also
admitted `latched` to `POST /api/v1/vehicle/command`. On the bench that returned
`200` and then `timeout`, every time — because §11.7 already rules **"no
bootstrap"**: only an accepted CSA establishes the §11.5a command binding, and a
command from a non-bound issuer is a *silent drop* (echoing to an unbound sender
would be a probe oracle). Existing law, not a new question, and the amendment as
first drafted contradicted it. A latch is exactly enough to *claim* a craft and
to follow it, and deliberately not enough to *command* one; the ground must move
it first, which is what `/csa` and `scout/quickconnect` do. The refusal now names
that remedy instead of issuing into silence.

Consequence for the ground menu this pass came from: CHANNEL is fixed by the
ruling, ADAPTIVE and over-air MODE are not — they need a claim, which is now
reachable because `/csa` works. Making a latched craft commandable would mean
amending §11.7's no-bootstrap clause and changing the craft side; that is a
separate ruling with a real security argument behind the status quo, and is not
taken here.

Considered and rejected: *auto-claim on latch* (mutates the craft's
`claimed_by` for what is a read-mostly link) and *an explicit "bind craft" menu
item* (honest about the state machine, but leaves a manual step in front of
every channel change — the operator interface this audit exists to fix).

**Why this grants nothing new.** The §11.4a MAC key for both campaign kinds is
the craft's *announced* token, ruled public in Pass 63 and cached from every
ANNOUNCE irrespective of selection state — quickconnect keys from that same
cache. Adoption changes which originator is looked up, not what may be looked
up. Ownership is still proven by connecting (§15.5a): a wrong `latched` belief
fails as a §11.6 `timeout` or §11.7 `REJECTED`, because the craft validates the
MAC and its own binding.

**Bounds of the adoption.** One-way and one-shot: `configured` → `latched` only.
A latch never overwrites `claiming`, `committed`, or an existing `latched`
tuple, so it cannot steal a deliberate claim and a §2 re-latch onto a different
craft cannot silently redirect campaigns. The adopted tuple carries the
receiver's own current channel/bw/net_id — a latch observes a craft, it does not
discover a channel; only `/csa` and `scout/quickconnect` move the link.

**Error-message split.** The one conflated string becomes two: `409 no craft
selected` (the tuple names nobody) versus `400 no live CSA key for craft`
(a craft is selected, no announced token cached — secret-mode craft, or one not
heard for >5 s). The old wording sent operators to re-scout a healthy link.

**§15.5 `catalog_fingerprint` (operator ruling, R2-a).** The same audit found
the ground cannot enumerate the mode catalog at all: `GET /api/v1/modes` is
TX-only, and the craft's control plane is loopback-bound, so in flight there is
no IP path to it. A ground menu must therefore hardcode its own copy — and
§11.7 `0x07` MODE addresses that catalog **by index**, so one mode file added or
removed on the craft shifts every later index and the ground silently applies
the wrong mode. (The live catalog sorts `100fps, 30fps, 60fps` — name order, not
human order — which is itself a trap for a hand-written copy.)

`GET /api/v1/modes` therefore gains `catalog_fingerprint`: `"<count>-<hex32>"`,
FNV-1a-32 over the name-sorted mode names joined by `\n`. A ground pins it
beside its hardcoded copy and re-checks whenever it *does* have an IP path.
Deliberately **not** carried over the air: the ground must be able to drive
modes with no IP path at all, so the hardcoded copy stays authoritative in
flight and the fingerprint is a pre-flight check, not a runtime gate. The hash
covers names only, in catalog order — exactly what the index mapping depends on.
Editing a mode file's *contents* keeps the fingerprint stable by design: the
index still resolves to the mode the ground meant.

Considered and rejected: no fingerprint (drift undetectable), and carrying it
over the air (a §3.12/§11.7 wire change, larger than this round warrants).

### Pass 108 addendum — bench verification (2026-07-26)

x86 ground, which had been sitting at `{"state":"configured","originator":0}`
since boot with the craft (originator 17) decoding fine the whole time:

| step | before | after |
|---|---|---|
| latch | `configured` / 0, indefinitely | `latched` / 17, plus `link: latched originator=17 (5805 MHz) — selection bound` |
| `POST /api/v1/csa` | 400 "no live CSA key … re-scout" | **200**, campaign `committed` |
| craft §11.5a binding | `claimed:false` | `claimed:true, claimed_by:9` |
| `POST /api/v1/vehicle/command` | 409 "no bound craft" | **200 → `acked`**, all six ADAPTIVE commands |
| craft-side applied state | — | `cmd_arq`/`cmd_selector_frozen`/`cmd_fps_ladder` set, `cmd_last_nonce` matching the issuer |

**CSA round trip.** 5805 → 5785 → 5805, both legs issued from the freshly
latched ground and both `committed`. Worth recording because it did **not** cost
anything: all three grounds followed via §11.5 and held ~100 fps / 25 Mbps
across both legs — the two non-issuing grounds were never stranded. Their
`link/selection` views differ, and the difference is instructive: the one on a
Pass 108 binary reports `committed 5785`, while the pre-108 one reports
`configured 5805` with `csa_state: COMMITTED` — its RF layer followed, but its
selection tuple could not record it. That is the same originator-0 blindness
this pass removes, seen from the outside.

**`catalog_fingerprint` on the craft.** `10-521a954c` for the shipped ten-mode
catalog. Note the name sort interleaves the fps groups (100, 30, 60) — index 3
is a 30 fps mode, not a 60 fps one — which is exactly the trap a hand-written
ground copy falls into and the fingerprint exists to catch.

**B9 staleness, re-confirmed end to end.** Swapping the craft binary restarts
its link, which resets the §11.5a binding and picks a fresh session, while the
ground keeps believing `committed`. A command issued on that stale belief
returns `200` and then `timeout` — the craft-side binding is authoritative,
exactly as §15.5 says. `POST /api/v1/csa` then re-keys from the craft's *new*
announced token, re-claims (`claimed:true, claimed_by:9`), and the next command
`acked`. No re-scout was needed at any point.

Worth carrying into the ground menu work this pass came from: for the operator,
"claim went stale because the craft rebooted" is indistinguishable from "command
ignored" — both are a `timeout` against a link that looks perfectly healthy. The
menu should show claim state, not just offer claim actions.

## Pass 109 — §15.3/§15.4 producer health is observable, not control (2026-07-26)

**Context.** Pass 107 removed a false ingress `shm_full_drops=0`: the only drop
counter then available lived in this process and incremented only when
waybeam-link produced an egress ring. `waybeam_venc` 0.57.0 now publishes three
producer-owned fields in the previously padded header bytes: `health_magic`
`"VHLT"` at 76, lifetime `full_drops` at 80, and `throttle_permille` at 88.
Header size and version remain 192 and 1.

**Ruling: availability is explicit.** A consumer accepts old producers forever.
Only the exact `VHLT` marker makes health valid; zero or any unknown marker means
"not reported", never "zero drops" or "unclamped". Stream stats gain
`shm_health_valid` and `shm_throttle_permille`. Ingress `shm_full_drops` becomes
the producer counter delta since attach or local stats reset. Reset and producer
replacement establish a fresh baseline; a counter rollback rebases instead of
underflowing.

`shm_ring_full` remains. It is a consumer observation and a leading indicator:
a ring may be seen full before the producer loses a frame, while a producer may
drop between consumer samples. Neither field substitutes for the other.

**Ruling: no control action.** Header health is observability-only in this pass.
It does not enter the selector cascade, local-pressure gauge, bitrate actuator,
FEC policy, or FPS ladder. "Drops/throttle imply an MCS demote" has unresolved
cadence, precedence, settle, pin, CSA-freeze, and flap semantics; implementing
one silently would violate the law. That decision is deliberately deferred.

**Ruling: four-frame bounded drain.** The former one-frame-per-loop rule was the
only remaining frame-count ceiling: if the vehicle loop ran below camera FPS,
reducing encoded bytes could not stop an eight-slot ring filling in frames. TX
now consumes at most four frames from each pending ring per loop. Air service
still runs first, and a ring that exhausts the four-frame budget remains pending
for the next iteration. Four clears half the venc ring in one visit without
turning an arbitrary producer burst into an unbounded vehicle-loop stall.

**Two-hop boundary.** Radeon-vrx and waybeam-hub consume waybeam-link RX's
separate `venc_frame_out`, not venc's vehicle-side `venc_frame`. Health is not
inside `VencFrameMeta` and therefore does not cross the radio. Current
waybeam-link egress leaves `health_magic=0`; ground health publication and
decoder stats are separate work. Their existing attach validators ignore bytes
76–89 and need no compatibility change.

## Pass 110 — classify acute loss; lock recurrently bad MCS rungs (2026-07-26)

**Observed failure.** On 5220 MHz the craft could hold excellent RSSI while a
specific high rung sustained several percent packet loss. Raising
`demote_milli` moved the threshold but did not change the loop: ordinary
reactive loss selected §9.2's highest-probability lower rung—normally the
physics-prior floor—then RSSI-only promotion retried the unsuitable rung.
Continuous moderate loss therefore looked like an emergency floor event and
the selector had no channel-conditioned memory.

**Ruling: loss has two fresh-report classes.** A confidence-qualified raw
100 ms window at 200‰ or above is `LOSS_EMERGENCY` and moves directly to the
resolved safe floor. Moderate loss uses a per-rung leaky score (+1 bad, −1
clean, unchanged below 32 unique packets); score 5 is `LOSS_PERSISTENT` and
moves exactly one rung. The former max-probability multi-rung target is retired.
EWMA remains useful smoothing/observability but cannot distinguish a spike from
persistence. Emergency bypasses settle, pressure, and cooldown; §9.5 bitrate
lead still binds. Persistent remains suppressed under local pressure.

The floor is never the string "MCS0": it is
`max(mode min_profile, table floor_profile)`. This same resolved floor now
binds emergency, persistent, RSSI, and stale-feedback paths.

**Ruling: per-rung lockout.** A loss-driven demote charges the rung where its
evidence was observed. Strikes 1–3 block it for 30 s; strike 4 latches until a
successful `(channel,bw)` change, accepted reporter session/source change, or
restart. Expiry retains strikes. The lowest blocked rung is an upward ceiling;
promotion and pressure escape cannot skip it. The verified floor is never
locked. Pins retain operator precedence and surface a conflict. No lockout
initiates CSA; a latched state is an operator channel recommendation.
Those values are the §17 seeds: when configured, every strike below
`rung_lockout_latch_count` is timed and reaching the count latches. The compact
wire summary saturates a longer timed remainder at its u16 maximum.

**Ruling: environment reset is broad but scoped.** A real RF tuple/source
change clears strikes, lockouts, per-rung loss evidence, and short RF/flap
smoothing. It preserves active profile/bitrate, the configured mode envelope,
anti-replay state, and CSA freeze. A same-channel mode change does not reset.

**Ruling: ground display consumes craft truth.** New 32-byte, 2 Hz-due
`SELECTOR_STATE` (type `0xE`) carries the effective ceiling, timed/latched state,
remaining time, masks, last loss, and reason. A ground accepts it only from its
latched RTP craft/session with a matching table and expires it after 1.5 s.
waybeam-hub renders a timed amber limit or latched red channel recommendation;
it never reconstructs or actuates selector state.

**Two existing spec/code gaps closed.** LINK_REPORT `uniq` is the interval
denominator already specified at §3.5, not the lifetime RX counter. §7.3's
never-implemented partial-window "immediate report" is removed: the 10 Hz
window bounds reaction to 100 ms and preserves the sample-confidence law.

**Addendum — no telemetry TX guard (operator).** The 2 Hz selector summary is
only a due cadence. It is inserted immediately before an already-transmitting
live RTP DATA packet; idle video coalesces the latest state until the next slot.
It never sends standalone, follows an EOB, re-arms the quiet gap, or extends the
craft TX→RX guard. Observability does not buy its own radio turnaround.

## Pass 111 — calibrate the per-rung local service boundary (2026-07-26)

**Observed failure.** With the selector holding MCS5 at excellent RSSI, the
encoder commanded the global 25 Mbps ceiling while the vehicle-side frame-SHM
ring repeatedly filled and the venc throttle oscillated as low as 30–40%.
Moving the whole fleet from the known-dirty 5220 MHz channel to 5805 MHz did not
remove it. The selector and Pass 110 loss classifier were healthy: RF-rung
viability and the local source-to-air service ceiling are separate limits.

**Measurement.** The craft ran the production 1280×720, 100 fps, GOP 2 stream
through frame-SHM, the shipped per-frame RLC overhead, quiet-gap pacer, and
SSC338Q radio backend on HT20/5805. Each candidate waited for venc throttle
recovery, then sampled at 5 Hz for 30 s; MCS5's accepted point also passed a
60 s sample. A clean point held `shm_throttle_permille == 1000`, frame-SHM fill
at no more than one of eight slots (12%), pressure false, and producer
`shm_full_drops` unchanged. Any throttle excursion rejected the point even when
the ring did not reach a full-drop event during the sample.

| MCS | highest clean (kbps) | first rejected (kbps) | rejection evidence |
|---:|---:|---:|---|
| 0 | 4000 | 4500 | throttle 303‰, fill 75%, +1 full drop |
| 1 | 7500 | 8000 | throttle 338‰, fill 100%, +26 full drops |
| 2 | 11500 | 12000 | throttle 608‰, fill 75% |
| 3 | 14500 | 15000 | throttle 640‰, fill 50% |
| 4 | 19000 | 19500 | throttle 384‰, fill 87%, +3 full drops |
| 5 | 23000 | 23500 | throttle 640‰, fill 62% |
| 6 | 24500 | 25000 | throttle 640‰, fill 50% |
| 7 | 26000 | 27000 | throttle 608‰, fill 62% |

**Ruling.** Preserve an existing derived rate when it is already no more than
95% of the clean boundary. Otherwise choose the greatest integer
`airtime_budget_permille` whose §9.5 all-integer derivation does not exceed 95%
of that boundary. The table moves from a flat 600‰ to
`{600,600,600,600,510,463,438,418}` and derives
`{2829,5754,10303,13769,18025,21839,23249,24658}` kbps. The 25 Mbps
`venc.max_bitrate_kbps` remains as the independent encoder-capability ceiling;
it is no longer asked to disguise per-rung transport limits.

This amends, rather than erases, Pass 75. Pass 75 correctly separated the SoC
ceiling from RF profile selection and rejected using one global airtime cut as
a proxy for CPU. The new evidence is per-rung and comes from the complete local
service path: MCS5, MCS6, and MCS7 fail at different rates. The table's
per-rung airtime field is therefore being used for its intended wall-clock
occupancy policy, while the rung-independent encoder ceiling remains intact.

**Boundary.** This is a fleet seed measured on one SSC338Q/RTL88x2 craft at
HT20, 100 fps, and the current framing/FEC/pacer implementation. It is not a
claim about every adapter or channel. Re-calibrate after changing channel
width, driver, framing/FEC, quiet-gap timing, camera cadence, or hardware class.
Do not encode ordinary RF interference into this table: persistent or acute
channel loss remains Pass 110 selector evidence.

Changing the four airtime values changes the §3.6 table hash and requires a
lockstep fleet redeploy.

## Pass 112 — apply frame caps before bitrate (2026-07-28)

**Observed failure.** The final MCS0 mode check used the production
`imx335-100fps-highrange` shape: 1024×576, 100 fps, `resilience=range`, profile
0 at 2829 kbps. The frame-SHM path was healthy (`throttle_permille=1000`, no
pressure or producer drops) and the receiver delivered without deadline loss,
but the encoder sometimes held only 300–850 kbps. Its configured bitrate still
reported 2829 and the link's commanded caps reported I=21761/P=4096, so neither
the source target nor backpressure telemetry explained the underfill.

Lifting the P cap to 12000/16000 bytes changed output but did not reliably
restore the target. Disabling caps live was worse (about 93 kbps), confirming
that generic cap removal is not a safe Star6E repair. The decisive A/B kept the
original I=21761/P=4096 tuple and changed only write order:

- bitrate then caps: persistent underfill;
- caps then a 2828→2829 bitrate re-apply: 2898 kbps measured over 10 s,
  994/1000 expected frames, zero unrecoverable/deadline/supersession drops.

The operator simultaneously confirmed that the displayed stream used its
target bitrate and remained stable.

**Ruling.** When bitrate and caps are pending together, apply caps first and
bitrate last. On demotion, the tighter burst ceiling lands before the lower
target. On promotion, the looser ceiling lands while the old lower bitrate
still binds, then bitrate rises. The same order governs the full-tuple
re-assert after a venc restart. This preserves write-on-change for independent
updates and changes no wire field, table hash, or TX opportunity.

**GOP boundary.** The same run separated recovery deadline from serialization
cadence. With `resilience=off`, 45 fps and a 2 s GOP, roughly one deadline miss
per GOP tracked 45–54 kB IDRs; the profile-0 80 ms I-class recovery deadline is
not an 80 ms exclusive air slot. The shipped MCS0-capable modes use
`resilience=range` (GDR), where an 85 s MCS0/100 fps soak delivered 8540 frames
with zero deadline or supersession drops. MCS0 remains eligible for that
authored GDR mode. Periodic-GOP safety must not be inferred from `maxIBytes`;
Star6E was observed emitting complete Annex-B access units above the configured
I ceiling, so that backend behavior remains separate venc work.

**Post-implementation verification.** The Pass 112 SSC338Q binary
(`md5 8d55ef4add3c0744e0288f8a2157858b`) was deployed on the same craft. A
real profile-0 → profile-2 promotion applied I=59440/P=10731 before the 10303
kbps target and delivered 10258.5 kbps over 15 s. The reverse profile-2 →
profile-0 demotion applied I=21761/P=4096 before the 2829 kbps target and
delivered 2801.3 kbps over 15 s. Both windows had zero unrecoverable,
deadline, or supersession drops, no new producer drop after attach, SHM fill
0%, pressure false, and `throttle_permille=1000`; the actuator reported zero
failures. A preceding 30 s profile-0 soak delivered 2996 frames at 2785.9
kbps with the same zero-drop result.

All ten shipped mode files were also audited: every one explicitly selects
`resilience:"range"`, including all modes whose `min_profile` is 0. No catalog
change is required to keep periodic GOP IDRs out of the authored MCS0 path.

Host verification: full ASan+UBSan build and 50/50 `ctest --preset dev`.
Target verification: clean SSC338Q configure/build with the OpenIPC toolchain.

## Pass 113 — craft-local channel set + runtime pairing gate (2026-07-30)

**Need.** The vehicle OSD menu (waybeam-hub, navigated with FC sticks over the
new MSP DisplayPort mode) must let a pilot at the craft (a) move the craft to
another allowlisted channel and (b) open or lock pairing so the first ground to
hear the beacon can claim the craft — with no IP path to any ground. Neither
action existed: CSA is issuer-only (`h.csa` deliberately null on TX), and
§11.4a selected the key source solely from config with "no separate mode
toggle".

**Ruling (operator, 2026-07-30).** Two new TX-node §15.5 endpoints:

- `POST /api/v1/channel {"mhz":N}` — local retune, gated on
  `policy.csa.channel_allowlist` (400 off-list). Executes the same commit
  sequence as a CSA switch (retune_all → selector `on_rf_environment` → §11.6
  RX-liveness guard), then clears any in-flight campaign
  (`CsaFollower::clear_campaign`) and drops the §11.5a binding
  (`release_binding`) — the ground must re-scout, and a stale
  `kCommitted`/`prev_chan` from before a local retune must never drive a
  revert. Volatile: reboot returns to the boot channel.
- `POST /api/v1/psk {"enabled":bool}` — the §11.4a runtime pairing gate.
  `false` = fresh token, re-key follower + vcmd verifiers (new pairing epoch:
  binding, campaign, anti-replay all cleared), announce `psk_present=1`;
  `true` = keep the current key, stop announcing it. Volatile; the config
  remains the sole boot-time selector.

Self state (`channel`, `psk_announced`, `claimed`, `claimed_by`) joins
`GET /api/v1/info`, and the TX now reports its live channel in `/api/v1/stats`
(`emit_stats` previously passed 0 on TX).

**Boundary.** No wire change, no table-hash change, no persistence —
`persist_channel`/`home_chan` remain dead config (explicitly out of scope).
Both actions are local management HTTP on the craft's control socket, never
over-air commands: an attacker who can reach that socket has already won,
while the over-air surface is unchanged.

## Pass 114 — TX-power override-latch, one endpoint for both RF backends (2026-07-30)

**Need.** The waybeam-hub vehicle menu drove TX power with raw
`iw dev wlan0 set txpower` shell commands — kernel-monitor-only, invisible to
waybeam-link's own §10 power resolve, and clobbered on every `mon-up` /
profile commit. The operator direction (2026-07-30, after the devourer-master
re-vendor A/B): power tables + ONE waybeam-link endpoint that behaves
identically on `kernel-monitor` and `radio`, replacing the hub's `iw` calls.
The roadmap knob had already been deferred from the §15.5 core write set
precisely because a raw write fights the §10.4 commit-time resolve.

**Ruling (operator, 2026-07-30).** New §10.5 + §15.5 rows:

- `POST /api/v1/tx/power {"qdb":N}` — **override-latch**, absolute qdb across
  every `role:"tx"` adapter: apply immediately, re-assert wherever the
  selector would have written power (commit, post-retune), and make the §10.2
  curve resolve yield while latched. `{"auto":true}` clears with one forced
  immediate restore. `GET /api/v1/tx/power` + a §15.3 `link.tx_power_override`
  bool expose the state. Exactly one of `qdb`/`auto` per request.
- Absolute-qdb body (operator choice over a portable-level override): maps 1:1
  to the hub presets and to `iw fixed/auto`, and needs no authored power_map
  to be useful. The §10.3 `max_power_qdb` ceiling still bounds it; nothing
  else does.
- Backend matrix (§10.5): radio → `SetTxPowerOffsetQdb(qdb)`; kernel-monitor →
  forked `iw set txpower fixed qdb×25 mBm` under the §11.6 bounded-CLI
  deadline, `auto` → `txpower auto`. Cadence law unchanged (never per frame).

**Parity repairs riding along (from the 2026-07-30 backend audit).**
`MonAir::set_power_qdb` was a stub, and `tx.apply_power` was wired only
`is_radio()` — the §10.4 commit resolve on kernel-monitor actuated nothing
(logged-intent only). Both fixed: a loaded `power_map` now actuates on both RF
backends through the same seam. Also fixed: the RadioAir stats branch computed
`rx_filtered` and never assigned `as.filtered` (value dropped). The audit's
remaining divergences (radio runtime net_id rebinding no-op — scout/pairing
silently monitor-only; no radio airtime model → §14.2 JSCC dead there; the
§11.6 recovery log printing on radio; `unicast_fallback` semantic split;
monitor 80 MHz retune width) are recorded as roadmap follow-ups, not this pass.

**Boundary.** No wire change, no table-hash change. The latch is volatile
(reboot restores config/curve behavior) and local-management HTTP only —
same §13 posture as Pass 113. `apply_mode` stays radio-only (Pass 13: monitor
carries MCS per-packet in radiotap); power is the one lever unified here.

**Pre-merge review fixups (same PR, 2026-07-30).** Two reviewer findings
tightened before merge: (1) `qdb` is now range-checked as a wide integer at
the route (`-511..511`, 400 outside — the actuator field width; previously a
64-bit JSON value narrowed to `int` *before* the range gate, so e.g. 2^32+20
latched as 20); the range is now spec'd in §10.5/§15.5. (2) A failed actuator
write (forked `iw`) is no longer cached as applied — the §10.4
change-detection cache only records a value the backend accepted, so a
transient `iw` failure retries at the next commit/re-assert instead of
sticking silently (§10.5 bullet added). Spec wording aligned in the same
edit: the latch *yields* at profile commit (nothing to re-write there) and
re-asserts after retune/CSA/recovery; GET reports the latched request while
the §10.3 ceiling clamps at the actuator; radio auto-restore issues one
offset-0 write before the curve resolve resumes; `backend` may read `udp` on
the dev bench (logged intent).

## Pass 115 — report authority follows the claim (2026-07-31)

**Need.** `ReportGate` (§3.5 Pass 41) is first-latcher, and it is sticky in the
wrong direction: a bench station powered on first and reporting continuously
never falls silent, so its latch never ages out, the flying ground's reports
are rejected for its whole uptime, and the craft's §9 selector adapts to a
receiver sitting on a desk. `preferred_originator` fixes it — but only at
config time, which means an edit and a restart on the craft to correct a
mistake made on the ground. There was no runtime lever, and no way to even see
the condition: `reports_rejected` was counted but the *holder* was never
exposed, so the symptom read as "the link adapts oddly".

**Ruling (operator, 2026-07-31, via coordination spec
`specs/cross/2026-07-31-report-latch-authority/`).** An accepted §11.4 CSA
transfers the §3.5 latch to that issuer, immediately, on both gates.

- **The trigger is the acceptance *event*, not the §11.5a binding.** This
  distinction is the whole ruling. An earlier design gated steady-state report
  acceptance on `CsaFollower::latched_` and would have been dead code:
  `do_claim()` is reachable only from `POST /api/v1/csa` and
  `scout/quickconnect`, so a ground that boots, latches and receives never
  issues a CSA and never sets that binding at all. Worse, bounding failover on
  `bind_release_s` (90 s) instead of `relatch_ms` (2 s) would have been a 45x
  regression on dual-ground takeover. Using the event keeps first-latcher and
  the silence rule untouched.
- **Both gates move together.** `feedback_gate_` (§3.10) and `report_gate_`
  (§3.5) are one authority. Moving only LINK_REPORT would leave JSCC feedback
  flowing from the displaced ground — a partial fix that presents as working.
- **Originator-only latch.** `latched_issuer()` carries no session, so the
  forced latch sets `{originator, 0}` and the first accepted report fills the
  session in through the existing same-originator reboot path. Carrying a
  session through from the CSA would be redundant and could stall if the
  reporter's session differs.
- **`preferred_originator` outranks it.** Configured pinning makes both the
  CSA transfer and the new endpoint no-ops (the endpoint answers 409). A
  partial effect here would be worse than no feature — it would look like the
  override worked.
- New §15.5 row `POST /api/v1/reports/latch {"clear":true}` /
  `{"originator":N}`, and §15.3 `return.report_latch_holder` +
  `return.feedback_rejected`.

**Boundary.** No wire change, no table-hash change, no persistence. The
endpoint is local management HTTP on the craft's control socket, same §13
posture as Pass 113/114. The NACK path is deliberately **not** in scope: it
carries no `ReportGate` today (only `target_originator`/`target_session`
matching at ingest), so gating it would be a new restriction on the ARQ path
rather than an extension of this one — raised to the operator separately.

**Known gap.** In a §14.3 cache or multi-originator diversity topology the
JSCC feedback source may legitimately differ from the CSA issuer, and moving
`feedback_gate_` will reject it. The common deployment has one ground as both,
so this is deferred rather than designed around; `feedback_rejected` is what
makes it visible instead of a silent degradation.

## Pass 116 — ARQ lock follows the claim (2026-07-31)

**Need.** Pass 115 made an accepted §11.4 CSA move the §3.5 report latch, and
the operator ruling that followed ships crafts with `preferred_originator: 0`
(the pin prevented a stuck latch only by making failover impossible, and it
made Pass 115 inert). But `preferred_originator` was doing a second job on
TX: §12 ARQ lock preemption (`scheduler.cpp:42-48`). Unpinning removed it, so
the resend lock fell back to pure first-latcher with contested release — and
that has the same stickiness as the report gate did. A bench station powered
on first keeps NACKing, keeps refreshing `last_nack_ms_`, and never releases.

The scenario is adversarial in the worst direction: a bench ground beside the
craft has a good link and NACKs little; the flying ground at range NACKs a
lot. Under budget pressure during a fade — exactly when ARQ earns its keep —
the flying ground's retransmits queue behind the bench's. After Pass 115 the
craft would also be reporting to one ground while repairing for another,
which is the split "claiming a craft" was supposed to end.

**Ruling (operator, 2026-07-31).** The CSA acceptance event moves the §12 lock
on every stream, alongside the §3.5 latch. Two deliberate differences from
Pass 115, both because the lock is a weaker thing than the gate:

- **Soft transfer.** `force_lock()` sets the holder and does NOT touch
  `last_nack_ms_`, so an actively-NACKing node reclaims through the existing
  contested-release rule inside `release_timeout_ms` (500 ms). A holder that
  is not NACKing has nothing to hold and the lock parks as usual. The report
  latch pins; this one nudges. That is correct rather than a compromise: the
  lock tracks who is *recovering*, the latch tracks who has *authority*.
- **It stays a tiebreak, not an exclusion.** §12 already says the lock is a
  tiebreak within the per-originator budget partition — a losing NACKer is
  still served, merely ordered second. Nothing here makes it exclusive, so
  the blast radius is bounded in a way the §3.5 discard is not. This is also
  why NACK ingress stays ungated (Pass 115 boundary): arbitration lives at
  the lock, and a second gate at ingress would be redundant.

`preferred_originator` still outranks both, unchanged.

**Observability.** `lock_holder` existed in `SchedulerCounters` and was read
only by tests — an invisible arbitration state, the same defect Pass 115 fixed
for the report latch. Now emitted per stream as §15.3
`streams[].arq_lock_holder`.

**Boundary.** No wire change, no table-hash change, no new endpoint, no
persistence. Craft-local TX state only.

**Known gap, carried from Pass 115.** A §14.3 cache node legitimately NACKs
for its own recovery, and a claim now moves the lock away from it. The soft
transfer defuses most of it — an actively-NACKing cache reclaims in
`release_timeout_ms` — but unlike the §3.10 feedback case this one sits on a
path that moves packets. Recorded rather than designed around; revisit if a
cache deployment appears.

---

## Pass 117 — the craft names its report holder on air (2026-07-31)

**Need.** Passes 115/116 made authority *transferable* and *countable*, and
`return.report_latch_holder` made it readable — on the craft. The ground still
could not see it. `ReportGate` runs at TX ingest, so on an RX the field is
present-but-zero by schema convention (Pass 115, Q4), and no packet carried it
craft→ground. The one node that most needs the answer is the ground that has
*lost* the latch: its LINK_REPORTs are discarded, the craft adapts to somebody
else, and the local symptom is an unexplained link with every local counter
looking healthy. Reaching the craft over IP answers it on a bench and nowhere
else — which is the wrong half, because the sticky-latch failure is a flying
failure.

waybeam-hub hit this implementing the ground status card: the plan assumed the
holder would ride the existing §15.3 stats stream the hub already ingests, and
on a ground that stream can only ever report 0. Rendering it would have printed
"no latch" over exactly the failure the card exists to expose.

**Ruling (operator, 2026-07-31).** §3.15 SELECTOR_STATE carries the holder.
That packet is already craft→ground advisory observability, already accepted
only from the latched RTP `(originator,session)` with a matching
`table_version`, already expires after 1.5 s so a rebooted craft leaves no
stale claim on screen, and is already emitted inside an existing live video
slot under the Pass 110 guard-cost boundary. Every property the holder needs
was built for the lockout summary; none of it is new cost.

**Length is flag-driven.** A fixed +2 bytes would have made every legacy
32-byte summary fail `len != kSelectorStateSize` and drop, taking the working
lockout display down on any mixed-version pair to add a field. So
`state_flags` bit3 (`kHolderPresent`) selects the length: set → 34 bytes with
the field, clear → the legacy 32 without it. A receiver accepts both and rejects
disagreement between the flag and the length, which is what makes the pairing
self-checking rather than a convention.

**It buys one direction only, and the other one is a real regression.** A new
receiver reads either shape. A pre-bit3 receiver validates `len == 32`
exactly, so a Pass-117 craft's 34-byte summary is dropped whole and that
ground loses profile/MCS, lockout state and loss window — everything §3.15
carries, not just the holder. Measured, not reasoned: old-TX→old-RX gives
`selector_state_valid=true, profile/mcs 7/7`; new-TX→old-RX gives
`false, 0/0`. §3.15 is advisory so no control path degrades and DATA is
untouched, but the lockout warning going dark is not nothing. **Deploy order
is receivers first, then crafts.** A new packet type would have been
transparently ignorable instead, but it would need its own cadence and its own
§7.2 guard-cost argument to carry one u16 that belongs, semantically, in the
summary it annotates — the ordering constraint is the cheaper price.

Bit3 is also what keeps the tri-state honest. "Not reported" and "no latch"
are different answers and the losing-ground case is precisely where confusing
them is expensive, so §15.3 emits `link.report_latch_known` beside
`link.report_latch_holder` rather than overloading 0. `link.` is deliberate:
on a TX it mirrors the local gate, on an RX it carries the craft's, so a
consumer reads one key on both roles instead of branching on role.

**Boundary.** Advisory only — the field names the holder, it never moves
authority; §3.5 first-latcher/relatch and the §11.4 transfer remain the only
ways the latch moves. No table-hash change (the profile table is untouched),
no new endpoint, no persistence, no change to the 2 Hz cadence or the slot it
rides in.

**Residual.** A legacy craft reports `report_latch_known=false` forever, so a
new ground shows "unknown" against an old craft rather than the answer. That
is the intended degradation and the reason the flag exists, but it does mean
the diagnostic only works once the craft side is updated — the ground alone is
not enough.

## Pass 118 — one rate mechanism, and the rate we received (2026-07-31)

**Question.** OpenIPC/devourer PR #334 (honour `RADIOTAP_F_TX_NOACK` on
Jaguar3) prompted an audit of what our radiotap actually does on the devourer
path. Two findings, and they point the same way.

First, our NOACK bit has never done anything through devourer.
`IEEE80211_RADIOTAP_F_TX_NOACK` appears exactly once in the vendored tree —
the constant's own definition. Jaguar1 parses TX_FLAGS into a local
(`RtlJaguarDevice.cpp:951`) and never reads it; Jaguar2/3 do not parse the
field at all. What actually suppresses ACK and retry on our broadcast frames
is the §3.0 broadcast DA driving `BMC=1` in the descriptor, not the flag we
set. The flag was decorative.

Second — the useful half — devourer already honours a **per-packet radiotap
MCS** on all three generations (`decode_radiotap_mcs_field`, shared by
`RtlJaguarDevice.cpp:969` / `RtlJaguar2Device.cpp:1389` /
`RtlJaguar3Device.cpp:1870`), and `TxMode` is documented as the default
"applied when a frame's radiotap carries no rate" (`src/TxMode.h:6-11`). The
capability the kernel-monitor backend was built around has been available on
the devourer path the whole time; Pass 13 split the two backends on a
constraint that no longer holds.

**Ruling (operator, 2026-07-31).** Converge. Both backends prepend the
13-byte HT radiotap with the per-packet MCS field; the Pass-13 two-mechanism
split is retired. `SetTxMode` is **kept**, committed in lockstep at each §9.5
transition, as a fallback-only default.

Keeping it is not redundancy for its own sake. Devourer consults `TxMode`
only for frames whose radiotap carries no rate, so on a healthy path it is
never read and costs nothing on air. What it buys is the failure mode: a
malformed radiotap prefix would otherwise drop the entire link to the
driver's legacy default (MGN 6M) silently, at full airtime cost, with every
local counter healthy. With the commit retained it degrades to the operating
point we last chose. One source of truth for the rate, one bounded way to be
wrong about it.

**This changes no on-air byte.** Radiotap is an injection-local convention,
never transmitted. Pre- and post-Pass-118 nodes interoperate in both
directions, there is no §3.1 version event, and no table hash moves. That is
what makes it safe to land ahead of the multi-rung work rather than inside
it — the mechanism can be benched on its own.

**Mechanism is per-packet; policy is not.** Every packet still carries the
rung the §9.5 transition committed. This pass deliberately does not introduce
per-packet rate divergence.

**The RX half is the reason to do it now.** Threading the received MCS up
from both backends — radiotap bit 19 on kernel-monitor, `RxPacket.data_rate`
(`DESC_RATEMCS0 = 0x0c`, so `mcs = code - 12`) on devourer — costs one field
on `AirRxMeta` and needs **no wire change**, because the rate a frame was
aired at is already recoverable at the receiver on both paths. §15.3 gains a
per-adapter `rx_mcs[8]` histogram plus `rx_mcs_unknown`, summing to `rx`.

That matters more than it looks. It means a per-MCS PER ladder — the thing
that would let adaptation act on measured per-rate degradation instead of
RSSI as a proxy — is a §9 change and not a §3 wire-version bump, so it never
needs a fleet lockstep redeploy. And because the selector already moves
between rungs over time, samples start accumulating immediately, before any
dual-rung capability exists.

**Boundary.** `rx_mcs` is advisory: no control path reads it, the selector is
untouched, and §9.1/§9.2 demote/promote still run on the existing loss window
and RSSI margin exactly as before.

**Residual — two, both deliberate.**

The PER ladder has a denominator and no numerator. `rx_mcs` counts frames we
*received* per MCS; PER needs the frames we *didn't*, and a sequence gap does
not carry the MCS the missing packet would have been aired at. It can be
inferred from the surrounding frames, but the inference is exactly wrong in
the case that matters — a burst lost at a rung we just promoted to. Left open
as a §9.2 question rather than guessed at here.

Time-separated samples are confounded. Until per-packet rate divergence
exists, per-MCS statistics compare rungs measured at different *times*, so
channel drift is baked into any ladder derived from them. Simultaneous
dual-rung transmission is what removes the confound, and that is gated on
§9.5 having a mixed-rate airtime model at all — the derived-bitrate math and
`airtime_budget_frac` assume one service rate, and `ht20_service_time_us()`
takes a single `mcs`. Nothing here is a step toward assuming otherwise.

## Pass 120 — craft-resident link calibration (2026-08-01)

**Question.** The bench sessions on PR #81 validated a closed-loop
calibration (place every rung in a target RSSI band, measure per-rung
overload ceilings, author the §10.2 curve, acceptance-sweep clean across
the whole ladder) — driven over IP from the desk. The fleet needs it in the
field, where the craft has no IP path. How does the loop run, and how does
the artifact reach the craft?

**Ruling (operator, 2026-08-01 — R1–R5 adopted as recommended).** It
doesn't reach the craft; it is born there. The §3.5 LINK_REPORT already
carries the complete feedback set (`rssi_best`/`rssi_mean`,
`loss_postdiv_prearq`, `uniq`) at report cadence, so the loop moves
craft-side and steers against what the ground reports hearing. No bulk
data crosses the air — which §11.7's narrow-channel law would have
forbidden anyway. The ground contributes exactly a claim-gated trigger and
a display.

- **R1 — registry.** `0x08 CALIBRATE {0=abort, 1=start}`, no further args
  (nobody speculates a dry-run). `start` REJECTED when already running,
  no power actuator, or no latched reporter — the last is load-bearing:
  the loop is blind without reports. Gating is the existing §11.7
  bound-issuer + PSK HMAC + nonce machinery, unchanged.
- **R2 — persistence.** The MODE precedent, split the same way: run state
  stays craft-session volatile; the *artifact* (curve + ceiling report +
  pairing fingerprint, ~1 KB) persists in `/etc/waybeam-link/calibration/`
  (atomic write, single last-good copy) and auto-loads as the TX adapter's
  `power_map` on boot **only when the fingerprint's craft-adapter identity
  matches the live adapter**. Mismatch ⇒ boot with no curve + surface
  CALIBRATION STALE. A curve calibrated for other hardware is never
  silently applied.
- **R3 — observability.** The §3.15 32→34 `report_latch_holder` extension
  pattern is the sanctioned mechanism: bytes 34–35, `state_flags` bit4,
  {state, rung} + CRC-8 artifact hash (§3.6 idiom). Failure reasons and
  the full report are management-HTTP only (`GET /api/v1/calibration`).
- **R4 — restore.** Every exit path converges on power-first restore
  (a probe may sit on a rung's ceiling), then the boot selector window,
  then the §10.4 resolve. Report-loss abort at 3 s (not the §9 report
  timeout — dwell-edge gaps must not thrash it); hard cap 240 s. §9.8
  fail-toward-degradation throughout.
- **R5 — placement.** The loop is a pure time-injected state machine in
  `core/` (the selector/CSA shape): inputs are report samples and ticks,
  outputs are pin/power/artifact actions. `io/` supplies actuation through
  seams that all exist (§10.5 `set_power_qdb`, §9.7 pin, file sink). The
  Android `:wifi` consumer vendors `core/` whole, so a phone ground
  inherits calibration display for free.

**Field-vs-bench fidelity, stated.** The field loop steers on
post-diversity loss (what reports carry); the bench tool used per-adapter
wire PER. They agree at the placement ("clean here") and at the cliff
(adapters fail together — measured, PR #81). Per-adapter fidelity remains
a bench-mode capability. Calibrate at 50–100 m separation (operator
direction): placement power is what range realism buys; the
receiver-referenced ceilings transfer regardless.

**Boundary.** Calibration writes the §10.2 curve; it does not touch the
selector's thresholds, §9.4's gate, or the channel. The per-rung ceiling
table it persists is the *input* a future banded promote gate would
consume — that gate is its own pass, not this one.

## Pass 121 — calibration max-power seek (2026-08-01)

**Trigger.** The first field run of the Pass 120 loop (craft 17, 50 m,
live over RF) failed in a way the bench could not show: ground RSSI sat
at −78…−85 for the whole run while the steer commanded power upward —
an RF power cap latched below the commanded values ("RF CAP P0",
operator-observed on-device), so the delivered power never followed and
the target band (−32 ± 3) was unreachable regardless. Every rung would
have "placed" at a commanded qdb the radio never emitted. The run was
aborted over RF; no artifact was written (aborted runs never persist —
the envelope held exactly as specced).

**Ruling (operator direction).** Retire the target-band steer. The loop
now seeks the **maximum power that does not break the link**, per rung:
ramp upward in `seek_step_qdb` steps and stop at the first of two walls —
the **loss wall** (report loss > `loss_bad_milli`: overload or break) or
the **cap wall** (a commanded step of ≥ 2 dB moves report `rssi_mean` by
< `cap_rise_db`: delivered power stopped following commanded power).
Placement = one step below the wall; the wall's bracketing RSSIs ARE the
overload-ceiling record, so the separate CEILING phase is deleted, not
moved. A `rssi_guard_dbm` (−20) sanity bound keeps a too-close run out
of the pure-overload regime. First-probe-already-bad descends until
clean (floor `min_qdb`).

**What this buys.**
- The curve becomes "max deliverable clean power per rung" — maximal
  link budget at range by construction, instead of "whatever landed at
  −32 on the bench that day".
- Commanded-vs-delivered divergence is *detected* (cap wall), not
  recorded as fiction.
- The calibration distance requirement collapses from a 50–100 m walk
  to **near-bench, 2–10 m** — far enough that upper-rung overload
  ceilings sit above the cap wall, close enough for a desk.

**Unchanged.** Artifact shape, store, CRC-8 fingerprint, STALE gate,
boot auto-load, §3.15 word, VCMD 0x08 semantics, the R4 restore order,
and both abort clocks — this is an engine-phase change (SEEK→VERIFY per
rung) plus spec text. §15.2 gains `seek_step_qdb`/`cap_rise_db`/
`rssi_guard_dbm`; the retired band keys are ignored. The §10.2 level
compensation stays: a tiered rung resolving below its own maximum is
headroom, not error.

**Pass 121 addendum (same day, operator).** First live seek run (bench
distance, fp 0xE8): rungs 4-7 found genuine loss walls (17-21 dBm), but
rungs 0-3 were stopped by the −20 rssi_guard, not by loss — a guard
artifact 2 dB short of the radio's true maximum. Operator ruling: the
guard is a token backstop, not a placement mechanism — default moves
−20 → **−6**; the loss wall is the intended limiter at every rung.

**Pass 121 addendum 2 (same day, operator).** The guard −6 rerun
(fp 0xE3) exposed a verify gap: rung 5's placement probed clean at
1.2 s dwells but the 2.5 s verify measured **942‰** — near-cliff
instability appears only under sustained exposure — and the engine
recorded it and moved on (rung 7 likewise verified 34‰ > loss_ok).
Ruling: **verify enforces loss_ok_milli** — on failure, step down one
seek_step_qdb and re-verify, bounded at 3 descents; a still-failing
floor is recorded with its measured loss (the artifact never lies).
Verify loss above loss_bad_milli also records the overload bracket.

**Pass 121 addendum 4 (same day, autonomous campaign).** The addendum-3
verification campaign failed 3/5 runs — all `report_loss`, all at rung 7.
Mechanism: at a wall probe near total overload the feedback channel
itself collapses (ground hears nothing, reports stop); a bad probe held
~2 s in v1 (just under the 3 s abort), and the confirmation dwell
doubled wall exposure past it. Ruling: a report blackout while probing
ABOVE the rung's last clean power is the strongest possible wall
evidence (loss = total), not a failure — book the bracket, retreat to
last_clean, re-arm the report clock, proceed to verify. Once per rung;
blackout at safe power / during verify / recurring on the rung still
aborts. The report-loss abort remains the outer safety net for a truly
dead ground.

**Pass 121 addendum 5 (same day, autonomous campaign).** v3 campaign
(addenda 3+4): 8/10 done, spreads collapsed — rungs 0-5 and 7 now ZERO
spread across runs (rung 7 settles honestly at 15 dBm; the v1 23s were
the noise, not the 15s). The 2 remaining failures were both rung 6 (the
one marginal rung left): seek blackout → retreat → verify at the retreat
placement blacks out again → abort, because verify had no retreat path.
Ruling: a blackout during VERIFY is a verify failure of total severity —
take the addendum-2 bounded step-down (shared 3-descent budget, clock
re-armed per descent) instead of aborting. Abort remains for no-floor /
exhausted descents / truly dead ground.

**Pass 121 addendum 6 (2026-08-01).** §15.3 clarified: on a ground/rx
node the `calib_*` stats fields mirror the RECEIVED §3.15 calibration
word of the current selector source (state/rung from byte 0, fingerprint
from byte 1, while bit4 is set and the source is fresh) — the ground
WebUI's only view of calibration progress when the craft has no IP path.
`calib_stale` is craft-local and stays false on ground. Prerequisite for
the waybeam-hub vehicle-menu calibrate button.

## Pass 122 — claimed-ground negotiated packet budget (2026-08-01)

**Trigger.** The vehicle SOC becomes CPU-bound once the high-rate video path
exceeds roughly 1000 packets/s. All deployed injection adapters are Realtek
parts capable of a 3072-byte Waybeam DATA packet, but using that size
unconditionally would silently strand legacy or unknown ground receivers.

**Ruling.** The claimed ground owns one global packet-budget tier, subject to
explicit vehicle acceptance. `VEHICLE_CMD 0x09 MTU_TIER` carries only a concrete
Default/Medium/High enum; the ground-local Automatic mode resolves the minimum
capability of all active ground adapters before sending it. The exact complete
DATA wire-packet budgets are 1424/2048/3072 bytes. No receiver quorum, telemetry
piggyback, or continuous MTU controller is added.

The vehicle applies an accepted tier at a frame/block boundary and frames with
`min(profile.max_payload, negotiated_packet_budget)`. It boots and becomes
unbound at Default. A request beyond any active craft adapter is rejected, never
clamped. Profiles remain the coarse packet-rate policy and should target
1000–1200 total DATA packets/s; FEC stays one frame per block and does not pad or
merge frames to force an arbitrary 20–30 symbols.

**Verification status — device-confirmed (same day).** The implementation pass
completed the 51-test host suite, SSC338Q/RK3566/x86 release builds, and the Hub
2080-test suite. It was deployed lockstep with table version `0x80` to an
SSC338Q + RTL8812EU craft, x86 ground with RTL8812EU uplink + RTL8812CU
diversity, and an RK3566 + RTL8812CU spectator. Every active monitor netdev
reported MTU 4052; ground Automatic resolved High, the authenticated command
ACKed, and both issuer and craft reported 3072. The spectator exposed read-only
capability state and rejected negotiation with HTTP 409 as required. Both x86
and RK Hub menus exposed Default/Medium/High/Automatic.

Matched 15 s RF samples used profile/MCS 5, a 21839 kb/s encoder target, and
about 23.5 Mb/s delivered video. Vehicle CPU is process CPU from `/proc` at
`CLK_TCK=100` (`perf` is not installed on the craft):

| mode | budget | vehicle CPU | source + repair pps | all air submits/s | source + repair symbols/block | delivered | FEC recovered | unrecoverable |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Default | 1424 | 28.1% of one core | 2627 | 2684 | 20.41 + 4.35 | 23.39 Mb/s | 138 | 0 |
| Medium | 2048 | 24.6% of one core | 1804 | 1860 | 14.12 + 3.01 | 23.53 Mb/s | 127 | 0 |
| High | 3072 | 21.3% of one core | 1263 | 1319 | 9.95 + 2.00 | 23.52 Mb/s | 126 | 1 |

High reduced link-process CPU by 24.2% relative and air submissions by 50.8%
without decode errors. A longer High/Automatic observation accumulated 11514
delivered frame-blocks, 806 FEC-recovered frames, one unrecoverable frame, zero
decode errors, zero craft FEC-oversize frames, zero SHM-full drops, and no radio
TX failure/wedge. The observed 1263 source+repair pps is close to, but slightly
above, the 1000–1200 policy aim; reaching the strict band would require either a
larger-than-v1 tier or less repair, so robustness remains preferred. The normal
flight mode and its profile range were restored after the benchmark; Automatic
remains selected, so the profile ceiling keeps low-rate profiles at 1424 and
admits 3072 automatically when a jumbo-capable high-rate profile is active.

## Pass 123 — jumbo source-symbol floor (2026-08-01; superseded by Pass 124)

**Trigger.** The Pass 122 hardware comparison achieved its CPU and packet-rate
goal at High, but a 100 fps P-frame averaged only 9.95 source symbols plus two
repair symbols. That leaves a short FEC block with too little erasure depth for
the intended burst-loss robustness; the operator requested a floor near 16
before merge readiness.

**Ruling (operator direction; exact threshold selected from the measured
trade-off).** Apply a per-frame **16-source-symbol jumbo guard**. The largest
symbol that still gives `k >= 16` is `floor((frame_len - 1) / 15)`. Intersect
that with the profile/negotiated ceiling, but never shrink below the existing
Default symbol size (1387 B). Thus jumbo mode cannot reduce a block below 16
when the Default path would have reached 16, while small frames that were
already below 16 at Default do not explode into tiny packets. The choice of 16
raises a 20% P-frame policy from roughly 2 to 4 repair symbols at the measured
high-rate operating point without restoring the full 20-symbol/Default packet
load.

This guard applies only to `rlc256` streams: shrinking a FEC-disabled stream
would spend the packet-rate/CPU budget without adding recovery depth. It is a
packet-size choice made once at the existing frame/block boundary, not padding,
frame merging, a new wire field, or a continuous MTU negotiation. Every block
remains homogeneous and self-describing. A cumulative
`mtu_floor_clamped_frames` stream stat makes the guard observable. Merge
readiness requires the complete host suite, all three target builds, a minimum
ten-case targeted matrix around integer boundaries and tier/profile
intersections, an independent full-diff review, and a repeated high-bitrate
hardware comparison.

## Pass 124 — repair-depth guard replaces source refragmentation (2026-08-01)

**Review question.** Is 16 the right source-symbol floor, or is another number
more suitable? Deterministic MDS properties and synthetic loss simulation show
that source count is the wrong actuator. Pass 123's measured benefit came from
the 20% policy crossing `ceil(16 * 0.20) = 4` repairs, not from `k=16` itself.

At the measured 100 fps operating point, the candidate blocks were simulated
under independent loss and a two-state Gilbert-Elliott burst process (80% loss
in the bad state, 0.2% in the good state, 2% stationary loss, four-packet mean
bad run; 1,000,000 blocks). Results:

| policy | block | DATA packets/s | iid failure at 5% | burst failure |
|---|---:|---:|---:|---:|
| Pass 122 High | 10+2 | 1268 | 1.9568% | 3.3685% |
| literal source floor | 16+4 | 2114 | 0.2574% | 2.5069% |
| High + equivalent repair depth | 10+4 | 1480 | 0.0427% | 1.6679% |

For the same four correctable erasures, `10+4` is both shorter and more robust
than `16+4`; the literal floor transmits six extra independently lossy source
packets and predicts roughly 25.6% vehicle CPU versus 22.4% for `10+4` from the
Pass 122 hardware slope. Floors 12/15 stop at three repairs; floor 20 adds four
source packets without a fifth repair; floor 21 reaches five repairs but
restores/exceeds Default packet load. Thus **16 remains the correct policy
threshold, but not a fragmentation target**.

**Ruling.** Supersede Pass 123's refragmentation. Keep the negotiated jumbo
symbol size. When jumbo produces `k < 16` for a frame that Default would have
encoded with `k >= 16`, raise final parity to at least the configured class
rate evaluated at `k=16` (four under the deployed 20% P-frame policy), after
the fixed or enforced §14.2 decision. Small frames, FEC-disabled streams, and
zero-rate classes are unchanged. Count actuations in `mtu_fec_guard_frames`.
This preserves most of the packet/CPU gain while providing greater erasure
robustness than the literal source-symbol clamp.

**Merge-review addendum.** Pass 124 is subordinate to two existing §14.1
hard gates. It does not resurrect parity for an ARQ-eligible `k <= min_k`
frame, and it does not recreate a block rejected because configured `min_r`
would make `k+r > 256`. Regression cases cover High-mode `k=10,min_k=10`
under all-frame ARQ and `k=10,min_r=255`; both remain source-only.

**Reviewed-binary hardware A/B (2026-08-01).** Commit `691ff36` ran on the
SSC338Q craft against the x86 ground at profile/MCS 5 and a commanded
21,839 kbps encoder rate. Each arm was a matched 20 s `/proc` process-CPU and
counter-delta sample; delivered video remained approximately 23.7 Mb/s.

| tier | budget | craft CPU (one core) | source+repair pkt/s | all air submits/s | mean k+r | guard frames | FEC recovered | unrecoverable / decode / oversize / TX fail |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Default | 1424 | 29.80% | 2687 | 2744 | 20.39+4.69 | 0 | 302 | 0 / 0 / 0 / 0 |
| Medium | 2048 | 25.60% | 1949 | 2006 | 14.20+3.97 | 1673 | 195 | 0 / 0 / 0 / 0 |
| High | 3072 | 24.25% | 1466 | 1522 | 9.70+4.00 | 2138 | 163 | 0 / 0 / 0 / 0 |

High therefore removed 45.4% of DATA packet submissions and 5.55 percentage
points of vehicle CPU versus Default while preserving an approximately
four-erasure block depth. The original 1000--1200 packet/s target is met by
source symbols alone (1038/s), but not by total DATA after the repair-depth
guard (1466/s); that is the explicit CPU/robustness trade-off adopted above.

**Review addendum.** Two authority seams are part of the same merge gate. The
craft resets MTU to Default on every newly accepted authenticated CSA campaign,
not only when `latched_issuer` changes: a rebooted ground may reuse the same
numeric originator while changing session and adapter capability. The generic
`POST /vehicle/command` rejects `mtu_tier`; only the typed `/link/mtu` path and
its internal post-claim reissue may start it, preserving local capability
validation and preference bookkeeping.

## Pass 125 — authenticated ground-uplink TX calibration plan (2026-08-02)

**Trigger.** Pass 120/121 calibrates the craft downlink only. The ground's
designated uplink adapter remains at its driver/config placement, so return
margin can collapse first at range and present to the craft as report blackout.
The original PR #82 draft proposed appending feedback to SELECTOR_STATE, but
that packet is explicitly unauthenticated/advisory and older receivers reject
new exact-length shapes wholesale. Neither property is acceptable for feedback
that moves an RF power actuator.

**Ruling.** Reserve `0xF UPLINK_QUALITY`, a **35-byte** craft→report-latched-ground
packet authenticated with the existing `csa_psk` over bytes 0..30. It carries the
exact target ground tuple, last accepted report epoch, cumulative
accepted-report count, cumulative signed RX-RSSI sum, craft-adapter fingerprint,
`last_rx_mcs`, and a four-byte HMAC. It is due at 2 Hz immediately before
existing live DATA and never creates an air slot. SELECTOR_STATE remains
byte-for-byte unchanged.

`last_rx_mcs` ships in v1 despite v1 having one uplink rung: the packet is
exact-length with no spare byte and no flags field, so adding it later is a wire
break, and it is the commanded-versus-delivered rung cross-check — the rung
analogue of §10.6's commanded-qdb-versus-delivered-RSSI cap wall. Given the
fleet's history of chips silently not honouring a commanded rate, a future
multi-rung uplink without it would present as an unexplained loss wall.

**Authority is the §3.5 report latch, not a §11.4 CSA claim.** The craft
addresses quality only to its accepted reporter, so a valid-MAC packet naming a
ground's own `(originator, session)` *is* proof that ground holds the latch.
Requiring a CSA claim would make uplink calibration unavailable in any
deployment that never changes channel, while the craft is already feeding that
ground authenticated quality; and `ReportGate` is single-holder, so two grounds
can never calibrate against one craft. §3.16's accept rule and §10.7's start
rule are therefore the same rule.

**Two clocks, not one.** Any accepted packet refreshes *liveness*; only an
advancing `reports_received` refreshes *counter progress*. Liveness loss for 2 s
aborts — the run has lost its observer. Stalled counters under live feedback are
a 1000‰ loss observation, not an absence of evidence. The first draft conflated
them, which would have aborted every run within 2 s of leaving the floor: unlike
§10.6, where the ground keeps emitting reports whose *contents* change at total
loss, §3.16's contents freeze when the uplink dies.

**Floor and ceiling are different walls — this amends §10.6.** A loss wall means
"too hot" only above a clean probe. With none yet, loss means "too cold" and the
seek MUST ascend to `max_qdb`, failing `no_clean_point` only there. Today
`calibrate.h` places at `min_qdb` in that case and its blackout retreat falls
through to a `report_loss` abort because it requires a `last_clean_`. The Pass
121 near-bench campaign never exercised this because a 1 dBm floor is always
receivable at 2–10 m; a one-rung MCS0 uplink at range exercises it every run.

**Dwell loss is anchored on the craft's own `last_report_epoch` bounds:**
`emitted = E_B - E_A` from the dwell's own first and last accepted packets. At a
40-epoch dwell one boundary-straddling report is 25‰ — between `loss_ok_milli`
and `loss_bad_milli` — so an unanchored denominator would make every clean dwell
read ambiguous and burn the one-shot extension to 80 on nothing. Because
`core/src/reporter.cpp` increments `report_epoch` once per emitted report, this
needs **no ground-side emission bookkeeping at all** — the craft's own delta is
the emitted count. The identity holds only while build and injection are paired;
an implementation that can drop a built report must increment at injection.
`received == 0` scores 1000‰ against the ground's local `Reporter::epoch()`
delta and contributes no RSSI sample.

**The uplink rung is configured, not inherited.** An rx-node never called
`set_tx_mode`: its uplink rung was the `TxRate` struct default, which happens to
be MCS0/LGI/HT20. A calibrated placement must not rest on a default no one
asserted, since the artifact records the rung and `last_rx_mcs` cross-checks it.
`air.uplink_rate` (seeds `{0, false, 20}`) commits it through the existing seam;
on-air behaviour at the seeds is byte-identical, and the future multi-rung pass
widens a config value rather than adding a mechanism.

Use ordinary unique LINK_REPORT epochs as the sparse probes: no padded traffic
and no new VEHICLE_CMD. Extract §10.6's pure seek/verify state into a reusable
time-injected `PowerSeek` taking an explicit `clean|bad|no_evidence` per-dwell
verdict; retain the eight-rung craft orchestrator and add a ground orchestrator
running one seeker per configured uplink rung (v1: one, MCS0/LGI/HT20 — a config
value, not a constant). Seed 16 qdb steps, 40-report probe dwells (one extension
to 80 when ambiguous), 200-report verification, the existing 15/50‰ loss walls,
and a 2 s liveness timeout.

**Actuator.** The `power_map` rejection re-keys from node role to **adapter
role**: rejected on any `role:"rx"` adapter, accepted on a `role:"tx"` adapter,
either node role. The full MCS0–7 curve loads; v1 resolves only the configured
uplink rung — a usage restriction, not a format one. §10.5's override latch
extends to any node with a `role:"tx"` adapter, giving the ground a manual
placement without a config edit and a restart. Explicit config outranks the
uplink artifact, which applies only when the selected craft and both adapter
identities match. Every non-success exit restores explicit config, the prior
matching artifact, or backend auto/default in that order.

**Interlock and order.** The two directions must never overlap: §10.7 drives
ground power to `min_qdb`, starving the report stream §10.6's dwells and
`report_loss_abort_ms` clock depend on. §10.7 start 409s on the mirrored §3.15
`calib_state == running`; while §10.7 runs the ground refuses to issue §11.7
`CALIBRATE`. Order is **uplink first, then downlink** — §10.6 needs a working
uplink, §10.7 needs only live DATA.

**Integration boundary.** Link owns wire, engine, actuator, persistence, REST,
and stats, and keeps the two operations independent and independently startable.
Hub exposes **one** bi-directional calibration action and owns the sequencing
(uplink via local `:8092`, stop on failure, then §11.7 downlink) — calibration
is a one-time commissioning step persisted on both sides, so a ~2-minute
combined run is the right unit of work and splitting it invites the wrong order.
SBC packaging pins reviewed Link/Hub heads. Android currently has no Link wire
decoder or selector-state consumer, so it needs no code change; unknown `0xF`
remains a compatibility test.

**Prepared for, not implemented: multi-rung ground uplink.** A future pass will
run the uplink at higher rungs so control/telemetry occupies less airtime. Pass
125 ensures nothing needs migrating for it — `power_map` is already the full
MCS0–7 curve, `TxRate{mcs,sgi,bw}` already flows per-frame through
`dot11_tx_prefix`, §10.5's latch is rung-agnostic, §3.16 carries `last_rx_mcs`,
the artifact's `placements` is a list with rate identity inside the entry, and
§15.3 carries `uplink_calib_rung`. The floor rule is a prerequisite, not a
nicety: a higher rung has a higher usable floor. That pass's other contact
points are §9.3's `airtime_budget_frac` (`io/airtime.h`) for uplink airtime
accounting and §7.2's return-window sizing, which assumes today's uplink frame
duration.

**Merge gate.** Full host suite and SSC338Q/x86-ground/RK3566 builds; at least
27 focused wire/calibrator/config/store/REST/stats tests; independent full-diff
review; ten consecutive hardware runs with placement spread no greater than one
seek step; a blacked-out-floor run proving the seek ascends rather than aborting;
interlock verification in both directions; failure injection for abort, liveness
blackout, mismatch, restart, and retune; the combined Hub action end to end
including uplink-failure-stops-sequence; plus calibrated-vs-default/manual range
evidence for report delivery, NACK recovery, blackouts, RSSI, and uplink packet
loss.

## Pass 126 — §10.7 defects found by adversarial review (2026-08-02)

Three CRITICALs against the Pass 125 implementation, each reproduced by an
executed probe rather than by reading. All three lived in `core/` — the part
that was best unit-tested and therefore most trusted. Two of the three produced
a **false success**, which is worse than a crash: nothing downstream inspects a
`done`.

**C1 — the blackout fallback keyed on the wrong clock.** `UplinkCalibrator`
ended a stalled dwell only when `received_ == 0`. A *partial* blackout — some
epochs land, the uplink then dies mid-dwell — leaves `received_ > 0` with the
craft's anchors frozen short of target, so the dwell never ended: reproduced as
`STILL RUNNING after dt=200900 ms, dwell 1/40, qdb=4`, i.e. wedged at `min_qdb`
for the full 600 s hard cap. That is the commonest shape of a real floor, so the
§10.6 floor rule was unreachable in precisely the scenario Pass 125 added it
for. The condition is now `emitted_` falling short of the epoch target, measured
against the ground's own `Reporter::epoch()` delta. §10.7 amended to state the
clock explicitly.

Fixing it exposed a second-order error: the local-epoch anchor was taken at
`begin_dwell`, i.e. *before* the settle window, while `emitted_` only starts
accumulating after it. The ground clock therefore ran ahead by `settle_ms`
worth of epochs and, once the fallback was reachable, would have tripped it on
a perfectly healthy dwell. The anchor is now armed at the first post-settle
tick, so both clocks start together.

**C2 — `verify` had no failure outcome.** `PowerSeek::verify_` returns `kDone`
when the descent budget or the floor is exhausted, even with loss above
`loss_bad_milli`. Reproduced from the ladder clean@4 / clean@20 / bad@36:
`state=2(kDone) placement qdb=4 loss=995 rssi=-90 last_clean=20` — a placement
at 995‰ recorded *while a clean probe 16 qdb higher was on record*. For §10.6
that is law and stays (`the artifact never lies`; a craft artifact is a record
the operator reads, and the report clock aborts a dead downlink long before
this). For §10.7 it is a false success: the ground artifact auto-applies at the
next boot and its terminal state gates the sequencer, so `done` would persist an
unusable power *and* start the §11.7 campaign across a dead uplink — defeating
the order law through a success state, which no interlock inspects because every
interlock is written against `running`/`failed`. Resolved as a §10.7 policy on
the shared seek's output (`verify_failed`, nothing persisted, power restored),
**not** as a change to `PowerSeek` — the operator's finding located it in
`verify_`, but applying it there would have broken the §10.6 rule quoted above.
§10.7 amended.

**C3 — the artifact failed its own fingerprint on reload.** `first_bad_qdb_`
survived `UplinkCalibrator::start()`, so a second run in one process published
the *first* run's bracket alongside `has_first_bad=false`. The writer serializes
that as JSON `null` and the loader reads it back as 0, so the rehash disagreed:
`write fp=0x7f -> load: artifact fingerprint mismatch`. Operator sees DONE, next
boot silently discards the measurement. Fixed at both ends — reset on `start()`,
and `canonical_bytes` hashes 0 for an absent bracket so the form round-trips
regardless of in-memory residue.

**D3 — `uplink_calib_matches()` had zero call sites.** Fully unit-tested,
exported, never called: the resolver checked only the *local* adapter identity,
so a §10.7 artifact would apply against the wrong craft, the wrong craft RX
chain, or the wrong channel — while the API reported `stale:false` asserting a
match nothing had tested. The cause was structural rather than an oversight: the
resolver was declared with the config tier, above `active_selection`,
`quality_gate` and `operating_chan`, so the craft half of the pairing was not in
scope. The resolve block now sits after that tuple and checks it in full.

That move exposed the other half of the same defect. Applicability is a
*function* of the pairing tuple, and every existing restore call site fires
either before the tuple is knowable (startup: no craft selected, no §3.16
feedback yet) or on an event unrelated to it (§10.5 unlatch, calibration exit).
A valid artifact loaded at boot would therefore never have been applied at all.
The service loop now re-resolves when the tuple moves — craft selection, first
feedback, post-CSA channel — and never while the calibrator owns the actuator.
The startup log distinguishes "awaiting craft" from "STALE": at boot the pairing
cannot be evaluated, and reporting that as a mismatch sends people hunting for
one that does not exist.

**Method note.** Six defects were self-found during Pass 125, every one by
*doing the next step* rather than by inspection — which is why review was
expected to find little. It found three CRITICALs, because the reviewer executed
probe programs against the state machines instead of reading them. For state
machines, reproduction beats reading, and self-testing stops at the cases the
author already thought of.

## Pass 127 — §3.16 keys off the announced token, not a configured secret (2026-08-02)

Found on the bench, before a single §10.7 run: **the whole feature was
inoperable on the fleet's actual configuration, and failed silently.**

Neither the craft (`.2.232`) nor the ground (`.2.199`) configures `csa.psk`.
That is not an oversight — it selects **announced mode** (§11.4a): the per-boot
token is both the CSA key and the advertised ANNOUNCE payload, and the ground
caches it off the air (Pass 63). Measured on the live pair: the craft
advertises `psk_present: true` and the ground's discovery catalog holds its
token.

Pass 125 keyed §3.16 off `l.cfg.policy.csa.psk` at startup on both ends:

- craft — `set_quality_identity()` called once with the empty config secret, so
  `UplinkQualityCounters::build()` hit its `psk.empty()` guard and returned
  `nullopt`. **UPLINK_QUALITY was never transmitted at all.**
- ground — `UplinkQualityGate` constructed with the empty secret, so `accept()`
  returned on its first line. **Every packet refused.**

The only operator-visible symptom is the §10.7 start prerequisite reporting "no
fresh authenticated §3.16 feedback" forever, which reads as a radio or pairing
problem rather than a key-provenance bug. The spec earned this: §10.7 said "a
configured `csa_psk`", so the implementation matched the spec and the *spec* had
the gap. Operator ruling: the PSK is shared over RF at pairing and must not
require a committed static secret.

Both ends now resolve the key continuously by §11.4a provenance — configured
secret if present, else the live announced token — the craft per emission, the
ground per accepted packet keyed on the *selected* craft. Continuous rather than
latched because the token is per-craft and the Pass 113 pairing gate re-keys it
at runtime; the same reason `/csa` and §11.7 already resolve per call rather
than caching. A key change drops the gate's counter baseline: it is a different
authenticated peer, and telescoping a §10.7 delta across two key epochs would
divide by a number that does not mean what it says.

The start prerequisite changed from "is a secret configured" to "is a key
resolvable", and its two failure messages now name which mode the operator is
in — "announced token not heard yet" is a different problem from "csa_psk is not
configured", and conflating them is what would send someone editing a config
that was already correct.

**Trust note, stated rather than assumed.** The announced token is public by
construction — it is broadcast so a spectator can pair. So in announced mode
§3.16 authenticates provenance against a *passive* third party, not against an
attacker who has heard the ANNOUNCE. This inherits announced mode's existing
trust model rather than widening it: the same is already true of the CSA
campaign and every VEHICLE_CMD, both of which move considerably more than one
node's own TX power. The residual §10.7 exposure is bounded by construction — a
forged quality packet can only mis-place the **ground's own uplink power**
inside `[min_qdb, max_qdb]`, only while an operator-initiated run is in flight,
and never above the adapter's §10.3 `max_power_qdb` ceiling. Deployments needing
spoof resistance configure `csa_psk` and get secret mode, unchanged. §13 threat
row and §10.7 amended.

**Method note.** Pass 126 fixed thirteen defects across three repos and re-ran
every automated gate green. This one was invisible to all of it: every test
supplies a PSK, because a test that wants to exercise the codec has to. The
defect lives exactly where the test fixtures stop — in what the *deployment*
omits. It surfaced in the first five minutes of reading real device configs,
which is an argument for reading the target's config before writing the harness,
not after.

## Pass 128 — blackout dwells score measured loss, not a flat 1000permille (2026-08-02)

Found by the **first live §10.7 run**, which failed on a link that was carrying
traffic without difficulty. The dwell trace (added in the same pass, because
without it the failure was just the word `verify_failed`):

```
dwell#1 probe  qdb=4   emitted=41/40   received=41   loss=0permille    rssi=-40
dwell#2 probe  qdb=20  emitted=41/40   received=41   loss=0permille    rssi=-40
dwell#3 probe  qdb=20  emitted=41/40   received=41   loss=0permille    rssi=-39
dwell#4 VERIFY qdb=4   emitted=199/200 received=198  loss=1000permille rssi=-66 [BLACKOUT]
```

Dwell #4 received **198 of 199** — roughly 5permille, clean by any reading —
and was scored 1000permille, which failed the verify and (correctly, per Pass
126's C2 rule) failed the whole run.

The cause is a defect in Pass 126's own C1 fix. C1 generalised the blackout
fallback from "`received == 0`" to "`emitted` fell short of target", which is
right, but kept the flat 1000permille score, which is not. `last_report_epoch`
advances only for reports the craft **accepted**, so on any lossy link the
craft's anchor lags the ground's local clock by exactly the lost count — and the
local clock therefore reaches the target *first*, on a healthy dwell. The
fallback then fires by one epoch and calls a 5permille dwell a total blackout.

Fixed by scoring the fallback against the **local span** as denominator rather
than a constant. That degrades correctly at both ends: a total blackout still
has `received == 0` and still scores 1000permille — the seek's floor evidence,
unchanged — while a one-epoch lag scores the loss that actually occurred. RSSI
likewise comes from the samples that did arrive whenever any did, instead of the
synthetic guard value.

**What the run also showed, and is not a defect.** The cap wall fired at
20 qdb: a commanded +4 dB moved the craft's received RSSI by under 1 dB
(-40 → -39), so the seek retreated to the last clean probe at 4 qdb, exactly as
Pass 121 addendum 3 specifies. On a ~1 m bench that is the honest reading —
delivered power is not tracking commanded power at the bottom of this adapter's
range — and it is worth carrying into P8, where the question is whether a
calibrated placement beats driver-auto at useful range.

**Method note.** Pass 126 and 127 both ran every automated gate green before
this. The defect needed three things a unit test did not have: a real craft
whose accept rate is under 100%, a 200-epoch verify dwell long enough for a
one-count lag to matter, and the observability to see `emitted=199/200` rather
than a bare failure string. The first two are bench facts; the third is the
lesson — the per-dwell trace was added to debug this and is now the campaign's
required per-run record, which is what it should have been from the start.

## Pass 129 — two operator rulings from the bench (2026-08-02)

Both raised as open questions after the first successful §10.7 hardware run;
both ruled the same way, that a result which cannot be used must not report
success.

**1. A failed artifact write fails the run.** With the calibration directory
absent the run logged `artifact write FAILED` and still reported
`state:"done"`, `fail_reason:null` — only `fingerprint: 0` betrayed it. The Hub
menu binds `@wblink_uplink_calib_state`, so the operator's OSD row read "done"
for a commissioning step that had persisted nothing and would lose its
placement at the next boot. This is the same false success the Pass 126 verify
rule removed, one layer further out, and worse for being invisible. §10.7 now
fails with `artifact_write_failed` and re-arms the restore edge so the actuator
returns to its pre-run owner rather than holding an unpersisted placement.

Note what this does NOT change: the measurement was valid and the placement was
applied for the session. The ruling is about what the operator is told, and the
premise of the feature — "persisted on both sides" — is what makes an
unpersisted run a failure rather than a partial success.

**2. The cap wall at the floor is handled the way §10.6 handles it: distance.**
The first live run placed at `min_qdb` because the cap wall fired on the first
step off the floor — a commanded +4 dB moved the craft's RSSI by under 1 dB at
roughly 1 m. That is not a §10.7 defect; it is the Pass 121 cap wall correctly
reporting that delivered power is not tracking commanded. §10.6 already answers
this procedurally — Pass 121 collapsed the requirement to **near-bench 2–10 m**
precisely "so the upper rungs' overload ceilings sit above the cap wall" — and
§10.7 inherits it.

What §10.7 adds is that the condition is now legible. A cap wall that lands the
placement ON the floor means no power was ever shown to deliver more than the
minimum, so "maximum deliverable clean power" was never measured. Persisting
that would auto-apply a floor placement at every subsequent boot on the strength
of a measurement that never happened. It fails with `cap_wall_at_floor`, whose
remedy is in its name. A cap wall above the floor is a real placement and
succeeds unchanged — the rule is about the floor case only.

**Method note.** Neither of these was reachable from a unit test, and neither is
a coding error: the first is a question about what an operator is entitled to
infer from a state field, the second about what a measurement means when the
bench geometry is wrong. Both surfaced within minutes of the first successful
hardware run, and both were operator rulings rather than implementer choices —
which is the process working as intended rather than a gap in it.

## Pass 130 — one monotone sweep; the cap wall is deleted (2026-08-02)

Operator ruling, after asking the right question about §10.7's first bench
failure: *"the whole idea like we did with craft calibration is to find the
highest clean tx power. why cap it. why do we care about last seed, it may be
wrong or seeded for a different adapter. this whole solution seems overly
complicated for what is essentially a max tx power probe with persist."*

All three parts were correct, and the evidence was already in hand.

**The cap wall was costing link budget, not protecting it.** Its premise: a
silently-capped radio makes the recorded qdb fiction. But commanding `max_qdb`
into a capped radio radiates the cap, so the placement behaves identically — the
stop buys nothing. Meanwhile a plateau that is *not* a cap costs real headroom,
and both directions were losing it:

| run | limiter | cost |
|---|---|---|
| craft P0a, rung 4 | cap wall | 4 dB discarded, loss 5permille |
| craft P0a, rung 6 | cap wall | 4 dB discarded, loss 0permille |
| craft P0a, rung 7 | cap wall | **12 dB discarded**, loss 1permille |
| ground §10.7 | cap wall | placed at `min_qdb`, **26 dB low** |

Rungs 4/6/7 of the craft's own bench-validated artifact were limited by an
RSSI-plateau heuristic with no loss wall in sight. The loss wall and
`rssi_guard_dbm` are the real limiters; they remain.

**Seed dependence was a measurement error, not an optimisation.** Rungs 1-7
seeded one step below the previous placement, so the result depended on where
the sweep began — and it misplaced: against the reference channel model the
descent grid stepped straight past a clean 68 qdb and landed on 60. Every rung
now sweeps from `min_qdb`. Slower, and a property of the channel alone.

**With no descent, three mechanisms have nothing left to decide.** The Pass 125
floor rule is withdrawn: a bad probe below the first clean one is "too cold" and
the sweep simply climbs. The addendum-4 blackout retreat and addendum-5 verify
step-down fall out of treating a blacked-out dwell as "not clean" — above a
clean probe it places at it, during verify it steps down, below the first clean
probe it climbs. One new guard was needed: a rung may never *complete* on dwells
that carried no reports, because a placement authored from silence is the
report-loss abort, not a result.

The sweep also fixed a bracket defect the seek had hidden: only a bad probe
**above** a clean one now books `first_bad_rssi`. It describes where the link
breaks from being too *hot*, and a sweep that starts in a dead zone was
recording the cold bottom of the range as an overload ceiling.

`PowerSeek` drops from 138 to 96 non-comment lines. Deleted outright:
`cap_rise_db` (configs carrying it still load, key ignored), `cap_confirm_`,
`capped_`, `floor_ascend_`, `blackout_used_`, `on_blackout()`, the descend
branch, and with them Pass 129's `cap_wall_at_floor` — which existed only to
refuse a result the cap wall should never have produced.

**Method note.** This is the second §10.7 carve-out from `PowerSeek` in two
passes (C2's verify semantics, then the cap gate), and the engine had already
been bent once *for* §10.7 (the Pass 125 floor rule changed shipped craft
behaviour). Three carve-outs and a bend is not a shared algorithm — it is two
algorithms wearing one type, and the extraction I justified as "so the two do
not grow differently-buggy copies" had instead grown one copy with two
personalities. The right unification was not a richer engine but a poorer one:
both directions genuinely are the same monotone sweep, which is what the
operator said at the outset.

## Pass 131 — eight rungs, no MAC, and a sample-rate budget (2026-08-02)

Operator scope for the ground uplink, given as three instructions: build the
eight-rung equivalent of §10.6, delete the §3.16 authentication before adding
anything, and expose it as one Hub action. Each of the three landed on a
different kind of question.

**1. §3.16 drops the MAC. 35 → 31 bytes.**

The operator's argument: LINK_REPORT is unauthenticated and §10.6 moves the
craft's power actuator on it, so authenticating the feedback that moves the
*ground's* power actuator — and nothing else — was asymmetry with no threat
behind it.

Reading the deployed configs made it stronger. Neither `.2.232` nor `.2.199`
sets `csa.psk`, which selects announced mode, where the §11.4a key is the
per-boot token **broadcast so a spectator can pair**. Pass 127's own trust note
already conceded that this authenticates provenance against a passive third
party, not against anyone who has heard an ANNOUNCE. So on the fleet's actual
configuration the MAC bought very little — while costing a per-emission and
per-accept key resolver (Pass 127), a counter-domain resync gate, and a
backward-threshold detector that wedged at 519 consecutive rejects (Pass 126).
Three of the most defect-dense mechanisms in the feature existed to protect a
secret that is published.

What the MAC *did* carry, and what nearly went out with it: §10.7's authority
rule. Pass 125 inferred the report latch from the packet — a valid-MAC packet
naming a ground's own `(originator, session)` was proof that ground held the
latch. That inference dies with the MAC. It needed no replacement wire, because
§3.15a already ships `report_latch_holder` and the ground already parses it into
`link.report_latch_holder`. The start prerequisite now reads the latch instead of
deducing it, which is also better evidence: `report_latch_holder` is the gate's
own state, whereas the inference was a property of a packet.

Two fields stay. `craft_adapter_fingerprint` is half the artifact pairing
identity in `uplink_calib_matches()`, and dropping it re-opens the D3 defect
Pass 126 fixed. `last_rx_mcs` stops being observability the moment the sweep
commands a rung per rung: it is the only evidence the radio honoured the
command, on a fleet with a history of chips that do not.

The de-authentication also *simplifies* a rule rather than weakening it. A
backward cumulative counter was ambiguous — reset or replay — and resolving it
needed the sustained-backward heuristic that wedged. With no replay attacker to
distinguish from, backward means reset, and the gate rebaselines on the spot.

**2. Eight rungs, not one.** Pass 125 calibrated the single configured uplink
rung and deferred the rest. That was the wrong unit of work: the sweep machinery
is per-rung either way, `placements` was already a list, and a one-entry artifact
meant the shadow-the-downlink policy could not be enabled later without re-running
commissioning at every site. The list allowance Pass 125 reserved is now spent
and the schema is unchanged.

Two consequences worth naming. The run now moves the uplink **rate** as well as
its power, so there is a second borrowed actuator and a run that exits at rung 5
must not leave the uplink on MCS5 — the same stranded-actuator hazard Passes 125
and 126 hit three times on power alone, at triple the surface. All three
borrowed actuators return on one edge, rate first. And a failed run persists
nothing rather than a truncated artifact, which would otherwise auto-apply at the
next boot looking complete.

The blackout question turned out to be already answered. Probing high rungs at
low power kills the return path for whole dwells, which sounded like it would
pollute the craft's §9.2 rung lockouts. It cannot: §9.2 excludes stale-report
transitions from strikes, and the loss values *inside* a LINK_REPORT are the
ground's measurement of the downlink, which the ground's own TX power does not
affect. The craft parks at its safe floor for the dwell and climbs back.

**3. The 120 s budget could not be met by the method as specified, and the
arithmetic said so before the bench did.**

Eight rungs is `8 × (8 × 40 + 200) = 4160` report epochs. At the §7.3 seed
cadence of 10 Hz that is **473 s**. The operator's instinct — "why would an
8-rung probe take 600 s, adjust the method" — was right that the method had to
change, but the obvious adjustment is the wrong one: the epoch counts are
already at their resolution floor. At 40 samples one lost report is 25‰, landing
*between* `loss_ok_milli` and `loss_bad_milli`, which is exactly why the one-shot
extension to 80 exists; at 20 samples it is 50‰ and a single unlucky report
fails a power the link was carrying. Cutting the gates buys speed by making the
measurement wrong.

Run time is samples ÷ rate, so the rate is the only lever that does not degrade
the result — which is what §10.7 already said: *"Counts are sample gates, not
timers."* For the duration of a run the ground raises its own report cadence to
60 Hz and drops `report_redundancy` to 1; `settle_ms` seeds down 800 → 300,
since its report-window term falls from 100 ms to ~17 ms. That is ~11.4 s per
rung, ~91 s for the uplink, ~118 s for the downlink — ~3.5 min for one Hub press.

**The ceiling is fps, and the reason is a trap.** §7.2 anchors LINK_REPORT on the
craft's post-EOB quiet gap and one gap opens per video *frame*. A cadence above
the frame rate does not fail loudly — §7.2 degrades the excess to §7.1
opportunistic return, which has no timing contract and a different delivery
probability. §10.7 would measure that transport difference as **uplink loss** and
place TX power against it. So `uplink_calib_report_hz` MUST NOT exceed the
craft's committed fps, and the operator's follow-up suggestion — cut the vehicle
bitrate to go faster — does not move this ceiling either: gaps open per frame,
not per byte. Lower bitrate widens each gap, which is worth having for
reliability (the §7.2 crossover), but it does not buy a single report per second.

`hard_cap_ms` is left alone. 600 s bounds a 91 s run with over 6× margin, and a
wedged dwell was never what a cap protects against — that was Pass 126's C1
defect, fixed by the local-epoch fallback. The plan initially proposed a per-rung
cap and a new config key; the arithmetic removed the need for both.

**Method note.** Passes 126–128 each found defects that unit tests could not
reach, and the lesson recorded there was "reproduction beats reading". This pass
adds a cheaper one: three of its four findings came from *arithmetic on the
spec's own seeds* — 473 s, 25‰ at 40 samples, one gap per frame — done before
any code was written. The blackout/lockout question and the report-rate ceiling
were both answered by reading §9.2 and §7.2 against the proposed change rather
than by running it. Cross-referencing a plan against the sections it borrows
from is not the same activity as reading the code it will modify, and it caught
the one design error (outrunning the gap rate) that would have produced a
plausible, wrong, and thoroughly green result.

A paste defect was cleared on the way: PROTOCOL.md carried §10.7 twice since
`d7e58c9`, the first copy terminating in a stray re-paste of §10.6's own
Observability and Forward-validity paragraphs.

## Pass 132 — the ground counts its own probes (2026-08-02)

Operator ruling, and a correction to how Pass 131 was reasoned rather than to
what it decided: *"you are taking too many truths to be hard and unbendable.
instead ask: what is the easiest and most frictionless way to implement the
ground uplink tx probe. THEN we can consider the smallest changes we need to
the protocols and rules... a calibration is inherently when the craft is
stationary and we dont have to adhere to the normal rules in this period."*

That was right, and it located a design error three passes deep.

**The error.** Pass 125 chose to measure uplink loss on a stream whose rate the
ground did not control — ordinary periodic LINK_REPORTs — because §10.7 forbade
synthetic probe traffic. Every complicated thing in this feature descends from
that one choice:

| mechanism | exists to answer |
|---|---|
| `emitted = E_B - E_A` anchoring identity | how many did the ground send? |
| local-epoch blackout fallback (P126 C1) | ...when the craft's anchor freezes |
| measured-loss fallback scoring (P128) | ...without calling 5‰ a blackout |
| one-shot ambiguous extension | ...when 40 samples cannot resolve the walls |

Two of those four were themselves bench defects, found in Passes 126 and 128.
It is the most defect-dense part of the feature, and all of it answers a
question the ground could always have answered directly: `Reporter` commits a
`report_epoch` **only on successful injection** (§3.5), so the ground's own
epoch delta is exactly the frames the radio took. The denominator was sitting
in `reporter.h` the entire time, with a comment explaining why it was exact.

**The ruling.** A dwell is a counted burst. The ground emits N probes at the
dwell's `(rung, qdb)`, stops, waits `uplink_drain_ms` for the craft's counter to
settle, and divides by its own N. All four mechanisms above are withdrawn.
`last_report_epoch` stays on the wire as observability and stops being
arithmetic.

The drain is the whole of what the anchoring identity was buying: nothing is
scored until the burst is finished and the craft has had longer than one §3.16
period to report all of it, so a probe still in flight cannot straddle a
boundary. One `uint64_t` replaces a spec section.

**Burst size replaces the ambiguous extension.** Pass 125's 40-probe gate put
one lost report at 25‰ — between `loss_ok_milli` (15) and `loss_bad_milli` (50)
— so a single loss produced a reading that could not be made, and the extension
to 80 existed to re-run the dwell longer. With the ground choosing N, the fix is
to choose one big enough: at 100 probes one loss is 10‰ and five are 50‰, so
every reading is decidable first time. Config now enforces
`1000/N ≤ loss_ok_milli` rather than shipping a mechanism to recover from
violating it.

**What "extend the bounds slightly" actually cost.** One sentence: §10.7 no
longer forbids probe traffic. That prohibition was written for a craft in
flight; calibration is stationary, pre-flight and operator-initiated. Probes are
ordinary LINK_REPORTs with ordinary unique epochs, so the craft counts them with
code that already exists and the wire is untouched. They ride the existing §7.2
return path and fill the quiet gap the craft already opens per video frame —
they never open a new one, so §7.2's guard-cost law is unchanged.

**And it withdraws Pass 131's third ruling entirely.** That ruling raised the
ground's report cadence to 60 Hz for the duration of a run and dropped
`report_redundancy` to 1, on the argument that "run time is samples ÷ rate, so
the rate is the only lever". The arithmetic was right and the framing was wrong:
it treated the *cadence* as the thing to change because it had accepted that the
ground could not choose the *sample*. Withdrawing it also withdraws its costs —
a shortened §9 loss window that Pass 110's reasoning said would make the craft's
selector go quiet, a redundancy path with no gap left to land in, and a config
key (`uplink_calib_report_hz`) whose ceiling could not be enforced by config
because it depended on the craft's fps. `policy.report_hz`,
`return.report_redundancy` and the §9 selector are now untouched by a run.

`settle_ms` 800 → 300 survives from Pass 131 on its own merit: the
report-window term it carried was for a dwell waiting on the cadence to produce
a sample, and a burst starts the instant settle ends.

**Method note.** Pass 131's method note congratulated itself for finding three
defects by doing arithmetic on the spec's own seeds before writing code. It did
— and it still missed this, because arithmetic on a premise cannot question the
premise. The thing that found it was a question about *friction*: what is the
easiest way to do this, asked before what the rules currently permit. Every
constraint I had treated as fixed (§7.2's gap cadence, the no-probe-traffic
rule, the 2 Hz feedback) turned out to be either irrelevant to a stationary
bench or cheaper to amend than to work around. Three passes of increasingly
careful work inside a bad frame is worse than one question about the frame.

## Pass 133 — sweep the whole range, and confirm the first clean probe (2026-08-02)

Two operator rulings from the 10 m campaign, the second overturning the first
answer I proposed.

**1. A bad probe above a clean one no longer ends the sweep.** §10.7's first
10 m run failed at rung 6, and I read that as "MCS6 is unreachable at this
geometry" and proposed capping the sweep there and persisting rungs 0-5. The
operator's response — *"the full probe need to be run!! of course it will fail
at 1db on 10m! you need to go all the way up"* — was correct, and the trace
proves it:

```
rung 6 probe qdb=4  -> 0permille    rssi -77   (fluke clean)
rung 6 probe qdb=20 -> 210permille  rssi -77   (bad above clean -> PLACED)
rung 6 VERIFY qdb=4 -> 100permille              -> verify_failed
```

The rung ran **2 of 8 steps**. It never tried 17.0 dBm, where the adjacent rung
had just placed cleanly. "Unreachable" was never measured, and my proposed
ceiling would have persisted that non-measurement as a result — a worse failure
than the one it replaced, because it would have looked like data.

`PowerSeek` now books the overload bracket on a bad-above-clean probe and keeps
climbing; the placement is the highest clean probe over the FULL range. This is
the third mechanism Pass 130's logic removes rather than tunes: terminating at
the first wall assumed the clean reading below it was real, which at the bottom
of the range on a marginal link it frequently is not. Runtime becomes
predictable too — every rung is exactly 8 probes plus its verify.

Re-run at 10 m with the full sweep, all eight rungs place: 27.0 dBm at MCS0-2,
25.0 at MCS3, 17.0 at MCS4-5, **13.0 at MCS6-7** — the rung that had been
declared unreachable. Every rung from 3 up books a real overload bracket above
its placement, losses 0-15permille. 14 dB of backoff, monotone.

**2. A rung's first clean probe is re-run once before it may establish
`last_clean`.** The fluke above is why. Only the first: later clean probes climb
from an established clean point, and confirming every one would double the probe
count on a rung that sweeps clean to the top. This is a §10.7 policy on the
seek's INPUT, mirroring C2's policy on its output — `PowerSeek` itself stays a
pure monotone sweep.

**Method note.** I proposed the ceiling ruling with a table of six good
placements and one "unreachable" rung, which read as a well-evidenced finding.
It was six good placements and one rung that had been measured for two steps out
of eight. The tell was in the trace I had already printed — rung 5 placed at
17.0 dBm and rung 6 never probed above 5.0 — and I did not look because the
failure had a plausible physical story attached to it. A plausible mechanism is
not evidence that the measurement happened.

## Pass 134 — the sanity ceiling must bound the sweep (2026-08-02)

Two operator rulings, both triggered by a craft artifact that placed seven of
eight rungs at 27 dBm and cost the video link when applied.

**Ground truth measured first, because the whole question rests on it.** With
neither `power_map` nor an artifact, `resolve_power_qdb()` returns `nullopt`
and the controller issues **no power command at all** — the adapter stays on
`iw txpower auto`. Measured on the bench fleet:

| node | adapter | uncalibrated |
|---|---|---|
| ground uplink | 8812EU `wlx84fc1450bcde` | **19.00 dBm** |
| ground diversity | 8812CU `wlx40a5ef2f2308` | 25.00 dBm (RX-only) |
| craft `.2.232` | 8812EU on SigmaStar | HW per-rate TXAGC curve (its init logs exactly that; `iw` reads 0.00 because the out-of-tree driver has no `get_txpower`) |

So **uncalibrated is neither flat nor maximal** — `auto` is the vendor's
per-rate TXAGC table, which already backs off as modulation order rises. That
is the same shape §10.6 exists to measure. A good calibration improves on it; a
bad one replaces a safe taper with a flat commanded maximum.

And nothing else in the stack bounds it. Measured directly on the ground
uplink adapter, whose phy advertises 20.0 dBm max at 5805:

```
cmd 2000 mBm -> readback 20.00
cmd 2700 mBm -> readback 27.00
cmd 3000 mBm -> readback 30.00     accepted, no error
```

No regd clamp, no driver clamp. In our own code `max_power_qdb` was unset in
both deploy configs, and even when set it reached only `resolve_power_qdb()`
and the §10.7 artifact apply — `calib_params_from()` takes only
`CalibrationPolicy`, so **the adapter ceiling never reached either
calibrator**. Both swept to a flat `max_qdb` = 108 (27 dBm) on every rung.

**1. The §10.3 ceiling bounds the sweep, tapered per rung by §10.2.**
`effective_max_qdb = min(policy.calibration.max_qdb, adapter.max_power_qdb)`,
and `rung_ceiling_qdb[m] = effective_max_qdb + (tx_power_level[m] − 4) × 8`.
The mask is derived, not authored: the §9.3 table already carries the per-rung
power intent `{4,4,3,3,2,2,1,1}`, and both ends already agree on that table by
hash, so this costs no new config key and no new wire.

Stated honestly, because it bounds what the mask can claim: the level scale is
2 dB/step from a baseline of 4 with a floor at 0, so it expresses at most
**8 dB** of taper (27/27/25/25/23/23/21/21 at a 27 dBm ceiling), while the
10 m uplink measurement produced a **14 dB** spread. The mask is a backstop
against a mis-measurement driving a top rung to full power. The loss wall is
still the placement mechanism at every rung.

A second reason the ceiling matters, found while reading the craft's
`curve.txt` from the bad run: §10.6 stores the curve re-referenced to level 4
(`curve_qdb[m] = placement − (level[m] − 4) × 8`), so a 27 dBm MCS7 placement
was written as a **33 dBm** curve entry —

```
MCS0-3:  27.0 27.0 29.0 27.0
MCS4-7:  31.0 31.0 33.0 33.0
```

The transform *widens* the exposure. `resolve_power_qdb()` clamps last, so the
ceiling contains it — but only if a ceiling exists, which until now it did not.

**2. A run that found no wall anywhere fails and persists nothing.** The bad
artifact recorded `placement_loss_milli` `[5,2,3,2,0,0,0,0]`. Its cause was a
§7.2 flush regression (fixed in `f9a4f35`) that cut the report rate from 10 Hz
to ~4.8 Hz: §10.6 scores every dwell from LINK_REPORTs, and a starved return
path fails in the most dangerous direction — fewer reports, fewer observed
losses, every probe reads clean, every rung places at the ceiling.

Two layers, because the persist-time rule alone only pattern-matches the
symptom: feedback health becomes a **start precondition** (distinct from the
3 s report-loss abort, which catches silence — a stream at half rate is never
silent), and persistence **refuses** an artifact where every rung placed at its
ceiling with `first_bad_qdb == null` throughout. §10.7 already refuses a result
authored from silence; this is the same rule for one authored from false
cleanliness.

**The continuous half of that check was withdrawn during implementation, and
the reason is Pass 133.** I specced health as "a precondition *and* a
continuous check", implemented it as a whole-run accepted-report rate floor,
and three existing tests went red — `test_blackout_retreat`,
`test_verify_blackout_descent`, `test_floor_ascend`. They were right and the
spec was wrong. Since Pass 133 the sweep no longer stops at the first wall, so
every rung climbs through the entire blacked-out region above its own overload
point; on a marginal link that is most of a run's wall time, **by design**. A
rate floor evaluated during a sweep is therefore indistinguishable from wall
evidence (Pass 121 addendum 4) and fails exactly the runs that are measuring
correctly. Report health is a property of the link **at rest**: once the sweep
starts, a low report rate is a measurement, not a fault. The precondition is
measured over a closed 4 s window of ordinary operation and rejected as a
§11.7 REJECT alongside the existing actuator and latched-reporter conditions —
"latched" was never the same question as "latched and keeping up".

A non-monotone placement curve is **surfaced, not refused** — the PA shape is a
physical expectation, not a protocol invariant, and at close range a flat curve
is a legitimate reading.

**Method note.** The premise I nearly shipped was that the ground's 27 dBm
placements were unreachable because its phy advertises a 20 dBm limit at 5805.
Two `iw` commands showed the driver accepts 30 dBm without complaint. The
advertised regulatory limit and what the actuator will do are different
questions, and only one of them was measured.

### Pass 134 addendum — restore with no curve hands power to the backend

Device-verified on the first §10.6 run under the new guards (craft `.2.232`,
bench range). The refusal worked exactly as specified — eight rungs swept in
~80 s, `no_wall_found`, nothing persisted, video untouched at 2‰ — and then:

```
calibrate: restore has no power authority (no curve, no override)
           — TX power left at the last probe value
calibrate: failed reason=no_wall_found
→ iw dev wlan0: txpower 15.00 dBm     (60 qdb = rung 7's mask ceiling)
```

§10.6 R4's restore order is "power first, then the boot selector window, then
the §10.4 resolve re-places the committed rung". On a node with **no curve and
no §10.5 override** that order has an undefined leaf: there is no resolve to
re-place with, so power was left wherever the last probe put it. Before Pass
134 the leaf was near-unreachable — a run that got far enough to move power
almost always ended by installing a curve. The `no_wall_found` refusal makes a
first-ever run end in *failure* routinely, so the leaf became the common path
on exactly the runs the refusal exists to catch.

This is not a new ruling. §10.5 already defines the same condition — release
power authority with no curve loaded — and its answer on kernel-monitor is
`txpower auto`, "after which the curve resolve resumes if a curve is loaded".
§10.6's restore now uses it. The remaining leaf is a backend with no auto
actuator at all (udp/dev), which is logged honestly rather than described as a
restore.

Worth stating for the operational picture, because it was measured the same
hour: `iw txpower auto` reports **19.00 dBm** on the ground's 8812EU and
**27.00 dBm** on the craft's. That number is a *cap* the driver will not
exceed, not evidence of a per-rate taper below it — an earlier claim in this
campaign that uncalibrated craft power is "the vendor's per-rate TXAGC curve"
was inferred from an init-script log line, not measured, and `iw` cannot see
below the cap. So an uncalibrated craft sits under a 27 dBm ceiling, and
`max_power_qdb` does **not** lower it: with no curve and no artifact the
controller issues no power command at all, so §10.3 binds the sweep and clamps
applied placements but is not an unconditional PA limit. Closing that would be
a behaviour change §10.3 does not currently authorise, and is left open.

### Pass 134 addendum 2 — §10.7 needs the refusal too, and my reason it didn't was wrong

The Pass 134 spec asserted:

> §10.6's flat-at-ceiling refusal has no §10.7 analogue and needs none — a
> ground that reads clean at every power on every rung has a dead §3.16
> counter stream, which the liveness expiry already catches as `quality_lost`.

A combined `start_both` at bench range falsified it in one run. The §3.16
counter stream was healthy the whole time — verify dwells returned real
0–10‰ values on every rung — and the run still produced:

```
rung mcs   gi   place    dBm  loss‰  rssi  first_bad
   0   0  LGI      84   21.0      5   -20       None
   1   1  LGI      84   21.0      5   -20       None
   2   2  SGI      76   19.0      0   -22       None
   3   3  SGI      76   19.0      5   -22       None
   4   4  SGI      68   17.0      0   -24       None
   5   5  SGI      68   17.0     10   -24       None
   6   6  SGI      60   15.0      5   -26       None
   7   7  SGI      60   15.0      5   -26       None
```

That is the §10.3 mask (`max_power_qdb` 84, levels {4,4,3,3,2,2,1,1}) read
back verbatim. It reached `done`, persisted as `fp=164`, and replaced the good
10 m `fp=110`. The craft half of the same run correctly refused with
`no_wall_found` — so the two directions disagreed about the identical
condition, on the same link, in the same 5 minutes.

The error was conflating two different causes of "clean everywhere". A dead
feedback path produces it, and the liveness expiry does catch that. **Close
range also produces it**, with a perfectly live feedback path, because no wall
exists to find — a property of the geometry. §10.7 now carries the same rule
as §10.6.

Worth stating plainly: the argument I used to exempt §10.7 was reasoning about
what *must* be true, on the direction I had just finished calling the stronger
case for the ceiling. The run that disproved it cost five minutes.

Two secondary corrections from the same session, both recorded so they are not
re-derived: the uplink test rig had a single flat `ceil_rssi` and therefore
modelled a channel with no wall on any rung — now a per-rung array mirroring
§10.6's rig, since "no wall anywhere" is the refused shape and can no longer be
the default fixture. And a rung's placement is the highest **grid** step
(`min_qdb + 16k`) that stayed clean, not the ceiling itself, so expected
placements do not equal `rung_ceiling_qdb` except where the grid happens to
land on it.
