# Devourer↔kernel-monitor parity — survey and working plan (2026-08-05)

**Survey and planning only. No spec ruling is made here, so there is no
`docs/review-log.md` Pass entry attached.** Every item below that touches
`PROTOCOL.md` is marked as needing an operator ruling; when one is made it
commits first as a spec amendment plus a numbered Pass, per the repo law.
Companion to `docs/devourer-revendor-review.md` (vendored-driver state) and
`docs/transport-architecture-review.md` (whose 2026-07-12 parity matrix this
supersedes for the devourer column).

**Deliberately open-ended.** Ordering, scope, and the per-item verdicts are
expected to move — several entries are gated on a bench result that has not
been read out yet (G0), and the test-surface item (G8) lands on top of the
in-flight unit-testing work rather than beside it. Treat the register as a
live working set, not a committed sequence.

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
behind a contract rather than out of a 530-line hand-dispatch. Two backends
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
| **G1** | §15.5a scout net_id retargeting is a no-op on radio | correctness | **OPEN — critical path.** Declared on `AirIface`; the fix is RX-thread synchronisation inside the backend |
| G2 | §14.2 JSCC airtime unavailable on radio | feature | RULING — declared (`estimate_airtime_us` → `nullopt`) |
| G3 | CSA retune is commanded, not confirmed, on devourer | correctness | OPEN (ungated) |
| G4 | `recover()` returns false on radio | feature | OPEN (ungated) — declared, pairs with G3 |
| G5 | `mtu_supported()` is a hardcoded assumption on radio | feature | OPEN (ungated) — declared |
| G6 | `tx_index()` returns 0 for radio | correctness | OPEN (trivial) — declared |
| G7 | `set_power_auto` semantics differ between backends | spec hygiene | RULING — declared |
| G8 | Neither RF backend has a unit test | test surface | OPEN — **harness now exists** (Pass 140 `FakeAir`) |
| **G9** | §3.11 heartbeat suppression was monitor-only | correctness | **DONE** — Pass 140; it was never in this register |
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

## G1 — §15.5a scout net_id retargeting is a silent no-op on radio

The worst gap in the set, and the only one that fails *silently* — everything
else either fails closed or degrades visibly. **On the critical path** under
the end-state ruling: devourer becomes the TX role, and the §15.5a scout is a
TX-role function.

Two things changed since this was written. It is no longer an `if (mon)` with
no `else` in `app/main.cpp` — Pass 140 moved it onto `AirIface`, so
`RadioAir::set_filter_net_id` / `set_stamp_net_id` are **declared** no-ops with
their consequence stated at the definition. And the fix is larger than a
forwarding call: `cfg.filter_net_id` is read on each adapter's **RX thread**
(`io/src/air_radio.cpp` `on_packet`), so a runtime setter is a synchronisation
change inside the backend. `MonAir` gets away with a plain setter because its
equivalent is a BPF re-attach syscall.

`app/main.cpp:1554-1559`:

```
void set_filter_net_id(std::optional<uint8_t> net_id) {
    if (mon) mon->set_filter_net_id(net_id);
}
void set_stamp_net_id(uint8_t net_id) {
    if (mon) mon->set_stamp_net_id(net_id);
}
```

No `else`. Both are called from 14 sites across the scout sweep, claim commit,
spectator tune, and CSA rollback (`app/main.cpp:4874`, `5005-5055`, `5548`,
`5660-5743`). On the radio backend every one of them does nothing:

- The RX filter never widens for a sweep, so with `node.net_id` configured a
  craft on a different net_id is **never discovered** — while the scout
  reports a clean sweep.
- After a claim, the ground keeps stamping its **boot** net_id, so the craft's
  §3.0 RX filter drops the ground's NACK/LINK_REPORT/CSA returns. ARQ and the
  CSA campaign both break, with no error surfaced at either end.

`RadioAirCfg` already carries `stamp_net_id` / `filter_net_id`
(`io/include/wblink/air_radio.h:32-33`); they are simply create-time only.
Devourer is *easier* than monitor here — `MonAir` needs an atomic plus a
`SO_ATTACH_FILTER` BPF re-attach (`io/src/air_mon.cpp:756-767`), devourer needs
only the atomic in the RX path.

**Open:** whether an interim fail-closed guard (reject a scout/quickconnect
config on `air.kind:"radio"` until the setters exist) is worth landing ahead
of the real fix. Cheap, and it converts a silent failure into a startup error.

---

## G2 — §14.2 JSCC airtime unavailable on radio

`io/src/config.cpp:926-931` rejects `airtime_efficiency_permille` for any
backend except kernel-monitor, and `AirBackend::estimate_airtime_us`
(`app/main.cpp:1537-1546`) has no radio branch — it returns `nullopt`. So on
devourer every JSCC decision falls back: `jscc_fallback_decisions` equals
decision frames and §14.2 enforcement never actuates
(`app/main.cpp:1967-1993`).

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

## G3 — CSA retune is commanded, not confirmed, on devourer

`io/src/air_radio.cpp:622-645` returns `true` unconditionally, because
devourer's `FastRetune` / `SetMonitorChannel` are `void`.
`app/main.cpp:1119-1135` documents the resulting confidence split honestly
(monitor *confirmed*, radio *commanded*), but the protocol state machine does
not distinguish them.

This is literally Critical finding #1 of
`docs/transport-architecture-review.md` — **fixed for kernel-monitor** via
Passes 49/69/80 (real `iw` retune plus the §11.6 RX-liveness guard,
soak-validated 20/20) and **still open for radio**. A devourer node can report
COMMITTED with the chip on the old channel.

The §11.6 liveness guard is backend-agnostic — it watches `rx_frames_total`
(`app/main.cpp:1494-1509`) — so most of the fix is reuse rather than new
design. Should land with G4; a retune guard with no recovery path is half a
fix.

---

## G4 — `recover_all()` returns false on radio

`app/main.cpp:1516-1525`: Pass 80's one-shot monitor re-init recovery declared
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

`app/main.cpp:1143-1148` returns `mtu_tier::kHighBudget` for radio on the
reasoning that successful `RadioAir` construction proves devourer accepted
every adapter. Kernel-monitor reads each netdev MTU and takes the minimum
(`io/src/air_mon.cpp:704-721`).

With PR #83 (negotiated MTU) merged and Passes 122/124 tuning the jumbo tier
and repair-depth guard, §9.3a on a devourer node now asserts the High budget
without evidence — and that feeds symbol sizing directly. Needs the devourer
equivalent bound (chip/USB transfer limit) identified and read, or an explicit
ruling that construction success *is* the evidence.

---

## G6 — `tx_index()` returns 0 for radio

`app/main.cpp:1564-1567` returns 0 for any non-monitor backend, so the §15.5a
scout roams adapter 0 by convention. `RadioAir` already knows `impl_->tx_idx`
(`io/src/air_radio.cpp:429`); this is an accessor plus a branch. Trivial, but
it is a real bug the moment a devourer ground lists its uplink second.

---

## G7 — `set_power_auto` semantics differ between backends

`app/main.cpp:1404-1414`: monitor calls `iw txpower auto` (driver default /
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
(`io/src/air_radio.cpp:76-81`), the `crc_err`/`icv_err` drop
(`io/src/air_radio.cpp:237`), RSSI/TSF extraction — has none.

**The harness now exists and the precondition is met.** Pass 137 made
`app/main.cpp` reachable from a test; Pass 140 put every backend behind
`AirIface` and added `FakeAir`, so `AirBackend`'s orchestration — the retune
and recovery loops — is unit-tested for the first time (`tests/app_test.cpp`,
151 checks). What G8 still wants is narrower than when it was written: the
**devourer-specific decode path** (`desc_rate_to_mcs`, the `crc_err`/`icv_err`
drop, RSSI/TSF extraction), which needs no fake because it is pure function of
a buffer. Follow the `FakeAir` shape rather than inventing a second harness.

---

## B2 — RadioAir cannot build a node without a TX adapter

`io/src/air_radio.cpp:325-334` hard-fails unless exactly one `role:"tx"`
adapter is present. `MonAir` has `allow_rx_only`
(`io/include/wblink/air_mon.h:37-40`), which `app/main.cpp:1174-1181` sets for
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

## G9 — §3.11 heartbeat suppression was monitor-only (DONE, was never listed)

**This register missed one.** `app/main.cpp`'s heartbeat guard read

    if (mon && !mon->has_tx()) return;

so a node with no TX adapter suppressed its §3.11 heartbeat **only on
kernel-monitor**. Unlike G1/G4/G5/G6 it carried no comment anywhere — it read
as a monitor implementation detail rather than a rule.

It is a rule: a node with no uplink does not beat, whichever backend it runs.
Pass 140 made it backend-agnostic via `AirIface::has_tx()`.

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

`io/src/air_radio.cpp:370-407` does `libusb_init` → `libusb_get_device_list` →
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

**Structural observation.** Closing G1/G2/G4/G5/G6 collapses most of the
`if (mon) … else if (radio) …` cascade in `AirBackend` (`app/main.cpp:1117`
onward). That is the natural moment to make the extraction a real virtual
interface rather than the current three-`optional` struct — done *as* the
parity work lands rather than as a separate refactor afterwards. Not proposed
as a commitment here; noted so the opportunity is not missed.

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

1. **G1** — critical path. The §15.5a scout is a TX-role function and this is
   the only gap that fails silently. Larger than it looks: the fix is RX-thread
   synchronisation inside `RadioAir`, not a forwarding call.
2. **G6** — trivial, and it is the same scout path as G1. Land them together.
3. **G3 + G4** together, as before: "commanded, not confirmed" and "no recovery
   path" are the same weakness seen from two sides, and a TX node is where a
   half-applied retune actually costs something.
4. **G5** — small; assert-vs-probe on a tier that is currently asserted.
5. **G2 / G7** — after their rulings. Both are declared on `AirIface` now, so
   the code change is bounded and the open part is genuinely the decision.
6. **B3** — hardware, runs in parallel.
7. **H1** — a per-unit acceptance check at the intended MCS, needed before a
   pure-devourer TX fleet ships, not before any of the above lands.
8. **G8** — narrower than when written; the devourer decode path, following the
   `FakeAir` shape rather than a second harness.

**Not on this list any more:** G0 (closed), G9 (fixed in Pass 140), B2 (dead by
ruling), H2 (settled and measured), and the interface collapse itself.

## What to re-read when a premise moves

G9 is the cautionary tale. It was invisible to the original survey precisely
because B2 was true — a radio node could not be RX-only, so the missing branch
was unreachable and looked like a monitor detail. **A dead item was hiding a
live one.** When any premise here is overturned, the items it was propping up
are worth re-walking, not just the ones it was gating.
