# waybeam-link

A best-effort, latency-first broadcast video + telemetry link over monitor/
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

- **`PROTOCOL.md` v1 is LAW.** When the spec has a gap or ambiguity, STOP and
  raise it to the operator — never pick silently. This has happened repeatedly
  (HMAC hash choice, RTT estimator anchor, SA field semantics) and every one of
  those was an operator ruling, not an inference.
- **Spec amendments commit FIRST**, as their own commit; code implementing the
  ruling follows in the same PR, as a separate commit.
- **Every spec ruling gets a numbered Pass entry in `docs/review-log.md`.**
  Currently at Pass 147 — read the last two or three passes before touching
  anything spec-adjacent, to pick up the reasoning, not just the verdict.
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

## Authoring configs

Node configs are dense (226 parsed keys) and **coupled across nodes**: the
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
- `_`-prefixed keys are comments by convention (`_comment`,
  `_verify_timeout_ms`) and are the only keys allowed to be unknown.
- Validate with `build/dev/waybeam-link <tx|rx> -c <cfg> --check`, then read
  the config dump — and remember `policy.csa.psk` must print redacted.
- `deploy/*.json` are the four flying nodes and the reference for what a real
  config looks like; `examples/*.json` are samples, not deployments.
- The archetype a config belongs to is not its `node.role`: a ground with an
  uplink is `node.role:"rx"` with one `role:"tx"` adapter, and a spectator
  differs from it by one boolean. See `docs/config-harness-plan.md` §1.
- **A new config key is a harness-visible change.** When you add one, say so in
  the PR — under the plan it also has to be registered so the schema, the
  strict check and the generator learn about it in the same commit.

## Build & test

```
cmake --build --preset dev && ctest --preset dev   # merge gate: 46 suites, ASan+UBSan
cmake --build --preset ssc338q                      # ARMv7 cross (SigmaStar target)
```

Additional flavors (chip families via `WBLINK_DEVOURER_CHIPS` = fleet | au |
eu | all): `x86-ground` and `rk3566` (aarch64 ground cross) build with
**all** devourer USB families for ground-side adapter tests; `ssc338q-au` /
`ssc338q-eu` are single-chip vehicle variants (smallest binary for the
~5.7 MB overlay). The default `fleet` trio is 8812AU + 8812CU + 8812EU.

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
- `app/main.cpp` — event loops (`tx`/`rx`/`loopback` modes).
- `tests/` — one `_test.cpp` per unit, run via ctest.
- `tools/` — bench analyzers: `gate2_rho.py` (cross-adapter loss correlation),
  `gate3_rtt.py` (NACK→RETRANSMIT latency), `rtp_feed.py` (synthetic RTP feeder).
- `profiles/` — the §9.3 operating-point table (data, not code).
- `examples/` — sample configs (loopback, udp-air tx/rx, radio tx/rx).
- `docs/` — `build-order.md` (§19 order + §17 gates), `review-log.md` (spec
  ruling history), `groundwork.md` (constant provenance), `findings-pass3.md`
  (adversarial-review arbitration), `step11-bench.md` (current bench state +
  remaining-work plan), `config-harness-plan.md` (the config generator /
  agent-facing schema proposal, with two open rulings).

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

## Working style

- Prefer minimal patches; match existing comment density and the
  spec-section-reference style (comments cite `§N.M`, not prose paraphrase).
- Current open work and PR stack: see `docs/step11-bench.md`.
