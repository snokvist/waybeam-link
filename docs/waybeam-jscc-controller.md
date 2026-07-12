# waybeam JSCC Controller — Investigative Brief

**Status:** design proposal / research brief. Not a spec. The purpose of this document is
to frame the problem precisely enough that the open questions become answerable by
measurement rather than argument.

**Scope:** the joint source–channel control loop between the waybeam link layer (consumer)
and the SSC338Q video encoder (producer), over RTL8812AU/EU raw injection.

---

## 1. Problem statement

The historical failure mode of wifibroadcast-class links is that **the encoder (producer)
and the link (consumer) are two independent control loops that do not talk to each other.**
The encoder emits frames at whatever size its internal rate controller decides; the link
chops them into MTU-sized packets and applies a FEC scheme chosen without reference to
frame boundaries, frame importance, or the display deadline. The two loops fight: the
encoder's VBV controller reacts on one timescale, the link's FEC/MCS logic on another, and
neither has authority.

The proposal is to collapse these into **one controller** that jointly allocates a single
scarce resource — airtime — between *source bits* (video information) and *channel bits*
(parity, retransmissions, modulation robustness), subject to a hard per-frame deadline.

This is **joint source–channel coding (JSCC)** with a closed feedback loop. It is not novel
in the literature (see §10); what is uncommon is doing it across MCS + MTU + FEC + ARQ +
encoder simultaneously, on a raw-injection link, with a hard real-time deadline.

---

## 2. Design invariants (non-negotiable)

These are conclusions already reached; they constrain everything downstream. Any proposal
that violates one of these is out of scope.

**I1. Per-frame deterministic delivery.**
One video frame = one FEC block. Frame *N* decodes if and only if frame *N*'s own packets
and parity suffice. No frame's decodability may depend on another frame's loss pattern.

**I2. Paced release.**
The link is paced to feed the real-time decode/display clock. The decoder drops the older
frame if two arrive together, so a batched release is equivalent to a frame loss *plus*
wasted airtime. Delivery must be smooth, not bursty-then-catch-up.

**I3. Intra-refresh (GDR), not IDR/GOP.**
Already the preferred approach. It is load-bearing here for two reasons, both essential:
- It flattens the frame-size distribution. Per-frame deterministic delivery is only
  *achievable* if frames are uniformly sized — a single IDR outlier blows the deadline.
- It makes frame dropping **safe**. With no GOP dependency chain, a discarded frame is a
  transient artifact that refreshes out, not a smear until the next keyframe.

**I4. Bounded, checkable decode latency.**
At emit time the controller must be able to decide whether a frame can land by its display
slot. Anything that makes this undecidable is rejected.

---

## 3. Rejected: sliding-window FEC (recorded, with reasoning)

Sliding-window RLC (RFC 8681) is the standard answer to the small-block FEC problem, and it
was seriously considered. **It is rejected**, and the reason should be written down so it
isn't relitigated:

Sliding-window codes do not merely *smear* frames across blocks — they **rank-couple** them.
A repair equation spanning frames *N-2…N* is only usable once every other unknown in that
window is resolved. So frame *N*'s decodability becomes a function of frame *N-2*'s loss
pattern, and decode latency is bounded not by the frame but by the window's worst
unresolved variable. That directly violates **I1** and **I4**, and produces exactly the
batched-release failure that **I2** forbids.

Sliding-window optimizes *coding efficiency and goodput*. This system optimizes *bounded
per-frame delivery*. Different objective; correct to decline.

**Corollary — the small-frame FEC "problem" was largely an artifact of thinking in rates.**
For k=1, m=2 (n=3), failure requires all three packets lost: 0.2³ ≈ 0.8% at 20% PER. That
is a 200% overhead *rate* that costs two packets. The fractional overhead balloons exactly
where the absolute airtime cost collapses, because small frames imply low bitrate implies
spare airtime. **Size m against a target residual failure probability, never against a
target code rate.** The small-block tax is real but it is cheap where it bites.

---

## 4. Control surface

### Link-side knobs

| Knob | Range | Timescale | Primary effect |
|---|---|---|---|
| **MCS** | 0–7 (+ 10/20/40 MHz) | seconds, hysteresis | raw rate vs. sensitivity |
| **MTU / symbol size `S`** | ~500–2304 B (MSDU cap) | per-frame | sets `k`; see §5 |
| **Parity `m`** | 0…(255−k) | per-frame | residual failure probability |
| **ARQ enable + retry budget** | 0–N rounds | per-frame | loss-conditional repair |
| **Deadline discard** | on/off | per-frame | abandon undeliverable frames |

### Encoder-side knobs (commanded by the link)

| Knob | Timescale | Notes |
|---|---|---|
| **Per-frame target size / QP** | per-frame | see §7 — this is the Salsify lesson |
| **Bitrate** | ~100 ms | |
| **FPS** | seconds, hysteresis | *last-resort knob* — see §6 |
| **Intra-refresh period** | seconds | trades keyframe-free recovery speed vs. bitrate |

### Observables

`RSSI`/`SNR` per antenna · packet error rate (windowed) · burst-length distribution ·
uplink RTT · airtime utilization · realized frame sizes · **deadline misses** (the true
objective metric — not PSNR, not packet loss).

---

## 5. MTU is a first-class knob: the `n ≤ 255` coupling

This is the specific insight that promotes MTU from marginal to structural.

RS over GF(256) has a hard ceiling: `k + m ≤ 255`. With frame-aligned blocks:

```
k = ceil(frame_bytes / S)
```

A large frame at fixed `S` can force `k + m > 255`, which would require **splitting the
frame into two blocks — violating I1**. The only knob that can pull `k` back down without
splitting is `S`. Therefore:

```
S_min = frame_bytes / (255 - m)
```

**Worked example.** A 300 KB frame with `m = 40` gives `k_max = 215`, so `S ≥ 1428 B`. At
S = 1200 the frame *must* split. At S = 1500 it fits in one block. MTU is what buys you
the one-frame-one-block invariant at the top end.

### The four competing effects of raising `S`

1. **↓ k** — keeps large frames under the 255 ceiling. *(the reason it's a knob)*
2. **↑ PER** — a longer packet has more bits to corrupt. Roughly `PER ≈ 1 − (1−BER)^(8S)`,
   so doubling `S` roughly doubles PER at low PER. This forces `m` back up. **Direct
   tension with (1) — quantify it before trusting it.**
3. **↑ airtime efficiency** — amortizes the fixed 802.11 per-packet cost (preamble, IFS,
   backoff ≈ 50–100 µs) over more payload.
4. **↑ packet duration vs. fade coherence time** — the dangerous one. At 10 MHz narrowband
   and low MCS, a 1500 B packet may occupy 2–3 ms of airtime, which is *comparable to the
   Rayleigh coherence time* of a moving airframe. A packet longer than a fade is a packet
   that dies to that fade. **This is a hard ceiling on `S` at long range, and it is
   independent of the FEC math.**

`S` is uniform within a block, but blocks are frame-aligned — so `S` is chosen at frame
start and held for the frame. No conflict with the code.

> **Open question O1:** where is the optimum of (1)+(3) against (2)+(4), as a function of
> MCS and channel width? This is the single most important measurement in the brief.

---

## 6. The deadline is the real constraint

At 144 fps the frame interval is **6.94 ms**. That is the entire budget:

```
frame_interval  ≥  tx + detect + NACK_uplink + retx + decode
```

Three consequences fall out of this, and they are the load-bearing design rules.

### 6.1 ARQ-vs-FEC is a function of RTT, not link quality

If measured RTT leaves room for ≥1 retry round inside the deadline, **ARQ carries the
frame** and FEC drops to a thin floor — ARQ costs airtime only on actual loss (~1.2× at
20% PER) versus FEC's *unconditional* 2–3×. If RTT exceeds the budget (long range, loaded
uplink), ARQ can never close in time and **FEC must carry it alone**.

This is the switch condition for the `toggle ARQ` knob, and it is currently the least
specified thing in the design.

Note the coupling: **lowering FPS widens the deadline and can bring ARQ back into range.**
That is a far better justification for the FPS knob than "make frames bigger," and it is
the one to build. FPS remains a *degradation-mode* knob, ranked last — dropping 144 → 60 fps
costs ~10 ms of sensor-to-glass latency, which is the product.

### 6.2 Loss *detection* latency is the hidden killer

With 1–3 packet frames, a sequence-gap detector cannot see a trailing loss until the *next
frame's* packets arrive — one full frame interval later. The deadline is gone before the
NACK is even generated.

**Fix: carry the frame's packet count `k` in every packet header.** Any single surviving
packet from frame *N* then tells the receiver exactly how many to expect, so a gap is
detectable immediately rather than inferred from what follows. Detection latency collapses
to ~0 for any frame where at least one packet lands.

For a *total* frame loss, a timeout is still needed — and here **I2 pays off twice**:
because delivery is paced to the display clock, the receiver knows when frame *N* *should*
have arrived. The timeout is tight and principled rather than a guess.

### 6.3 Deadline-aware discard

If frame *N* cannot make its display slot, **abandon it and spend the airtime on *N+1***.
Today the link would finish transmitting a frame the decoder is going to throw away — pure
wasted airtime, which also delays *N+1* and propagates the miss. The display layer already
drops old frames; the link should make the same decision earlier, where the cost is still
recoverable. **I3 (intra-refresh) is what makes this safe; I1 (per-frame blocks) is what
makes it decidable.**

---

## 7. Controller structure

### 7.1 Kill the encoder's internal rate control

This is the Salsify lesson and it is the whole point. If the SSC338Q's rate controller is
still running its own VBV loop while the hub commands bitrate, there are **two control
loops fighting over the same variable with different time constants** — which is the
original producer/consumer desync, merely relocated. Drive per-frame target size or QP
directly and let the link own the loop. Anything less and the desync survives the rewrite.

### 7.2 Timescale separation is mandatory (and physics, not taste)

At 5.8 GHz with the airframe at 20 m/s, Rayleigh **fading coherence time is on the order of
1 ms**. The feedback path is tens of ms. **You cannot close a control loop on fast fading —
ever.** Any knob driven from an instantaneous measurement will hunt.

Therefore:

| Loop | Period | Drives | Input |
|---|---|---|---|
| **Inner** | per-frame (~7 ms) | `m`, ARQ retries, deadline discard | current frame state |
| **Middle** | ~100 ms | bitrate, `S`, per-frame QP | windowed PER, RTT, airtime |
| **Outer** | seconds + hysteresis | MCS, FPS, intra-refresh period | EWMA SNR, deadline-miss rate |

FEC and ARQ handle the fast stochastic layer because **they are the only things that can**.
The encoder loop must target *statistics* — windowed PER, mean SNR, airtime headroom — and
never instantaneous channel state.

---

## 8. Open questions

- **O1** *(highest value)* — the `S` optimum: where does ↓k + ↑airtime-efficiency stop
  beating ↑PER + ↑fade-exposure? Sweep S × MCS × channel width.
- **O2** — measured uplink RTT distribution vs. range. This alone determines whether ARQ is
  viable at 144 fps, and where it stops being viable.
- **O3** — burst-length distribution on RTL8812 at 5.8 GHz under flight. All the binomial
  FEC math assumes independent loss; real fades are bursty. **How wrong is the independence
  assumption, and does it invalidate the `m`-sizing model?**
- **O4** — is packet-level interleaving within a frame worth it, given that intra-refresh
  already caps frame size? (Interleaving costs latency; it may be redundant here.)
- **O5** — the `m`-sizing function under bursty loss: replace the binomial tail with an
  empirical model fitted to O3.
- **O6** — does raising `S` to fit a frame in one block ever cost more (via ↑PER → ↑m) than
  simply splitting into two blocks? If so, I1 has a price and it should be known.

---

## 9. Test harness (build this first)

**Four coupled adaptive subsystems means a bad frame in flight has four plausible causes,
and you cannot bisect it in the air.** Before the controller grows:

- **Deterministic replay harness.** Recorded loss/RSSI/RTT traces in → controller decisions
  out. The loop must be testable on the bench, offline, reproducibly.
- **Trace corpus** from real flights: SNR, PER, burst lengths, RTT, frame sizes.
- **Objective metric: deadline-miss rate.** Not PSNR, not packet loss. A frame that arrives
  late is exactly as lost as a frame that never arrives, and the metric must say so.
- **Ablations:** each knob pinned in turn, to prove it earns its complexity.

Skip this and the system becomes unfalsifiable — it will get tuned by superstition.

Implementation note, given the bug history in this ecosystem (u8 sequence overflow in
wfb-ng, u8 `block_id` overflow in wfb-rs): **scrutinize every width in the block/sequence
bookkeeping.** Variable MTU and variable `k` make these fields harder to reason about, not
easier.

---

## 10. Prior art

### Closest precedent — read this first
- **Salsify: Low-Latency Network Video through Tighter Integration between a Video Codec and
  a Transport Protocol** (Fouladi, Emmons, Orbay, Wu, Wahby, Winstein — NSDI '18).
  <https://www.usenix.org/conference/nsdi18/presentation/fouladi>
  The thesis *is* this project's problem statement: codec and transport are independent
  loops that fight, and the codec's rate controller lags the network. Salsify optimizes the
  compressed length and transmission time of *each frame* against a live capacity estimate,
  rather than controlling longer-term metrics like frame rate or bit rate. Reports 3.9×
  lower delay and 2.7 dB higher quality vs. FaceTime/Hangouts/Skype/WebRTC.
  Project page: <https://snr.stanford.edu/salsify>

### FEC schemes
- **RFC 8681 — Sliding Window RLC FEC for FECFRAME** (Roca & Teibi, INRIA, Jan 2020).
  <https://datatracker.ietf.org/doc/rfc8681/> — *evaluated and rejected (§3), but the
  latency-budget framing (`max_lat`) and the GF(2)/GF(2⁸) density-threshold design are worth
  reading before rejecting it.*
- **RFC 6330 — RaptorQ FEC Scheme** (Luby, Shokrollahi, Watson, Stockhammer, Minder, 2011).
  <https://www.rfc-editor.org/info/rfc6330> — fountain code; rateless, escapes the n≤255
  ceiling, decode-failure probability near-flat in block size. **The main alternative to RS
  that preserves I1.** Qualcomm-patented. Worth a serious look if O6 turns out badly.
- **RFC 6363 — FECFRAME framework.** <https://datatracker.ietf.org/doc/rfc6363/>

### Feedback-driven error resilience
- **RFC 4585 — Extended RTP Profile for RTCP-Based Feedback (RTP/AVPF).**
  <https://www.rfc-editor.org/info/rfc4585/>
  Defines PLI, SLI, and **RPSI (Reference Picture Selection Indication, §6.3.3)**. The key
  idea: on loss, RPSI lets the sender **avoid sending a large intra-frame** and instead
  continue with inter-frames that reference an indicated known-good frame. Since this design
  already has per-frame ACK state from ARQ, RPSI-style recovery is nearly free — and it
  composes with intra-refresh rather than competing with it.

### Rate adaptation with network feedback (the link half, weaker than this design —
bitrate-only, never MCS/FEC/MTU)
- **RFC 8298 — SCReAM** (Johansson & Sarker, Ericsson, 2017). Window-based, self-clocked,
  hybrid loss+delay, **designed for mobile radio links**. Ericsson presented it for
  *remote-controlled vehicles over 4G/5G* — the closest use case in the RMCAT family.
  <https://datatracker.ietf.org/doc/html/rfc8298> · <https://github.com/EricssonResearch/scream>
- **RFC 8698 — NADA** (Zhu et al., Cisco). <https://datatracker.ietf.org/doc/html/rfc8698>
- **Google Congestion Control** — `draft-ietf-rmcat-gcc-02` (expired; de-facto spec of the
  algorithm in libwebrtc).
- **RFC 8888 — RTCP Feedback for Congestion Control.** The per-packet feedback message.

### Adaptive coding & modulation (the MCS half)
- **DVB-S2 ACM** — the canonical precedent for jointly adapting modulation *and* coding rate
  from receiver feedback.
- **LTE/5G link adaptation** — CQI → MCS selection plus HARQ. The textbook fast-inner /
  slow-outer split this design is copying.

### Theory
- **Joint source–channel coding (JSCC)** — the formal frame for the whole system: allocate
  rate between source coding and channel coding under one budget. Large wireless-video
  literature, mostly 2000s; see also the resource–distortion optimization framework
  literature for the optimality benchmark.
- **Cross-layer optimization for wireless video** — same era, explicitly PHY ↔ codec.

---

## 11. Summary of the verdict

The architecture is sound and well-precedented. Build it, but with:

1. **One** controller, not four.
2. Explicit **timescale separation** (inner/middle/outer), justified by coherence time.
3. The encoder's **internal rate control ripped out**.
4. **Intra-refresh** instead of IDRs — a precondition, not an optimization.
5. **MTU as a first-class knob**, because it is what keeps one-frame-one-block feasible
   under `n ≤ 255`.
6. **`k` in every packet header**, because detection latency is the real ARQ killer.
7. **Deadline-aware discard**, because airtime spent on a doomed frame is spent twice.
8. **FPS demoted to a last-resort knob** — and justified by *widening the ARQ deadline*, not
   by fattening frames.
9. The **replay harness before the controller**, or none of the above is falsifiable.
