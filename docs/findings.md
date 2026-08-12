# Findings

The **Tier-2 channel** (see `CLAUDE.md`, "The law"): dated notes on anything
still being measured — loss walls, gates, dwell counts, seeds, sweep bounds,
estimator behaviour. A finding records evidence and an open question; it never
amends `PROTOCOL.md`. When a mechanism settles, ONE spec amendment plus a
numbered Pass in `review-log.md` closes it out, citing the finding.

Format per entry: date, title, what was measured (setup + numbers), what it
means, what stays open. Newest first. Delete or strike entries a later Pass
has closed, with a pointer to the Pass.

---

## 2026-08-10 — Kernel-monitor retirement audit: the code is clean, the RECORD is not — a whole bench campaign and one bench-gate verdict rest on the deleted backend

**What was audited.** Every reference to the `kernel-monitor` / MonAir backend
in this repo outside `third_party/`, after the Pass 164 deletion, asking one
question per site: does anything **live** still depend on it?

**Code — clean, confirmed by re-derivation, not by trusting the Pass.**
`io/src/air_mon*`, `scripts/mon-up.sh` and the `deploy/` monitor configs are
gone. `air.kind: "kernel-monitor"` is rejected at `io/src/config.cpp:1203-1208`
with an explanatory message and tested at `tests/config_test.cpp:513`.
`adapters[].ifname` — MonAir's only config key — is registered `never_live` at
`io/src/config_registry.cpp:163` with inert-verdict tests for both the radio
and udp paths (`tests/config_strict_test.cpp:269,274`). **Zero references
exist outside this repo** — hub, builder, sbc-groundstations, venc and Android
never named the backend.

**Find 1 — `radiotap.h`'s RX half is dead code with a live test suite.** The
retirement notes record radiotap.h as still in use, and it is: `radiotap_tx_ht`
and the `kRxMcs*` buckets are live via `radio_decode.h`, `dot11.h`, `stats.h`,
`air_iface.h`. But `radiotap_parse()` and `RadiotapRx` — the **RX** half — have
**no production caller**; the only callers left are `tests/radiotap_test.cpp`.
MonAir was the sole consumer. Devourer takes FCS state from `RxAtrib.crc_err`
and length from `mpdu_len_without_fcs()`, never from radiotap. So a passing
test suite currently guards code no shipping path reaches, and
`docs/verification-hardware.md`'s "monitor-frame FCS rule" documents its
contract as if live. Flagged rather than taken at first, because deleting tests
that pass is a reachability change and this one had no defect driving it.

**RULED (operator, 2026-08-10): delete.** Landed as **PR #170** — the parser,
the struct and the four `test_rx_*` cases go; `radiotap_tx_ht` and the
`kRxMcs*` buckets stay, and the FCS-at-end rule survives as documentation in
`verification-hardware.md` for anyone who rebuilds a radiotap-RX path.
Verified there: `dev` 73/73, and `scripts/gates.sh` 28/0/**0 skipped** with
both cross toolchains, so the reduced header genuinely compiled on
ssc338q{,-au,-eu} and android-arm64.

**Find 2 — the record overstates coverage, and by more than one campaign.**
`docs/followup-plan.md` carried DONE rows whose RF was collected on
kernel-monitor. The scope was **larger than the four rows previously
identified**: reading `review-log-archive-p001-152.md` shows the entire
2026-07-19 sequence — Passes 47 through 52 — ran on kernel-monitor. Passes
47–50 name the ground monitor netdevs `229b`/`2308`; Pass 52 used a different
pair (`:1292` — "8812EU TX→8812CU RX monitor link"); Pass 51 names no netdev.
The row labelled "Stationary N=2 **radio** soak" is included: "radio" there
means real RF, not `air.kind: "radio"`. Rows are now marked
`[HISTORICAL — kernel-monitor]` with what carries over stated per row. Two
that survive intact: Pass 50's 3 ms first-NACK grace measures a **localhost**
UDP/IP round trip, not the air — and the loss it was measured under was a
**deterministic synthetic 150‰ post-radio drop** (`:1124`, `:1221`), not RF
loss, which if anything strengthens the transfer — and Pass 52's controller
gates ran on **UDP**, not RF. One that is void: Pass 52's actuation result —
Pass 164 deleted the `iw`-forked actuator it exercised.

**Pass 51 is the one whose *conclusion* hangs off the backend, not just its
measurement.** It ruled that waybeam-link runs as root and creates
`/venc_frame_out` `0666` because *"the kernel-monitor backend requires raw
packet access"* (`:1241`, `:1256`). That premise is void — devourer uses
libusb, not `AF_PACKET`. Whether the privilege posture should now change is an
open question this audit does not answer.

**Find 3 — §17 bench gate 2 is marked PASSED on evidence from the deleted
backend, and a live seed hangs off it.** The only physical gate-2 measurement
is the Pass 47 walk fade, run entirely on kernel-monitor. `step11-bench.md`
§4.1 records it COMPLETED, and `frame-fec-plan.md:382` checks the **10% FEC
operating point** off against it. The case for transfer is real — gate 2
measures cross-adapter loss correlation ρ, a property of antenna geometry and
the channel, and the backend moves *absolute* loss rather than the
*correlation* between two co-located ears. The case against is precedent: the
monitor-era "devourer cannot transmit MCS4+" premise was refuted on hardware
in Pass 139, so monitor-era conclusions have transferred badly here before.
**RULED TRANSFERABLE (operator, 2026-08-10).** ρ is geometric: the backend
changes absolute loss, not how two co-located ears correlate. Pass 139 was
weighed and set aside — it refuted a devourer *TX capability* claim, not a
property measured identically either way. The verdict and the 10% FEC seed
stand on devourer. Provenance is recorded at `step11-bench.md` §2 and §4.1,
`frame-fec-plan.md:382`, `README.md`'s status block, `mon-air-verification.md`
§"Gate 2" (which `step11-bench.md` §4.1 forwards readers to), and the
`followup-plan.md` register. `devourer-integration-analysis.md:384,394` cite
the walk's numbers as FEC design evidence and need no flag — under the ruling
those numbers carry.

**Find 4 — PROTOCOL.md §11's CSA machinery IS monitor-derived, and one live
guard may now shield against a failure mode that cannot occur. TIER 1 — RULED:
MEASURE.** This entry originally cleared §11 on the reasoning that
`FastRetune`/`SetMonitorChannel` are devourer API names and the craft never
ran a kernel netdev. **The first half is true and the second is false**, and
the pre-merge review caught it. The repo's own record: *"the craft ran
kernel-monitor before Pass 145 and now runs devourer on the same adapter"*
(`review-log-archive-p001-152.md:6818`, which is Pass **146**). The craft's
backend then moved *back*: **Pass 149** (`:7189`) measured its devourer TX path
as **10× worse** than monitor on the same channel/MCS/RSSI — post-diversity
loss 17–19‰ vs 1–2‰ — and *"everything below was therefore re-measured on the
monitor backend"*. So the craft was on kernel-monitor later than Pass 146
suggests, which **strengthens** this find rather than weakening it. The exact
date it settled on devourer for good is not pinned here; Pass 164 is the upper
bound, because after it there was no alternative. What follows:

- **§11.6's craft post-retune RX-liveness guard (Pass 80) is a live spec
  mechanism characterised entirely on the retired path.** The half-retune it
  defends against was found on the craft 8812EU and is attributed explicitly
  to *"the in-place `iw set freq` retune path"*, recovered by *"full monitor
  bring-up"* (`archive:2345-2354`). `iw set freq` **is** the kernel netdev.
  Devourer retunes through `FastRetune`/`SetMonitorChannel` instead, so the
  open question is sharp: **does the half-retune failure mode exist on
  devourer at all?** If it does not, a live guard is defending against
  something that can no longer happen. If it does, it has never been confirmed
  there. Either answer is a spec-relevant fact, and neither is in evidence.
- **§11.2's `dt_to_switch_ms` class-0/1 budgets (150/500 ms) are not
  monitor-derived either — they are unmeasured.** `step11-bench.md` §4.3 says
  they were *"derived on paper against wfb_ng precedent, not measured against
  this radio's actual hardware TSF latch behaviour"*, and `preflight-open-
  issues.md` C3 records that run as **never done**. So the correct statement
  is "unvalidated", not "devourer-measured".
- **§18's "measured monitor-mode retunes (§11.2)"** therefore is not the
  harmless wording ambiguity first recorded here. It may be citing the retired
  backend literally.

**This is the Tier-1 trigger, and the ruling is to measure.** **Operator,
2026-08-10: run the devourer CSA retune trial** rather than rule §11 from the
armchair — queued as an RF leg on issue #134. It answers three things at once:
whether the half-retune failure mode exists on devourer, a real devourer
retune cost to size §11.2 against, and `preflight-open-issues.md` **C3**, open
since 2026-07-24. No spec text is amended until it reports.

**The rest of PROTOCOL.md is clean.** The eleven sites naming the backend
(~201/202/210 as one, ~2145, ~2326, ~2382, ~2472, ~2508, ~2698, ~2789, ~4730,
~5161, ~5383) all state the retirement and what went with it. §9.10's wedge
detector (~2145) was monitor-measured, but its defence — the failure is the
chip's, not the backend's — is now **independently confirmed on devourer** by
the entry below (induced wedge, 5/5 cleared by backend rebuild, 0/5 do-nothing
control, **PR #168**), so that seed is no longer monitor-only evidence. The
equivalent "monitor" wording in `README.md`, which is not spec, was fixed
directly.

**Find 5 — the per-MCS PER ladder's blocker was already lifted, by a route
the plan filed as a fallback. RULED (operator, 2026-08-10): sequence-derived.**
`docs/per-mcs-per-ladder-plan.md` §6 recorded a STOP: neither backend could
deliver bad-FCS frames on the fleet-default chips, so the ladder had no
numerator. Retiring kernel-monitor removes the *symmetry* constraint that made
bad-FCS the choice in the first place — but the real answer is that Pass 163
already closed §9.2's numerator by computation: rate is a pure function of
`seq`, so a missing probe-slot seq's rate is known without any signalling.
That is the plan's own option 3.

**Verified against the shipped code, and the plan is NOT fully closed by it.**
`core/src/mcs_probe.cpp` + `core/include/wblink/mcs_probe.h` implement the
schedule (`probe_slot_hit`), which rides `ProfileTable` as
`probe_period`/`probe_slot` (`core/include/wblink/table.h:48-49`).

**"Rate is a pure function of `seq`" is Pass 163's shorthand, and it is
looser than it sounds.** The probe rate resolves through
`probe_up_candidate_mcs(table, active_profile)` (`mcs_probe.cpp:10-26`) — a
function of `seq` **and** the sender's active profile **and** `table_version`.
That is exactly why the RX window carries four guards rather than trusting the
schedule: successes must be **rate-verified** against the candidate, gap losses
are **epoch-gated** to windows where non-probe frames confirm the TX is flying
the commanded rate, and at least one direct candidate-rate observation is
required or a non-probing TX on the fleet-shared schedule would manufacture a
phantom veto. The numerator is real; it is derived-and-verified, not derived
alone. Two further limits: the probe is **up-candidate only** (Pass 163's second operator ruling
— no down-slot, downshift stays loss-driven), and its evidence is a **veto,
never a warrant**. So it gives PER at *one adjacent rate*, not the 8-rung
PER-versus-RSSI waterfall the plan set out to build. Two other pieces already
ship toward that: `rx_crc_mcs[]` (per-MCS CRC-error counts, rate-attributed
pre-FCS, `io/src/air_radio.cpp:385-391`) and the Pass 158 windowed SNR/EVM
accumulator. **The plan's Parts A and B are superseded and need re-scoping
against those three surfaces; §2's bad-FCS path should not be implemented.**

**Out of scope, ruled at the start:** the wfb_ng residue (waybeam-hub 18
files, sbc-groundstations 15, builder 17, waybeam_venc 7). A separate
retirement, already executed in hub #153 and sbc #16, related to monitor mode
but not to MonAir.

---

## 2026-08-10 — A §9.10 wedge clears by DESTROYING AND RECONSTRUCTING the backend in-process; re-enumeration alone never clears it (0/5 control)

**Pass 148's "in-process re-init is impossible" is true of the thing it tested
and silent about the thing that was never tried.** It rests on devourer's
`InitWrite` unconditionally assigning `_coex_thread` and `std::terminate`-ing on
a second call — a statement about calling it twice **on one live object**. A
freshly constructed device has a default-constructed, non-joinable
`_coex_thread`, so the assignment at
`third_party/devourer/src/jaguar3/RtlJaguar3Device.cpp:900` is legal, and both
`Stop()` (`:684`) and the destructor (`:386-395`) set `_coex_stop` and join
first. Destroy-the-object-and-construct-a-new-one had never been measured.

Harness: `tools/hwtrial_reinit` (new). It uses the **production** detector
(`wblink::TxWedge` is pure and clock-injected, so it polls what `run_tx` polls,
with the craft's own policy) and **induces the fault itself** (Pass 147's usbfs
deauthorize/reauthorize), so no interval below contains an operator's reaction
time.

### The control comes first, because without it the recycle arm proves nothing

The device is reauthorized ~3 s after induction and the §9.10 verdict lands
~0.3 s later, so a recycle arm on its own cannot separate *"the rebuild healed
it"* from *"re-enumeration plus a few seconds healed it"* — both predict the
same timing. `--on-wedge wait` changes nothing and watches the same object.

**Craft 8812EU, 5 episodes: RECOVERED WITHOUT RECYCLE 0/5.**
`tx_reports` froze at **1102** and stayed there for the rest of the run while
`tx_submitted` climbed 1610 → 7710 and `tx_failed` 508 → 6608 — submissions
advancing, zero backend progress, permanently, across ~2.6 minutes and five
re-enumerations. Episode 1 is the clean replicate (induced, reauthorized,
watched 25 s); episodes 2-5 are that same unhealed wedge continuing, because
the arm never rebuilds the object and so cannot produce independent replicates.
**Effective n for "a fresh wedge does not self-heal" is 1**; the other four say
"a 30-160 s old wedge does not self-heal", which is weaker but still evidence.
This reproduces Pass 147's "USB re-enumeration does not heal the dead libusb
handle" on this unit, and it is what makes the next table attributable to the
rebuild.

The two arms also started from a matched state, which is worth more than the
episode count: control ep1 reached its verdict at `submitted=1610 failed=508
reports=1102`, A1 ep1 at `submitted=1605 failed=505 reports=1100` — within
0.3 %.

### Step 0 — healthy teardown/rebuild cycles (the gate)

| unit | cycles | frames | fd | task | RSS |
|---|---|---|---|---|---|
| bench 8812AU, bus 5-1 (Jaguar1) | **20/20** | 4000/4000, `tx_failed 0`, 4000 reports | 4 → 4 | 1 → 1 | 5464 → 5484 kB (**+20 kB**) |
| craft 8812EU, bus 1-1 (**Jaguar3/8822e**) | **5/5** | 500/500, `tx_failed 0` | 4 → 4 | 1 → 1 | 2568 → 2588 kB (**+20 kB**) |

The EU row is the load-bearing replicate: it is the **Jaguar3** path Pass 148
actually cited. `task 1 → 1` after every destructor is direct evidence that
`_coex_thread` is joined.

### Step 1 — clearing a real wedge (craft 8812EU, ch 5805, MCS 0, −72 qdb)

| arm | episodes | t(induce→restored) | mean |
|---|---|---|---|
| **A0** live supervised deployment, `exit 9` → re-exec | 5 | 13.82 / 13.29 / 13.05 / 13.35 / 13.77 s | **13.46 s** |
| **A1** in-process destroy + reconstruct, no unbind | 5 | 9.86 / 10.00 / 9.88 / 9.95 / 9.86 s | **9.91 s** |
| **CONTROL** no recycle | 1 clean + 4 persistence (see below) | never (0/5) | — |
| **A2** with the sysfs unbind | not run — see the open list |

**A1 cleared 5/5**, and the restoration criterion has a **dwell**: first
progress arms a 2 s hold, and the episode only scores when reports are still
advancing at the end of it (`+374..378` reports since the rebuild baseline, of
which ~364-369 land inside the dwell itself; `tx_failed 0`,
`tx_report_fails 0`). An earlier "8 reports, ever" criterion would have scored a
burst-then-re-wedge as recovered. A1's 9.91 s therefore *includes* 2 s of
proving durability; first progress arrives at **7.91 s** mean.

**Do not read 13.46 − 9.91 as the cost of `exec`.** The two arms reach
different milestones and both carry arbitrary constants: A0's respawn path has a
hardcoded `sleep 2` plus `free_adapter`'s `sleep 2`, then process start, config
load and control-server bind, and its poll granularity is 200 ms; A1 carries
`--settle-ms 2000`. What is comparable is the *shape*: a rebuild that reaches
first CCX progress in 7.91 s versus a re-exec that reaches a serving daemon in
13.46 s.

**Control-plane downtime, measured rather than asserted.** In A0, `:8091`
answers until the daemon's own §9.10 verdict at ~3.55 s and is then silent for
**9.24-9.96 s (mean 9.62 s)** — not the whole 13.46 s. In A1 a listening socket
held across the recycle was **STILL ACCEPTING 5/5**.

**The fault-path teardown does not leak.** Step 0 only ever tears down a
*healthy* adapter; the one that matters is the yanked-device teardown, and it
is now sampled per episode: **fd 12 → 12 and task 4 → 4 on all five**, RSS
+100 kB on episode 1, +4 kB on episode 2, then flat.

### Step 2 — the hard-fail fallback still fires

Adapter left deauthorized: the rebuild retried for its bounded window, reported
`reconstruct FAILED for 8000 ms: adapter "bus-1-1" claim/reset failed (in
use?)`, ended NOT CLEARED and exited **non-zero** — the daemon's `exit 9` path.
**37,457 bytes** of log for the episode against Pass 147's ~700 kB bound;
`/tmp` 4680K → 4720K with 40.7 MB free.

### What this supports, and what it does not

In-process recovery **works and is attributable to the rebuild** (the control
settles that), it keeps the control plane bound, and it reaches first progress
sooner than a re-exec. It is **not** a replacement for `exit 9`: Step 2 is why
the fallback must remain, and the recommendation is an in-process attempt
**first**, `exit 9` **after** a bounded number of failures.

### Open

- **Nothing is wired into the flying path.** `run_tx`, `node/` and
  `deploy/vehicle-waybeam-link.init` are untouched; §9.10's exit contract does
  not change here. Adoption means a config knob defaulting off, then one
  Tier-1 amendment once the mechanism settles.
- **H2 is not excluded.** The finding shows the unbind is not *required*; it
  does not show the kernel driver was competing. A control run — no waybeam
  process, deauthorize/reauthorize — had `rtl88x2eu` bound within ~4 s, but A1
  begins its rebuild ~2.3 s after reauthorize, and during A1 the old object
  still held the libusb handle across the fault. Whether devourer's
  `libusb_detach_kernel_driver` (`UsbOpen.cpp:105`, read in source, not
  observed) was exercised is unknown. **A2 was not run.**
- **The §15.5 server itself is untested.** The probe is a bare listening fd
  with no serving thread, no handler holding a reference to the `RadioAir` that
  the recycle destroys, and no request issued during the ~7 s with no radio
  object. It proves the teardown closes no stray descriptor; it does not model
  concurrent access to a destroyed backend, which is the real adoption hazard.
- **One unit per chip family**, one 8812AU and one 8812EU (Pass 139: two of the
  same part number are not a replicate — these are one each).
- **One induction mechanism.** Every wedge here is a usbfs
  deauthorize/reauthorize. A thermal or firmware wedge with the device still
  enumerated may behave differently.
- **The sanitized path is not the path under test.** The ASan/UBSan run was the
  bench **Jaguar1** AU (no AddressSanitizer error, no leak report, only the
  three vendored Jaguar1 items `CLAUDE.md` already calls noise) — and that run
  ended FAIL on the harness's own RSS guard, which is ASan quarantine (~3.64
  MB/cycle), not a leak. The Jaguar3 EU was release-only. **No TSan run
  anywhere**, on a thread-lifetime question.
- **No over-air confirmation.** Restoration is CCX reports — §9.10's own
  progress signal — not a receiving node; the ground was not running
  (`rssi_best` −128 throughout).
- **Four instrument bugs were found and fixed before any number here was
  believed**, every one of which first produced a confident FALSE result: a
  stale timestamp that underflowed `uint64` into an instant "NOT RESTORED"; a
  restoration test keyed on a detector transition a freshly-reset detector can
  never emit (reported 0/2 while its own log showed 5058 reports on the
  reconstructed object); a mutation test that "passed" because the mutant had
  not compiled and the stale binary ran; and the missing control above, without
  which A1 and "re-enumeration heals it" were indistinguishable. The harness's
  own guards are mutation-verified — a deliberate fd leak, a tightened RSS
  slack and a bogus bus path each turn it red.

---

## 2026-08-08 — Legs A4 and A6 closed: §15.2 mac-pin re-bind, and recover() observed to perform no USB reset

Issue #140. Run on the bench rig alongside PR #146, both dongles, kernel
drivers unloaded and restored. Closes everything in #140 that does not need an
Android phone.

### A4 — §15.2 mac-pin re-bind

Pass 154's two-pass claim/re-bind, which #139 modified (it added the `by_fd`
guard to the bus pass). Three cases, each a spectator config so the node
cannot transmit:

| case | config | result |
|---|---|---|
| listing order vs enumeration order | both stanzas mac-pinned, listed AU-then-CU | `pin-au` → 8-1 (AU mac), `pin-cu` → 5-1 (CU mac) |
| mac pin vs bus pin on the same unit | stanza 0 mac-pinned to the CU, stanza 1 bus-pinned to 5-1 (the CU) | `DISPLACED ... (§15.2 precedence)` fires on the bus-pinned stanza, which then takes the AU |
| pinned mac absent | stanza 0 pinned to `02:00:00:00:00:01` | `NOT PRESENT ... at the safe boot offset; identity-bound calibration is withheld (§10.6 D2)` |

The first case is the substance. Enumeration order on this host is 5-1 (CU)
then 8-1 (AU) — established by case 3, where the unmatched stanza fell to the
first free unit, 5-1. So the provisional by-index claim would have given
`pin-au` the CU and `pin-cu` the AU, and the re-bind corrected **both**. That
is the same input the issue's "dongles swapped between ports" produces:
the pinned MAC is not at the position the claim assigned.

**Stated plainly: the dongles were not physically moved.** The condition was
created by making listing order disagree with enumeration order. The re-bind
sees only enumeration order and EFUSE MACs, so a physical swap is the same
input by a slower route — but if anyone wants the literal test, it is still
unrun.

### A6 — §11.6 recover() performs no USB reset

#139 asserted this from reading the code (`recover()` is StopRxLoop /
SetMonitorChannel / StartRxLoop, no reset anywhere). Now observed.

Method matters here, because the first attempt proved nothing: with
`do_reset` at its default `true`, bring-up resets the device and puts
`usb 8-1: reset SuperSpeed USB device` in `dmesg` — which is exactly the line
A6 is looking for, from the wrong cause. The probe therefore sets
**`do_reset=false`**, so any reset in the log must be `recover()`'s.

Measured, with a second process injecting at MCS 0 / −24 qdb so there was
real traffic to lose:

```
before: rx=5346   →  recover() -> true  →  after: rx=12400
RX DELTA ACROSS recover(): 7054 frames
dmesg for the ear's bus (5-1): nothing
```

`recover()` returns true, the RX loop restarts (devourer re-submits its URB
ring), and **7054 frames arrive across and after the recovery** with zero
resets, disconnects or re-enumeration on that adapter. An earlier run without
a transmitter showed the loop restarting but `rx` flat at 0 — correct, and
worthless as evidence, since nothing was being sent. A6 needed traffic to say
anything.

Incidentally re-confirms `do_reset=false` on the *enumerated* path (not just
the wrapped-fd path #139 needed it for): bring-up succeeded and the EFUSE MAC
read correctly without the reset.

### Two bench notes

- The 8812CU's kernel driver is **`88x2cu`**, not `8812eu`. `8812eu` loads
  with zero users and rmmod'ing it does nothing for the CU. `CLAUDE.md`
  already names `88x2cu` — this is a note for anyone who guesses from
  `lsmod` instead.
- After a devourer run ends with the card disabled, `modprobe rtw88_8812au`
  alone did **not** bring the AU's netdev back; `modprobe -r` then `modprobe`
  did. Check `ip -br link` after restoring drivers rather than assuming.

**Open:** B/6 (a real Android `UsbManager` fd) is the only remaining #140 leg
and is genuinely phone-blocked. The composite-dongle question needs an
RTL8822BU, not a phone.

## 2026-08-08 — Leg A5: §15.3 stats from a real node, and the fd separation the log sinks depend on

Run alongside PR #146 (issue #144, B8) to check the injected sinks against a
real node rather than only against `ctest`. Issue #140 leg A5.

Setup: bench host, both dongles, kernel drivers unloaded (`rtw88_8812au`,
`88x2cu` — note the CU is `88x2cu`, not `8812eu`, which loads with zero users
and is the wrong module to rmmod). Config: the spectator template
`deploy/ground-192.168.2.199.json` with `air.kind:"radio"`, both units as
`role:"rx"` on 5805/20, `stats.hz=2`, `stats.stdout=true`. **Spectator, so
§15.2 withholds ARQ/NACK/LINK_REPORT and the node provably cannot transmit** —
this is a non-radiating measurement.

Measured:

- Both units bind and report their EFUSE identities unchanged from every prior
  run — `20:0d:b0:c4:a7:6a` (Jaguar1, `8-1`), `40:a5:ef:2f:23:08` (Jaguar3,
  `5-1`).
- §15.3 emits with a populated `adapters[]` — two entries carrying real
  per-unit counters (`rx`, `filtered`, `kernel_drop`, `rssi_best`,
  `adapter_stalled`, `rx_dead`, `tx_wedged`), every line parsing as JSON.
- Cadence exact: `t_ms` deltas 500/501 ms at `stats.hz=2` over 15 lines.
- **Complete fd separation**: stats on fd 1, every diagnostic on fd 2, zero
  crossover in either direction. This is the property B8's log sink depends
  on — a diagnostic reaching stdout corrupts the NDJSON stream a consumer is
  parsing, and it would do so only under the conditions that produce
  diagnostics, i.e. when something is already wrong.

One measurement artifact worth not rediscovering: bring-up of two devourer
adapters takes ~7 s, so a `timeout 8` run yields **one** stats line and looks
like a broken emitter. It is not — the same config at `timeout 15` gives 15
lines at the correct interval. Size the run past bring-up before concluding
anything about cadence.

**Open:** nothing here blocks. What A5 does *not* cover is a node with a live
peer — `adapters[].rx` is 0 throughout because nothing was transmitting, so
the counters are structurally-correct-but-zero. The schema question in §5 of
`docs/library-extraction-plan.md` (per-backend counter dispatch, `MonAir`
leaving the tree under #120) is unaffected either way and stays for Phase 2a.

## 2026-08-08 — TX confirmed over the air on merged Phase-1 code: 500/500 submitted, 500/500 reported, 499 received

Follows the RX-only run below (#140 legs A1/A3/B1–B5), which deliberately never
transmitted. This is the TX half, on `main` at `e65047b`.

Setup: one host, two dongles, kernel drivers unloaded. **Two processes**,
because a single `RadioAir` drops frames stamped with its own originator and so
can never hear itself — 8812CU at `5-1` as an RX ear (`originator 2`), 8812AU
at `8-1` as the uplink (`originator 1`), both `net_id 7`, ch 5805 MHz. Sweeping
from the safe end per repo law: **MCS 0** and a **−24 qdb power offset**, i.e.
the most robust rate well below the die default.

```
TX:  inject_ok=500  submitted=500  failed=0  reports=500  report_fails=0
RX:  rx_frames=499  filtered=0  dropped=0
```

**Every frame was submitted, every frame got a CCX TX-status report, none
failed, and the ear accepted 499 of 500 (99.8%).** The single loss is ordinary
air loss at a bench gap. So the whole §3.0 encapsulation → radiotap → inject →
air → decap → filter → accept chain is intact on the merged Phase 1a/1b/1a′
code, which until now had only been verified by compiling it.

**Two harness traps found, both worth knowing before writing another one.**

- **A §3.0 payload is not free-form.** `dot11_parse`'s pre-check requires the
  §3.1 magic `0x57 0x42` as the first two payload bytes. A filler burst without
  it put **236 frames in the ear's `rx_filtered` counter and zero in
  `rx_frames`** — which reads exactly like a TX failure and is not. The filter
  was doing its job; that run had already proven frames were crossing the air.
- **CCX reports arrive asynchronously.** Reading `tx_reports` immediately after
  the inject loop gave 353/500; the same run read after a 3 s settle gives
  500/500. A counter read too early looks like a TX-wedge signal (§15.3 Pass
  8's detector is exactly "reports stalling while `tx_submitted` advances").

**Open:** this is one direction, one rate, one power, at bench range, with no
ARQ, no FEC and no video. It confirms the TX path is alive on merged code; it
is not a link-quality measurement and must not be quoted as one.

## 2026-08-08 — Phase 1b on hardware: B11's last leg PASSES, and wrapped-fd identity is source-independent

First hardware run of anything from #109's Phase 1 (issues #140 legs A1, A3,
B2–B5). x86 bench rig, `tools/hwtrial_bringup`, RX-only bring-up — nothing
injected, kernel drivers unloaded and restored around the runs. Units: 8812AU
(Jaguar1) at bus path `8-1`, 8812CU (Jaguar3) at `5-1`.

**B11's unproven leg passes.** Wrapped fd + `do_reset=false` + `InitWrite` +
EFUSE walk — the combination Pass 154 narrowed B11 to and which had never
executed anywhere — returns an identity on both dies, and returns **the same
MAC as the enumerated path**: `20:0d:b0:c4:a7:6a` (Jaguar1) and
`40:a5:ef:2f:23:08` (Jaguar3), byte-identical across the two device sources.
Independently corroborated: after the run the kernel's own netdev for the AU
came back as `wlx200db0c4a76a`. So **§10.6 calibration identity is available
under a wrapped fd**, D3 fail-closed is not needed on that account, and B11
closes. `docs/library-extraction-plan.md` filed this leg under Phase 3 as
phone-blocked; it never was — `libusb_wrap_sys_device` takes any usbfs fd on
Linux.

**The Phase 1b duplicate-device guard is correct in both limbs**, which had
only been argued from source. Two distinct units bring up clean (no false
positive — the failure mode that would have refused a valid two-adapter node).
The same dongle claimed both ways is refused **immediately**:
`adapters "bus-8-1" and "fd-8/4" resolve to the same USB device (8:4)`, instead
of 1.25 s of BUSY retries blaming a process that does not exist.

**Why devourer's advisory lock could not have caught that** is now visible in
the filesystem: the enumerated claim writes `/tmp/devourer-usb-8-1.lock` (port
path) and the wrapped claim writes `devourer-usb-8-a4.lock` (bus+devaddr,
because a wrapped device has no port numbers). Two different keys, same
dongle.

**Mixed sources in one process work** — one enumerated adapter beside one
wrapped fd, both up with correct identities. That is the per-context
`NO_DEVICE_DISCOVERY` claim (Phase 1b) confirmed on hardware rather than from
the libusb source.

**`lock_dir` is honoured**: `--lock-dir /run/wblink-locktest` put the lock file
there and nowhere else.

**fd ownership holds.** The harness `fcntl(F_GETFD)`s its descriptors *after*
`RadioAir` teardown and they are still open — libusb's `fd_keep` contract, and
the reason a caller must close them itself.

**BUSY retry behaves, with one nuance worth writing down.** Against a held
adapter it prints exactly `retrying (1/5)` … `(5/5)` — six attempts, no
`(6/6)` line, then the accurate `claim/reset failed (in use?)`. But the 1.25 s
window does **not** cover a live peer's full bring-up plus teardown (measured
longer than that), so the retry cannot mask a genuinely contending process.
That is the intended property, not a shortfall: it exists to ride out a *dead*
owner's lingering advisory lock and kernel claim across a supervisor re-exec.

**Open:** nothing here exercised TX, ARQ, or a second unit per die (Pass 139).
Legs A4 (mac-pin re-bind with the dongles swapped between ports), A5 (stats
schema from a real node) and A6 (§11.6 recovery) remain, and B/6 — a real
Android `UsbManager` fd — stays blocked on a phone.

## 2026-08-08 — Pass 163 probe window: two known evidence biases (both fail toward "no opinion" or optimism, never a wrong veto)

- **ARQ resend masking (optimistic).** A lost probe-slot first-send whose
  §12 resend arrives before the gap walk settles is marked seen — the
  candidate failure is never counted. Mis-credit is impossible (resends fly
  the committed rate; same-MCS adjacency is disarmed), so the bias only
  under-counts candidate failures on ARQ-repaired streams, weakening the
  veto. Importance-gated video ARQ keeps the volume low. Revisit if flight
  data shows the veto missing real walls.
- **Blackout skip (conservative).** A seq jump ≥ the 1024-bit seen-window
  discards attribution across the gap entirely (nothing during an outage
  confirmed the commanded rate). Long outages therefore contribute no
  evidence — by design.

## 2026-08-08 — bench-gate campaign: stage 0 clean on all three dies; four gates measured; two pinned to geometry

One session, x86 rig (8812AU `20:0d:b0:c4:a7:6a` bus 8-1, 8812CU
`40:a5:ef:2f:23:08` bus 5-1) + craft .232 (8822EU `dc:57:5b:00:d0:57`,
devourer via tmpfs binary, kernel driver rmmod'd for the run and restored).
Channel 5805/HT20 throughout; every process SIGTERM-stopped and both ends
verified silent after. Apparatus: two env knobs in this branch —
`WBLINK_MCS_CYCLE` (TX: DATA radiotap MCS = wire seq % 8, the harshest
per-packet mix) and `WBLINK_MCS_TRACE` (RX: per-frame `seq/rx_mcs/adapter/
rssi/sid` lines) — plus `scratchpad-link/stage0_correlate.py` offline.
Attribution caveat for lossy re-runs: §12 resends reuse the wire seq and
fly the COMMITTED rate (inject_resend is deliberately outside the cycle
knob), so duplicate-seq trace lines are resends, never rate mismatches —
this campaign's runs had ARQ off and 0–2‰ loss, so none occurred.

**#101 stage 0 — PASS on every die present; the premise holds.**
Per-packet commanded rate flies frame-for-frame on all three fleet dies:
AU→CU 3600/3600 rate-verified (0 lost, mismatch matrix EMPTY), CU→AU
3590/3590 (10 lost = 2‰, spread across rates — and their rates are known
by computation, which IS the §9.2 numerator working), EU(craft)→dual ears
au 3593/3593 + cu 3600/3600 (both ears independently agree). CCX
cross-check on every TX: `tx_reports == tx_submitted` exactly (3737,
3741), `tx_report_fails = 0` across ~11k broadcast frames — the Jaguar
retry rate-walk is dormant on the no-ACK path, confirmed on air.
Kernel-monitor leg: MOOT (ruling #120). **Open:** per-unit coverage is one
unit per die — Pass 139's lesson wants a second unit of at least the CU/EU
parts on the rig before probing is enabled fleet-wide (fail-closed default
stands).

**#97 LDPC/STBC — proof-of-flight PASS both codings (AU TX → CU ear).**
`air.ldpc`: rx_ldpc 1878/1878 received frames; control (T1, ldpc off)
rx_ldpc 0/3738. `air.stbc`: rx_stbc 1878/1878. No caps refusal on the
Jaguar1 TX die. **Open:** the cliff A/B (PER shift at range) — needs
attenuation the bench can't produce at 30 cm.

**#98/#125 saturation knee — instrument PASS, knee not reached at the
default offset cap.** MCS7 pinned, offset swept −24→0 qdb (safe end
first): peak RSSI −17→−12 tracked the commanded 6 dB, SNR 33–36, EVM
−30..−34 (valid throughout), PER 0‰ at every dwell. *Corrected
2026-08-08:* the original "unreachable in-law" conclusion was wrong —
offset 0 is only the `power_offset_max_qdb` **default**, an
operator-authored key, and the calibration-v2 window spans [−24,+24].
**Open:** config-only rerun first (max raised to +24, sweep 0→+24 from
the safe end, issue #134); physical geometry only if that still doesn't
reach compression.

**#96 unicast A/B — mechanism PASS; retry distribution degenerate at
bench SNR.** A-leg: 236 unicast returns, fallback 0 (SA latched from
first frame), `tx_report_fails` 0 → the retry-8 ceiling never touched;
craft `reports_received` 211/211. B-leg (broadcast): same 211/211.
**Open:** the retry *distribution* only becomes non-trivial on a marginal
link — same geometry limit as #98.

**#99 aim A/B — the AU-uplink rule double-confirmed on this host's own
units.** Ground uplink = c812: release-lateness mean 2261 µs (n=1373,
max 27 ms, ZERO releases under 1 ms), driven by `ReadTsf` mean 1234 µs —
the ±1000 µs window is structurally unreachable. Ground uplink = AU:
`ReadTsf` mean 184 µs (max 441), lateness mean 1462 µs with a healthy
sub-50 µs population (102) and tail bounded at 7.4 ms. The c812 number
matches the 2026-08-07 Jaguar3 finding (2.2 ms class). **Caveat:** the
AU-leg absolute lateness (1462 µs mean) does NOT reconcile with the
2026-08-07 Jaguar1 p99 ≈ 101 µs — pacer parameters were not matched
between runs, so only the relative die comparison is quotable until a
matched-methodology rerun (issue #134).

**#95/#100 scout on-air — Pass 161 machinery verified; out-ranking is
geometry-limited; craft-home non-inflation CONFIRMED.** Leg 1 (CU flood
400 pps on 5785, net_id 1 = undecodable): at 30 cm the flood leaks FA
into EVERY bin (util 833–890 band-wide) and the ranking correctly refuses
with `BROAD_DEGRADATION` — the swamped near-field genuinely is not
channel-attributable. The discriminator that survives: **burstiness** —
5785 reads q90−q50 = 72 vs ≤5 on every other bin; the second axis sees
the interferer when the first saturates. Leg 2 (decodable net-0 craft on
5805 at 120 pps): 5805 `wifi_util` 165 with the **lowest** interference
index of all bins (363 vs 620–715) — decodable home traffic lands in the
wifi axis and does NOT inflate the FA index. Implication for the #95
out-ranking gate: an in-band decodable interferer *depresses* its own
channel's FA index (valid PHY detections are not false alarms), so
out-ranking must be judged on **total util** (both axes), never on the
FA/interference axis alone. #100 mechanics on air: rounds folded (3–4),
domain = the scout's EFUSE MAC, rejects gauges all zero, confidence
seeded correctly. **Open:** true out-ranking (loaded bin worse than
quiet bins from the same ear) needs physical separation — judged on
total util per the above.

**Pass 162 RX-only bring-up (B2 follow-up) — PASS on hardware.** CU
brought up RX-only (full Jaguar3 init + IQK), EFUSE MAC read, 8 s stats
with `tx_submitted` pinned 0 (heartbeat guard live), then ingested 3600
frames as the T1 ear — the success-path contract holds. Cosmetic: boot
restore prints `uplink: artifact STALE (stored mac/..., live udp)` on an
uplink-free node — "no uplink" would read better; harmless.

**Defect found and fixed by the campaign** (commit in this branch):
RadioAir teardown use-after-free — `~Impl` closed the libusb handle
before the devourer device destructor ran its `rtw_hal_deinit` power-down
writes; ASan flagged it on every radio teardown. `dev.reset()` now
precedes `libusb_close`.

## 2026-08-07 — first frame-free occupancy sweep: the two axes are demonstrably independent on ambient air

**Setup.** x86 devourer ground (8812AU scout), Pass 155 build, 7-channel
allowlist sweep at 300 ms dwells, craft link stopped (zero waybeam traffic
anywhere).

**Measured.** `wifi_util_permille` 0 on all seven bins (correct — nothing
decodable of ours on air) while `interference_util_permille` independently
ranked them: 5180 = 638, 5220 = 590, 5825 = 468, 5805 = 311, 5745 = 249,
5765 = 176, 5785 = 175. 5180/5220 are where this bench's household APs
live. `noise_dbm` filled only on 5180 (−81, passive floor — the one bin
with decodable foreign frames); null elsewhere, no fake zeros.

**Means.** The pre-155 ranking would have scored all seven bins identically
pristine (wifi_util 0 everywhere); the interference-inclusive ranking picks
5785/5765 over the AP-occupied bins. Ambient "quiet" UNII-3 bins read
~175–300 on the index — the fa-half seed (200 FA/s) puts the ambient FA
floor mid-scale, which is fine for ranking (monotone within the adapter)
but is a reminder the index is not a duty cycle.

**Open.** The #95 operator bench gate (a *known controlled* interferer
out-ranking quiet bins; craft video on its home channel not inflating its
own bin) — needs a hand on the signal generator. Whether the fa-half seed
wants re-derivation per §17 once #100's rank normalisation lands.

## 2026-08-07 — CU RF re-baseline after the #384 re-vendor (rfe_type 0→3): placements within flat-field noise, wall pattern unchanged

**Setup.** Same rig as the entry below (x86 devourer ground: 8812AU TX
`mac/20:0d:b0:c4:a7:6a`, 8812CU diversity ear `40:a5:ef:2f:23:08`; craft .232
8822EU `dc:57:5b:00:d0:57`, 5805/HT20, dwells 500/1000). Ground running the
Pass-154 branch with devourer re-vendored to `5a5dd62` — the first run on
this rig where the 8812CU's PHY tables load with EFUSE `rfe_type` 3 (the
#384 walk fix; the measured delta vs the old column is 7 RF register values,
see `docs/devourer-revendor-review.md`). Craft on the pre-bump deployed
binary (8822E tables are untouched by #384).

**Measured.** Bi-directional `start_both` completed (after a §11.5a claim —
the first attempt without one failed `downlink_no_ack` with the craft never
starting, which is the binding working, not a defect). Downlink placements
`[0, 0, 0, +14, +10, +2, −6, +4]` (all 0‰), brackets booked on rungs 3–7
(first_bad_rssi −33/−36/−36/−41/−38, placement RSSI −31..−40); uplink MCS0
flat to +24 (no bracket), capped placement 0 @ 4‰, RSSI −53. Morning
pre-bump baseline (below): downlink `[−8, 0, 0, +8, +8, 0, −8, 0]` with 5
rungs bracketed, uplink capped −8 @ 2‰ (RSSI −48).

**Means.** Same shape either side of the bump: flat clean low rungs, walls
on the top half, uplink wall outside the window. Placement deltas are
one-to-two seek steps inside a 0‰ flat field — the entry below already
concluded such differences are noise, and geometry/power state moved between
sessions too (uplink RSSI −53 vs −48). No gross CU RF regression: the
diversity ear delivers, the link held 1‰ at HOLD pre-run, calibration
completes. **The rig's baseline is now these post-bump numbers.** Nothing
measured pre-bump survives as an artifact either way: the Pass 154 identity
change re-keys every stored artifact (`id/radio/…` reads STALE), so no
pre-bump RF state can silently apply.

**Open.** A same-session A/B (old vs new devourer on the CU, rig unmoved)
was not run — placements are noise-bounded at this range, so only a
purpose-built RX-sensitivity A/B would resolve the CU delta finer. The
8822EU per-unit 64-QAM early-wall note (below) stands.

## 2026-08-07 — flat-field verify selection is noise; widening the offset window recovers real walls

**Setup.** x86 devourer ground (8812AU TX `ground-au-1`, 8812CU diversity) +
.232 craft (8822EU `craft-eu-1`), 10 m, 5805/HT20, calibration v2 dwells
(500/1000 frames).

**Measured.** With the original [−24, 0] offset window the whole field is
flat (1–10‰ everywhere, no bracket bookable), so the §10.7 verify walk's
"best" is noise-selected: morning runs placed (−8 @ 1‰, then 0 @ 6‰),
midday runs refused `no_wall_found` **six consecutive times** (verify at the
ceiling kept reading 1–3‰ vs 4‰ one step down) — the outcome tracked slow
RF drift, not the link. After the same-day rulings (offset-space exemption +
window widened to [−24, +24] with the unbracketed-placement cap) the same
bench books **real walls on 5 of 8 downlink rungs** (fp=133 placements
`[-8, 0, 0, +8, +8, 0, -8, 0]`, brackets at first_bad_rssi −66/−41): the
walls were simply above the old window's ceiling. Uplink at MCS0 stays
wall-less even at +24 (RSSI −48, 4‰) — capped placement −8 @ 2‰.

**Means.** Within a flat region, placement differences of one seek step are
not reproducible measurements; only a booked bracket makes a placement a
property of the channel. The window should be wide enough to contain the
wall, and the reference cap handles the case where it is not.

**Open.** Uplink MCS0 wall not yet within [−24, +24] at 10 m — either a
longer placement or a higher-MCS uplink rung would book it. The craft's
rung-6 (64-QAM) early wall (first_bad −41) matches the known per-unit
8822EU 64-QAM TX weakness; unify with that finding when the unit is
re-characterised.

## 2026-08-07 — ground binary wedges on SIGTERM after an in-process calibration run

Twice this session the x86-ground process ignored SIGTERM (stop script +
direct kill; REST already dead, process alive until SIGKILL) — both times
after it had completed at least one §10.7 run in-process; a fresh instance
stops cleanly. Suspect a teardown path wedged in devourer USB close while
calibration-era actuator state is present. Bench impact only (SIGKILL is
acceptable on x86, never on SigmaStar). Open: reproduce under gdb / with
devourer verbose teardown logging; check whether the §10.7 restore path
leaves an actuator thread parked.

## 2026-08-07 — §7.2 aim error budget: instruments landed, numbers owed (issue #99)

**What exists now (bench knob, no spec surface):** `WBLINK_AIM_LOG=1`
histograms two of issue #99's three error terms — (a) *release lateness*,
how late past the computed `QuietGap::return_deadline` the host loop
actually fired the return window, and (b) the `ReadTsf()` control-transfer
cost, the §7.2 term with **no measured number at all** (devourer bounds it
0.5–1.2 ms on Jaguar3; a bulk-flooded adapter additionally starves the
read). Dumped to stderr every 30 s as bucketed distributions
(<50/<100/<200/<500/<1k/<2k/<5k/≥5k µs) — the tail is the contract, means
hide it. **rx role only**: the dump lives in the ground loop; on a tx node
the flag collects and never prints (the gate-4 campaign is a ground-side
evaluation — extend the dump if a craft-side number is ever wanted). The third term (craft-side arrival phase relative to its own gap)
is **deliberately not instrumented yet**: it needs either a host↔TSF fit
(devourer tdma example) or a host-time proxy whose error is exactly the
terms under study — that placement choice is part of the §17 gate-4
evaluation itself.

**What the vendored bench already says (act on it now):** submit→air p99 is
**101 µs on Jaguar1 (8812AU, async USB2)** vs **2.2 ms on Jaguar3 (8822CU,
sync USB3)** against a ±1000 µs window budget — on a Jaguar3 uplink, that
one term alone blows the budget ~1 % of the time, and the failure is
*correlated* return loss inside a window (defeating Pass 78's redundancy,
which assumes independence). **The ground's `role:"tx"` adapter should be
the 8812AU** wherever the rig has a choice; the x86 bench rig already
complies (AU = `au-uplink`), now as a rule rather than an accident (also
noted in `deploy/README.md`).

**Open:** the gate-4 campaign — run the instruments per uplink generation
(AU vs CU), measure end-to-end aim as a distribution, report the miss rate
against `[eob+guard, eob+guard+window]`, then recommend re-derived
`guard_us`/`return_window_us` seeds or a documented miss budget.
`disable_cca` is NOT a lever (Pass 139: clearing it costs ~45 % of the
uplink). TDMA stays deferred per the issue's own assessment.

## ~~2026-08-07 — §10.7 walls referenced to a measured at-rest floor~~ CLOSED by Pass 153

The floor mechanism (and its `uplink_floor_min_samples` knob) is deleted:
calibration v2 pauses the craft's video for the run, so the contention floor
the walls were being referenced against is structurally zero and the walls are
absolute again. See `review-log.md` Pass 153.

## ~~2026-08-06 — §10.7 report-loss is an under-powered observable~~ CLOSED by Pass 153

Resolved in the direction the entry proposed: §10.7 (and §10.6) measure with
dedicated MTU-padded §3.16 PROBE bursts and per-dwell TALLYs — probe density
is no longer capped by the 10 Hz report cadence, so the n≈1500-per-dwell
sample the estimator arithmetic demanded is cheap. The at-rest σ evidence
(21 windows of n≈150: sd 23.3‰ vs binomial 22.1‰) lives on in the archived
Pass 152 addendum and the Pass 153 entry. See `review-log.md` Pass 153.

## ~~2026-08-06 — §10.7 spec/code drift: `uplink_verify_epochs` 400 vs 200~~ CLOSED by Pass 153

Dissolved: the key is retired; the v2 dwell knobs are `dwell_probe_frames`
(500) / `dwell_verify_frames` (1000). See `review-log.md` Pass 153.

## 2026-08-09 — a tier below the sweep floor collapses an offset-space calibration and reports success

Bench: `.242` ground (8812AU, bus `8-1`) ↔ `.232` craft, 5805→5765 after claim,
both ends on the Pass 166 branch. Fleet ladder
`power_offset_presets_qdb: [-72,-48,-24,0,24]`, `power_offset_qdb: -24`,
`power_offset_max_qdb: 24`.

With §11.7 `0x0A` tier 1 in force (`ceiling_qdb: -48`), `POST /api/v1/calibration
{"action":"start"}` on the ground uplink:

```
state done   phase idle   fail_reason null   probes 1500
placements: [{"mcs":0,"placement_qdb":-24,"last_clean_qdb":-24,
              "first_bad_qdb":null,"placement_loss_milli":7}]
```

One point. The previous artifact — `fingerprint 94`, `last_clean_qdb: 24`,
i.e. a sweep that had climbed to the configured bound cleanly — was
**overwritten**. Restored by hand afterwards; the degraded copy is kept as
evidence.

Mechanism: the ground's *startup* window fold is branched by space and takes
`[power_offset_qdb, power_offset_max_qdb]`, but the §15.5 tier handler's
*runtime* fold was unconditional — `seek.max_qdb = min(cp_max_qdb,
ceiling_qdb)` — so an offset ceiling of −48 landed under the −24 floor. The
craft half refuses this case (`offset_window()` returns nullopt and §11.7
CALIBRATE gates on it); the ground half had no equivalent, so a degenerate
sweep looked like a successful one.

Ruled Tier-1 the same day (Pass 167): in offset space a tier does not narrow
the calibration window at all. That removes the mechanism rather than adding a
refusal to it — but the asymmetry is worth remembering, because the ground
still has no "refuse an empty window" guard of its own and a config with
`power_offset_max_qdb == power_offset_qdb` falls to the ABSOLUTE startup arm
(`app/main.cpp`, the `else if (upwr.ceiling_qdb)` after the relative window
fold). Since Pass 166 the number folded there is an **offset**, not the 108
`max_power_qdb` it used to be, so that arm mixes spaces and can reproduce the
same one-point run from config alone. A future pass picking this up should
start from that description, not from the pre-Pass-166 one.

## 2026-08-09 — the TX half of `node/` cannot ship in a receive-only archive (#109 Phase 3)

Measured while adding the `wblink_tx_*` C ABI, not reasoned from the code.

`examples/node-linkcheck` builds `wblink::node` with `WBLINK_FRAME_SHM`,
`WBLINK_CONTROL_SERVER` and `WBLINK_VENC` **off** — the phone's configuration —
and was green on the branch that had just lifted `run_tx` into
`node/src/tx_node.cpp`. Adding one C caller of `wblink_tx_run` turned it red
with **~22 undefined references**: `FrameShmRing::attach/read_frame/stats/…`,
`ControlServer::create/service/publish_stats`, `VencActuator::set_fps/
set_bitrate/request_idr/…`.

Nothing had broken. `run_tx` uses all three subsystems unconditionally — the
same three `WBLINK_BUILD_APP` already refuses to build without — and a static
archive extracts a member only when something references it. Until the C ABI
existed, nothing referenced `run_tx` in that configuration, so the member was
never extracted and the gate never looked inside it.

That is the failure shape `node-linkcheck` was written to end (its CMakeLists
records the nine references B10 removed), reappearing one level deeper: the
gate proves the archive *links*, which is not the same as proving the archive
*resolves*. A link gate can only see the members its consumer pulls in.

Fix: `node/src/tx_node.cpp` and `node/src/tx_node_c.cpp` compile into
`wblink_node` only when all three subsystems are ON. A receive-only consumer
(Android `:wifi`, bionic, no `shm_open`) gets an archive that resolves; a
transmitter configures what a transmitter needs. The `node-linkcheck` project
now asserts against the target's SOURCES property rather than inferring it
from the options, and deleting the guard upstream makes it fail at configure
time (verified by mutation).

Two things this does NOT claim. It is not a bug in the lift — the references
were latent, never live, and no shipped build was affected. And it is not
proof that no other member of `wblink_node` carries the same latency: the check
is specific to the TX sources, and the general property still has no gate.

## 2026-08-09 — first coordinated RADIO decode through the in-process node; and the loss is not power

**The decode.** waybeam-hub `ground_x86` with `WBLINK=1`, `pixelpilot.frame_shm.source=wblink`,
running `wblink_rx_run()` in-process against a real craft (`.232`, 8812EU) over
RF at 5805 MHz. Ground was a **single 8812AU on bus 5-1 acting as the TX/RX
combo** (operator rule 2026-08-09: do not pair a second adapter for the ground
role; `RadioAir`'s `role:"tx"` adapter is duplex and receives too).

Sustained **27829 frames, 1920x1080 @ ~60 fps, ~10.2 Mbps**, with
`shm_gate_bypasses 0` (the gate opened on a real IDR, not a bypass),
`shm_reattach_count 0`, no pipeline rebuild, and no GStreamer error. Operator
confirmed the picture on screen. This is the first end-to-end proof over a
radio rather than the localhost `udp` air backend.

**The loss, and what it is NOT.** The link sat at profile 2 with
`transition_reason LOSS_PERSISTENT` and a ground-side `loss_ewma_milli` in the
20-60 range. The obvious suspect at bench range was receiver overload: the
ground read **RSSI -6..-8 dBm** while every rung of the craft's calibration
artifact was measured with `last_clean_rssi` between **-22 and -35** — i.e. we
were operating 14-27 dB hotter than anything the curve covers, which is the
regime where PA compression normally shows up.

It is not that. An ordered sweep (-72/-48/-24/0 qdb, one 18 s dwell each)
suggested -48 was best (25 vs 49 milli), but an **alternating A/B** of 0 vs -48,
three passes, 20 s dwell, reversed it:

| pass | 0 qdb | -48 qdb |
|---|---|---|
| 1 | 6 | 44 |
| 2 | 22 | 50 |
| 3 | 63 | 62 |

Cutting 14 dB never helped. RSSI -6 vs -20 against noise -38 vs -52 leaves
**SNR ~32 dB either way**, so the link is not SNR-limited and power is the wrong
knob. The single ordered sweep was noise; only the alternation showed it.

**What the numbers actually say.** On the craft:
`lockout_latched true`, `lockout_profile 3`, `lockout_strikes 4`,
`lockout_active_mask 8` — **profile 3 was tried, failed four times and is
latched out**, which is what pins `lockout_ceiling_profile` to 2. Separately
`promote_blocked_saturated 18339`. The calibration itself is clean:
`calib_stale false`, fingerprint 53, and all eight rungs report
`placement_loss_milli 0`. So "the calibration is bad" is not supported —
a runtime lockout plus saturation is.

**Unexplained, and the reason this is a finding and not a ruling.** Loss rose
monotonically across the ~8-minute A/B *regardless of the setting* — 6 -> 22 ->
63 at 0 qdb and 44 -> 50 -> 62 at -48. Some time-dependent factor dominates both
arms and no measurement here isolates it. Ruled out on the spot: the ground
host's own WiFi (`wlp2s0` is on 5180 MHz, 625 MHz away). Not yet excluded:
channel occupancy at 5805, craft thermal, venc rate behaviour under a held
profile. **Chase this before trusting any loss number from this bench**, and
note that an ordered sweep will lie about it — alternate.

## 2026-08-12 — Scout is a craft finder; its dwell was priced for occupancy

**Tier 2.** Dwell counts, per `CLAUDE.md`. No spec text, no Pass.

#173 established that ScoutEngine cannot report generic RF occupancy at all:
FA/CCA are event counters with no duration semantics, and the one
duration-capable primitive (phydm CLM) is unimplemented in the vendored
devourer, which is off-limits. Occupancy is therefore not a goal the sweep can
serve — which removes the reason the sweep was slow.

**What the base dwell was buying.** `finalize_current` divides accumulated
decoded airtime by the *elapsed* dwell to get `wifi_util_permille`. Leaving a
channel early shortens that denominator and inflates the result, so `tick()`
held the full base dwell on every channel and only broke out early on a channel
whose dwell had *already* been extended. Every empty channel paid a full dwell
to protect a denominator feeding a number that is published `duty_cycle_known:
false` and rendered nowhere.

**Measured, Android + RTL8812CU (Jaguar3), craft 17 @ 5805 MHz:**

| | before | after (projected) |
|---|---|---|
| base dwell | 1000 ms | 250 ms |
| `kExtendMs` | 1200 | 1500 |
| full 38-ch sweep | ~50 s measured (38 s floor, 83.6 s ceiling) | ~12-14 s projected |

**The change.** The base dwell becomes a short presence probe, and *anything
heard* extends the dwell once — where the old form extended only when no
candidate had resolved yet. An empty channel costs one base dwell; a channel
with waybeam traffic gets base + `kExtendMs` to cover the announce cadence.
The 250 ms base is safe *because* of that extension, not in spite of it: video
is high-rate, so presence trips `frames > 0` long before an ANNOUNCE arrives.

**A rejected design, recorded because it nearly shipped.** The first cut ended
the dwell on the first *resolved candidate*, which is faster still. It is also
wrong: `accum_.candidates` is non-empty at the FIRST announce, so a second
craft sharing that channel — announcing independently, up to a second later —
is silently dropped. It cost ~3 s of a ~12 s sweep and paid in missed craft,
which is the worst failure available to a craft finder and one that leaves no
trace, because the sweep still completes and still finds *a* craft. Note the
old 1000 ms base dwell truncated the same way; always-extending is strictly
better co-channel coverage than the behaviour being replaced, not merely equal
to it. `test_a_second_craft_on_one_channel_is_not_truncated_away` pins it.

**What did NOT change, deliberately.** The 30 ms sense barrier stays. It reads
as an occupancy device, but `on_frame` gates on `barrier_done_`, so it is also
the retune-leak guard that stops a settling frame being attributed to the
channel just entered. That attribution is the finder's entire job, and #173
records a still-open CU/Jaguar3 retune misattribution defect — removing the
barrier for ~150 ms across a 14 s sweep would have traded the core function for
1% of the runtime. The per-channel sense *read* was also kept for the same
reason it is cheap: two register reads against a dwell budget it cannot
meaningfully dent.

**Known cost.** The airtime denominator is no longer uniform across a sweep:
an empty channel is measured over ~250 ms and a channel with traffic over
~1750 ms. `wifi_util_permille` was never comparable across channels in a
strong sense, but it is now visibly less so, and a short quiet window makes a
single stray frame read as a larger fraction of it. This is acceptable only
because the number is published `duty_cycle_known:false` and rendered nowhere.
If a future CLM primitive (#173) makes occupancy real, this pacing must be
revisited first — a duration-based measurement needs a fixed window back.
