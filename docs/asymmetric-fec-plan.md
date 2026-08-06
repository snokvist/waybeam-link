# Asymmetric FEC for non-referenced frames — design

> **STATUS: PLAN ONLY. No code in this PR.** This documents how a third FEC
> class should be wired so it can be driven and measured later. Nothing here
> is pinned in PROTOCOL.md yet — per project law a spec amendment commits
> FIRST, so §14.1 must be amended in the implementing PR, not this one.

---

## 1. Why

The encoder can now mark half its frames as non-referenced. waybeam_venc PR
snokvist/waybeam_venc#216 adds `video0.resilience = "ltr"`, which configures
SVC-T as `InRnRnRn…` — one non-referenced frame in every two.

A **non-referenced frame is disposable by construction**: nothing predicts
from it, so losing it costs exactly one frame and the next frame is already
clean. Spending the same FEC on it as on a referenced frame buys nothing —
the parity protects a frame whose loss the decoder was going to absorb
anyway.

Today waybeam-link spends exactly the same on both. That is the gap.

### What the encoder guarantees (measured, not assumed)

From the venc PR's device measurements on Star6E, which bound what this is
worth:

- `u32Enhance` is a **period**: one non-referenced frame per `enhance + 1`.
  At `resilience=ltr` (enhance=1) that is **50 %** of frames. At `range`
  (enhance=4) only 17.6 %. At `off`, **zero** — no frame carries the flag.
- Non-referenced frames are emitted as HEVC `TRAIL_N` (type 0); referenced
  ones as `TRAIL_R` (type 1).
- The referenced half is an ordinary P-chain — it does **not** predict from
  the IDR. Losing one of those still cascades to the next IDR, so the
  referenced class must stay well protected. This is not a proposal to
  lower overall protection; it is a proposal to stop paying for the half
  that does not need it.

So the ceiling on this optimisation is: **eliminate FEC overhead on up to
50 % of frames**, and only when the craft is running `ltr`.

---

## 2. Current state

Two classes, decided in two places in `core/src/frame_framer.cpp`:

```cpp
// :41  — repair_count()
const uint32_t rate =
    is_idr ? cfg_.fec.i_rate_permille : cfg_.fec.p_rate_permille;

// :130 — jumbo_fec_guard
const uint32_t rate =
    is_idr ? cfg_.fec.i_rate_permille : cfg_.fec.p_rate_permille;
```

`is_idr` comes from `frame_blob_is_idr()`
(`core/include/wblink/frame_shm_format.h:85`).

**The flag we need is already on the wire and already parsed.** Same header,
line 58:

```cpp
inline constexpr uint8_t kFrameFlagIdr     = 0x01;
inline constexpr uint8_t kFrameFlagGdr     = 0x02;
inline constexpr uint8_t kFrameFlagEnhance = 0x04;  // SVC-T droppable layer
inline constexpr uint8_t kFrameFlagsKnown =
    kFrameFlagIdr | kFrameFlagGdr | kFrameFlagEnhance;
```

`kFrameFlagEnhance` is defined, is included in `kFrameFlagsKnown` (so it
survives validation rather than being rejected as unknown), and is read into
`VencFrameMeta.flags` by `read_frame_meta()`. There is simply no accessor for
it and no policy that consults it.

**This is therefore a small change, not new plumbing.** No wire-format
change, no venc change, no protocol version bump.

---

## 3. Proposed wiring

### 3.1 Accessor

Mirror the existing IDR accessor in `frame_shm_format.h`:

```cpp
// True if the blob is a non-referenced (SVC-T droppable) frame. Nothing
// predicts from it, so its loss costs exactly one frame (§14.1 FEC class).
inline bool frame_blob_is_enhance(const uint8_t* blob, size_t len) {
    VencFrameMeta m;
    return read_frame_meta(blob, len, &m) &&
           (m.flags & kFrameFlagEnhance) != 0;
}
```

An IDR is never marked enhance, so the classes are disjoint. Classify in
priority order **IDR → enhance → P** and assert the disjointness rather than
relying on it.

### 3.2 Config

Add one optional field to `StreamFecCfg` (`io/include/wblink/config.h:34`):

```cpp
struct StreamFecCfg {
    FecScheme scheme = FecScheme::kNone;
    uint16_t i_rate_permille = 250;
    uint16_t p_rate_permille = 100;
    // §14.1 non-referenced (SVC-T droppable) frames. UNSET => inherit
    // p_rate_permille, i.e. today's behaviour bit-for-bit.
    std::optional<uint16_t> e_rate_permille;
    uint16_t min_k = 3;
    uint16_t min_r = 2;
};
```

`std::optional` is load-bearing: **unset must be indistinguishable from
today**, so a config that predates this feature, or a craft not running
`ltr`, behaves exactly as it does now. Do not default it to 0 — that would
silently strip protection from any existing deployment the moment the craft
switches preset.

Parse in `io/src/config.cpp` beside the existing rates (~:313), and extend
the runtime `/api/v1/fec` handler (`io/src/control_server.cpp:528`) with an
`e_permille` parameter so it can be swept on the bench without a restart.

### 3.3 The `min_r` trap — the one subtlety worth getting right

`repair_count()` applies a floor **after** the rate:

```cpp
if (rate == 0) { return 0; }           // short-circuits BEFORE the floor
uint32_t r = (k * rate + 999u) / 1000u;
if (r < cfg_.fec.min_r) { r = cfg_.fec.min_r; }
```

So:

- `e_rate_permille = 0` → **genuinely zero parity**. The `rate == 0` branch
  returns before `min_r` can raise it. This is the configuration that
  actually realises the saving.
- `e_rate_permille = 10` (a "small but nonzero" value) → on a k=20 frame,
  `ceil(0.2) = 1`, floored up to `min_r = 2` — an **effective 100 ‰**, i.e.
  the same as the P rate it was meant to undercut.

**Document that the useful settings are 0 or "clearly above `min_r/k`".**
Anything in between is silently dominated by the floor. If graduated low
rates turn out to be wanted, `min_r` needs a per-class variant — do not
special-case it inside `repair_count()`.

The `jumbo_fec_guard` at `:130` has the same `rate != 0` guard, so it
inherits this behaviour consistently; it needs the same three-way rate
selection and nothing more.

### 3.4 ARQ

Under `arq_mode: "all-frames"`, non-referenced frames currently become
`pframe_arq` eligible (`frame_framer.cpp:105`). **Retransmitting a
non-referenced frame is close to pure waste** — by the time the repair
lands, the frame it would fix has been superseded and the decoder has
already moved on cleanly. It spends airtime and adds a return-path round
trip for no visible benefit.

Proposal: exclude enhance frames from `pframe_arq` regardless of
`arq_mode`. This does not need a new mode — `kIdrOnly` already excludes
them (it excludes all P-frames), so the change is confined to the
`kAllFrames` branch.

Note the interaction at `frame_framer.cpp:37`: the `min_k` gate skips FEC
when `arq_eligible`, on the reasoning "don't spend parity where ARQ will
recover it anyway". Once enhance frames are ARQ-ineligible, that gate stops
firing for them — which is correct, but means a small enhance frame that
previously got ARQ-only now falls through to the `e_rate` path. With
`e_rate = 0` it ships bare, which is the intent, but it must be a
**deliberate, tested** outcome and not a surprise. This is the same class of
bug as B11 in the §14.1 comment history.

### 3.5 Observability

Without a counter this feature is invisible until someone notices artefacts.
Add to `stats.h` / `stats.cpp`:

- `fec_enhance_frames` — frames classified non-referenced
- `fec_enhance_bytes_saved` — parity bytes not emitted vs. the P rate

The first is the drift detector: **if the operator configures a low
`e_rate` while the craft is on `off` or `range`, this counter reads zero (or
17.6 %) instead of ~50 %,** and the misconfiguration is visible rather than
silent. The venc and link settings must be chosen together, and nothing
enforces that across the two processes — this counter is what makes the
mismatch observable.

---

## 4. What this is worth, honestly

Bounded by how many frames carry the flag:

| venc preset | frames flagged | max parity saving |
|---|---|---|
| `off` | 0 % | none — feature inert |
| `range` (enhance=4) | 17.6 % | small |
| `ltr` (enhance=1) | **50 %** | up to half the P-class parity |

At the shipped defaults (`p_rate = 100 ‰`) halving the protected frame count
saves roughly **5 % of stream bytes** — not transformative on its own. It
becomes interesting in the configuration the venc PR was built for: keep a
**long GOP**, spend the freed budget on a **much higher `i_rate`** so the IDR
is very hard to lose, and accept that the referenced half is protected at the
current P rate.

That is the actual thesis to test, and it is a **link-side budget
reallocation**, not a bitrate saving. It should be evaluated as "glitch rate
and glitch duration at fixed airtime", not as "bytes saved".

---

## 5. Verification plan

1. **Unit** — classification is disjoint and correctly prioritised
   (IDR / enhance / P); `e_rate` unset reproduces current `repair_count()`
   output for every `(k, is_idr)` pair; `e_rate = 0` yields `r == 0` and is
   not resurrected by `min_r` or the jumbo guard.
2. **Bench, no RF** — craft on `resilience=ltr`, inject loss with the
   existing `rx_drop_permille` hook (`io/src/air_mon.cpp:442`). Sweep
   `e_rate` ∈ {unset, 0} × `i_rate` ∈ {250, 500, 750}. Record
   `fec_enhance_frames` to confirm ~50 % classification before trusting any
   other number.
3. **Metric** — glitch *duration* distribution, not loss count. The whole
   claim is that a lost enhance frame is a 1-frame glitch while a lost
   referenced frame is a cascade to the next IDR; only a duration histogram
   distinguishes those.
4. **Guard** — confirm a craft on `resilience=off` with `e_rate = 0`
   configured shows `fec_enhance_frames == 0` and byte-identical airtime to
   today. That is the "operator misconfigured the pair" case, and it must be
   safe.

---

## 6. Open questions

- Should `e_rate` be per-stream (as drafted) or global? Per-stream matches
  the existing `StreamFecCfg` shape; no current topology needs it to differ.
- Does the §14.2 JSCC shadow controller need to know about the third class,
  or does it stay a two-class controller with enhance frames simply excluded
  from its accounting? Leaning excluded — it optimises a loss/latency
  tradeoff that does not apply to a frame nothing depends on.
- Is `kFrameFlagGdr` worth a fourth class later? Out of scope here, and
  `ltr` forces intra-refresh off, so the two do not co-occur in the target
  configuration.
