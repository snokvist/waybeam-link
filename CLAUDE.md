# waybeam-link

A best-effort, latency-first broadcast video + telemetry link over raw
injection WiFi (RTL8812AU/CU/EU via vendored OpenIPC devourer), with per-adapter
RX diversity as primary redundancy, opt-in importance-gated ARQ, an adaptive
link layer, and a follow-me channel switch. See `README.md` for the full pitch
and current status.

**Registered as a `waybeam-coordination` submodule since 2026-07-30** (coord
PR #126), tracking `main`. That repo owns the cross-repo view: the inventory
card `repos/waybeam-link.md`, the roadmap `roadmaps/waybeam-link.md` (this repo
no longer keeps its own — `ROADMAP.md` is a pointer stub), and the port registry
`protocols/port-registry.md`, where `:8091` / `:8099` / `:9110` and bench-only
`:5801`/`:5810` are recorded. This repo still owns its own spec: `PROTOCOL.md`
is **not** a coordination `protocols/` file, and the over-air wire format is
settled here, not there.

## The law

`PROTOCOL.md` v1 is LAW — but the law has two tiers, and applying tier 1 to
tier-2 material is how Passes 120–152 spent a third of themselves reversing
each other. Decide the tier BEFORE writing anything.

**Tier 1 — contract.** Wire formats, trust/auth machinery, state-machine
behaviour a peer implementation depends on, config semantics. Process
(unchanged):

- A spec gap or ambiguity is an **operator ruling** — STOP and raise it,
  never pick silently. (HMAC hash choice, RTT anchor, SA semantics — every
  one was a ruling, not an inference.)
- **Spec amendments commit FIRST**, as their own commit; implementing code
  follows in the same PR as a separate commit.
- **Every ruling gets a numbered Pass in `docs/review-log.md`** — ≤40 lines:
  verdict, changed `§N.M`, evidence pointer. No addenda; a new fact is a new
  Pass or a finding. Passes 1–152 live in
  `docs/review-log-archive-p001-152.md` and all citations still resolve.

**Tier 2 — findings.** Anything still being measured: loss walls, gates,
dwell counts, seeds, sweep bounds, estimator choices. These are **config
knobs + dated entries in `docs/findings.md`** — never spec text, never a Pass,
until the mechanism settles. Then ONE amendment (tier-1 process) closes the
finding out. Litmus: *"is this a behaviour change I intend to keep?"* A
threshold chosen to make one run pass is not; an understood mechanism is.
Prefer one amendment after understanding over three during discovery. If an
unmerged amendment asserts something later evidence undermines, soften it
before merge instead of stacking a correction on top.

**Unchanged either way:**

- **Never commit to main.** Feature branch (`impl/`, `fix/`, `docs/`) → PR →
  squash-merge, and merge only on the operator's explicit word.
- **Never edit vendored code under `third_party/`** (devourer, libusb-cmake).
  Portability/build issues are fixed from our CMake only (see the
  `pkgconf-libusb.sh` shim and the per-target `WBLINK_WARNINGS` pattern in
  `CMakeLists.txt` — directory-level flags would leak into vendored code).
  UBSan runtime errors inside vendored Jaguar1 `EepromManager`/`HalModule`
  (misaligned u16 loads, invalid enum) are known x86-dev-only noise:
  non-fatal, do not patch.
- **A config is never trusted because it loaded.** `io/src/config.cpp` reads
  every field through `value()`/`contains()` and never enumerates the object,
  so an unrecognised key — a typo, or a name invented from an older example —
  is **silently ignored**. `--check` passing proves the config parses, not that
  it says what you meant. See "Authoring configs" below.
  `waybeam-link config-schema --json` prints the declared key surface
  (`io/src/config_registry.cpp`, 263 keys) so a key can be checked without
  reading the loader. The registry is not a second source of truth:
  `tests/config_registry_test.py` reconstructs every accessor site in
  `config.cpp` and fails the build if the two disagree in either direction.
  `--check --strict` classifies the keys in a config against it: **unknown**
  (not read by the loader) and **inert** (registered, but this node's mode does
  not read it — §15.2 withholds `policy.return.*` from a node with no
  `role:"tx"` adapter, for one). Both are warnings; exit stays 0 until #106
  item 8. `--json` gives the same report machine-readably.

## Verification playbook

These practices repeatedly caught bugs that green test suites and four clean
cross-builds sailed past. They are the half of the process that earns its
cost — do not skip them to save time.

- **Review by executing probes, not by reading.** Pre-merge, run the actual
  command/trace against the claim (Passes 126/136/150: six CRITICAL
  half-converted paths found this way). A test that injects the very value
  production computes wrongly cannot catch that class of bug.
- **Read deployed state before trusting a design.** The fleet's real configs
  (`deploy/*.json`) and real hardware identities, not examples. One `cat` of
  sysfs serials would have voided the Pass-146 identity design (all dongles
  report `123456`); reading the PSK-less fleet config would have voided the
  §3.16 MAC (Pass 127).
- **Device A/Bs widen their readout.** When an A/B says "nothing happened",
  dump the whole object before concluding (Pass 144: the "latch risk" was a
  stranger's video decoded onto our stream output).
- **Refuse false success.** A result that cannot be used must not report
  success (artifact-write-failed, flat-at-ceiling). Every such refusal later
  fired correctly on hardware.
- **Sweep from the safe end** on any live-link parameter (power, MCS, MTU,
  FEC, channel), and sweep the FULL commanded range — terminating at the
  first wall placed a rung 14 dB wrong (Pass 133).
- **Two adapters of the same part number are not a replicate** (Pass 139:
  the "broken chip" behind the whole devourer-vs-kernel-monitor posture was
  one bad unit). Per-unit, not per-chip.
- Method lessons like these graduate to the coordination repo's memory, not
  into the review log.

## Operating loop

Current phase order (operator direction, 2026-08-06): **consolidate → expand
→ distill.**

1. **Consolidate:** settle §10.5 offset-space actuation (PR #114), calibration
   v2 (`docs/calibration-v2-symmetric-probes.md` — design → ruling → impl),
   minimal strict config check (#106 first slice).
2. **Expand:** devourer upside tranche #95–#101, in the order filed.
   Since Pass 164 devourer is the ONLY air backend — kernel-monitor is
   deleted, and RX/spectator nodes run on it via `allow_rx_only` (Pass 162).
3. **Distill:** library extraction (#109) — `core/` plus a real "waybeam
   node" layer out of `app/main.cpp`; the calibration-v2 engine should land
   already library-shaped (pure core, injected send/tally callbacks).

## Authoring configs

Node configs are dense (263 registered keys, `config-schema --json`) and
**coupled across nodes**: the
cache's `store.controller.endpoint` must equal the owning ground's
`cache.repair.listen`, the ground's `cache.repair.caches[]` must name the
cache's `store.listen` and `originator`, every receiver's
`preferred_originator` must equal the craft's `originator`, and `net_id`,
channel/bw and `policy.csa.{home_chan,channel_allowlist}` must agree
fleet-wide. Nothing checks any of that today — `deploy/README.md` says so in
prose. Until the harness in `docs/config-harness-plan.md` exists:

- **Verify every key against `io/src/config.cpp`, not against an example.**
  Examples and deployed configs can predate a rename; the loader cannot. A key
  the loader does not read is a silently dead line.
  `waybeam-link config-schema --json | grep <key>` answers this faster and is
  build-checked against the loader, so it cannot go stale the way an example
  can. It answers "does this key exist", not "is it live on THIS node" — for
  that run `--check --strict`, which reports inert keys with the reason.
- `_`-prefixed keys are comments by convention (`_comment`,
  `_verify_timeout_ms`) and are the only keys allowed to be unknown.
- Validate with `build/dev/waybeam-link <tx|rx> -c <cfg> --check --strict`,
  then read the config dump — and remember `policy.csa.psk` must print
  redacted. `--strict` adds the unknown/inert key report; without it a typo
  still loads clean.
- `deploy/*.json` are the flying nodes that could be read from live hardware
  and the reference for what a real config looks like; `examples/*.json` are
  samples, not deployments. `.199` and `.247` are missing on purpose — see
  `deploy/README.md`.
- The archetype a config belongs to is not its `node.role`: a ground with an
  uplink is `node.role:"rx"` with one `role:"tx"` adapter, and a spectator
  differs from it by one boolean. See `docs/config-harness-plan.md` §1.
- **A new config key is a harness-visible change.** When you add one, say so in
  the PR — under the plan it also has to be registered so the schema, the
  strict check and the generator learn about it in the same commit.

## Build & test

```
scripts/gates.sh            # EVERY merge gate; what CI runs. --quick = dev + ctest only
```

Run that rather than a remembered checklist. It covers the eight presets, the
71-suite `ctest`, the B7 embed check, the B10 node link check, the install/`find_package` round trip and
the `deploy/*.json` `--check`s, and it **fails on a diagnostic for our
targets even when the build exits 0**. Toolchains the host lacks are SKIPPED
loudly and counted separately — a skip is never a pass. Export
`WBLINK_SSC338Q_TOOLCHAIN` and `WBLINK_ANDROID_NDK` to get the cross presets.
`.github/workflows/gates.yml` calls the same script, so local and CI cannot
drift.

The individual commands, when you want one of them:

```
cmake --build --preset dev && ctest --preset dev   # 71 suites, ASan+UBSan
cmake --build --preset ssc338q                      # ARMv7 cross (SigmaStar target)
```

Additional flavors (chip families via `WBLINK_DEVOURER_CHIPS` = fleet | au |
eu | all): `x86-ground` and `rk3566` (aarch64 ground cross) build with
**all** devourer USB families for ground-side adapter tests; `ssc338q-au` /
`ssc338q-eu` are single-chip vehicle variants (smallest binary for the
~5.7 MB overlay). The default `fleet` trio is 8812AU + 8812CU + 8812EU.

`cmake --build --preset android-arm64` is the **bionic** compile-only gate
(libraries only — no app, no tests). Two of its cache variables are
load-bearing, not tidiness: `WBLINK_RADIO=ON` because `io/src/air_radio.cpp`
is the only TU that hands devourer a cookie stream, so turning the radio off
stops exercising what the shim exists *for* (since #146 `io/src/log.cpp`
includes `cookie_stream.h` unconditionally, so the shim itself compiles in
every config — the radio is what proves it against a real consumer); and
`WBLINK_WERROR=ON` because without it the preset merely *prints* a portability
diagnostic into a log dominated by vendored devourer/libusb warnings — a
human-attention gate, not a build gate. It needs an NDK:
`-DWBLINK_ANDROID_NDK=<root>` or env `WBLINK_ANDROID_NDK` /
`ANDROID_NDK_HOME` / `ANDROID_NDK_ROOT`; API 26 + arm64-v8a match
Waybeam-android's `:wifi`. It exists because `ssc338q` proves ARMv7 and
32-bit but is glibc — bionic has neither `fopencookie` (see
`io/include/wblink/cookie_stream.h`) nor `shm_open`, and `nfds_t` is
narrower there, all of which this preset catches at build time rather than
in the consumer.

`build/dev/hwtrial_bringup` is the **hardware trial harness** (issue #140).
It brings adapters up from either device source — `--auto`/`--bus <path>` for
the enumerated path, `--fd <bus>/<dev>` for the wrapped-fd path — prints each
unit's EFUSE identity and RX counters, and tears down. **It never transmits**
(adapters are created `allow_rx_only`), which is what makes it safe to run
without an RF session. The fd mode needs usbfs write access, so run it under
`sudo` with the kernel drivers unloaded (`CLAUDE.md` bench notes), and restore
them afterwards. It is the only way to exercise the Android-shaped path
without an Android device.

`cmake -S examples/embed-consumer -B build/embed && cmake --build build/embed`
is the **embedding gate**. It is the smallest project that consumes this tree
by `add_subdirectory` and links `wblink::io`, and it asserts at configure time
that embedding does not build **our** daemon's world, does not write
`BUILD_SHARED_LIBS` into the consumer's cache, and does not hijack the
consumer's `PKG_CONFIG_EXECUTABLE`. All three regressed before it existed.
**Anything added at the top of `CMakeLists.txt` with `CACHE ... FORCE` will
break it** — use a normal variable, which shadows the cache for this directory
and below without writing to the consumer's.

Two limits worth knowing rather than discovering. Embedding still leaks ~70
`EXCLUDE_FROM_ALL` targets from the vendored trees (devourer's example
executables, libusb's selftests); nothing extra is *built*, but a consumer
with its own `doctor` or `sense` target hits a target-name collision, and we
cannot gate that without editing `third_party/`. And a consumer's explicit
`-DDEVOURER_*` or `-DPKG_CONFIG_EXECUTABLE` is ignored inside our subtree —
`WBLINK_DEVOURER_CHIPS` is the supported knob. (`FORCE` overrode them too,
more destructively; do not "fix" this with `if(NOT DEFINED ...)`, which would
let a stale cache entry win.)

Do not `cmake --install` a `dev` build: it ships an ASan-instrumented
`libwblink_core.a` whose exported target carries no `-fsanitize` usage
requirement, so the consumer's link fails on `__asan_*`. Install from
`release`.

Two consumption shapes, deliberately not symmetric:

| shape | gets | why |
|---|---|---|
| `find_package(wblink)` | `wblink::core` | the dependency-free wire library, installable on its own |
| `add_subdirectory(...)` | `wblink::core`, `wblink::io` **and** `wblink::node` | `wblink_io` PUBLIC-links vendored `devourer`/`usb-1.0`, which have no install rules of their own, so it cannot be exported without authoring install rules for `third_party/` |

Use the namespaced aliases (`wblink::core`, `wblink::io`, `wblink::node`)
in anything new;
the bare names exist for this file's own targets. `wblink_core` carries
`cxx_std_20` as a PUBLIC usage requirement — `CMAKE_CXX_STANDARD` is
directory-scoped and does not travel with an exported target.

The library feature options — `WBLINK_FRAME_SHM`, `WBLINK_CONTROL_SERVER`,
`WBLINK_VENC`, `WBLINK_BUILD_APP` — all default **ON**, so every flying
build is unaffected. They exist for a consumer that links `wblink_io`
without the daemon. `WBLINK_BUILD_APP=ON` requires all three subsystems
(`app/main.cpp` uses each unconditionally) and refuses at configure time
otherwise.

`dev` is the gate for every PR. `ssc338q` is compile-only verification (no
sanitizers, no run) but must stay green and warning-free for **our** targets
(`wblink_core`, `wblink_io`, `waybeam-link`) — vendored subdirectories build
under their own flags and are not held to `WBLINK_WARNINGS`.

clangd/LSP diagnostics on edited files are stale-compile-DB noise. The build
is the gate, not the IDE — don't chase a squiggle the build doesn't reproduce.

## Layout

- `core/` — pure protocol logic: wire codec, table hashing, RX engine, ring,
  scheduler, quiet-gap pacer, adaptive selector, CSA. No sockets/threads/wall
  clocks; time is injected. Zero dependencies beyond the C++ stdlib — this is
  the piece vendored whole into consumers (Android `:wifi`), so it must stay
  32-bit-clean and dependency-free.
- `io/` — config (JSON), UDP bindings, stats NDJSON writer, dot11 encapsulation,
  the devourer `RadioAir` backend, venc HTTP actuation, power-file loader.
  **Diagnostics go through `wb_logf()` (`io/include/wblink/log.h`), never
  `fprintf(stderr)` directly**, and the §15.3 line goes through
  `StatsEmitter`'s local sink, never `stdout` directly — both default to the
  old behaviour, and both exist so an embedding consumer is not deaf (#144).
- `node/` — node behaviour above `io/` (#109 Phase 2a/2c). STATIC since the RX
  run loop moved in: `src/rx_node.cpp` holds `run_rx`, and a consumer that
  links `wblink::node` can now RUN a receiving node rather than only build its
  objects. It links with `WBLINK_FRAME_SHM=OFF` / `WBLINK_CONTROL_SERVER=OFF`
  since B10 added the `FrameSink` callback egress
  (`docs/library-extraction-plan.md` §4.10). The gate for that is
  `examples/node-linkcheck` and it is a **LINK**, not a build:
  `android-arm64` is compile-only and a static archive does not resolve its
  own undefined symbols, so that preset was green while `libwblink_node.a`
  carried nine unresolvable references. The headers:
  `rx_node.h` (`run_rx` — the run loop, implemented in `src/rx_node.cpp`),
  `rx_core.h` (`RxCore` + `rx_policy()`), `discovery.h` (`DiscoveryCatalog`,
  `ScoutEngine`), `air_backend.h` (`AirBackend`, `PacketEventTrace`),
  `tx_core.h` (`TxCore` + the §15.2->core policy adapters), `stats_fill.h`
  (`emit_stats`, `ArqTimingTracker`, the §15.5 `/info` + `/health` payloads),
  `uplink_power.h` (`UplinkPower` — the §10.3/§10.5/§10.7/§11.7 0x0A
  precedence chain), `policy.h` (`csa_params`, `vcmd_params`,
  `quietgap_policy`, `channel_allowed`), `vcmd.h` (§15.5 REST names ↔ §11.7
  ids), `frame_kind.h` (§7.2 frame predicates), `entropy.h` (the two
  /dev/urandom reads), `clock.h` (`now_ms`/`now_us`) and
  `aim.h` (§7.2 histograms). **`node/` owns no process** — no `exit`, no
  `fork`, no signal handling; those stay in `app/main.cpp`.
  `tests/node_layering_test.py` enforces **three** rules and fails the build on
  any of them: no process-owning call in `node/`; no `node/` include from
  `core/` or `io/`; and **no namespace-scope definition in a `node/` header
  without `inline`** — the third because eight of them shipped in Phase 2a and
  nothing could catch it while `app/main.cpp` was the only includer. It carries
  its own self-test, so a matcher that stops matching fails rather than passes.
  **Layering rule:
  `node/` may use `core/` and `io/`; neither may use `node/`.** Anything
  moved here becomes reachable from a real unit test — before the layer
  existed the only way to touch `RxCore` was `tests/app_test.cpp`
  `#include`-ing the whole of `app/main.cpp`, which is why several Pass
  165-167 defects could only be proven on hardware.
- `app/main.cpp` — the driver, **1.7k lines** after Phase 2a/2c (was 8.8k).
  The RX loop lives in `node/src/rx_node.cpp` now; what remains here is
  `run_tx`, `run_loopback`, argument handling, and the process-owning half —
  signal handlers, `spawn_mode_applier`'s double fork, the §9.10 wedge exit.
- `tests/` — one `_test.cpp` per unit, run via ctest.
- `tools/` — bench analyzers: `gate2_rho.py` (cross-adapter loss correlation),
  `gate3_rtt.py` (NACK→RETRANSMIT latency), `rtp_feed.py` (synthetic RTP feeder).
- `profiles/` — the §9.3 operating-point table (data, not code).
- `examples/` — sample configs (loopback, udp-air tx/rx, radio tx/rx,
  frame-shm tx/rx).
- `docs/` — `build-order.md` (§19 order + §17 gates), `review-log.md` (live
  Tier-1 ruling log, Pass 153+), `review-log-archive-p001-152.md` (frozen
  Pass 1–152 history), `findings.md` (Tier-2 measurement notes),
  `groundwork.md` (constant provenance), `findings-pass3.md`
  (adversarial-review arbitration), `step11-bench.md` (current bench state +
  remaining-work plan), `config-harness-plan.md` (the config generator /
  agent-facing schema proposal, with two open rulings),
  `calibration-v2-symmetric-probes.md` (the calibration redesign, Tier 2
  until ruled).

## Runtime / bench gotchas

Each of these cost real debugging time during bring-up — don't rediscover them.

- The binary needs the repo root as cwd (loads `profiles/` by relative path).
- Stats are NDJSON on stdout at `stats.hz` (schema = PROTOCOL.md §15.3,
  golden-tested — don't hand-edit the schema without updating the golden file).
- A TX node sends no **video/return** traffic without an RTP feed on its input
  binding, so drive one with `tools/rtp_feed.py`. (It is not fully idle: the
  1 Hz HEARTBEAT and 2 Hz ANNOUNCE fire regardless — but the Pass 70 issuer
  video-confirm still needs a real feed.)
- `RadioAir` requires **exactly one** `role:"tx"` adapter per process (it's
  duplex — that adapter also RXes). Monitor RX delivers the MPDU with the
  chip-validated 4-byte FCS appended — already stripped/handled in `RadioAir`;
  remember it when reading raw pcaps.
- Kill bench processes only from a script file:
  `pkill -TERM -f 'build/dev/waybeam-link'` typed directly in an interactive
  shell matches its own cmdline too and kills the shell (exit 144). SIGTERM
  only, never SIGKILL.
- `csa_psk` is secret — must never appear in stats/logs (config dump prints
  `"(set, redacted)"`). Preserve that property in any new output path that
  touches config.
- x86 bench rig: 8812AU on USB bus `6-1`, 8812CUs on `1-1.1` (craft) /
  `1-1.2` (ground diversity). Bus paths **shuffle** after any re-plug —
  re-check with `lsusb -t` before assuming a path is stable. Unload kernel
  drivers before runs: `sudo rmmod 88x2cu rtw88_8812au`. The machine's own
  MediaTek WiFi (`wlp2s0`) is untouched. RTL88x2 USB wedges (RX counter
  frozen) need a physical re-plug, not a driver reload.
- Bench-only knob `air.rx_drop_permille` (0–1000): per-adapter independent
  synthetic RX drop, used to manufacture known-independent loss for gate-2
  validation.
- A calibration run needs **no video displayed** — bench it headless (the
  v2 design pauses the video feed entirely for the run).

## Working style

- Prefer minimal patches; match existing comment density and the
  spec-section-reference style (comments cite `§N.M`, not prose paraphrase).
- Current open work and PR stack: see `docs/step11-bench.md` and the
  "Operating loop" above.
