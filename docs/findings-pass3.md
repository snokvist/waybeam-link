# Pass 3 findings — adversarial review + latency-first redesign

**Date:** 2026-07-10 · **Status:** proposal awaiting operator sign-off. Nothing in
this document has been written into `PROTOCOL.md` yet. On approval, the accepted
items fold into the spec as one revision commit.

## Method

Five investigators, grounded in verified source (not recall):
- **Prior-art** — Minstrel-HT, RFC 8681 RLC / RFC 9407 Tetrys, 802.11 CSA,
  wfb_ng/OpenHD/RubyFPV, deterministic-feedback prior art (PCF/HCCA, LoRaWAN, SRT).
- **Code-grounding** — devourer APIs, `docs/frequency-hopping.md`, TX/RX concurrency,
  radiotap knobs, RX metadata, our `waybeam_wfb_ng/vehicle/csa`.
- **Spec-audit** — line-by-line truth classification + collision map + defects.
- **Red-team (Opus)** — attacks the audit missed under the new semi-anarchy model.
- **Design-synth (Opus)** — concrete wire layouts, state machines, constants.

Full agent outputs archived in the session transcript. This doc is the arbitrated
result: what changes, why, and the concrete spec text.

## Operator decisions locked before this pass

1. Multi-RX ARQ default = **first-latcher lock**.
2. `originator` = **config-assigned stable node ID**.
3. FEC = **evaluate SWFEC + prove deterministic latency, then decide** (bench-gated).
4. Deliverable = **findings doc first**, then fold into PROTOCOL.md on approval.

---

## Part A — Rulings on the 8 audit BLOCKERs

| # | Blocker | Ruling |
|---|---|---|
| 1 | `demote_per_milli` 80‰ was a **pre-FEC** threshold | **RE-DERIVE.** waybeam-link has no FEC, so react on *post-diversity* loss (`loss_pre_recov`); seed **~20‰ (2%)** as a placeholder, bench-tune. Not 80‰. |
| 2 | §1/§11 single-RX invariant falsified by semi-anarchy | **REWRITE.** "Single merged NACK generator **per RX node**" stays true; **multiple RX nodes on-air are now first-class**, each with its own `originator`; first-latcher lock arbitrates ARQ. |
| 3 | `originator` vs `session_id` undefined | **RESOLVED.** Three orthogonal IDs: `originator` u16 (stable, who sent it), `session_id` u32 (per-boot nonce, namespaces seq/block + anti-replay epoch), `destination` u16 (advisory). `originator` is *additive* to `session_id`. Dedup key → `(originator, session_id, stream_id, seq)`. |
| 4 | NACK/LINK_REPORT need two identities | **RESOLVED.** Common prefix = **sender**; control-packet body carries a **target descriptor** (`target_originator`/`target_session`/`target_stream_id`) = the stream being repaired/scored. Keep them as **dedicated packet types**, not stream_types under DATA (their bodies are structurally different and the core, not an I/O binding, must consume them). |
| 5 | CSA needs a reliability sub-protocol vs best-effort ethos | **ACCEPTED as a scoped exception.** CSA is rare + catastrophic, so it gets its own anti-replay + rendezvous machinery (Part C). This does **not** make the data path reliable. |
| 6 | §13.0 objective inverted vs latency-first | **REWRITE.** New objective: among points meeting *delivered* loss/latency, maximize robustness margin; airtime/energy is a subordinate tiebreak only (Part D). |
| 7 | DATA header never got `active_profile` | **FIX.** Added to the DATA header (Part B), plus `table_version`. |
| 8 | Profile-table mismatch undetectable | **FIX.** `table_version` u8 rides every DATA packet; mismatch → best-effort default profile + a raised stat, same fallback as unknown `stream_type`. |

---

## Part B — Header rework (concrete wire layout)

Resolves blockers 3, 4, 7, 8 and the `0xWB`/`report_epoch`-wrap/three-name majors.

**Common prefix — 11 bytes (all packet types):**

```
off size field           notes
0   2    magic       u16  0x5742 = ASCII "WB"  (0xWB was invalid hex)
2   1    ver_type    u8   hi nibble=version, lo nibble=type
3   2    originator  u16  sender node id (config-assigned, stable)
5   2    destination u16  advisory filter; 0x0000 = broadcast
7   4    session_id  u32  sender boot nonce
```

`ver_type` low nibble: `0x1 DATA · 0x2 NACK · 0x3 LINK_REPORT · 0x4 HEARTBEAT ·
0x5 CSA`. 5 of 16 slots used — no scarcity, the "collapse vs expand the nibble"
tension the audit flagged is moot.

**DATA — 26-byte header:**

```
11  1    stream_id       u8
12  1    stream_type     u8    profile selector (§3.4)
13  4    seq             u32
17  4    block_id        u32   1 block = 1 RTP frame
21  1    data_flags      u8    bit0 END_OF_BLOCK, bit1 ARQ, bit2 RETRANSMIT,
                                bit3 FEC_REPAIR, bit4-7 reserved
22  1    active_profile  u8    operating point TX is on   (§13.2.1 promise, delivered)
23  1    table_version   u8    profile-table generation   (blocker 8 fix)
24  2    payload_len     u16
26  var  payload               opaque RTP
```

26 B header on ~1450 B usable MPDU → **1424 B max payload**, ~1.8% overhead.

**NACK — 23-byte fixed + bitmap:**

```
11  2    target_originator u16
13  4    target_session    u32
17  1    target_stream_id  u8
18  4    base_seq          u32
22  1    bitmap_len        u8
23  var  bitmap                 SACK-style; bit i ⇒ base_seq+i missing
```

**LINK_REPORT — 39 bytes** (`report_epoch` widened u16→**u32**: u16 wrapped ~1.8 h
at 10 Hz mid-flight; u32 = 13.6 y):

```
11  2  target_originator u16      18  4  report_epoch  u32   (was u16)
13  4  target_session    u32      22  1  table_version u8
17  1  target_stream_id  u8       23  1  rssi_best     i8
       (0xFF = node-scope)        24  1  rssi_mean     i8
                                  25  2  loss_pre_recov u16  post-diversity ‰
                                  27  4  uniq          u32
                                  31  4  diversity     u32
                                  35  1  adapters      u8
                                  36  2  probe_per     u16   0xFFFF=no probe
                                  38  1  recommended_prof u8
```

**Naming fix:** TX power is `tx_power_qdb` (quarter-dB, matches
`SetTxPowerOffsetQdb`) in **every** file — kills the `tx_power_cap`/`_level`/`_qdb`
three-name drift.

**Fragmentation:** waybeam-link **does not fragment.** Invariant: each ingress
datagram fits one MPDU. Encoder RTP payloader configured `mtu = 1400` (24 B margin
under 1424). H.264/H.265 payloaders already fragment NALs to MTU → config assertion,
not new code. Closes the u16-`payload_len`/no-fragmentation gap.

---

## Part C — The deterministic-return arbitration (the pivotal ruling)

The two Opus passes disagreed. **Design-synth** proposed a TSF-anchored
frame-boundary quiet-gap contract as achievable. **Red-team** argued deterministic
catch is unbuildable and must be retired *before* a gap-scheduler is built.

**My ruling: they converge, and the resolved position is red-team-baseline with the
synth's mechanism as an opportunistic, bench-gated optimization — never load-bearing.**

Why the red-team is right that it can't be a *guarantee*:
- devourer injection is fire-and-forget; **TX cannot timestamp when its own packet
  left the air** (only aggregate `TxStats`). The LoRaWAN "fixed offset after MY
  transmission" precedent is therefore unimplementable as stated — TX has no anchor.
- TX cannot **reserve** airtime (no TDMA/NAV/RTS). Stopping URB submission ≠ quiet
  air; the FIFO tail drains for an unknown time. The gap is probabilistic.
- At 120 fps + saturated bitrate the idle gap shrinks below the ~2 ms turnaround the
  return needs → returns collide with video.
- Single-adapter craft often can't hear its own return: 8812EU has a TXAGC-register
  **RX-desense trap right after TX**, and J3 needs `enable_with_tx` before InitWrite.

Why the synth's mechanism is still worth keeping as an *optimization*:
- Its core insight is real: the EOB packet's **RX hardware `tsfl` coincides with the
  craft's real send-of-EOB** (propagation ~µs), so RX can anchor its reply to a
  physical instant it shares with the craft **without any clock crossing**. That is
  the one sound way to aim a reply at the craft's idle gap.
- At ≤90 fps with normal bitrates the idle gap comfortably fits the window, so the
  optimization measurably reduces return collisions where it matters least (but still
  helps ARQ close in deadline).

**Resolved design:**
1. **Baseline (must work standalone):** RX injects NACK/LINK_REPORT **opportunistically
   on CCA** via its designated-TX adapter — wfb_ng-proven best-effort. No timing
   contract. This is what the protocol ships on.
2. **Optimization (behind a bench gate):** RX *may* time its return to
   `rx_tsfl(EOB) + guard_us + return_window_us/2` to land in the craft's paced idle
   gap. TX *paces* to leave the gap when airtime allows; it is a hint, not a promise.
   Seeds: `guard_us=300`, `return_window_us=2000`. Enabled only if the bench shows it
   beats pure-opportunistic on return-delivery rate.
3. **Craft return-RX:** **recommend a dedicated 2nd RX adapter** on the craft (separate
   `IRtlDevice`, listens continuously, sidesteps the desense trap). Single-adapter
   craft is the degraded fallback.
4. **Graceful degradation (Tetrys/RubyFPV-style):** when `report_epoch` stops advancing
   for `report_timeout_ms≈500`, the adaptive layer fails **toward degradation** (hold,
   then step toward a safe floor — never fail optimistic) and the craft runs
   **vehicle-only heuristics** it owns without any return: last-heard return RSSI/CFO,
   `TxStats.last_was_timeout` (local FIFO backpressure), venc queue fill.

**Cadence (SRT-style):** NACK event-driven, coalesced one bitmap per return window,
rate-limited by per-seq hold-down. LINK_REPORT periodic 10 Hz floor + immediate on
step change (RSSI-floor breach, loss spike).

**The requirement "TX can catch the telemetry" is met as best-effort-with-a-real-
optimization, not as a hard guarantee.** State the crossover honestly in the spec.

---

## Part D — Latency/robustness-first link selection

**New §13.0 objective (replaces the inverted energy framing):**

> Among operating points whose **delivered** (post-diversity, post-ARQ) loss and
> latency meet the target, choose the one most likely to deliver the next frame
> intact within its deadline — maximum robustness margin. Airtime/energy is a
> subordinate tiebreak only among points statistically equivalent on robustness.

**Minstrel steal (lightweight):** one success-EWMA per rung (fed by `loss_pre_recov`).
The **max-probability rung** = highest recent delivery probability; under multi-rung
stress the cascade demotes **toward it, not blind min−1**. Reject all
A-MPDU/aggregation/sampling machinery (no MAC-ACK referent under injection). One
EWMA per rung is the entire new state footprint.

**wfb_ng cascade — disposition:**

| Element | Verdict | Reason |
|---|---|---|
| Rule cascade, first-match (not weighted score) | KEEP | Deterministic, debuggable, proven. |
| Reactive-demote 80‰ | RE-DERIVE | Pre-FEC value → ~20‰ delivered, bench. |
| RSSI-floor −85 dBm | KEEP | Hardware RF guard-rail, FEC-independent. |
| RSSI-fade (−10 dB/s, ≤−65 dBm, 3 ticks) | KEEP | Predictive, FEC-independent. |
| Backpressure escape (2.0 s) | KEEP | Anti-death-spiral, local venc signal. |
| V+2 probe as *separate stream* | DROP (v0) | Injection model has no wfb side-stream to probe. |
| RSSI-margin promote (+6 dB hyst + dwell) | KEEP | Self-contained; add active probe later only if promotes too timid. |
| §13.5 sequencing (bitrate leads demote / lags promote) | KEEP | Encoder-overshoot physics, FEC-independent. |
| `mcs_settle_s` 5.0 s | RE-DERIVE | No-FEC loss is spikier — bench. |
| Flap-freeze + reentry dwell | KEEP | Flap is *worse* without FEC (each flap = visible glitch). |
| `min==max` pin | KEEP | Bench / known-bad-link escape hatch. |
| 10 Hz report cadence | KEEP as floor | + event-driven immediate (Part C). |
| EWMA α 0.3/0.5 | RE-DERIVE | Tuned to FEC-smoothed loss; no-FEC may want faster. |
| MCS rungs 0–5, 6–7 | KEEP rungs, DROP the probe reservation | 6–7 become normal high rungs the RSSI-margin promote reaches. |
| Max-probability fallback rung | ADD | Minstrel steal. |

---

## Part E — Follow-me channel switch, hardened

Lift `waybeam_wfb_ng/vehicle/csa` (N=5 decrementing-dt copies → one absolute
T_switch; ARMED→VERIFY→COMMITTED; vehicle auto-revert), fold into the packet model as
dedicated type `0x5`, add what it lacks.

**CSA packet (type 0x5) — 30 bytes:**

```
11  4  csa_nonce      u32   campaign id (anti-replay)
15  1  csa_seq        u8    copy counter N..1 within campaign
16  2  target_chan    u16   center freq MHz (band-agnostic)
18  1  target_bw      u8    0=20,1=40,2=80
19  1  retune_class   u8    0=fast intra-band, 1=cross-band
20  2  dt_to_switch_ms u16  DECREMENTING across copies → one absolute T_switch
22  2  t_revert_ms    u16
24  2  prev_chan      u16   revert target
26  1  prev_bw        u8
27  2  home_chan      u16   fleet rendezvous freq
29  1  power_intent   u8    profile power level to ReApply post-switch
```

**Constants:** N=5 copies @20 ms (100 ms campaign, bench-validated). Follower anchors
`target_tsf = rx_tsfl + dt·1000` (**TSF, not host clock** — fixes the ~10 ms host-
jitter smear the red-team found; re-derive the bench's 0–5 ms precision against TSF).
`dt1` (first copy) ≥ campaign span + max retune + margin: **class 0 fast intra-band
150 ms**, **class 1 cross-band 500 ms** (8812AU full path ~277 ms).

**Mandatory paired `ReApplyTxPower()`** on entering COMMITTED after any retune —
`FastRetune` skips TXAGC re-apply, so power drifts stale otherwise.

**Adaptive-layer CSA freeze:** on ARMED, freeze the §13 cascade for
`csa_settle_s=3.0 s` **and pause the report-epoch watchdog** — the retune blackout +
re-acquire silence would otherwise trip a spurious demote AND the fail-safe step-down.

**Anti-replay (no full auth):** accept a CSA only if `session_id` == current session
for that originator (kills cross-reboot replay), `csa_nonce` not already applied, and
the issuer is the currently-latched command source. Plus a **target-channel allowlist**
and **rate-limit ≤1 switch / 5 s** to blunt a forged-switch flood.

**Two-tier straggler safety (fixes silent-strand):**
- **Short auto-revert:** switched but no valid traffic within `verify_timeout_ms=150`
  → revert to `prev_chan`.
- **Long rendezvous:** never saw the CSA, or lost link `rendezvous_timeout=5 s` after
  revert → retune to `home_chan` and listen; craft beacons `home_chan` when it detects
  it lost the fleet.
- **Implicit ACK:** followers resuming LINK_REPORT/NACK on the new channel *is* the
  ack; craft seeing no returns for `csa_ack_timeout=1000 ms` post-switch auto-reverts.
  Reuses the return channel — no new wire type.

**Intra-process atomic switch:** if the RX *process* accepted the CSA (any adapter
heard it), it fires all local `FastRetune` calls at T_switch — a straggler adapter
follows because a sibling heard it. Kills in-process split-brain.

---

## Part F — SWFEC candidate (designed, decision deferred to bench)

**Critical red-team correction folded in:** the design-synth's default GF(2)/XOR-only
scheme recovers **one loss per window** — exactly the isolated-loss case diversity
already handles best — and **fails on the 5–30 ms correlated burst that motivates FEC
at all** (a burst loses a contiguous run of ~8–48 packets; XOR-1 recovers 1). So:

**The real decision is binary — pure no-FEC (diversity + concealment + short GOP) vs a
proper GF(256) RLC/Tetrys scheme (n−k ≥ burst length).** Drop the XOR middle option:
it spends airtime on the case that needs no help and dies on the case that does.

**Gate the binary choice on measured cross-adapter loss correlation ρ** (from the
`diversity`/`adapters` counters) — the single load-bearing unmeasured premise. If ρ is
low, diversity carries the correlated fade and no-FEC stands; if ρ→1 (co-located ground
antennas in a null; airframe occlusion), diversity degrades to single-adapter loss,
ARQ self-throttles exactly when needed, and only forward parity sized to the burst
helps → GF(256) RLC becomes justified.

**If FEC is adopted it must be budgeted, not bolted on:** add `fec_overhead_frac` per
profile (§13.3); parity is **live-priority** (only useful in-deadline), so debit it
straight from encoder bitrate:
`bitrate_budget_eff = capacity(profile) × (1 − fec_overhead_frac − arq_reserve_frac)`,
folded into the §13.5 atomic transition (which then co-varies a third quantity).
No license-clean embedded C impl exists (OpenFEC sliding-window is proprietary; swif-
codec research-grade) → a GF(256) RLC would be hand-rolled.

**Go/no-go vs ARQ (bench together — they share the pivotal input, return reliability):**
- **SWFEC wins** when the return channel is unreliable/high-RTT (ARQ can't close in
  deadline) AND losses are small-and-frequent.
- **ARQ wins** when returns are reliable/low-RTT AND losses are bursty-but-rare
  (don't pay always-on parity).
- Neither wins the correlated-all-adapter SNR-edge fade (both ride the faded channel).

---

## Part G — No-auth injection hardening (red-team, near-zero cost)

A small common defense neutralizes most of the no-auth injection attack class:

1. **Plausible-forward-window clamp** — RX rejects any `seq`/`block_id`/NACK `base_seq`
   that jumps > K ahead of the current cursor (K ≈ a few blocks / one resend-ring).
   Kills in one check: forged high-`block_id` **video flush** (supersession abuse,
   HIGH), NACK-bitmap targeting garbage, discovery-cache jumps.
2. **Global-per-seq hold-down** — a resend for seq N suppresses re-resend for a window
   **keyed by seq globally** (not per-originator). Collapses **NACK amplification** (a
   2040-bit forged bitmap → still 1 resend) to factor 1. Mark load-bearing.
3. **Bitmap sanity clamp** — reject NACKs with popcount > one block's worth or
   `base_seq` outside the resend-ring window.
4. **Per-originator resend budget** — partition the airtime cap by originator so one
   flooder caps only its own share; the pilot's share is fenced.
5. **`preferred_originator` config int** — first-latcher lock *prefers the configured
   pilot ID* whenever it is NACKing, falling back to first-latcher only when the pilot
   is silent. Fixes the red-team's sharpest finding: **first-latcher structurally
   selects the WORST link** (the node with most loss NACKs first) — inverts that bias.
6. **Release lock only when contested** — `(silence ≥ T) AND (another originator is
   actively NACKing)`. Kills clean-stretch thrash and the "pilot in deep fade looks
   like silence → lock stolen" race.
7. **Monotonic wrap-aware discipline** on `csa_seq`, `csa_nonce`, `report_epoch`
   (now u32) so replayed stale control frames can't feed the watchdogs.
8. **Discovery admission control** — require N packets over T before a session enters
   the latch-picker; LRU-age the rest; resolve `stream_type` on sustained/recent
   traffic, not strict first-seen (stops one forged early packet pinning a
   misclassification). Caps the enumeration-flood memory blow-up.

**Scoped-auth exception (the one place no-auth costs more than it saves):** the
red-team rates **forged CSA = whole-fleet blackout from one ~80 B packet as CRITICAL**.
Anti-replay + allowlist + rate-limit + home-channel (Part E) are the floor; **a 4-byte
keyed MAC on the `csa_commit` only** (`trunc(HMAC(psk, csa_nonce‖fields), 4)`, operator
PSK) is proportionate — it authenticates one rare control action and **never touches
the data path**. Flagged for operator ruling below.

---

## Part H — I/O + observability layer (was absent)

**Binding model:** ≤1 shm (in XOR out), ≤1 unix socket (in XOR out), ≤4 UDP (each in
or out). **v0 = UDP only.** Each `stream_id` → exactly one binding, in *xor* out.
Control packets never touch a binding — the core consumes them. JSON config +
newline-delimited JSON stats schema (node/adapters/streams/policy; per-adapter
rx/dup/rssi/tx_submitted/tx_timeout; per-stream loss_pre_div vs loss_post_div,
recovered_arq/fec, dropped_superseded/deadline, decode_errors, active_profile,
table_version; link state/flap_freeze/csa_state/report_age_ms). Full schema drafted;
lands in the spec's new I/O section on approval.

---

## Part I — Editorial fixes (mechanical, fold silently)

- Delete stray code fence at `PROTOCOL.md:628` (breaks §14 rendering to EOF).
- Reconcile §12 build-order with `docs/build-order.md` (single source of truth).
- Fix review-log.md:10-11 miscite (§13.4 → §5.3/§13.3 airtime quota).
- Soften §11 "No FEC (deliberately)" → "decision deferred, bench-gated on ρ" and
  scope §13.3's argument to *adaptive block RS* (it does not bear on sliding-window).

---

## Bench gates (must pass before the dependent design is trusted)

1. **Two `IRtlDevice`s in one process** (libusb context sharing) — **UNKNOWN in
   devourer; blocks BOTH ground multi-RX diversity AND craft 2nd-RX-adapter return.**
   If it fails, both move to multi-process + IPC (design survives, topology changes).
   Add a per-adapter liveness watchdog: a stalled-but-counted adapter inflates
   `diversity`/`adapters` → TX rides phantom-diversity MCS/power → real loss spikes.
2. **Cross-adapter loss correlation ρ** — decides no-FEC vs GF(256) RLC (Part F).
3. **NACK→RETRANSMIT round-trip P90** vs I-frame deadline on the saturated uplink —
   decides whether ARQ is in-deadline at all.
4. **Return-window fit** — at target fps/bitrate, does the TSF-anchored quiet gap beat
   pure-opportunistic return delivery (Part C optimization go/no-go)?

---

## Operator rulings (locked 2026-07-10)

1. **4-byte MAC on CSA only — ACCEPTED.** `trunc(HMAC(psk, csa_nonce‖fields), 4)` on
   the CSA packet (type 0x5), operator PSK, off the data path. It rides in the 30-byte
   CSA layout (append `csa_mac` u32 → **34 bytes**). Anti-replay + allowlist +
   rate-limit + home-channel remain as defense-in-depth. No auth anywhere else.
2. **CSA issuer = GROUND leads.** Ground issues the switch (uplink command); craft +
   spectators follow; craft auto-reverts to `prev_chan` if it loses the command source
   (`csa_ack_timeout` with no fleet returns) and beacons `home_chan`. Revert/rendezvous
   ownership sits with the follower for short auto-revert, with the craft for the
   fleet-lost beacon. Ground never auto-reverts its own card unasked (matches
   `vehicle/csa`'s `--no-revert` ground discipline).
3. **Craft = SINGLE adapter, PERMANENT hardware constraint (not a tradeoff).** The
   vehicle will **never** carry a second adapter — multi-adapter diversity is a
   **ground-RX-only** capability. There is therefore **no robust-hardware option** for
   the craft return path; it is best-effort by physics and the design must make
   single-adapter-craft work as well as it can, in software. The craft timeshares one
   radio between TX-video (dominant) and RX-return: it can hear returns **only when its
   own radio is idle**, i.e. inside the paced quiet gap. Consequences that are now
   load-bearing, not optional: **(i)** the TSF quiet-gap (Part C) is the craft's
   **primary** return mechanism — it is both when the ground times its reply *and* when
   the craft can listen; gate 4 (return-window fit) is the pivotal validation. **(ii)**
   `guard_us` must cover **max(ground turnaround, craft TX→RX AGC/PLL settle)** — the
   8812EU RX-desense-after-TX trap means the craft is deaf for the front of its own gap.
   **(iii)** the `report_timeout` step-down/promote pair **must** be hysteretic + min-
   dwell-damped and observable, because return starvation is the *normal* high-duty state
   and an undamped loop oscillates at the floor (finding 3). No revisit trigger — this is
   fixed.
4. **Magic width = u16 `0x5742`** (guard strength over the 1 saved byte).

### Ripple from ruling 3 (single-adapter craft return)
- Part C baseline stands, but weight shifts: **vehicle-only heuristics must hold the
  link open through return-starved stretches**, not just brief feedback gaps. The
  `report_timeout_ms≈500` fail-safe and the conservative open-loop step-down are the
  primary safety net, not a backstop.
- The TSF-anchored quiet-gap (Part C optimization) is **promoted from nice-to-have to
  the craft's main return-catch mechanism** — its bench gate (gate 4) is now higher
  priority, since without it a single-adapter craft hears returns only by luck.
- CSA implicit-ACK (Part E) inherits the same weakness: the craft confirming a switch
  via returns on the new channel is now lossier → lean on the `verify_timeout` short
  auto-revert + `home_chan` rendezvous, don't assume the ACK arrives.

## Pass 3b — Fable adversarial review (2026-07-10)

A fifth-pass skeptic reviewed this document and **confirmed the 3-adapter fact against
the actual Android code**: `Waybeam-android/wifi/src/main/cpp/wifi_jni.cpp` runs N=3
adapters in one process with a **per-adapter `libusb_context` + per-adapter RX thread**,
funneled to one dedup buffer — but **RX-only, no member injects**. Architect
dispositions on its findings:

**Gate 1 rescoped (finding 1/2 — ACCEPT).** Gate 1 was mis-framed as "context sharing"
— the proven pattern needs *no* sharing (N independent contexts). Multi-adapter
single-process RX is **proven at N=3**. The real residual unknown is narrower: **one
injecting `IRtlDevice` + N monitoring siblings in one process** (ground designated-NACK-TX
adapter; craft injector+listener). Rewrite gate 1 accordingly. Consequence: the software
risk that justified avoiding a 2nd craft RX adapter is **gone** — ruling 3 is now a pure
SWaP tradeoff, and its "revisit if gate 1 forces multi-process" trigger is **dead** (gate 1
passed). Re-condition the revisit on **gate 4** (return-window fit) failing.

**Finding 3 (BLOCKER — return path incoherence + floor-oscillation) — ACCEPT; craft
single-adapter is a HARD constraint (no 2nd adapter, ever) → resolve empirically.** The
2nd-craft-adapter fix the reviewer implied is **off the table** — the vehicle is
permanently single-adapter (diversity is ground-RX-only), so there is no hardware escape
and the return path is best-effort by physics. The incoherence is resolved by wording:
the TSF quiet-gap is **not "never load-bearing"** for the craft — under the single-adapter
constraint it *is* the craft's primary return mechanism (it's both when the ground replies
and when the craft can listen). The floor-oscillation limit cycle (high duty → no returns
→ step to floor → duty drops → returns resume → promote → starve) is real; software
mitigations specced: **(a)** hysteresis + min-dwell on the step-down/promote pair, **(b)**
make it observable (finding 6 — `reports_expected/received`, `return_window_hits/misses`).
**Whether the quiet gap actually fits at target duty, and whether the damped loop holds a
stable operating point rather than oscillating, is deferred to the empirical bench (gate
4) — to be resolved and confirmed during device tests, not designed further on paper.**

**Finding 15 (forged LINK_REPORT — MAJOR, the biggest Part G gap) — ACCEPT.** A forged
optimistic report (RSSI −40/loss 0, epoch advancing) defeats §13.8's "never fail
optimistic" → sustained blackout from a 39 B packet, no PSK needed. Fix: accept
LINK_REPORTs **only from the latched/preferred `(originator, session_id)`**; add
plausibility cross-check (reported loss vs TX-observed NACK behavior; conflicting
concurrent reports for one target ⇒ fail toward degradation). Add to Part G.

**Finding 13 (pilot starvation contradiction) — ACCEPT.** Items 5 and 6 conflict: a
chatty spectator on a bad link never goes silent, so contested-release never fires and the
pilot is locked out *during its own fade*. Fix: **pilot preference is preemption** — a NACK
from `preferred_originator` seizes the lock immediately, unconditionally; silence-AND-
contested release applies only among non-preferred originators.

**Finding 14 (preferred_originator spoof) — ACCEPT (document + clamp).** The privileged ID
is plaintext-forgeable; honest per the no-auth ethos, but clamp: spoofed-pilot NACKs still
pass bitmap-sanity + hold-down + the per-originator budget, and reconcile lock-vs-budget
(the lock is a tiebreak *within* the budget partition, not a second exclusive mechanism).

**Finding 17 (craft-misses-CSA strand under ground-leads) — ACCEPT, strong fix.** Ground-
leads created a design win the doc didn't exploit: the ack now rides the **strong downlink
direction** (craft DATA → ground multi-RX diversity). So **ground commits the switch only
after the craft acks** (an ARM-phase ack flag on the craft downlink) — converting the
whole strand class from "recover after" to "never happens." Plus: the **issuer (ground)
reverts to `prev_chan` after `csa_ack_timeout` with no video** — an issuer abandoning a
failed campaign is not "unasked revert," closing the ruling-2 gap.

**Finding 16 + 19 (CSA MAC coverage + PSK distribution) — ACCEPT.** MAC covers **common
prefix + all CSA fields** (else a valid body re-wraps under a fresh session, defeating
anti-replay). `csa_nonce` = strictly-increasing per `(originator, session)`, accept iff
`nonce > last_applied`. Add `csa_psk` to the Part H config; **trust boundary = craft+ground
only** (spectators follow unauthenticated — their divergence is self-harm); a MAC-valid CSA
may *establish* the craft's session binding (bootstrap). `home_chan` becomes **config-pinned,
not wire-carried** (finding 18) — removes it as an attack surface; CSA shrinks 34→32 B.

**Finding 8 (`loss_pre_recov` semantic flip — MAJOR) — ACCEPT.** The field's meaning
silently inverted (pre- vs post-diversity) across documents → a 4×-too-eager demote waiting.
**Rename the wire field** (`loss_postdiv_prearq`) in the rewrite; never reuse the old name.

**Finding 9 (`table_version` content hash) — ACCEPT.** A hand-bumped u8 detects *lineage*
skew, not *content* skew (two edited configs both "3" reproduce blocker 8). Define
`table_version` as a **truncated content hash** (CRC of a canonical serialization).

**Finding 10 (endianness) — ACCEPT.** All multi-byte fields **big-endian (network order)**;
magic transmitted as bytes `57 42`. One sentence, non-negotiable for the rewrite.

**Findings 20/21 (ρ non-stationary + hidden third FEC options) — ACCEPT.** ρ is geometry/
attitude-dependent (a banking turn is the operative ρ→1 transient) → decide on **P95
windowed ρ**, not a one-shot mean, and define the estimator. The "no-FEC vs GF(256)-RLC"
binary is a false dilemma — add two middle options to the bench: **(a) FEC on ARQ-class
(I-frame) blocks only**; **(b) Tetrys-style reactive coded repair** (one GF(256) coded
packet per NACK instead of k retransmits — ARQ-shaped, no always-on parity, cuts round
trips, and directly reduces the finding-3 return-path dependence).

**Accept-as-edits (MINOR):** finding 11 (all per-stream RX state keyed by full
`(originator, session_id, stream_id)`); 12 (oversize ingress = drop-with-stat); 22 (parity
priority is "recommended live-priority," a middle shed-first class is allowed); 23 (age
unvisited rung EWMAs toward a physics prior so staleness degrades safe); 4/6 (move liveness
watchdog + `adapter_stalled`, `reports_expected/received`, `return_window_hits/misses` into
Part H unconditionally); 5 (annotate each bench gate with loopback-sufficient vs real-RF).

**Wire math verified clean** (finding 7): all offset columns re-add correctly.

## Next step (operator choice: review-then-fold)

Operator reviews this document (incl. Pass 3b) and the one escalated decision (revisit
ruling 3 — the 2nd craft RX adapter is now software-cheap); on sign-off I rewrite
`PROTOCOL.md` as one revision commit incorporating Parts A–I, the four rulings, and the
Pass-3b dispositions. No spec edits until then.
