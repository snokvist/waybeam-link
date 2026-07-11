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

## Open questions for the next pass

- [ ] **Ruling 3 is FIXED, not revisitable** — vehicle is permanently single-adapter;
      diversity is ground-RX-only. Craft return path is best-effort by physics; the
      quiet-gap fit + floor-oscillation damping are **resolved empirically at gate 4**,
      not designed further on paper.
- [ ] Run the four bench gates (gate 1 now RX-proven; gate 4 return-window-fit is the one
      that gates the craft return path under single-adapter).
