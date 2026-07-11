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
