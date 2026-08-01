# Link calibration as a feature — scope + rulings needed

**Status:** design scope, pre-Pass. Follow-on from the validated bench loop
(`tools/curve_author.py`, `docs/mcs-power-sweep.md` fourth session).
Operator directions already given (2026-08-01): integrate into waybeam-link,
trigger from the waybeam-hub vehicle menu, calibrate per craft/ground
pairing at **~50–100 m separation**, persist until the pairing changes,
artifact must remain valid across the future devourer/per-packet-TX move.

## The field constraint, and the architecture it forces

In the field the craft has **no IP path** — the RF link carries §3 packets
only, and §11.7's law keeps the air channel narrow (one enum byte; bulk
data "never over the air"). So neither the bench tool's REST drive nor a
curve-file upload can reach a flying craft.

**Resolution: run the loop craft-side.** The bench prototype's brains move
into the craft; the ground's only roles are *trigger* and *display*:

- **Feedback path already exists.** The §3.5 LINK_REPORT at 10 Hz carries
  `rssi_best`/`rssi_mean`, `loss_postdiv_prearq`, `uniq`, `diversity` —
  the craft steers each rung's power against ground-reported RSSI and
  verifies against ground-reported loss, exactly the closed loop the bench
  tool runs, with **zero new uplink bytes**.
- **The artifact is born where it is consumed.** The craft authors the
  §10.2 curve + ceiling report locally and persists them locally. There is
  no file to sync. The "claim-gated syncup" question dissolves into a
  claim-gated *trigger* — which §11.7 already provides (bound-issuer only,
  PSK-keyed HMAC, per-session nonce monotonicity, rate-limit).

The rejected alternative — ground-driven loop + chunked artifact upload
over RF — would need a new bulk-transfer wire type with ack/HMAC/resume,
against the §11.7 philosophy, to move a file to the only node that never
needed it moved.

## Proposed mechanics (each line item is a ruling to take)

1. **Trigger: VCMD `0x08 CALIBRATE`, `cmd_arg` {0=abort, 1=start}.** Fits
   the Pass-68 ≤5-choice bound. `REJECTED` when: no `power_map`-capable
   actuator, calibration already running (for `start`), not running (for
   `abort`), or reports are not currently flowing from a latched reporter
   (the loop is blind without them).
2. **Safety envelope while running:** selector frozen (the loop owns rung
   pins); channel never touched; total duration bounded (~8 rungs × ~20 s
   ≈ 3 min, hard cap); **abort-on-report-timeout** → immediate restore of
   the committed operating point and power; abort restores likewise. The
   §9.8 fail-toward-degradation posture applies throughout. Operator is
   responsible for airspace/time (same class as MODE's pre-flight rule,
   but calibration is expected *airborne at 50–100 m* — the envelope, not
   an on-ground gate, is the protection).
3. **Persistence exception:** §11.7 state is craft-session volatile by
   convention; the calibration *artifact* (curve + ceiling report +
   fingerprint) must persist (`/etc/waybeam-link/`), like mode files.
   Needs an explicit ruling that the command's product — not its state —
   survives reboot, and that a persisted curve auto-loads as the adapter's
   `power_map` on boot.
4. **Pairing fingerprint + staleness:** artifact records craft adapter
   (MAC/chip), the reporting ground `(originator, adapter-count)` observed
   during calibration, band/channel, timestamp, and the placement RSSI
   band. Runtime surfaces CALIBRATION STALE when the live pairing
   mismatches (the mode-catalog fingerprint pattern: detect drift, never
   guess).
5. **Ground visibility (small §3.15 extension):** the selector-state
   broadcast gains a calibration word — {idle, running(rung r), done-ok,
   failed(reason)} + a curve-fingerprint byte — so the hub vehicle menu
   can show progress and result with no IP path. Bench-time HTTP (§15.5)
   exposes the full report for analysis.
6. **Hub integration:** a vehicle-menu entry issuing `CALIBRATE start`/
   `abort` through the existing ground REST → VCMD path, displaying the
   §3.15 calibration word. No hub-side smarts.

## Fidelity notes (stated, not hidden)

- The field loop steers on **post-diversity** loss (what LINK_REPORT
  carries) where the bench tool used per-adapter wire PER. At placement
  ("is it clean here") and at the cliff (adapters fail together — measured)
  the two agree; per-adapter fidelity remains a bench-mode luxury.
- Ceilings are receiver-RSSI-referenced and transfer across range; the
  *placement* is what the 50–100 m requirement buys. The bench-range curve
  is tooling validation only, never a flight curve.
- Forward-validity to devourer per-packet TX: the artifact's content is
  target RSSI placements + per-rung ceilings; per-packet power is a finer
  actuator for the same targets, and `SetTxPowerRateDiffs` absorbs
  per-rate chip-cal skew (the measured devourer-path MCS5 anomaly).

## Rulings requested before implementation

R1. Add `0x08 CALIBRATE` to the §11.7 registry (semantics above).
R2. Persistence exception + boot auto-load for the calibration artifact.
R3. §3.15 calibration word (field layout, fingerprint byte semantics).
R4. Abort/timeout restore semantics as specified in the safety envelope.
R5. Whether the craft-side loop lands in `core/` (pure, time-injected,
    testable dry — recommended) with `io/` supplying actuation.
