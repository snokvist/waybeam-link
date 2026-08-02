# Ground-uplink calibration — Claude Code implementation handover

Status: implementation-ready design for PR #82. The PR remains draft until the
code, cross-repo integration, independent review, and hardware gates below are
complete.

## Objective

Implement automatic calibration of the ground station's one designated uplink
TX adapter. The ground varies its own TX power; the selected craft reports how
many normal LINK_REPORT frames it accepted, their actual receive RSSI, and the
rung they arrived on; the ground finds and persists one maximum-clean placement
per configured uplink rung (v1: one, MCS0/LGI/HT20).

This closes the asymmetry left by craft/downlink calibration without adding
uplink probe traffic, weakening claim authority, changing SELECTOR_STATE, or
turning calibration into a continuous power controller. Together the two
directions form a single one-time commissioning step, persisted on both sides
and exposed as one Hub action.

## Start here

1. Work in `/home/snokvist/dev/waybeam-coordination` and run the coordination
   `/sync-repos` workflow.
2. Read `CLAUDE.md`, `repos/waybeam-link.md`, `repos/waybeam-hub.md`,
   `repos/sbc-groundstations.md`, and each affected repo's `CLAUDE.md`.
3. In Link, check out PR #82's branch
   `spec/pass122-ground-uplink-calibration-plan`. The branch name is historical;
   the rebased design is **Pass 125**. Do not renumber Passes 122–124.
4. Treat `PROTOCOL.md` §3.16 and §10.7 as law. If implementation exposes a gap,
   stop and record an explicit operator ruling before choosing new wire or
   safety semantics.
5. Never edit `third_party/`. Keep the pure engine in `core/`, time-injected and
   dependency-free. Do not commit directly to a default branch.

PR #82 is the Link/spec implementation PR. Hub and SBC changes belong on their
own feature branches/PRs and are linked from #82. Do not mix unrelated local
worktree changes into any commit.

## Locked decisions

- Packet type `0xF` is `UPLINK_QUALITY`, exact size **35 bytes** (§3.16), MAC
  over bytes 0..30.
- `last_rx_mcs` (offset 30) ships in v1 even though v1 has one uplink rung. The
  packet is exact-length with no spare byte and no flags field, so adding it
  later is a wire break; and it is the commanded-versus-delivered rung
  cross-check the fleet's rate-handling history calls for.
- Feedback is authenticated with the existing `csa_psk`; there is no new key.
- SELECTOR_STATE stays byte-for-byte unchanged.
- Quality is emitted at 2 Hz only immediately before existing live DATA. It is
  never standalone and never extends/re-arms an EOB quiet gap.
- Only the craft's accepted §3.5 reporter is targeted. Feedback does not create
  or transfer a claim.
- **Authority is the report latch, not a CSA claim.** A valid-MAC packet naming
  the ground's own `(originator, session)` is proof it holds the latch.
- **Two clocks.** Any accepted packet refreshes *liveness*; only an advancing
  `reports_received` refreshes *counter progress*. Liveness loss aborts; stalled
  counters under live feedback are a 1000‰ loss sample. Never conflate them.
- **Floor ≠ ceiling.** With no clean probe yet, loss means "too cold" — ascend.
  Retreat/place only above a clean probe. This amends §10.6 too.
- **Dwell loss is anchored on the craft's own `last_report_epoch` bounds**, not
  on ground wall-clock or raw ground emission count. Formula is spec'd in §10.7.
- Calibration uses normal LINK_REPORT epochs. Do not add padded probes.
- Ground calibration is local REST, not VEHICLE_CMD.
- Uplink is one placement per configured rung; v1 configures one
  (MCS0/LGI/HT20). Do not hardcode "MCS0" as a constant — it is a config value.
- Extract one reusable pure seek/verify engine; preserve the existing craft
  calibrator's observable behaviour **above the first clean probe**.
- Probe gate: 40 unique epochs, one ambiguous extension to 80. Verify gate: 200
  epochs. Default step: 16 qdb. Default liveness timeout: 2 s. Reuse the
  existing 15/50‰ loss walls and cap-wall/retreat semantics.
- Explicit config wins over a stored artifact. Every unsuccessful exit restores
  config, the prior valid artifact, or backend auto/default in that order.
- The uplink artifact's `placements` is a **list** from v1, length 1.
- The two directions are mutually interlocked and ordered: **uplink first**.
- Hub exposes **one** bi-directional calibration action, not two. Link keeps two
  independent operations; Hub owns the sequencing.
- Ground quality/calibration stats use `uplink_*`; never overload `calib_*`.
- Android needs no implementation change at present. Verify that unknown `0xF`
  remains harmless.

## End-to-end flow

1. Ground emits ordinary LINK_REPORTs at its configured cadence and records the
   unique epochs it emitted, keyed by epoch number.
2. Craft accepts reports through the existing report-authority gate — which
   latches passively; no CSA is involved. For each newly accepted report it
   increments a cumulative count, adds the accepted copy's `AirRxMeta.rssi` to a
   signed cumulative sum, and records that copy's `rx_mcs`.
3. Before live DATA, at most 2 Hz, craft emits authenticated UPLINK_QUALITY for
   that exact ground `(originator, session)`.
4. Ground validates source, selected craft/session, destination, target ground
   session, HMAC, and cumulative ordering. Any accepted packet refreshes
   liveness; only an advancing count refreshes counter progress.
5. A local REST start checks prerequisites — including that the craft is not
   itself calibrating — snapshots the current power owner, then runs the seek.
   Dwell loss uses the §10.7 formula, with `emitted` counted over
   `(E_A, E_B]` from the dwell's own first and last accepted packets.
6. A stalled count under live feedback is a 1000‰ dwell. With no clean probe yet
   the seek ascends through it; above a clean probe it is a wall.
7. A successful verify atomically persists and applies the placement. Any other
   exit restores the pre-run owner and value.
8. Hub then runs the downlink phase (§11.7 `CALIBRATE`) and reports the combined
   result as one operation. A failed uplink phase stops the sequence.

## Link implementation plan

### 1. Wire codec and authenticated quality state

Expected touch points:

- `core/include/wblink/types.h`
- `core/include/wblink/wire.h`
- `core/src/wire.cpp`
- existing HMAC helper used by CSA/VEHICLE_CMD, or a small shared core helper
- `tests/wire_roundtrip_test.cpp` plus a focused quality test

Add the exact §3.16 structure and codec — **35 bytes**, `last_rx_mcs` at offset
30, MAC over bytes 0..30. Encode signed `rssi_sum_dbm` as a big-endian
two's-complement i32. Do not authenticate or extend SELECTOR_STATE. The tag is a
u32 equality compare, matching `csa.cpp`'s existing posture; do not introduce a
constant-time requirement §11.4 does not hold itself to.

Implement a pure receive gate that owns the last accepted target/source tuple,
counters, and **two independent clocks**. It must reject wrong key, wrong
source, wrong local target/session, stale craft session, and backward cumulative
state. An exact duplicate is accepted, refreshes liveness, and yields no sample.
A target tuple change resets the domain. Keep all u32 comparisons wrap-aware.

### 2. Craft counter collection and emission

Expected touch points:

- `app/main.cpp` around `TxCore::on_air`, accepted LINK_REPORT handling, and
  selector-state emission
- the `AirRxMeta` path that already contains received RSSI

Thread the receive metadata RSSI **and `rx_mcs`** to the point where a
LINK_REPORT has passed the existing authority/epoch acceptance gate. Under
craft-side RX diversity use the accepted copy — the adapter that first delivered
that report. Increment quality counters only there — never for a structurally
valid but rejected/duplicate report. Reset the counter epoch on accepted
reporter `(originator, session)` transition.

Schedule UPLINK_QUALITY beside SELECTOR_STATE but independently. It becomes due
at 2 Hz, is coalesced, and is inserted immediately before live DATA. Add a test
that idle craft traffic does not emit it and a test that an EOB does not create
another slot. The packet targets the accepted reporter and uses the craft's
configured PSK and canonical adapter-identity CRC-8.

### 3. Reusable one-rung seek

Expected touch points:

- `core/include/wblink/calibrate.h`
- a matching `core/src` file if extraction makes the header unwieldy
- existing and new calibrator tests

The current `Calibrator` hardcodes eight rungs, pin actions, and an eight-entry
artifact; it is not reusable unchanged. Extract its power ramp, cap/loss wall,
wall confirmation, blackout retreat, verify descent, and hard-cap mechanics
into a pure `PowerSeek` with injected time and observations. Keep orchestration
outside it:

- existing `Calibrator`: eight rungs, selector pins, existing artifact/word;
- new `UplinkCalibrator`: one placement per configured rung (v1 one),
  report-count sample gates, local power ownership, separate artifact/status.

`PowerSeek` must take an explicit per-dwell verdict — `clean | bad |
no_evidence` — rather than inferring `no_evidence` from zero samples the way
`calibrate.h` does today. The craft adapter supplies `no_evidence` on a blank
dwell; the uplink adapter supplies `bad`, because a stalled counter under live
feedback is evidence.

`PowerSeek` must also implement the §10.6 floor rule: with no clean probe on the
rung, a bad probe **ascends** to `max_qdb`, and only a bad probe at `max_qdb`
fails with `no_clean_point`. Today `calibrate.h` places at `min_qdb` in that
case, and the blackout retreat falls through to a `report_loss` abort because it
requires a `last_clean_`.

First lock down current craft behaviour with characterization tests. The
refactor is unacceptable if existing actions, placements, failure reasons, or
restore ordering change unintentionally **above the first clean probe**. The
never-been-clean branch is expected to change — baseline it separately and run a
craft-side regression, since it alters the bottom of every rung-0 ramp.

The uplink observation adapter consumes deltas between authenticated quality
samples, using the §10.7 formula: `emitted` is the count of unique epochs this
ground emitted in `(E_A, E_B]`, where `E` comes from the dwell's own first and
last accepted packets. Do not use ground wall-clock or the raw emission count
over the dwell — at 40 samples one boundary-straddling report is 25‰ and lands
between the ok and bad walls. Never substitute wall time for missing samples.
Bound the ambiguous extension once and share the existing three verify-descent
budget.

### 4. Ground actuator ownership and conflict handling

Expected touch points:

- `io/src/config.cpp`, `io/include/wblink/config.h`
- `app/main.cpp` ground `run_rx`, `AirBackend::tx_index`, power apply/restore,
  retune/recovery paths
- config and ground-runtime tests

Re-key the `power_map` rejection from **node role to adapter role**: reject on
any `role:"rx"` adapter regardless of node role, accept on a `role:"tx"` adapter
on either node role. Today `io/src/config.cpp` tests `cfg.node.role ==
Role::kRx`, which both permits a never-applied map on a tx-node diversity
adapter and blocks the one adapter that can use it on an rx-node. Load the full
per-MCS curve; resolve only the configured uplink rung at level 4, apply
`max_power_qdb`, and reapply after every retune/recovery that can reset TXAGC.
More than one `role:"tx"` adapter on an rx-node is a config error. Do not
broaden craft selector ownership.

Extend the §10.5 override latch to an rx-node's uplink adapter (same range,
clamp, re-assert points, restore). This is the manual placement the hardware
A/B needs and removes the config-edit-plus-restart workaround.

Define one central ground power-owner resolver:

1. explicit uplink `power_map` placement for the configured rung;
2. matching persisted uplink artifact;
3. backend auto/default.

Calibration temporarily owns the actuator but snapshots the resolved owner.
Start must 409 during scout, CSA/retune, an MTU command campaign, a §10.5 power
override, another local calibration, **or a running craft calibration** (the
mirrored §3.15 `calib_state`, already in `stats.h`). While it runs, the ground
refuses to issue a §11.7 `CALIBRATE`. If one of those starts anyway, abort and
restore before the conflicting operation changes hardware. Use a scope
guard/finalizer or an equivalent single convergence path so process shutdown and
error returns cannot strand probe power.

### 5. Persistence

Expected touch points:

- `io/include/wblink/calib_store.h`, `io/src/calib_store.cpp`, or a distinct
  `uplink_calib_store.*` if that keeps schemas unambiguous
- persistence tests with temporary directories

Do not serialize a one-rung result as the existing eight-rung craft artifact.
Use a versioned, canonical schema containing at least:

```json
{
  "schema": 1,
  "direction": "uplink",
  "t_unix": 0,
  "local_adapter_identity": "...",
  "craft_originator": 0,
  "craft_adapter_fingerprint": 0,
  "channel_mhz": 0,
  "bw_mhz": 20,
  "placements": [
    {
      "mcs": 0,
      "short_gi": false,
      "placement_qdb": 0,
      "placement_rssi_dbm": 0,
      "placement_loss_milli": 0,
      "last_clean_qdb": 0,
      "first_bad_qdb": null
    }
  ]
}
```

`placements` is a list from v1 with exactly one entry, and rate identity
(`mcs`, `short_gi`) lives **in the entry**, never in the top-level identity
block. A future multi-rung uplink then appends entries instead of bumping the
schema and migrating every deployed artifact and Hub parser. Identity for the
stale check is local adapter + craft originator/fingerprint + channel +
bandwidth.

CRC-8 is over a pinned canonical binary serialization, not JSON formatting.
Write to a temporary file, fsync as the existing store does, and atomically
rename. Store separately under `policy.calibration.artifact_dir`. Exclude craft
session from the pairing identity: a reboot alone must not stale a valid result.
A mismatch is visible but never applied.

### 6. REST and stats

Expected touch points:

- `io/include/wblink/control_server.h`, `io/src/control_server.cpp`
- `io/include/wblink/stats.h`, `io/src/stats.cpp`
- `app/main.cpp` handler wiring
- control/stats golden tests

Keep `/api/v1/calibration` role-specific:

- craft GET: existing body plus `direction:"downlink"`;
- ground GET: §10.7 local state, quality, artifact, and failure reason;
- ground POST: exactly `{"action":"start"}` or `{"action":"abort"}`;
- unsupported role/action: 409; malformed body: 400.

Expose the Pass 125 `uplink_calib_*` and `uplink_quality_*` fields named in
§15.3 on every role with stable neutral defaults, including `uplink_calib_rung`
(0) and `uplink_quality_rx_mcs` (255). `uplink_quality_age_ms` measures
**liveness**, not counter progress. Never print the PSK or MAC input. Keep
failure reasons bounded stable tokens suitable for Hub display.

## Hub integration

On a fresh Hub feature branch, read its current Link client and calibration menu
before editing.

Expose **one** operator action — full bi-directional calibration — not two.
Calibration is a one-time commissioning step persisted on both sides, so a
~2-minute combined run is the right unit of work, and splitting it invites the
wrong order. Hub owns the sequencing:

1. uplink phase — local Link `POST :8092 /api/v1/calibration {"action":"start"}`,
   poll `GET` to a terminal state;
2. **stop if the uplink phase failed** — §10.7's order law is the entire reason
   the sequence exists;
3. downlink phase — the existing §11.7 `CALIBRATE` vehicle command, poll the
   mirrored §3.15 word to a terminal state.

The view renders which phase is running, per-phase progress/sample count,
prerequisites, both placements, stale state, and the bounded failure reason with
its phase. Abort cancels the phase in flight and does not advance. Never route
an uplink start through `/api/v1/vehicle/command`; never route a downlink start
through local REST.

Parse the new stats fields without making them mandatory for an older Link
binary. Add client parsing, route/handler, sequencing, and UI tests — including
uplink-fails-so-downlink-never-starts — then run the full Hub suite and both
supported ground builds.

## SBC packaging

After Link and Hub heads are reviewed, create a fresh SBC feature branch:

- pin the exact reviewed Link and Hub commits;
- ship any required ground config defaults without embedding hardware-specific
  calibration results;
- preserve the disabled unrotated `waybeam-link.log` setting;
- rebuild Powkiddy/x86 and Radxa/RK3566 targets;
- verify the installed local Link control endpoint remains `:8092` and no port
  conflict is introduced.

Do not update coordination snapshots until the operator reviews the drift.

## Required automated verification

Run the full Link gate:

```sh
cmake --build --preset dev
ctest --preset dev
cmake --build --preset ssc338q
cmake --build --preset x86-ground
cmake --build --preset rk3566
```

Add at least these 26 focused successful cases (they may be grouped into test
binaries, but report individual cases):

1. UPLINK_QUALITY exact 35-byte round trip, including negative RSSI sum and
   `last_rx_mcs` = 0, 7, and 0xFF.
2. Encode refuses a short output buffer; decode rejects short and long inputs.
3. Wrong HMAC is rejected; the MAC covers bytes 0..30 (flipping `last_rx_mcs`
   invalidates the tag).
4. Wrong target originator is rejected.
5. Wrong target session is rejected.
6. Wrong/old craft session is rejected.
7. A duplicate is accepted, refreshes liveness, and yields no sample.
8. Backward/replayed cumulative counters are rejected.
9. Target reporter session change resets the counter domain.
10. Counter/RSSI delta arithmetic remains correct across u32 wrap.
11. Rejected or duplicate LINK_REPORT does not enter craft quality counters.
12. Quality emits before live DATA at 2 Hz and never standalone/after EOB.
13. A legacy/unknown-type receive path ignores `0xF` without losing DATA or
    SELECTOR_STATE processing.
14. Existing eight-rung calibrator characterization is unchanged above the first
    clean probe after `PowerSeek` extraction.
15. **Floor rule:** a bad probe with no clean probe yet ascends; a bad probe at
    `max_qdb` with none anywhere fails `no_clean_point`; neither places at
    `min_qdb` nor aborts on the report clock.
16. **Stalled counters under live feedback score 1000‰** and do not abort; the
    seek ascends through a blacked-out floor to a clean placement.
17. Clean ramp reaches max and verifies.
18. Cap wall confirms once and places the last clean step.
19. Loss wall/blackout retreats and verify descents remain bounded and apply
    only above a clean probe.
20. **Loss denominator:** `emitted` counted over `(E_A, E_B]` is unaffected by a
    report emitted after `E_B`; the naive per-dwell emission count is shown to
    misreport that same scenario as ~25‰.
21. Two-second **liveness** expiry aborts and restores prior power.
22. Config accepts a map on a `role:"tx"` adapter on either node role and
    rejects it on any `role:"rx"` adapter on either node role; multiple-uplink
    on an rx-node fails.
23. §10.5 override latch applies, clamps, re-asserts, and restores on an
    rx-node's uplink adapter.
24. **Interlock:** start 409s while the mirrored §3.15 `calib_state` is
    `running`; a §11.7 `CALIBRATE` issue is refused while §10.7 runs; an
    override latched mid-run aborts and restores before it applies.
25. Artifact round trip with a one-entry `placements` list; a two-entry list
    parses without a schema bump; identity stale handling; atomic store.
26. Explicit map/artifact/auto precedence, REST validation, and stats golden
    defaults (`uplink_calib_rung` 0, `uplink_quality_rx_mcs` 255) all pass.

Also run the full Hub suite and SBC target builds. Run the coordination protocol
audit and port check; Link's own `PROTOCOL.md` remains the wire source of truth.
Perform an independent full-diff review after tests, specifically checking
authentication boundaries, power ownership, all exits, retune restoration,
counter wrap, and mixed-version behaviour.

## Hardware verification — must be device-confirmed

Automated tests are code-level only. Do not mark this PR merge-ready until all
of the following are run on the deployed SSC338Q craft plus both x86 and RK3566
ground variants with available RTL8812EU/CU uplink adapters:

§10.7 requires live DATA, so every run needs a real feed — on a bench that means
driving one with `tools/rtp_feed.py` or a live encoder, per the idle-TX gotcha in
`CLAUDE.md`. Run the uplink phase before the downlink phase throughout.

1. Confirm normal 60 fps video and a stable report latch before calibration.
2. Perform ten consecutive near-bench calibrations. Require 10/10 successful
   completions and placement spread no greater than one 16-qdb seek step.
3. For every run record duration, samples per dwell, placement, RSSI/loss,
   bracket, liveness timeouts, report delivery, NACKs, `last_rx_mcs`, and
   restore result.
4. **Blacked-out floor:** start a run at a distance/attenuation where `min_qdb`
   delivers nothing, and confirm the seek ascends to a clean placement rather
   than aborting or placing at the floor. This is the case the near-bench Pass
   121 campaign never exercised — do not skip it.
5. Inject abort, liveness blackout, local adapter mismatch, craft fingerprint
   mismatch, ground restart, craft reboot, and retune. Confirm safe restore and
   correct stale semantics.
6. **Interlock:** attempt an uplink start while the craft is calibrating and a
   §11.7 `CALIBRATE` while the uplink runs; confirm both are refused and neither
   strands probe power.
7. Compare default/driver-auto, a §10.5 manual placement, and the calibrated
   placement at useful range. Record LINK_REPORT delivery, NACK recovery,
   report blackouts, craft RX RSSI, and uplink loss. Calibration must not make
   the return path less robust.
8. Verify quality adds at most its 2 Hz piggyback packets, creates no standalone
   TX opportunities, and causes no material CPU/packet-rate regression on craft
   or ground.
9. Reboot both ends and confirm only an identity-matching artifact auto-applies;
   explicit `power_map` still wins, and the craft's own downlink artifact
   survives independently.
10. Run the combined Hub action end to end: one operator press produces both
    placements in the right order, and an induced uplink failure stops the
    sequence before the downlink phase starts.

Preserve raw JSONL/log artifacts under `docs/data/` with the tested commit IDs,
hardware identities, config, distance/setup, and exact time windows. Mark each
claim **device-confirmed** or **code-level only**.

## Completion checklist

- [ ] Link spec commit precedes implementation commits in PR #82.
- [ ] Full Link tests and three target builds pass.
- [ ] At least 26 focused cases above pass.
- [ ] Hub feature branch/PR passes its full suite and exposes one bi-directional
      action that sequences uplink then downlink and stops on uplink failure.
- [ ] SBC feature branch/PR pins reviewed heads and both images build.
- [ ] Protocol audit is CONSISTENT and the port check has no conflict.
- [ ] Independent full-diff review has no unresolved blocker/major finding.
- [ ] Ten consecutive hardware runs pass with bounded placement spread.
- [ ] Failure injection and range A/B are device-confirmed.
- [ ] PR bodies link all three repos and distinguish code-level from hardware
      evidence.

## Scope guardrails

- Do not redesign MTU/FEC, selector policy, CSA authority, or report latching in
  this work.
- Do not add continuous/adaptive power steering after calibration.
- **Do not implement the multi-rung uplink in this PR.** A future pass will let
  the ground uplink run at higher MCS rungs so control/telemetry occupies less
  airtime. Pass 125 only ensures nothing has to be migrated for it: the
  `power_map` is already the full MCS0–7 `PowerCurve`, `TxRate{mcs,sgi,bw}`
  already flows per-frame through `dot11_tx_prefix`, §10.5's latch is
  rung-agnostic, §3.16 carries `last_rx_mcs`, the artifact's `placements` is a
  list, and the stats carry `uplink_calib_rung`. That future pass adds a rate
  policy and a rung loop; its other contact points are §9.3's
  `airtime_budget_frac` (`io/airtime.h`) for uplink airtime accounting and
  §7.2's return-window sizing, which assumes today's uplink frame duration.
- Do not widen the data path's authentication scope.
- Do not silently repair unrelated craft artifact-schema gaps in the same diff;
  file a follow-up unless Pass 125 cannot be implemented without it.
- Do not claim Android verification beyond the mixed-version/unknown-type audit.
- Do not deploy or merge on automated evidence alone.
