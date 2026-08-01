# Ground-uplink calibration — Claude Code implementation handover

Status: implementation-ready design for PR #82. The PR remains draft until the
code, cross-repo integration, independent review, and hardware gates below are
complete.

## Objective

Implement automatic calibration of the ground station's one designated uplink
TX adapter. The ground varies its own TX power; the claimed craft reports how
many normal LINK_REPORT frames it accepted and their actual receive RSSI; the
ground finds and persists one maximum-clean MCS0/LGI/HT20 placement.

This closes the asymmetry left by craft/downlink calibration without adding
uplink probe traffic, weakening claim authority, changing SELECTOR_STATE, or
turning calibration into a continuous power controller.

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

- Packet type `0xF` is `UPLINK_QUALITY`, exact size 34 bytes (§3.16).
- Feedback is authenticated with the existing `csa_psk`; there is no new key.
- SELECTOR_STATE stays byte-for-byte unchanged.
- Quality is emitted at 2 Hz only immediately before existing live DATA. It is
  never standalone and never extends/re-arms an EOB quiet gap.
- Only the claimed/report-latched ground is targeted. Feedback does not create
  or transfer a claim.
- Calibration uses normal LINK_REPORT epochs. Do not add padded probes.
- Ground calibration is local REST, not VEHICLE_CMD.
- Uplink is one fixed MCS0/LGI/HT20 placement, not an eight-rung curve.
- Extract one reusable pure seek/verify engine; preserve the existing craft
  calibrator's observable behaviour.
- Probe gate: 40 unique epochs, one ambiguous extension to 80. Verify gate: 200
  epochs. Default step: 16 qdb. Default quality timeout: 2 s. Reuse the existing
  15/50‰ loss walls and cap-wall/retreat semantics.
- Explicit config wins over a stored artifact. Every unsuccessful exit restores
  config, the prior valid artifact, or backend auto/default in that order.
- Ground quality/calibration stats use `uplink_*`; never overload `calib_*`.
- Android needs no implementation change at present. Verify that unknown `0xF`
  remains harmless.

## End-to-end flow

1. Ground claims the craft through the existing authenticated CSA path.
2. Ground emits ordinary LINK_REPORTs at its configured cadence and records the
   unique epochs it attempted.
3. Craft accepts reports through the existing report-authority gate. For each
   newly accepted report it increments a cumulative count and adds the received
   frame's `AirRxMeta.rssi` to a signed cumulative sum.
4. Before live DATA, at most 2 Hz, craft emits authenticated UPLINK_QUALITY for
   that exact ground `(originator, session)`.
5. Ground validates source, selected craft/session, destination, target ground
   session, HMAC, and cumulative ordering. Only a new cumulative sample refreshes
   quality freshness.
6. A local REST start snapshots the current power owner, checks prerequisites,
   then runs the one-rung seek. Dwell loss is derived from locally emitted unique
   epochs versus craft-accepted count deltas; RSSI is derived from signed-sum
   deltas.
7. A successful verify atomically persists and applies the placement. Any other
   exit restores the pre-run owner and value.

## Link implementation plan

### 1. Wire codec and authenticated quality state

Expected touch points:

- `core/include/wblink/types.h`
- `core/include/wblink/wire.h`
- `core/src/wire.cpp`
- existing HMAC helper used by CSA/VEHICLE_CMD, or a small shared core helper
- `tests/wire_roundtrip_test.cpp` plus a focused quality test

Add the exact §3.16 structure and codec. Encode signed `rssi_sum_dbm` as a
big-endian two's-complement i32. MAC the already encoded bytes 0–29 and compare
tags in constant time. Do not authenticate or extend SELECTOR_STATE.

Implement a pure receive gate that owns the last accepted target/source tuple,
counters, and freshness. It must reject wrong key, wrong source, wrong local
target/session, stale craft session, and backward cumulative state. Exact
duplicates are harmless but do not refresh the 2 s clock. A target tuple change
resets the domain. Keep all u32 comparisons wrap-aware.

### 2. Craft counter collection and emission

Expected touch points:

- `app/main.cpp` around `TxCore::on_air`, accepted LINK_REPORT handling, and
  selector-state emission
- the `AirRxMeta` path that already contains received RSSI

Thread the receive metadata RSSI to the point where a LINK_REPORT has passed
the existing authority/epoch acceptance gate. Increment quality counters only
there—never for a structurally valid but rejected/duplicate report. Reset the
counter epoch on accepted reporter `(originator, session)` transition.

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
- new `UplinkCalibrator`: one MCS0 placement, report-count sample gates, local
  power ownership, separate artifact/status.

First lock down current craft behaviour with characterization tests. The
refactor is unacceptable if existing actions, placements, failure reasons, or
restore ordering change unintentionally.

The uplink observation adapter consumes deltas between authenticated quality
samples. It also records locally emitted unique report epochs so missing epochs
are counted as loss. Never substitute wall time for missing samples. Bound the
ambiguous extension once and share the existing three verify-descent budget.

### 4. Ground actuator ownership and conflict handling

Expected touch points:

- `io/src/config.cpp`, `io/include/wblink/config.h`
- `app/main.cpp` ground `run_rx`, `AirBackend::tx_index`, power apply/restore,
  retune/recovery paths
- config and ground-runtime tests

Change the rx-node `power_map` rejection narrowly: accept it only on the single
adapter whose adapter role is `tx`; keep rejecting maps on all `role:"rx"`
adapters. Resolve MCS0 at level 4, apply `max_power_qdb`, and reapply after every
retune/recovery that can reset TXAGC. Do not broaden craft selector ownership or
allow more than one uplink injector.

Define one central ground power-owner resolver:

1. explicit uplink `power_map` MCS0/level-4 placement;
2. matching persisted uplink artifact;
3. backend auto/default.

Calibration temporarily owns the actuator but snapshots the resolved owner.
Start must 409 during scout, CSA/retune, an MTU command campaign, a power
override, or another calibration. If one of those starts while calibration is
running, abort and restore before the conflicting operation changes hardware.
Use a scope guard/finalizer or an equivalent single convergence path so process
shutdown and error returns cannot strand probe power.

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
  "mcs": 0,
  "short_gi": false,
  "placement_qdb": 0,
  "placement_rssi_dbm": 0,
  "placement_loss_milli": 0,
  "last_clean_qdb": 0,
  "first_bad_qdb": null
}
```

CRC-8 is over a pinned canonical binary serialization, not JSON formatting.
Write to a temporary file, fsync as the existing store does, and atomically
rename. Store separately under `policy.calibration.artifact_dir`. Exclude craft
session from the pairing identity: a reboot alone must not stale a valid result.
Do include local adapter identity, craft originator/fingerprint, band,
bandwidth, MCS, and GI. A mismatch is visible but never applied.

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
§15.3 on every role with stable neutral defaults. Never print the PSK or MAC
input. Keep failure reasons bounded stable tokens suitable for Hub display.

## Hub integration

On a fresh Hub feature branch, read its current Link client and calibration
menu before editing. Preserve the existing vehicle command used for craft
downlink calibration. Add a separately labelled **Ground uplink** view that:

- reads local Link `GET /api/v1/calibration` on the deployment's `:8092`;
- starts/aborts with the new local POST action;
- renders prerequisites, running progress/sample count, placement, stale state,
  and bounded failure reason;
- never routes an uplink start through `/api/v1/vehicle/command`;
- clearly distinguishes Craft downlink from Ground uplink in labels and status.

Parse the new stats fields without making them mandatory for an older Link
binary. Add client parsing, route/handler, and UI tests, then run the full Hub
suite and both supported ground builds.

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

Add at least these 20 focused successful cases (they may be grouped into test
binaries, but report individual cases):

1. UPLINK_QUALITY exact 34-byte round trip, including negative RSSI sum.
2. Encode refuses a short output buffer; decode rejects short and long inputs.
3. Wrong HMAC is rejected.
4. Wrong target originator is rejected.
5. Wrong target session is rejected.
6. Wrong/old craft session is rejected.
7. Duplicate feedback does not add samples or refresh freshness.
8. Backward/replayed cumulative counters are rejected.
9. Target reporter session change resets the counter domain.
10. Counter/RSSI delta arithmetic remains correct across u32 wrap.
11. Rejected or duplicate LINK_REPORT does not enter craft quality counters.
12. Quality emits before live DATA at 2 Hz and never standalone/after EOB.
13. A legacy/unknown-type receive path ignores `0xF` without losing DATA or
    SELECTOR_STATE processing.
14. Existing eight-rung calibrator characterization remains unchanged after
    `PowerSeek` extraction.
15. One-rung clean ramp reaches max and verifies.
16. Cap wall confirms once and places the last clean step.
17. Loss wall/blackout retreats and verify descents remain bounded.
18. Two-second quality expiry aborts and restores prior power.
19. Config accepts a map only on the rx-node's designated `role:"tx"` adapter;
    diversity-map and multiple-uplink cases fail.
20. Explicit map/artifact/auto precedence, identity stale handling, atomic
    store round trip, REST validation, and stats golden defaults all pass.

Also run the full Hub suite and SBC target builds. Run the coordination protocol
audit and port check; Link's own `PROTOCOL.md` remains the wire source of truth.
Perform an independent full-diff review after tests, specifically checking
authentication boundaries, power ownership, all exits, retune restoration,
counter wrap, and mixed-version behaviour.

## Hardware verification — must be device-confirmed

Automated tests are code-level only. Do not mark this PR merge-ready until all
of the following are run on the deployed SSC338Q craft plus both x86 and RK3566
ground variants with available RTL8812EU/CU uplink adapters:

1. Confirm normal 60 fps video and a stable claim before calibration.
2. Perform ten consecutive near-bench calibrations. Require 10/10 successful
   completions and placement spread no greater than one 16-qdb seek step.
3. For every run record duration, samples per dwell, placement, RSSI/loss,
   bracket, quality timeouts, report delivery, NACKs, and restore result.
4. Inject abort, feedback blackout, local adapter mismatch, craft fingerprint
   mismatch, ground restart, craft reboot, and retune. Confirm safe restore and
   correct stale semantics.
5. Compare default/driver-auto, a known manual placement, and calibrated
   placement at useful range. Record LINK_REPORT delivery, NACK recovery,
   report blackouts, craft RX RSSI, and uplink loss. Calibration must not make
   the return path less robust.
6. Verify quality adds at most its 2 Hz piggyback packets, creates no standalone
   TX opportunities, and causes no material CPU/packet-rate regression on craft
   or ground.
7. Reboot both ends and confirm only an identity-matching artifact auto-applies;
   explicit `power_map` still wins.

Preserve raw JSONL/log artifacts under `docs/data/` with the tested commit IDs,
hardware identities, config, distance/setup, and exact time windows. Mark each
claim **device-confirmed** or **code-level only**.

## Completion checklist

- [ ] Link spec commit precedes implementation commits in PR #82.
- [ ] Full Link tests and three target builds pass.
- [ ] At least 20 focused cases above pass.
- [ ] Hub feature branch/PR passes its full suite and exposes both directions.
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
- Do not widen the data path's authentication scope.
- Do not silently repair unrelated craft artifact-schema gaps in the same diff;
  file a follow-up unless Pass 125 cannot be implemented without it.
- Do not claim Android verification beyond the mixed-version/unknown-type audit.
- Do not deploy or merge on automated evidence alone.
