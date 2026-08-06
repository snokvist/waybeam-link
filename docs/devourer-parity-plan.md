# Devourer↔kernel-monitor parity — survey and working plan (2026-08-06)

**Survey and planning only. No spec ruling is made here, so there is no
`docs/review-log.md` Pass entry attached.** Every item below that touches
`PROTOCOL.md` is marked as needing an operator ruling; when one is made it
commits first as a spec amendment plus a numbered Pass, per the repo law.
Companion to `docs/devourer-revendor-review.md` (vendored-driver state) and
`docs/transport-architecture-review.md` (whose 2026-07-12 parity matrix this
supersedes for the devourer column).

**Deliberately open-ended.** Ordering, scope, and the per-item verdicts are
expected to move; the test-surface item (G8) lands on top of the in-flight
unit-testing work rather than beside it. Treat the register as a live working
set, not a committed sequence.

**File:line citations below are as of `main` at Pass 142.** They have already
been invalidated once wholesale (Pass 140 moved ~25 methods off `AirBackend`
onto `AirIface`), so prefer the symbol name over the number when they
disagree.

## Why this document exists

The end-state was originally written here as a **pure devourer**
waybeam-link — one RF backend, kernel-monitor removed. **That is no longer the
plan (operator, 2026-08-06).** kernel-monitor is kept as the spectator / RX /
Ethernet-cache backend; devourer takes the **TX role**. Both backends ship.

What survives unchanged is the reason the document exists: the devourer backend
must be feature-equivalent to kernel-monitor **on the node classes it will
run** — which, under the ruling, means the TX role. It is not, and the gaps
were not recorded in one place. This is that place.

The library ambition is unchanged and is now closer: `AirIface` (Pass 140) is
the extraction boundary, and it exists precisely so a backend can be lifted out
behind a contract rather than out of a 632-line hand-dispatch (516 after). Two backends
also make that boundary honest — an interface with a single implementation
proves nothing.

## Standing position

The §3.0 on-air encapsulation is backend-agnostic and interoperable — either
backend's frames decode on either backend's RX (Pass 13, reinforced by Pass
118's per-packet radiotap MCS convergence). **Migration can therefore be
incremental and mixed-fleet**; no flag day is required, and a node can be
moved and moved back. Every item below is independently landable.

**The end-state is now a ruling, and it changes how this register is ordered
(operator, 2026-08-06).** kernel-monitor is **not** being stripped: it is kept
as the spectator / RX / Ethernet-cache backend. Devourer becomes the **TX
role**. That single sentence sorts the whole register, because every remaining
devourer gap is TX-role functionality — scout net_id retargeting (G1), wedge
recovery (G4), scout `tx_index` (G6), MTU tier (G5), airtime (G2), power-auto
semantics (G7). Monitor keeps everything it needs for the roles it is retained
for, so "which of these matter" is answered by "does the TX role need it",
not by G0-gating. **G1 is therefore on the critical path**, not a side item.

This register was written before that ruling and before G0 read out. It has
been realigned rather than rewritten: items whose premise moved say so.

## Already at parity — do not re-derive

| Capability | State |
|---|---|
| §3.0 wire / encapsulation | Backend-agnostic, interoperable (Pass 13) |
| Rate actuation + RX MCS attribution | Per-packet radiotap on both; `SetTxMode` is a rate-less-prefix fallback. Bench-verified 2026-08-01, both backends, both directions, `rx_mcs_unknown = 0` (Pass 118, `docs/step11-bench.md` §4.9) |
| TX power actuation | One seam (Pass 114): radio → `SetTxPowerOffsetQdb`, monitor → `iw txpower fixed` |
| TX-wedge watchdog | Both: CCX `tx.report` (radio) / netdev `tx_packets` (monitor), §9.10 |
| §11.6 post-retune RX flush | Both, generation-stamped |
| Diversity threading, return counters, adapter stats | Both |

Devourer is **ahead** of kernel-monitor on RX signal quality: `evm[4]`,
`cfo_tail`, `snr[4]`, and the windowed `GetRxQuality()` fusion, plus `rx_dead`
(Pass 101, RadioAir-only). `docs/per-mcs-per-ladder-plan.md` §2 records that
EVM has no radiotap field and therefore no kernel-monitor path at all. Moving
to pure devourer is not a lateral move for the §9.4 selector — it unlocks a
promote gate the monitor path can never have. That is upside to schedule
separately, not parity work.

## Register

Status values: OPEN, RULING (needs an operator decision before code), DONE.
**Nothing is gated on G0 any more — it read out.** Ordering is now by whether
the TX role needs the capability.

| id | item | kind | status |
|---|---|---|---|
| **G0** | Close the 5805 MCS4+ pivot rationale | verification | **DONE** — Pass 139 / #89, closed *affirmatively* |
| **G1** | §15.5a scout net_id retargeting is a no-op on radio | correctness | **DONE** (Pass 142, #91) — atomic filter + device A/B; uncovered and fixed a missing `AirRxMeta::net_id` |
| G2 | §14.2 JSCC airtime unavailable on radio | feature | RULING — declared (`estimate_airtime_us` → `nullopt`) |
| G3 | CSA retune is commanded, not confirmed, on devourer | correctness | **DONE** (Pass 143) — `GetSelectedChannel()` read-back; driver-level, not RF |
| G4 | `recover()` returns false on radio | feature | **DONE** (Pass 143) — RX-loop restart; `InitWrite` is one-shot so it is not a MAC/PHY bring-up |
| G5 | `mtu_supported()` is a hardcoded assumption on radio | feature | OPEN (ungated) — declared |
| G6 | `tx_index()` returns 0 for radio | correctness | **DONE** (Pass 142, #91) |
| G7 | `set_power_auto` semantics differ between backends | spec hygiene | RULING — declared |
| G8 | Neither RF backend has a unit test | test surface | OPEN — **harness now exists** (Pass 140 `FakeAir`) |
| **G9** | §3.8 heartbeat suppression was monitor-only | correctness | **DONE** (guard unified, Pass 140) **+ RULING** — §3.8 does not say a TX-less node suppresses; and it cannot fire on radio |
| B2 | RadioAir cannot build a node without a TX adapter | blocker | **DEAD** — operator ruling: enumerate and decline to inject |
| B3 | The Ethernet cache runs an MT7921 | blocker (hardware) | RULING — BOM, not code |
| **H1** | Some 8822e units cannot transmit 64-QAM under devourer | hardware | **OPEN** — per-unit, not per-chip; see below |
| **H2** | Carrier-sense posture is a devourer-only knob | settled | **DONE** — Pass 139, measured; `air.disable_cca` ships `false` |

**"Declared" means the limit is now stated on `AirIface`** (Pass 140) rather
than being the absence of a branch in `AirBackend`. That does not close the
item — the capability is still missing — but the answer lives where a test can
assert it, and closing one flips a test rather than going unnoticed.

---

## G0 — 5805 MCS4+ pivot rationale: CLOSED AFFIRMATIVELY

**Read out on hardware 2026-08-05 (Pass 139, PR #89). Devourer transmits
MCS4–7 at 5805.** The vehicle's 8822e delivered **98.21 / 96.42 / 95.87 /
98.21 %** at MCS4/5/6/7 through devourer carrying real venc video, measured
against a constant kernel-monitor reference receiver with `rx_at_other_rates`
zero in every window. Re-measured on the re-vendored `800c3c8` tree: at or
indistinguishable from lossless, ≥98 % throughout.

`docs/mon-air-verification.md` carries a refutation banner on its "Why the
pivot" section. The backend itself keeps its separate and still-valid
rationale — a real RF path with no devourer or libusb at `WBLINK_RADIO=OFF` —
which was never in question and is reinforced by the end-state ruling above.

**The correction worth keeping.** The first reading of this experiment was
"the 8822e TX path is broken under devourer, exclude it from TX roles" — wrong,
and wrong in the expensive direction, since it would have become a BOM
constraint on the whole end-state. It came from testing **one adapter** and
generalising to a chip. A direction-swap to a different chip (8822c) looked
like a control but only established that *some* adapter works; the
discriminating test was **the same chip in a different unit**. Two adapters of
the same part number are not a replicate. What survives as a real finding is
H1 below.

---

## G1 — §15.5a scout net_id retargeting is a silent no-op on radio — **DONE (Pass 142)**

> Landed in #91. The filter moved out of `cfg` into an atomic read per frame
> in `on_packet` (`-1` = hear-any, the `MonAir` encoding); the stamp stayed a
> plain main-thread write. Device A/B on two mismatched-net_id devourer nodes:
> before, 0 candidates; after, the craft found at its real net_id — and the
> ear filters it again once the sweep stops.
>
> **The survey missed half of it.** `RadioAir` never filled
> `AirRxMeta::net_id`, so the first working sweep reported the craft at net_id
> 0. Invisible while the filter was pinned; wrong the moment it widened. Since
> selection re-pins both roles to the *reported* value, the scout would have
> claimed 0 and lost the craft it had just found. Only the device test could
> surface it — a register item's stated fix can be necessary and still not be
> sufficient.

**What it was.** The worst gap in the set, and the only one that failed
*silently* — everything else fails closed or degrades visibly. Both net_id
setters did nothing on radio, so a sweep never widened (a craft on another
net_id was never discovered, while the scout reported clean) and a claim left
the ground stamping its **boot** net_id, which the craft's §3.0 filter then
dropped — breaking ARQ and the CSA campaign with no error at either end. The
`if (mon)`-with-no-`else` shape was removed by Pass 140; Pass 142 supplied the
answer behind the declaration.

**The interim fail-closed guard** this section proposed — reject a
scout/quickconnect config on `air.kind:"radio"` — is moot and was never landed.

**What it left behind (new, open).** Making the setters live exposed two
callers that were previously unreachable on radio:

- **A claim during an in-flight sweep is unguarded.** `do_claim` does not stop
  the scout, so a sweep completing afterwards calls `rest()`, which restores
  both the resting filter *and* the resting channel — clobbering the claim
  mid-campaign. Pre-existing on kernel-monitor; now live on radio. Wants a
  `scout.stop()` in the claim path, or an explicit rejection.
- **The widen is node-wide, but §15.5a scopes it to the scout adapter.** Both
  backends widen every ear (`MonAir` re-attaches BPF per adapter; `RadioAir`
  shares one atomic across RX threads). The scout *survey* is correctly scoped,
  but the §2 selector, the CSA follower and the discovery table are not, so a
  diversity ear on the resting channel admits foreign-net_id traffic for the
  sweep's duration. **RULING** — either scope the widen per adapter or amend
  §15.5a to describe what both backends actually do.

---

## G2 — §14.2 JSCC airtime unavailable on radio

`io/src/config.cpp:1013-1018` rejects `airtime_efficiency_permille` for any
backend except kernel-monitor, and `AirBackend::estimate_airtime_us`
(`app/main.cpp:1630-1635`) has no radio branch — it returns `nullopt`. So on
devourer every JSCC decision falls back: `jscc_fallback_decisions` equals
decision frames and §14.2 enforcement never actuates
(`app/main.cpp:2076-2080`).

The model itself is already backend-agnostic —
`ht20_service_time_us` (`io/include/wblink/airtime.h`) is pure. The work is
plumbing plus **one ruling**: what replaces the `SIOCOUTQ` pending-bytes term
that `MonAir::estimate_airtime_us` uses (`io/src/air_mon.cpp:724-745`).
Candidates, not a recommendation:

1. Devourer USB submit-queue depth, if it can be read cheaply per call.
2. Pass `include_pending=false` on radio and document the precision loss.
3. Something derived from `tx_submitted - tx_reports` (the wedge delta).

Pass 56 authored the monitor service model as an explicit opt-in calibration
rather than an optimistic default; whatever lands here should keep that
posture — a devourer node with no calibration should read *unavailable*, not
*estimated*.

---

## G3 — CSA retune is commanded, not confirmed, on devourer — **DONE (Pass 143)**

> Closed in #92. `retune()` reads `GetSelectedChannel()` back and fails on a
> mismatch. The read-back is the driver's own record, so it catches a refused
> or misrouted call and *not* a chip that disagrees with its driver — the same
> rung `iw` puts kernel-monitor on. RF-level confirmation stays with the
> §11.6 guard, which was always backend-agnostic.


`RadioAir::retune` (`io/src/air_radio.cpp:650-681`) returns `true` unconditionally, because
devourer's `FastRetune` / `SetMonitorChannel` are `void`.
`app/main.cpp:1324-1329` documents the resulting confidence split honestly
(monitor *confirmed*, radio *commanded*), but the protocol state machine does
not distinguish them.

This is literally Critical finding #1 of
`docs/transport-architecture-review.md` — **fixed for kernel-monitor** via
Passes 49/69/80 (real `iw` retune plus the §11.6 RX-liveness guard,
soak-validated 20/20) and **still open for radio**. A devourer node can report
COMMITTED with the chip on the old channel.

The §11.6 liveness guard is backend-agnostic — it watches `rx_frames_total`
(`app/main.cpp:1595-1602`) — so most of the fix is reuse rather than new
design. Should land with G4; a retune guard with no recovery path is half a
fix.

---

## G4 — `recover()` returns false on radio — **DONE (Pass 143)**

> Closed in #92, and smaller than this section assumed. **`InitWrite` is
> one-shot**: it unconditionally assigns `_coex_thread`
> (`jaguar3/RtlJaguar3Device.cpp:207`), so a second call destroys a joinable
> thread and terminates the process — found by doing it. `recover()` is
> therefore an RX-loop restart (`StopRxLoop` → join → `SetMonitorChannel` →
> `StartRxLoop`), not the MAC/PHY bring-up kernel-monitor gets. The question
> this section raised — whether an in-process re-init clears an RTL88x2 USB
> wedge — is still **unmeasured**, and a full bring-up would need vendored
> changes (upstream, not here). `AdapterHealth`'s probes were surveyed as
> suggested; see the H1 note in Pass 143.


`app/main.cpp:1606-1625` (`recover_all`) / `io/src/air_radio.cpp:710-715`: Pass 80's one-shot monitor re-init recovery declared
devourer out of scope.

Devourer has the primitives. `IRtlDevice::StartRxLoop` is documented
restartable and `StopRxLoop` asks a running loop to exit
(`third_party/devourer/src/IRtlDevice.h:55-66`), with `InitWrite` available for
the bring-up half — so a `RadioAir::recover()` is constructible without
touching vendored code.

**Worth measuring before designing:** `CLAUDE.md` records that RTL88x2 USB
wedges (RX counter frozen) need a *physical re-plug*, not a driver reload.
Open question is whether a devourer-level RX-loop re-init clears the same
condition, whether `libusb_reset_device` is required, or whether the failure
is genuinely below both. `third_party/devourer/src/AdapterHealth.h` describes a
related "enumerates fine, radio stone-deaf" failure mode with probes
(`ProbeEfuseStability`, `GetFwBootStatus`) that may be the better detector —
survey it before writing a recovery path.

---

## G5 — `mtu_supported()` is a hardcoded assumption on radio

`AirBackend::mtu_supported()` (`app/main.cpp:1335`) delegates; the assertion is `io/src/air_radio.cpp:750-754` on the
reasoning that successful `RadioAir` construction proves devourer accepted
every adapter. Kernel-monitor reads each netdev MTU and takes the minimum
(`io/src/air_mon.cpp:704-721`).

With PR #83 (negotiated MTU) merged and Passes 122/124 tuning the jumbo tier
and repair-depth guard, §9.3a on a devourer node now asserts the High budget
without evidence — and that feeds symbol sizing directly. Needs the devourer
equivalent bound (chip/USB transfer limit) identified and read, or an explicit
ruling that construction success *is* the evidence.

---

## G6 — `tx_index()` returns 0 for radio — **DONE (Pass 142)**

Returned 0 for any non-monitor backend, so the §15.5a scout roamed adapter 0 by
convention — a real bug the moment a devourer ground lists its uplink second.
`create()` already resolved it into `impl_->tx_idx`. Landed with G1 in #91.

---

## G7 — `set_power_auto` semantics differ between backends

`io/src/air_radio.cpp:717-722`: monitor calls `iw txpower auto` (driver default /
per-rate TXAGC curve); radio calls `set_power_qdb(adapter, 0)` — offset zero
relative to the efuse-calibrated per-rate table. Probably the correct §10.5
semantic for devourer, but it is an undeclared asymmetry currently living in a
`main.cpp` comment. Wants a line in the spec, or an explicit ruling that the
two are equivalent for §10.5 purposes.

---

## G8 — neither RF backend has a unit test

No `air_radio_test.cpp` / `air_mon_test.cpp`. Kernel-monitor at least has its
RX parsing covered by `radiotap_test.cpp` and the shared encapsulation by
`dot11_test.cpp`; the devourer-specific path — `desc_rate_to_mcs`
(`io/src/air_radio.cpp:77-82`), the `crc_err`/`icv_err` drop
(`io/src/air_radio.cpp:249`), RSSI/TSF extraction — has none.

**The harness now exists and the precondition is met.** Pass 137 made
`app/main.cpp` reachable from a test; Pass 140 put every backend behind
`AirIface` and added `FakeAir`, so `AirBackend`'s orchestration — the retune
and recovery loops — is unit-tested for the first time (`tests/app_test.cpp`,
150 checks). What G8 still wants is narrower than when it was written: the
**devourer-specific decode path** (`desc_rate_to_mcs`, the `crc_err`/`icv_err`
drop, RSSI/TSF extraction), which needs no fake because it is pure function of
a buffer. Follow the `FakeAir` shape rather than inventing a second harness.

---

## B2 — RadioAir cannot build a node without a TX adapter

`io/src/air_radio.cpp:336-349` hard-fails unless exactly one `role:"tx"`
adapter is present. `MonAir` has `allow_rx_only`
(`io/include/wblink/air_mon.h:41`), which `app/main.cpp:1366-1368` sets for
two node classes:

- the **store-only Ethernet cache** (`cfg.cache.store.enabled && streams.empty()`)
- the **§2/§13 passive spectator** (Pass 74) — a display receiver with no
  return path

**DEAD — operator ruling, 2026-08-06.** This was never a hardware or driver
limit: a devourer node can enumerate its adapters and simply decline to use the
TX injection path, which makes it RX-only. The item is a construct of the
current `create()` guard, and the guard itself stays deliberate (ordinary
ground/craft configs must fail closed when their uplink is missing), so what
remains is the same opt-in flag `MonAir` already has — not a blocker, and not
gating anything.

One consequence to carry: **`heartbeat` (G9) depended on this being true.** Its
guard was monitor-only, and the reason it was harmless is that a radio node
could not be RX-only. Once one can, the guard had to become backend-agnostic —
which Pass 140 did.

---

## G9 — §3.8 heartbeat suppression was monitor-only (DONE, was never listed)

**This register missed one.** `app/main.cpp`'s heartbeat guard read

    if (mon && !mon->has_tx()) return;

so a node with no TX adapter suppressed its §3.8 heartbeat **only on
kernel-monitor**. Unlike G1/G4/G5/G6 it carried no comment anywhere — it read
as a monitor implementation detail rather than a rule.

Pass 140 made the *guard* backend-agnostic via `AirIface::has_tx()`, which is
the part that is genuinely done. Two things this register asserted are not:

- **"It is a rule" is an inference, not law.** §3.8 says "**every node** emits
  HEARTBEAT at 1 Hz while otherwise quiet", and names grounds and quiet rx
  nodes as the ones the HEARTBEAT path serves. It says nothing about a node
  with no TX adapter suppressing it. Suppression may well be right — a node
  that cannot transmit cannot beat — but the spec does not say so, and this
  repo's law is that a gap is an operator ruling. **RULING** wanted, then a
  §3.8 amendment.
- **It cannot fire on radio.** `RadioAir::has_tx()` returns `true`
  unconditionally, because `create()` requires exactly one `role:"tx"`
  adapter. So the unified guard is unified in shape only; the moment the
  RX-only devourer config shape described under B2 lands, the heartbeat is
  *not* suppressed there. G9 is closed for monitor and udp and carries a live
  follow-up for radio — the same "a dead item was hiding a live one" pattern
  this document closes with, reproduced inside the entry that names it.

Worth recording *why* the register missed it: the item only becomes reachable
once B2 is false. While a radio node could not be built without a TX adapter,
the missing branch was unreachable and therefore invisible to a survey that
walked the backends looking for divergence. **A dead item was hiding a live
one** — which is an argument for re-reading a register after any of its
premises is overturned, not just the gating one.

---

## H1 — some 8822e units cannot transmit 64-QAM under devourer

Not a chip property, not a band property, not a driver-version property: a
**per-unit** one. It is recorded here because a pure-devourer TX fleet has to
know that units vary.

- The **craft's** 8822e delivers MCS4–7 at 95.9–98.2 % through devourer at
  5805 with real video.
- A **bench** 8822e — same chip, same `rfe_type=0x15`, same DPK-bypass and eFEM
  path, same binary — delivers MCS4 and then collapses: MCS5/6/7 at ~1.7 / 0.5 /
  0.1 %. The **same physical adapter** does 99.96 % at MCS7 under the *kernel*
  driver, so its radio is sound.

Ruled out by measurement, each separately: warm re-init (a true VBUS cold cycle
to full de-enumeration), devourer version (upstream `800c3c8`, 21 commits on),
TX power configuration, link budget and RX overload (the craft *works* at
−27 dBm, weaker than the bench arm failing at −22), RFE variant, **a physical
re-plug**, and **the kernel driver having touched the chip at all** (`8812eu`
blacklisted so it cannot auto-load, port VBUS-cycled, chip verified to come
back with no module loaded and no driver bound — fails identically).

The only surviving difference is per-unit efuse calibration: bench
`ref A=0x43 B=0x42` against craft `A=0x48 B=0x3f`. Two further observations
from the re-vendored tree point the same way — the retry ladder now **delivers
the fallback retries** (`rx_at_other_rates` ≈14–16 k in every failing window),
so the radio is transmitting and only the dense constellations are
unrecoverable; and the *virgin* chip is **worse** at MCS4 than one the kernel
driver initialised first, which is what an incomplete per-unit calibration in
devourer looks like and the opposite of what driver interference would predict.

**What this means for the end-state:** a pure-devourer TX fleet needs a
per-unit acceptance check at the intended MCS, not a per-model assumption. It
is not a reason to avoid devourer, and it is emphatically not the BOM exclusion
the first reading of G0 proposed.

---

## H2 — carrier-sense posture is a devourer-only knob (DONE)

waybeam-link never called `SetCcaMode` and never set `tuning.disable_cca`, so
it **inherited** devourer's carrier-sense-enabled default. Measured on the
craft (Pass 139): clearing the gate costs ~45 % of the **uplink** — returns
sent ~760 per window either way, craft heard 556/588 with the gate on
(73.3/77.0 %) against 331/318 with it off (43.2/41.7 %); downlink unaffected.

RX is not deafened, it is **talked over**: the craft is half-duplex on one
radio, so transmitting is not listening, and carrier-sense is what holds TX off
while the ground is mid-return. devourer's own `streamtx` example takes the
opposite posture because it is a **TX-only streamer with no return path** —
"the link owns the channel" costs it nothing, and does not describe us.

`air.disable_cca` ships **false**. Listed here because it is a devourer-only
surface that a monitor-only fleet never had to decide.

---

## B3 — the Ethernet cache runs an MT7921

`docs/verification-hardware.md:15`: cache `.247` carries an MT7921
(`14c3:7961`, `mt7921e`) alongside an RTL8812AU. Devourer is Realtek-only, so
that adapter can never be driven by it.

A pure-devourer fleet therefore requires either Realtek-only hardware on the
cache node or a permanent kernel-monitor exception there. This is a BOM /
fleet decision rather than code, but it needs making **explicitly** — "two
moving parts" implies no kernel-monitor fallback anywhere, and this is the one
node where the hardware currently forbids that.

Note the AU on the same host is Jaguar1, which devourer does support — so the
question is narrowly about the MT7921 leg, not the whole node.

---

## Library extraction — notes carried forward

Not parity work, but the reason parity matters. Recorded here so it is not
re-derived.

`core/` is already dependency-free and 32-bit clean (`CLAUDE.md`), and is the
piece intended for vendoring. The POSIX assumptions live in `io/`, and the
devourer-specific one is **device acquisition**:

`io/src/air_radio.cpp:390-407` does `libusb_init` → `libusb_get_device_list` →
`libusb_open`, matching VID/PID and an `lsusb -t`-style bus path. **Unrooted
Android cannot enumerate usbfs** — it must take a file descriptor from the Java
`UsbManager` and call `libusb_wrap_sys_device()`. The vendored libusb has that
entry point (`third_party/libusb-cmake/libusb/libusb/core.c:1406`), so
`RadioAir` needs an alternate construction path accepting a pre-opened handle
rather than a bus path. Two smaller notes:

- devourer's `claim_interface_then_reset` already parameterizes `lock_dir`
  (`third_party/devourer/src/UsbOpen.h`), so `UsbDeviceLock`'s `/tmp` default
  is not a blocker on a platform without `/tmp`.
- Its kernel-driver-detach step and `libusb_reset_device` behaviour both need
  checking under a wrapped fd.

**Structural observation — landed ahead of the parity work.** This section
predicted that closing G1/G2/G4/G5/G6 would collapse the `if (mon) … else if
(radio) …` cascade in `AirBackend`, and proposed doing the virtual-interface
extraction *as* those items landed. Pass 140 did it first and independently:
`AirBackend` now holds one `std::unique_ptr<AirIface>` (`app/main.cpp:1296`)
and the contract is all-pure-virtual. What survives of the cascade is the
§15.3 per-adapter counters fill (`app/main.cpp:1701-1780`), which none of
G1–G6 touches — the two backends' counter structs differ by real fields, so
reconciling them is a §15.3 schema question, deliberately left open.

## Adjacent, deliberately out of scope

- The in-flight uplink TX-power calibration work (PRs #84/#85, Passes
  131–135) rides the Pass 114 seam that already covers both backends and is
  substantially orthogonal to this plan. **One touchpoint worth a look before
  it merges:** `calib_identity()` (`io/src/calib_store.cpp:72-86`) returns
  `ifname/MAC` when `ifname` is set and `bus/<path>` otherwise, so the same
  physical adapter gets a different identity per backend. That is fail-safe in
  the right direction — a monitor-calibrated curve reads STALE on devourer
  rather than being silently applied, which is correct given `iw txpower fixed
  <mBm>` and `SetTxPowerOffsetQdb` (relative to the efuse per-rate table) are
  different actuators needing different curves. The concern is that the
  devourer identity is a **bus path**, and `CLAUDE.md` records that bus paths
  shuffle after any re-plug. On a pure-devourer fleet every artifact would go
  stale on a re-plug. Worth resolving to something stable (chip MAC via efuse)
  before calibration becomes load-bearing.
- The devourer feature surface surveyed in `docs/devourer-revendor-review.md`
  (LDPC, DIS_CCA, per-packet TX power on Jaguar3, `FASTRETUNE_FW`, chanmig) is
  *upside* available once the backend moves, not parity. Its Open decisions
  1–4 stay where they are.
- Rewiring §9.4 onto EVM / `GetRxQuality()` is the largest piece of that
  upside and wants its own pass; `docs/per-mcs-per-ladder-plan.md` §5 already
  flags it as "the first thing to revisit if the backend moves fully to
  devourer".

## Sketch of an order

Realigned 2026-08-06. G0 is closed and the `AirBackend` interface collapse has
landed (Pass 140), so the two items that used to sit at either end of this list
are gone. Ordering is now by **what the TX role needs**, since that is the role
devourer is taking.

1. **G5** — small; assert-vs-probe on a tier that is currently asserted.
2. **G2 / G7** — after their rulings. Both are declared on `AirIface` now, so
   the code change is bounded and the open part is genuinely the decision.
3. **The G1 leftover**: the unguarded claim-during-sweep race. The widen-scope
   question was ruled node-wide (Pass 143, §15.5a amended); what stays open is
   that the §2 selector, CSA follower and discovery view are not net_id-scoped
   during a sweep.
4. **B3** — hardware, runs in parallel.
5. **H1** — a per-unit acceptance check at the intended MCS, needed before a
   pure-devourer TX fleet ships, not before any of the above lands.
6. **G8** — narrower than when written; the devourer decode path, following the
   `FakeAir` shape rather than a second harness.

**Not on this list any more:** G0 (closed), G1/G6 (Pass 142), G3/G4 (Pass 143), G9 (fixed in
Pass 140), B2 (dead by ruling), H2 (settled and measured), and the interface
collapse itself.

## What to re-read when a premise moves

G9 is the cautionary tale. It was invisible to the original survey precisely
because B2 was true — a radio node could not be RX-only, so the missing branch
was unreachable and looked like a monitor detail. **A dead item was hiding a
live one.** When any premise here is overturned, the items it was propping up
are worth re-walking, not just the ones it was gating.
