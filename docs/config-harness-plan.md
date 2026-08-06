# Config creation harness — proposal

**Status: proposal, not a ruling.** Two items below need an operator decision
before any code is written (marked **OPEN**). Everything else follows from what
`io/src/config.cpp`, `io/src/air_radio.cpp` and `deploy/` already do.

The problem is not that a config is long. It is that a config is long **and
coupled**: `deploy/README.md` already warns that "changing either requires
updating both deployments", and the four files in `deploy/` agree with each
other on a dozen values no single-file editor can check. A generator that emits
one node in isolation buys very little; a generator that owns the fleet-wide
values buys the whole thing.

## 1. The four archetypes, in config terms

The operator names and the config shape do **not** line up, which is exactly
where hand-editing goes wrong. Derived from `deploy/`:

| Archetype | `node.role` | `node.spectator` | adapters | streams | distinguishing blocks |
|---|---|---|---|---|---|
| `tx-vehicle` | `tx` | — | exactly one, `role:"tx"` | `dir:"in"` (frame-shm or udp) | `venc`, `policy.select`, per-stream `fec`/`arq_mode` |
| `tx-ground` | **`rx`** | false | one `role:"tx"` uplink + N `role:"rx"` ears | `dir:"out"` | `scout`, optional `cache.repair` |
| `rx-spectator` | `rx` | **true** | all `role:"rx"` | `dir:"out"` | no uplink at all (§3.8 heartbeat suppression) |
| `rx-cache` | `rx` | false | all `role:"rx"` | **empty** | `cache.store` (+ `controller`, `status_to`) |

`tx-ground` is a `role:"rx"` node. `rx-cache` has zero streams and no
`profile_table`. `rx-spectator` differs from `tx-ground` by one boolean that
silences the transmitter (Pass 48/49/74). Those three facts are the harness's
entire reason to exist — the operator picks a word, the tool picks the shape.

Invariants the tool must enforce, all of which are already load-time or
runtime-time errors today and all of which are cheaper to catch at authoring
time:

- `RadioAir` requires **exactly one** `role:"tx"` adapter per process.
- `node.spectator` requires `node.role:"rx"` (`config.cpp:130`).
- `power_map` / `max_power_qdb` / `power_presets_qdb` on a `role:"rx"` adapter
  are rejected (`config.cpp:182`, `:194`).
- `power_presets_qdb` ≤ 5 entries (§11.7 preset index bound).
- Cross-node: cache `store.controller.endpoint` **must** equal the owning
  ground's `cache.repair.listen`; the ground's `cache.repair.caches[].endpoint`
  and `.originator` must equal the cache's `store.listen` and `node.originator`;
  every receiver's `preferred_originator` must equal the craft's `originator`;
  `net_id`, `channel`, `bw`, `csa.home_chan` and `csa.channel_allowlist` must
  agree fleet-wide. Nothing checks any of this today.

## 2. **OPEN — a node has one backend, not a mix**

`air.kind` is a single node-level enum (`AirCfg::Kind`, `config.h:351`). There
is no per-adapter backend field. The `.247` cache runs an MT7921 *and* an
RTL8812AU, but both under `kernel-monitor` — that is a mix of **chips**, not of
**backends**.

So "rx-spectator / rx-cache may use devourer or monitor **or a mix**" is, as
stated, not representable. Devourer and kernel-monitor are also mutually
exclusive per *device*: a Realtek dongle is either bound to a kernel driver (and
visible as a netdev) or claimed by devourer over libusb, never both — the bench
notes say `rmmod 88x2cu rtw88_8812au` for exactly this reason.

Two of these are cheap, one is not:

1. **Per-node choice only** (today's model). The harness offers `radio` or
   `kernel-monitor` for the RX archetypes and refuses a mix. No spec change.
2. **Per-adapter `backend` key**, one process driving both an AF_PACKET ear and
   a libusb ear. Mechanically plausible — `RadioAir` and `MonAir` are separate
   `AirIface` implementations — but it needs a composite air backend, a ruling
   on how `air.rx_drop_permille` / `§14.2 airtime` / TSF anchoring apply across
   two clock sources, and it touches the RX diversity path.
3. **Two processes, one per backend**, joined at the cache/repair layer. No new
   spec, but two originators and two stat streams for one physical node.

**Question for the operator: which of these does the harness target?** The plan
below assumes (1) and is written so (2) is an additive change, not a rewrite.

## 3. Where the tool lives — a mode of `waybeam-link`, not a new binary

`waybeam-link discover` and `waybeam-link config`, alongside `tx|rx|loopback`
in `app/main.cpp`.

Rationale, in order of weight:

- **Discovery must run where the adapters are.** The craft is a SigmaStar
  overlay with busybox and no Python; the only thing guaranteed to be on it is
  the binary you just deployed.
- **Zero new dependencies.** libusb, nlohmann/json and the config loader are
  already linked into that binary. A separate `waybeam-config` target would
  re-link all of it for a second ~MB artifact on a 5.7 MB overlay.
- **It cannot drift from the loader.** The generator's output is validated by
  calling the real `load_config()` + `BindingSet::create()` in-process — the
  same path as `--check` — and refusing to write on failure. A Python generator
  would re-implement the schema and rot, the way
  `tools/hw/uplink_artifact_sign.py` is *explicitly* a test-only mirror of
  `io/src/uplink_calib_store.cpp`.
- If the added text is unacceptable for `ssc338q-au` / `ssc338q-eu`, gate it
  with `WBLINK_BUILD_CONFIGTOOL` (default ON, OFF for those two presets) rather
  than moving it out of the binary.

### No ncurses

Plain line-oriented prompts: numbered menus, defaults in `[brackets]`, `?` for
help, re-ask on bad input. Not a TUI, because:

- ncurses on the craft means a cross-built library **and terminfo in the
  rootfs**, which busybox overlays generally do not carry.
- The craft is frequently reached over a serial console and over `ssh -T`,
  where a full-screen TUI is at best unpleasant.
- A line protocol is scriptable. `--answers answers.json` (or a here-doc on
  stdin) makes the wizard testable under ctest with no TTY, which is what keeps
  it honest — see §6.

## 4. `waybeam-link discover`

Non-invasive inventory, `--json` for machine use, human table by default.

**Devourer candidates** (`WBLINK_RADIO` builds): libusb enumeration of VID
`0x0bda`, reusing `usb_path_of()` from `air_radio.cpp:52` — lifted into a small
header so the bus path the harness writes is byte-identical to the one
`RadioAir` matches on. Per device: bus path, idProduct, chip family, and
whether a kernel driver is currently bound (→ "run `rmmod 88x2cu`").

**Kernel-monitor candidates**: `/sys/class/net/*` entries carrying a
`phy80211` link — ifname, MAC, driver, current type, operstate. Same sysfs
idiom `air_mon.cpp` already uses.

**Reconciliation.** A Realtek dongle with a kernel driver bound appears in
*both* lists. It must be shown **once**, with both identity forms — `bus/1-1.2`
and `ifname/<mac>`, which are precisely the two `calib_identity()` strings
(`calib_store.h:26`) — and the note that choosing devourer means unbinding the
kernel driver, and that any calibration artifact is per-identity and therefore
per-backend.

Two things discovery must **not** do by default: claim a device, or reset one.
`claim_interface_then_reset()` resets the chip; that belongs behind an explicit
`--probe` flag for operators who want the devourer-resolved chip name and
accept the reset. Bus paths shuffle after a re-plug — the human output should
say so, since the generated config pins them.

## 5. `waybeam-link config` — and the fleet file

```
waybeam-link config [--fleet fleet.json] [-o node.json] [--answers a.json]
```

Flow: read (or create) `fleet.json` → pick archetype → run discovery → assign
adapters and roles → answer the node-local questions → emit → validate in
process → write both the node config and the updated `fleet.json`.

`fleet.json` is the source of truth for everything that must agree across
nodes: `net_id`, channel/bw, `csa.home_chan` + `channel_allowlist`, the
`scout.channels` list, and the roster of `{originator, archetype, host}`. The
first node authors it; every subsequent node inherits it and is asked only what
is local. That is what turns the cache↔ground endpoint pairing, the
`preferred_originator` propagation and the `status_to` lists into generated
values instead of touch-ups. It also lets the tool allocate a unique
`originator` instead of the operator picking 9/10/17/33 from memory (§2: an
operator-set tail number, uniqueness is on us).

**`policy.csa.psk` is never written to `fleet.json`.** It is prompted per node,
echoed as `(set, redacted)`, and lands only in the node config — same property
the config dump already preserves. If the psk is left unset the tool must say
out loud what `deploy/README.md` says: the fleet then runs announced-token mode
(Pass 61/63) and the only takeover defence is the §11.5a sticky binding.

**Acceptance criterion:** the harness must be able to reproduce the four files
in `deploy/` — modulo comments and site-specific addresses — from a fleet file
plus the per-node answers. If it cannot, it does not yet describe the fleet we
actually fly.

## 6. Tests

`tests/config_wizard_test.cpp`: drive the wizard with scripted answers across
{four archetypes} × {radio, kernel-monitor where legal}, assert every emitted
config loads through `load_config()` and matches a golden under
`tests/golden/`. Discovery is injected (a fake inventory), so no hardware is
needed and the `dev` preset stays the gate. When the loader grows a required
key, the golden diff is the reminder to teach the generator about it.

## 7. TX power — two levels, one of them needs a ruling

### Level 1 — the config knobs (no spec question, do this first)

The wizard offers `power_map`, `max_power_qdb` and `power_presets_qdb` on the
TX adapter, and validates them at authoring time by actually calling
`load_power_curve()` (`power_file.h:31`) on the referenced file — a missing or
malformed PHY_REG_PG table becomes an error at your desk instead of a surprise
on the craft. It should also repeat the `deploy/README.md` warning: on
kernel-monitor, power actuation is a documented no-op and any regulatory limit
must be met at the adapter/driver.

### Level 2 — **OPEN — an operator-authored §10.6 artifact**

Mechanically this is easy: `calib_store_write()` (`calib_store.h:32`) already
writes `artifact.json` + `curve.txt`, binds the identity, and returns the CRC-8
fingerprint carried in the §3.15 word. Handing it an operator-supplied curve is
a few dozen lines.

The reason this is not in the plan as decided work is that §10.6 describes a
**last-good measurement**, and an authored artifact written through the same
path is indistinguishable from a measured one — to the boot auto-load, to
`GET /api/v1/calibration`, and to the fingerprint on the wire. Three questions,
none of which the harness may answer on its own:

1. Is an authored (never-measured) artifact legal under §10.6 at all, or must a
   vendored curve arrive by some other route — e.g. as a `power_map` plus
   ceilings, which is Level 1 and needs no ruling?
2. If legal, does it carry provenance (`source: "operator" | "campaign"`), and
   does `/api/v1/calibration` and the §15.3 stats object have to surface that
   distinction? A ground that trusts a fingerprint is trusting a measurement.
3. Identity. Pass 73 / T7 recorded `calibrate: STALE artifact (stored
   wlan0/98:03:cf:cf:a4:28, live bus/1-1)` — the same physical adapter has a
   different identity per backend, and on devourer the identity is a **bus
   path** that changes on every re-plug. A vendored artifact would go stale on a
   re-plug. Does a vendored artifact get a weaker identity (chip + adapter
   name), or does it accept staleness and re-authoring?

Per the law in `CLAUDE.md` these are operator rulings, and whichever way they
go the ruling gets a numbered Pass entry in `docs/review-log.md` and commits
ahead of the code.

## 8. The same harness as an API for coding agents

Most configs in this repo are drafted with AI assistance, and an agent hitting
a hand-written schema in a prompt is exactly the thing that goes stale. The
answer is not a second surface: **the CLI is the API, with `--json` on
everything**, and the schema is emitted by the binary rather than checked in
next to it.

Three surfaces, all of them the same code paths a human uses:

1. `waybeam-link config-schema --json` — the machine-readable schema **of this
   binary**. An agent working on a feature branch asks that branch's binary
   what it accepts, so the schema cannot be older than the loader it describes.
   A checked-in `schema.json` in a prompt or a vendored MCP server has no such
   property.
2. `waybeam-link <mode> -c cfg --check --strict --json` — structured
   diagnostics instead of a prose line on stderr:
   `{"ok":false,"errors":[{"path":"policy.calibration.seek_step_qdb",
   "msg":"must be >= 8 (2 dB …)","section":"§10.6"}],"warnings":[…]}`.
   This is what closes the agent's loop — draft, check, repair — without a
   human reading the error out loud.
3. `waybeam-link config --archetype tx-vehicle --answers a.json -o node.json`
   — the wizard's non-interactive mode from §3 is already an agent API. The
   agent fills the answers file; the tool still owns the shape, the
   invariants and the fleet coupling.

MCP on top of this is a thin stdio shim over three subcommands with no logic of
its own, so it stays an option rather than a prerequisite. `--json` first.

### `--strict`, and why it matters more than the schema

**The loader silently ignores keys it does not recognise.** All 225 lookups in
`config.cpp` go through `value()` / `contains()`; nothing enumerates the object
and complains about the rest. So `"max_bitrate"` for `"max_bitrate_kbps"`, or a
key invented from an older doc, loads clean and flies wrong. That is the single
most likely failure mode for an AI-drafted config, and no schema handed to the
agent prevents it — only the binary rejecting it does.

Two things make this safe to adopt:

- `_`-prefixed keys are comments (`_comment`, `_verify_timeout_ms` in
  `examples/config.radio-rx.sample.json`) and stay exempt by rule.
- Every node config in `deploy/` and `examples/` was checked against the key
  set in `config.cpp`: **no unknown keys today**. The only hit is
  `examples/topology.frame-shm-udp.sample.json`, which is not a node config —
  it is input to `tools/expand_arq_topology.py`. Strict mode can therefore be
  turned on without breaking anything we currently fly.

Suggested staging: `--check --strict` warns first, and becomes an error once
the harness is what authors configs.

### Tracking waybeam-link as it changes

This is the part that decides whether the harness is worth building, because a
generator that lags the loader is worse than no generator. The mechanism should
be structural, not a habit:

- **One key registration per parsed field**, declared next to the
  `value()`/`contains()` call that already exists, serving three purposes at
  once: unknown-key detection for `--strict`, the field list for
  `config-schema`, and the drift check. Adding a feature means adding one line
  beside the parse line you were already writing — not a 1248-line rewrite of
  the most safety-critical path in `io/`.
- **A golden schema under `tests/golden/`**, diffed in the `dev` preset. A new
  config key then shows up as a reviewable line in the PR that introduces it,
  which is how a new feature becomes visible to the harness instead of silently
  bypassing it.
- **A test that fails when a key is parsed but unregistered**, so the
  registration cannot be forgotten. Mechanical enforcement; no reviewer
  vigilance required.
- **The rule in `CLAUDE.md`**: node configs are generated and validated, never
  hand-written — draft with `config --answers`, verify with `--check --strict`,
  and read the schema from the binary rather than from memory or from an
  example that may predate the feature.
- Emit the `§N.M` section reference alongside each field. The comments in
  `config.cpp` already carry them; putting them in the schema points an agent
  at `PROTOCOL.md` instead of leaving it to guess semantics from a key name.

Prior art worth noting: `tools/expand_arq_topology.py` already expands one
topology description into matched TX/RX configs for the UDP ARQ bench. The
fleet file in §5 is the same idea generalised to the shipping archetypes — and
the reason to move it into the binary is precisely the drift problem above.

## 9. Suggested order

1. Key registration + `--check --strict --json` + `config-schema --json`, with
   the golden schema and the unregistered-key test. This lands **first**: it is
   the smallest change, it is useful to every agent-drafted config from the day
   it merges even with no wizard at all, and it is the anti-drift floor
   everything else stands on.
2. `waybeam-link discover` + `--json` + the reconciliation note. Useful on its
   own for every bring-up.
3. Ruling on §2 (backend mix) — it decides the wizard's adapter model.
4. The wizard for the two RX archetypes (`rx-spectator`, `rx-cache`), which are
   the smallest configs, plus the fleet file and the golden test.
5. `tx-ground`, then `tx-vehicle` (largest surface: venc, fec, select, modes).
6. Level 1 TX power inside the wizard.
7. `--strict` from warning to error; the `CLAUDE.md` rule.
8. Ruling on §7 Level 2, then the authored artifact if it is granted.
