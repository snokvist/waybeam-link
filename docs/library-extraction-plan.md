# Library extraction — state of the art, blockers, and issue sequencing

**Survey and planning only. No spec ruling is made here, so there is no
`docs/review-log.md` Pass entry attached.**

**Refreshed 2026-08-08 against `main` at Pass 163 (`56463c0`).** The first
draft was written against Pass 148. Three of its twelve blockers have since
been closed outright by work that landed for other reasons and a fourth was
halved, leaving nine live. Two moved the other way: **B8 got worse** — a claim
both drafts recorded as a safe expectation is false, and blocks the radio
backend on Android outright — and **B2's resolution exposed a successor
question** the drafts had assumed away (§0). Every symbol and line cited below
was re-grepped at `56463c0`, then probe-executed; the previous draft's numbers
were stale by roughly 1,100 lines of `app/main.cpp` alone. What changed is
recorded in §6 so a reader of the old version can see which conclusions
moved.

**Citations are symbol-first.** Per `docs/devourer-parity-plan.md`'s standing
warning, prefer the symbol name over the number when they disagree.

Companions: `docs/devourer-parity-plan.md` (whose "Library extraction — notes
carried forward" section this supersedes), `docs/config-harness-plan.md`
(whose §2 ruling gates part of this), `docs/devourer-integration-analysis.md`
(the upside register behind #95–#101, seven of eight now landed — see §3.1).

## Why this document exists

`docs/devourer-parity-plan.md` closed its register observing that the library
ambition "is unchanged and is now closer": `AirIface` (Pass 140) is the
extraction boundary. That was correct for a parity document, which answered
*"can devourer do what kernel-monitor does"*. The extraction question is a
different one: *"what does a consumer that is not `app/main.cpp` need in order
to be a waybeam node?"*

The answer is not mostly about devourer, and it is less about devourer now
than it was in the first draft. It is mostly about the fact that **there is no
layer that is a waybeam node.** `core/` is protocol logic, `io/` is devices and
config, and everything that composes them into a node lives in one
**8,635-line** `app/main.cpp`. That is the finding this document exists to
state, and it is the only one of the original twelve blockers that has grown
rather than shrunk.

## 0. What the consumers actually need

Three consumers are now named, one of them by ruling.

**Waybeam-android's target node is a ground receiver.** Its `:wifi` module is
a *passive leech*: `WifiRadioNative` opens N adapters by fd, locks a channel,
and decapsulates unencrypted IPv4/UDP off the air (`nativeStartLeech`,
`wifi/src/main/cpp/wifi_jni.cpp:797`; the decap and protected-bit reject at
`:188`/`:201`), plus a channel survey (`nativeStartSurvey`, `:657`) and an
inspect view (`nativeStartInspect`, `:906`). It speaks no waybeam protocol at
all — the whole module is five Kotlin files and one 1,004-line `wifi_jni.cpp`,
and its consumer treats it as a raw byte sink
(`app/src/main/java/com/waybeam/app/service/WaybeamService.kt:537`). Replacing
it with waybeam-link means the phone becomes a real node, and what "real"
means here is decided by which archetype it is. That is `run_rx`, not
`RadioAir`.

**Which archetype is an OPEN question, and the first draft got it wrong by
assuming the answer.** Both earlier versions said Android is `rx-spectator`
and listed §8 NACK/ARQ returns, §9 LINK_REPORT and §11 CSA follow among what
it gains. **A spectator gets none of those three.** §15.2 is explicit
(`PROTOCOL.md:4508-4519`): a spectator

> may run with **zero `role:"tx"` adapters**: it delivers by FEC + diversity
> only, generates **no ARQ / NACK / LINK_REPORT** … with **no §11 claim** …
> A spectator does **not** follow CSA channel moves; it re-acquires a hopped
> craft by **re-scout**.

And the archetype is *forced*, not chosen. `allow_rx_only` is derived, not
configured (`app/main.cpp:1600-1602`):

```cpp
rc.allow_rx_only = (cfg.cache.store.enabled && cfg.streams.empty())
                   || cfg.node.spectator;
```

Android needs media streams, so the cache branch is closed to it and
`node.spectator` is its only RX-only route. So the two candidate shapes carry
genuinely different capability:

| | adapters | gets | loses |
|---|---|---|---|
| `rx-spectator` | N receive dongles, no uplink | §3.0 decap, RX diversity, FEC recovery, §15.5a scout/quickconnect, passive-tune feed select | ARQ/NACK, LINK_REPORT, §11 claim + CSA follow (re-scout instead) |
| `tx-ground` | N receive + **one `role:"tx"`** | all of the above **plus** the three return-path features | requires a transmit-capable dongle and an uplink budget on the phone |

**RULED 2026-08-08 (operator): `tx-ground`, with spectator also supported.**
B2's resolution exposed this question rather than answering it, and the
answer is the second column. Note precisely what it means: Android is a
**ground with an uplink** — `node.role:"rx"` plus one `role:"tx"` adapter —
**not** a craft. So the target loop is still `run_rx` and Phase 2a's target
does not move. `deploy/ground-192.168.2.242.json` is the template
(`role:"rx"`, not spectator, 2 adapters, one `role:"tx"` named `eu-uplink`);
`ground-192.168.2.199.json` is the spectator variant.

Three consequences the spectator-scoped plan had excluded, now in scope:

- the wrapped-fd path (B1) must serve a `role:"tx"` adapter, not only RX ears;
- the TX-die knobs — `air.ack_responder`, `policy.return.unicast`, `air.ldpc`,
  `air.stbc`, `air.mcs_probe` — become reachable, having been fail-closed
  refusals on an RX-only node since Pass 162;
- §8 ARQ/NACK, §9 LINK_REPORT and §11 claim/CSA-follow are back on the
  critical path.

**The trust boundary does not move, which is worth stating because it looked
like it would.** §11 claim needs the `csa_psk` boundary, and a phone holding
the fleet's operator secret would be a real change. Measured instead: all
four `deploy/*.json` have `policy.csa.psk` **absent**, i.e. the fleet runs
the announced-session-token mode (§11.4a, `psk_present=1`). Android matches
today's posture with no secret provisioned. It becomes a question only if the
fleet ever moves to operator-secret mode.

**~~The MonAir external repo is the second proving consumer~~ — SUPERSEDED
(Pass 164).** Issue #120 item 3 had `MonAir` *moved, not rewritten* into an
external RX-only repo that vendors the extracted core, making the split's
success criterion concrete: the extraction is done when a repo outside this
one can build a receiving node. The operator ruled **DROP** on 2026-08-08
instead — a devourer spectator carries the archetype (19466 frames, loss 0 ‰,
zero `role:"tx"` adapters), so the backend had no remaining user. `air_mon.cpp`
is deleted, not relocated.

**Consequence: Phase 3's proof rests on the Android consumer alone —
RULED SUFFICIENT (operator, 2026-08-09).** There is no second outside repo
queued and none is coming; the fleet is x86, RK3566, the SSC338Q craft and
Android, and nothing else consumes this code. So the Phase-3 criterion is
`:wifi` building a node, and the four in-tree cross-presets (`x86-ground`,
`rk3566`, `ssc338q*`, `android-arm64`) carry the portability half. Do not
invent a synthetic second consumer to restore the old two-consumer
criterion — `examples/embed-consumer` stays what it is, a *linking* gate,
and its scope is deliberately not widened.

**OpenWRT is the easiest consumer** and mostly needs what the SSC338Q cross
already proves, plus a musl build and an install/export target. It is not
driving the design.

## 1. Three layers, three very different amounts of work

### L1 — `core/`: ready today

Re-verified at `56463c0`, not assumed: `core/src` and `core/include` contain
**no** reference to `unistd.h`, `sys/socket`, `pthread`, `<thread>`,
`<mutex>`, `stderr`, `fprintf`, or `nlohmann`. Time is injected. 32-bit
cleanliness is a build gate, not a claim — `cmake --preset ssc338q` is ARMv7
and builds `wblink_core` warning-free.

This held through the whole expand tranche: Passes 155–163 added
`core/src/mcs_probe.cpp` (198 lines) among others without breaching it. The
discipline is real, not incidental.

What is missing is packaging, not portability: an install/export target, an
umbrella header, and a version. That is hours, not weeks.

### L2 — `io/`: the boundary exists, the device model does not

`AirIface` (`io/include/wblink/air_iface.h`) is genuinely the right seam and
is all-pure-virtual by deliberate design, so a new backend or a new capability
is visible at every implementer rather than silently falling through. It now
carries **26 pure-virtual methods**, two of them added during the expand
tranche — `rx_sense` (`:201`, Pass 155) and `set_mcs_probe` (`:142`, Pass
163). Both additions went through cleanly, which is evidence the boundary
holds under growth.

`load_config_json()` and `load_profile_table_json()` both still take strings
(`io/src/config.cpp:115`, `:1313`), so a consumer with no filesystem is
already served **for config**. That does not generalise to `io/` as a whole:
`calib_store.cpp`, `uplink_calib_store.cpp`, `power_file.cpp` and `modes.cpp`
all do real filesystem I/O and none of them is on B6's optional-source list.
A filesystem-less consumer loses calibration artifacts and the mode catalog,
not its config.

`io/` has also been absorbing pure logic on its own: `ScoutStore`
(`io/include/wblink/scout_store.h:67`, 199 lines of `.cpp`) is injected-time
and socket-free, and `scout_sense.{h,cpp}` is a pure derivation unit. The
extraction pattern this document proposes is one the repo has already started
following by itself.

What blocks a consumer is enumerated in §2. None of it is deep.

### L3 — node behaviour: not a library at all

This is the whole cost, and it grew. `app/main.cpp` holds `DiscoveryCatalog`
(`:461`), `ScoutEngine` (`:636`), `AirBackend` (`:1471`), `TxCore` (`:2087`),
`RxCore` (`:3820`), `PacketEventTrace`, `ArqTimingTracker`, `UplinkPower`, the
stats emitter, the info/health JSON builders, and the three mode loops:
`run_tx` (`:4763`, ~980 lines), `run_rx` (`:5741`, **~2,600 lines**) and
`run_loopback` (`:8345`).

Two things soften this, both still true:

- **`RxCore` is already clean.** Constructed from `Config` + session + table
  (`:3825`), with `RxCore::on_air` taking an injected `now` and deliver
  callbacks. It is a library class living in the wrong file, not a tangle.
- **`AirBackend` is already down to one `unique_ptr<AirIface>`** (Pass 140,
  `:1484`). The only per-backend dispatch left in it is the §15.3 counters
  fill, which its own comment sets aside as a schema question, not a dispatch
  one — the two backends' counter structs differ by real fields
  (`kernel_dropped`/`bpf_filtered` against `evm`/`cfo`/`snr`).

What is genuinely entangled is `run_rx`/`run_tx` themselves: uplink power
resolve, calibration store, control server, venc actuation, scout sweep, CSA
campaign, cache following, and the stats cadence are interleaved with the poll
loop.

## 2. The blocking list

### 2a. Resolved since the first draft — three closed outright, one halved

**B2 — `RadioAir` RX-only — RESOLVED by Pass 162.** This was the draft's
"single hardest blocker, and it is not code". It is now code, and merged.
`RadioAirCfg::allow_rx_only` (`io/include/wblink/air_radio.h:70`) is derived
from the archetype — cache-with-no-media-streams, §2 spectator — and is
**never a config key**; `app/main.cpp:1600` sets it for the radio backend
(`:1558` for monitor). The gate at `io/src/air_radio.cpp:477` reads
`n_tx > 1 || (n_tx == 0 && !cfg.allow_rx_only)`, so more than one uplink
remains an error everywhere. Covered by `tests/radio_rx_only_test.cpp`.

The draft asked how a tolerant `RadioAir` answers the four TX-adapter
dependencies. Re-checked, all four are answered without a TX die:

| dependency | how it answers RX-only |
|---|---|
| `has_tx()` | truthful — returns `impl_->has_tx`, set at `air_radio.cpp:508` |
| `tx_index()` | returns 0; meaningful only under `has_tx()`, so the §15.5a scout roams adapter 0 |
| `read_tsf(adapter)` | per-adapter (`dev->ReadTsf()`), never TX-specific |
| `reapply_tx_power(adapter)` | per-adapter (`dev->ReApplyTxPower()`), never TX-specific |
| `set_tx_mode` / `set_mcs_probe` | node-state writes; inert with nothing to send |

Fail-closed corollary, also merged: `air.ack_responder`,
`policy.return.unicast`, `air.ldpc`, `air.stbc` and `air.mcs_probe` each name
a TX-die property, so setting any of them on an RX-only adapter set **refuses
create** rather than running silently inert.

**The Android N-dongle no-uplink node is now expressible.** What remains of
the Android *device* story is B1, and B1 is mechanical.

**But "expressible" is not "capable", and the difference is a ruling, not a
detail.** The only RX-only route for a streams node is `node.spectator`, and
the spectator contract deliberately withholds ARQ/NACK, LINK_REPORT, the §11
claim and CSA follow (§0). B2 closed the *construction* gate exactly as the
draft framed it; it did not decide what an uplink-free Android node is
allowed to do, and the draft's own §0 promised three capabilities the
resolution does not grant. That successor question is now the open one.

**B3 — backend mix — DISSOLVED by ruling #120, then moot (Pass 164).** The
draft asked whether `air.kind` should stay node-level or grow a per-adapter
`backend` key, so a mixed devourer + kernel-monitor RX node could be
represented. Ruling #120 made devourer the sole in-tree backend; Pass 164
deleted the other one outright. There is no second RF backend to mix with, so
the question no longer has a subject. `#106` item 3 should be closed as overtaken rather than ruled.
`air.kind` stays node-level; a consumer copying that shape copies the right
one.

**B6's `air_mon.cpp` leg — CLOSED BY DELETION (Pass 164).** The Pass-148
draft proposed relocation to an external repo; the operator ruled DROP. It
never needed a `WBLINK_MON` option and it no longer needs a destination:
1103 lines plus every backend branch left the tree in one commit. Cheaper
still than relocation, and it takes the §15.3 per-backend dispatch with it.

**B11 — calibration identity under a wrapped fd — CLOSED 2026-08-08 on
hardware.** The one unproven leg described below was run (issue #140, leg B2):
wrapped fd + `do_reset=false` + `InitWrite` + EFUSE walk returns an identity on
both fleet dies, and returns the **same MAC as the enumerated path**. See
`docs/findings.md`. The narrowing analysis is kept because it is what made the
leg cheap to identify and run.

**B11 — calibration identity under a wrapped fd — NARROWED to one unproven
bench leg by Pass 154.** The draft's concern was that `calib_identity()` had
*neither* an `ifname` nor a bus path under a wrapped fd. Pass 154 re-based
radio identity onto the per-unit EFUSE MAC: `io/src/calib_store.cpp:83` now
returns the single derived tier `"mac/" + efuse_mac` on `kRadio`, and the MAC
is read **off the die over USB**
(`ad.dev->GetPermanentMacAddress(mac)`, `io/src/air_radio.cpp:693`). There is
no longer a host-side lookup to be missing, which is the substance of the
resolution.

**It is not "available by construction", and an earlier version of this
refresh said so wrongly.** The read is not merely after `CreateRtlDevice`
(`:662`) — it is after **bring-up**, `ad.dev->InitWrite(...)` at `:685`, and
devourer's contract requires that (`third_party/devourer/src/IRtlDevice.h:347-353`):

> the EFUSE is only guaranteed readable on a **brought-up chip** … for a
> guaranteed answer on a programmed unit, ask **after bring-up**.

Our own code already records the same constraint in a stronger form
(`io/src/air_radio.cpp:546`): *"The 8822E EFUSE is only reliably readable
during InitWrite, so a MAC pin cannot drive the CLAIM"* — which is why Pass
154 claims stanzas provisionally and **re-binds** them after bring-up. That
whole dance depends on `InitWrite` succeeding.

The Android consumer — the very thing cited as proof that a wrapped fd behaves
like an enumerated handle — **never calls `InitWrite`**. Its leech, survey and
inspect paths all call `dev->Init(...)` (`wifi_jni.cpp:624`, `:709`, `:854`,
`:922`). So the exact combination B11 needs — wrapped fd **and**
`do_reset=false` (forced by B4) **and** `InitWrite` **and** the EFUSE walk —
has never been executed anywhere, on any bench.

Correct status: **closed for enumerated handles; one unproven leg under a
wrapped fd**, and that leg is cheap to run once Phase 1b exists. If it fails,
D3's fail-closed path is the honest fallback — no absolute curve, safe boot
offset, loud log — and it needs no new machinery. What must not happen is a
silent apply.

One prerequisite falls out of this and belongs to Phase 3, not here: Android's
devourer submodule is pinned at `73f1cb4` (2026-07-09), which **predates**
`GetPermanentMacAddress` (#383/#386). See B7.

### 2b. Still live — none; B9 and B10 both closed

Ordered by how much they constrain the design, not by size. **Seven of the
original nine have since closed in code and are kept below with a status
line rather than deleted, because each one's reasoning is what the next
phase is built on.** B6's residue and B12 closed with Phase 1a (PR #138);
B1, B4 and B5 with Phase 1b; B7 with Phase 1a′; B8 with Phase 1c. What
remains is **nothing**: B9 closed structurally with Phase 2a (see §4.5), and
B10 with §4.10 — the callback egress sink and the link gate that holds it.

#### B1 — device acquisition: enumeration vs. a wrapped fd

**CLOSED by Phase 1b.** `RadioAirCfg::adapter_fds` is the second device
source; see §4.2 for what landed and for the two things the survey below got
wrong.

`RadioAir::create` does `libusb_init` (`io/src/air_radio.cpp:579`) →
`libusb_get_device_list` (`:583`) → `libusb_open` (`:622`), matching
`kRealtekVid` (`:38`, tested `:595`) and an `lsusb -t`-style bus path.
**Unrooted Android cannot enumerate usbfs.**

The replacement is proven in the consumer.
`Waybeam-android/wifi/src/main/cpp/wifi_jni.cpp:363` `build_device()` does:

```
libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY)   :364
libusb_init(&out.ctx)                                            :365
libusb_wrap_sys_device(out.ctx, (intptr_t)fd, &out.handle)       :370
devourer::claim_interface_then_reset(handle, 0, logger,
                                     do_reset=false, lock, lock_dir)  :389
driver.CreateRtlDevice(out.handle, out.ctx, out.lock, {})        :405
```

Two details the first draft missed, both worth carrying into the
implementation: the claim is wrapped in a **6-attempt EBUSY retry loop with a
250 ms sleep** (`:388-394`), not a single call; and `lock_dir` is a JNI string
argument (`:379-383`) that every Kotlin caller fills with
`context.cacheDir.absolutePath` (`WifiDongleManager.kt:411`, used at `:233`,
`:260`, `:317`, `:359`).

Shape: `AdapterCfg`/`RadioAirCfg` needs a device *source* — bus path (today)
or a pre-opened fd — rather than a bus path string as the only way to name a
device. Not a large change; it is on the critical path because everything else
on Android sits behind it.

#### B4 — `do_reset` and the reopen path under a wrapped fd

**CLOSED by Phase 1b** — and **one claim below is wrong**, corrected in §4.2:
`recover()` performs no USB reset, so `do_reset=false` costs nothing there.

`RadioAir` hardcodes `/*do_reset=*/true` (`io/src/air_radio.cpp:634`).
`claim_interface_reset_reopen` recovers from reset re-enumeration by re-finding
the **same bus + port path** (`third_party/devourer/src/UsbOpen.h:68-72`).
Under a wrapped fd there is no bus path and the reset orphans the app's fd —
which is why the Android JNI passes `do_reset=false`.

So `do_reset` must become a construction parameter, and the consequence has to
be written down rather than discovered: **a devourer adapter opened by fd
cannot be reset.** That interacts with two shipped features:

- §11.6 Pass 80 `recover()` — the one-shot full re-init after a wedge.
- §9.10 v2 Pass 148 — the TX node *exits the process* on a sustained wedge
  (`app/main.cpp:5704-5710`, gated by `air.wedge_exit_windows`) so a
  supervisor re-execs it. There is no supervisor on Android, and a library
  must not call `exit()`. See B9.

`CLAUDE.md` already records that RTL88x2 USB wedges need a physical re-plug,
not a driver reload. On Android the honest recovery is "ask the Java layer to
close and re-request the fd" — a capability the library must *expose*, not
one it can perform.

#### B5 — `lock_dir` is not plumbed

**CLOSED by Phase 1b** (`RadioAirCfg::lock_dir`, empty = devourer's `/tmp`).

devourer parameterises the advisory lock directory
(`UsbOpen.h:47-52`, empty = `/tmp`), but `lock_dir` appears **nowhere** in
`io/` or `app/` — `RadioAir` passes only `ad->lock` (`air_radio.cpp:634`) and
takes the `/tmp` default. Android has no `/tmp`. Small and mechanical, and the
consumer already demonstrates the parameter's use.

#### B6 (residue) — compile-time feature options

**CLOSED by Phase 1a (PR #138).** `WBLINK_FRAME_SHM`, `WBLINK_CONTROL_SERVER`,
`WBLINK_VENC` and `WBLINK_BUILD_APP` all exist and all default ON; the survey
sentence below ("only `WBLINK_RADIO` exists as an option today") describes
`56463c0`, not `main`.

With `air_mon.cpp` deleted (Pass 164), **three** sources remain that
`wblink_io` compiles unconditionally and a phone cannot or should not have.
Only `WBLINK_RADIO` exists as an option today (`CMakeLists.txt:12`).

| source | why it must become optional |
|---|---|
| `io/src/frame_shm.cpp` | `shm_open` — **confirmed absent from bionic**, and it is a *compile* failure, not the link failure the earlier drafts predicted. bionic documents the omission deliberately: `bits/posix_limits.h:69`, `#define _POSIX_SHARED_MEMORY_OBJECTS __BIONIC_POSIX_FEATURE_MISSING /* mmap/munmap are implemented, but shm_open/shm_unlink are not. */`. Zero `shm_open` symbols in `libc.so`. |
| `io/src/control_server.cpp` | An HTTP listener a phone app does not want by default. |
| `io/src/venc_http.cpp` | TX/vehicle-side actuation; dead weight on a receiver. |

Proposed: `WBLINK_FRAME_SHM`, `WBLINK_CONTROL_SERVER`, `WBLINK_VENC`, all
defaulting `ON` so no existing preset changes behaviour.

#### B7 — build-system duplication is the extraction debt, made visible

**CLOSED by Phase 1a′ — see §4.3.** The consumable unit is
`add_subdirectory` + `wblink::io`; `find_package(wblink)` installs and exports
`wblink::core`. The Android copy described below can now be deleted, which is
Phase 3 work.

`waybeam-link/CMakeLists.txt` has **no `install()`, `export()` or `EXPORT`
target at all** — re-verified, zero occurrences. There is nothing to consume.

Meanwhile the Android consumer has hand-copied the wiring, and the copy has
already drifted exactly as predicted:

- the libusb-cmake `BUILD_SHARED_LIBS`/`LIBUSB_*` force-block
  (`wifi/src/main/cpp/CMakeLists.txt:8-12` against `CMakeLists.txt:29-36`);
- devourer chip-family selection (`:21-27` — JAGUAR1 + JAGUAR3_8822C +
  JAGUAR3_8822E on, hardcoded rather than driven by anything like
  `WBLINK_DEVOURER_CHIPS`). It matches our `fleet` set today **only by
  accident**: it never sets `DEVOURER_KESTREL_8852B`/`_8852C` because those
  options do not exist in its pinned `73f1cb4`. In our `5a5dd62` they exist
  and default **ON** (`third_party/devourer/CMakeLists.txt:32-33`), and our
  fleet path force-sets them OFF (`CMakeLists.txt:88-89`). **So the moment
  Phase 3 bumps that submodule to match ours, the Android build silently
  acquires two Kestrel 11ax families.** That is the sharpest instance of this
  document's own drift thesis, and it is a Phase 3 step, not a surprise;
- the static archive link order, copied verbatim down to the comment
  (`:35-36` "devourer must precede usb-1.0 (referencer before provider for
  static archives)" against `CMakeLists.txt:27`/`:154`);
- the `pkgconf-libusb.sh` shim — **correction to the first draft**: the script
  is copied to `wifi/src/main/cpp/cmake/pkgconf-libusb.sh` but is *not* wired
  from CMake. It is injected as a CMake argument from Gradle
  (`wifi/build.gradle.kts:31`, `-DPKG_CONFIG_EXECUTABLE=…`).

**And the second devourer is not a plain-tree copy — it is a git submodule**
(`.gitmodules` → `https://github.com/OpenIPC/devourer`) pinned at
**`73f1cb4`** (2026-07-09). Ours is a plain-tree copy of `5a5dd62`
(`third_party/README.md`). The two have drifted by roughly a month and ~160
upstream PRs: 86 differing or missing entries under `src/`, 6 under `hal/`.
Android's pin predates both the EFUSE MAC identity (#383/#386) and the
Jaguar3 EFUSE-walk append-order fix (#384). Their `libusb-cmake` pins do
match (`c8477c1`).

The library must export the wiring as one consumable CMake unit so the
consumer deletes its copy rather than maintaining a second one. Until then the
drift compounds, and B11's resolution is unavailable on Android until that
submodule is bumped.

#### B8 — log and diagnostic sinks — **CLOSED by Phase 1c (§4.4)**

**Status: closed by PR #146 (issue #144), 2026-08-08.** Both streams are now
injectable — `wb_log_set_sink()` for diagnostics (`io/include/wblink/log.h`)
and `StatsEmitter::set_local_sink()` for the §15.3 line. Defaults reproduce
the old behaviour exactly, so no flying node changed. See §4.4. The survey
below is kept because the *counting* is what the next phase inherits.

`io/src` mentions `stderr` in **28 places** (was 24) as counted before
Pass 164: `air_radio.cpp` 15, `air_mon.cpp` 8 (now deleted), `venc_http.cpp` 2,
and one each in `calib_store.cpp`, `frame_shm.cpp`, `config.cpp`. Those are token hits, not write statements —
in `air_radio.cpp` two are comments, one is a `set_diag_stream(stderr)`
restore and one is the `fflush` half of a preceding write, so the real figure
there is ~11. The other five files' counts are exact. That half of the blocker
is unchanged: a library must not own the consumer's stderr, and it needs an
injectable sink (logcat / syslog / caller callback).

**`stderr` is not the whole surface — `stdout` carries the stats stream.**
`StatsEmitter::emit` does `fwrite(…, stdout)` + `fflush(stdout)` whenever
`stats.to_stdout` is set (`io/src/stats.cpp:574`, key at
`io/src/config.cpp:826`). On Android stdout is discarded, so this is **silent
loss of the entire §15.3 stats line**, not log noise — a worse failure mode
than the stderr sites, and easy to miss because it is not a diagnostic path.
Phase 1c's injectable sink must cover both streams.

The other half was wrong in both earlier drafts. They said `fopencookie` "is
expected to carry [on bionic] from API 23 … so it should not be a blocker —
but that is an expectation, and the phase-1a preset is what settles it."
**Measured 2026-08-08 against NDK 26.3.11579264 (the version `:wifi` itself
pins), it does not exist at all:**

```
$ grep -rl fopencookie  <sysroot>/usr/include            → 0 files
$ nm -D --defined-only  <sysroot>/.../26/libc.so | grep -c fopencookie → 0
$ aarch64-linux-android26-clang++ t_fopencookie.cpp
  error: unknown type name 'cookie_io_functions_t'
```

This is not a logging nicety. `RadioAir::create` builds both devourer sinks
with it — the event stream that feeds the `tx.report` harvester and the diag
stream that carries the Pass 147 log-volume bound (`air_radio.cpp:521`,
`:532`). **The radio backend, the one thing Android definitely needs, does not
compile on bionic today.**

The remedy is cheap, which is why this is a scope change and not a redesign.
bionic ships BSD `funopen`/`funopen64` instead — present in `libc.so`,
declared `__INTRODUCED_IN(24)` under `__USE_BSD` (`stdio.h:232`), so it is
available at `:wifi`'s minSdk 26. Both devourer sinks take a plain `FILE*`, so
`funopen` is a drop-in behind a ~15-line adapter; the only real difference is
the write callback's signature (`int(void*, const char*, int)` against
`ssize_t(void*, const char*, size_t)`).

Consequence for the phasing: **1a and 1c are coupled.** The `android-arm64`
preset cannot come up green without at least the minimal `fopencookie` →
`funopen` shim, so that shim moves forward into 1a. The preset stops being a
discovery gate and becomes a regression gate — the discovery already happened,
here.

**Preserve on the way through:** `csa_psk` must never reach a log or stat
(`CLAUDE.md`). A new sink is a new output path that touches config.

#### B9 — the library must not own the process

Three process-level behaviours, all correct for a daemon and wrong for a
library:

- `main()` installs `SIGPIPE`/`SIGINT`/`SIGTERM` deliberately **without**
  `SA_RESTART` (`app/main.cpp:8613-8625`, with the reasoning in-comment).
- `spawn_mode_applier` double-forks (`:112-135`).
- Pass 148 exits the process on a sustained wedge (`:5704-5710`).

The wedge behaviour in particular is a *protocol-adjacent* decision (§9.10
v2), so "what does a library do instead of exiting" is a real question, not a
mechanical one. It is also now entangled with B4: on Android neither the exit
path nor `recover()`'s USB reset is available, so the answer has to be a
surfaced capability, not a substituted action.

#### B10 — egress: RX video has no callback sink

**CLOSED by §4.10.** `run_rx` takes an optional `FrameSink`; `BindKind` is
unchanged at `{kUdp, kFrameShm}` and nothing in §15.2 moved, exactly as the
survey below wanted ("additive"). The one thing the survey did not see is
that it was not merely additive but *required*: without it `wblink::node`
cannot link on the preset that models the phone.

`BindKind` is still `{kUdp, kFrameShm}` (`io/include/wblink/config.h:23`).
Android wants frames handed to MediaCodec. UDP-to-localhost works today with
zero library work and is the correct first step; a callback sink is the clean
end-state and is additive.

#### B12 — arch coverage

**CLOSED by Phase 1a (PR #138).** The `android-arm64` preset proposed at the
end of this section is the one that landed, with `WBLINK_WERROR=ON` so it
fails rather than prints. musl/OpenWRT stays untested and unclaimed.

Better than expected, unchanged. `ssc338q` is `arm-openipc-linux-gnueabihf`
(`cmake/toolchain-ssc338q.cmake:36-37`) — ARMv7 hard-float, 32-bit — and it
builds `wblink_io` and the full app, so `io/` is already 32-bit-proven, not
just `core/`. What that does **not** cover is the libc: it is glibc. Untested
are **bionic** (any arch) and **musl** (OpenWRT). Android's `:wifi` is
`arm64-v8a` only (`wifi/build.gradle.kts:15-16`, with a comment reserving
`armeabi-v7a` for a later phase).

Cheapest gate: an `android-arm64` compile-only preset alongside `ssc338q`, so
the bionic build breaks in CI rather than in the consumer.

## 3. Sequencing

### 3.1 The expand tranche is done — the first draft's table has expired

The draft sequenced the extraction around eight open issues. All eight have
landed:

| issue | landed | what it added |
|---|---|---|
| #95 scout occupancy | Pass 155 | `AirIface::rx_sense` (`air_iface.h:201`) + frame-free occupancy fields |
| #96 hardware ACK | Pass 156 | `air.tx_retry_limit`, the §3.0 coupling law |
| #97 LDPC/STBC | Pass 157 | `air.ldpc` / `air.stbc`, both default off |
| #98 SNR/EVM | Passes 158–160 | §15.3 quality window, §3.16 LINK_VERDICT `0x03`, §9.4 saturation gate |
| #99 quiet-gap budget | **stage 1 only, no Pass** | PR #130 is *"stage 1: §7.2 aim instrumentation (**Tier-2 bench knob**) + AU-uplink rule"* — a findings knob, not a ruling. Do not count it as landed. |
| #100 scout ranking | Pass 161 | hysteresis, confidence, enumerated reasons |
| #101 rate probing | Pass 163 | `AirIface::set_mcs_probe` (`:142`), `core/mcs_probe.{h,cpp}` |
| #92 asymmetric FEC | Pass 149 | `streams[].fec.e_rate_permille` (`config.cpp:402-419`), `fec_enhance_frames` |

Seven of the eight landed as Tier-1 rulings; **#99 is the exception** and is
called out in the table because an earlier version of this refresh swept it in
with the rest. Its GitHub issue and #134 both record it as actively
unreconciled — release-lateness mean ≈1462 µs against a Jaguar1 p99 ≈101 µs,
*"these do not reconcile … before any absolute number is quoted"*. It defines
no library surface, so it does not block the split; it simply is not done.

The GitHub issues for #95–#101 all remain OPEN pending device verification,
but for the other seven the code is merged and the surface they define is
settled. **#92 is closed**, so the whole of the draft's §3.0 — "#92 lands
first, and it moves one of Phase 0's two items" — is obsolete and has been
deleted rather than amended.

Two conclusions follow.

**The `AirIface` boundary survived two additions during the tranche.** That is
the strongest available evidence that the seam is in the right place. It also
means the draft's worry about adding methods to a published pure-virtual
interface has been tested twice at zero cost — because there are no external
implementers yet. The cost curve is exactly as the draft argued: cheap now,
expensive after the split.

**The awkward #95/#100 recommendation is 2/3 moot.** The draft proposed
splitting them into a boundary half before the split and a ranking half after.
Both landed whole, and `ScoutStore` came out as pure logic in
`io/include/wblink/scout_store.h` on its own. What is still true is step 2:
**`ScoutEngine` remains in `app/main.cpp:636`** and must be lifted, because
Android's quickconnect wants the scout. That is now a Phase 2a item, not a
sequencing dilemma.

### 3.2 What still has to land before the split

Exactly one thing, and it is not extraction work.

**#106 item 1 — key registration + `--check --strict --json` +
`config-schema --json`.** The library's config surface *is* its API, and
`io/src/config.cpp` reads every field through `value()`/`contains()` and never
enumerates the object — re-counted: **239 such sites** (190 `.value(` + 49
`.contains(`), and **zero** occurrences of any strict or unknown-key check
anywhere in `io/` or `app/`. An unrecognised key loads clean and flies wrong.
A library consumer has strictly less context than an operator does, so this
must precede any external consumer.

**One caveat on what item 1 actually buys.** #106's own ordering makes item 1
land `--strict` as a *warning*; it is item **8** that promotes it to an error,
and item 8 is gated on the generator (items 5–6). So item 1 gives the library
a published, golden-tested key registry — which is the part a consumer needs —
but the "loads clean and flies wrong" hole is not closed until item 8. Worth
saying plainly so nobody reads item 1 as the fix. #106 item **9** (an operator
ruling on author-supplied §10.6 artifacts, which touches calibration identity
and therefore B11) is also still open and is not addressed here.

The surface kept growing through the tranche, which is the argument for
freezing it rather than against: Pass 163 alone added `air.mcs_probe`
(`config.cpp:1130`), `policy.select.probe_veto_permille` /
`probe_veto_ttl_s` (`:544-547`), and the `table` object's
`probe:{period,slot}` — the last being *hashed content* (§3.6), so a
mis-registered key there is a fleet-lockstep failure, not a local one.

The draft queued this behind #92. #92 is closed, so **item 1 is unblocked and
is the head of the queue.**

`#106` items 3 and 4 are both overtaken — item 4 by Pass 162 (B2), item 3 by
ruling #120 (B3). They should be closed as such rather than ruled. That leaves
#106 with item 1, item 8, item 9 and the generator work.

**Issue #134 carries two items this document depends on**, and an earlier
version of this refresh omitted it entirely:

- *"#120 follow-through — cache/spectator configs flip to devourer (RX-only
  bring-up is hardware-proven; pure config change on those nodes)"* — this is
  the **hardware** evidence for B2. §2a argues B2 from a unit test; #134 is
  where the fleet actually proves it.
- *"#101 per-unit stage-0 … Fleet arming of `air.mcs_probe` (Pass 163,
  merged) waits on this"* — so a key Phase 0 must register is merged but not
  yet armable. That does not block registration; it does mean the registry
  will describe a key no node currently sets.

Both are operator-present bench legs, so neither is actionable here — they are
recorded so the split does not claim evidence it has not got. Issue #125
(saturation-knee calibration observability) does **not** bear on extraction.

### 3.3 What no longer needs an answer before the split

The draft listed two open rulings as Phase 0 blockers. Both are answered:

- B9's "what does a library do instead of `exit()`" is still an open design
  question, but it is no longer *blocking*: it only binds when the library is
  actually driving a node, which is Phase 2. Phase 1 does not touch it.
- B11's "identity under a wrapped fd" is closed by Pass 154 (§2a).

## 4. Suggested phasing

Each phase is independently landable and each keeps `ctest --preset dev`
(**61 suites**, ASan+UBSan) and `cmake --build --preset ssc338q` green.

**Phase 0 — the config surface.** #106 item 1 only. Close #106 items 3 and 4
as overtaken. Nothing here is extraction work; it constrains all of it.

**Phase 1 — mechanical, no behaviour change.**

- 1a. **LANDED (#138).** Feature options B6 and an
  `android-arm64` compile-only preset B12. Gate: every existing preset
  byte-identical. **Scoped and measured — see §4.1.** The export/install
  package B7 moved out of 1a into its own phase, 1a′ (§4.3), so 1a's
  "every preset unchanged" gate stayed easy to argue.
- 1b. **LANDED — see §4.2.** Device-source abstraction B1 (including the
  EBUSY retry loop), `lock_dir` B5, `do_reset` B4. Gate met: all four
  `deploy/*.json` `--check` dumps byte-identical, `dev` 61/61, `ssc338q` and
  `android-arm64` clean.
- 1c. Log-sink injection B8; `csa_psk` redaction preserved.

**Phase 2 — the actual extraction.**

- 2a. Lift `AirBackend`, `ScoutEngine`, `DiscoveryCatalog`, `TxCore`/`RxCore`
  wiring and the stats emitter out of `app/main.cpp` into a `node/` layer;
  `app/main.cpp` becomes a thin driver over it. `RxCore` first — it is already
  clean. B9 is answered here, where it binds.
- 2b. Callback egress sink B10.
- 2c. A stable facade. C++ first — Android reaches it through JNI either way,
  and a C ABI is only worth its cost if a non-C++ consumer appears.

**Phase 3 — the consumer.** The MonAir external repo is gone (Pass 164), so
Android is the whole of Phase 3. In this order:

1. Bump the devourer submodule from `73f1cb4` to match ours — a prerequisite
   for B11's identity — **and in the same commit force
   `DEVOURER_KESTREL_8852B`/`_8852C` OFF**, which the bump otherwise turns on
   by default (B7). Doing these as one step is the whole point; doing them as
   two ships an 11ax-capable build nobody asked for.
2. ~~Run B11's unproven leg~~ **DONE 2026-08-08, passes** — on the bench, with
   no phone (`tools/hwtrial_bringup --fd`). Identity under a wrapped fd matches
   the enumerated path on both fleet dies, so calibration is available on
   Android and the D3 fail-closed contingency is not needed for this reason.
3. Replace the leech in `wifi_jni.cpp` with the library and delete `:wifi`'s
   duplicated CMake wiring. **Resolve the claimed interface number here**
   (`find_wifi_interface()` before the claim, §4.2) — composite BT+WiFi
   dongles are a phone-population problem, not a fleet one, and it needs a
   composite adapter on the bench to verify.
4. Keep the survey/inspect JNI surface — it is genuinely useful and has no
   waybeam-link equivalent.

None of this starts before the archetype ruling in §0, which decides whether
the Android node needs a `role:"tx"` adapter at all.

### 4.1 Phase 1a, scoped

Phase 1a can start immediately. Both portability claims the earlier drafts
deferred to it are now **settled ahead of it** (§2b B6, B8): `shm_open` is
absent from bionic and `fopencookie` is absent from bionic. So 1a is no longer
a discovery step — it is the step that makes the two known absences
structurally impossible to regress.

The *optional-unit* half of 1a is still **much smaller than B6 makes it
sound**. The three optional units are **leaves**. Re-measured at `56463c0`:

- Nothing in `io/` includes `frame_shm.h`, `wblink/venc.h` or
  `control_server.h`.
- **But the includer list is wider than `.cpp` + `app/` + tests**, and an
  earlier version of this section got that wrong. Two `tools/` targets also
  include `frame_shm.h` — `tools/frame_shm_feed.cpp:27` and
  `tools/frame_shm_gst_bench.cpp:16` — and both are `add_executable` targets
  linking `wblink_io` (`CMakeLists.txt:186-190`, `:197-200`). They are gated
  **only** by `NOT CMAKE_CROSSCOMPILING`: not by `WBLINK_BUILD_APP`, not by
  `WBLINK_BUILD_TESTS`. So `WBLINK_FRAME_SHM=OFF` breaks every *native*
  preset unless 1a gates them too. This is the one place where the phase has
  no existing escape hatch, and it is exactly the class of thing an
  include-grep of `io/` cannot see.
- `io/src/udp.cpp` touches frame-shm only as a `BindKind::kFrameShm` enum
  check (`:244-246` — "frame-shm streams are owned by the app (FrameShmRing),
  not by BindingSet"). It never calls into `FrameShmRing`.
- `io/src/config.cpp`'s frame-shm references are all the enum and its
  validation messages. No API use. (Count them by spelling and you get 12
  `frame-shm` strings plus 6 `BindKind::kFrameShm` over 18 lines; the earlier
  "9" counted one spelling. The substantive point — enum and validation only,
  never an API call — is what matters and it holds either way.)
- `control_server.cpp` and `venc_http.cpp` depend only on `binding.h`
  (`split_host_port`).
- The `MonAir` mentions in `io/src/air_radio.cpp` were comments and are gone
  with the backend (Pass 164).

Grep is the weak form of that argument, so it was also checked at the link
level, which is the form that actually decides whether the archive still
resolves. Across all 16 objects in `build/dev/libwblink_io.a`, **no object
other than the three themselves carries an undefined reference to
`wblink::FrameShmRing`, `wblink::ControlServer` or the venc symbols**:

```
nm -C --undefined-only build/dev/libwblink_io.a \
  | awk '/\.cpp\.o:$/{o=$0} /FrameShmRing|ControlServer|Venc/{print o" <- "$0}' \
  | grep -v '^\(frame_shm\|control_server\|venc_http\).cpp.o'
  → empty
```

**So the optional-unit half of 1a is CMake-only: three options and three
`target_sources()` guards, with zero source edits in `io/`.** A `BindKind` a
build cannot construct becomes a runtime config error, which is the honest
outcome and needs no new machinery.

**But 1a is no longer CMake-only overall.** `air_radio.cpp` is not optional on
Android — it is the whole point — and it does not compile against bionic
(B8). The minimal `fopencookie` → `funopen` shim therefore lands inside 1a, or
the preset cannot come up green. That is the one real source edit in the
phase, it is confined to `air_radio.cpp`'s two sink constructions, and the
rest of B8 (injectable sinks for the 28 `stderr` sites) stays in 1c where the
draft put it.

Three further wrinkles. Two are already solved by existing options; the third
is not, and is the reason 1a's acceptance below has a `FATAL_ERROR` clause.

- `app/main.cpp` references all three unconditionally (`FrameShm` ×10 +
  `ShmOut` ×12, `ControlServer` ×6). Guarding those would be real surgery, and
  it is unnecessary: **Android links `wblink_io` + `wblink_core`, not the
  executable.** Add a `WBLINK_BUILD_APP` option (default ON) and have the
  `android-arm64` preset turn it off. The 12 `WBLINK_RADIO` sites already in
  `app/main.cpp` show the alternative if the daemon ever does need to build
  without a subsystem — the precedent exists, it just should not be spent
  here.
- The four tests that include the optional headers (`frame_shm_test`,
  `frame_shm_loopback_test`, `venc_actuator_test`, `control_server_test`) need
  no guarding either: `WBLINK_BUILD_TESTS` already exists
  (`CMakeLists.txt:9`) and the compile-only preset turns it off.
- **`tools/frame_shm_feed` and `tools/frame_shm_gst_bench` have no such
  option.** They are gated on `NOT CMAKE_CROSSCOMPILING` alone, so unlike the
  app and the tests they cannot be switched off by anything that exists
  today. 1a must gate them on `WBLINK_FRAME_SHM` explicitly. This is the one
  genuinely new build-system item the phase carries.

**LANDED 2026-08-08** (`impl/phase1a-bionic-build`). What the phase actually
cost, against the estimate above: four CMake options, three
`target_sources()` guards, five guarded tests, three `nfds_t` casts, one
92-line header, one toolchain wrapper, one preset. The `dev` translation-unit
set is byte-identical before and after (231 TUs, diffed via
`compile_commands.json`), so the "no behaviour change" claim is measured
rather than asserted. Two things the estimate missed and the build found:
the strip block at `CMakeLists.txt:204` also references the `waybeam-link`
target and needed the `WBLINK_BUILD_APP` guard, and **`nfds_t` is `unsigned
int` on bionic against `unsigned long` on glibc**, so three `::poll()` call
sites tripped `-Wconversion` (`io/src/udp.cpp:307`, `:349`,
`io/src/air_udp.cpp:205`). That third one is the preset earning its cost on
its first run — a real 64→32-bit narrowing that no existing gate could see.

**Three forward items the Phase-1a reviews surfaced**, recorded here rather
than acted on, because each belongs to a later phase:

- **`WBLINK_VENC=OFF` is right today and wrong as a destination.**
  `VencActuator` is a non-optional member of `TxCore` (`app/main.cpp:3602`)
  driven every tick, and it is the sole egress for the §9.6 rate decision — an
  HTTP GET to a same-SoC `waybeam_venc`. That is meaningless on a phone
  encoding through MediaCodec, so a full-TX Android node needs a **rate-sink
  abstraction** with `venc_http` as one implementation, not `WBLINK_VENC=ON`.
- **`WBLINK_FRAME_SHM=OFF` stays correct**, and #137 (filed after this
  document merged) confirms the direction: it proposes an injected frame-sink
  at the `core/frame_reassembler.cpp` layer with `io/src/frame_shm.cpp`
  demoted to one sink among several. The residual footgun is that
  `io/src/config.cpp` still parses a `frame-shm` binding kind unconditionally,
  so a build without the ring accepts such a config and produces nothing. Not
  reachable in-repo — only `app/main.cpp` constructs `FrameShmRing`, and
  `WBLINK_BUILD_APP=ON` forces the option ON — and #137 may retire the shape
  entirely, so no load-time refusal is added for a consumer that does not yet
  exist.
- **devourer does NOT block the wrapped-fd path, and a review claim that it
  does is wrong.** `claim_interface_then_reset`
  (`third_party/devourer/src/UsbOpen.cpp:74`) takes an **already-opened
  handle** and never enumerates; the `libusb_get_device_list`/`libusb_open`
  pair at `:209`/`:222` lives in `claim_interface_reset_reopen` (`:167`), the
  reopen-after-reset path that `do_reset=false` does not take. The Android
  consumer already calls the plain variant (`wifi_jni.cpp:389`). So B1 needs
  **no `third_party/` change and no upstream request** — it stays bounded to
  `io/src/air_radio.cpp`.

Acceptance for 1a:

- `WBLINK_FRAME_SHM`, `WBLINK_CONTROL_SERVER`, `WBLINK_VENC`,
  `WBLINK_BUILD_APP`, all defaulting `ON`.
- **The invalid matrix cells fail loudly, not confusingly.** `app/main.cpp`
  includes all three optional headers unconditionally (`:37`, `:48`, `:73`),
  so `WBLINK_FRAME_SHM=OFF` with `WBLINK_BUILD_APP=ON` cannot compile;
  likewise the two `tools/` targets against `WBLINK_FRAME_SHM=OFF`. Each such
  combination must `message(FATAL_ERROR …)` at configure time. An option
  matrix with a silently broken cell is worse than no option.
- The two `tools/` frame-shm targets gated on `WBLINK_FRAME_SHM` as well as
  the existing `NOT CMAKE_CROSSCOMPILING`.
- **All seven** configure presets unchanged — `dev`, **`release`**,
  `x86-ground`, `rk3566`, `ssc338q`, `ssc338q-au`, `ssc338q-eu` — same
  sources, same warnings, `ctest --preset dev` 60/60 green.
- The `fopencookie` → `funopen` shim, with the glibc path byte-identical to
  today (the shim selected by the platform, not by a new config knob).
- A new `android-arm64` preset that builds `wblink_core` + `wblink_io` only,
  radio on, the other three off, app and tests off, **and comes up green** —
  which now means the two known bionic absences are fenced, not that they are
  discovered. NDK 26.3.11579264 is present on the dev host and is the version
  `:wifi` pins, so this preset is buildable here today.

### 4.2 Phase 1b, as landed

Closes B1, B4 and B5. `RadioAirCfg` gains three fields — `adapter_fds`,
`do_reset` (default `true`) and `lock_dir` (default empty) — all
**programmatic-only** per the operator's 2026-08-08 ruling: `io/src/config.cpp`
is untouched, no key is added, and the #106 registry learns nothing. The
measured consequence is that all four `deploy/*.json` produce a
**byte-identical `--check` dump** before and after.

**Two survey claims did not survive contact with the code.** Both were
probe-executed, not re-read:

1. **`do_reset=false` does not disable §11.6 `recover()`.** B4 above says it
   does, and the follow-on reasoning about "the honest recovery is to ask the
   Java layer to re-request the fd" was built on that. `RadioAir::recover()`
   is `StopRxLoop` → `SetMonitorChannel` → `StartRxLoop` — it calls no
   `libusb_reset_device`, because Pass 143 had already established that
   `InitWrite` is one-shot and the restartable surface is the RX loop. So the
   only thing `do_reset=false` gives up is the **bring-up** reset. Recovery on
   an fd-supplied adapter is exactly recovery on an enumerated one. §9.10's
   process-exit wedge path (B9) is unaffected by this and remains open.
2. **A wrapped fd and an enumerated adapter can coexist in one process.** The
   consumer sets `LIBUSB_OPTION_NO_DEVICE_DISCOVERY` globally via
   `libusb_set_option(nullptr, …)`, which reads as "this process cannot
   enumerate". It is in fact stored **per context**
   (`linux_context_priv::no_device_discovery`,
   `third_party/libusb-cmake/libusb/libusb/os/linux_usbfs.c`), consulted in
   `op_init` to skip the usbfs scan. `RadioAir` already gives every adapter
   its own context, so Phase 1b sets the option per-context through
   `libusb_init_context` — the fd adapter skips discovery, a path-claimed
   adapter beside it still enumerates. It must be set **at init**: setting it
   on a live context arrives after the scan and leaves `op_exit`'s
   `init_count` unbalanced.

**What the fd path skips, and what it keeps.** A wrapped handle carries no
port numbers (`op_wrap_sys_device` calls `initialize_device` with no sysfs
dir), so an fd-supplied stanza is excluded from bus-path claiming, from
`used_paths`, and from the §15.2 bus re-bind pass — carried by an explicit
`Adapter::by_fd` flag, not by the emptiness of a string. It **keeps the mac
re-bind**, which still works because identity is read off the die. A stanza
that supplies both an fd and a `bus` pin is refused: the pin can be neither
honoured nor checked, and §15.2's posture is that a pin which stops meaning
what it says must be loud.

**Fail-closed rules, all validated before any libusb call** (so
`tests/radio_fd_source_test.cpp` is hermetic, like `radio_rx_only_test`):
`adapter_fds` must be empty or exactly parallel to `adapters`; no fd may back
two stanzas; an fd-supplied stanza may not carry a `bus` pin; and `do_reset`
must be `false` when any fd is supplied — refused rather than silently
downgraded, since the reset re-enumerates and orphans the caller's fd.

**Ownership.** libusb marks a wrapped handle `fd_keep`, so teardown closes
libusb's side and leaves the caller's descriptor open. The fd must outlive
the `RadioAir`, and closing it is the caller's job.

**Two behaviour changes on the enumerated path**, both deliberate:

- **`LIBUSB_ERROR_BUSY` at claim is now retried 6 × 250 ms** instead of
  failing immediately (the consumer's proven shape). BUSY is never a
  configuration error, so waiting cannot mask one, and every other failure
  code still returns at once. It buys a supervisor re-exec the second it takes
  for a dead owner's advisory lock and kernel claim to clear.
- **A leak on `create()`'s failure paths is fixed.** The in-flight `Adapter`
  was a local `unique_ptr` not yet pushed into `Impl`, so a context opened
  moments earlier was dropped on the floor when the adapter search failed —
  **measured at 2 fds per failed `create()`, 100 over 50 calls**, and invisible
  to LeakSanitizer because libusb keeps every context on a global list. It
  barely mattered for a daemon that exits on the failure, and it matters a lot
  for a library consumer that retries in-process (Android, after a permission
  grant). Adapters are now owned by `Impl` from allocation, so `~Impl` runs
  over the half-built one. Same probe after the fix: 0.

**One gap stays open and belongs to Phase 3: the claimed interface number.**
`RadioAir` claims interface `0` (`air_radio.cpp`, both device sources), and
so does the Android JNI today, so nothing regresses. But devourer's own header
says `find_wifi_interface()` **must** run first, because composite RTL8822BU
adapters put Bluetooth on interfaces 0/1 and WiFi on 2
(`third_party/devourer/src/UsbOpen.h`). Our fleet's 8812AU/CU/EU are not
composite, which is why this has never bitten. The fd path opens the door to
exactly the population where it would — a phone claiming whatever dongle the
user plugs in — so it must be fixed before Android ships, and it needs a
composite adapter on a bench to verify, which is why it is not fixed here.

**Review-driven changes worth carrying forward as method notes.** The
pre-merge review found no defect in the production change, and one CRITICAL
**in the new test**: a case asserting the `-1 = enumerate` semantics ran a
full enumerated bring-up, which on this host only failed because
`/dev/bus/usb` is root-owned. On a bench rig with the usbfs udev rule that
lets `waybeam-link` run unprivileged, it would have reset and brought up live
flight hardware inside `ctest`. Two rules fall out: **a unit test must never
let `create()` enumerate**, and a case that reaches the device open must
supply an fd that is provably not a USB device (the test uses `/dev/null`,
verified by `strace`: no device node opened, no sysfs walk). The same review
also showed one assertion passing for the wrong reason — a mac-pin case whose
pinned stanza was never reached because an earlier stanza failed first.

**B11's unproven leg has since been RUN and passes** (2026-08-08, issue #140
leg B2) — see `docs/findings.md`. It never needed a phone: `libusb_wrap_sys_device`
takes any usbfs fd on Linux, so `tools/hwtrial_bringup --fd <bus>/<dev>`
exercises the whole path on the bench. Identity under a wrapped fd is
byte-identical to the enumerated path on both fleet dies, so §10.6 calibration
is available on Android and B11 closes.

### 4.3 Phase 1a′, as landed

Closes B7. The survey framed this as "there is no `install()` rule", which is
true but is not the blocker. **Probing an actual embedding consumer first
changed the whole scope**, and it is the reason this phase is small: a
throwaway project that did `add_subdirectory(waybeam-link)` and linked
`wblink_io` configured and built *successfully* on the pre-change tree — so
the survey's framing would have had us write install rules for a problem that
was not the one hurting. What that consumer actually got was:

- its `PKG_CONFIG_EXECUTABLE` **cache entry permanently redirected** to
  `cmake/pkgconf-libusb.sh`, a shim that answers `libusb-1.0` and `exit 1`s on
  everything else — so every other `pkg_check_modules()` in the consuming
  project failed. Fail-loud rather than fail-silent, but fatal either way;
- its `BUILD_SHARED_LIBS` forced `OFF` project-wide, silently turning its own
  shared libraries into static ones;
- **133 targets**, including the `waybeam-link` daemon, all 61 test binaries
  and both bench tools.

All three came from `set(... CACHE ... FORCE)` at the top of our
`CMakeLists.txt`. The fix is that `option()` honours a normal variable under
CMP0077 (NEW at our `cmake_minimum_required`), and a normal variable shadows
the cache for this directory and everything below it — identical effect on our
build, nothing written to the consumer's cache. The daemon's world now
defaults to `${PROJECT_IS_TOP_LEVEL}`.

**What landed**: namespaced aliases `wblink::core` / `wblink::io`; `cxx_std_20`
as a PUBLIC usage requirement on both; the `FORCE`-cache block converted to
normal variables (libusb, devourer chip families, pkg-config shim);
`WBLINK_BUILD_APP` / `WBLINK_BUILD_TESTS` and both `tools/` targets gated on
`PROJECT_IS_TOP_LEVEL`; an `install()`/`export()` package for `wblink_core`;
and `examples/embed-consumer/`, which asserts all four properties **at
configure time** so a regression is a build failure rather than something a
reader has to notice.

**The asymmetry is deliberate, not unfinished.** `install(EXPORT)` refuses a
target whose link interface names targets outside the export set, and
`wblink_io` PUBLIC-links `devourer` and `usb-1.0`. devourer has no `install()`
rule at all, and libusb-cmake's are force-disabled here because we link its
static target directly. Exporting `wblink_io` would mean authoring install
rules for vendored trees we may not edit, whose interface include directories
point into our source tree. So `find_package(wblink)` offers `wblink::core`;
embedding offers both. The remaining named consumer (Android via Gradle)
embeds, so nothing is blocked by this.

**Two defects found by building a consumer rather than reading the CMake**,
worth recording because neither is visible in a source review: `install(EXPORT
NAMESPACE wblink::)` prefixes the **target** name, so the package exported
`wblink::wblink_core` while embedding offered `wblink::core` — the same
library under two spellings depending on how it was consumed, fixed with
`EXPORT_NAME`. And `CMAKE_CXX_STANDARD` is directory-scoped and does not
travel with an exported target, so a `find_package()` consumer at its
compiler's default standard failed inside our own `table.h`.

**The review of this phase found five real defects, all in the new code and
all found by probing rather than reading**, and two are worth carrying:
`add_subdirectory(... ${CMAKE_BINARY_DIR}/libusb)` puts the vendored trees at
the *consumer's* build root when embedded, and `Waybeam-android` vendors
libusb-cmake at byte-identically the same expression — so its hand-copy must
be deleted in the **same commit** that adds `add_subdirectory(waybeam-link)`;
the migration cannot be staged. And the normal-variable conversion quietly
gave up a guarantee `CACHE ... FORCE` had: if a future re-vendor lowered
devourer's `cmake_minimum_required` below 3.13, CMP0077 would go OLD, our
option values would be ignored **with no warning**, and a vehicle build would
silently gain two 11ax families. The tree now asserts on what devourer
actually compiled (`DEVOURER_HAVE_KESTREL_*` in its interface definitions),
because checking our own variable would prove nothing — CMP0077 OLD overwrites
it only inside devourer's scope.

One review claim was **wrong and is recorded so it is not re-raised**: that
Android already builds both Kestrel families by devourer default. Its pinned
`73f1cb4` has **no Kestrel `option()` at all** (7 `DEVOURER_*` options against
our 9), which is exactly why its hand-copy sets 7 entries and matches our
fleet set today. The risk is entirely in the bump, as §2b B7 says.

**Not moved**: the Android submodule bump and the Kestrel force-OFF that must
accompany it stay Phase 3 (§4). This phase makes the deletion of Android's
hand-copy *possible*; it does not perform it.

### 4.4 Phase 1c, as landed

PR #146, issue #144. Two sinks, both defaulting to exactly what the code did
before, so every flying node is byte-for-byte unaffected — verified by running
the daemon, not by reading the diff.

**Diagnostics** (`io/include/wblink/log.h`, `io/src/log.cpp`). One
process-wide sink, installed with `wb_log_set_sink()`; `wb_logf()` is the
printf-style entry point, `wb_log_write()` the pre-formatted one, and
`wb_log_stream()` a `FILE*` view for the vendored APIs that accept nothing
else. Default sink writes to `stderr`.

Three decisions worth recording, because each had a plausible alternative:

- **A global, not a threaded-through logger object.** `io/` has no logger, no
  context reaching `RadioAir`'s RX threads, and no per-object diagnostics
  policy. Twenty-four line-oriented writes do not justify inventing one; a
  function pointer is the whole requirement.
- **The sink is one atomic pointer to a caller-owned `LogSink`, not an
  atomic pair.** Storing `fn` and `cookie` as two atomics would let a reader
  pair a new callback with the previous cookie. That is a use-after-free with
  a plausible-looking stack, so it is designed out rather than documented
  around.
- **`wb_logf` allocates rather than truncates.** It formats into a 512-byte
  stack buffer and re-formats on the heap if that is short. A truncated
  diagnostic loses its tail, and the tail is where the bus path and the MAC
  are — the two things that make the line worth emitting. Boundary cases 510,
  511, 512, 513 are asserted.

`RadioAir`'s teardown also moved off `stderr`: the destructor used to restore
`set_diag_stream(stderr)` before closing our cookie stream, which would have
pulled devourer's last lines back out of a consumer's sink. It now restores
`wb_log_stream()`. That change is defensive rather than load-bearing — the
`Logger` is owned by `Impl` and dies a few statements later, and every other
holder is released above it, so nothing can currently log through the
re-pointed stream. What matters is the destination, not the lifetime.

**The §15.3 stats line** (`StatsEmitter::set_local_sink`). It **replaces** the
stdout write rather than adding to it, so a consumer cannot double-emit and
`stats.stdout` keeps meaning what it meant for every node that installs
nothing. The UDP binding is independent of both. No config key, nothing
harness-visible, and `last_line()` still serves §15.5 the identical bytes.

**Two counts in §2b B8 were wrong, in opposite directions**, which is worth
recording because the survey's numbers were quoted onward. The real write
count in `air_radio.cpp` is **11 `fprintf` sites plus 2 raw stream writes**,
not 15 tokens; and the total converted across `io/src` is **24 `fprintf` sites
+ 2 raw**, against the survey's "28 mentions, ~11 real". The survey was
counting `stderr` tokens, which include comments and `fflush` halves.

**Verified by probe, not by reading:**

- Negative control on each half separately — forcing the stats dispatch past
  the sink, and forcing the log sink to the default — makes `log_sink_test`
  fail 3 and 9 checks respectively, and the broken stats build visibly leaks
  the §15.3 line onto the test's stdout. Restored: 30 checks, 0 failures.
- `--check` on a config that trips the §10.3 clamp: 2 diagnostics on fd 2,
  **0 on fd 1**. A diagnostic leaking to stdout would corrupt the NDJSON
  stream, so the fd separation is the property, not the message.
- `loopback` for 3 s: 3 stats lines on stdout, all parsing as JSON.
- On hardware (issue #140, both bench dongles, kernel drivers unloaded):
  `hwtrial_bringup --bus 8-1 --bus 5-1` brings both units up and the converted
  `air_radio.cpp` sites print correctly with the right MACs
  (`20:0d:b0:c4:a7:6a` Jaguar1, `40:a5:ef:2f:23:08` Jaguar3); teardown clean
  through the new `wb_log_stream()` path.
- **Legs A5, A4 and A6 run at the same time**, closing everything in issue
  #140 that does not need an Android phone. A5: a real spectator node emits
  §15.3 with a populated `adapters[]`, 500 ms apart at `stats.hz=2`, no fd
  crossover. A4: the §15.2 mac-pin re-bind corrects both stanzas when listing
  order disagrees with enumeration order, and both warnings (`DISPLACED`,
  `NOT PRESENT`) fire where they should. A6: `recover()` restores RX — 7054
  frames across the recovery — with **no USB reset**, which #139 had only
  asserted from reading the code. See `docs/findings.md`.
**Pre-merge review found four defects worth recording, because three of them
are properties no amount of reading would have surfaced:**

- **CRITICAL, and it was a use-after-free at every process exit.**
  `wb_log_stream()` is never closed, so the C library's exit-time cleanup
  flushes it — and that runs *after* static destructors, i.e. after a
  consumer's sink object is gone. Reproduced under ASan. The stream is now
  `_IONBF`, so there is never residue to flush. `setvbuf` here is correctness,
  not tuning, and the same change fixes the second finding.
- **The stream was fully buffered**, so it handed the sink 8192-byte chunks
  split mid-line — a direct breach of `log.h`'s "one complete `\n`-terminated
  message" contract. It only appeared to work because vendored devourer
  happens to `fflush` after every line, i.e. correctness rested on an
  unenforceable property of code `CLAUDE.md` forbids editing.
- **The teardown contract was documented backwards.** Uninstalling a sink does
  not wait for in-flight calls, so the instinctive shutdown order (stop the
  callbacks, then tear down) is a use-after-free while an RX thread is inside
  the callback. The safe order — destroy wblink objects, *then* uninstall,
  *then* free — is now stated explicitly in `log.h`. No lock was added: it
  would sit in the RX threads' diagnostic path, and the ordering is the
  cheaper contract.
- **Four of the test's own stated properties were vacuous or unasserted**,
  and the two memory-safety defects sat exactly underneath them: an `fflush`
  hid the buffering bug, `udp=nullptr` made the independence claim untestable,
  the psk check never ran the loader that touches the secret, and nothing
  captured fd 1 or fd 2 — so "the defaults are unchanged", the single claim
  protecting every flying node, was not being checked at all. The test now
  redirects both descriptors and asserts placement, not just callback arrival.
  47 checks, up from 30.

The mechanical rewrite itself came through clean: every format string is
byte-identical after literal concatenation, and `-Wformat` under
`-Wall -Wextra -Wconversion` covers all 24 sites for arity and type.

- All 26 gates green, `android-arm64` included — and the bionic gate now
  covers `cookie_stream.h` from `log.cpp` as well, so it no longer depends on
  `WBLINK_RADIO=ON` to compile the shim at all.

### 4.5 Phase 2a, first move, as landed

`RxCore` and `rx_policy()` out of `app/main.cpp` into `node/`, plus the
`wblink_node` target and the layering rule. **No behaviour change, and no
Tier-1 surface moves — there is no Pass entry for this and there should not
be.**

Scoped deliberately small. `RxCore` went first because the plan said it was
already clean, and that held on inspection: 570 lines referencing no other
app-layer structure — no `AirBackend`, no `ScoutEngine`, no `UplinkPower`, no
`TxCore` — only `core/` plus `Config` and `StatsSnapshot` from `io/`.
`rx_policy()` moved with it because `RxCore`'s constructor was its only
caller; its siblings (`arq_policy`, `selector_policy`, `quietgap_policy`) have
callers this layer does not and stay behind until the TX half moves.

**Header-only, on purpose.** `RxCore` is entirely inline, so `node/` adds no
translation unit and the `dev` TU set is unchanged from before the move — the
same no-change control Phase 1a used. It becomes a STATIC library the moment
something here needs a `.cpp`.

**The gate that matters** is not the 25 green checks; it is that the struct
is byte-identical to the one that left `main.cpp` (verified by substring
against `git show HEAD:app/main.cpp`), so the move cannot have changed
behaviour.

**What it buys immediately:** `tests/node_rx_core_test.cpp` constructs
`RxCore` from one `#include`, with no `app/main.cpp` include, no suppressed
`main()`, and no `-Wno-unused-function`. That is the first time any of this
code has been reachable from a real unit test, and it is the direct answer to
why several Pass 165-167 defects could only be proven on hardware. Writing it
immediately paid: the first draft asserted that `select_originator()` sets
`selected_originator()`, and the real contract is that the accessor reports
the *engine's* output-want pin — nullopt on a node with no `dir:"out"` stream,
however often it is called. Both shapes are now pinned.

**Two build-system notes.** `wblink_node` is an INTERFACE target linked into
`waybeam-link`, `app_test` and the new suite; it is deliberately **not**
exported by `find_package(wblink)`, for the same reason `wblink_io` is not —
the install package stays `wblink::core` alone. And the `embed-consumer` gate
passes unchanged, which is the check that a new target has not leaked into a
consumer's build.

**Second move, same shape:** `DiscoveryCatalog` (168 lines) and `ScoutEngine`
(420) into `node/discovery.h`. Both were nominated clean and both were: no
app-layer references at all. `ScoutEngine` was already built for injection —
every side effect it has is a `Hooks` callback — which is why its test drives
a full sweep with no radio, no socket and no clock, and can pin the two
properties that are invisible from outside: that `start()` widens the RX
filter to hear all net_ids (a sweep that keeps the narrow filter finds only
its own fleet and calls the band empty), and that `stop()` restores while
`abandon()` deliberately does not.

**Third move — the first one that was not clean.** `AirBackend` (524 lines)
carries `PacketEventTrace` (122) with it, so the two moved together rather
than leaving a reference across the layer boundary. Pulling them out then
forced four more pieces over, and each is a small argument for the layer:
`packet_type_name` and `mcs_trace_enabled` (their only callers moved),
`now_ms`/`now_us` into `clock.h`, and the §7.2 `AimHist` pair into `aim.h`.

The clock and histogram moves are the ones worth reading. Both were
file-static in `main.cpp`, which is identical to `inline` **in one translation
unit and not otherwise** — a per-TU copy would mean `AirBackend` accumulating
into one histogram while `run_rx` dumps another. They are `inline` variables
now. Nothing about that was visible while the code had exactly one consumer;
the move is what made the distinction real, which is the general shape of what
this phase surfaces.

**Fourth move: `TxCore`, and the entanglement was not where the plan thought.**
1812 lines, flagged as the one genuinely tangled with `run_rx` — and it turned
out to have **no app-layer code dependency at all**. The two `UplinkPower`
mentions in it are comments about the ground half. What it actually needed were
six free helpers (`s_to_ms`, `selector_policy`, `calib_params_from`,
`bw_code`, `scheduler_policy`, plus `seed_calib_policy`, which is a member and
travelled for free). `resolve_power_qdb` and `load_power_curve` did **not**
move: they were already in `io/`, which is the layering working as intended.

The real cost was invisible from `main.cpp`: **eleven headers** that TxCore had
been getting transitively — `scheduler.h`, `framer.h`, `frame_framer.h`,
`frame_caps.h`, `frame_shm.h`, `fps_ladder.h`, `jscc_runtime_shadow.h`,
`mcs_probe.h`, `report_gate.h` and two more. Every one compiled fine inside the
8.2k-line TU and none was declared. That is the tax the layer is collecting,
and it is a one-time tax per move.

**Final move: the §15.3 stats assembly.** `emit_stats()` (177 lines),
`ArqTimingTracker`, `Loaded`, the two fill helpers and `InfoSelfState`. This
one could not go earlier — `emit_stats()` reads `AirBackend`, `RxCore`,
`TxCore` and `Loaded`, so it is the join point of the whole node and was
always going to be last. It is also the piece that proves the layer: with it
here, a consumer can produce a §15.3 line without `app/main.cpp` at all.

### B9 — ANSWERED, and structurally rather than by design

B9 asked "what does a library do instead of `exit()`", listing three
process-level behaviours. Phase 2a did not have to decide: **all three stayed
in `app/main.cpp`**, which is now the driver, while every node-behaviour
object moved out. The Pass 148 sustained-wedge path was already
`return kExitTxWedged` — a value `main()` propagates, not an `exit()` in the
loop — and the signal handlers and `spawn_mode_applier`'s double fork never
belonged to any of the moved objects.

Measured, not asserted: `node/` contains **no** free-standing call to `exit`,
`_exit`, `abort`, `fork`, `execl*`, `signal`, `sigaction`, `raise`, `atexit`
or `longjmp`. All nine such calls in the tree are in `app/main.cpp`.

`tests/node_layering_test.py` makes it stay true, and enforces the other half
of the rule at the same time — that nothing in `core/` or `io/` includes a
`node/` header. Both rules were mutation-checked independently: injecting a
`::exit(1)` into `node/clock.h` and an `#include "wblink/node/clock.h"` into
`io/stats.h` produces exactly one failure each. The guard also fails if
`node/` has no sources, so it cannot pass vacuously.

### 4.6 Phase 2b cannot go before 2c — measured, not planned

The phasing above lists **2b (B10 callback egress sink) then 2c (facade)**.
That order does not work, and the reason only became visible once 2a landed.

B10 wants RX video frames handed to a consumer callback instead of a UDP
socket. Every part of that path — `write_egress`, `egress_for`, the 13
`shm_outs` sites, both `Deliver` lambdas — lives **inside `run_rx`**, a
2662-line function in `app/main.cpp`'s anonymous namespace (opened at
`app/main.cpp:90`). Nothing outside that translation unit can call it.

So after 2a a consumer linking `wblink::node` can *build* an `RxCore`, a
`TxCore` and an `AirBackend`, and can fill a §15.3 snapshot — but it **cannot
run a node**, because `node/` exposes no run loop and the three that exist are
unlinkable. Adding a callback sink now would create a seam nothing can attach
to, which the repo's minimal-implementation rule specifically forbids
("do not create abstractions for hypothetical future requirements"). The
requirement is real — Android — but it is not reachable yet.

**Revised order: 2c before 2b.** The facade is not a thin wrapper over the
existing pieces; it is the job of lifting `run_rx`'s construction and event
loop into `node/` so that something can own the sink. Once a consumer can
construct and drive a node, B10 is a small additive change to a path that has
a caller — and it should follow the PR #139 precedent and be
**programmatic-only**, with no `BindKind` value and no §15.2 surface, so it
stays Tier-2 and needs no ruling.

> **Half of that is wrong — see §4.8, measured while landing 2c step 1.**
> The dependency runs *both* ways: B10's seam is unreachable until the lift
> (true, above), and the lift cannot reach its target consumer until B10
> exists. They land together. The paragraph is kept because everything else
> in it — that the facade is not a thin wrapper, and that B10 stays
> programmatic-only — still holds.

**What that lift is, honestly.** `run_rx` keeps its state in locals captured
by reference from ~40 lambdas. That structure is not incidental: it is exactly
what produced the `issue_vcmd` stack-use-after-scope crash found during
Pass 166 verification — a handler capturing a block-scoped local by reference
while the ControlServer outlived the block. Lifting it means giving those
locals an owning struct, which is worth doing for that reason alone, and it is
the largest single piece of work left in #109. It wants a fresh session and
its own byte-identity strategy, because unlike every 2a move it is **not** a
verbatim relocation.

**What is left of #109:** 2c (lift `run_rx` — the big one) together with 2b
(B10 — see §4.8 for why they cannot be separated), then Phase 3 — Android —
which binds at the end, when the layer owns enough to make "what does a
library do instead of `exit()`" a question with a concrete answer.

### 4.7 Phase 2c step 1, as landed — `run_rx`'s free-function dependencies

`run_rx` cannot move while the things it calls stay behind it, so the first
step measured what those actually are and moved that set. **Verbatim, checked
byte-for-byte**, so this step carries no behaviour risk of its own and the
lift that follows starts from a clean boundary.

**The measurement is the useful part.** Of the twenty-odd free functions and
structs left in `app/main.cpp`'s anonymous namespace, `run_rx` reads twelve,
and — this is the part the plan had wrong — **`spawn_mode_applier` is not one
of them**. The double fork is reached only from `run_tx` and `run_loopback`
(`app/main.cpp:655`, `:887`, `:4143`), and the §9.10 `kExitTxWedged` return is
likewise off the RX path. So two of B9's three process-level behaviours were
never in `run_rx` at all, which is why 2a could answer B9 structurally without
deciding anything: `run_rx` was already clean of them.

Fifteen free functions and one struct moved, not twelve: `announce_token` and
`frame_is_live_rtp_data` travelled with their families rather than leave one
member behind a layer boundary, and `power_tier_json` is reached from the RX
path only through `UplinkPower::json()`.

What moved, and where it went:

| destination | contents | why there |
|---|---|---|
| `node/policy.h` | `csa_params`, `vcmd_params`, `quietgap_policy`, `channel_allowed` | §15.2 Config → core param blocks. Siblings of the five already in `tx_core.h`, but read by BOTH loops, so they cannot live in a header named for the TX half. `channel_allowed` tests `policy.csa.channel_allowlist` — the block `csa_params` translates. |
| `node/vcmd.h` | `vcmd_id_for`, `vcmd_name_for`, `mtu_tier_for_mode` | §15.5 REST spelling ↔ §11.7 ids. `core/` owns the ids in `types.h` and must not learn the control-plane vocabulary. |
| `node/frame_kind.h` | `frame_is_eob`, `frame_is_paced_eob`, `frame_is_live_rtp_data` | §7.2 predicates. The third is TX-only and moved anyway: one family, one header. |
| `node/entropy.h` | `session_nonce`, `announce_token` | `core/` is deliberately RNG-free and only ever verifies against a supplied key, so the two `/dev/urandom` reads have always belonged a layer up. |
| `node/uplink_power.h` | `UplinkPower`, `power_tier_json` | The §10 precedence chain. See below. |
| `stats_fill.h` | `build_info_json`, `build_health_json` | The node's JSON output, reading the same three objects the §15.3 assembly already reads. |

**`UplinkPower` is the one that pays immediately.** It was already built for
injection — `apply_qdb`, `apply_auto` and `artifact_qdb` are all callbacks —
so the whole precedence chain was always *testable* with no radio, no file
and no socket. It just was not *reachable*: the only path to it ran through
`app_test`'s whole-TU `#include "app/main.cpp"`.

Be precise about which chain, because the first draft of this section was
not. The two order-dependent tier defects Pass 166/167 verification found —
`install_curve()` re-seeding the adapter ceiling, and a craft latch
outranking the tier — belong to **`TxCore`**, not to `UplinkPower`, and stay
pinned in `tests/app_test.cpp` where they were found. `UplinkPower` is the
**ground** uplink's twin of that chain: same two operations, same ordering,
and the same never-exercised sequence, since every existing case installs a
curve *before* selecting a tier. `tests/node_uplink_power_test.cpp` runs it
both ways round and pins seven rules — latch reports the request while
actuation is clamped; an out-of-range tier is REJECTED, not clamped; a tier
re-lowers a configured map (the Pass 136 shape); `effective` follows the owner
and not the tier; a configured map outranks an artifact; the artifact callback
does not run above its rank (it has a stale-flag side effect); and `apply()`
falls through to backend auto rather than to silence. Every mutant of those
rules is killed.

**The check.** `app/main.cpp` went 4692 → 4363 lines, 71 suites green.
`tools/move_identity.py` verifies all sixteen moved blocks against a base
revision of `app/main.cpp`: it takes each source line range, applies the
permitted edits, and asserts the result appears verbatim in the destination.
The **only** permitted edit is the storage-class prefix a header needs
(`inline`; `static` → `inline` on `power_tier_json`) and the
continuation-line realignment that prefix forces — both enumerated in the
script, so a third kind of edit fails it. It is a review-time tool, not a
merge gate: it compares against a base revision that moves, so `scripts/`
deliberately does not run it. Mutating one moved line turns it red, which is
the control that a substring check can otherwise pass vacuously.

### 4.8 The real constraint on lifting `run_rx` — and why B10 comes with it

§4.6 says the hard part of the lift is that `run_rx` keeps its state in
locals captured by reference from ~40 lambdas. That is true, and it is a real
hazard — it is the structure that produced the Pass 166 `issue_vcmd`
stack-use-after-scope crash. But it is not what *blocks* the lift, and the
difference decides the order of the remaining work.

**What blocks it is a build configuration.** `run_rx` names `FrameShmRing` at
5 sites and its local `ShmOut` wrapper — which holds one — at 11 more, plus
`ControlServer` at 2, all unconditionally: there is no `#if` around any of
them, because `WBLINK_BUILD_APP=ON` refuses to configure
without all three optional subsystems, so `app/` has never had to care.
`node/` does not get that luxury:

```
$ jq '.configurePresets[] | select(.name=="android-arm64") | .cacheVariables'
{ "WBLINK_RADIO": "ON", "WBLINK_WERROR": "ON", "WBLINK_BUILD_APP": "OFF",
  "WBLINK_BUILD_TESTS": "OFF", "WBLINK_FRAME_SHM": "OFF",
  "WBLINK_CONTROL_SERVER": "OFF", "WBLINK_VENC": "OFF" }
```

`android-arm64` is the preset that exists **specifically** to prove the Phase 3
consumer, and it builds with frame-shm off, because bionic has no `shm_open`
(the same reason `io/include/wblink/cookie_stream.h` exists for
`fopencookie`). A verbatim `run_rx` in `node/` therefore cannot serve the one
consumer the whole extraction is for.

So the dependency is mutual, and §4.6 recorded only one direction of it:

- B10's seam is unreachable until the lift — nothing outside `app/main.cpp`'s
  TU can call `run_rx`, so a sink would have no caller (§4.6, still true).
- The lift cannot reach its target consumer until B10 — the egress path has
  to survive `WBLINK_FRAME_SHM=OFF`, and a callback sink is exactly what that
  takes.

**They land together.** Neither is a follow-on to the other, and planning them
as separate phases is what made the order look decidable.

**One trap to design around rather than discover.** `android-arm64` is
compile-only and builds libraries, and a static archive does not resolve its
own undefined symbols. A `node/` that references `FrameShmRing::create`
without `io/src/frame_shm.cpp` in the build would therefore go **green** on
that preset and fail in a consumer's link instead — the gate that exists to
catch bionic problems would sail past this one. Whatever shape B10 takes, the
check that it worked is not "the android preset builds"; it is a link, which
today means extending `examples/embed-consumer` or accepting that this stays
unproven until `:wifi` links it.

### 4.9 Phase 2c step 2, as landed — the RX run loop

`run_rx` is in `node/src/rx_node.cpp`. `app/main.cpp` is **1714 lines**, down
from 8808 before Phase 2a and 4363 before this step. `wblink_node` is STATIC —
the `.cpp` the Phase 2a CMake comment said would arrive when something needed
one.

**It was a verbatim move, and §4.6's estimate of why it would not be was
wrong in an instructive way.** The warning was that `run_rx` keeps its state
in locals captured by reference from ~40 lambdas. That is true, and it is the
structure that produced the Pass 166 `issue_vcmd` stack-use-after-scope crash.
But it describes the function's *interior*, and a move only cares about its
*exterior*. After step 1 moved the free functions, `run_rx` referenced exactly
**one** app-scope name — the stop flag. So the whole 2661-line move is two
edited lines, both declared in `tools/move_identity.py`:

```
int run_rx(const Loaded& l) {            ->  int run_rx(const Loaded& l, const std::atomic<int>& stop) {
    while (g_stop == 0) {                ->      while (stop == 0) {
```

The lesson generalises the one Phase 2a already recorded about `TxCore`:
**measure the coupling before believing an estimate of it.** Both times the
plan's "entangled" call was made by reading the code's shape rather than by
counting what it reaches.

**The stop flag is the only design decision.** In `app/main.cpp` it was a
file-scope `volatile sig_atomic_t` written by a signal handler. A library
cannot own that, and the consumer that matters — Android — stops a node from
another *thread*, where `volatile` says nothing at all. `std::atomic<int>` is
both: lock-free on every target here, so it stays legal to write from a signal
handler, and properly synchronised across threads. `app/main.cpp` keeps the
flag, keeps the handlers, and passes a reference; `static_assert
(std::atomic<int>::is_always_lock_free)` sits next to it so the
signal-handler half cannot quietly stop being true.

**What the move exposed: eight ODR violations, latent since Phase 2a.**
`emit_stats`, `rx_policy`, `packet_type_name`, `bw_code`, `s_to_ms`,
`selector_policy`, `scheduler_policy` and `calib_params_from` were all defined
at namespace scope in `node/` headers **without `inline`**. Nothing could
catch it: `app/main.cpp` was the only translation unit that included them, and
one definition in one TU links fine. The instant the RX loop became a second
TU in the same binary, the linker rejected `emit_stats` outright — the first
thing this step did was fail to link.

This is the same failure Phase 2a recorded for the aim histograms
(file-`static` is identical to `inline` in one TU and not otherwise), arriving
a second time by a different route. So it is now rule 3 of
`tests/node_layering_test.py` rather than a thing to remember.

**And the guard needed a guard.** Its first draft stripped `//` comments before
the regex but not before the brace test, so `uint8_t bw_code(uint8_t w) {  //
note` passed while the linker rejected it — the cheapest possible way to
reintroduce exactly the bug the rule exists to catch, found in pre-merge
review. The rule now joins a signature's continuation lines before deciding
(which also stops it flagging a correctly wrapped *declaration*), covers
variables as well as functions — a namespace-scope `static` variable in a
header never link-errors at all, it just forks per TU, which is the
aim-histogram failure and the worse one — and carries a `_SELF_TEST` table of
shapes it must and must not catch, so a matcher that stops matching fails
rather than passes. Its remaining limits (a column-0 anchor, explicit template
specialisations) are stated in the file.

**The android trap is no longer hypothetical — it is measured.** §4.8 predicted
that a `node/` naming `FrameShmRing::create` without `frame_shm.cpp` in the
build would go green on `android-arm64` and fail in a consumer's link. It does:

```
$ llvm-nm -u build/android-arm64/libwblink_node.a | wc -l          # 187 undefined
$ ...minus symbols defined in the preset's own archives, the NDK sysroot,
   and libunwind                                                   # -> 9
$ llvm-nm -u build/android-arm64/libwblink_node.a \
      | grep -cE 'FrameShmRing|ControlServer'                      # 9 — the same 9
```

Do the subtraction, not just the `grep`: the filtered form returns 9 whether or
not a *third* unresolvable subsystem is present, so on its own it cannot verify
the thing it is being used to verify. Done properly the answer is the same —
five `FrameShmRing` symbols and four `ControlServer` ones, nothing else — which
is what fixes B10's scope. `scripts/gates.sh` meanwhile reports
`passed 25  failed 0  skipped 0`. **This step ships that state deliberately**,
because the alternative — inventing the egress seam here — would have made a
2661-line verbatim move unreviewable. B10 closes it, and the check that B10
worked must be a **link**, since a build demonstrably is not one.

### 4.10 B10, as landed — the callback egress sink

`wblink::node` now links with `WBLINK_FRAME_SHM=OFF` and
`WBLINK_CONTROL_SERVER=OFF`. That was the last thing standing between the layer
and its Phase 3 consumer.

**The seam is one `std::function`, and no config can select it.** Following the
PR #139 precedent it is programmatic-only — no `BindKind` value, nothing in
§15.2 — so it stays Tier 2 and needs no ruling:

```cpp
using FrameSink = std::function<void(uint8_t stream_id,
                                     const uint8_t* frame, size_t len)>;
int run_rx(const Loaded& l, const std::atomic<int>& stop,
           const FrameSink& frame_out = {});
```

**What it takes, and what it deliberately does not.** §15.2's
`bind.kind: "frame-shm"` is the *whole-frame* egress kind — blocks reassembled
into a frame, frame handed on — and B10 makes *where* it is handed a choice
rather than a constant. A supplied sink takes those streams. UDP-bound
out-streams are datagram egress, a different thing, and are left alone;
hijacking them would surprise a consumer that wanted only its video. The old
`ShmOut` is `FrameOut` now, because a name that says "shm" on the
Android-facing path would be a lie.

**Both new `#if` arms fail closed.** A frame-shm stream configured on a build
without the subsystem and without a sink is refused at startup, as is a
`control.bind` on a build without the server. A node that silently dropped its
video because the build lacked a subsystem its config asks for would look like
a link fault and be debugged as one.

**The gate is a LINK, and it had to be.** `examples/node-linkcheck` configures
the tree exactly as a phone would — all three optional subsystems off, no
daemon — and links an executable against `wblink::node`. `scripts/gates.sh`
runs it (27 gates now, up from 25).

It is a separate gate from `examples/embed-consumer` on purpose: that one holds
B7 (embedding does not build the daemon's world), and folding two properties
into one gate blurs what a red result means.

Negative control, and the first attempt at it was wrong in a way worth
recording: reverting the `#if` around the ring branch made the mutant fail to
*compile*, and a check that counted "undefined reference" lines scored that as
zero — a passing result for a broken build. Reverting the struct member as well
lets the TU compile and puts the linker back in the loop, which is where the
check lives: **3 undefined references, build exit 2.** Restored, it links and
runs.

Measured after: `llvm-nm -u build/android-arm64/libwblink_node.a` names
**zero** `FrameShmRing` or `ControlServer` symbols, down from nine.

**§15.3 stays honest on the sink path**, and the first draft of this did not.
`frame_count`, `frame_bytes`, `frame_size_*`, `frame_interval_us` and
`frame_jitter_us` come from the *ring's* counters, and `io/src/stats.cpp`
emits those keys **unconditionally** — so simply skipping the entry for a
sink-backed stream published zeros, which is precisely the stalled-egress
reading the code comment claimed to be avoiding. `write_egress` now keeps the
same counters itself, mirroring `FrameShmRing::note_frame` including its
fixed-point jitter, so a consumer cannot tell which egress produced them. The
ring-only fields (`reads`, `ring_full`, the producer-health block) stay zero
because there is no ring to observe.

### 4.11 B10 proven at runtime — byte-exact, on localhost

The link gate proves the sink's symbols resolve. It cannot prove a frame
reaches it, and no unit test can either: `run_rx` opens adapters and blocks.

`tools/frame_sink_probe` closes that. It is the smallest thing that is a real
consumer — links `wblink::node`, supplies a `FrameSink`, counts what arrives —
and `tools/frame_sink_bench.sh` drives it end to end with no radio, no
hardware and no second repo:

```
frame_shm_feed produce   ->  waybeam-link tx (frame-shm ingest, FEC, udp air)
                         ->  frame_sink_probe (egress = callback)

producer frames=180 bytes=3960000 full_drop=0 oversize_drop=0
probe:   stream 0: 180 frames, 3960000 bytes, size 20000..80000
```

**Byte-exact**, and the §15.3 counters the sink path now keeps for itself read
`frame_count 180`, `frame_bytes 3960000`, `frame_interval_us 33291` at the
producer's 30 fps. Had the first draft shipped — the one that skipped the
stats entry for a sink-backed stream — every one of those would have been 0
while video flowed.

**The pairing is the trap, and it cost a run.** The first attempt fed the
frame-shm RX from `config.air-tx.sample.json`, which ingests *RTP datagrams*.
The RX received all 10799 of them and emitted **zero frames** — which reads
exactly like a broken sink. It is not: whole-frame egress needs a TX that
*framed* the input, so `config.frame-shm-tx.sample.json` is the only correct
partner. What settled it was the control rather than the reasoning: stock `rx`
mode on the same feed also reports `frame_count 0`, which exonerates the sink
before any of its code is read. Run the control first.

**What is still not proven** is that the frames are *decodable by a real
consumer* — the probe counts bytes, it does not decode. That wants
**waybeam-hub on the x86 dev host**, whose `mod_pixelpilot` /
`mod_video_player` already consume whole frames from the frame-SHM ring;
wiring it to take them from the callback instead tests the seam against a
decoder that exists, on a machine already on the bench. Then Android, then the
RK3566 decoder (operator's order, 2026-08-09).

### 4.12 The C ABI, and what the consumers turn out to want

`node/include/wblink/node/rx_node_c.h` — four functions, `extern "C"`.

**Both named consumers are C.** waybeam-hub is a C daemon; Android reaches
native code through JNI. Without a shared shim each writes its own wrapper,
and both have to invent an answer for the same problem: `run_rx` takes a
`const std::atomic<int>&`, and a C caller cannot name that type. C11's
`_Atomic int` is not guaranteed layout-compatible with it, so passing the flag
across the boundary would be a portability bet. The shim owns the flag behind
an opaque handle instead, and the question disappears.

It deliberately does **not** own a thread. `wblink_rx_run` blocks and the
caller supplies the thread — what `app/main.cpp` does and what the hub's module
model already does. `node/` staying out of thread policy is the same argument
that keeps it out of process policy (B9).

**A handle runs once**, and that is the design decision review forced. The stop
flag is sticky — `run_rx` takes it by const reference and cannot clear it — so
a second run would fall straight out of the loop and return a healthy-looking
**0** *after* claiming the adapter: a dead node that looks fine, against a
consumer (the hub) whose model is start/stop/start. Clearing the flag on entry
instead would lose a stop issued between spawning a thread and that thread
reaching the call, which is a race to hand a user. So reuse returns 3, and a
consumer creates a handle per start. Relatedly, a pre-stop now returns before
`load_all` and `AirBackend::create`: the header promised that call was prompt
while it was in fact ~1800 lines and one radio-open away.

**The gate is a C translation unit, compiled with a dialect.**
`examples/node-linkcheck` compiles `c_consumer.c` **as C11** with
`-Wall -Wextra -Wpedantic -Wstrict-prototypes -Werror`, and links it beside
the C++ side. Both halves of that matter, and review found both:

- The first evidence for this gate was two mutants — `std::size_t` in the
  header, and the `extern "C"` guard deleted — and **neither isolates the C
  axis**. Both also fail the C++ TU, so the gate would have looked healthy
  while proving nothing new. The mutants that actually isolate it are C-only:
  a **default argument**, an **empty struct**, and an **unprototyped
  function** — each valid C++, each rejected by the C compile, verified
  independently (`C++ errors=0, C errors≥1` for all three).
- Without a dialect and `-Wpedantic`, the last two of those are silent GCC
  extensions. A header can be invalid ISO C and green here, and waybeam-hub
  at `-std=c99 -pedantic-errors` would then be the thing that discovers it.

**The gate also RUNS.** Two contract rules need no hardware, because a
pre-stopped handle opens nothing: a pre-stop returns 0 without reading the
config, and a second run on the same handle returns 3. `scripts/gates.sh`
executes the binary rather than only building it — the first draft printed its
verdict and exited 0, which is a gate that reports failure as success.
Gates: 27 → 28.

**`tools/frame_sink_probe` now drives the C ABI rather than `run_rx`**, so the
runtime proof re-runs through the boundary the consumers will actually use —
still byte-exact, 180 frames / 3960000 bytes. A C++ probe over the C++ API
would have left the ABI unexercised at runtime, which is most of the point of
having one.

**Linking it from C is not free, and nothing said so.** This is a C++ library
behind a C header: a C consumer needs `-lstdc++ -lm` (a bare `cc` link produces
~3000 undefined references), the archives in order, and — since
`find_package(wblink)` exports `wblink::core` alone — either `add_subdirectory`
or a Makefile pointed at the build tree. That is now in the header, where the
consumer will actually read it.

**What the consumers turn out to want (operator, 2026-08-09).** This is not a
second video source that lives beside frame-SHM. waybeam-hub will **drop
frame-SHM on the ground** and run the node in-process, and the same module
goes on the **vehicle** running a TX node — where the encoder's frame-SHM
ingest stays, because that is venc→link and nothing replaces it. Two things
follow: **`run_tx` still lives in `app/main.cpp`** and has to be lifted the way
`run_rx` was before the vehicle half can start; and the eventual
`protocols/frame-shm.md` edit is consumer-side only, since the producer path
is untouched. The `wblink_rx_*` naming leaves room for `wblink_tx_*` beside it,
so nothing landing here needs redesigning for that.

## 5. Loose ends worth knowing before starting

- **The §15.3 counters schema** is the last per-backend dispatch in
  `AirBackend`, set aside by Pass 140 because the two backends' counter
  structs differed by real fields. **Pass 164 resolved it by subtraction**: the
  MonAir stats loop is deleted, so only the radio branch remains. Phase 2a
  inherits one dispatch, not two.
- **`profiles/` by relative cwd** (`CLAUDE.md`) is a smaller problem than it
  looks: `profile_table` is a config key and `load_profile_table_json()` takes
  a string, so a consumer passes the table as text and never touches a path.
- **License is settled and is not a blocker.** `Waybeam-android/LICENSE.md:1`
  carries `SPDX-License-Identifier: GPL-2.0-or-later` with a "Why the license
  changed" section naming the devourer link as the trigger, matching
  waybeam-link. **Correction to the first draft:** two docs are stale, not
  three — `README.md:58` and `USERGUIDE.md:291` still say "Autod Personal Use
  License"; that repo's `CLAUDE.md` has no license section at all. A docs fix
  on the Android side, worth doing before anyone reads them as authoritative.
- **`Waybeam-android` records no plan to consume this library.** Zero
  references to `waybeam-link` or `wblink` anywhere in that repo. The
  coordination happens entirely in this document and in the coordination repo;
  Phase 3 should start by writing it down there.
- **`app/main.cpp:111` misattributes a fork.** It credits `RadioAir` with
  `execvp('iw', …)` for channel retunes as precedent for the double-fork; that
  was `MonAir`, now deleted. `RadioAir` is fork-free, which is the property
  that matters here. Still unfixed, and now the cited precedent does not exist
  at all.

## 6. What changed in the 2026-08-08 refresh

For a reader of the Pass-148 draft. Conclusions that moved:

- **B2 resolved** (Pass 162) — the draft's single hardest blocker, and it was
  answered by the expand tranche rather than by extraction work.
- **B3 dissolved, B6's `air_mon` leg closed by deletion** (ruling #120 as
  amended by Pass 164 — DROP, not relocate).
- **B11 closed** (Pass 154, EFUSE-MAC identity read off the die).
- **§3.0 deleted** — #92 is closed and its implementation landed as Pass 149.
- **The #95/#100 split recommendation is moot** — both landed whole; only
  "lift `ScoutEngine`" survives, as a Phase 2a item.
- **#106 item 1 is unblocked and is now the only pre-split dependency**;
  items 3 and 4 are overtaken.
- **B7 is worse than described** — the second devourer is a *submodule* at
  `73f1cb4`, ~160 upstream PRs behind ours, and predates the identity work
  B11's resolution depends on. The `pkgconf` shim is injected from Gradle, not
  CMakeLists.
- **B1 gained two implementation details** — the 6-attempt EBUSY retry loop
  and the `cacheDir` lock path.
- **B2's resolution exposed a successor question, and §0 was wrong.** Both
  drafts said Android is `rx-spectator` and listed §8 ARQ/NACK, §9
  LINK_REPORT and §11 CSA follow among what it gains. A spectator gets none
  of the three (`PROTOCOL.md:4508-4519`), and the archetype is derived, not
  chosen. Whether the Android target is a spectator or a ground with an
  uplink is now the document's one open operator question.
- **B11 was over-claimed and is corrected here.** An earlier version of this
  refresh said the identity is available under a wrapped fd "by
  construction". It is not: the EFUSE read follows `InitWrite` bring-up
  (`air_radio.cpp:685` → `:693`) per devourer's own contract, and the Android
  consumer never calls `InitWrite`. Status is *closed for enumerated handles,
  one unproven leg under a wrapped fd*.
- **§4.1's includer enumeration was wrong** — `tools/frame_shm_feed.cpp` and
  `tools/frame_shm_gst_bench.cpp` also include `frame_shm.h`, and are gated
  only on `NOT CMAKE_CROSSCOMPILING`. The link-level claim survived
  unchanged; the target inventory did not.
- **#99 did not land** as a Tier-1 ruling — PR #130 is a stage-1 Tier-2 bench
  knob, and #134 records it as unreconciled. Seven of eight, not eight.
- **#134 was omitted** and carries B2's hardware evidence plus the reason
  `air.mcs_probe` is merged but not fleet-armable.
- **B8 went the wrong way, and it is the refresh's one genuinely new
  blocker.** Both drafts expected `fopencookie` to exist on bionic and treated
  it as a non-blocker. Measured against NDK 26.3.11579264 it is absent
  entirely — headers and `libc.so` both — so `RadioAir` does not compile on
  Android today. `funopen` is the ~15-line remedy, but the shim moves into
  Phase 1a, which is therefore no longer CMake-only. The lesson is the
  document's own: the two claims flagged as "expectations, settled by the
  phase-1a preset" were settled by *running the compiler for ten seconds*
  instead, and one of the two was wrong.

Numbers that moved: `app/main.cpp` 7,540 → **8,635**; `run_rx` ~2,360 →
**~2,600**; ctest 46 → **60** suites; `io/src` stderr sites 24 → **28**;
config `value()`/`contains()` sites ~225 → **239**; `WBLINK_RADIO` sites in
`app/main.cpp` 6 → **12**; 
vendored devourer `800c3c8` → **`5a5dd62`**. `AirIface` gained two methods
(`rx_sense`, `set_mcs_probe`), reaching 26 pure virtuals.

Findings 1–14 of an adversarial review are folded in above; three were
CRITICAL (the §0 archetype gap, the B11 over-claim, the missing `tools/`
targets) and each was re-verified against the source before being accepted.

Citations that survived unchanged: `io/include/wblink/config.h:23`
(`BindKind`) and `io/src/udp.cpp:246` (the frame-shm enum check). The string
loaders moved (`config.cpp` 113 → **115**, 1110 → **1313**) — caught by
probe-executing every `file:line` in this document against the tree rather
than by re-reading it, which is the only reason the refresh does not ship its
own stale numbers.
