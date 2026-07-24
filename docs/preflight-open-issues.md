# Pre-flight audit — remaining open issues

Tracking list from the 2026-07-24 flight-readiness audit (base `c920483`,
Pass 78). Everything here was **found and verified but deliberately NOT fixed**
in the Passes 81–88 PR, either because it needs an operator decision, or because
the fix is larger than that PR's scope.

Nothing in this file is a spec ruling. Items needing one say so.

Severity: **BLOCKER** = do not fly. **SHOULD-FIX** = fly only with the risk
understood. **NICE-TO-HAVE** = cleanup.

---

## A. Needs an operator decision before it can be fixed

### A1 — No `csa.psk` in any deploy config — SHOULD-FIX

`grep psk deploy/*.json` returns nothing. Per Pass 61, an absent `csa.psk`
selects **announced-token mode**, and Pass 63 makes that token *public* on the
ANNOUNCE beacon. The only remaining takeover defence is the §11.5a sticky
binding (90 s release).

Deliberately not fixed: the value is a secret the operator chooses. It must be
provisioned on the craft **and** `.242` together — a mismatch is worse than
neither, since Pass 85 now (correctly) fails closed on a key that does not
verify.

`deploy/README.md` "Before flight" does not mention it; it should once a
decision is made.

### A2 — Craft logging fills a RAM-backed filesystem — SHOULD-FIX

`deploy/vehicle-waybeam-link.init` sends stdout (the §15.3 NDJSON) and stderr
to `/tmp`, which `README.md:213-215` documents as a small tmpfs on the SSC338Q.
At `stats.hz: 1` that is roughly 4 KB/s ≈ 14 MB/h, unrotated and uncapped. The
respawn supervisor added in Pass 88 appends rather than truncating, so a
crash-loop no longer resets it either.

Three viable answers, all one-line, none obviously right without knowing how the
operator consumes the stream:

- point `stats.bind` at a UDP sink and drop stdout to `/dev/null`;
- keep stdout but lower `stats.hz` on the craft;
- keep it and add a size cap in the supervisor loop.

On `.242`/`.199` the same stream goes to journald at `stats.hz: 5`
(~20 KB/s ≈ 70 MB/h) for data nobody reads — the hub scrapes REST `:8092`.

### A3 — Should a §9.7 `min==max` pin yield to the §9.8 fail-safe? — needs a ruling

Raised and deliberately left open in §9.8 by Pass 84. The PINNED branch
(`core/src/selector.cpp`, `lo == hi`) returns *before* the fail-safe branch, so
an explicit pin freezes adaptation through a total feedback loss. By the same
"never fail optimistic" argument Pass 84 used, a pin held at a high rung through
a real fade is fail-optimistic — but a pin is also a deliberate operator
override for bench / known-bad-link work, so changing it is a behaviour change,
not a bug fix.

Pass 84 fixed only the `min_profile < max_profile` envelope case. The deployed
vehicle (`min 1 / max 5`) is unaffected either way.

---

## B. Real defects, larger than the Passes 81–88 PR

### B1 — Blocking venc HTTP on the craft event loop — BLOCKER

`io/src/venc_http.cpp:28` sets `SO_SNDTIMEO`/`SO_RCVTIMEO` to 200 ms and calls
that a "200 ms connect/send/recv budget". It is **per-operation**: a
hung-but-accepting venc (process alive, not draining) can cost ~600 ms in one
call — connect, send and recv each hitting their own timeout.

Call sites are on the hot path:
- `app/main.cpp` `TxCore::tick` → `venc_.set_bitrate(...)`, once per loop
  iteration, holdoff `now + 500 ms`;
- `app/main.cpp` → `venc_.request_idr(...)`, reached from inside the **air RX
  dispatch callback**, and it does **not** consult the shared `no_retry_until_ms_`
  — it has an independent 1 s gate.

While blocked: `service_air` does not run (ARQ NACKs unserved), the §7.2
quiet-gap guard is meaningless, and — the one that loses the aircraft —
`csa.tick()` is not called, so a TSF-anchored retune can fire late while the
ground issuer has already committed. Craft and ground end up on different
channels.

A dead venc is harmless (loopback `ECONNREFUSED` is instant). A *wedged* one is
not.

Fix direction: non-blocking connect + a state machine polled from the loop; at
minimum, move the venc push below `csa.tick()` and give `request_idr` the shared
holdoff.

### B2 — `fork()` + `execvp()` + untimed `waitpid()` on the flight loop — BLOCKER

`io/src/air_mon.cpp` `iw_set_freq()` and `run_cli()` both do
`waitpid(pid, &status, 0)` with **no timeout**, called synchronously from the
event loop via `AirBackend::retune_all` on the CSA commit/revert/abort paths and
from `MonAir::recover()` (five sequential forked CLIs including `ip link down` /
`up`) on the Pass 80 RX-liveness guard.

Two compounding problems:

1. **No timeout.** CLAUDE.md itself records that RTL88x2 USB wedges need a
   physical re-plug. If `ip link set up` blocks in the kernel on a wedged USB
   device, the flight loop blocks *forever*.
2. **`std::signal` gives `SA_RESTART` semantics on glibc**, so a SIGTERM
   delivered during that `waitpid` **restarts it** — the operator cannot even
   kill the process out of the hang. Nothing in `io/` uses `sigaction`.

During recovery the vehicle transmits nothing, reads nothing, emits no stats and
services no ARQ — by design, briefly. The unbounded case is the defect.

Fix direction: `waitpid(WNOHANG)` poll with a hard deadline + `SIGKILL` of the
child, driven from the event loop; switch to `sigaction` without `SA_RESTART`.

### B3 — A dead RX adapter is silent — SHOULD-FIX

`io/src/air_radio.cpp` catches the exception, prints one stderr line, and the
thread exits. Nothing in `AdapterStats` says "this ear is dead"; `rx_frames`
merely stops advancing, which is indistinguishable from a quiet channel. On the
monitor backend an error return is swallowed by `if (n <= 0) continue;` with no
backoff and no counter — a persistently-erroring socket spins unthrottled, and a
permanently-blocking one is equally invisible.

With diversity, this stays hidden until the *second* adapter dies.

### B4 — Exceptions from the radio backend can terminate the process — SHOULD-FIX

The code demonstrates devourer throws: `RadioAir::read_tsf` wraps `ReadTsf()` in
try/catch ("control transfer raced the RX bulk load") and `StartRxLoop` is
wrapped. But `inject`, `inject_resend`, `inject_return`, `set_tx_mode`,
`set_power_qdb`, `retune` and `reapply_tx_power` are **not**, and neither
`run_tx` nor `run_rx` has any `try`. A throw on USB failure mid-flight is an
uncaught exception → `std::terminate`.

(Now partly mitigated: the Pass 88 respawn supervisor restarts the craft. It
should not be the primary defence.)

### B5 — frame-SHM ingress wedges permanently on an oversize frame — SHOULD-FIX

`app/main.cpp` allocates a fixed `kFrameRingDefaultSlotSize` (512 KiB) read
buffer; `io/src/frame_shm.cpp` returns `-1` without advancing the read index when
`len > cap`, commenting "caller retries with a bigger buffer". **The caller never
does**, and `FrameShmRing` exposes no accessor for the producer's declared slot
size. If `waybeam_venc` creates `venc_frame` with a larger slot size, the first
oversize frame stops the read index **forever** — video ingress dies for the rest
of the flight, with `shm_bad_slots` as the only silent numeric symptom.

Fix direction: size the buffer from the attached ring's geometry, and log once on
`-1`.

### B6 — Unbounded UDP drain loops — SHOULD-FIX

`io/src/udp.cpp` (`for (;;) { recv_one(...) }`, twice) and two
`while ((rn = cache_*_sock->recv_one(...)) > 0)` loops in `app/main.cpp` have no
iteration cap. The ingress sockets are plain bound UDP with no source filter
(the BPF in `udp.cpp` is never installed on the real path — `reject_originator`
defaults to 0). Any host that can reach the node's RTP or cache port can hold the
flight loop inside these drains indefinitely.

Fix direction: cap iterations per poll pass (~64) and let the loop breathe.

### B7 — Control plane is unauthenticated, and a shipped sample binds it to `0.0.0.0` — SHOULD-FIX

`examples/config.radio-rx.sample.json:80` → `"control": {"bind": "0.0.0.0:8091"}`.
`io/src/control_server.cpp` has no auth of any kind. Exposed writes include
`POST /api/v1/csa` (moves the whole fleet's channel),
`/api/v1/vehicle/command` (remote ARQ off, FPS),
`/api/v1/scout/quickconnect` (claim a craft) and `/api/v1/link/profile`.

The `csa_psk` redaction boundary itself holds — but that is irrelevant when the
issuer's own API is open to the LAN. Listed on `ROADMAP.md` as planned.

### B8 — A CSA hop can strand the craft while the ground reports success — FIXED (Pass 89)

Found on hardware 2026-07-24, hop 4 of the Pass 80 soak (see C1). **Not a
regression from the Passes 81–88 PR**: the two sides' windows are equal at the
deployed config, so the Pass 86 `min()` is a no-op here.

The two ends of a §11 campaign confirm on *different evidence*:

- The **craft** (`core/src/csa.cpp:151-174`) opens its verify window at its own
  landing and reaches `kCommitted` only by **hearing the ground** — valid
  traffic or a §11.6 beacon — within `verify_timeout_ms`.
- The **ground** (`core/src/csa.cpp:333-346`) opens its window at
  `max(now, switch_at_us_)` and declares `kSuccess` on `video_seen_` alone.

But the craft transmits video throughout its VERIFY window, *before* it has
decided to stay. So a ground whose `iw` retune lands late still harvests that
pre-revert video, logs `csa: campaign confirmed`, and holds the new channel —
while the craft's window expires unheard and it reverts to `prev_chan`.
`video_seen_` is not proof the craft committed; there is no "craft COMMITTED"
signal anywhere in the issuer's confirm path.

Fixed by **Pass 89** (`CSA_ARMED` spans the campaign; the issuer latches
video-verify only on an ARMED-*clear* craft frame) and re-anchored by **Pass 92**
so both ends measure the window from the same event. Verified: 21 campaigns
across the two Pass 80 runs, 0 splits.

Note for the archaeology below — "the 150 ms default (neither deployed config
overrides it)" was more literally true than it read at the time: the §15.2
default was *also* 150 and silently overrode the engine's, which is Pass 92's
first defect.

Observed, with both `verify_timeout_ms` at the 150 ms default (neither deployed
config overrides it):

```
craft : csa: armed -> 5745 MHz (nonce 14, dt 149 ms)
        csa: VERIFY -> 5745 MHz
        csa: IDLE -> 5805 MHz                     <- reverted
ground: csa: commit -> 5745 MHz
        csa: campaign confirmed -> 5745 MHz       <- stayed
```

Result: ground on 5745, craft on 5805, ground believing the fleet is fine.
Recovery took an operator re-scout plus re-claim. Hops 1–3 passed at 1.0 s
commit; this is a marginal-timing race, which is exactly why it survived 21
campaigns earlier the same day.

The craft's own §11.6 Pass 80 guard worked correctly on the way back
(`RX SILENT 750 ms after retune to 5805 MHz — half-applied retune, monitor
re-init` → `RX-liveness recovery on wlan0 -> 5805 MHz ok`), so the craft never
went deaf. The defect is purely the confirm asymmetry.

**This is a spec question, not an implementation choice** — §11.6's success
criterion is what needs the ruling, so it is the operator's call, not mine.
Candidate directions, all needing a ruling:

- require a positive craft-side commit indication before `kSuccess`;
- have the ground adopt the craft's revert (it can see the craft reappear on
  `prev_chan`) rather than holding a channel the craft left;
- widen `verify_timeout_ms` on both ends so the ground's shell-out retune fits
  inside the craft's window with margin — mitigation, not a fix.

Until ruled on, a channel switch can strand the aircraft on the old channel
with the ground station reporting success.

### B9 — A craft reboot silently disables CSA on a claimed ground — BLOCKER

Also found on hardware 2026-07-24, and also **pre-existing** — reproduced on
both the pre-PR craft binary and the Pass-88 build.

`POST /api/v1/csa` returns `{"ok":true}`; the ground logs
`csa: aborted (no CSA_ARMED)`; the craft's `csa_state` never leaves `IDLE`. The
link is otherwise perfect — video and audio delivering, `report_age_ms 0`, no
stalled adapter. Channel switching is simply dead, with no error anywhere.

In announced-token mode (`csa_psk` unset — how the fleet is deployed) the
issuer's key is set in exactly one place: `do_claim`
(`app/main.cpp:3802-3809`), from `discovery.token_for(orig)`. The
`POST /api/v1/csa` handler (`app/main.cpp:3916`) never re-keys. The craft
regenerates its announced token every boot, so after a craft reboot the ground
signs with the previous boot's token, the §11.4 MAC check fails, and
`CsaFollower::on_csa` returns false at `core/src/csa.cpp:60` — a silent drop by
design, no counter, no log. `issuer.start()` only rejects an *empty* key, so a
stale-but-present key sails through and the handler reports success.

The §3.12 comment at `app/main.cpp:1174-1179` already states the intent — "a
rebooted ground can re-learn the token and re-claim in place". The re-learn
happens; the re-key does not.

A second defect surfaced in the same sequence: `do_claim` takes the craft's
channel from `scout.candidate_for(orig)` rather than live discovery, so a claim
against a stale candidate retunes the ground's ears to the craft's *old*
channel and then aborts. The rollback is clean (no strand), but nothing tells
the operator that a re-scout is what is missing.

Needs, in priority order:

1. A §15.3 counter for MAC-rejected CSA copies, so an abort is diagnosable.
   §11.4 makes rejection deliberately silent to deny an oracle, so whether to
   expose this is an operator ruling.
2. The ground should detect that its cached token no longer matches the craft's
   live ANNOUNCE, and either re-key or fail the campaign with a real error
   instead of `{"ok":true}`.
3. `do_claim` should prefer live discovery for the craft's current channel, or
   fail with "stale candidate, re-scout".

Interim operating procedure: **after any craft reboot, re-scout and re-claim
before relying on channel switching.**

### B10 — A following RX node reports its startup channel forever — SHOULD-FIX

Reported from the field 2026-07-24: the `.199` ground's OSD shows
`CH5805 CSA:COMMITTED` and never updates when it follows a channel switch. The
x86 ground at `.242` updates correctly.

`.242` is correct only because it is the **issuer**. On an RX node the follower
path retunes the radio and updates nothing else (`app/main.cpp:4342-4345`):

```cpp
const CsaAction fa = follower.tick(now_us_it);
if (fa.kind != CsaAction::Kind::kNone) {
    air.value->retune_all(fa.chan_mhz, fa.bw, fa.fast);   // radio moves...
}                                                         // ...operating_chan does not
```

`operating_chan` is assigned in exactly one place — `operating_chan = ia.chan_mhz`
at `app/main.cpp:4252`, inside the **IssuerAction** switch — and it is what feeds
`link.channel` into §15.3 at `app/main.cpp:4416`. So any node that *follows* a
campaign rather than issuing one keeps reporting the channel it booted on. The
follow itself works: the radio moves and video keeps flowing. Only the report
is wrong.

The sticky `CSA:COMMITTED` is a separate and largely intended effect — §11.5a
holds the follower in `kCommitted` until reboot, so the field is accurate but
uninformative. The channel number is the actual defect.

Fix is a one-liner (mirror the issuer's assignment on the follower branch), but
it is deliberately **not** folded into the Passes 81–88 PR: `link.channel` is
Pass 76 §15.3 surface with a golden-tested schema, and this compounds with
**B8** — during a split, a follower's own display cannot tell the operator where
it actually is. Both want settling together.

---

## C. Verification still owed

### C1 — Pass 80 10× alternating CSA soak — MET

`docs/review-log.md` Pass 80 requires it and no doc records the run. It guards
the worst in-flight scenario: craft TX moves, RX goes deaf, the craft cannot hear
a re-claim, and recovery needs a restart that is impossible airborne. The wedge
is **intermittent**, so a single successful hop proves nothing.

Must be re-run against the **corrected** `MonAir::recover()` — the pre-Pass-88
sequence omitted `otherbss` and `txpower auto` and could "recover" a radio that
was still deaf or mute.

**Run 2026-07-24 against the Pass-88 craft build: 3/4 hops, then stopped.**
Hops 1–3 passed every check (commit 1.0 s, craft RX advancing, `report_age_ms 0`,
video and audio advancing). Hop 4 stranded the fleet — see **B8**. The soak
driver halted on the failure rather than continuing, which is the correct
behaviour but means hops 5–10 were never attempted.

Two things the run did establish:

- The corrected `MonAir::recover()` **works**. Hop 4's return retune landed
  half-applied and the guard caught it:
  `RX SILENT 750 ms after retune to 5805 MHz — half-applied retune, monitor
  re-init` → `RX-liveness recovery on wlan0 -> 5805 MHz ok`. The craft never
  went deaf. That is the specific thing Pass 80 exists to guard, and it held.
- The Passes 81–88 PR introduces **no CSA regression** — the pre-existing
  failures in B8 and B9 both reproduce on the older craft binary.

**MET 2026-07-24: 20/20 on the branch head (`impl: Pass 92`, ground
`dbb229b3…` / craft `75e0ea1d…`).** Two independent 10-hop runs, both clean.
Every hop committed in 1.0 s with craft RX advancing, video and audio
advancing, `report_age_ms 0` and **zero** recovery fires. Across the two runs:
21 campaigns armed, 21 reached VERIFY, **0 follower reverts, 0 issuer reverts,
0 RX-liveness recovery fires**.

This supersedes an earlier 20/20 recorded against the Pass 90 implementation
commit (`d00ca67`), which two later code changes invalidated. The rule that
produced this correction is worth keeping: **any code change invalidates the
previous soak.** Both re-soaks that followed found a real defect the previous
"passing" run had not exercised.

It took four rulings to get here, and the order matters for anyone reading
this later:

- **Pass 89** made failure *safe*. Before it, a hop that the craft abandoned
  left the ground holding the new channel and logging `campaign confirmed`
  (B8). After it, both ends revert together.
- **Pass 90** made failure *rare*. The residual ~1-in-5 hop failure was not a
  retune problem at all — the craft was never receiving the campaign. Copies
  now repeat until the craft ACKs and ride the §7.2 quiet gap.
- **Pass 92** made it *not happen*. The stubborn ~1-in-3 residue was two
  separate faults wearing one symptom. First, the 500 ms verify window Pass 89
  ruled had never reached a binary — the §15.2 config default restated 150 and
  is copied over the engine's, so every "500 ms" soak since Pass 89 had in fact
  measured 150. Second, the two ends anchored the same budget on different
  events: the follower from its own landing, the issuer from T_switch, so the
  issuer abandoned the craft a full craft-retune-cost early. Re-measured over
  27 instrumented hops; see Pass 92 in `docs/review-log.md` for the table.

An intermediate hypothesis — that the issuer needed the craft's own
RX-liveness guard — was **tested and refuted** before being acted on; see
Pass 90 in `docs/review-log.md`. Sampling both ground adapters at 20 Hz
through a failing hop showed them completely static, so there was no retune
to recover.

On the strength of this result: at the historical failure rate a clean 10/10
would occur by chance about 11% of the time, so one run was not sufficient
evidence and the soak was repeated — 20/20 consecutive is ~1%. The earlier
soaks are the control: the identical driver failed at hop 4 twice against the
pre-Pass-90 binaries, and at hop 3 against the Pass 91 binary.

### C2 — Gate 4 range validation — SHOULD-FIX

`docs/step11-bench.md`: the 300/2000 µs seeds are desk-validated only;
"range-sensitive return-path and adaptive-loop stability remain to be validated".
This governs the craft's single-radio return path — the thing most likely to
misbehave at 500 m.

### C3 — §4.3 CSA on real TSF — never run

`docs/step11-bench.md` §4.3 has no STATUS/RESULT line, unlike §4.2/§4.4/§4.5/§4.6.
The `dt_to_switch_ms` class 0/1 seeds (150/500 ms) are still paper-derived against
wfb_ng precedent. Pass 69 later measured cross-band set-freq at 117/65/30 ms, but
that was an incident investigation, not the per-class budget check.

### C4 — Pass 78/79 conclusions rest on one channel

The Pass 79 ruling itself orders re-verification on 149 / 5745 MHz — and the
attempt to do so is what uncovered the Pass 80 CSA wedge. AUDIO (Pass 77) has
never flown, and it is the feature that destabilised the selector twice in three
days (Passes 78, 79). Both grounds and the vehicle carry stream 1.

### C5 — Uplink HW-ACK is devourer-gated

Validated on the desk only, and Pass 78 confirms it is unavailable on the
deployed kernel-monitor craft. The deployed fleet runs the ~45% report-heard
baseline, mitigated only by `report_redundancy=2`.

### C6 — Pre-flight items already prescribed but not done

`docs/verification-hardware.md` and `deploy/README.md` both list a props-off
power-cycle test and an antenna-separated walking/range test as *to do*. That
walk is also the missing gate-4 range sample (C2).

---

## D. Doc drift found during the audit — NICE-TO-HAVE

- `deploy/README.md`: "profile/MCS 3" — the vehicle config has been adaptive
  `min 1 / max 5` since #47.
- `deploy/README.md`: "the configured 27 dBm vehicle TX power" — nothing in
  `deploy/vehicle-192.168.2.232.json` configures power, kernel-monitor power
  actuation is a documented no-op, and `mon-up.sh` sets `txpower auto`. The
  regulatory claim is unenforced.
- `deploy/README.md`: says the vehicle link is deliberately not linked into rcS;
  the craft now boots into waybeam-link via S96 (and S96 is not in this repo).
- `ROADMAP.md`: still lists Passes 64–66 as active work; they merged.
- `docs/transport-architecture-review.md`: still says kernel-monitor CSA is "not
  implemented; now rejected" and that monitor has no wedge detection — both
  superseded by Passes 49, 69, 80.
- `CLAUDE.md`: "a TX node sends nothing without an RTP feed" is now half true —
  1 Hz HEARTBEAT and 2 Hz ANNOUNCE fire regardless. The *video*-only issuer
  confirm is what keeps the Pass 70 asymmetry alive.
- `CLAUDE.md`: says 43 test suites; there are 46.
- `examples/config.radio-{tx,rx}.sample.json` and `docs/step11-bench.md` still
  carry `rendezvous_timeout_s`, removed by Pass 59. Unknown config keys are
  silently ignored (`io/src/config.cpp` validates values, not key names), so a
  typo in a flight config runs with defaults and `--check` passes.

---

## E. Smaller items — NICE-TO-HAVE

- `app/main.cpp` `build_info_json` concatenates adapter names into JSON without
  escaping; a name containing `"` yields malformed `/api/v1/info` output.
- `io/src/config.cpp` "JSON parse error: " + `e.what()` — nlohmann's message
  embeds the offending input, so a syntax error near the `csa.psk` line can echo
  part of the secret to stderr. Every other output path redacts correctly.
- `stats.hz > 1000` makes `static_cast<uint64_t>(1000.0 / hz)` zero, which
  silently disables stats entirely.
- `quietgap_policy` never sets `p.skip_backlog`, so that knob is dead and the
  header default of 32 always applies.
- `io/src/air_mon.cpp` issues a `setsockopt(SO_PRIORITY)` on **every** injected
  frame, and `counters()` does an open/read/close of a sysfs file per adapter per
  stats tick.
- The RECOVERY_REQUEST stderr line has no rate limit; stderr is unbuffered, so a
  looping ground becomes one `write()` syscall per packet.
- `io/src/air_radio.cpp` startup failure paths after `libusb_init` skip
  `libusb_close`/`libusb_exit`. Process exits immediately, so harmless today.
- `core/src/cache_store.cpp` `requesters_` is an unbounded map keyed by
  on-air originator, with no §13 enumeration cap (`RxEngine` has one). Mitigated
  today by §14.3 being IP-transport-only in v1.
- `core/src/rx.cpp` `note_adapter_seq` never re-anchors downward, so
  `loss_prediversity` can freeze for an adapter after a resync. Stats-only.
- `core/src/ring.cpp` runs the byte-budget eviction loop *before* the
  `len > byte_budget` sanity check, so one pathological oversized frame flushes
  the entire resend ring before being rejected.
- `core/src/rlc.cpp` Gauss-Jordan is O(k²·s) ≈ 268 M byte-ops per decode at
  k=256, unbudgeted, on ARMv7. FEC is default-off, so this is a future cliff.
- `core/src/csa.cpp` TSF elapsed is unbounded: a garbage/reset TSF delta ≥ `dt_us`
  collapses `switch_at_us_` to `now_us`, retuning up to 150 ms early. Clamp and
  count.

---

## Checked and clean

Recorded so the next audit does not re-derive it:

- **Time**: `steady_clock` only — no `system_clock`, `gettimeofday` or
  `CLOCK_REALTIME` anywhere in `io/` or `app/`. Both loops take one timestamp per
  iteration; `heartbeat`/`announce` guard against backward steps.
- **Signal handler**: `on_signal` writes only a `volatile sig_atomic_t` —
  async-signal-safe. (The `SA_RESTART`/`waitpid` interaction is B2.)
- **Long-flight growth**: every long-lived container is bounded — ARQ maps trim
  at 4096, `Series::recent_` at 512, `DiscoveryCatalog` at 64 with 5 s ageing, RX
  queues at 512 with drop-oldest accounting, `ControlServer` at 16 conns / 8 KiB
  with a 2 s slow-client drop.
- **frame-SHM SPSC protocol**: memory ordering, producer-replacement (dev/ino)
  detection, reader-thread lifetime and the "don't unlink someone else's
  replacement" teardown are all correct.
- **RX-thread ↔ main-loop sharing**: relaxed atomics on counters, mutex-guarded
  queue, correct acquire/release on `flush_gen`, separate mutex for `sa_latch`,
  and no lock nesting anywhere — no lock-ordering hazard exists.
- **fd hygiene**: every `Result::fail` path in `udp.cpp`, `cache_udp.cpp`,
  `control_server.cpp`, `frame_shm.cpp` and `air_mon.cpp` closes what it opened;
  the socket wrappers are correct move-only types.
- **`csa_psk` redaction** holds on every output path except the parse-error string
  noted in E.
