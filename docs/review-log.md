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

## Pass 198 — §3.7 loss gets a live view; the cumulative pair is a session average (2026-08-30)

**Verdict.** Operator report: the OSD **AIR** bar sits full while the picture is
clean, and "degrades very slowly and is not live". Both halves are real and
neither is a wrong binding. §3.7's two ratios are computed from totals since
latch, so inertia grows without bound — measured, pre-diversity moved
**33.6 -> 33.4 across 90 s and 88,000 delivered frames**. Worse, a §15.5a scout
sweep parks one ear off-channel and bakes a 100%-loss stretch into the mean for
the rest of the session, which is enough to pin a loss bar at alert on a link
delivering cleanly. §15.3 now also publishes both ratios over a short trailing
window. Second finding recorded in the same amendment: the pre-diversity figure
sums opportunities across adapters, so it is the **mean per-ear** loss — a
two-ear ground measured **350 permille pre against 0 post**. The two answer
different questions and a consumer must label which it is showing.

**Changed:** §3.7 (the cumulative pair is explicitly a session average; a
receiver MUST also publish the windowed pair; empty denominator holds rather
than reads zero; pre-diversity is mean-per-ear and must be labelled as such),
§15.3 (`loss_prediversity_window_milli`, `loss_postdiv_window_milli`,
`loss_best_ear_window_milli` — ears windowed separately, minimum taken). No
wire change and no packet field — both ends compute locally, and the window length
is a §17 seed (500 ms), not a constant either peer depends on.

**Why a window and not an EWMA.** A window forgets completely where an EWMA
trails a tail past the event — the exact complaint. Anchored on the oldest
sample still inside it rather than a fixed sample count, because the emit
cadence is config (`stats.hz`) and a stalled emitter must not silently widen
the span it reports.

**What the tests pin.** `tests/loss_window_test.cpp` asserts the forgetting,
and every case also asserts the cumulative ratio would have answered
DIFFERENTLY at that instant — without that they would pass against the average
they replace. Mutation-verified: disabling the aging reports 800 permille where
the window reports 0, and 71 where it reports 500.

**Device result.** AIR bound to the best ear reads **0.0-1.6%, moving every
sample**, against a mean-per-ear of ~49% and a session average frozen at 48.2%
on the same link at the same instant.

**Evidence.** Branch `impl/live-loss-window`; `docs/findings.md` 2026-08-30
entry for the 90-second cumulative-drift measurement and the two-ear split.

## Pass 197 — §11.2 quick-connect was issuing the rejected retune class; §3.4 gets a voice (2026-08-30)

**Verdict.** Two silent failures, one operator-visible symptom. (1) §11.2 Pass 91
recorded that class-1 campaigns lose the §11.6 race and raised class 0 to 300 ms
*instead of* using class 1 — but that ruling reached no code:
`/api/v1/scout/quickconnect` hardcoded `retune_class = 1`, justified by slack for
an `iw` shell-out that stopped existing when Pass 164 deleted kernel-monitor. The
one path whose job is to connect to a craft was the one running the rejected
class, which is why the OSD channel jumps worked and quick-connect did not.
Re-measured on same-channel campaigns, class the only variable: **class 0
confirmed 20/20, class 1 reverted 8/20.** (2) §3.4's best-effort fallback was
unobservable: a table mismatch suspends ARQ eligibility, supersession and
deadline drops, and the counters that would show it read 0 — like a healthy link.

**Changed:** §11.2 (every issuer campaign uses class 0 unless the move crosses
bands; the pre-position silence is the exposure, and `rx_liveness_ms` counts it
while `verify_timeout_ms` does not), §11.6 (a revert MUST name which half failed
— `armed_seen`/`landing_seen`/`video_seen`), §3.4 (the fallback MUST be
observable, and is sticky until re-latch), §15.2 (`streams[].originator` is a
boot pin a reverting claim can never escape; the config summary prints it),
§15.3 (per-stream `best_effort`, `table_mismatch`), §15.5 (`arq_effective`
beside `arq_enabled`; `csa.channel_allowlist` + `home_chan` published). No wire
change, no packet field added, no timing default moved —
`kCsaVerifyTimeoutMsDefault` stays 500 and `< rx_liveness_ms` is untouched.

**Why the class was the bug and the window was not.** The obvious reading was
that 500 ms is too short for an 8733B craft and a one-eared ground, and the
obvious fix was re-deriving the seed — which drags the `< rx_liveness_ms`
invariant with it. The measurement refused that: at a *fixed* window, changing
only the class took the failure rate from 40% to 0. Class 1 adds
`T_switch - commit` — ~400 ms of pre-position silence against ~200 — which sits
outside the verify window but inside the liveness guard.

**Why it took hours to find.** A revert lands on `prev_chan`; when that is a
`home_chan` no craft occupies, the silence trips the liveness guard into a
backend re-init — a few frames, then nothing. Every layer reported success: the
claim returned `ok`, the craft sat in COMMITTED, `/features` said ARQ enabled.

**Evidence.** Branch `impl/auto-adapters` (PR #256); `docs/findings.md`
2026-08-30 entry for the A/B table and the §3.4 measurement.

## Pass 196 — §9.4 probing is per-DIE, and the fleet dies are licensed (2026-08-30)

**Verdict.** The `air.mcs_probe` enablement stops being per-unit. Operator
ruling 2026-08-30: enable it fleet-wide and withdraw the reservations against
it. §15.2's Pass-195 refusal of `air.mcs_probe` + `adapters.auto` goes with
them — the two now compose.

**Changed:** §9.4 (enablement clause AMENDED; two "per-unit" phrasings in the
observability and guard-4 prose corrected to per-node), §15.2 (the refusal
paragraph replaced), `docs/findings.md` 2026-08-08 (its "Open: per-unit
coverage" clause struck and closed), `deploy/vehicle-192.168.2.232.json`
(the "do not copy without its own proof" note withdrawn). No wire change, no
default changed: probing is still off unless a config asks for it, still
radio-only, still TX-node-only.

**Why the reservation did not survive contact with the evidence.** The gated
property is whether the silicon honours the per-packet commanded rate in the
TX descriptor. That is a property of the die and its HAL path — there is no
per-dongle state it could depend on — and findings.md 2026-08-08 measured it
that way and passed on every die present: AU→CU 3600/3600 with an EMPTY
mismatch matrix, CU→AU 3590/3590, EU→dual ears 3593/3593 + 3600/3600, and
`tx_reports == tx_submitted` exactly across ~11k broadcast frames.

The per-unit framing came from Pass 139's scar — one defective dongle once
carried an entire architectural posture — and was applied to a measurement it
did not fit. Pass 139's lesson is real and stays: two adapters of one part are
not a replicate. It earns its keep where the quantity is per-unit (a power
curve, an EFUSE table, a compressing PA). A logic path either exists in the
HAL or it does not.

**What the caution actually cost.** Three things, all found while implementing
§15.2 auto:

1. **Nobody enforced it.** `node/src/tx_node.cpp` arms the probe on
   `l.cfg.air.mcs_probe` alone. Nothing compares the bound unit's EFUSE
   identity against any proof, and the §10.6 D2 fallback — mac pin absent, a
   DIFFERENT unit bound — does not disarm it. The comment above that line said
   "on this stage-0-proven unit"; nothing checked which unit that was. The rule
   lived only in a config comment.
2. **It blocked auto** on every craft that had the key set, which is the one
   craft with a probe-carrying table.
3. It invited a fix in the wrong direction — binding the proof to a MAC —
   which would have made an unenforceable rule enforceable rather than asking
   whether the rule was right.

**What still fails closed.** A die family with no stage-0 evidence must still
not probe; adding one is a stage-0 run, not a config edit. Off by default,
radio-only, TX-node-only, and the four §9.4 receiver window guards are
untouched — guard (4) in particular, which is what stops a non-probing TX
manufacturing a phantom veto, and which does not depend on this ruling.

## Pass 195 — §15.2 `adapters` gains an auto form; §10.6 artifacts key by identity (2026-08-30)

**Verdict.** `adapters` may be an OBJECT carrying an `auto` block instead of a
hand-authored array. The node enumerates its own radios, elects one TX by part
priority, and synthesizes the stanzas. Separately, the §10.6/§10.7 calibration
artifacts move from one fixed filename per node to one per adapter identity.

**Changed:** §15.2 (auto form, election law, enumeration filter, two
fail-closed refusals), §10.6 (artifact filename carries the identity; legacy
fallback), §11.5 (clarifies that `home_chan` was never read, and becomes live
under auto). No wire change, no field removed, no existing config altered in
meaning — an array-form config parses and behaves byte for byte as before.

**Why election must follow bring-up.** RTL8812EU and RTL8812AU share USB PID
`0x8812` (`third_party/devourer/src/WiFiDriver.cpp:45`), so the two highest
priorities are indistinguishable from the descriptor; family comes from
SYS_CFG2, read inside `CreateRtlDevice`, and the EFUSE MAC only during
`InitWrite`. A pre-claim ranking cannot express the required order, and a
separate probe pass is a second full bring-up. Auto therefore reuses the Pass
154 sequence unchanged — claim provisionally, bring up, read identity, bind —
and adds one branch at the point where mac pins already re-bind
(`io/src/air_radio.cpp`).

**Why RX-only needs no key.** §3.11 (Pass 162) already defines the uplink-free
archetypes and `AirBackend::create` already derives `allow_rx_only` from
`node.spectator || (cache.store && no streams)`. Auto elects a TX unless that
holds, so a spectator ground stays TX-less and an ordinary ground never can be.
Inventing an `rx_only` key would have created a second, drifting answer to a
question the spec had settled.

**The enumeration filter is a pre-existing bug.** The claim scan tested
`idVendor != kRealtekVid` and nothing else (`io/src/air_radio.cpp`), so a
first-free claim could open a Realtek Bluetooth, card-reader or ZeroCD device.
Under auto — which claims up to `max_adapters` — that becomes routine rather
than unlucky. The descriptor filter (vendor-specific interface with bulk
IN+OUT) applies to both forms, and is preferred to a PID allowlist because a
PID list goes stale and this does not.

**Why the artifact store changed with it.** Identity was already correct: a
swapped unit reads STALE and is refused (Pass 154 D2/D3). But the store held
one `artifact.json` per node, so swapping A→B destroyed A's measurement and
swapping back left the operator re-running a calibration that had already been
performed. Under auto, where the TX is elected rather than pinned, that turns
from an inconvenience into the normal case. Per-identity filenames make
swap-and-return lossless; a missing per-identity file falls back to the legacy
name and is accepted only on an identity match, so a deployed node upgrades in
place with no migration.

**Two refusals, both fail-closed.** `air.mcs_probe` + auto is a config error:
§9.4 gates probing on a per-unit stage-0 proof, and `deploy/vehicle-192.168.2.232.json`
records that proof for one named 8812EU MAC — an election that may land on a
different die cannot inherit it. `air.usb_tx_agg` is applied to every claimed
unit under auto, because the elected unit does not exist when the devices are
constructed; this is the same concession the Pass 154 re-bind already makes for
`tx.report`, it costs an ear one MAC-init write, and the default 0 keeps every
deployment byte-identical.

**Declared, not inferred.** The standing rejection of auto-detected spectator
mode (archive Pass, "silently drops ARQ") is not in tension with this: auto is
an explicitly requested mode whose full outcome is logged, printed through the
config summary, served by `GET /api/v1/info`, and reproducible as a pasteable
array via `waybeam-link adapters --emit`.

**A busy dongle must not be fatal — found on hardware, not in review.** The
first device run of this feature (x86 bench, 8812CU + 8812AU) failed outright:
a running `waybeam_hub` ground held the AU via usbfs, and auto refused the
whole node over a radio it never asked for by name. Under auto there is no
per-device intent, so an unclaimable candidate is now logged and skipped and
only an empty survivor set fails. The array form keeps its hard failure, where
the operator did name the device. §15.2 records the measurement.

**Also found on hardware, and NOT fixed here.** A `RadioAir` created and
destroyed with no `poll_once()` in between hangs in `~Impl`'s join. It is
pre-existing — `hwtrial_bringup --seconds 0` built from `origin/main` hangs
identically — and `~Impl` already names the mechanism in its own comment
("a join can block while a bring-up is still in flight"). The new `adapters`
mode is simply the first shipping caller to reach it; it drains for ~1 s
before teardown, which is what every real consumer does. The backend's
start/stop race is its own change with its own device pass:
`docs/findings.md` 2026-08-30 carries the isolation table and what stays open.

**Not done, deliberately.** No sticky TX election across replugs (operator
ruling 2026-08-30) — the MAC tiebreak is deterministic and the array form is
the pin. No mac-keyed per-unit power table — the auto block's power keys cover
the elected TX, which is the whole of the single-adapter craft case.

## Pass 194 — §15.2 aggregation becomes observable: tx_bulk / tx_bulk_failed (2026-08-29)

**Verdict.** `air.usb_tx_agg` has been shippable since Pass 184 and could not
be verified from a running node. §15.3 publishes two new per-adapter counters,
`tx_bulk` and `tx_bulk_failed`, taken from the backend's transport counters
(devourer `TxStats`), so `tx_submitted / tx_bulk` states whether frames were
actually packed.

**Changed:** §15.3 (two adapter fields added; example and prose updated).
Additive — no field removed, no semantics altered, no config key added.

**Why nothing existing could answer it.** `tx_submitted` counts FRAMES on both
paths: `RadioAir::flush_staged()` adds the whole batch at once
(`io/src/air_radio.cpp`), so the counter reads identically at
`usb_tx_agg` 0 and 3. Devourer emits a per-URB `tx.agg` event carrying the real
frame count, but `RadioAir::Impl::ev_write` consumes and drops every event that
is not `tx.report`, and nothing in this tree called `IRtlDevice::GetTxStats()`.
A node therefore published no signal that distinguished packing from not
packing.

**Evidence.** 8812EU craft (Jaguar3, SSC338Q, 1920x1080@60, ~19 Mbps,
2026-08-29), four alternating 20–25 s arms against an x86 ground: at
`usb_tx_agg` 0 the craft's `tx_submitted` ran 2134–2155/s and at 3 it ran
2146–2152/s — indistinguishable, while hub CPU moved 26.2–28.7% → 20.2–20.6%
and softirq 4.1% → 2.5%. The CPU moved and the published counters did not,
which is the gap this Pass closes.

**Why the backend's count and not our flushes.** A HAL that clamps descriptors
per bulk window splits one `flush_staged()` across several transfers — Jaguar1
takes 1 per window (`third_party/devourer/src/jaguar1/RtlJaguarDevice.cpp`) —
so a flush counter would report an aggregation the chip never performed. That
is precisely the case under investigation on 8812AU, where `usb_tx_agg` 2 and 3
both cut craft CPU ~35 points and took the link to 966‰ loss and 0 fps.

**Reading a zero.** `IRtlDevice::GetTxStats()` defaults to `{}`, and the udp
air backend has no transport counters, so `tx_bulk` 0 means "not reported" and
never "no transfers". §15.3 says so.

## Pass 193 — §9.6 horizon frame caps are withdrawn: they never bound (2026-08-29)

**Verdict.** The per-frame ceilings this spec has commanded since Pass 37
(`video0.maxIBytes`/`maxPBytes` under `FRAMEBITS_FIRST`) do not constrain the
encoder. They are removed from the spec and the implementation, together with
`venc.frame_caps`, `venc.cap_ceiling_bytes` and `venc.i_/p_headroom_permille`,
the `derive_frame_caps()` derivation, the `kCaps` transaction, and the
`venc_max_i_bytes`/`venc_max_p_bytes` §15.3 fields.

**Changed:** §9.6 (horizon-cap section replaced with the withdrawal; the Pass
112 caps-then-bitrate ordering rule deleted with it), §9.11 (cap-coupling
bullet deleted), §15.3 (two stats fields removed; example corrected), §17
RE-DERIVE table (cap-headroom row reduced to `fps_hint`).

**Evidence.** SSC338Q 1280x720@60 H.265 CBR GDR, 2026-08-28: across `maxPBytes`
33144 → 25000 → 16000 → 10000 → 6000 B the delivered rate moved <0.3% and every
one of 863 access units exceeded the cap; `maxIBytes` 91788 → 2000 left IDR
size 66–81 KB across 16 sampled IDRs. Controls: CBR held its target to 98.8%,
`minQp=30` on the same scene gave ~490 B/frame, every step re-read from
`/api/v1/get`. Independently reported in OpenIPC/waybeam_venc#111.

**Bounded, deliberately.** venc 0.45.0 measured `maxPBytes=2000` moving a
Star6E 5619 → 1868 kbps — influence, 3× below this sweep's floor. It
reconciles: 1868 kbps at 60 fps is ~3892 B/frame against a 2000 B cap, ~1.95×
over. The caps influence below ~6000 B and bind nowhere measured. Not-binding
is what disqualifies a ceiling, not absence of effect — the spec says so
rather than claiming they do nothing.

**Consequences to observe.** `venc_pushes` falls 3 → 1 per rung transition,
and the §9.6 settling anchor now lands on the bitrate write instead of a
trailing caps write, so the §9.11 `ACTUATOR_SETTLE` hold ends earlier after
each transition. Both are documented in §9.6.

**Co-requisite, craft-side ordering.** venc drops the fields in contract
0.21.0. On each craft, deploy this side FIRST: a stale actuator's cap
transaction 404s at the head of its queue forever and starves bitrate, fps and
IDR with it — the starvation already observed on CV610's 501
(`docs/findings.md`, `deploy/vehicle-192.168.2.181.json`).

## Pass 192 — the §9.6 write path is contract state, so §15.3 publishes it (2026-08-28)

**Verdict.** §9.6 Pass 73 makes "never written to `/etc/waybeam.json`" a
guarantee, but the actuator's knowledge of which path it is on
(`VencActuator::live_fallback()`) never left the process. §15.3 now carries
`venc_live_fallback` and `venc_persisted_writes`. A guarantee a consumer
cannot read is not a contract, and this one was being re-derived by hand on a
craft.

**Two fields, not one — neither is sufficient alone.** The latch heals on the
10-min re-probe, so a bool read after a heal reports `false` while the flash
writes it caused stay invisible; and under write-on-change a latched actuator
on a steady link leaves the counter flat, so a low count does not mean the
next change is safe. `venc_live_fallback` answers "will the next commanded
change write flash", `venc_persisted_writes` answers "has this process ever
written flash". `0` on the counter is the affirmative form of the §9.6
guarantee.

**Not resettable.** Both are read straight from the actuator, matching
`venc_pushes`/`venc_failures`, and so are outside `POST /api/v1/stats/reset`.
Spelled out in §15.3 because the partial-reset surface has misled a rate
calculation before.

**Excluded:** `/request/idr` — not a config write, and it returns from
`finish_txn()` before the persistence branch.

**Changed:** §9.6 (volatile-first bullet — the path is observable, and
`venc_failures` is explicitly *not* a proxy for it), §15.3 (both fields, the
sample object, the reset exclusion).

**Evidence.** Craft `.232` (SSC338Q Star6E, venc 0.70.0, hub with in-process
wblink TX), 2026-08-28: a forced `GET /api/v1/restart` drove a full re-assert
ladder — `venc_pushes` 56→99, 8 `maxFrameSize changed` transitions — with zero
`[venc_config] Config saved` lines and an unchanged `/etc/waybeam.json` mtime
(1787930869 before and after). Settling that required shell on the craft and a
deliberate encoder restart; it is now two fields in `/api/v1/stats`. Issue #248.

## Pass 191 — local debug reads the process, never guesses from files (2026-08-23)

**Verdict.** Operator ruling 2026-08-23. The co-located Hub may inspect and
mutate only its own Link node over the existing trusted management HTTP plane.
Profile-envelope and synthetic RX-loss controls are volatile local levers; no
RF command, port, persistence, or authentication surface is added.

**Effective configuration is a Link answer.** `GET /api/v1/features` reports a
sanitized role-neutral summary of the validated configuration and profile table
the running process actually loaded, plus the live ARQ and FPS-ladder gates.
The Hub must not re-parse the config file: it can change after startup, embeds
may supply config as text, and duplicating Link defaults would manufacture a
plausible but false status page. Paths, binds, and secrets are omitted.

**Changed sections.** §15.5 gains the TX-local profile GET/POST, RX-local
synthetic-loss GET/POST, and the read-only effective-feature summary. §9.7's
existing profile envelope and §6.3b's concealment semantics are unchanged.

**Evidence.** `feature/link-debug-controls`; waybeam-hub
`feature/link-debug-webui`; coordination
`specs/cross/2026-08-23-link-debug-webui`.

## Pass 192 — delivery ends at an accepting egress, not a void callback (2026-08-23)

**Verdict.** §6.3a/§6.3b/§15.3/§15.4 now distinguish reconstruction from
delivery. A complete frame refused by the local SHM writer or in-process sink
is finalized without retry, but is not counted as delivered/recovered, is not
learned by `SpatialRepair`, and cannot settle §3.9 merely because it was an
IRAP. It increments `frames_egress_rejected`, not
`frames_unrecoverable`/`salvage_failed`.

**Why terminal.** The radio block is already complete; another NACK cannot
make a full local ring or detached decoder accept it. Retrying the frame would
add latency and violate the drop-not-block egress contract. The next accepted
frame resumes delivery; decoder bootstrap remains governed by §3.9.

**API compatibility.** C++ `FrameSink` returns acceptance. The C ABI adds
`wblink_frame_ack_cb` + `wblink_rx_run_ack`; the original void callback and
`wblink_rx_run` stay exported and adapt their callback as always accepted.
Existing Android/vendor pins therefore do not require a flag-day update, while
waybeam-hub can report its real GStreamer handoff result.

**Blast radius.** No over-air or VFRM layout change. waybeam-link owns the
reassembler/state/stats change; waybeam-hub adopts the ack API. Android remains
source/ABI compatible and can opt in when its deliberate vendor bump lands.
Evidence is the byte-exact accept/reject regression, independent link/hub builds,
and the `.242` x86 ground running against the `.232` SSC338Q craft. A controlled
pipeline restart rejected five boundary frames without increasing unrecoverable
loss, held the IDR gate through four deltas, and resumed at about 60 fps / 19 Mbps.

## Pass 189 — a salvaged frame says so: §15.4 `flags` bit 3 (2026-08-23)

**Verdict.** Operator ruling 2026-08-23. §15.4 `VencFrameMeta.flags` gains
bit 3, **salvaged**, set by the §6.3b RX path on every blob it emits — slice
salvage and whole-frame freeze alike. It is the only bit an encoder never
writes.

**The gap it closes.** §6.3b rebuilds a failed block into a decodable access
unit and hands it on as an ordinary frame. That is right for display and wrong
for every consumer that treats a frame as a *trustworthy picture*: waybeam-hub's
recorder was advertising salvaged frames as seek points (a cue is a promise
that decoding can start there, which a frame of synthesized all-skip slices
cannot keep) and could cache parameter sets out of one. The hub had no way to
know — the RTP depayloader it no longer uses could flag damage, and neither the
ring nor the C ABI could.

**No interface change, which is why this is small.** `write_egress`
(`node/src/rx_node.cpp`) hands the ring writer and the `FrameSink` C-ABI
consumer the same `[VencFrameMeta][Annex-B]` blob, so a bit inside the payload
prefix reaches both with no signature change and no source break for existing
consumers. An earlier draft of the consuming spec assumed a flags argument had
to be added to `wblink_frame_cb`; reading `write_egress` retired that.

**What the bit does NOT assert.** Absence means "not known to be salvaged", not
"intact" — a producer predating the bit leaves it clear, so a consumer that
treats absence as today's behaviour fails safe. The §6.3a byte-identical
guarantee is narrowed in the same amendment to the fast and FEC paths, which is
where it was ever true.

**Blast radius.** No over-air change: this is metadata on a frame the link
already delivers. `kFrameFlagsKnown` gains the bit, so the frame-shm bench
validator does not start counting salvaged frames as bad metadata. The
canonical header is `waybeam_venc/include/venc_frame_ring.h`; reserving bit 3
there is filed as follow-up so a future encoder flag cannot collide with it.

**Evidence.** waybeam-hub #221 (the recorder-side guards, device-verified on
the `.170` rk3566 ground under induced loss) and coordination
`specs/cross/2026-08-23-frame-damage-flag`.

## Pass 188 — the probe keeps measuring a locked-out rung; only the veto stays clamped (2026-08-21)

**Verdict.** Operator ruling 2026-08-21, reversing the *arming* half of Pass
187 (a) while keeping its control property. §9.4 arming follows the §9.7
`max_profile` pin alone; the candidate is no longer clamped to the effective
ceiling.

**What changed the answer: a measurement, not an argument.** Pass 187 clamped
to `adaptive_hi` on the reasoning that a veto cannot help where §9.2 has
already barred the climb. The reasoning is still correct about the veto. It is
wrong about the *measurement*, and the first real range walk showed by how
much: the probe was **disarmed for 46 % of a degrading link**, 100 % correlated
with `lockout_active` (66 of 66 samples), and `promote_blocked_probe` never
moved once in 8.4 minutes on a link that reached −86 dBm. The sequence is
self-defeating — loss locks the rung above, the clamp switches the probe off,
and by the time the candidate rate is worth knowing about nothing is measuring
it. §9.2 re-entry then runs on the RSSI floor alone, exactly as blind as before
the probe existed. `docs/findings.md` 2026-08-21.

**The veto needs no new mechanism to stay clamped.** Both climb rules are
gated on `rung_ < adaptive_hi` *before* `probe_veto_fresh` is consulted
(`core/src/selector.cpp`), so a climb to a locked-out rung is unreachable and
evidence about it can suppress nothing. Pass 187's control property therefore
survives the reversal for free — which is the fact that makes this a one-line
change rather than a redesign, and it should have been noticed when Pass 187
was written.

**Cost, stated rather than buried.** A `1/period` share of video frames now
flies a rate that has just demonstrably failed, in the window when the link can
least afford it, and most of those frames are expected to be lost. That is the
accepted price of having rate evidence in the only regime where it is
actionable. Reducing rather than removing the duty under a lockout — a longer
effective period — is recorded as an open refinement, not adopted.

**What stays contract.** No wire change. Pass 187's other half (§10.6 pinning
by profile ID, not MCS) is untouched, as is the per-tick re-derivation and its
change-guard.

**Evidence.** `docs/findings.md` 2026-08-21 walk test; issue #226.

## Pass 187 — the probe clamp follows the EFFECTIVE ceiling, and §10.6 stops conflating an MCS with a profile ID (2026-08-21)

**Verdict.** Two amendments, both found by adversarially reviewing Pass 186
rather than by running anything. Filed as issues #227 and #228 first, so the
ruling is on the record separately from the patch.

**(a) §9.4 — the candidate clamps to `adaptive_hi`, not to `max_profile`
(operator ruling 2026-08-21, issue #227).** Pass 186 clamped the probe
candidate to the §9.7 operator pin. The ceiling the climb rules *actually*
honour is that pin **narrowed by the §9.2 lockout**, so a rung §9.2 had barred
was still being probed at the full `1/period` duty — the same waste Pass 186
removed, through the other door.

The ruling was genuinely two-sided and is recorded in §9.4 as such. Against
clamping: a locked-out rung is the only one whose current viability is
directly measurable, and §9.2 expiry otherwise re-enters on RSSI margin alone.
For clamping, decisively: **probe evidence is a veto and can never authorize
the re-entry**, so it could at best block one the RSSI floor would have
allowed — while the lockout window is by construction the moment the link is
weakest, and therefore the most expensive one in which to fly a share of video
frames at a rate that has just demonstrably failed. Re-entry is not left blind
either; the rung floor plus `reentry_backoff_s`/`reentry_dwell_s` gate it.
Conflict fallback is inherited unchanged: where a lockout collides with the
operator envelope the envelope wins and the lockout is ignored, in the clamp
exactly as in `evaluate()`.

Because the effective ceiling moves with §9.2 state rather than only with
commits and pin writes, the candidate is now re-derived **every selector
tick**, with the radio written only on an actual change — a steady link issues
no traffic for this.

**(b) §10.6 — the calibration sweep index is an MCS, and §9.7 pins take
profile IDs (issue #228).** `CalibActions::pin_rung` carries the calibrator's
loop variable, which indexes a **per-MCS** artifact (`curve_qdb[mcs]` is how
§10.2 resolves power). It was being handed straight to `set_profile_pin`,
which takes profile **IDs**. The two coincide only on a ladder whose ids equal
its MCS values — which the fleet table happens to be, which is why nothing
ever failed. On any other ladder §9.7 resolves the unmatched id by saturating
to the top rung, so the sweep runs **unpinned** and every dwell measures
whatever rung the selector drifts to, silently, producing a §10.2 curve
attributed to the wrong rates.

§10.6 now states the two spaces and the mapping between them, and requires
that an MCS the ladder cannot select **fails the run** with a distinct reason
rather than sweeping an unpinned rung. **Pre-existing, not a Pass 186
regression** — Pass 186 only routed the same call through a seam that made the
value matter twice, which is how it surfaced.

**What stays contract.** No wire change in either. (a) can only ever reduce
the set of frames flying the candidate rate. (b) changes no artifact format:
`curve_qdb` stays per-MCS and every stored artifact remains valid.

**Evidence.** (a) is a ruling, not a measurement — the mechanism is
`core/src/selector.cpp:621-622` (`adaptive_hi`) against Pass 186's clamp
reading only `max_profile`. (b) is read from the code: `rung_` initialised 0
(`calibrate.h:402`), bounded `>= 7` (`:593`), indexing `placement_qdb[rung_]`
(`:508`) and `levels` documented "per MCS" (`:301`), with `init_calibration`
writing `p.levels[pr.mcs]` — an MCS space end to end, against
`selector.h:90` / `selector.cpp:81` requiring IDs.

## Pass 186 — the §9.4 probe becomes observable, and stops probing above its own ceiling (2026-08-21)

**Verdict.** Two amendments to §9.4, both found by the first device run of the
Pass 163 probe (`docs/findings.md` 2026-08-21) rather than by reading.

**(a) The candidate is clamped to the §9.7 `max_profile` pin.**
`probe_up_candidate_mcs()` took `(table, active_profile)` and walked to the
next ascending id with no knowledge of the ceiling. The measured craft pins
`policy.select.max_profile: 4` while sitting at profile 4, so its probe flew
a 1/64 duty producing evidence whose only consumer — a veto — could only ever
suppress a climb policy already forbids. The clamp reads the **live** pin, so
a §11.7 `SELECTOR` freeze (`min == max == current`) disarms the probe too;
ceiling resolution is §9.7's, an absent id saturating to the top rung so
`max_profile = 255` still unpins. The **receiver is deliberately not
clamped**: `max_profile` is node-local policy, not table content, so the RX
cannot know it — its window keeps deriving the unclamped candidate, observes
nothing, and guard (4) reports `0xFFFF`. One-sided knowledge degrades to
absence of evidence, which is the property §9.4 already requires.

**(b) `probe_per` and its guard tallies enter §15.3.** The value was computed
on the RX, encoded into the §3.5 report, consumed by the selector — and
exposed nowhere, on either end. The single observable was
`promote_blocked_probe`, which moves only when a climb is *both attempted and
vetoed*, so "no opinion" and "favourable opinion" were indistinguishable and a
fleet could enable probing and never confirm it produced evidence. Six
role-dependent fields (`verdict`'s precedent): `probe_per`,
`probe_per_age_ms`, `probe_candidate_mcs` on both roles; `probe_successes`,
`probe_failures`, `probe_observed` on the receiver. `probe_observed` is guard
(4)'s counter and is the operational proof — a nonzero value is the only
reading that separates a probe that is *working* from one that is merely
*scheduled*.

**What stays contract.** No wire change: the §3.5 `probe_per` field, its
`0xFFFF` sentinel, the four receiver guards, veto-not-warrant, and the
fail-closed per-unit `air.mcs_probe` enablement are all untouched. (a) can
only ever *reduce* the set of frames that fly the candidate rate, so a fleet
mid-upgrade sees strictly less probing, never mis-scored probing.

**Evidence.** `docs/findings.md` 2026-08-21 — probe verified on the live
fleet at `probe {period:64, slot:4}`: both grounds independently derived the
same probe set from `seq` (2013 vs 2012 frames at MCS 5 in one 60 s window,
1988 vs 1987 in the repeat), shares 1.527 % / 1.504 % against the ideal
1.563 %, an `air.mcs_probe: false` control reading exactly 0 on both, and no
measurable cost (`delivered/rx` 97.56 / 97.52 / 97.53 %). That run is also
what proved both defects: the craft was at its pin, and the only way to
confirm the probe was working at all was to reconstruct it by hand from
per-MCS RX histograms.

## Pass 185 — a failed FEC block is not an empty one (2026-08-19)

**Verdict.** New §6.3b (operator-directed, 2026-08-19): opt-in per frame-SHM
egress stream (`streams[].conceal.mode: "slice-skip"`, default `"off"`), the
verified systematic source symbols of a block that finalizes below `k` are
salvaged instead of discarded, HEVC slice completeness is reconstructed from
the surviving byte ranges, and each erased slice is replaced by a synthesized
all-skip P slice so the decoder receives a complete, standards-valid access
unit with the lost region frozen from the reference picture. §6.3a outcome 4
and §14.1's "no partial delivery" now defer to §6.3b when enabled; §6.3a
point 5's byte-identity gains its one exception (a repaired slot is
reconstructed, not the producer's bytes). Off = bit-for-bit today's behaviour.

**What stays contract.** Integrity tiering (RECEIVED = FCS + length-exact +
subheader cross-checks, RECOVERED = full-decode only, ERASED = never used);
IRAP pictures are never concealed (ARQ/`i_rate` own them); zero-reorder holds
— a salvage that would emit behind a newer block is dropped; every refusal
falls back to the pre-§6.3b drop. No wire change: salvage consumes only what
§5.1a/§14.1 subheaders already carry (`k`, `i`, `s`, `frame_len`).

**Evidence.** Offline proof, branch `claude/waybeam-spatial-hevc-dkoqq3`:
x265 (WPP) + HM-18.0 (no-WPP, SAO on, TMVP on/off) multi-slice vectors,
synthesized-slice substitution decoded clean by ffmpeg 6.1 and libde265
1.0.15; concealed region byte-equal to the previous picture's co-located
interior; untouched slices byte-equal to the reference decode; next IDR
resyncs exactly. Full-chain sim (chunk → loss → salvage → rebuild → decode):
25% i.i.d. symbol loss, r=2 — 0/120 frames dropped (34 repaired, 15 frozen)
vs 49/120 dropped under §6.3a outcome 4. See `docs/findings.md` 2026-08-19.

## Pass 184 — how frames are carried is not how they are paced (2026-08-17)

**Verdict.** New §15.2 `air.usb_tx_agg` (0..3, default 0 = off): how many
already-decided frames the host may put in ONE USB bulk-OUT transfer. Host
cost only — this is the first §15.2 key that changes nothing a peer can
observe, which is exactly why it needed a ruling on where the line sits.

**The distinction the key rests on.** A transport knob that groups frames
usually means pacing, and pacing is wire-visible: it changes when frames air.
This one cannot, because it batches only frames the framer emitted back to
back within a single fan-out (one video frame's data + parity), all of which
were already decided and would have gone out in that order microseconds
apart. Nothing is ever held waiting for a partner to arrive: a partial run is
submitted at its fan-out boundary. So the key selects a *carrier*, not a
schedule, and stays out of §3 and §7 entirely.

**Ordering is the invariant, not throughput.** The failure mode is silent —
a frame that overtakes another looks exactly like ordinary RF loss to every
consumer, and no counter would show it. Three rules keep it, and all three
are in the code rather than in this document's good intentions: every
unbatched send flushes the batch first (§10.6 selector state, §12 resends,
the §11.7 command echo); a §7.2 paced EOB is submitted *before*
`note_eob_sent` re-arms the listen window, so a window never opens while the
frame that opened it is still staged; and every fan-out boundary flushes, so
nothing straddles a tick. The 802.11 sequence the §9 loss estimator reads is
stamped at stage time, in producer order.

**Ceiling is the hardware's.** The HalMAC families parse at most 3
descriptors per bulk transfer (`BLK_DESC_NUM`), so >3 is a config error, not
a silent clamp — a config that says 8 would otherwise quietly mean 3.

**Evidence.** Device A/B on the CV610 craft .181 at ~1100 pps, its own
config and frame-SHM ingress, two interleaved runs: 316→223 and 337→260
µs/frame, **8.6–10.5 points of one core**, pps unchanged (1107→1100,
1104→1101). Mechanism measured separately with usbmon on the same 8733BU
part, because the craft build compiles INFO out and the link silences
devourer's event sink: **10974 → 4347 bulk-OUT URBs for identical frames**
at an identical 1098 fps (frames/URB 1.00 → 2.52 — not 3.00, and the
shortfall is the design: partial runs and unbatched control frames).

Neither of those would notice frames arriving corrupted, duplicated or
reordered — pps and URB counts look identical either way — so correctness
was measured separately and end to end: 8733BU TX to an 8812AU running a
real `rx` node, `tools/frame_shm_feed` stamping payload byte j of frame i
as `(i*31+j)&0xff` and verifying every byte after §6.3a reassembly.
**bad=0 in every run** — not a delivery count, every byte. Five paired runs
delivered 400/400/399/400/400 unbatched against 400/397/398/399/400 batched:
both arms drop the occasional frame, so that is RF, not the knob.

**This is not A-MPDU, and it does not burst.** The two are separate
capabilities in the driver — USB TX aggregation is host-to-chip transport,
A-MPDU (`SetAmpduMode`, never called here) is the MAC folding MPDUs into one
PPDU. Each frame keeps its own preamble and contention cycle, so no airtime
is saved. The remaining worry was SPACING: frames arriving in the TXDMA
together might key up back to back, changing the burst structure that
per-frame FEC's loss-independence assumption sits on. **Measured at a
witness's chip TSF, and they do not**: 33.7k inter-frame gaps, batched and
unbatched, give p10/p50/p90 of 205/222/232 µs either way, with **0.0% of
frames closer than 200 µs in both arms**. At MCS7 the air was already the
pacer and the host never was — what changed is how frames reach the chip,
not how the chip airs them.

Residual caveats, smaller but real: five 400-frame delivery runs cannot
exclude a ~1-in-400 PER effect, and the earlier wording "adds no latency…
never the timing" overstated — a frame waits the microseconds it takes to
stage its neighbours, and the §7.2 drain flushes after its loop rather than
inside it.

Limitation, stated rather than left implicit: both craft A/B runs took
agg_off first, so strict order-effect bias is not excluded by the A/B
alone. It is excluded by the URB count, which is causal and order-free.
Upstream half: OpenIPC/devourer#400. Consumer default stays 0, so only
`deploy/vehicle-192.168.2.181.json` changes behaviour.

**Process note.** This amendment landed AFTER its implementing commit,
inverting the tier-1 order ("spec amendments commit FIRST"). Recorded rather
than hidden by a rebase: the key was treated as a host-side build detail
until review asked whether a §15.2 addition can ever be tier 2, and the
answer is no.

## Pass 183 — the uplink data plane exists, and it is best-effort (2026-08-15)

**Ruling (operator, 2026-08-15; closes the contract half of issue #177).**
A ground MAY originate DATA for TELEMETRY/CONTROL — new §7.5. Four rulings,
all operator-selected from presented options:

1. **Auth: bound-issuer only.** The craft accepts uplink DATA solely from
   the §11.5a latched originator (the `vehicle_cmd.cpp` identity check,
   minus MAC/nonce); unbound or other-issuer frames are silent drops.
   Consistent with §13 harden-not-prevent and §18's data-path posture.
2. **ARQ: best-effort by construction (v1).** No NACKs, no resend ring, no
   FEC; craft emits nothing about uplink streams (Pass 79 already bars
   non-RTP from reports/selection). Guarantees belong to §11.7 campaigns.
3. **Pacing: gap-gated + blind fallback.** Uplink is a return flush class
   (after CSA/NACK, before reports), never re-arms a listen window
   (Pass 78 law). CONTROL holds depth 1 latest-state; TELEMETRY FIFO 32
   drop-oldest; no anchored window for `uplink.fallback_ms` (seed 50) →
   §7.1 opportunistic; `uplink.pps_budget` (seed 100) caps ingress.
   Seeds RE-DERIVE at §17 gate 4.
4. **Delivery: existing egress bindings.** `dir` stays socket-local; node
   role fixes air direction — rx-node `dir:"in"` = uplink ingress,
   tx-node `dir:"out"` = uplink delivery. RTP/AUDIO/frame-shm uplink
   refused at `--check` and node startup (role-dependent shape). Uplink
   DATA stamps `active_profile`/`table_version` 0; the craft ignores
   both; seq is a strictly-monotonic accept cursor over the current
   sender session (one cursor per stream; a new session replaces it).
   Datagrams over 1398 B (§3.2 budget minus header) drop at ingress
   with a counter, never truncated.

**Changed:** new §7.5; §3.4 uplink note; §15.2 `policy.uplink.*` + stream
bullet; §15.3 uplink counters; §17 knob row. **Evidence:** issue #177
(gap inventory: no craft DataView arm, no ground framer instance, no §7
contract), issue #99 (airtime budget context), coordination memory
Pass 78/79 measurements; implementation + loopback E2E in this PR.

## Pass 182 — the sweep retunes fast where the die says it can (2026-08-15)

**Ruling (operator, 2026-08-15, with Pass 181).** The scout sweep's
per-channel retune passes `fast=true` when the scout adapter's §15.5
caps view states `fastretune`; the §11.2 class-0 path applies only to
same-width hops, which a 20 MHz sweep always is. Dies without the
capability keep the full retune — measured on the RTL8733B, whose
"fast" path still blocks ~277 ms vs 345 ms full, which is why devourer
reports `fastretune:false` there and why the gate is the caps bit
rather than a try-it heuristic. Exit paths (selection, rest) stay
`retune_all` + `reapply_tx_power`; mid-sweep the class-0 path leaves
TXAGC untouched, which is not a regression — the full-path sweep never
re-applied it either.

**Changed:** §15.5a (sweep bullet). **Evidence:** findings.md
2026-08-15 (AU fast call 41 ms vs full 130; CU 13 vs 32; BU 277 vs
345, radio-live unchanged on all three); device sweeps on the .181
8733BU, 10/10 detection at 300 ms and 500 ms dwells, caps gate
self-disabling observed via `/api/v1/info`.

## Pass 181 — dwell_ms buys listening, not retuning (2026-08-15)

**Ruling (operator, 2026-08-15; direction decided by measurement per
the operator's process note — the amendment follows the settled
mechanism rather than preceding the experiment).** The scout's
per-channel dwell deadline anchors when the retune hook RETURNS, not
on the tick timestamp that triggered it. Every downstream deadline —
dwell, extension base, §15.5a sense barrier — re-anchors with it. The
engine takes an injected `Hooks::now` clock; a null hook preserves the
tick anchoring byte for byte (both directions pinned in
`node_discovery_test`).

**Why.** The blocking retune call costs 32–345 ms per chip/host
(findings 2026-08-15). Anchored pre-retune, the 8733BU listened
~155 ms of a 500 ms dwell and would listen ZERO at the 300 ms floor —
the Android dwell knee (memory `scout_retune_radio_silence`) is the
same arithmetic with that platform's numbers. A per-chip settle
constant in the caps view was rejected: the dominant term is per-host,
so no static per-die number is valid across hosts.

**Changed:** §15.5a (sweep bullet). **Evidence:** findings.md
2026-08-15; device A/B on the .181 8733BU (old vs new binary, real
craft emitter): detection 10/10 both, wall +0.8 s = the restored
listening time.

## Pass 180 — a build states what it can do (2026-08-15)

**Ruling (coordination `specs/cross/2026-08-14-wblink-library-parity`
R6).** `wblink_build_info()` returns the compiled feature set as JSON —
`{"frame_shm":B,"control_server":B,"venc":B,"radio":B,"node_tx":B}` —
baked into the archive at ITS compile time, not the consumer's.

**Why a feature set and not a version.** The install rules already
refuse to fabricate a version, and the reasoning does not stop at CMake:
the failures are capability failures. waybeam-hub configured
`pixelpilot.frame_shm.source: wblink` against a library built
`WBLINK_FRAME_SHM=OFF` renders a permanently black screen with every
counter reporting healthy.

**Baked, not recomputed.** The string is a constant in the library's own
translation unit, so it describes the archive a consumer LINKED, not the
headers it compiled against. The hub consumes this tree by sibling path
and its Makefile passes only `-I.../node/include`, so its own compile
sees none of the feature macros and could not compute an expectation to
compare — but it can read what the linked archive says.

**No new failure mode is claimed for it.** It does NOT catch the hub's
`WBLINK=1` stale-object trap: a `hub_main.o` compiled without the
define registers no module, so nothing is left to call this.

**What the guard does and does not cover.** `tests/build_info_test.cpp`
takes its expectation from CMake through a different target than the
library's own definitions, so a derivation that goes wrong on one side
fails against the other. It cannot catch a mistyped variable NAME —
both sides would read the same undefined symbol and agree on "false" in
every configuration — so the five names are constructed from one list
whose members are asserted `DEFINED`, which turns that typo into a
configure error. `scripts/gates.sh` additionally runs the test in a
REDUCED build (frame-SHM/control/venc off, radio on) because every
preset that runs tests leaves all five options on, so the false
direction had never once been executed.

**Evidence.** Branch `feat/r6-build-info`; three configurations measured
(full, receive-only, reduced-mixed).

## Pass 179 — config is an API, and so is selection (2026-08-15)

**Ruling (coordination `specs/cross/2026-08-14-wblink-library-parity`
R4).** Three additions, one theme: an embedder should not have to own a
file, or the library's own config vocabulary, to start a node.

1. **Config as text.** `wblink_{rx,tx}_set_config_json()` supplies the
   config as a string; `run()` is then called with a NULL path. Supplying
   both is BAD_ARG rather than a precedence rule — an ambiguity the
   caller can only resolve by guessing is not a contract. The io layer
   already parsed from text (`load_config_json`); only `node/` insisted
   on a path.

2. **Selection as a call.** `wblink_rx_set_selection(originator, net_id,
   channel_mhz)` overrides the three fields a scouted selection pins,
   applied after load and before the run. It moves EVERY adapter to the
   craft's channel, matching what the runtime §15.5a select already does
   to a spectator's ears — not `adapters[0]` alone, which is only
   correct on a single-ear node. This retires Waybeam-android's
   `applyScoutSelection`, which re-serialised the whole config through
   `org.json` to write three fields and had to hard-code
   `preferred_originator` / `net_id` / `channel` — library vocabulary
   living in a consumer, where a rename cannot reach it.

3. **Load diagnostics stop being stderr-only.** `load_all` reported
   config and profile-table errors with `std::fprintf(stderr, ...)`,
   which on Android goes nowhere a user will ever see. They go through
   `wb_logf()` now, so the sink an embedder already installs (#144)
   receives them. No new error-string ABI: the sink IS that channel, and
   a second one would be a second thing to keep in sync.

**Not in this pass.** A parsed-struct config surface (`Config` is C++
and the C ABI would need a mirror of 263 keys) and any *runtime*
reconfiguration — a handle still runs once, and a config still binds at
run.

**Evidence.** Branch `feat/r4-config-as-api`; `tests/node_config_api_test.cpp`.

## Pass 178 — the control endpoint is answered by the socket, not the config (2026-08-14)

**Ruling (coordination `specs/cross/2026-08-14-wblink-library-parity`
R3, slice 3).** `wblink_rx_control_endpoint()` /
`wblink_tx_control_endpoint()` report where this node's §15.5 control
server is **actually listening**, under the `snapshot_copy.h` buffer
contract. Published after a successful bind and never before: an
embedder that reads 3 knows there is no control plane to talk to,
rather than being handed an address that answers nothing.

**The value comes from `getsockname`, not from `control.bind`.**
`ControlServer` gains `bound_endpoint()` computed at create time,
because the configured string is a *request*: `host:0` is legal, parses,
and binds an ephemeral port, so echoing config back would recreate the
very failure this getter exists to kill — two sources of truth for one
endpoint that silently disagree (hub `metrics.waybeam_link` vs the
node's own `control.bind`; every route 502s and the address looks
right in both files).

**Scope.** A build with `WBLINK_CONTROL_SERVER=OFF` publishes nothing
and the getters answer 3 — the same answer that build already gives by
refusing to start with a non-empty `control.bind`. No REST route
changes; `/info` continues to report the configured string as its own
`control` field, which is what an operator authored.

**Evidence.** Branch `feat/r3-telemetry-abi` (stacked on Passes
176-177); `io/src/control_server.cpp` bind path; publication at both
run loops' control-server construction; endpoint cases in
`tests/tx_node_c_test.cpp` and `tests/node_fd_source_test.cpp`
(unpublished → 3 on a node that never bound).

## Pass 177 — the library states its own liveness (2026-08-14)

**Ruling (coordination `specs/cross/2026-08-14-wblink-library-parity`
R3, slice 2).** Both C handles carry an explicit lifecycle state:
`wblink_rx_state()` / `wblink_tx_state()` return
`WBLINK_NODE_CREATED` / `RUNNING` / `EXITED` (shared constants,
`node_state_c.h`) and, once EXITED, write the run's return code through
the out-param. Transitions are owned by the run wrapper: RUNNING the
moment the one-run claim succeeds, EXITED (rc first, state second) on
every return after it — including stop-before-start (a clean 0) and
config-load failure. A NULL-argument or reused-handle refusal never ran
and therefore never transitions.

**Two deliberate narrowings of the spec text.** (1) No separate WEDGED
state: the wedge is `EXITED` with `WBLINK_TX_WEDGED` as the rc — the
role-specific return spaces are already law (Pass 148 /
`tx_node_c.h` note 2), and a duplicate state would be a second decoder
for the same fact. (2) No terminal-exit reason *string* in the ABI: rc
is the machine-actionable reason; human-readable causes are log lines
and belong to the embedder's `wb_log_set_sink` (R6's gate), not to a
second string surface the loop would have to thread through every
error return.

**What this retires.** waybeam-hub's `mod_wblink_running()` re-derives
liveness because `wblink_rx_run` returns within milliseconds on a
missing radio (hub #195 review); Android keeps an equivalent latch.
Both can now read `state == RUNNING` from the handle's owner.

**Evidence.** Branch `feat/r3-telemetry-abi` (stacked on Pass 176);
lifecycle cases through the real shims (stop-before-start run,
bad-arg/reuse non-transitions) in `tests/tx_node_c_test.cpp` and
`tests/node_fd_source_test.cpp`.

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
