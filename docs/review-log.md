# Review log

Numbered record of **Tier-1 spec rulings** — the contract law of `PROTOCOL.md`.
Passes 1–152 (2026-07-10 → 2026-08-06) live in
`review-log-archive-p001-152.md`; this file continues the numbering from
Pass 153. The two-tier split itself is defined in `CLAUDE.md` ("The law").

## Format contract

- **Tier-1 rulings only.** An entry exists because a *contract* changed: wire
  format, trust/auth machinery, a state-machine behaviour peers depend on, or
  config semantics. Measurement-phase results — walls, gates, dwell counts,
  seeds, sweep parameters still being characterised — go to `findings.md`,
  not here, and not into `PROTOCOL.md`.
- **One numbered Pass per entry, ≤40 lines**: the verdict, the changed spec
  sections (`§N.M`), and an evidence pointer (branch, data dir, findings
  entry). No narration, no method essays.
- **No addenda.** A new fact after an entry is committed is a new Pass or a
  new finding — an entry is never reopened.
- **Method lessons** (process traps, measurement discipline) go to the
  coordination repo's memory, not this log.
- Spec-amendment-commits-first still applies to every entry here; it does
  **not** apply to findings.

## Passes

## Pass 176 — health and stats are C-ABI snapshots from the one fill path (2026-08-14)

**Ruling (coordination `specs/cross/2026-08-14-wblink-library-parity`
R3, slice 1).** The §15.3 stats line and the §15.4 health object become
readable through the C ABI on both roles: `wblink_rx_stats` /
`wblink_rx_health` / `wblink_tx_stats` / `wblink_tx_health`, under the
snapshot buffer contract (`snapshot_copy.h`). Both strings are published
by the run loop at the **same site** that feeds the control server —
`emit_stats()` + `build_health_json(last_snap)` — so REST, NDJSON and
the C ABI are three transports of one generation path; no second
serializer exists, and the publication is NOT guarded by
`WBLINK_CONTROL_SERVER` (a receive-only Android build gets the same
view the hub does).

**Cadence is stats.hz, by design.** These surfaces ARE the §15.3 walk:
`stats.hz=0` disables the walk, so the getters answer 3 (unpublished).
The always-on liveness surface remains the Pass 174 status snapshot
(and the lifecycle-state slice that follows it); an embedder that
disables stats has chosen a blind link view, and the contract does not
silently re-enable the walk behind its back.

**Consolidation rider (2026-08-14 review, finding 8 remainder).** The
three generation-carrying RX copy bodies (`copy_scout`,
`copy_selection`, `copy_command`) fold onto one
`copy_generated_snapshot_json()` beside the plain contract in
`snapshot_copy.h`; `GeneratedSnapshot` moves there with it. Behaviour
is bit-identical — the existing runtime-control tests are the proof.

**Evidence.** Branch `feat/r3-telemetry-abi`; publication sites
`node/src/rx_node.cpp` (stats tick) and `node/src/tx_node.cpp` (stats
tick); mailboxes `RxRuntimeControl` / `TxRuntimeInfo`; tests
`rx_runtime_control_test.cpp`, `tx_runtime_info_test.cpp`.

## Pass 175 — recovery is prep + construct, and prep already lives in the library (2026-08-14)

**Ruling (operator-approved plan, coordination
`specs/cross/2026-08-14-wblink-library-parity` R2/R7).** The supported
recovery shape on BOTH roles is **destroy → create → run on a fresh
handle**, in-process or across processes. No in-place `recover()` enters
the ABI: #168 measured fresh-object recovery 5/5 and the in-place
runtime-control path 0/5, and a second `InitWrite` on a live devourer
object terminates the process (#197). The one-handle-one-run contract has
encoded half of this since Phase 3; this pass states the other half — a
fresh create in the same process is *required to work* — in the C headers
where a supervisor author looks.

**Measured: the external prep is redundant on the devourer path.** The
coordination R7 inventory found three prep implementations (the ground
unit's `ExecStartPre` sysfs unbind, the vehicle init's `free_adapter()`,
Android's claim seam). Probed 2026-08-14 on the x86 bench: a node claims
an 8812AU with `rtw_8812au` BOUND — no rmmod, no unbind — because
`claim_interface_then_reset` detaches an active kernel driver before the
claim (`third_party/devourer/src/UsbOpen.cpp:105`, read not edited) and
retries BUSY at 6×250 ms. What `create()` already owns: presence → loud
named failure; bound driver → detach; stale claim → BUSY retry; two
stanzas on one unit → dev_key refusal. The scripts stay (harmless
belt-and-braces) but are no longer part of any embedder contract — an
in-process supervisor needs NO root sysfs help, which is what actually
unblocks the hub's #197 vehicle TX role.

**One residue.** devourer detaches explicitly (not auto-detach), so the
kernel driver stays detached after exit (measured: `Driver=[none]`) until
re-plug or manual rebind — invisible where devourer owns the dongle.

**Changed.** No wire, no §15.x — the recovery-loop contract enters
`rx_node_c.h` / `tx_node_c.h`.

**Evidence.** #168 (5/5 fresh vs 0/5 in-place); #197 (InitWrite
terminate); the 2026-08-14 bench probe (bound driver → `/info` served);
`deploy/vehicle-waybeam-link.init:28-34` and the ground unit's prep
script as the inventoried externals.

## Pass 174 — the TX node states its own status: `wblink_tx_status` + `wblink_tx_adapters` (2026-08-14)

**Ruling (operator-approved plan, coordination
`specs/cross/2026-08-14-wblink-library-parity` R2 slice 2).** A TX embedder
reads the node's state from published snapshots instead of re-deriving
liveness from `run()`'s return timing — the hub's `mod_wblink_running()`
latch exists only because `wblink_tx_run` can return in milliseconds on a
missing radio with nothing in the log. Two C calls on the buffer contract
the RX surface set (0/2/3/4, size query includes NUL, reads stay valid
after stop until destroy):

- **`wblink_tx_adapters`** — the Pass 172 adapters/caps object, published
  at bring-up and republished at ~1 Hz (the object carries the LIVE
  channel — 2026-08-14 review fix). Closes Pass 172's named TX deferral.
- **`wblink_tx_status`** — republished at 1 Hz, independent of `stats.hz`
  (stats-off nodes still inform their embedder), and published ONE FINAL
  TIME on every exit path — the §9.10 wedge exit used to return above the
  publish site, so a supervisor reading the terminal status after
  WBLINK_TX_WEDGED recorded a healthy node (2026-08-14 review fix):
  `{"session":N,"channel":N,"csa":"...","claimed":B,"claimed_by":N,
    "mode":{"active":"...","apply_configured":B},
    "wedge":{"enabled":B,"progress_proven":B,"wedged":B,"consecutive":N,
    "windows":N}}`
  Every field keeps its existing semantics: `mode` mirrors §15.5
  `GET /api/v1/mode`; `wedge` mirrors §9.10 — Pass 170's
  `progress_proven` makes an inert watchdog *visible here*, which was
  that pass's REPORTED-not-silent condition; `claimed`/`claimed_by`
  mirror §11.4; `channel` is the live channel, CSA/retune included.

**Mechanism.** `TxRuntimeInfo` — the snapshot half of the RX mailbox
pattern (publish swaps an immutable string; the caller only copies).
Deliberately no command half: TX runtime *control* is a later pass.
`run_tx` gains the runtime-info overload; the 3-arg symbol stays and
forwards nullptr (the RX precedent).

**Changed.** No wire, no §15.x surface — the C ABI headers and this entry.

**Evidence.** Coordination sweep item 3 + hub feedback; Pass 172's
deferral note; `txwedge.h` accessors (public, never before surfaced).

## Pass 173 — TX gets the device source RX has: `wblink_tx_set_adapter_fds` (2026-08-14)

**Ruling (operator-approved plan, coordination
`specs/cross/2026-08-14-wblink-library-parity` R2 first slice).** The TX C
ABI accepts pre-opened USB fds under the exact contract the RX call set on
2026-08-08: a **call, not a config key**; caller keeps fd ownership
(libusb `fd_keep` — teardown closes the handle, never the fd); supplying
any fd forces the bring-up `libusb_reset_device` off (B4: a wrapped fd
must not be reset); refused after the run has started (3), because the
config is consumed by then and a caller with the order wrong would watch
enumerate-by-bus-path fail on a device it cannot see.

**Changed.** No wire, no §15.x surface — the contract lives in
`tx_node_c.h` beside the RX twin, and this entry. `run_tx`'s fd plumbing
rides the same role-shared `AirBackend::create` path RX uses; nothing
adapter-facing forks by role.

**Why.** The RX call is THE device source unrooted Android has; TX had no
equivalent, so an Android transmit path could never enter the library the
way its receive path does — the asymmetry the coordination sweep named
(R2), now closed one slice at a time. The vehicle daemon case is
unaffected: empty (the default) stays enumerate-by-bus-path, byte for
byte.

**Evidence.** `rx_node_c.h` fd-source contract (2026-08-08 ruling);
coordination sweep memory `waybeam_link_library_sweep_2026_08.md` item 3;
`node/include/wblink/node/air_backend.h` (`adapter_fds` consumed
role-agnostically at create).

## Pass 172 — a capability is an answer, not a log line: §15.5 `/info` carries the per-die caps (2026-08-14)

**Ruling (operator-approved plan, coordination
`specs/cross/2026-08-14-wblink-library-parity` R1).** What a die can and
cannot do is a **stated per-adapter answer** on the library's contract, not
something an embedder scrapes from a bring-up log. `AirIface` grows
`adapter_caps(size_t)` — pure virtual, every backend states its answer —
and ONE builder serves every consumer shape.

**Changed §15.5** (`GET /api/v1/info` `adapters[]` row): each entry gains
`chip`, `power_actuator`, `ldpc_rx_flag`, `fastretune` — static per-die
answers read once at bring-up. `chip` is the chip-generation name (`"udp"`
on the bench backend, other three false); `power_actuator` is §10.5's
`actuator` discriminator as a boolean (Pass 171); `ldpc_rx_flag` is
per-frame LDPC **reporting** (§15.3 Pass 157 — the 8814A decodes LDPC
while reporting none; the 8812A HAS the descriptor bit, measured at first
device verify, correcting the sweep's claim); `fastretune` is the lean
retune override.

**C ABI.** The same JSON reaches control-server-less embedders (Android's
`:wifi` builds `WBLINK_CONTROL_SERVER=OFF`) as `wblink_rx_adapters()` —
published at bring-up and republished at ~1 Hz, because the array also
carries the LIVE channel (CSA, craft-local retunes and scout dwells move
it; a one-shot publish froze it — 2026-08-14 review fix). The caps fields
never change between publishes. TX parity deferred to R2.

**Threading contract, stated with it.** The node spawns no dispatch
thread: `FrameSink`/`wblink_frame_cb` fire on the thread that called
`run_rx`, the mode applier on `run_tx`'s — one thread each, stable for
the life of the run. Both embedders already depend on this (hub
push_frame injection, Android JNI thread-attach); now the headers say it.

**Evidence.** Coordination sweep memory
`waybeam_link_library_sweep_2026_08.md`; `io/src/air_radio.cpp:881-894`
(reads, no re-export); Pass 171's `actuator` field as the shape this
generalizes.

## Pass 171 — an adapter with no power actuator announces and keeps flying, but never reports success (2026-08-14)

**Ruling (operator, 2026-08-14).** A node whose `role:"tx"` adapter has no
TX-power actuator **starts**. It announces the fact once at bring-up, reports
it as a stated value, and refuses every operation that would otherwise return
success for a move the chip cannot make. It does not refuse to fly.

**Changed §10.5** (new bullet, plus the §15.5 rows for `GET`/`POST
/api/v1/tx/power` and `POST /api/v1/calibration`).

**Why.** Pass 169 covers a lever out of travel; this covers one never
connected. devourer answers an unsupported knob with **0**, which is
byte-identical to a successful zero-offset apply, and reports no rail while
having no travel — so `applied_qdb:0, saturated_low:false` reads exactly like
health on the field §10.5 itself calls load-bearing. `GetTxPowerCaps().supported`
is the static per-die discriminator and nothing read it.

**Why the node still flies.** The chip is not at an *unknown* power: devourer
pins the 8733B at `kSafeTssiTargetQdbm8733b` (16 dBm) or the flat safe index.
§10.2's "no adapter may run at an uncharacterised power" is therefore not
breached — reporting integrity was, and that is what this amends. Refusing to
start would ground a board's only link and buy nothing announcing does not.

**Surface.** `actuator` ∈ `offset`|`none` on `GET /api/v1/tx/power`, always
present because omission is already taken (it means "no write yet"); `none`
omits the three Pass 169 fields, 400s a `qdb` POST, and refuses a §10.6/§10.7
calibration start. `{"auto":true}` still succeeds.

**Evidence.** `docs/findings.md` 2026-08-14 — CV610 + RTL8733BU: 18 dB of
commanded offset aired **nothing**, EVM flat at −22 across every rung, every
POST `{"ok":true}`. Matched positive control on the same receiver minutes
earlier: an 8822EU moved 5.6 dB of air for a 6 dB command, then railed with
`saturated_low`. Root cause certain from source — `Rtl8733bDevice` overrides
none of the `IRtlDevice` power family, so the call reaches
`IRtlDevice.h:134`'s `(void)qdb; return 0;` and no register is written.
Upstream tracked at `snokvist/devourer#1`.

**Not taken.** Modelling it as a failed actuator write. `resolve_and_apply_power`
caches only on success, so a `false` would re-attempt the same dead write at
every profile commit for the life of the node.

## Pass 170 — a §9.10 wedge presupposes progress, so the verdict needs proof of it (2026-08-14)

**Ruling (operator, 2026-08-14).** The TX-wedge verdict is withheld until this
backend has been observed completing at least one frame. Until then the
detector reports `unproven` and renders no verdict.

**Changed §9.10.**

**Why.** The rule was "submissions advanced, zero completions → wedged". That
conflates *has never completed* with *has stopped completing*. On a backend
with no CCX report path the first is permanent, so the verdict is a permanent
false alarm.

**Evidence.** 2026-08-14, CV610 + RTL8733BU (`third_party/devourer/src/rtl8733b`
implements no TxReport path — grep finds no CCX site in the family). The
in-process node reached `tx_submitted` 1320 with `tx_reports` 0 and the hub
logged *"tx node WEDGED (§9.10) — the transmitter is dead"*. A second radio
(8812AU) on 5805 simultaneously received **22 087 frames from that adapter,
21 093 delivered at 2 permille loss**, RSSI -26. The transmitter was healthy;
only the evidence channel was absent.

**Cost, stated not hidden.** A backend that wedges before its first completion
is no longer caught — that state is indistinguishable from one that never
reports. `unproven` is therefore reported rather than silent, so an inert
watchdog is visible.

**Not taken.** A per-family capability gate. `AdapterCaps` carries no
TX-report flag, so it would have meant hardcoding a chip family in our code —
brittle, and wrong the moment devourer adds reports for that family. The
proven-progress precondition is family-agnostic and self-correcting.

## Pass 169 — §10.5 reports what the actuator took, not only what was asked (2026-08-14)

**Ruling (operator, 2026-08-14 — #180 items 1-3 authorised, item 4 held).**
`POST /api/v1/tx/power` returning success
means the backend accepted the write. It has never meant the chip moved, and
until now nothing in the response said so. §15.5 `GET /api/v1/tx/power` gains
`applied_qdb`, `saturated_low` and `saturated_high`.

**Changed §10.5, §15.5** (`GET /api/v1/tx/power` row).

**Why.** devourer folds a relative offset into a TXAGC index and clamps it:
`effective = clamp(baseline + steps, 0, index_max)`
(`third_party/devourer/src/TxPower.h:17`). The usable negative travel is
therefore exactly the calibrated baseline index; past that rail every further
step commands the same power. devourer reports both halves —
`SetTxPowerOffsetQdb` returns the APPLIED qdb, and `GetTxPowerState` carries
the rail flags, documented as *"the signal a closed-loop controller uses to
know the knob has run out of travel"* — and `RadioAir::set_power_qdb` discarded
the first with `(void)` while nothing anywhere read the second.

**Evidence.** 2026-08-14, `docs/findings.md`: craft 17 stepped over
`POST /api/v1/tx/power` while two independent receivers watched (8812AU
Jaguar1, 8812CU Jaguar3). Both saw RSSI track ~6-7 dB per 6 dB commanded down
to -12 dB, then stop dead — **18 dB of commanded range moved nothing** and
every write reported success. Two different RX chips agreeing places the rail
at the transmitter.

**Scope.** Reporting only. No caller changes behaviour on the new fields, and
§10.6/§10.7 rung placement is untouched — a calibration that refuses or marks
a railed rung is a behaviour change and wants its own ruling (issue #180).
The fields are omitted before any write and on a backend with no actuator, so
an absent `applied_qdb` can never be read as 0.

## Pass 168 — scout evidence is resolved before it becomes actionable (2026-08-12)

**Ruling (operator, 2026-08-12).** A scout result is actionable only after
channel evidence resolves to one channel. `candidates[]` contains at most one
resolved row per originator; raw per-dwell observations remain diagnostic in
`candidate_sightings[]` and must not be counted as separate craft.

**Resolution rule.** One uncontested channel resolves, including the resting
channel on a fresh install. With evidence on competing channels, a clearly
dominant frame count wins. Near-equal evidence fails closed unless an explicit
prior operator selection already pinned that originator to the resting channel;
an automatic latch is not trust. Invalid rows are not selectable.

**Measurement semantics.** Realtek false-alarm and CCA counters count events,
not busy duration. The legacy occupancy values are therefore named as decoded
Waybeam airtime and a chipset-local ranking/interference score, retain their old
aliases for compatibility, and publish `duty_cycle_known:false`. They must not
be presented as generic RF or 802.11 channel occupancy.

**Changed.** §15.5 endpoint schema and §15.5a candidate resolution,
per-channel evidence, occupancy field semantics, and ranking input.

**Evidence.** Android RTL8812CU/EU validation decoded the same 5805 craft under
multiple dwell labels; RTL8812AU isolated it to 5805 while quiet adjacent bins
still produced high false-alarm scores. Unit mutations cover ambiguous evidence,
trusted-rest resolution, and an uncontested fresh resting-channel result.

## Pass 167 — a tier bounds flight power, not the offset-space sweep (2026-08-09)

**Ruling (operator, 2026-08-09).** In offset space a §11.7 `0x0A` tier does
**not** narrow the §10.6/§10.7 calibration window. It keeps the half it owns —
flight power, via the §10.4 resolve clamp and the §10.5 latch clamp. On an
absolute backend the Pass 134 behaviour is unchanged.

**Measured first, ruled second.** Pass 166 folded the tier into the sweep bound
for symmetry with the absolute backend. On the `.242`↔`.232` bench, fleet tier
1 puts the ceiling at −48 while the window floor is `power_offset_qdb` −24, so
`max < floor`: the ground's uplink run swept **one point**, reported
`state: done` with no `fail_reason`, and **overwrote an artifact holding
`last_clean_qdb: 24`** — a degenerate run claiming success and destroying the
record of measured headroom, the failure `CLAUDE.md`'s "refuse false success"
rule exists to prevent. Numbers in `docs/findings.md`.

**Why remove the coupling rather than guard it.** Calibration is how a unit's
real maximum is found. The §10.5 band is a config-level safety property; a
tier is a session-volatile convenience reachable from a menu. Letting the
second narrow the first means one button press can hide a unit's headroom for
the session and destroy the artifact recording it.

**Changed.** §10.3 (`0x0A` no longer claims the sweep in offset space), §10.7
(the derived window is the §10.5 band, with the measurement as its reason),
§11.7 `0x0A`.

**Consequence for the ladder.** The top preset stays at
`power_offset_max_qdb` (+24 on the fleet). A tier is a ceiling, never a
command, so the top entry radiates nothing by itself — it is the "release the
ceiling" position, and without it no tier restores full flight power once one
is selected. Placements above the efuse reference are real on this fleet
(ground `last_clean_qdb: 24`, craft +14/+10/+4), so a ladder topping at 0
would clamp measured headroom.

**Left open, deliberately.** The ground has no "refuse an empty sweep window"
guard — the craft's `offset_window()` returns nullopt and §11.7 CALIBRATE
gates on it; the ground has no twin. A tier can no longer produce that state,
but `power_offset_max_qdb == power_offset_qdb` still falls to the ABSOLUTE
startup arm, and since Pass 166 the number folded there is an **offset**
(`upwr.ceiling_qdb`), not 108 — so that arm now mixes spaces. Its own pass.

## Pass 166 — the `0x0A` power tier is re-based into offset space (2026-08-09)

**Ruling (operator, 2026-08-09).** Re-base the tier; the top preset is the
configured `power_offset_max_qdb`, not the safe seed.

**Why now.** Pass 151 refused an absolute tier on a relative backend, Pass 165
bound the ground half, and Pass 164 deleted the only absolute RF backend. The
three together left the fleet with **no in-flight power lever at all**: `0x0A`
is the only power command in the §11.7 table, and §10.5's latch is
management-HTTP only — unreachable from a ground with no IP path to the craft.
§10.5 had promised "tiers are re-based with the rest of §10.5"; this is it.

**Changed.** §10.3 (space table, and why deriving was refused), §11.7 `0x0A`
(REJECT condition; wire unchanged — still an ordinal `0..4`), §15.2 (new key,
preset-list scoped by space), §15.5 `GET`/`POST /api/v1/tx/power_tier`. This
Pass also folded the tier into the §10.7 sweep bound; **Pass 167 reverts that
half before either shipped** — softened here rather than corrected on top,
per `CLAUDE.md`.

**The key.** `adapters[].power_offset_presets_qdb` — 1..5 entries, `role:"tx"`
only, each clamped at load to `power_offset_max_qdb`, logged when the clamp
binds. Term for term the absolute rules with `max_power_qdb` →
`power_offset_max_qdb`. A node reads exactly one list; the other is inert on it
(registered, schema- and `--strict`-visible).

**Deriving was refused.** `preset − max_power_qdb` gives `[−48…0]` on both
flying configs, below their configured `power_offset_max_qdb: 24`, so a tier
could never express the operator's own ceiling — and it reintroduces an
absolute anchor into the section that says there is none.

**Pass 165's residual closes structurally**, not by a new guard: the §10.7
artifact resolve always clamped to the node's ceiling, but on a relative node
that ceiling held the absolute `max_power_qdb` (108), so the `min` was a no-op
against offsets. Seeding it from `power_offset_max_qdb` makes it bind.

**Deliberately NOT ruled:** whether `max_power_qdb` is inert on a relative
backend. Its one remaining reader there clamps a list that is itself now
inert, which looks like inertness — but Pass 164's review had to supply that
enumeration after the fact, and a false "key is dead" on a power key is the
worse direction. Live and unpredicated until the retirement pass.

## Pass 165 — the §11.7 `0x0A` relative-backend refusal binds the ground half (2026-08-09)

**Verdict.** Pass 151 ruled a power tier REJECTED when the air backend is
relative: `power_presets_qdb` are absolute qdb and there is no offset-space
tier. Only the **craft** implemented it (`app/main.cpp:3304`). The §15.5
`POST /api/v1/tx/power_tier` ground path applied the tier unconditionally, and
`:7058` folded its absolute `ceiling_qdb` into the **offset-space** §10.7 sweep
bound — overwriting the window the same function derives correctly at startup
(`:5769`, which comments that the absolute ceiling "is deliberately not folded
in"). The ground now refuses identically, **before** the tier is recorded.

**The chain, corrected in pre-merge review.** The sweep's own actuation is
clamped to `power_offset_max_qdb` (`:7500`), so a moved bound does **not** make
the run radiate above §10.5 — this Pass's first cut said it did. The hazard is
one step later: every step above the bound commands the same clamped power, the
seek reads flat, and it persists a **placement** as high as the preset.
`uplink_artifact_qdb` clamps a stored placement only by the absolute
`ceiling_qdb` (`:5935`, a no-op at 108), and `upwr.apply_qdb` (`:5993`) applies
it with **no `power_offset_max_qdb` clamp** — so it reaches the chip on the next
apply, pairing pass or boot.

**Reachable on the fleet.** `deploy/ground-192.168.2.242.json` carries
`power_presets_qdb: [60,76,84,92,108]` and `power_offset_max_qdb: 24`, so one
`POST {"tier":4}` opened that chain toward 108 qdb of offset (**+27 dB**)
against the intended +24 (+6 dB) — the point Pass 150 measured driving an
8822EU into compression at 54 ‰ loss against 6 ‰ two dB below. No calibration
run had exercised the sequence, which is why it survived Passes 151–164.

**Changed.** §11.7 `0x0A` (the refusal binds both halves; `0x0A` and its REST
twin are reachable only on the udp bench since Pass 164, and §10.5
`POST /api/v1/tx/power` is the actuation path on a relative node); §15.5
`POST /api/v1/tx/power_tier` gains the 409.

**Named, not fixed.** The artifact apply stays unclamped by
`power_offset_max_qdb` (with the bound pinned, nothing can author a placement
above it); the §10.7 placement clamp is a no-op only while that bound is small,
since the key validates to ±511; re-basing presets into offset space needs its
own ruling. Found by adversarial pre-merge review, twice — the first pass found
the gap, the second corrected the mechanism.

## Pass 164 — kernel-monitor is deleted; devourer is the only RF backend (2026-08-08)

**Verdict.** Ruling #120 item 3 had `MonAir` *moved* to an external RX-only
repo at the library split, as its second proving consumer. The operator ruled
**DROP** instead: the backend's last archetype is a spectator, and a devourer
spectator with **zero `role:"tx"` adapters** delivered 19466 frames, uniq
19466, diversity 19430, loss 0 ‰, both dies EFUSE-autoloaded. Both flying nodes
migrated to devourer 2026-08-06; only the repo was stale. `air.kind
"kernel-monitor"` is now rejected by the loader. Nothing on air changes — the
backend was never wire-visible — so no §3.1 version event.

**Changed.** §3.0 catalog (`air.kind` ∈ `udp | udp-broadcast | radio`, plus a
retirement paragraph); §10.5 matrix loses the `iw` absolute row and
`max_power_qdb`'s *reference* role; §10.6/§10.7 identity tiers 2–4
(`id/monitor/`, `ifname/<MAC>`, `bus/`) retired, `mac/<efuse-mac>` stands
alone; §15.5 `GET /api/v1/tx/power` `backend` ∈ `radio | udp`; §14.2
`airtime_efficiency_permille` radio-only; §11.6 recovery is the devourer
RX-path restart.

**Stranded keys — two, inert not retired.** `adapters[].ifname` and
`adapters[].calib_id` lose their last reader. Both still load; `--check
--strict` reports them **inert**. Retiring them is a separate pass.

> **Corrected before merge.** The first cut also called `max_power_qdb` and
> `power_presets_qdb` inert. False — §15.3 `tx_power_ceiling_qdb`
> (`app/main.cpp:3186`), §15.5 `GET /api/v1/tx/power_tier`, the ground
> `UplinkPower::hw_qdb()` override clamp (`:1197`) and the §10.7 sweep bound
> (`:5935`) read them on radio, and both are set on both flying configs:
> `--strict` would have told the operator to delete a key that clamps TX power.
> Predicate dropped and pinned. The same trace found the ground gap **Pass 165**
> closes.

**Deploy.** `.199` and `.247` deleted, not migrated: both offline, so their real
state could not be read, and `gates.sh` `--check`s every `deploy/*.json`.
Inventing content would ship configs no node ever loaded. Re-author when
powered. §15.3 `bpf_filtered` keeps its slot at a permanent 0 (schema
stability). #109 loses the split's second proving consumer; Phase 3 now rests
on Android alone, and B6's `air_mon.cpp` leg closes by deletion.

## Pass 163 — sequence-derived rate probing: the §9.2 numerator closed, probe as veto (2026-08-08)

**Verdict.** Issue #101's code stages, on two operator rulings: the probe
changes the **MCS and nothing else** (2026-08-06 — `probe_per` is RATE
headroom, not profile headroom) and the schedule is **up-candidate only**
(2026-08-08 — no down-slot; downshift stays loss-driven; the up-candidate's
PER is what `probe_per` reports, settling the §3.5 reporting shape without
widening the field). A first-send video DATA frame with
`seq % probe_period == probe_slot` flies the next ascending-id profile's
`mcs` at the current rung's power/GI/payload; both ends derive the probe
set from `seq` alone. Rate a pure function of seq closes §9.2's open
numerator: a missing probe-slot seq's rate is known by computation.
Evidence is a **veto, never a warrant** — fresh `probe_per ≥
probe_veto_permille` suppresses both climb paths (Pass 160 shape); nothing
promotes on probe evidence alone. Receiver window enforces four guards:
rate-verified successes, epoch-gated gap losses, CRC-errored frames
attribute rate-free (descriptor rate is pre-FCS) and seq-free, and — third
ruling, 2026-08-08, from the pre-merge review — **failure-only evidence
never reports**: at least one direct candidate-rate observation (success or
CRC-verified failure) is required, or a non-probing TX on the fleet-shared
schedule would manufacture a phantom veto from ordinary air loss. Any
operating-context change resets the window (TX-side too: rung transitions
clear the selector's probe evidence). Enablement is fail-closed:
`air.mcs_probe` default off, radio-only, per-unit stage-0 proof required
(findings.md 2026-08-08); RX side needs no knob — rate-verification makes
one-sided enablement inert stats.

**Changed.** §3.5 `probe_per` row (up-candidate rate PER); §3.6 canonical
form appends `probe_period`/`probe_slot` u16s after `floor_profile` — a
deliberate fleet-lockstep `table_version` rotation, vendored golden hashes
recompute; §9.2 heading + numerator note + ceiling wording; §9.4 rewritten
(probe subsections replace the deferred "v1 active probe"); §15.2
`air.mcs_probe`, `policy.select.probe_veto_permille`/`probe_veto_ttl_s`;
§15.3 `promote_blocked_probe`; §17 probe schedule/window seed row.

**Evidence.** Stage-0 premise device-proven on all three fleet dies,
per-frame rate-verified with CCX cross-check, retry walk dormant on
broadcast (findings.md 2026-08-08; issue #101). Window guards and veto
semantics from devourer `rc_proto.py`/`score.py` (constants re-seeded as
§17 RE-DERIVE, not copied).

## Pass 162 — RX-only devourer bring-up: spectator/cache leave kernel-monitor (2026-08-08)

**Verdict.** B2 of ruling #120 (devourer sole in-tree backend): `RadioAir`
bring-up accepts zero `role:"tx"` adapters for the two uplink-free
archetypes — dedicated cache with no media streams, §2 spectator (Pass 74)
— the same gate kernel-monitor has carried since Pass 74
(`allow_rx_only`, derived from the archetype, not a config key). All other
node shapes keep the one-designated-uplink requirement; more than one
`role:"tx"` stays a config error everywhere (§6.4). `has_tx()` becomes
truthful on the devourer backend, so the Pass 143 heartbeat guard now
fires there too — the §3.11 note claiming it "cannot fire on devourer" is
struck. Fail-closed corollary (Pass 156/157 posture): `air.ack_responder`,
`policy.return.unicast`, `air.ldpc`, `air.stbc` each name a TX-die
property, so setting any of them on an RX-only adapter set refuses create
instead of running silently inert. Send paths return 0, TX counters stay
0, `tx_index()` is defined only under `has_tx()` (callers keep the guard —
the §15.5a scout on an uplink-free node roams adapter 0, unchanged from
kernel-monitor's RX-only behaviour).

**Changed.** §3.11 (RX-only bring-up block replaces the devourer
cannot-fire note).

**Evidence.** Branch `impl/rx-only-radioair`; ruling #120 item 2 (B2
promoted onto the queue; cache/spectator migrate to devourer);
kernel-monitor template `io/src/air_mon.cpp` (`allow_rx_only`, guarded
`send_frame`, `tx_report_counters`); archetype expression
`app/main.cpp` AirBackend::create (monitor branch, now shared).

## Pass 161 — scout evidence accumulates; ranking gains hysteresis, confidence, reasons (2026-08-07)

**Verdict.** Issue #100 (downstream of #95; the passive-scout-adapter role
stays DECLINED per the 2026-08-06 operator ruling — cross-sweep
accumulation is the settled shape, not an interim one). Four chanmig ideas
adopted as *shapes* with our values as §17 seeds, their engine not
adopted: (1) sweeps FOLD into per-bin evidence rings (8 samples, age
bound 15 min) instead of replacing; rounds advance only when every bin
has fresh evidence (anti-starvation), and low rounds degrade confidence,
never suppress the answer. (2) Fold trust boundary: per-adapter
calibration-domain key (EFUSE MAC else index) — a foreign domain RESETS
the store; implausible samples rejected; both counted by reason.
(3) Hysteresis: qualify bar (200‰) + recommend margin (80‰ over the
resting channel) + broad-degradation hold (≥700‰ bins unqualified ⇒ the
interference is not channel-attributable) + deterministic lowest-MHz
tie-break. (4) One classifier, one owner: a fresh §3.16 `Weak`/
`Saturated` verdict on the active link REFUSES the recommendation with
that reason (range/self-jam is not the channel's fault) — the scout
consumes #98's verdict rather than growing a second classifier.
Explainability is the acceptance bar: enumerated reasons, never prose.

**Changed sections.** §15.5a (evidence store, ranking, trust boundary,
verdict reuse, results shape); §15.5 endpoint table (`scout/results`).

**Evidence.** Issue #100 (chanmig analysis, adaptation table);
`docs/devourer-integration-analysis.md` §1 option B; Pass 155 occupancy
fields (the evidence being ranked); Pass 159 verdict (the classifier).

## Pass 160 — §9.4 saturation gate: a fresh Saturated verdict suppresses every climb (2026-08-07)

**Verdict.** Issue #98 stage 3 (operator-approved 2026-08-07). The
saturation flap — strong RSSI reads as headroom, promote, EVM collapses,
loss, demote — is invisible to every selector input and its correct
response inverts (power *off*). With the cause now on the wire (Pass 159),
the selector consumes it: a fresh `Saturated` suppresses **both** climbing
paths (§9.4 RSSI-margin promote AND the backpressure escape — gating one
reroutes the flap). `Unknown`/stale gates nothing (absence of evidence —
kernel-monitor fleets unchanged by construction); demote paths never
consult the verdict (§9.0). Freshness knob `policy.select.verdict_ttl_s`
seed 3.0. Suppressions counted (§15.3 `promote_blocked_saturated`).

**Changed sections.** §9.4 (saturation gate bullet + residuals: the §10
power-down actuation and Interference/Weak consumption are their own
future rulings); §15.3 (`promote_blocked_saturated`).

**Evidence.** devourer saturation-knee sweep (issue #98); Pass 159 wire;
selector rules 5/6 in `core/src/selector.cpp`.

## Pass 159 — LINK_VERDICT: the cause crosses the wire as §3.16 0x03, not a version event (2026-08-07)

**Verdict.** Issue #98 stage 2, shape 2 (operator-approved 2026-08-07):
one **cause byte**, never a vendor scalar. The issue's pricing (§3.5
append + §3.1 version event) predates Pass 153 — the EXTENDED registry is
the additive path, an old craft ignores ID `0x03` by law, and mixed
fleets need no flag-day. 23-byte addressed frame: target tuple +
`report_epoch` (monotone, ties the verdict to the report stream) + verdict
`0..6` (Unknown/NoSignal/Saturated/Interference/Weak/Marginal/Healthy).
Computed at the drain from the **best-peak ear** (the ear `rssi_best`
describes — the verdict and the RSSI series must describe the same thing)
via vendored `classify_link_health`, frame-metric legs only (energy/IGI
stay with the scout, Pass 155). Emitted ≤1 Hz alongside reports, only
while reports flow; craft acceptance = the §3.5 latch filter + epoch
monotone. Pass 158's "stats emitter drains" wording is amended: the drain
moves inside the backend (≥1 s guard), shared by the stats line and the
classification — a second consumer can never split the delta.

**Changed sections.** §3.16 (registry `0x03` + LINK_VERDICT layout and
semantics); §15.3 (quality-window drain wording; `link.verdict` /
`verdict_age_ms` role-dependent views).

**Evidence.** Pass 153 registry design notes (§3.16); `LinkHealth.h`
thresholds + `classify_link_health` (pure, self-tested);
Pass 158 window.

## Pass 158 — §15.3 quality window: harvest the SNR/EVM the silicon already hands us (2026-08-07)

**Verdict.** Issue #98 stage 1 (observation only — stages 2/3, the §3.5
wire carriage and §9.4 saturation gate, are their own future rulings; the
sensor is shared with issue #125's calibration observability leg).
`RxAtrib` delivers per-frame per-chain RSSI/SNR/EVM and `on_packet` kept
RSSI alone, discarding the two scalars that distinguish "strong but dirty"
(front-end saturation — EVM reverses while RSSI climbs and SNR sits flat)
from "weak and dirty". Harvest them per adapter with devourer's own
folding (`RxQualityAccumulator` reused whole, not reimplemented): path-A
raw, `rssi_raw <= 0` skipped, EVM folded only when present (and the −128
no-stream rail discarded at the feed — one railed sample per window would
read *impossibly clean* and mask the very knee this exposes), passive
noise floor `(rssi_raw − 110) − snr_raw/2`, window PEAK for RSSI. Known
accumulator asymmetry, pinned in §15.3: SNR is folded over all
RSSI-carrying frames (no presence guard), so `snr: 0` is ambiguous. This
deliberately does NOT touch `GetRxQuality()` — the scout owns that
device-side window and its FA/CCA delta (Pass 155 exclusivity); the
per-frame fold is our own accumulator over our own delivered frames, so
there is no shared-delta owner question.

**Changed sections.** §15.3: radio-backend `rssi_best`/`rssi_mean`/
`snr`/`noise` become real windowed values (peak / mean / dB / passive
floor; previously last-frame RSSI and constant 0); new `evm` +
`evm_valid` fields (mean dB, lower better; valid=false means no data, not
perfect coding); delta-window semantics, stats emitter is the single
reader; empty-window and non-radio fallbacks pinned.

**Evidence.** Issue #98 (devourer saturation-knee sweep: RSSI ↑, SNR flat
18 dB, EVM −28 → −13 dB); `third_party/devourer/src/RxQuality.h`
(fold conventions), `LinkHealth.h:18-20` (units: PWDB dBm ≈ raw−110,
SNR/EVM signed half-dB); `RxPacket.h:57-66` (per-chain arrays).

## Pass 157 — TX coding becomes commandable: air.ldpc / air.stbc, RX-proved (2026-08-07)

**Verdict.** Issue #97. The radiotap MCS known mask has always claimed FEC
and STBC known while leaving both flag bits clear — every frame we air
affirmatively commands BCC and zero STBC streams. The two bits become
config: `air.ldpc` (FEC = LDPC) and `air.stbc` (stream count 1), **both
default off** — the enable is a future ruling on cliff-A/B evidence, this
pass is the mechanism plus its proof surface. Two knobs, not one: LDPC is
coding gain on one stream, STBC is diversity across two chains; separate
arms. Ruling #120 reframes the issue's open kernel-monitor strip question:
the backend is frozen, the knobs are refused there (Pass 154 mac posture),
devourer-only.

**Changed sections.**
- §3.0 (rate-mechanism block): coding paragraph — per-packet mechanism,
  node-wide policy (calibration probes inherit the node coding, so delivery
  walls are per-coding); Pass 156 capability leg on `TxCaps.ldpc_ok` /
  `stbc_ok`; fleet-decision warning (a TX cannot see a remote ear's caps —
  a non-decoding receiver reads an LDPC arm as pure loss; fleet
  `ldpc_rx_ht` is true on all three dies); proof-over-inference via
  devourer `RxAtrib.ldpc`/`stbc` on `ldpc_rx_flag` dies; default-on flip is
  a §9.3/§17 RE-DERIVE trigger.
- §15.2: the two keys, radio-backend-only (refused elsewhere), defaults
  off, sample updated.
- §15.3: per-adapter `rx_ldpc` / `rx_stbc` counters + static
  `ldpc_flag_ok` (a zero counter means nothing on a flag-incapable die —
  the 8814A decodes but cannot report). Advisory like `rx_mcs`.

**Evidence.** Issue #97 (fleet caps table, measurement caveats);
`io/include/wblink/radiotap.h:41` (the known mask);
`third_party/devourer/src/RadiotapTxFlags.h` (send-path decode, all three
jaguar generations); `AdapterCaps.h:128–139` (ldpc_rx trio),
`TxCaps.h:20–36`; LDPC ≈ +3 dB at the 10 %-delivery crossing, MCS7/20 MHz
(devourer `tests/ldpc_waterfall.sh`).

## Pass 156 — hardware-ACK hybrid: responder and retry limit are one decision (2026-08-07)

**Verdict.** Issue #96. devourer #354 moved the TX retry-limit default to 0
(WFB posture), and `RadioAir` never set `dc.tx.retry_limit` — so the armed
§3.0 Pass 12 hybrid was a one-ended ARQ loop: a peer that ACKs correctly and
a sender that never retransmits, silently inert. The responder knob and the
retry knob are one decision, not two independent defaults.

**Changed sections.**
- §3.0 (Pass 12 hybrid): the stale "descriptor retry limit 12" claim
  corrected; retries are `air.tx_retry_limit`, and the coupling is law — on
  the radio backend `return.unicast` or `air.ack_responder` with
  `tx_retry_limit: 0` is a config validation error, never an inert run.
  Two inherited devourer behaviours pinned load-bearing: RX-pool posture
  stays `backpressure` (`drop` produces ACKed-but-undelivered — the loop's
  one way to lie), and a Jaguar3 retry may fall down the rate ladder
  (`MCS3 ×4 → MCS2 → 6M ×4` witnessed), so no airtime accounting assumes a
  retry flew at the commanded rung.
- §15.2: `air.tx_retry_limit`, default **8** (operator-ruled 2026-08-07),
  range 0–63. The ruling point: devourer's sweep gives 8 → 99.97 %
  delivered where airtime is precious and a FEC floor exists (§14 runs
  one); 16 buys the last 0.03 % at +5.4 % retry airtime. Inert for
  broadcast, so the default costs nothing on a broadcast-only node.

**Out of scope, recorded so it stays closed:** A-MPDU stays rejected — the
+30 % headline is a high-MCS broadcast shape; at this hybrid's exact shape
(MCS3, 512 B, ACK+retry 8) aggregation measured −8 %, suppresses ~60 % of
SPE_RPT (blinding the §9.10 wedge sensor and the retry distribution), and
paces launches 0.8–3 ms in front of the §7.2 quiet-gap design (issue #96
evidence at vendored `800c3c8`).

**Review addendum (pre-merge, same pass).** The config coupling closed the
config-value hole but not the die-capability hole: devourer keeps the
vendor `DATA_RETRY_LIMIT` carve-out where `caps.tx_retry_limit_ok = false`
(8814A; 8821C false-as-unmeasured) — the descriptor write is silently
skipped and a validated nonzero limit still never retransmits. §3.0 gains
the capability leg: `return.unicast` refuses bring-up when the resolved TX
unit (post §15.2 re-bind) reports the cap false. `ack_responder` is
deliberately not caps-gated — `SetAckResponder` refusal is loud at arm time
and the run degrades (returns still received via broadcast RX), while the
skipped retry field has no signal at all. §15.2 also records Kestrel's
attempts-counting WD field: devourer folds the +1, effective ceiling 62,
authored 63 runs 62 with a device-log note. No fleet impact: AU (8812A),
CU (8812C/jaguar3), EU (8822E) all read the cap true.

**Evidence.** Issue #96 (devourer sweep table, per-die responder rates);
`third_party/devourer/src/DeviceConfig.h` retry_limit doc; witness rate
ladder in the issue notes; capability rows
`third_party/devourer/src/jaguar1/RtlJaguarDevice.cpp:1807` /
`jaguar2/RtlJaguar2Device.cpp:1138` / `jaguar3/RtlJaguar3Device.cpp:1541` /
`kestrel/RtlKestrelDevice.cpp:829` (clamp at :81–86, :1122–1124).

## Pass 155 — §15.5a occupancy: frame-free fields become real, ranking follows (2026-08-07)

**Verdict.** Issue #95 implemented. The scout's occupancy was fed exclusively
by decoded waybeam frames, so `emptiest()` ranked channels by how much of
*our own* traffic they carried — a channel saturated by a non-decodable
emitter scored pristine and maximally attractive. The reserved §15.5a fields
become real frame-free measurements on the radio backend, and the ranking
input moves to the interference-inclusive total (filling the fields alone
would have left the defect intact).

**Changed sections (§15.5a occupancy block).**
- `interference_util_permille`: saturating index `1000·r/(r+H)` of the
  frame-free false-alarm rate over the observe window (H = 200 FA/s Tier-2
  seed — the form the vendored chanmig scorer proved on-air). An index
  comparable within one adapter, not an absolute duty cycle. `null` on
  sensor-less backends.
- `noise_dbm`: absolute idle floor where the generation provides it, else
  the passive `rssi − snr` floor, else the v1 min-RSSI proxy (labeled).
- `util_permille`: total occupancy `min(1000, wifi_util + interference)`;
  equals `wifi_util` on sensor-less backends by construction.
- `emptiest()` ranks on `util_permille` — the structural fallback covers
  kernel-monitor (frozen, #120) without a special case.
- Dwell hygiene: retune → settle → discard barrier (throwaway delta drain)
  → observe → read; interference denominator is the observe window.
- NHM excluded from reported fields (generation-dependent floor); it may
  inform the #100 scoring layer only. Nullable convention restated.
- Struck the false clause that the candidate craft's own traffic is
  excluded from its channel's counts — exclusion is per adapter (Pass 65),
  and the accounting never excluded the craft.

**Evidence.** Issue #95 (the blind-metric chain, symbol-cited); vendored
chanmig `ChannelScore.cpp` `cell_occupancy` (fa_half model, NHM exclusion
rationale); `docs/scout-design.md` §6 field reservation.

## Pass 154 — EFUSE-MAC adapter identity; calibration binds to the unit (2026-08-07)

**Verdict.** Per-adapter calibration binds to the per-unit EFUSE MAC, not the
USB bus path (operator rulings D1–D3, issue #118, 2026-08-07; unblocked by
vendored devourer #383 `GetPermanentMacAddress` + #384 Jaguar3 EFUSE-walk
fix). A USB path identifies a *port*, not a device — swapping two dongles
silently applied each other's absolute qdb curve to the wrong PA, with no
regulatory clamp behind it (§10.3).

- **D1** — new optional `adapters[].mac`; stanza match precedence
  **`mac` > `bus` > first-free**, `bus` kept as an explicit port pin. The
  §10.6/§10.7 artifacts anchor on the same MAC.
- **D2** — **fail closed + safe offset**: an identity that does not match
  never applies a `power_map`/artifact; the adapter still comes up at the
  §10.5 safe boot offset (−24 qdb) with a loud log — flyable, curve withheld.
- **D3** — a unit reporting no identity (`GetPermanentMacAddress` → false)
  is refused an absolute curve outright; no declared/bus fallback tier on
  the radio backend (no dual code path). Upstream Jaguar2 wiring is
  hardware-gated (no unit on the bench); upstream independently landed #386
  meanwhile — moot for our builds (family compiled out).

**Changed sections.**
- §10.6 identity block: radio resolution collapses to the single derived
  tier `mac/<efuse-mac>` (Pass 146's 3-tier order survives only on
  kernel-monitor, frozen per the backend ruling, issue #120);
  `id/radio/<calib_id>` and `bus/…` retired on radio — artifacts keyed to
  them read STALE, a re-run re-keys. "devourer cannot reach it" paragraph
  replaced: the upstream request is fulfilled.
- §10.2: "per physical adapter" binds by unit identity, never USB position.
- §15.2: `adapters[].mac` key (format, post-bring-up binding, D2 fallback,
  duplicate/backend rejection); example updated.
- §15.5: `GET /api/v1/info` `adapters[]` gains `mac` (null = none).

**Evidence.** Issue #118 (rulings + measured serial-placeholder/bus-shuffle
record); coordination memory `devourer_efuse_walk_and_mac_identity` (vendor
offset 0x157, rfe 0→3 seven-register delta); `third_party/README.md`
provenance `5a5dd62`; CU re-baseline in `docs/findings.md` (2026-08-07).

## Pass 153 — 0xF EXTENDED type + calibration v2 probe exchange (2026-08-07)

**Verdict.** Calibration v2 adopted as spec
(`docs/calibration-v2-symmetric-probes.md`, operator rulings Q1–Q3 2026-08-06,
D-A/D-C 2026-08-07, EXTENDED generalization 2026-08-07). Type `0xF` becomes
the **EXTENDED type**: first payload byte = extended type ID registry
(`0x00` reserved-invalid, `0x01` CAL_PROBE, `0x02` CAL_TALLY, `0x03`–`0xFF`
unassigned; unknown IDs ignored). The version nibble is reserved for
breaking changes only — additive growth goes through the registry. The
§3.16 UPLINK_QUALITY cumulative-counter layout is retired. One dwell
primitive, both directions: N MTU-padded CAL_PROBE frames at `(rung, qdb)`,
one per-dwell CAL_TALLY back, self-denominating loss. Video pauses for the
run (input-starve), restored on the rate/power restore edge.

**Changed sections.**
- §3.1: type table `0xF UPLINK_QUALITY` → `0xF EXTENDED`; version nibble
  reserved for breaking changes.
- §3.16: rewritten — the type-ID registry, then CAL_PROBE (22B fixed + pad
  to `mtu_effective`, range-length) and CAL_TALLY (26B exact; carries
  `rx_mcs` + `adapter_fingerprint` — D-A ruling keeps the delivered-rung
  cross-check and evidence identity gate). Unknown ID = ignorable, not a
  decode error; `0x00` = reserved-invalid. Craft feed pause on first
  accepted CAL_PROBE of a new run, resume on probe-quiet timeout (D-C
  ruling — no VCMD). FEC/ARQ exemption stated structurally.
- §10.6: evidence = per-dwell tallies; Pass 134 report-health precondition
  deleted (self-denominating evidence cannot author false-clean); blackout
  rules keep addendum semantics with `evidence_lost` trigger; feed-pause and
  2 Hz unconditional §3.15-word emission while paused.
- §10.7: Pass 125/126/128/132 counting apparatus withdrawn whole; walls
  absolute again (contention floor structurally zero — closes the wall-origin
  question, `docs/findings.md` entries struck); **single rung again**
  (reverts Pass 131's widening; artifact list shape unchanged, loader accepts
  any length); no feedback-freshness precondition.
- §3.15: the word-acceptance tuple **latches** — once accepted from a
  live-consumed RTP `(originator,session)`, the tuple survives that stream's
  §2 idle teardown until a different tuple's live stream replaces it.
  Without this the §3.16 pause-emission clause is unreceivable: the pause
  starves the stream past teardown, the ground refuses every mid-run word,
  and the §10.7 sequencer falsely fails `downlink_no_ack` (found on the
  2026-08-07 hardware bench; craft-side runs completed correctly throughout).
- §10.6/§10.7: the Pass 134 flat-at-ceiling `no_wall_found` refusal is
  scoped to **absolute space**. In offset space (Pass 151 backends) a flat
  window completes and persists (operator-ruled 2026-08-07 after six
  consecutive refusals hard-blocked the sequencer at 10 m). The window may
  extend above offset 0 (second ruling, same day: 0 is the efuse
  reference, not a proven per-unit maximum), with one placement cap: a run
  that booked **no overload bracket places no higher than offset 0** —
  above the reference sits per-unit PA compression a close-range flat
  field cannot see, so an unbracketed best there is noise, not evidence.
  A bracketed sweep places below its wall as measured.
- §15.2: dwell knobs `dwell_probe_frames`/`dwell_verify_frames`/
  `probe_pace_us`/`tally_wait_ms`/`tally_retries`/`feed_quiet_ms` (Tier-2
  seeds); nine keys retired. §15.3: `uplink_quality_*` (6 fields) →
  `calib_probes_sent`/`calib_tallies_rx`/`calib_rx_mcs`/`feed_paused`.
- §11.7 `0x08` row, §11 trust note, threat-model row updated to the family.

**Evidence.** `docs/findings.md` 2026-08-06 (report-loss under-powered:
σ≈19‰ at the 80‰ baseline, n≈1500 needed), 2026-08-07 (floor mechanism);
archived Pass 152 field addendum; design doc §1 churn record (~14 reversals
traced to probe-is-payload + cumulative counters).
