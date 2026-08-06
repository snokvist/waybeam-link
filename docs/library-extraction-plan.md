# Library extraction — state of the art, blockers, and issue sequencing (2026-08-06)

**Survey and planning only. No spec ruling is made here, so there is no
`docs/review-log.md` Pass entry attached.** Two items below need an operator
ruling and are marked **OPEN**; both are already tracked as ordered work in
[#106](https://github.com/snokvist/waybeam-link/issues/106). When either is
ruled, it commits first as a spec amendment plus a numbered Pass, per repo law.

**Citations are symbol-first.** File:line is as of `main` at Pass 148. Per
`docs/devourer-parity-plan.md`'s standing warning, prefer the symbol name over
the number when they disagree — Pass 140 invalidated a whole round of numbers
once already.

Companions: `docs/devourer-parity-plan.md` (whose "Library extraction — notes
carried forward" section this supersedes and expands), `docs/config-harness-plan.md`
(whose §2/§2a rulings gate part of this), `docs/devourer-integration-analysis.md`
(the upside register behind #95–#101).

**#92 (asymmetric FEC for non-referenced frames) lands first.** It is in flight
and it moves two surfaces this plan proposes to freeze — see §3.0.

## Why this document exists

`docs/devourer-parity-plan.md` closed its register with the observation that the
library ambition "is unchanged and is now closer": `AirIface` (Pass 140) is the
extraction boundary. It recorded three notes about device acquisition and left
it there. That was correct for a parity document. It is not enough to plan
against, because the parity work answered *"can devourer do what kernel-monitor
does"* and the extraction question is a different one: *"what does a consumer
that is not `app/main.cpp` need in order to be a waybeam node?"*

The answer is not mostly about devourer. It is mostly about the fact that
**there is no layer that is a waybeam node.** `core/` is protocol logic, `io/`
is devices and config, and everything that composes them into a node lives in
one 7540-line `app/main.cpp`. That is the finding this document exists to state.

## 0. What the consumers actually need

Two consumers are named: `Waybeam-android` (replacing the raw-devourer
integration in its `:wifi` module) and OpenWRT-class routers.

**Waybeam-android's target node is a ground receiver.** Its current `:wifi`
module is a *passive leech*: `WifiRadioNative` opens N adapters by fd, locks a
channel, and decapsulates unencrypted IPv4/UDP off the air
(`nativeStartLeech`), plus a channel survey and an inspect view. It speaks no
waybeam protocol at all. Replacing it with waybeam-link means the phone becomes
a real node: §3.0 decapsulation, per-adapter RX diversity by packet-seq dedup,
FEC recovery, §8 NACK/ARQ returns, §9 LINK_REPORT, §11 CSA follow, and the
§15.5a scout/quickconnect path. That is `run_rx`, not `RadioAir`.

**In config terms Android is `rx-spectator` or `tx-ground`** (see
`docs/config-harness-plan.md` §1) — and which of the two it is turns on an open
ruling, see B2 below.

**OpenWRT is the easier consumer** and mostly needs the same thing the SSC338Q
cross already proves, plus a musl build and an install/export target. It is not
the one driving the design.

## 1. Three layers, three very different amounts of work

### L1 — `core/`: ready today

Verified, not assumed: `core/src` and `core/include` contain **no** reference to
`unistd.h`, `sys/socket`, `pthread`, `<thread>`, `<mutex>`, `stderr`,
`fprintf`, or `nlohmann`. Time is injected. 32-bit cleanliness is not a claim
but a build gate — `cmake --preset ssc338q` is ARMv7 and builds `wblink_core`
warning-free.

What is missing is packaging, not portability: an install/export target, an
umbrella header, and a version. That is hours, not weeks.

### L2 — `io/`: the boundary exists, the device model does not

`AirIface` (Pass 140, `io/include/wblink/air_iface.h`) is genuinely the right
seam and it is all-pure-virtual by deliberate design, so a new backend or a new
capability is visible at every implementer rather than silently falling through.
`load_config_json()` and `load_profile_table_json()` both already take strings
(`io/src/config.cpp:113`, `:1110`), so a consumer with no filesystem is already
served — that is a real head start and it was not obvious before checking.

What blocks a consumer is enumerated in §2. None of it is deep.

### L3 — node behaviour: not a library at all

This is the whole cost. `app/main.cpp` holds `AirBackend` (the `AirIface`
owner and §15.3 counter fill), `ScoutEngine`, `DiscoveryCatalog`,
`PacketEventTrace`, `ArqTimingTracker`, `UplinkPower`, `TxCore`, `RxCore`,
the stats emitter, the info/health JSON builders, and the three mode loops.
`run_rx` alone is ~2360 lines.

Two things soften this:

- **`RxCore` is already clean.** It is constructed from `Config` + session +
  table, and `RxCore::on_air` takes an injected `now` and deliver callbacks. It
  is a library class living in the wrong file, not a tangle.
- **`AirBackend` is already down to one `unique_ptr<AirIface>`** (Pass 140).
  The only per-backend dispatch left in it is the §15.3 counters fill, which
  the parity plan set aside on purpose as a schema question.

What is genuinely entangled is `run_rx`/`run_tx` themselves: uplink power
resolve, calibration store, control server, venc actuation, scout sweep,
CSA campaign, cache following, and the stats cadence are interleaved with the
poll loop.

## 2. The blocking list

Ordered by how much they constrain the design, not by size.

### B2 (first, because it is a ruling) — **OPEN: `RadioAir` cannot be RX-only**

`RadioAir::create` hard-fails unless exactly one adapter is `role:"tx"`
(`io/src/air_radio.cpp:394-403`). `MonAir` carries a `has_tx` flag and tolerates
zero, which is what the `.199` spectator flies on.

An Android phone with N receive dongles and no uplink is exactly the
`rx-spectator` × `radio` cell that `docs/config-harness-plan.md` §2a flags as
not constructible today. **This is the single hardest blocker and it is not
code** — it is `#106` ordered item 4: does a devourer spectator nominate a
silent `role:"tx"` adapter, or does `RadioAir` grow the tolerance?

It cannot be deferred past the split, because it decides whether the library can
express the Android node *at all*.

Note the second-order effect either way: `RadioAir` uses its TX adapter for
`set_tx_mode`, `read_tsf` (§7.2 quiet-gap anchor), `tx_index()` (§15.5a scout)
and `reapply_tx_power`. A tolerant `RadioAir` has to answer all four for a node
with no transmitter, and `AirIface` is where those answers get written down.

### B3 — **OPEN: backend mix (#106 item 3)**

`air.kind` is node-level (`AirCfg::Kind`, `io/include/wblink/config.h:359`).
Per-node-only is today's model and is probably right for Android too, but the
ruling decides the library's device model, so it wants to be settled before a
consumer copies the shape.

### B1 — device acquisition: enumeration vs. a wrapped fd

`RadioAir::create` does `libusb_init` → `libusb_get_device_list` →
`libusb_open`, matching VID/PID and an `lsusb -t`-style bus path
(`io/src/air_radio.cpp:459-487`). **Unrooted Android cannot enumerate usbfs.**

The good news is that the replacement is already proven in the consumer:
`Waybeam-android/wifi/src/main/cpp/wifi_jni.cpp` `build_device()` does
`libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY)` →
`libusb_init` → `libusb_wrap_sys_device(fd)` →
`devourer::claim_interface_then_reset(..., do_reset=false, lock_dir)` →
`CreateRtlDevice`. That is the exact sequence `RadioAir` needs as an alternate
construction path.

Shape: `AdapterCfg`/`RadioAirCfg` needs a device *source* — bus path (today) or
a pre-opened fd — rather than a bus path string as the only way to name a
device. Not a large change; it is on the critical path because everything else
on Android sits behind it.

### B4 — `do_reset` and the reopen path under a wrapped fd

`claim_interface_reset_reopen` recovers from reset re-enumeration by re-finding
the **same bus + port path** (`third_party/devourer/src/UsbOpen.h`). Under a
wrapped fd there is no bus path and the reset orphans the app's fd — which is
why the Android JNI passes `do_reset=false` with that exact comment.

So `do_reset` must become a construction parameter, and the consequence has to
be written down rather than discovered: **a devourer adapter opened by fd cannot
be reset.** That interacts with two shipped features:

- §11.6 Pass 80 `recover()` — the one-shot full re-init after a wedge.
- Pass 148 (`5ef44f7`) — the TX node *exits the process* on a sustained wedge so
  a supervisor re-execs it. There is no supervisor on Android, and a library
  must not call `exit()`. See B9.

`CLAUDE.md` already records that RTL88x2 USB wedges need a physical re-plug, not
a driver reload. On Android the honest recovery is "ask the Java layer to close
and re-request the fd" — which is a capability the library has to expose, not
something it can do itself.

### B5 — `lock_dir` is not plumbed

devourer parameterises the advisory lock directory already; `RadioAirCfg` does
not carry it, so `RadioAir` takes the `/tmp` default. Android has no `/tmp`
(the JNI passes the app cache dir). Small and mechanical.

### B6 — compile-time feature options

`wblink_io` unconditionally compiles four things a phone cannot or should not
have. Only `WBLINK_RADIO` exists as an option today.

| source | why it must become optional |
|---|---|
| `io/src/air_mon.cpp` | AF_PACKET + `fork`/`execvp("iw")`. Needs root; useless on Android. Pulls `linux/if_packet.h`, `linux/filter.h`, `sys/wait.h`. |
| `io/src/frame_shm.cpp` | `shm_open` — bionic does not provide POSIX shared memory. Expected to be a **link** failure on Android, not a runtime one; the `android-arm64` preset in phase 1a is what proves it rather than this table. |
| `io/src/control_server.cpp` | An HTTP listener a phone app does not want by default. |
| `io/src/venc_http.cpp` | TX/vehicle-side actuation; dead weight on a receiver. |

Proposed: `WBLINK_MON`, `WBLINK_FRAME_SHM`, `WBLINK_CONTROL_SERVER`,
`WBLINK_VENC`, all defaulting `ON` so no existing preset changes behaviour.

### B7 — build-system duplication is the extraction debt, made visible

`Waybeam-android/wifi/src/main/cpp/CMakeLists.txt` has **hand-copied** the
`pkgconf-libusb.sh` shim, the libusb-cmake `BUILD_SHARED_LIBS`/`LIBUSB_*`
force-block, the devourer chip-family selection, and the "devourer before
usb-1.0, static archive order" link rule — all of which exist in
`waybeam-link/CMakeLists.txt` with the reasoning attached. Two vendorings of
devourer now exist at two commits and will drift.

The library must export that wiring as one consumable CMake unit so the consumer
deletes its copy rather than maintaining a second one.

### B8 — log and diagnostic sinks

`io/src` writes to `stderr` in 24 places (12 of them in `air_radio.cpp`), and
`RadioAir` builds a devourer `Logger` whose event/diag streams are
`fopencookie` handles. `air_radio.cpp:6` already calls it a "glibc/musl
extension"; bionic is expected to carry it from API 23 (`:wifi` is minSdk 26),
so it should not be a blocker — but that is an expectation, and the phase-1a
preset is what settles it.

The blocker that is *not* in doubt: a library must not own the consumer's
stderr. Needs an injectable sink (logcat / syslog / caller callback).

**Preserve on the way through:** `csa_psk` must never reach a log or stat
(`CLAUDE.md`). A new sink is a new output path that touches config.

### B9 — the library must not own the process

`main()` installs `SIGPIPE`/`SIGINT`/`SIGTERM` handlers deliberately without
`SA_RESTART` (`app/main.cpp:7518-7530`), `spawn_mode_applier` double-forks, and
Pass 148 exits the process on a sustained wedge. All three are correct for a
daemon and wrong for a library. The wedge behaviour in particular is a
*protocol-adjacent* decision (§9.10 v2), so "what does a library do instead of
exiting" is a real question, not a mechanical one.

### B10 — egress: RX video has no callback sink

`BindKind` is `kUdp | kFrameShm` (`io/include/wblink/config.h:23`). Android
wants frames handed to MediaCodec. UDP-to-localhost works today with zero
library work and is the correct first step; a callback sink is the clean
end-state and is additive.

### B11 — calibration identity has no answer on a wrapped fd

`calib_identity()` returns `ifname/MAC` when `ifname` is set and `bus/<path>`
otherwise. Under a wrapped fd there is **neither** — no netdev name and no bus
path. So §10.6/§10.7 artifacts on Android have no identity to key on.

Pass 145/T7 already flagged bus paths as unstable across a re-plug, and
`d216287` records the EFUSE-MAC identity ask upstream. Android does not make
that ask more urgent for the fleet, but it does make it *unavoidable* for the
library: the wrapped-fd path needs a stable identity or calibration is simply
unavailable there, and "unavailable" needs to read as STALE, not as a silent
apply.

### B12 — arch coverage

Better than expected. `ssc338q` is `arm-openipc-linux-gnueabihf`
(`cmake/toolchain-ssc338q.cmake`) — ARMv7 hard-float, 32-bit — and it builds
`wblink_io` and the full app, so `io/` is already 32-bit-proven, not just
`core/`. What that does **not** cover is the libc: it is glibc. Untested are
**bionic** (any arch) and **musl** (OpenWRT). Android's `:wifi` is `arm64-v8a`
only today, with a comment reserving `armeabi-v7a` for later.

Cheapest gate: an `android-arm64` compile-only preset alongside `ssc338q`, so
the bionic build breaks in CI rather than in the consumer.

## 3. Sequencing

### 3.0 — #92 lands first, and it moves one of Phase 0's two items

[#92](https://github.com/snokvist/waybeam-link/pull/92) is a draft design PR
(docs only, `feature/asymmetric-fec-plan`) whose implementation is the current
work. It spends less FEC on SVC-T non-referenced frames, and per its own plan
the implementing PR carries:

- a `kFrameFlagEnhance` accessor in `core/include/wblink/frame_shm_format.h`
  and a third rate branch at the two `is_idr ? i_rate : p_rate` sites in
  `core/src/frame_framer.cpp`;
- **a new config key** — `streams[].fec.e_rate_permille`, beside the existing
  `i_rate_permille` / `p_rate_permille` / `min_k` / `min_r`
  (`io/src/config.cpp:316-319`);
- **a new §15.3 counter** — `fec_enhance_frames`, in a schema that is
  golden-tested;
- a **§14.1 spec amendment**, committing first with its own numbered Pass.

Two consequences, and only one of them is a real constraint:

1. **Phase 0's config-surface freeze must queue behind #92.** `CLAUDE.md`
   already states that a new config key is a harness-visible change that has to
   be registered in the same commit. Landing #106 item 1 first would register a
   key set that goes stale immediately and churn both the schema golden and the
   §15.3 stats golden twice. This is sequencing, not conflict — #92's
   implementation should simply register `e_rate_permille` if item 1 has
   already merged, and item 1 should include it if it has not.
2. **Phase 1 does not conflict with #92 at all.** 1a (CMake feature options,
   export package, `android-arm64` preset), 1b (device source, `lock_dir`,
   `do_reset`) and 1c (log sink) touch `CMakeLists.txt`, `io/src/air_radio.cpp`
   and `io/include/` — no overlap with `core/src/frame_framer.cpp` or the
   framer's config struct. **Phase 1 can start now, in parallel.**

There is also a small alignment worth noticing rather than acting on: #92 makes
`core/` spend FEC by frame class, which is exactly the kind of policy an Android
receiver inherits for free by vendoring `core/` whole. It costs the extraction
nothing and it is a good argument for the layering as it stands.

### 3.1 Should the open issues land before or after the split?

### Land before — they define the surface the library publishes

- **#106 item 1** (key registration + `--check --strict --json` +
  `config-schema --json`). **The library's config surface is its API.** Freeze
  and publish it before a second consumer starts copying key names from
  examples, because `io/src/config.cpp` silently ignores unrecognised keys and
  a library consumer has strictly less context than an operator does. This is
  also already the smallest item on that issue's list and it is useful the day
  it merges even with no generator.
- **#106 item 4** (devourer spectator ruling) — B2. Blocking, as above.
- **#106 item 3** (backend-mix ruling) — B3.

### Land after — RadioAir-internal upside, boundary does not move

These are the `docs/devourer-integration-analysis.md` upside register. Each is
either inside `RadioAir` or inside a pure unit that already has a test harness,
and none of them changes what a consumer sees:

- **#96** hardware ACK ↔ `tx.retry_limit` — `RadioAirCfg`-internal.
- **#97** LDPC/STBC — TX descriptor flags, `RadioAir`-internal.
- **#98** SNR/EVM into §9.4 — extends `io/include/wblink/radio_decode.h`, which
  Pass 144 created as pure and unit-testable for exactly this. It must be
  absent-safe anyway (`docs/per-mcs-per-ladder-plan.md` §2: "the ladder must not
  depend on it"), so it cannot become a library dependency.
- **#99** §7.2 quiet-gap error budget — measurement plus `core/` quietgap.
- **#101** sequence-derived rate probing — TX-side, and device-verified first by
  its own framing.

### The genuinely awkward pair: #95 and #100

Both are **scout** work, and `ScoutEngine` lives in `app/main.cpp` — the layer
that is not a library. Landing them as filed writes several hundred more lines
into the monolith that then have to move, and Android's quickconnect wants the
scout, so it cannot stay in `app/`.

But #95 is not purely an `app/` change. Its sensor reads are devourer's
`GetRxEnergy(with_nhm=true)` / `GetRxQuality()` — frame-free energy that
`RadioAir` does not surface today. **That means #95 adds a capability method to
`AirIface`**, and `AirIface` is the published boundary. Since every method there
is pure-virtual by design, adding one is a visible break at each implementer —
which is the contract working as intended, but is much cheaper before the
library has external consumers than after.

Recommendation, which is neither "before" nor "after":

1. **Land #95's boundary half before the split**: the `AirIface` frame-free
   energy read (nullopt on kernel-monitor and udp-air, as the mixed-fleet
   checkbox already requires), plus the pure derivation rules in
   `radio_decode.h` with unit tests. No radio needed for the code gate.
2. **Lift `ScoutEngine` out of `app/main.cpp` as part of the split** (§4 phase
   2a below).
3. **Land the rest of #95 (the ranking move in `emptiest()`) and all of #100 on
   the extracted class.** The issue's own warning applies here — filling the
   fields without moving the ranking closes every checkbox and changes nothing —
   so the ranking move must not be separated from the field fill by very long.

That ordering is a judgement call, not a ruling, and it is the one item here
most likely to be overturned.

## 4. Suggested phasing

Each phase is independently landable and each keeps `ctest --preset dev` (46
suites, ASan+UBSan) and `cmake --build --preset ssc338q` green.

**Phase 0 — rulings and the config surface.** #106 items 1, 3, 4. Plus a
decision on B9 (what a library does instead of `exit()` on a §9.10 wedge) and
on B11 (identity under a wrapped fd). Nothing here is extraction work; all of it
constrains extraction work. **Item 1 queues behind #92** (§3.0); the two
rulings do not — they can be taken at any time and cost nothing but a decision.

**Phase 1 — mechanical, no behaviour change.**

- 1a. Feature options B6, the export/install package B7, and an `android-arm64`
  compile-only preset B12. Gate: every existing preset byte-identical.
- 1b. Device-source abstraction B1, `lock_dir` B5, `do_reset` B4. Gate: the
  x86 bench path unchanged; a wrapped-fd path that compiles and is exercised by
  the Android consumer.
- 1c. Log-sink injection B8; `csa_psk` redaction preserved.

**Phase 2 — the actual extraction.**

- 2a. Lift `AirBackend`, `ScoutEngine`, `DiscoveryCatalog`, `TxCore`/`RxCore`
  wiring and the stats emitter out of `app/main.cpp` into a `node/` layer;
  `app/main.cpp` becomes a thin driver over it. `RxCore` first — it is already
  clean. This is the phase that makes "waybeam-link plus some integration glue"
  a true description on Android.
- 2b. Callback egress sink B10.
- 2c. A stable facade. C++ first — Android reaches it through JNI either way,
  and a C ABI is only worth its cost if a non-C++ consumer appears.

**Phase 3 — Android.** Replace the leech in `wifi_jni.cpp` with the library,
delete `:wifi`'s duplicated devourer vendoring and CMake shim, and keep the
survey/inspect JNI surface (it is genuinely useful and has no waybeam-link
equivalent).

## 5. Loose ends worth knowing before starting

- **The §15.3 counters schema** is the last per-backend dispatch in
  `AirBackend`, set aside by Pass 140 because the two backends' counter structs
  differ by real fields. A library that publishes stats has to answer it; a
  daemon could keep deferring.
- **Two devourer vendorings already exist** — `waybeam-link` at `800c3c8` and
  `Waybeam-android/wifi/src/main/cpp/third_party/devourer` at its own commit.
  They will drift until Phase 3 deletes one.
- **License is already settled and is not a blocker.** `Waybeam-android`
  relicensed to GPL-2.0-or-later when it linked devourer, which matches
  waybeam-link's `SPDX-License-Identifier: GPL-2.0-or-later`. Three of that
  repo's docs (`CLAUDE.md`, `README.md`, `USERGUIDE.md`) still say "Autod
  Personal Use License" and contradict its own `LICENSE.md` — a docs fix on the
  Android side, worth doing before anyone reads them as authoritative.
- **`profiles/` by relative cwd** (`CLAUDE.md`) is a smaller problem than it
  looks: `profile_table` is a config key and `load_profile_table_json()` takes a
  string, so a consumer passes the table as text and never touches a path.
- **`app/main.cpp:108` misattributes a fork.** It credits `RadioAir` with
  `execvp("iw", …)` for retunes; that is `MonAir` (`io/src/air_mon.cpp:258-274`).
  `RadioAir` is fork-free, which is the property that matters here.
