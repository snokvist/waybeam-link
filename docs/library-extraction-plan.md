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

**The MonAir external repo is the second proving consumer, by ruling.** Issue
#120 item 3: at the library split, `MonAir` is *moved, not rewritten* into a
new external RX-only repo that vendors the extracted core. That makes the
split's success criterion concrete — the extraction is done when a repo
outside this one can build a receiving node — and it removes `air_mon.cpp`
from the conditional-compilation problem entirely (§2, B6).

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
| `tx_index()` | returns 0; meaningful only under `has_tx()`, so the §15.5a scout roams adapter 0, matching kernel-monitor |
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

**B3 — backend mix — DISSOLVED by ruling #120.** The draft asked whether
`air.kind` should stay node-level or grow a per-adapter `backend` key, so a
mixed devourer + kernel-monitor RX node could be represented. Ruling #120
makes devourer the sole in-tree backend and moves `MonAir` out at the split.
There is no second in-tree backend to mix with, so the question no longer has
a subject. `#106` item 3 should be closed as overtaken rather than ruled.
`air.kind` stays node-level; a consumer copying that shape copies the right
one.

**B6's `air_mon.cpp` leg — SOLVED BY RELOCATION, not conditional
compilation.** Ruling #120 item 3 moves the file to an external repo at the
split. It never needs a `WBLINK_MON` option; it needs a destination. This is
the cheaper resolution by a wide margin — 927 lines plus its backend branches
leave the tree instead of acquiring a build flag.

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

### 2b. Still live — nine

Ordered by how much they constrain the design, not by size.

#### B1 — device acquisition: enumeration vs. a wrapped fd

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

devourer parameterises the advisory lock directory
(`UsbOpen.h:47-52`, empty = `/tmp`), but `lock_dir` appears **nowhere** in
`io/` or `app/` — `RadioAir` passes only `ad->lock` (`air_radio.cpp:634`) and
takes the `/tmp` default. Android has no `/tmp`. Small and mechanical, and the
consumer already demonstrates the parameter's use.

#### B6 (residue) — compile-time feature options

With `air_mon.cpp` leaving by relocation, **three** sources remain that
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

#### B8 — log and diagnostic sinks — **now a confirmed hard blocker, and it gates `RadioAir` itself**

`io/src` mentions `stderr` in **28 places** (was 24): `air_radio.cpp` 15,
`air_mon.cpp` 8, `venc_http.cpp` 2, and one each in `calib_store.cpp`,
`frame_shm.cpp`, `config.cpp`. Those are token hits, not write statements —
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

`BindKind` is still `{kUdp, kFrameShm}` (`io/include/wblink/config.h:23`).
Android wants frames handed to MediaCodec. UDP-to-localhost works today with
zero library work and is the correct first step; a callback sink is the clean
end-state and is additive.

#### B12 — arch coverage

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
(**60 suites**, ASan+UBSan) and `cmake --build --preset ssc338q` green.

**Phase 0 — the config surface.** #106 item 1 only. Close #106 items 3 and 4
as overtaken. Nothing here is extraction work; it constrains all of it.

**Phase 1 — mechanical, no behaviour change.**

- 1a. Feature options B6, the export/install package B7, and an
  `android-arm64` compile-only preset B12. Gate: every existing preset
  byte-identical. **Scoped and measured — see §4.1.**
- 1b. Device-source abstraction B1 (including the EBUSY retry loop),
  `lock_dir` B5, `do_reset` B4. Gate: the x86 bench path unchanged; a
  wrapped-fd path that compiles.
- 1c. Log-sink injection B8; `csa_psk` redaction preserved.

**Phase 2 — the actual extraction.**

- 2a. Lift `AirBackend`, `ScoutEngine`, `DiscoveryCatalog`, `TxCore`/`RxCore`
  wiring and the stats emitter out of `app/main.cpp` into a `node/` layer;
  `app/main.cpp` becomes a thin driver over it. `RxCore` first — it is already
  clean. B9 is answered here, where it binds.
- 2b. Callback egress sink B10.
- 2c. A stable facade. C++ first — Android reaches it through JNI either way,
  and a C ABI is only worth its cost if a non-C++ consumer appears.

**Phase 3 — the two consumers.** Move `MonAir` to its external RX-only repo
(ruling #120 item 3) — the split's proof that an outside repo can build a
receiving node. And on Android, in this order:

1. Bump the devourer submodule from `73f1cb4` to match ours — a prerequisite
   for B11's identity — **and in the same commit force
   `DEVOURER_KESTREL_8852B`/`_8852C` OFF**, which the bump otherwise turns on
   by default (B7). Doing these as one step is the whole point; doing them as
   two ships an 11ax-capable build nobody asked for.
2. Run B11's unproven leg: wrapped fd + `do_reset=false` + `InitWrite` +
   EFUSE walk on a real dongle. If it returns false, D3 fail-closed is the
   answer and calibration is simply unavailable on Android — which must read
   as STALE, never as a silent apply.
3. Replace the leech in `wifi_jni.cpp` with the library and delete `:wifi`'s
   duplicated CMake wiring.
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
- The two remaining `MonAir` mentions in `io/src/air_radio.cpp` (`:142`,
  `:1279`) are both comments.

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

## 5. Loose ends worth knowing before starting

- **The §15.3 counters schema** is the last per-backend dispatch in
  `AirBackend` (`app/main.cpp:1471-1484`), set aside by Pass 140 because the
  two backends' counter structs differ by real fields. Ruling #120 changes the
  economics: with `MonAir` leaving the tree, the question may resolve by
  subtraction rather than by unification. Worth revisiting at Phase 2a rather
  than deciding now.
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
  is `MonAir` (`io/src/air_mon.cpp:261`, `:274`). `RadioAir` is fork-free,
  which is the property that matters here. Still unfixed.

## 6. What changed in the 2026-08-08 refresh

For a reader of the Pass-148 draft. Conclusions that moved:

- **B2 resolved** (Pass 162) — the draft's single hardest blocker, and it was
  answered by the expand tranche rather than by extraction work.
- **B3 dissolved, B6's `air_mon` leg solved by relocation** (ruling #120).
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
