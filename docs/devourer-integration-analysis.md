# Devourer deep-integration analysis — occupancy/scout, aggregation/HW-ACK, FEC, adaptive link (2026-08-05)

**Investigation only. No spec ruling is made here**, so there is no
`docs/review-log.md` Pass entry attached. Every item under "Open decisions" is
deferred to an operator ruling; when one is made it commits first as a spec
amendment + numbered Pass, per repo law.

Scope: the four areas the operator named — **channel occupancy + the channel
scout**, **aggregation and hardware ACKs**, **FEC**, and devourer's
**interpretation of the adaptive link** (`third_party/devourer/docs/adaptive-link.md`,
`adaptive-link-building-blocks.md`). The question asked is specifically: *what
does devourer cover natively that we currently build ourselves?*

This extends `docs/devourer-revendor-review.md` (2026-07-24, written against
devourer `7a6541f`). That survey is still the correct broad map; it is re-read,
not replaced. What changed since it was written:

- We are now vendored at **`800c3c8`** (two bumps: `a71060f` #68, `800c3c8` #89).
- **G0 is closed affirmatively** (Pass 139): devourer transmits MCS4–7 at 5805 on
  the 8822e, on the vehicle, on real venc video, ≥98% at every rate on `800c3c8`.
  The `MonAir` pivot rationale does not survive, so the RadioAir-gated items in
  that survey are no longer blocked on a measurement.
- The **DIS_CCA** open decision is **settled and closed** (Pass 139): measured,
  `air.disable_cca` ships **false** — clearing the gate costs ~45% of the uplink,
  because half-duplex on one radio means transmitting is not listening.

**The standing precondition for almost everything below.** Both fleet nodes run
`air.kind: "kernel-monitor"` (`deploy/vehicle-192.168.2.232.json:49`,
`deploy/ground-192.168.2.242.json:50`). Every devourer-native sensor and lever in
this document is reachable only through `RadioAir`. The kernel monitor path gives
radiotap RSSI and nothing else — no EVM, no frame-free energy, no NHM, no CCX.
So the honest framing of this whole document is: *these are the things that become
available when a node moves to `air.kind: "radio"`, and several of them are
strong enough to be reasons to move.*

---

## 0. Reachability matrix for the actual fleet

Three chips are in the fleet, spanning two devourer generations. That split
decides more of this analysis than any policy argument, so it goes first.

| capability | 8812AU (Jaguar1) | 8812CU (Jaguar3/8822c) | 8812EU (Jaguar3/8822e) |
|---|---|---|---|
| per-frame RSSI / SNR / EVM (`RxPacket.h:61-66`) | ✓ | ✓ | ✓ |
| frame-free FA/CCA deltas (`GetRxEnergy`) | ✓ | ✓ | ✓ |
| NHM 12-bucket power histogram | ✓ | ✓ | ✓ |
| live IGI | ✓ | static hint | static hint |
| active absolute noise floor | ✓ (8812A CAL, once at bring-up) | — | — |
| `GetRxQuality` + LinkHealth verdict | ✓ | ✓ | ✓ |
| thermal meter (`GetThermalStatus`) | ✓ | ✓ | ✓ |
| `SetAmpduMode` | ✓ (but no deep feed — async TX) | ✓ | ✓ |
| **`rx.keep_corrupted`** (fused-FEC precondition) | ✓ | **✗** | **✗** |

`keep_corrupted` is Jaguar1 + Jaguar2 **only** — it is honoured in
`src/jaguar1/RadioManagementModule.cpp:204` and `src/jaguar2/HalJaguar2.cpp:2573`
and nowhere else. Neither Jaguar3 die reads the field. Roles matter here: the
craft TX is the 8812EU and the ground diversity ears are 8812CU, so the only
fleet adapter that can surface corrupt frames at all is the 8812AU on the bench
ground. This is decisive for §3.

Also worth recording: `src/chanmig/` is **already in the static archive we link**
(`third_party/devourer/CMakeLists.txt:198-215`). `ChannelScore.cpp` and
`MigGate.cpp` are the only two `.cpp` files there; the rest — `SurveyRecord.h`,
`EvidenceStore.h`, `ScanPlan.h`, `ActiveLink.h`, `ChannelDef.h` — are pure
header-only logic with no device dependency. (`src/cell/` is header-only and not
in the library source list at all, so `RxReceipt.h` / `UeRxAttribution.h` are
include-only.) Using the scoring half costs zero new build wiring and does not
require the migration wire protocol.

---

## 1. Channel occupancy and the scout — the largest real gap

### What we build today

`ScoutEngine` (`app/main.cpp:628`) sweeps the allowlist, dwelling `scout.dwell_ms`
per channel, and aggregates an occupancy record whose field set was deliberately
shaped as an ACS superset (`docs/scout-design.md` §6) so that a later hardware
backend would be a field-fill rather than a reshape. v1 fills only the
packet-derivable fields.

### The defect

**The occupancy metric is fed exclusively by successfully-decoded waybeam
frames, so it is blind to every non-waybeam emitter on the channel.**

The chain is short and airtight:

1. `RadioAir::Impl::on_packet` (`io/src/air_radio.cpp:235`) drops anything that
   fails `dot11_parse` against our prefix — foreign traffic increments
   `rx_filtered` (`:248`) and is discarded there.
2. `ScoutEngine::on_frame` is fed from the decoded-frame path and reads
   `originator` straight out of the waybeam §3.1 prefix (`app/main.cpp:684`).
3. `wifi_util_permille` is therefore the airtime of *waybeam* frames only
   (`app/main.cpp:689`), `bss_count` is a count of distinct waybeam
   `originator`s rather than BSSs, `noise_dbm` is the minimum RSSI **of our own
   frames** (`app/main.cpp:883`), and `interference_util_permille` is hardcoded
   `null` (`app/main.cpp:749`).

So the endpoint documented as ranking "the emptiest allowlisted channel" ranks
channels by *how much of our own fleet's traffic is on them*. A channel
saturated by an AP, a microwave, or another FPV link scores as pristine — and
because `quality_permille = availability_permille = 1000 − util`, it scores as
maximally *attractive*. The ground config sweeps seven channels this way
(`deploy/ground-192.168.2.242.json`), and the whole point of the sweep is to pick
a home for a video link.

This is not a latent bug in the sense of a coding error — `docs/scout-design.md`
§6 says plainly that v1 is "rough and Wi-Fi-only." The finding is that "Wi-Fi
only" turned out in practice to mean "*our* Wi-Fi only", which is a weaker claim
than the design intended, and that the hardware backend it was reserving space
for is sitting in the archive we already link.

### What devourer covers natively

- **`GetRxEnergy(with_nhm)`** (`src/IRtlDevice.h:533`, struct at `src/RxSense.h:30`)
  — frame-free FA/CCA counters, IGI, and the 12-bucket NHM in-band power
  histogram. Frame-free is the whole point: **no frame has to decode**, so this
  sees energy from emitters we cannot parse. That is exactly the axis our metric
  is missing.
- **`GetRxQuality`** (`src/IRtlDevice.h:544`) — the windowed fusion: frame
  aggregate (RSSI/SNR/EVM), the frame-free energy, a passive noise floor
  (`rssi_dbm − snr_db`, works on every generation), the opt-in absolute idle
  floor, and the LinkHealth verdict.
- **`src/chanmig/`** — the scoring layer. `SurveyRecord`/`SurveyJsonl` (a
  versioned dwell record with a validity bitmask), `EvidenceStore` (folds records
  behind a trust boundary: wrong plan hash, implausible counter deltas from a
  wrapped hardware counter, staleness, foreign calibration domain — all rejected
  by counted reason), `ScanPlan` (revisit deadlines, most-overdue-first, round
  counter advancing only on full coverage so a favoured bin cannot starve a
  rescan), and `ChannelScore`/`RecommendEngine` (two-leg law, hysteresis,
  anti-churn, every decision citing its evidence generation and policy hash).

Three specific pieces of their design would fix defects our scout has today, and
they are worth naming individually because each is cheap:

- **Dwell hygiene.** The FA/CCA counters are delta-on-read. devourer's dwell runs
  `FastRetune → settle → DISCARD BARRIER → observe → read`, where the barrier is a
  throwaway `GetRxEnergy()` that resets the hardware deltas and drains the frame
  aggregate (`docs/adaptive-channel-migration.md`). Our scout has no barrier: each
  bin is charged with its own retune and settle, plus frames still in the USB
  pipeline from the previous channel. We already found the tail of this
  empirically — Pass 66's "heard-most, not last" rule exists precisely because
  buffered frames leak across a retune boundary. A discard barrier addresses the
  cause rather than the symptom.
- **Own-traffic attribution.** They split decoded occupancy into `dvr_air_us` vs
  `oth_air_us` by transmitter SA, so "the active channel is busy with our own
  signal" can never read as interference. `docs/scout-design.md` §6 states the same
  requirement ("exclude the candidate craft's own traffic") and we implement it by
  simply not counting anything foreign — which satisfies the requirement by
  discarding the signal it was protecting.
- **A dwell no longer needs to hear anything.** Our dwell logic extends once when
  it heard waybeam frames but no ANNOUNCE yet (`app/main.cpp`, Pass 72), because
  our only sensor is a decoded frame. Frame-free energy decouples occupancy
  measurement from discovery entirely: occupancy needs a short fixed observe
  window, and only *discovery* needs to wait for an ANNOUNCE. That is a
  latency win on a seven-channel sweep as well as a correctness one.

### Options, in ascending cost

| option | what it is | cost | verdict |
|---|---|---|---|
| **A — field-fill** | Per dwell, call `GetRxEnergy(true)` behind a discard barrier; fill `interference_util_permille` from the FA/CCA delta and `noise_dbm` from the passive/absolute floor; keep our record shape, our sweep, our endpoints. | Small, RadioAir-only, no wire change. §15.5a occupancy *semantics* change, so it needs a Pass. | **Recommended first.** This is the field-fill `scout-design.md` §6 explicitly reserved space for. |
| **B — adopt the scoring layer** | Additionally fold dwells into `EvidenceStore` and rank with `ChannelScore`, keeping our `POST /scout/*` control plane as the façade. | Moderate. Header-only, already compiled, selftested upstream. Needs a policy artifact + hash. | **Worth a ruling.** Buys the trust boundary, min-rounds/staleness gating, and anti-churn we have not built. |
| **C — converge on chanmig wholesale** | Take their ground-proposes / drone-commits wire protocol and `MigGate` too. | Large, and directly conflicts with §11, which is LAW here and already has a CSA campaign, HMAC key provenance, binding lifecycle, and the §11.6 strand-proof ACK. | **Not recommended.** The previous review posed this as an open architecture question; the sharper answer now is that the *sensing and scoring* half is separable from the *protocol* half, and only the first half is a gap. |

One structural mismatch to flag under B/C: their scout is a **dedicated passive
second adapter** that structurally cannot command anything, feeding the advisory
engine over a file. Our scout roams the `role:"tx"` uplink adapter and is
mode-exclusive on a single-adapter ground (`scout-design.md` §6). `RadioAir`
requires exactly one `role:"tx"` adapter, so making an RX-only ear the scout is a
real change, not a config flip. Not a blocker for option A — the uplink adapter
can read its own energy counters during its own dwell — but it is the shape B
assumes.

---

## 2. Aggregation and hardware ACKs

### Hardware ACK/BlockAck — already integrated, and carrying a latent trap

We already call `SetAckResponder` (`io/src/air_radio.cpp:479`), armed with the
craft's own SA so the ground's unicast returns match the responder's MACID
(§3.0 Pass 12 hybrid). It is opt-in via `air.ack_responder`, default false
(`io/include/wblink/config.h:388`), and **unset in both fleet configs** — so it
is dormant today.

**Finding: enabling it today would arm a one-ended ARQ loop.** devourer `#354`
made the TX retry limit configurable with default **0**
(`third_party/devourer/src/DeviceConfig.h:225`), and its own doc-comment says
"Hardware-ARQ (`SetAckResponder` + unicast TA) **needs a nonzero value**."
`RadioAir` sets `dc.rx.enable_with_tx`, `dc.tx.report` and
`dc.tuning.disable_cca` and never touches `dc.tx.retry_limit`
(`io/src/air_radio.cpp:413-425`). So `air.ack_responder: true` would give us a
peer that ACKs correctly and a sender that never retransmits — the loop is armed
at one end and disabled at the other. Pass 139's addendum recorded that the
default moved; what it did not draw out is that the responder knob and the retry
knob must be set *together* or the feature is inert. Whatever the ruling on
enabling the hybrid, the two should be bound in one config decision rather than
left to independent defaults.

Two adjacent notes for whenever that hybrid is enabled:

- `ack_timeout_us` defaults to 128 µs ≈ a ~15 km round-trip budget, programmed
  identically on every generation. Fine for us; worth knowing it is a *range*
  lever, not a timing detail.
- The `DEVOURER_RX_POOL_EXHAUST` policy defaults to `backpressure`, which parks
  exhausted URBs so the chip declines further ACKs and loss stays ARQ-visible.
  The `drop` alternative produces ACKed-but-undelivered frames that the TX logs
  as delivered and never retries. We inherit the safe default; the requirement is
  simply not to change it.

### A-MPDU — the answer is still no, and now for better reasons

Pass 8 rejected A-MPDU partly because Jaguar1/8812AU collapses under the deep feed
it needs. The craft is now Jaguar3, so that premise did change — and the previous
review correctly flagged the changed premise while recommending no reversal. The
evidence in `docs/aggregation.md` at `800c3c8` makes the rejection stronger, not
weaker:

- **The +30% is the wrong shape.** It is a high-MCS, broadcast/no-ack,
  near-PHY-ceiling number. The same aggregation measured at the **unicast-ARQ
  shape** (MCS3, 512 B, hardware ACK + retry 8 — i.e. the FPV ARQ configuration)
  delivered **−8%**: at low MCS the preamble amortization is small and the
  BlockAck machinery costs more than it saves.
- **It breaks our TX-side sensor.** Under `AGG_EN`, ~60% of requested SPE_RPT
  reports are never emitted and every emitted report reads `retries=0`. Our §9.10
  TX-wedge watchdog (`io/include/wblink/txwedge.h`) takes CCX completions as its
  devourer-side progress feed, and its trigger is progress *absence* — a 60%
  suppression against a `min_submits`-gated window is exactly the input that
  watchdog is least able to interpret. The `tx.report` retry distribution, the
  only TX-side link-quality sensor we have, goes to zero information at the same
  time.
- **Latency.** The aggregate-fill timer paces launches at ~0.8–3 ms, added in
  front of a link whose entire §7.2 design is a quiet-gap pacer.

**Recommendation: no reversal, and record the fresh evidence** so the next
re-vendor does not re-open it from the +30% headline alone.

### The separable piece: USB TX aggregation

`send_packets` + `tx.usb_agg_max` is **independent of A-MPDU** — host↔chip
transport only, no on-air change, byte-identical descriptors. It removes
per-frame host/USB overhead, which is the kind of thing that matters on the
SSC338Q. But: the chip flow-controls a multi-frame URB anyway (so it lowers host
cost, not an air ceiling), and our injection path submits exactly one frame per
`RadioAir::inject` call from the scheduler. Batching therefore means restructuring
the §7.2-paced emission path, and deliberately introducing queueing in front of a
latency-first pacer to save host CPU is the wrong trade unless SSC338Q CPU is
measured to be the binding constraint. **Note it; do not pursue it without that
measurement.**

---

## 3. FEC

### There is less to integrate here than the docs suggest

`third_party/devourer/docs/fused-fec.md` is the most exciting document in the
tree and the least actionable for us. Its outer codes (RaptorQ / RLC / RS), the
sub-block-integrity layer, and the per-SVC-layer FEC UEP are **Python tooling
under `tools/precoder/`**, not library capability. Nothing there is linkable.

Our own §14.1 systematic Cauchy Reed-Solomon over GF(256) (`core/src/rlc.cpp`,
`core/src/gf256.cpp`) is C++, in-core, pure, 32-bit clean for the Android-vendored
core, MDS with a guaranteed any-k recovery, and deployed with per-importance-class
rates (30% IDR / 20% P-frame on the vehicle). It is the same *family* of code as
their `stream_fec_rs.py` — both are GF(2⁸) MDS. **There is no version of "adopt
devourer's FEC" that is an improvement.** That should be stated plainly so it
stops being re-litigated at each bump.

### The one library-level piece, and why it is out of reach

The genuinely native capability is `rx.keep_corrupted` — surfacing FCS/ICV-failed
frames instead of dropping them, so an outer code can salvage the surviving
sub-blocks of a mostly-correct body. `RadioAir` currently drops them at
`io/src/air_radio.cpp:237`.

**It is unavailable on the fleet's Jaguar3 adapters** (§0). The craft TX is the
8812EU and the ground diversity ears are 8812CU; neither honours the field. Only
the bench 8812AU could produce corrupt frames at all. The previous review listed
this as a Tier-D building block on the strength of the Gate-2 walk's 232
partially-observed unrecoverable frames; the per-generation constraint means that
tail is not reachable at the receivers that observed it.

Even setting the hardware aside, two things argue against it for us:

- **It is a wire change, not a knob.** SBI requires fixed-size CRC-guarded
  sub-blocks *inside* the DATA body — a §5.1/§14 amendment, a new framing layer
  between our RLC symbols and the radio body, at both ends.
- **We are in the wrong loss regime.** Their own A/B says SBI's CRC tax is not
  worth it below ~10% loss and wins decisively above ~15%. Our Gate-2 measurement
  is 86‰ pre-diversity reduced to **24‰ post-diversity** — an order of magnitude
  below where SBI starts paying. Per-adapter RX diversity is our primary
  redundancy by design, and it works; it moves us out of the regime that makes
  fused FEC worth its complexity.

**Recommendation: close this item rather than carry it.** Record the
generation constraint and the regime argument so it is not re-surveyed at each
bump. If it is ever revisited, the trigger is a measured sustained loss above
~15% post-diversity, not a new devourer release.

### Two adjacent FEC-family items that *are* live

- **LDPC remains unadopted.** Grepping our tree, `LDPC` appears **nowhere** —
  not in `core/`, `io/`, `app/`, `PROTOCOL.md`, or `docs/review-log.md`. The
  previous review called it "the standout omission" (≈ +3 dB coding gain at the
  10%-delivery crossing, MCS7/20) and put it first in its suggested order; it is
  still first and still undone. It is a per-packet radiotap flag, it works on the
  kernel-monitor path too, so it is testable on the *current* deployment without
  waiting on anything in this document. Adjacent to the operator's question
  rather than inside it, but it is the cheapest range on offer and it keeps
  getting skipped.
- **Frequency diversity as an erasure source.** Their observation that per-packet
  radiotap channel hopping spreads a block's N symbols across N_ch channels — so
  a dead channel erases only ⌈N/N_ch⌉ of each block and an MDS outer code absorbs
  it — is genuinely elegant and composes with our existing decoder unchanged
  (we dedup by block/symbol index already). It is also deeply at odds with §11's
  single-channel hold and with a half-duplex return path that must be heard in the
  §7.2 quiet gap. High spec cost, recorded for completeness, not recommended.

---

## 4. The adaptive-link interpretation

This is the item with the most transferable thinking and the least transferable
code — devourer is explicit that it is "the mechanism, not the policy," and the
controller lives upstream. So the question is not "adopt it" but "which of its
ideas answer questions we have already written down as open."

### Objective: less opposed than it looks

Their objective is minimum Joules per *delivered* bit under a per-layer quality
floor; ours is §9's quality-and-latency ladder with hysteresis plus
importance-gated ARQ. Read as objectives these are different systems. Read as
*levers* they mostly agree: the energy-minimizing reflex is "ride the highest
modulation the link will bear, because airtime is the dominant energy term," and
a latency-first broadcast link wants the same thing for a different reason
(short airtime, more room in the quiet gap). They diverge in exactly one place —
the **strong-link reflex**. An energy-minimizer backs power and FEC off and lets
the amplifier idle; a quality-maximizer spends the headroom on bitrate. That
divergence is a genuine operator choice about what a craft's margin is *for*, and
it is not settled by any measurement in either repo.

### The idea worth taking: "the model proposes; measurement disposes"

Their central mechanism: a small deterministic share of frames flies the rates
*adjacent* to the selected one, and **both ends derive which frames those were
from the sequence number alone — no signalling**. The receiver then holds measured
per-rate delivery with small-sample confidence bounds, and the evidence corrects
the model in the safe direction only.

That mechanism directly answers an open question this repo has already written
down. `PROTOCOL.md` (Pass 118) says received-MCS is observation-only, and:

> It is the denominator half of a per-MCS PER ladder; the numerator (attributing
> a sequence gap to the MCS the *missing* packet would have carried) is an open
> §9.2 question.

**If the rate is a deterministic function of the sequence number, the missing
packet's rate is known.** The numerator falls out for free, with no wire change
and no signalling — the receiver computes it from the gap. That is the enabling
primitive for two things Pass 118 explicitly deferred: the per-MCS PER ladder,
and per-packet importance-gated rung divergence (which Pass 118 gated on §9.5
airtime accounting and §9.2 loss attribution). It is the highest-value single
idea in their document for us.

**And the return path is already built.** §3.5 carries `probe_per` — u16 ‰ at
offset 36, "promote-probe PER; `0xFFFF` = no probe" (`PROTOCOL.md:400`,
`core/include/wblink/wire.h:82`) — codec'd in both directions
(`core/src/wire.cpp:97,520`) and hardwired to `kNoProbe` by the reporter with the
comment *"§9.4: no probe machinery in v0"* (`core/src/reporter.cpp:85`). So the
wire slot for exactly this measurement was reserved in v1 and has never been
filled. Unlike the EVM item below, the probe half of this needs **no wire
amendment at all** — only the two ends agreeing on the sequence→rate function
and the reporter finally computing the field it already sends.

### Where their controller differs from §9, concretely

| axis | §9 today | their design | worth taking? |
|---|---|---|---|
| promote gate | RSSI margin over a node-local per-rung floor (§9.4, `rung_rssi_floor_dbm`) | measured delivery lower bound must clear what the FEC needs | **Yes**, as a *complement*. RSSI margin is a counterfactual about a rate not currently flying. |
| bad-rate escape | Pass 110 recurrent-loss rung lockout: timed strikes latch, promotion never probes across the lowest active lockout | a selected rate whose measured delivery stays condemned is abandoned and barred even while the model believes in it | **Already converged.** Ours is a strike counter, theirs a confidence bound; same reflex. |
| emergencies | §9.8 fail-safe: stale feedback never promotes, damped step-down | "emergencies keep trusting the model, because escaping a failing rate must never wait for samples" | **Already converged.** Useful confirmation that the asymmetry is right. |
| power | §11.7 runtime ceiling + the ground-uplink calibration owner (Passes 132–138) | power↔margin probe: the *minimum power that clears the margin* | **Already converged**, independently. Worth noting we built their probe. |
| per-layer protection | per-importance-class FEC (30/20) — one MCS for all | per-layer on **both** knobs: robust layers get robust MCS *and* heavy FEC | **Half-built.** The FEC half exists; the MCS half is Pass 118's residual. |
| saturation | not modelled | EVM is the tell; RSSI-strong + EVM-dirty means back power **off** | **Yes — see below.** |

### EVM, and the one wire cost

`RxPacket.h:61-66` delivers per-chain `rssi[4]`, **`snr[4]`, and `evm[4]` on every
received frame**. `RadioAir::Impl::on_packet` reads `rssi[]` and nothing else
(`io/src/air_radio.cpp:266-278`). So two of the three link-quality scalars the
silicon hands us are being discarded at the receiver, for free, today.

This matters because of the specific failure mode `LinkHealth.h` documents from
`tests/saturation_knee_sweep.sh`: as power rises on a near-field link, RSSI
climbs monotonically while EVM improves and then **reverses** at the front-end
saturation knee — and SNR misses it entirely (flat at 18 dB across the whole
sweep while EVM went −28 → −13 dB). Our §9.4 promote gate is RSSI-margin-driven.
A saturated link presents as strong RSSI, which reads to the cascade as headroom,
which promotes, which collapses on EVM, which shows up as loss, which demotes —
and the §9.7 flap layers then have to absorb an oscillation whose cause is
invisible to every input the selector has. The fix is not a new algorithm; it is
a discriminator that already arrives on every frame.

**The cost is a wire amendment.** §9.1 puts the decision at the TX (the craft),
and the ground is where the clean view of link quality lives, so the verdict has
to travel over LINK_REPORT. §3.5 is a fixed **39 bytes** with no spare field
(`PROTOCOL.md:384-402`) — it carries `rssi_best` and `rssi_mean` and nothing
about signal *quality*. Adding even one byte of verdict is a §3.5 amendment and
a §3.1 version event. That is the real price of this item and it should be
priced honestly: the sensor is free, the plumbing is not.

Three shapes, cheapest first, for whoever rules on it:

1. **Reuse `recommended_prof`** (already an RX hint, TX has final authority) —
   let the ground's LinkHealth verdict inform the hint it already sends. Zero wire
   change. Weakest, because the hint is a profile index, not a cause, and the TX
   cannot distinguish "RX thinks lower" from "RX thinks you are saturated."
2. **One byte: a `LinkVerdict` enum** (`NoSignal|Saturated|Interference|Weak|Marginal|Healthy`).
   Minimal amendment, carries the *cause*, and the §9 cascade can gate the
   promote rule on `!Saturated` without any new numeric threshold.
3. **Two bytes: verdict + `evm_mean`.** Lets the craft see the knee itself rather
   than a classification, at the cost of importing devourer's raw half-dB EVM
   units onto our wire — which the "no floats on any wire-visible path" rule
   tolerates but which ties the wire to a vendor unit convention.

Recommendation if it is taken: **shape 2**. It puts a *cause* on the wire rather
than a vendor scalar, which is the same instinct that made `LinkHealth` worth
writing in the first place.

---

## Open decisions (deferred — each needs an operator ruling + a numbered Pass)

1. **Scout occupancy field-fill** (§1 option A) — fill
   `interference_util_permille` and `noise_dbm` from `GetRxEnergy`/NHM behind a
   discard barrier. Changes §15.5a occupancy semantics. *Highest value in this
   document.*
2. **Scout scoring layer** (§1 option B) — adopt `EvidenceStore` + `ChannelScore`
   behind our existing `POST /scout/*` façade. Touches §15.5a.
3. **chanmig convergence** (§1 option C) — the previous review's open item #4;
   this document's recommendation is to **close it as declined**, on the grounds
   that the sensing/scoring half is separable and only that half is a gap.
   Touches §11.
4. **`ack_responder` and `tx.retry_limit` as one decision** (§2) — bind them, or
   the hybrid is inert when enabled. Touches §3.0.
5. **A-MPDU** (§2) — recommend recording the −8%-at-ARQ-shape and CCX-suppression
   evidence and closing it, so the +30% headline does not re-open it each bump.
6. **Fused-FEC / `keep_corrupted`** (§3) — recommend closing on the Jaguar3
   generation constraint plus the 24‰ post-diversity loss regime.
7. **Sequence-derived rate probing** (§4) — the primitive that closes the §9.2
   numerator question and unblocks Pass 118's residual (per-packet
   importance-gated rungs) and the per-MCS PER ladder. Reports through the
   already-reserved §3.5 `probe_per` field, so no wire amendment. Touches §9.2,
   §9.4, §9.5.
8. **EVM / LinkHealth verdict on LINK_REPORT** (§4) — a §3.5 amendment and a §3.1
   version event; recommended shape is a one-byte verdict enum.
9. **Per-packet TX power on the EU craft** (carried unchanged from the previous
   review) — §10.1's "no per-packet TX power" premise was measured on the 8812AU;
   the craft is a Jaguar3 with programmable BB offset banks. Still open.
10. **`FASTRETUNE_FW=2` cross-band offload** (carried unchanged) — ~2–2.6 ms vs
    ~90 ms cross-band, ~3× less host CPU on the SSC338Q. Touches §11.

Closed since the previous review, recorded so they are not re-opened: **G0**
(devourer MCS4+ at 5805, Pass 139) and **DIS_CCA** (`air.disable_cca` ships
false, measured, Pass 139).

## Suggested order

1. **LDPC A/B** — unchanged from the previous review's suggested order, still
   undone, still testable on the *current* kernel-monitor deployment with no
   dependency on anything else here.
2. **Scout occupancy field-fill** (#1) — the clearest correctness gap, and it is
   a field-fill into a record shape that was designed for it.
3. **EVM/SNR harvest at the RX** — start consuming `RxPacket`'s `snr[]`/`evm[]`
   into stats (§15.3) *before* ruling on the wire amendment (#8). The sensor is
   free; a Pass on the wire should be argued from our own measured knee, not
   from devourer's bench.
4. **Bind `ack_responder` + `retry_limit`** (#4) — small, and it prevents a
   future "we enabled it and nothing happened."
5. **Sequence-derived rate probing** (#7) — the biggest unlock, and the one that
   should be designed rather than rushed; it is the gate on Pass 118's residual.

Everything in 2–5 is `air.kind: "radio"`-only. If the fleet is not moving to
RadioAir, only item 1 is reachable.
