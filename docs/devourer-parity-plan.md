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

The end-state under consideration is a **pure devourer** waybeam-link: one RF
backend, extractable as a portable library (Android `:wifi` being the first
non-Linux consumer), and ultimately integratable as a module inside
`waybeam-hub` so the deployed system reduces to `waybeam_venc` +
`waybeam-hub`. That end-state requires the devourer backend to be at least
feature-equivalent to kernel-monitor on every node class the fleet runs.

Today it is not, and the gaps are not recorded in one place. This is that
place.

## Standing position

The §3.0 on-air encapsulation is backend-agnostic and interoperable — either
backend's frames decode on either backend's RX (Pass 13, reinforced by Pass
118's per-packet radiotap MCS convergence). **Migration can therefore be
incremental and mixed-fleet**; no flag day is required, and a node can be
moved and moved back. Every item below is independently landable.

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

Status values: OPEN, GATED (blocked on another item), RULING (needs an
operator decision before code), DONE. Nothing here is scheduled until G0
reads out.

| id | item | kind | status |
|---|---|---|---|
| **G0** | Close the 5805 MCS4+ pivot rationale | verification | **OPEN — gates everything else** |
| **G1** | §15.5a scout net_id retargeting is a silent no-op on radio | correctness | **OPEN — live bug, not gated on G0** |
| G2 | §14.2 JSCC airtime unavailable on radio | feature | RULING |
| G3 | CSA retune is commanded, not confirmed, on devourer | correctness | GATED (G0) |
| G4 | `recover_all()` returns false on radio | feature | GATED (G0), pairs with G3 |
| G5 | `mtu_supported()` is a hardcoded assumption on radio | feature | GATED (G0) |
| G6 | `tx_index()` returns 0 for radio | correctness | OPEN (trivial) |
| G7 | `set_power_auto` semantics differ between backends | spec hygiene | RULING |
| G8 | Neither RF backend has a unit test | test surface | OPEN — **rebase onto the unit-testing PR** |
| B2 | RadioAir cannot build a node without a TX adapter | blocker | GATED (G0) |
| B3 | The Ethernet cache runs an MT7921 | blocker (hardware) | RULING — BOM, not code |

---

## G0 — close the 5805 MCS4+ pivot rationale

**This gates the rest.** `docs/mon-air-verification.md:8-16` is why the entire
fleet runs kernel-monitor: devourer could not transmit 16-QAM+ (MCS4+) on
Jaguar3 (8812EU) in UNII-3, including **5805 MHz — the §4.1 gate channel**.
`docs/devourer-revendor-review.md` §1 still reads *"the fix is vendored, but
not yet re-verified at 5805"*, and carries it as Open decision #5.

**The evidence may already exist, collected incidentally.**
`docs/step11-bench.md` §4.9 A1 (2026-08-01, craft `.232` ↔ ground `.242`,
ch 5805/HT20) records `air.kind:"radio"` tracking MCS 2→5→7 interval-to-interval
on both ground adapters, and the sharp-variant run counted **~22k frames in
bucket 7 and zero at bucket 0**. That is MCS7 aired through devourer at 5805
and decoded. But it was run as a *rate-attribution* test for Pass 118, not as
the close-out for the pivot, so it was never read as one.

What is missing is a delivery/EVM A/B against the `MonAir` baseline
(`docs/mon-air-verification.md:44-54` — 99.97% at MCS7), and a numbered Pass
that supersedes the "Why the pivot" section and resolves revendor Open
decision #5. Likely a short bench session rather than a fix.

Until this reads out, **every item gated on it is speculative work.**

---

## G1 — §15.5a scout net_id retargeting is a silent no-op on radio

The worst gap in the set, and the only one that fails *silently* — everything
else either fails closed or degrades visibly. **Not gated on G0**: it is a
live correctness bug for anyone running a devourer ground today.

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

This matters little today and a great deal the moment the backend is extracted
as a library. **Land on top of the in-flight unit-testing PR**, not beside it —
this document will be rebased once that merges, and the shape of G8 should
follow whatever harness that work establishes rather than inventing a second
one.

---

## B2 — RadioAir cannot build a node without a TX adapter

`io/src/air_radio.cpp:325-334` hard-fails unless exactly one `role:"tx"`
adapter is present. `MonAir` has `allow_rx_only`
(`io/include/wblink/air_mon.h:37-40`), which `app/main.cpp:1174-1181` sets for
two node classes:

- the **store-only Ethernet cache** (`cfg.cache.store.enabled && streams.empty()`)
- the **§2/§13 passive spectator** (Pass 74) — a display receiver with no
  return path

Under pure devourer neither is constructible. The guard is deliberate (ordinary
ground/craft configs must fail closed when their uplink is missing), so the fix
is the same opt-in flag, not a relaxation.

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

Held loosely — G0 may change the shape of everything under it.

1. **G0**, and read out what the 2026-08-01 A1 run already proves.
2. **G1**, independent of G0's verdict.
3. **G3 + G4** together.
4. **B2, G5, G6** — small and mechanical.
5. **G2** — after its ruling.
6. **B3** — hardware decision, runs in parallel with all of the above.
7. **G8** and the `AirBackend` interface collapse, rebased onto the
   unit-testing work.
