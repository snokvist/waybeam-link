# Findings

The **Tier-2 channel** (see `CLAUDE.md`, "The law"): dated notes on anything
still being measured — loss walls, gates, dwell counts, seeds, sweep bounds,
estimator behaviour. A finding records evidence and an open question; it never
amends `PROTOCOL.md`. When a mechanism settles, ONE spec amendment plus a
numbered Pass in `review-log.md` closes it out, citing the finding.

Format per entry: date, title, what was measured (setup + numbers), what it
means, what stays open. Newest first. Delete or strike entries a later Pass
has closed, with a pointer to the Pass.

---

## 2026-08-31 — the scout dropped a craft it could plainly hear (4/8 -> 8/8)

Craft **19** (`.181`, -50 dBm, 5700 MHz at ~920 permille interference) appeared
in only **4 of 8** sweeps using the WebUI's exact call (`mode:list`, no
`dwell_ms`). Crafts 17 and 18 were 8/8. `rejects` read all-zero on every miss.

**A wrong mechanism, recorded so it is not re-derived.** First hypothesis: the
8812AU is deaf ~220-270 ms after a retune, the base dwell defaults to 300 ms,
so the extension gate — `accum_.frames > 0`, which decides whether a channel
earns the 1500 ms window its ANNOUNCE needs — was being decided on ~30-80 ms of
real listening. Plausible, and false. Charging a 250 ms settle to the sweep
instead of the listening window moved the rate **not at all**: 4/8 before, 4/8
after, on the same craft and geometry. Reverted rather than shipped, since it
cost ~6 s per 25-channel sweep for no measured benefit.

**What the measurement actually said.** Probing the per-channel record on a
MISS sweep: 5700 came back `tuned=true, evidence_valid=true,
wifi_util_permille=256, bss_count=1`. `wifi_util` is accumulated inside
`on_frame` from decodable waybeam frames on the scout adapter — so on a sweep
where the craft was "not found", the scout had heard a quarter of that
channel's airtime in that craft's own frames. The dwell extended. It still
produced no candidate.

**Cause.** `Candidate` is only ever constructed inside
`if (const Announce* an = std::get_if<Announce>(&dec))`
(`node/include/wblink/node/discovery.h`). Presence is fully known from ANY
frame — `frames_by_orig`, `best_rssi_by_orig`, and the originator read straight
out of the §3.1 prefix — and is discarded unless a §3.12 ANNOUNCE also lands.
On a weak craft on a saturated channel the announce is precisely the frame most
likely to be missing, so the scout knew where the craft was and refused to say.

**Fix and result.** Emit heard-but-unannounced craft with `announced:false` and
session/claimed/psk_known left at their unknown defaults:

| craft | before | after |
|---|---|---|
| 17 (-18 dBm) | 8/8 | 8/8 |
| 18 (-42 dBm) | 8/8 | 8/8 |
| **19 (-50 dBm, congested)** | **4/8** | **8/8** |

And it is switchable, not merely visible: quickconnect to craft 19 while it
listed `announced=false, psk_known=false` reached `committed|19|5700`. The CSA
token survives in `DiscoveryCatalog` from an earlier announce (the token cache
has no aging, only a 64-entry cap), so the claim keys fine.

Three containments, because this touches a shared structure:
- **Occupancy is untouched** — only `announced` rows may invalidate a channel
  in `channel_evidence_valid`, so which channels fold into the occupancy store
  is unchanged. That subsystem has its own evidence rules.
- **The claim path refuses precisely** — `do_claim`'s staleness check now runs
  only for announced candidates. An unannounced one carries `session 0`, which
  that check would have reported as "craft rebooted since scout": the right
  refusal for the wrong reason.
- **The API distinguishes them** (`announced`), so a picker can grey the row
  instead of implying full metadata.

CLOSED 2026-08-31 — ONE stale config line explained all of it. `.181`'s
`craft.json` pointed `profile_table` at `/etc/waybeam-link/table-8733b.json`
(Aug 15) while the ground and every other craft had moved to
`table.example.json` (Aug 30). Both files exist on both nodes, byte-identical
across them, so this was never a deploy or binary problem — I rebuilt and
redeployed the cv610 hub first on a "stale binary" theory and `table_version`
stayed at 164, which is what ruled that out.

The two tables differ in `profile 4`'s `airtime_budget_frac` (0.51 vs 0.6),
in whether the §9.4 MCS probe block is armed, and in two `_note` strings —
and the notes alone are enough, because §3.6 hashes the whole table into
`table_version` ("edit in lockstep fleet-wide").

The mismatch put stream 0 into §3.4 BEST-EFFORT, which disables ARQ,
supersession and deadline drops. Repointing `.181` at the common table:

| | before | after |
|---|---|---|
| `table_version` | 164 | **242** |
| BEST-EFFORT log lines per latch | 2 | **0** |
| stream 0 loss (see caveat) | 394 permille | 6 permille |
| scout frames per dwell | 242 | **1417** |
| `announced` in the picker | false (4/8 sweeps listed it) | **true** |

**CAVEAT on that loss row, corrected 2026-08-31 after re-measuring:**
`/api/v1/health`'s `loss_milli` is CUMULATIVE SINCE LATCH, not instantaneous.
Both numbers are averages over different windows and are NOT comparable: the
394 included a long pre-fix period, and the 6 was a short clean window right
after a fresh latch. Re-measured later with all three crafts airing it read
373 and then decayed monotonically — 205, 167, 118, 101, 89, 79 — as good
samples accumulated, which is the signature of a cumulative field, not a
recovery. Do not quote 394 -> 6 as a steady-state improvement.

The evidence for the fix that does NOT depend on this field, and which stands:
BEST-EFFORT log lines per latch 2 -> 0 (binary), ARQ/supersession/deadline
drops re-enabled as a direct consequence, scout frames per FIXED dwell
242 -> 1417, and `announced` false -> true. See
[[hub_air_osd_bar_is_a_lifetime_average]] for the same trap in the OSD bars.

So the announce was never a cadence or 8733B-specific defect: at 394 permille
the craft simply could not get enough frames through for one to land inside a
dwell. Note `.181`'s `air.mcs_probe: true` had been INERT the whole time — that
key does nothing unless the table carries a probe block AND every receiver has
the identical table; aligning the table also activates the probe on that die.

This does not retire the Pass 200 picker change: a craft you can hear should be
listed whatever its config, and that fix is what kept the craft selectable
while the real cause was still unknown. But the operational lesson is that a
`table_version` mismatch is not cosmetic — it silently costs ARQ, and the loss
it produces looks exactly like a bad radio.

---

## 2026-08-31 — Pass 199 device matrix: an acquire parks, a retune still reverts

Ground `.242` (x86, in-process node) against three crafts: **17** = `.232`
8812EU @5540 (table 242), **18** = `.233` maruko 8812AU @5825 (table 242),
**19** = `.181` 8733BU @5700 (table 164, so §3.4 BEST-EFFORT, on a channel
measuring ~920 permille interference).

| # | scenario | result |
|---|---|---|
| S1 | quickconnect with a foreign `target_chan` | REFUSED, selection untouched |
| S2 | acquire 19→17, no `target_chan` | `committed\|17\|5540` — craft NOT moved |
| S3 | acquire back 17→19 | `committed\|19\|5700` |
| S4 | acquire FAILS (RX starved via `bench/rx-drop`) | **`select_failed\|17\|5540` — PARKED on target**, not reverted to 19@5700 |
| S4b | craft reappears after a park | promotes `select_failed` → `latched\|17\|5540` |
| S5 | **negative control**: retune fails | still REVERTS — `csa: aborted (no CSA_ARMED) -> resting 5540 MHz` |
| S6 | WebUI click path, 4 clicks over 3 crafts | 4/4 landed, every direction |

S5 is the one that matters for trust: the revert safety net is intact for a
RETUNE, so Pass 199 is scoped to the acquire intent rather than removing
revert wholesale.

**S2 is the quiet win.** With `target_chan` omitted, do_claim used to default to
`scout.emptiest()` — so "switch to craft B" also MOVED it to another channel.
Two operations, only the second able to fail, and its failure reverted the
first. Same-channel now, always.

**A second defect S4 surfaced.** Parking worked and video flowed at 6 permille,
but `selection_state` stayed `select_failed` forever, because promotion to
`latched` was gated on `selection_state == "configured"` (the boot state). The
UI would have reported a failure the operator could see was not happening.
Fixed narrowly: only the originator we parked ON promotes, so it can never
adopt a different craft that happens to share the channel.

**WebUI verified end to end**, not just the link API: the HTML's
`cc.chan || sc3.current_chan || 0` expression was evaluated against live
`/wblink/scout` data and yields each craft's own channel for all three, so a
click sends exactly what Pass 199 accepts and nothing hits the `|| 0` path.
Clicks were then issued through the hub proxy at `:8060` in the HTML's payload
shape.

**The operator workflow works: ONE scout, then click freely.** Three clicks
(19 → 17 → 18) with no re-scout between them all landed, and the candidate list
stayed complete — `[(17,5540),(18,5825),(19,5700)]` — after every switch. The
scout list persists until the NEXT sweep; a selection change does not discard
it. (An earlier read that "switching forgets the other craft" was wrong: that
was a re-scout dropping the weak craft, not the switch discarding anything.
`/api/v1/discovery` does age a silent node at 5 s, but the WebUI renders the
scout list, not that catalogue.)

### CLOSED — the scout lost the weak craft (see the 2026-08-31 scout entry above)

Craft 19 listed in only 4/8 sweeps. My first mechanism for this was WRONG and
is recorded here so nobody re-derives it: I attributed it to the ~220-270 ms
post-retune deafness eating the 300 ms base dwell, so that the extension gate
(`accum_.frames > 0`) was decided on ~30-80 ms of listening. Charging the
settle to the sweep changed the detection rate by exactly nothing — 4/8 before,
4/8 after — and the change was reverted. The real cause was the ANNOUNCE gate;
see the entry above.

---

## 2026-08-30 — hybrid on the DEPLOYED stack, and the quiet gap misses 89 %

Second session, distinct from the entry below it: that one ran the ground
standalone against a craft on a **pre-PR** binary. This one is the shipped
topology — waybeam-hub with the in-process node on **both** ends, merged
`f7f3a36`. Ground `.242` (x86, 8812AU uplink elected by `adapters:{auto}` over
a CU and a BU) ↔ craft `.232` (8812EU, originator 17, table_version 242,
5540 MHz, live ~58 fps venc feed), ~-22 dBm at roughly 50 cm, 90 s arms.
The two sessions agree within ~1 pp, which is corroboration, not a conflict.

| | control (hybrid off) | full hybrid |
|---|---|---|
| craft report delivery | 813/901 = **90.23 %** | 899/900 = **99.89 %** |
| ground `tx_report_fails` | 0 / 2790 | 89 / 2796 = **3.2 %** |
| downlink loss | 4 ‰ | 7 ‰ |

**Soak, 6 min, same PIDs both ends** (ground 3542588, craft 3511), monotonic
uptime asserted every 30 s: `unicast_fallback` 0 throughout, no new
`unicast_stale`, downlink drifting 7 → 6 ‰, `tx_report_fails` steady at
**1056/49706 = 2.1 %** cumulative. No restarts, no wedges.

**§15.5 live toggle exercised as designed:** the craft was armed over
`POST /api/v1/air/ack_responder` with **no restart**, reporting
`mac 56:42:00:00:11:00` (prefix, net_id 00, originator 0x0011, adapter 0) and
`configured:false` beside `armed:true` — which is how an operator sees that a
restart would revert it. §11.7 command campaign reached `acked` over the
now-unicast return path with `unicast_fallback` 0.

**Unplanned storm-guard validation.** Later the same evening `.232` dropped off
the network on its own (not provoked, not `bench/rx-drop`). `unicast_stale`
advanced 88 → 170 and `unicast_sent` stopped: the guard fired on a real
disappearance exactly as it did on the staged one.

### OPEN — the quiet gap misses ~89 % of returns

Measured on the running deployed stack: `return_window_hits` **15995** against
`return_window_misses` **126351** — an **11.2 % hit rate**. §7.2 exists to
schedule returns into gaps when the craft is not transmitting, and nearly nine
in ten are landing while it is pushing ~58 fps of video. That is exactly when
the craft's SIFS turnaround ACK is most likely to be missed, so it is a
candidate explanation for the 2.1 % residual `tx_report_fails`. This predates
Pass 198 — but the hybrid is what makes a miss *cost* something, so it is now
worth chasing. Not yet attributed: no test separates "missed the window" from
"ACK lost for other reasons".

### OPEN — no posture for a craft that is present but cannot answer

The stale latch gives the hybrid an out-of-range posture. There is no
equivalent for a craft that is healthy, transmitting, and structurally unable
to ACK — the `.181` 8733BU today, and every craft mid-rolling-upgrade. The
latch keeps being refreshed by the craft's own video, so the guard never
fires, and the ground pays `retry_limit + 1` copies of every return forever:
measured at **14 ‰ downlink loss against 4 ‰** in arm B. The signal already
exists — `tx_report_fails` running ~1:1 with `unicast_sent` — and nothing acts
on it. See the issue filed against this entry.

### OPEN — the hybrid is neither visible nor disarmable in flight

`GET /api/v1/health` returns `{state, profile, mcs, rssi_best, loss_milli,
delivered, csa_state}` — none of `unicast_sent`, `unicast_stale`,
`tx_report_fails` or `armed`. Distinguishing "ACK loop closing" from "degraded
to arm-B economics" means dividing two counters out of the full §15.3 stream by
hand. And `policy.return.unicast` is consumed at `RadioAir::create()`, so the
**ground** half needs a restart to disarm while the craft half toggles live.
Both are acceptable for a hand-driven bench and neither is acceptable for a
default.

---

## 2026-08-30 — hardware-ACK hybrid on air: the AU solicits, and the storm is ~5 s

First bench run of Pass 198. Ground `.242` (x86, 8812AU uplink elected by
`adapters:{auto}` over a CU and a BU) ↔ craft `.232` (8812EU, originator 17,
5540 MHz, live ~58 fps venc feed), ~-22 dBm, 90 s arms. The ground ran PR-258
code standalone; **the craft ran its pre-PR binary**, which is sufficient
because the storm guard is entirely ground-side.

**1. The Jaguar1 descriptor-BMC concern is disproven.** `RtlJaguarDevice.cpp:1224`
hardcodes `SET_TX_DESC_BMC_8812(usb_frame, 1)` unconditionally outside the NDPA
branch, and the runbook called that a blocking gate. On air, with unicast on
and the craft responder OFF, `tx_report_fails` ran **2796/2796 — exactly 1:1
with `unicast_sent`**. A frame the MAC treats as broadcast carries no ACK
policy and cannot report retry exhaustion at all, so 1:1 failures prove the AU
solicits on every frame. Arming the responder collapsed that to **48/2790
(1.7 %)**. The descriptor BMC bit does not gate ACK policy on this die.
Corroborated by OpenIPC/devourer#406, whose responder matrix uses an 8812AU as
the soliciting side. **Closes** a question open since the Pass 12 notes.

**2. The A/B.** A (off/off) 820/900 = 91.11 %, `tx_report_fails` 0, downlink
3 ‰. B (on/off) 902/902 = 100 %, fails 2796/2796, downlink 14 ‰. C (off/on)
829/901 = 92.01 %, fails 0, downlink 3 ‰. D (on/on) 900/900 = 100 %, fails
48/2790, downlink 5 ‰. Arm B says unicast **alone** reaches 100 % by brute
retransmission at 4.7× the downlink cost; arm C says arming a responder costs
a mostly-transmitting craft nothing.

**3. Retry 3 vs 8 — supports the ruling.** In the doomed-frame regime (unicast
on, responder off), interleaved: retry 3 = 13–14 ‰ downlink loss over two
runs, retry 8 = **29 ‰**, reports 100 % either way. So 8 costs ~2.2× the
airtime of an unanswered return and buys no delivery. Seed 3 stands on
measurement now, not just on the devourer sweep. 4 and 5 remain unmeasured.

**4. The storm guard holds, and the storm is smaller than argued.** Provoked
with `POST /api/v1/bench/rx-drop {"permille":1000}`, which drops at
`air_radio.cpp:650`, before `latch_sa` at `:673` — so the latch starves as it
would for a departed craft, with no craft-side action. Guarded
(`unicast_stale_ms: 1000`): `unicast_sent` advanced **+2** after the last heard
frame then froze, `tx_report_fails` froze with it, 84 returns aired as
broadcast fallback, age-out ~1.0 s, re-latch automatic within 1 s of release.
Control (`unicast_stale_ms: 0`): `unicast_sent` **+83**, fails climbed 1:1 to
355. Derived post-loss airtime 92 vs 332 airings (3.6×); soliciting frames 2 vs
83 (41×).

**OPEN / corrected:** the unguarded storm is **not unbounded in time** as §3.0
argued — it self-terminated after ~5 s when the ground's own return generation
stopped for want of RX. The guard is still right (an incidental ~5 s tail
becomes a controlled ~1 s one) but the "forever" framing was wrong; §3.0,
`review-log.md` and the runbook now say so. What a *fly-away* costs, where the
craft is intermittently heard rather than cleanly gone, is still unmeasured and
could be worse than either arm here.

**Still unverified on hardware:** the craft-side §15.5 endpoint and the
`create()` boot-arm refactor (craft ran pre-PR code); §11.7 VEHICLE_CMD via
`inject_return`; and the `.181` 8733BU loud-degrade path.

---

## 2026-08-30 — hardware-ACK hybrid seeds: retry 3, stale 1000 ms, ACK window 128 us

Pass 198 turns the Pass 12 hybrid into the return path's ARQ. Three of its
numbers are SEEDS, not measurements, and are recorded here rather than argued
in the spec.

**`air.tx_retry_limit` 3 (was 8).** Operator-ruled band 3-5; 3 is the only rung
inside it the devourer sweep actually measured (99.72% delivered, 0.26%
residual, vs 8's 99.97%/0.03%). 4 and 5 are authorable and unmeasured. The
trade is airtime per UNDELIVERABLE frame — 4 copies rather than 9, each retry
also waiting a full ACK window. **Open:** the delivery cost of dropping from 8
to 3 has never been measured on OUR return path, only in devourer's collision
regime; and finding #96 still stands — at bench SNR the retry distribution is
degenerate (`tx_report_fails` 0, the ceiling never touched), so the number that
matters only appears on a marginal link.

**`policy.return.unicast_stale_ms` 1000.** Derived, not measured: an order of
magnitude above the §1 correlated-fade band (~5-30 ms) so a fade the hybrid
exists to punch through cannot disarm it, 10x the §7.3 LINK_REPORT interval,
and an order of magnitude below §2 `idle_teardown_ms` so the latch always ages
out before the stream is torn down. **Open:** the real question is the worst
fade a healthy link produces at range, which is a gate-4 measurement. Watch
`unicast_stale` rising while `unicast_sent` stalls — that pair IS the
out-of-range signal, and a nonzero `unicast_stale` on a link nobody considers
out of range means the seed is too tight.

**`air.ack_timeout_us` 128.** Not a new value — devourer's own unified default,
so this changes nothing on air. It becomes a key because the number is a range
budget (~6.7 us/km round trip, so ~15 km once ~50 us of ACK flight and
detection margin is out) that used to be inherited silently. Operator ruling:
128 is the fleet value and a longer link leans on §3.4 fallback video.
**Open, and it is a real one:** the fleet's `.181` craft is an **8733BU**,
where devourer hardcodes REG_ACKTO to 0x21 = **33 us** in `init_wmac()` and
never reads `tx.ack_timeout_us` at all (snokvist/devourer#2). That die also has
no ACK responder and no CCX `tx.report`, so on the `.181` craft the hybrid's
craft half cannot be armed and the per-frame retry-drop sensor does not exist.
Today that costs nothing — the craft is the responder, not the solicitor, and
the responder half is what is missing — but any future craft-side unicast on
that die would run a 33 us window without saying so.

**Not measured at all yet:** the whole pass is bench-unvalidated. §4.4's
86.9% -> 99.9% at 3000 pps was measured with retry 8, a never-expiring latch and
NACK/LINK_REPORT only. Every one of those three has changed.
## 2026-08-30 — the §3.7 loss ratios are session averages, and AIR is mean-per-EAR

Operator: the OSD **AIR** bar shows full while the picture is fine, and
"degrades very slowly and is not live". Investigated on the x86 ground against
craft 19. Three separate facts, none of them a wrong OSD binding.

**1. Both ratios are cumulative since latch.** Sampled once every 10 s for 90 s
on a running link:

| t | pre_pct | post_pct | delivered |
|---|---|---|---|
| 0 s | 33.6 | 0.2 | 73,269 |
| 90 s | 33.4 | 0.2 | 161,633 |

88,000 frames delivered moved the headline number by **0.2 points**. The
inertia grows with the stream, so late in a flight the bars are effectively
frozen. A scout sweep is the pathological input: it parks one ear off-channel,
and that 100%-loss stretch stays in the mean permanently.

**2. AIR is the mean across diversity EARS, not "the air".**
`RxEngine::streams()` sums `prediv_expected` / `prediv_lost` over every
adapter. With a good ear at -49 dBm and a weak one at -56, that is a permanent
~33% while post-diversity is **0.2%** and the picture is clean. The OSD's
`warn 5 / alert 12` were calibrated when a ground had one ear; §15.2
auto-adapters made multi-ear grounds normal, which is what turned AIR into a
bar that is always red and therefore carries no information.

**3. `wblink_loss_window_pct` / `wblink_loss_window_milli` are not a substitute.**
They exist in the hub's key list but read 0.0 / 0 on a ground — they come from
the §9.8 selector state, which is TX-side.

**Fixed by Pass 198** for (1): §15.3 gains `loss_prediversity_window_milli` and
`loss_postdiv_window_milli`, a 500 ms trailing window. (2) is a presentation
decision left to the operator — the windowed pre-diversity figure is still
mean-per-ear, it is just live. What windowing DOES fix for (2) is the scout
poisoning, which was the dominant term in the 33% seen here.

**Window length is a seed, not a measurement.** 500 ms at ~1400 pkt/s is ~700
opportunities, so quantisation is ~1.4 permille — fine. At a floor-rung 2829
kbps (~250 pkt/s) it is ~125 opportunities and ~8 permille, which will look
jumpy. Raise it if that proves distracting; the cost is reaction time.

## 2026-08-30 — the CSA retune class, measured: class 1 loses 40% of campaigns class 0 wins

The `.242` x86 ground (2 ears: 8812CU rx + 8812AU tx, auto-elected) against the
`.181` CV610 craft (8733BU) on 5700 MHz. **Same-channel** campaigns —
`POST /api/v1/csa {"mhz":5700,"class":K}` while already resting on 5700 — so the
retune distance is zero on both ends and `retune_class` is the only variable.
20 campaigns per arm, 6.5 s apart (`min_interval_s` is 5).

| retune_class | dt budget | confirmed | reverted |
|---|---|---|---|
| 0 | 300 ms | **20** | 0 |
| 1 | 500 ms | 12 | **8** |

Verdict latency was 0.90 s for every class-0 confirm, and 1.01 s (confirm) /
1.12 s (revert) at class 1 — the extra 200 ms is the dt difference, so the
campaigns are running the arithmetic §11.2 describes.

**This under-states it.** The operator's actual failure was `quickconnect`,
which is a CROSS-channel move (ground on 5805 or 5540, craft on 5700) and was
observed reverting **7/7** before the class was changed. Same-channel was chosen
here to isolate the class; the real path adds retune time on top.

**What class 1 costs that class 0 does not.** `T_switch - commit`: the issuer
commits the instant the craft ACKs (Pass 69 pre-position) but the craft does not
leave the old channel until T_switch, so the issuer sits alone on the target for
~400 ms at class 1 against ~200 ms at class 0. That silence is outside
`verify_timeout_ms` and inside the §11.6 `rx_liveness_ms` guard (seed 750). A
backend re-init mid-campaign was observed directly: `GET /api/v1/stats` adapter
`rx` froze and every stream counter reset to 0 about 6 s after a quick-connect.

**Second run, degraded uplink (same day, after the ground's 8812AU TX ear
partially wedged — 32 pkt/s against the 8812CU's 1378 on the same channel).**
The asymmetry got *sharper*, not noisier: **class 0 confirmed 6/6, class 1
reverted 4/4.** Running total 26/26 vs 12/24. The §11.6 revert reason added in
this Pass named it on every class-1 attempt: `armed=1 landed=1 video=0` — the
craft ACKed and the issuer saw it land, but no `CSA_ARMED`-**clear** frame
arrived before the deadline, which is Pass 89's commit proof failing because
the craft needs the (impaired) uplink to reach COMMITTED. Three seconds to read
what previously took a source dive.

**Not isolated:** whether the re-init is the mechanism for every one of the 8
class-1 reverts, or only for the long tail. The A/B settles which class to ship;
it does not settle why each individual campaign failed. The §11.6 revert-reason
log added in the same Pass is what would answer that on the next occurrence.

## 2026-08-30 — a §9.3 table mismatch costs ARQ, not just the §9.4 probe

Craft `.181` ran `table-8733b.json` (tv **164**), the `.242` ground
`table.example.json` (tv **242**). The divergence is *deliberate* and recorded
above (2026-08-21: rung-4 `airtime_budget_frac` 510 vs 600, the CV610 craft's
clean point never measured) — the finding here is what it silently costs.

§3.4 drops a mismatched stream to best-effort, which suspends **NACK generation,
§6.2-2 supersession and deadline drops**. Measured on the ground while latched:
`nacks_sent 0`, `recovered_arq 0`, and `frames_unrecoverable` climbing to 11.
Temporarily aligning the two tables and re-latching: **0 unrecoverable in 45 s**
at 50.6 fps, everything else unchanged. Raw radio loss did not move
(`loss_prediversity_milli` 15-16 either way) — the fallback was costing recovery,
not reception.

**Nothing said so.** `RxStreamInfo::best_effort` existed and was populated but
had no consumer; `counters.table_mismatch` never left `core/`;
`dropped_unrecoverable` is not in `StreamStats` at all; RX-side
`StreamStats::table_version` is always 0; and `/api/v1/features` reported
`arq_enabled: true` throughout, because that field is the §11.7 operator latch
and cannot see the engine. Pass 197 adds §15.3 `best_effort` / `table_mismatch`,
the §15.5 `arq_effective` sibling, and a one-shot log naming both versions.

**What stays open.** (a) `best_effort` is sticky — nothing clears it but stream
teardown — so aligning tables mid-flight does not heal a latched stream. That is
deliberate (a flapping peer must not flap the profile logic) but it means the
recovery needs an explicit re-latch, and no REST call forces one today. (b) A
mixed-die fleet on one ground cannot give every craft a matching table: a ground
loads exactly one. Either per-craft tuning or ARQ, not both, until §3.4 gains a
per-peer table.

## 2026-08-30 — a RadioAir created and destroyed WITHOUT a poll hangs in teardown

Found while device-verifying §15.2 auto on the x86 bench (.242, 8812CU at
1-1). `waybeam-link adapters -c <cfg>` brought the adapter up, printed the
election correctly, and then never exited — killed at 45 s, 60 s and 90 s.

**It is not the auto path, and it is not new.** Isolated in four steps:

| binary | shape | result |
|---|---|---|
| this branch, `adapters`, auto form | create, no poll, destroy | HANGS |
| this branch, `adapters`, array form (role rx AND role tx) | create, no poll, destroy | HANGS |
| this branch, `hwtrial_bringup --bus 1-1 --seconds 2` | create, POLL 2 s, destroy | exits, 9 s |
| **main**, `hwtrial_bringup --bus 1-1 --seconds 0` | create, no poll, destroy | **HANGS** |

The last row is the one that matters: a binary built from `origin/main`, with
none of this branch's changes, hangs identically. The condition is
create-then-destroy with no `poll_once()` in between; auto, the adapter role
and the stanza form are all irrelevant.

**Where it is stuck.** `/proc/<pid>/task/*/stack` on the hung process: main
thread in `futex_wait` (a `std::thread::join`), plus a `libusb_event` thread in
`poll`, one worker in `poll` and one in `nanosleep`. So `~Impl`'s
StopRxLoop-then-join is not bringing the RX loop down.

**Likely mechanism, NOT yet proven.** `~Impl` already documents the shape of
it: *"A join can block while a bring-up is still in flight (bring-up does not
poll the stop flag)."* With a poll in between, the RX thread is well inside
devourer's loop when `StopRxLoop` arrives and sees the flag; with no poll,
create() spawns the thread and teardown starts before it gets there, so the
stop is set and cleared — or simply never observed — and the loop runs
forever. That is a start/stop race in the backend's threading contract, and
the fix is not in this tree: `third_party/` is never edited, so it is either an
upstream devourer change or an `~Impl`-side handshake (an atomic the RX thread
raises before entering the loop, which teardown waits on — and even that is not
airtight against devourer's internal state).

**What was done instead.** `waybeam-link adapters` drains for ~1 s before
tearing down, which is what every real consumer does anyway and lets the mode
report per-adapter `rx_frames`. 5/5 clean exits at 9–10 s. This is a
work-around at the one new call site, not a fix: **any future caller that
constructs a RadioAir and drops it without polling will hang**, and there is
nothing in the type that says so.

**Open.** (a) Prove the race rather than infer it — instrument the RX thread's
entry into `StartRxLoop` against the `StopRxLoop` timestamp. (b) Decide whether
the handshake belongs in `~Impl` or upstream. (c) Until then, consider whether
`RadioAir::create` should simply refuse to hand back a backend nobody can
safely destroy, or whether the poll requirement belongs on the contract in
air_radio.h.

## 2026-08-30 — the §15.2 auto TX-priority order is a seed, and it is not a ranking of "best radio"

Pass 195 gives `adapters.auto.tx_priority` a default of
`["8812EU", "8812AU", "8812CU", "8733BU"]`. That list is **tier-2 config, not
spec law**, and this entry is why: nothing in the tree has ever measured these
four parts against each other as *transmitters* under one controlled setup.
The seed is an operator preference plus the scattered evidence below, and it is
expected to move.

**What is actually known, per part, from this repo's own runs:**

- **8812EU (RTL8822E, Jaguar3)** — the craft TX on `.232` through every §9.4 and
  §14 device session. USB TX aggregation verified at `usb_tx_agg` 3. It is also
  the only part with a stage-0 proof for the §9.4 rate probe, and that proof is
  recorded per-unit (MAC `98:03:cf:cf:a4:28`), not per-part.
- **8812AU (RTL8812A, Jaguar1)** — the standing ground TX/RX combo. But
  aggregation is **broken on air** on this family: at `usb_tx_agg` 2 and 3 craft
  CPU falls ~35 points while the link goes to 966‰ loss and 0 fps (Pass 194).
  Root-caused 2026-08-30: monitor-injected multi-block frames air 0 even with a
  byte-correct vendor config, because injection cannot enter the
  associated-station OQT path. So AU ranks second on general reliability while
  being the one part that must never be paired with aggregation.
- **8812CU (RTL8822C, Jaguar3)** — the only part whose TX-power step and
  per-rate diffs devourer marks *measured* (`RtlJaguar3Device.cpp:1359,1371`),
  which is a real argument for ranking it higher than third. Against it:
  adjacent-channel bleed is reproducible on this part and peaks at a mid power
  (2026-08-14), which an auto-elected TX cannot reason about.
- **8733BU (RTL8733B)** — 802.11n only, 20/40 MHz, no 80. Gained a TX-power
  actuator only in devourer #399. Aggregation verified at 3. Legitimately last
  for a video uplink, on bandwidth alone.

**What stays open.** (a) No head-to-head TX comparison at equal power, equal
antenna, equal channel exists — the parts live on different hosts
(`.242` AU, `.232` EU, `.181` BU), and two adapters of the same part number are
not a replicate anyway, so a fair test needs several units per part. (b) The
ordering conflates two different questions — which part transmits best, and
which part the *rest of the feature set* works on (probe proofs, aggregation,
power actuation) — and those may not have the same answer. (c) 8812CU's
measured power step arguably outranks AU for any node that will be calibrated.
Until (a) is run, treat the default as a starting position, not a result; a
node that knows better sets `tx_priority`, and a node that knows exactly which
unit it wants uses the array form.

## 2026-08-21 — walk test: on a real degrading link the §9.4 probe veto has NO operating regime, because §9.2 disarms the probe first

First real range data for the §9.4 probe (#226 Leg A). Craft on battery, both
ends raised off their bench floors to `power_offset_qdb: 0`, shipped selector
values otherwise. 8.4 min of ground record at 2 Hz, 2.5 min of craft record at
1 Hz written to the craft's SD card — the craft half exists because
`promote_blocked_probe` is **craft-only** (§15.3) and the grounds cannot see it.

**Headline: `promote_blocked_probe` stayed 0 for the entire walk**, while
`promote_blocked_saturated` reached **25159**. The veto did not fire once on a
link that reached −86 dBm and flapped through LOSS_EMERGENCY repeatedly.

**Why, and it is structural rather than bad luck.** The probe was **disarmed for
46 % of the walk** (66 of 143 craft samples read `probe_candidate_mcs: 255`),
and the correlation is total:

| samples with probe disarmed | 66 |
|---|---|
| ...of those, `lockout_active` true | **66 (100 %)** |
| lockout ceilings seen while disarmed | 1, 2, 3 |
| profiles seen while disarmed | 1, 2, 3, 4 |

In every case the §9.2 lockout ceiling had fallen **to the current rung**, so
the up-candidate sat above the effective ceiling and Pass 187's clamp turned
the probe off. Worked example from the craft log:

```
 up   rssi prof cand  lock_act lock_ceil lock_rem  reason
  45   -68    2    3     False         5        0  PROMOTE
  48   -76    1    2      True         2    28385  LOSS_EMERGENCY
  52   -87    1  255      True         1    28085  LOSS_PERSISTENT
  73   -86    1  255      True         1     7828  LOSS_PERSISTENT
  81   -81    3    4     False         5        0  PROMOTE     <- lockout expired, probe back
  88   -80    3  255      True         3    23185  PROMOTE
```

So the sequence on any degrading link is: loss → §9.2 locks the rung above →
the probe clamp disarms → no evidence → `probe_per` reads `65535` → the veto
has nothing to act on. **By the time the candidate rate is worth vetoing, §9.2
has already barred it and switched the probe off.**

**This is Pass 187 behaving exactly as ruled (#227), and the ruling's cost is
now measured.** The operator ruling — clamp the candidate to the effective
ceiling including the lockout — was argued on the grounds that a veto cannot
help where policy already bars the climb, and that the lockout window is when
the link can least afford the duty. Both still hold. What neither side of that
argument weighed is the *combination*: §9.2 covers the degraded regime so
completely that the probe now has no regime left. The band where the veto can
fire requires **all** of: candidate rung not locked out, promote otherwise
eligible (RSSI + dwell + `loss_ewma < demote_milli`), `probe_per ≥ 50 ‰` and
fresh, and no fresh `Saturated` verdict — which `core/src/selector.cpp` checks
**first** in the same `else if` chain, so saturation shadows the veto whenever
both apply. That band was never entered in 8.4 minutes of real link.

**Open, and it is a §9.4 ruling, not tuning.** Either accept that the veto is
vestigial on a link with §9.2 lockouts and say so in the spec — the probe's
value then being the §15.3 `probe_per` readout that Pass 186 added, not a
control input — or decouple *arming* the probe from *clamping* the candidate,
so a locked-out rung is still measured for observability (and potentially to
inform §9.2 re-entry, which today is RSSI-only) while the veto stays clamped.
The second reopens the duty question the #227 ruling settled, now with the
knowledge that the duty buys the only rate evidence available in the regime
that matters.

**Second: the return path is the range limit, and that is expected — not a
defect.** At the far point the ground heard the craft at **−56 dBm** while the
craft heard the ground at **−86 dBm**, both at `power_offset_qdb: 0`. A bench
sweep the same day put the gap at **22 dB** at fixed geometry (the uncompressed
−48 row; see the entry below), roughly constant across the offset range, so it
is the radios rather than the path.

**That asymmetry is the system working as designed.** `power_offset_qdb: 0`
means each adapter sits on its own EFUSE per-rate table — i.e. **each end runs
at its own maximum**, which is what both ends are supposed to do. Two different
parts (8812AU ground, 8812EU craft) have different maxima, so the two
directions are unequal, and there is nothing to compensate: you cannot raise
the weaker end past its own ceiling, and lowering the stronger one would only
throw away downlink range. An earlier draft of this entry called offset 0 "not
a common reference point" and asked for the offsets to be chosen to equalise
EIRP — that was wrong, and it is retracted (operator, 2026-08-22).

What survives is purely operational: §7.3 LINK_REPORTs and §12 NACKs ride the
weaker direction and §9.8 descends on report timeout, so **the craft starts
fail-safing while ground-side RSSI still looks healthy**. A range test that
watches only the ground misjudges where the link ends, and link budget should
be sized by the return path. In the field that means reading craft-side RSSI
off the SD logger in `tools/walk/`, since the craft is unreachable.

**Third: §6.3b did substantial real work** (Phase E data, on the `.242`
ground): **95 frames salvaged, 116 frozen, 709 slices synthesized**, against 5
`salvage_failed` and 5 `frames_unrecoverable` over 141233 delivered and 699 FEC
recoveries. Freeze engaged far more often than on the bench, as expected when
loss finally exceeds the FEC budget. No decode complaints from the in-process
consumer.

**Fourth, smaller: `probe_per` reports less often as RSSI falls.** Even where
the probe stayed armed, the fraction of samples reporting `65535` rose from
~23 % at −30..−21 dBm to 83–100 % below −40. Higher loss means fewer probe
frames arrive, so the window cannot reach `min_samples` inside `max_age_ms`.
The probe goes quiet precisely when its evidence would be most informative —
independent of, and additive to, the reset-gap porosity found earlier the same
day.

**Caveats.** One walk, one geometry, one craft. RSSI above −20 dBm is near-field
compressed and is excluded from the bucketed numbers. The craft's clock has no
RTC and resets on a battery boot, so the craft log's wall time is wrong by
weeks; uptime is the correct axis and is what the analysis uses. The bench
floors (craft −48, ground −72) were restored afterwards.

## 2026-08-21 — the §9.4 veto FIRES, and it is porous: a window reset reads as "no opinion" and a promote walks through the gap

The veto path had never engaged — `promote_blocked_probe` was 0 on every node in
every configuration tried (finding below, issue #226). It engages now, and the
run also showed **why it does not hold**.

**Method — synthetic, and the synthesis is the point.** A rate-selective
failure cannot be produced at 50 cm: the 8812EU craft is already at its
measured power floor (`power_offset_qdb −48`, below which it saturates), and at
RSSI −26 nothing fails, MCS 7 included. So the loss was manufactured
**rate-independently** with the bench knob `air.rx_drop_permille: 90` on the
ground, and the craft was configured so the veto was the only thing left that
could block a climb:

| knob | value | why |
|---|---|---|
| `air.rx_drop_permille` (ground) | 90 | ~9 % synthetic per-adapter RX drop, probes included |
| `policy.select.demote_milli` (craft) | **150** (seed 45) | so the §9.0 loss gate does not block the promote first |
| `policy.select.promote_dwell_s` | **12** (seed 0.5) | so 32 samples can accumulate before the promote becomes eligible |
| `policy.select.probe_veto_permille` | **50, the shipped seed** | deliberately NOT relaxed |
| `min/max_profile` | 4 / 6 | headroom, so a climb is pending |

**Result: `promote_blocked_probe` 0 → 1062 → 2100 → 3148 → 4201 → 5157 → 6331**
over about 50 s. The craft held at profile 4 for ~24 s with the ground
reporting `probe_per` 63–129 ‰ against the 50 ‰ threshold, while
`loss_ewma_milli` sat at 70–93 ‰ — **below** the raised `demote_milli` 150, so
the loss gate was demonstrably not the blocker. The counter is a per-tick
gauge, so its magnitude is blocked *time*, not blocked rules.

**That is the whole chain proven end to end for the first time:** RX window →
§3.5 report → `Selector::probe_veto_fresh` → both climb paths suppressed →
counter. Every link in it had been exercised only in unit tests until now.

**The porosity, which is the finding that matters.** The craft still climbed
4 → 5 → 6. It escaped in the gap that the §9.4 freshness rule opens:

```
t6   ground per=69     obs=147   craft prof=4  BLOCKED=4201
t7   ground per=65535  obs=2     craft prof=5  BLOCKED=5157   <-- promoted
...
t10  ground per=70     obs=159   craft prof=5  BLOCKED=5513
t11  ground per=65535  obs=0     craft prof=6  BLOCKED=6331   <-- promoted
```

Both escapes land on a sample where the reported value is `65535`. The window
rolls every `max_age_ms` (8 s); on the roll `successes`/`failures` reset, the
count falls under `min_samples` (32), and `probe_per()` returns `kNoProbe`.
§9.4 says that "is absence of evidence and gates nothing" — so for the second
or so it takes to refill, **the veto is off**, and any promote whose dwell
elapses in that window goes through. A 12 s dwell against an 8 s window found
the gap twice in a row; a 0.5 s dwell would find it far more often.

The rule that makes `kNoProbe` gate nothing is correct in isolation — it is
what stops a non-probing TX manufacturing a phantom veto (guard 4). It is the
*combination* with a periodic reset that leaks: a window that reported 129 ‰
one tick ago is not "no evidence", it is evidence that has just been thrown
away on a timer.

**Open ruling, now with numbers behind it (#226).** Options: hold the maximum
reported value over `probe_veto_ttl_ms` rather than the last one; carry the
previous window's verdict across the roll until the new one fills; require N
consecutive clean windows to clear a veto; or make the reset overlapping
rather than hard. Any of these changes veto strength, so it is a §9.4
amendment, not a tuning knob.

**A second structural point, free from the same run.** The shipped seeds put
`probe_veto_permille` (**50 ‰**) *above* `demote_milli` (**45 ‰**). For loss
that is not rate-selective, the demote gate therefore always fires before the
veto can — which is why `demote_milli` had to be raised to 150 to observe the
veto at all. That ordering is defensible on purpose (the veto exists for
*rate* headroom, and uniform loss is the demote path's business), but it means
the veto is unreachable under any uniform degradation, and nothing says so.

**Pass 187 verified on the same run.** With the craft at profile 6 —
`max_profile` 6, i.e. its ceiling — `probe_candidate_mcs` read **255** and
`promote_blocked_probe` froze at 6331: no probe duty and nothing left to
block. Meanwhile both grounds reported `probe_candidate_mcs: 6`/`7` with
`probe_observed: 0`, deriving the unclamped candidate they cannot know is
barred and correctly reporting no opinion. The clamp and the §9.4 asymmetry,
on the deployed binary.

**What this does NOT show.** The loss was uniform and synthetic. It proves the
mechanism, its timing hole, and the seed ordering — **not** that a real
rate-selective failure produces these numbers, and not what `probe_per` reads
when MCS+1 genuinely fails while MCS holds. That still needs a marginal link:
a range or power walk-down, #226 Leg A on the #134 queue.

Fleet reverted to shipped config afterwards; `rx_drop_permille` removed,
`demote_milli` back to 45, `promote_dwell_s` and the pin restored.

## 2026-08-21 — Pass 186 on hardware: the clamp works, the probe measures every rung, and the veto cannot be reached from this bench

Fleet on the Pass 186 build (`.232` craft, `.242` + `.199` grounds), table
`0xF2`, `probe {period:64, slot:4}`.

**The ceiling clamp, as a clean A/B.** The craft sat at profile 4 with
`max_profile: 4`. Restarting it onto the clamping build moved
`probe_candidate_mcs` from 5 to **255** — not probing — and both grounds went
from ~230 candidate observations per 8 s window to **exactly 0**. Nothing else
changed: same table, same `air.mcs_probe: true`, same channel.

**The RX asymmetry behaves as §9.4 requires, measured rather than argued.**
With the craft disarmed, both grounds still reported `probe_candidate_mcs: 6`
— they derive the unclamped candidate because `max_profile` is node-local
policy and never reaches them — and both reported `probe_per: 65535`. Guard
(4) turned one-sided knowledge into *absence of evidence*, not into a phantom
veto.

**What the probe actually measures.** Given headroom (`max_profile` 5, later
6) the up-candidate PER reads **0–8 ‰ at every rung**, candidates MCS 2
through 6, at RSSI −28 dBm. Against `probe_veto_permille` 50 that is a
uniformly favourable opinion — the reading that was indistinguishable from
"no opinion" before Pass 186 exposed it. Raising the craft's ceiling from 4
to 5 on that evidence promoted cleanly and held: profile 5, `HOLD`,
`loss_ewma_milli` 2, three frames salvaged in the first minute.

**The veto did not fire, and two independent mechanisms explain why.**
`promote_blocked_probe` stayed 0 throughout.

1. **RSSI-margin promote outruns evidence formation.** §9.4 resets the window
   on any `active_profile` change, and evidence needs `min_samples` 32 inside
   `max_age_ms` 8000. At the shipped `promote_dwell_s` 0.5 the craft climbed
   **0 → 4 → 6 in about four seconds**, with at most **4** observations at any
   rung — the veto branch is only reached when a promote is *otherwise
   eligible*, and eligibility arrived first every time. Raising
   `promote_dwell_s` to 12 fixed that half: evidence then formed at every rung
   (186, 214, 132 observations).
2. **The veto reads the last window, not a trend.** Even with evidence
   present, `Selector::probe_veto_fresh` gates on the most recently *reported*
   `probe_per`. A candidate that fails intermittently — MCS 6 here, one
   failure per few thousand probes — produces mostly `0` reports with an
   occasional `4`–`8`, so the latched value at the instant the dwell elapsed
   was `0`. Lowering `probe_veto_permille` to **1** did not change that: the
   climb 5 → 6 still went through.

Neither is a defect. Together they say the veto's operating regime is a link
that **dwells** under marginal conditions, not one that is climbing — which is
consistent with its design as a veto rather than a warrant, but means **the
veto path remains unproven end to end.** It needs a candidate that fails in
most 8 s windows, i.e. a range or power walk-down (Phase E), not a 50 cm bench
at −28 dBm. Worth deciding then whether a single clean window should be able
to clear a veto, or whether the freshness rule wants hysteresis.

**Fleet left at:** profile 5 / MCS 5 (the shipped envelope), craft
`probe_candidate_mcs: 255` — at its ceiling it spends no probe duty at all,
which is exactly what Pass 186 was for. Craft config reverted to shipped
values; `probe_veto_permille: 1` and `promote_dwell_s: 12` were test-only and
are gone.

## 2026-08-21 — the x86 ground had the §6.3b config key and a binary that could not read it

`deploy/ground-192.168.2.242.json` carried `conceal.mode: "slice-skip"` from
2026-08-20 (Pass "§6.3b on by default"), and on that basis the ground was
reported armed. It was not. The node's `waybeam_hub` was built **2026-08-16**
and §6.3b landed **2026-08-20 22:28**, so the in-process node linked a wblink
that had never heard of the key. Three independent reads agree:

| artifact | `conceal` strings | `salvag` strings |
|---|---|---|
| running `/usr/local/bin/waybeam_hub` (Aug 16) | **0** | **0** |
| rebuilt `ground_x86/waybeam_hub` (Aug 21) | 11 | 3 |
| `.199` known-good rk3566 build | 7 | — |

And the node's own `/etc/waybeam-link/ground.json` had **no `conceal` block at
all** — the repo file and the live file had drifted in *both* directions: the
repo carried the conceal key the node lacked, while the node carried a `mac`
pin, `policy.csa.adjacent_guard_mhz: 40`, and `bus: "8-1"` against the repo's
stale `5-1` (the 8812AU is on bus 8 — `lsusb -t`, re-measured). Both files now
agree on every non-comment key.

**A repo `deploy/*.json` is not evidence about a node, and neither is a git
log.** The config says what the node would do if it were running code that
reads the key; only the binary says whether it does. When a feature is
"deployed", check the artifact that executes — `strings` on the running file,
or the log line the feature emits at startup. Here the honest signal was
already designed in and simply never looked for:

```
rx: stream 0 §6.3b conceal enabled (freeze_frame=1)
```

Absent from every x86 boot before 2026-08-21, present on the first one after.
Within the first 20 minutes stream 0 reported **6 frames salvaged and 11
slices synthesized** against `recovered_fec` 6572 — §6.3b working on the x86
ground, not merely configured. (The first 30 s showed `frames_salvaged: 0`
with one `salvage_failed`, which is a two-event sample, not a result; the
salvage rate per FEC recovery runs ~9× lower here than on `.199` because this
ground sits at RSSI −26 against `.199`'s −35 and fewer of its losses exceed
the FEC budget.)

**What stays open.** The rebuild is a plain `make ground_x86` against
`build/x86-ground`, and nothing in either repo notices when the hub's linked
wblink is older than the feature the config asks for. A version or feature
handshake between `mod_wblink` and the config it loads would turn this from a
silent no-op into a refusal. Note also that the hub's link rule does not
depend on the wblink archives, so `make ground_x86` after a library change is
a **no-op that silently ships the old binary** — remove the target first.
## 2026-08-21 — the §9.4 probe actuates exactly as specified, and `probe_per` is invisible

First device run of the §9.4 Pass 163 rate probe. `probe {period:64, slot:4}`
added to the **deployed** table (never the repo file) and pushed identically to
`.232`, `.242` and `.199`; `air.mcs_probe: true` on the craft only. All three
came up at `table_version=0xF2`, up from `0x68` without the block — the
lockstep worked and the hash moved exactly as `--check` predicted offline.

Craft at profile 4 / MCS 4, so the up-candidate is MCS 5, and the MCS-5 bucket
of `adapters[].rx_mcs[]` read exactly 0 on both grounds beforehand. 60 s
windows, two independent receivers:

| window | `.242` MCS-5 share | `.199` MCS-5 share | `.242` delivered/rx |
|---|---|---|---|
| probe ON | 1.527 % (2013 frames) | 1.505 % (2012 frames) | 97.56 % |
| probe OFF (control) | **0.000 %** (0) | **0.000 %** (0) | 97.52 % |
| probe ON (repeat) | 1.527 % (1988) | 1.504 % (1987) | 97.53 % |

Ideal is 1/64 = 1.563 %; the shortfall is the non-video traffic in `rx` that
never probes. **The two receivers saw the same probe frames** — 2013 against
2012, then 1988 against 1987 — which is the property that matters: both
derived the schedule from `seq` alone, with nothing on the wire saying which
frames were probes.

The control was taken by flipping `air.mcs_probe` on the craft **without
touching the table**, so `table_version` stayed `0xF2` across all three
windows and the only variable was TX actuation. The repeat ON window
reproduces the first to three decimals, which bounds scene drift.

**This is the per-unit stage-0 proof** the §9.4 enablement gate asks for, for
the craft's 8812EU (`98:03:cf:cf:a4:28`): the unit honours a per-frame
radiotap rate. It was previously proven per-part, not per-unit.

**Cost: none measurable.** `delivered/rx` moves 97.56 / 97.52 / 97.53 % across
ON/OFF/ON and `recovered_fec` 500 / 522 / 506 — no direction, let alone a
1.5 % penalty. Expected: MCS 5 delivers fine at this RSSI (−34/−48). The duty
cost only becomes real where MCS+1 starts failing, which is exactly where the
veto is supposed to earn it back.

**Two things this run did NOT verify, and one is a gap.**

- **`probe_per` is exposed nowhere.** Not in the §15.3 stats NDJSON, not on
  any REST path. The only probe observable is `link.promote_blocked_probe`,
  a downstream effect counter that moves only when a climb is both attempted
  *and* vetoed. So there is no way to tell "the RX has no opinion" (guard 4:
  `candidate_observed_ == 0`, too few samples, or stale) from "the RX has an
  opinion and it is favourable". **A rollout would be flying blind** — you
  could enable the probe fleet-wide and never confirm it is producing
  evidence. Exposing `probe_per` should precede any rollout.
- **The veto was not exercised, and the reason is structural.** The craft's
  own config pins `policy.select.max_profile: 4` while it sits at profile 4,
  so profile 5 is not a promotion target the selector is ever allowed to
  pick — there is no climb for the veto to block. `probe_up_candidate_mcs()`
  (`core/src/mcs_probe.cpp:10`) takes only `(table, active_profile)` and
  walks to the next profile **by id**; it has no knowledge of `max_profile`.
  **So on this node as configured the probe's 1.5 % duty buys nothing** — it
  can only ever veto a promotion policy already forbids. Either the
  candidate should be clamped to the selector's `max_profile` (and the probe
  suppressed when `active_profile == max_profile`), or a probing node has to
  be configured with headroom above it. Worth an issue before rollout;
  exercising the veto for real additionally needs the cliff — a power
  walk-down or range — not this 50 cm bench geometry.

`probe.period`/`probe.slot` remain the §17 seeds inherited from the
devourer/wfb_ng bench fit. This run shows the mechanism actuates at 64/4; it
says nothing about whether 64/4 is the right fit for our fps and block
structure.

## 2026-08-21 — Phase C: Rockchip MPP accepts a §6.3b repair and is SILENT on the gap it replaces

Phase C acceptance on the rk3566 ground (`.199`, OpenIPC SBC GS, kernel
6.1.84). The README's `--decoder gst:<mpp element>` recipe has nothing to bind
to on the shipped image: 28 gstreamer plugins, no rockchip plugin, no
`mpi_dec_test`, no compiler, no numpy. What the image does have is
`librockchip_mpp.so.1` — the library `mod_pixelpilot` links. So the picture is
produced on the device by `tools/spatial_conceal/mpp_dec_yuv.c`, which copies
the four settings from `waybeam-hub/src/pixelpilot/video_decoder.c`
`set_mpp_decoding_parameters()` verbatim (`base:split_parse=1`,
`DISABLE_ERROR` / `IMMEDIATE_OUT` / `ENABLE_FAST_PLAY` = `0xffff`) and is
compared on x86 through `decode_compare.py --decoder raw`. **Testing against
MPP defaults would not have been an acceptance test** — the hub does not ship
defaults.

Vector: 40 AUs cut from `capture_232_4slice.265` at an AU boundary
(`ffmpeg -c copy -frames:v 40`), AU 22 (4 slices, 14207/14245/13458/5118 B),
slice 1 either repaired by `hevc_conceal_cli` (14249 B -> a 20 B all-skip
slice) or deleted by `slice_drop.py`.

| comparison | worst dB | damaged CTU-64 rows | frames differing |
|---|---|---|---|
| C0 ffmpeg(ref) vs MPP(ref) | 41.70 | 3-16 | 8/40 — exactly n≡4 (mod 5) |
| C1 MPP(ref) vs MPP(rep) | 46.63 | **4-10**, healing | 3/40 |
| C2 MPP(ref) vs MPP(gap) | 42.14 | **4-16**, worsening | 3/40 |
| C3 ffmpeg(rep) vs MPP(rep) | **89.04** at the repair | 4-5 | C0's 8, plus 22 and 23 |
| C4 ffmpeg(gap) vs MPP(gap) | **39.44** at the fault | 3-16 | C0's 8, plus 22, 23, 24 |

Peak/mean luma delta, computed independently of `decode_compare.py`:
frame 22 rep 32/0.338 against gap 59/0.538; frame 24 rep 28/0.144 against gap
56/0.577.

**Three things this establishes.**

1. **MPP accepts the repair.** On the repaired stream the two decoders agree
   to 89 dB over two CTU rows — rounding, not divergence. Everything else is
   C0. On the gap stream they produce materially different pictures (39.4 dB
   over the whole frame): two ground stations would show different pixels and
   neither would be right.
2. **The gap costs the rest of the picture.** Same decoder, same reference:
   the repair stays inside rows 4-10 and decays by frame 24; the gap reaches
   row 16 — the bottom — and is *worse* at frame 24 than at 22. The heatmap
   shows a bright seam at the slice boundary with error smeared below it.
3. **MPP reported `errinfo 0 discard 0` on all 40 frames of all three
   streams, including the gap.** The external lab's claim reproduces on our
   hardware, our stream and our decoder configuration. `video_decoder.c:1352`
   gates the partial-frame log *and the IDR requester* on
   `errinfo || discard`, so **this fault class never fires either.** A
   ground that logged nothing has not shown it received anything good.

C0 is the third independent decoder to disagree on TRAIL_N pictures — ffmpeg,
AMD VAAPI and now MPP — at the same ~42 dB, the same 1-in-5 cadence, the same
full-frame extent, while the other 32/40 frames are bit-identical between
ffmpeg and MPP. MPP's own log says `h265d: nal: type mismatch 0 1`, the same
complaint ffmpeg makes. The venc-side non-conformance is not an ffmpeg
artifact.

**Open.** Conceal is NOT armed on `.199`: its `waybeam-link` is the Aug-4
binary and its config still declares `"air": {"kind": "kernel-monitor"}`, the
backend deleted in Pass 164, with the adapter named by `ifname`. Arming
requires a node re-bring-up (devourer on bus 1-1 with `rtl88x2cu` unloaded,
current table, current binary), not a config key — and `.199` is live-RX right
now (3.42M packets, RSSI -28), so that takes a working receiver off the air.
Its `/etc/waybeam-link/table.example.json` is also the Aug-1 short-GI table
(`b154d494`) where `.232` and `.242` both carry `017f9c18`.

## 2026-08-20 — §10.7 needed no recalibration for long GI, and the ground could not write an artifact at all

**The premise was wrong, checked before acting on it.** The claim that the
guard-interval change stranded §10.7 artifacts on rungs 2+ does not survive
contact with the system: calibration-v2 §2.2 scopes an uplink run to the
configured `air.uplink_rate` rung **alone**, the ground config sets no
override so that rung is **MCS 0**, and MCS 0 was always long GI. The stored
artifact holds exactly one placement, `{mcs:0, short_gi:false}` — there were
never any short-GI entries to strand. Retracted in PROTOCOL.md §9.5.

The artifact *was* stale, for unrelated reasons: captured on **channel 5765**
while the link now runs 5805, and `craft_adapter_fingerprint` 136 against a
live 212. So it was worth re-running anyway.

**Ground-host defect, found by running it: `artifact_write_failed`.**
`/etc/waybeam-link/calibration/` was `root:root` 755 while the ground hub runs
as **`snokvist`**, so the engine swept the full range (26502 probes, power
−34 → 0 qdb) and then correctly **refused to report success** it could not
persist — the §17 "refuse false success" rule doing its job rather than
silently producing an unusable placement. `chown -R snokvist:snokvist` on that
directory fixed it. Any calibration on this host would have failed the same
way; the artifact on disk dated from a root-run standalone `waybeam-link`.

Re-run after the fix: **done, `stale:false`**, 54002 probes / 103 tallies,
channel **5805**, placement `{mcs:0, short_gi:false, qdb:-2, rssi:-39 dBm,
loss:5‰, last_clean:24, first_bad:null}`. `first_bad:null` means the sweep
**never found a failing point** across the whole commanded range — at 50 cm
bench geometry the uplink is clean everywhere, so this is a bench placement,
not a range-valid one, exactly as the ground config's own `_power_comment`
warns.

**Read the drop counter with that in mind.** `shm_full_drops` jumped 18 →
8552 across the run and then froze. That is the §2.4 video-input starve, not a
fault: the run silences video and restores it, and nothing resets the counter
afterwards, so a post-calibration craft looks alarming until you check that it
is static. Throttle returned to 1000, ground receiving at rssi −28 / snr 29.

## 2026-08-20 — the airtime ceiling moved UP since Pass 111: rung 4's clean point is >21000, not ~19000

Long GI deployed to both ends (craft `.232` + this ground, `table_version` 91
→ 62), then the §9.5 clean-point re-measure. Method is the Pass 111 one: a
steady full-cadence source is clean only while `shm_throttle_permille == 1000`
with the ring at idle occupancy and `shm_full_drops` not advancing. Profile
pinned, bitrate stepped (it is a **live** venc field, so no restart and no
ring re-attach transient), stats reset per step, sampled 25 s apart.

**Rung 4 (MCS4, long GI):**

| target kbps | throttle | full_drops | ring_full | verdict |
|---:|---:|---:|---:|---|
| 16213 (derived) | 1000 | 0 | 0 | CLEAN |
| 17000 | 1000 | 0 | 0 | CLEAN |
| 18025 (old short-GI rate) | 1000 | 0 | 0 | CLEAN |
| 19000 | 1000 | 0 | 0 | CLEAN |
| 20000 | 1000 | 0 | 0 | CLEAN |
| **21000** | **1000** | **0** | **0** | **CLEAN** |
| 22000 | **740** | 0 | 0 | THROTTLED |

So the clean point is **21000–22000, above the ~19000 Pass 111 found under
short GI** — with *less* airtime available. **The bottleneck at this rung is
not airtime**; the encode → frame-SHM → FEC → injection path got faster since
Pass 111, and the obvious candidate is the USB bulk-OUT TX aggregation
(#216/#217). The Pass 111 permille were stale by more than the guard interval
costs.

Applying the §9.5 rule (600 is the policy cap; the clean point only ever
lowers it): rung 4 at 600 derives 19092 ≤ 95% of 21000 = 19950, so it is no
longer oversubscribed → **permille 510 → 600**. Net effect of both changes
together: rung 4 goes 18025 (short GI) → 16213 (long GI) → **19092**, i.e.
**higher than before while also more robust**.

**Rung 5 measured clean at every rate up to 25000** — but 25000 is
`venc.max_bitrate_kbps` (§9.6), not a link limit, so its clean point is
**censored, not measured**, and the 95% rule cannot be applied. Raising rung 5
alone on the censored bound would also derive 23739 against rung 6's 20914 —
a **non-monotonic ladder**, where promoting a rung would *lower* the bitrate.
Rungs 5–7 therefore stay at 463/438/418. Retuning them needs their clean
points measured together with the §9.6 ceiling raised far enough not to censor
the answer.

Final ladder `{2829,5754,9264,12384,19092,19646,20914,22183}`, monotonic.
Deployed to both ends, `table_version` **104** on each; craft steady at 19092
with `shm_throttle_permille` 1000 and `full_drops` flat over 40 s, ground
receiving at rssi −28 / snr 30.

**`table-8733b.json` is deliberately NOT retuned.** That is the CV610/`.181`
craft's table and its clean point was not measured here; that craft is
CPU-limited and pins `venc.max_bitrate_kbps=12288`, which caps rungs 3–5
anyway. The two tables now legitimately differ at rung 4 (600 vs 510).
**What that costs was not known when this was written** — see the 2026-08-30
entry at the top: the differing hash puts every `.181`↔`.242` stream into §3.4
best-effort, which suspends ARQ, supersession and deadline drops. The
divergence stands; the price is now measured and, since Pass 197, visible.

## 2026-08-20 — low-bitrate arm: per-slice overhead is ~26 BYTES, and 17 is free at 2.8 Mbps too

The bitrate axis is reachable after all — **not** by fighting the rate
controller but by using it. `POST /api/v1/link/profile {"min":N,"max":N}` on
the craft's link (`127.0.0.1:8091`) pins the profile, and the link then
actuates venc to that profile's budget: profile 1 → **5754 kbps**, profile 0
→ **2829 kbps**, both at 60 fps with zero ring drops. Craft config pins
`min_profile: 1, max_profile: 4`, which is what to restore.

Static scene, 2829 kbps, 62 s dwells, 6 MB tails from offset 8 MB:

| N | mean AU | Mbps@60 | QP mean | QP sd |
|---:|---:|---:|---:|---:|
| 4 | 5,525 | 2.65 | 30.385 | 0.531 |
| 6 | 5,514 | 2.65 | 30.500 | 0.563 |
| 9 | 5,508 | 2.64 | 30.559 | 0.555 |
| 17 | 5,895 | 2.83 | 30.534 | 0.544 |
| 4 *(control)* | 4,927 | 2.37 | 30.566 | 0.588 |

The single pass could not separate overhead from drift — mean AU fell 11%
between the two N=4 points while N=17 sat 7% above them — so it was repeated
**interleaved, A-B-A-B**:

| order | N | mean AU | Mbps | QP mean |
|---:|---:|---:|---:|---:|
| 1 | 4 | 5,525 | 2.65 | **30.381** |
| 2 | 17 | 5,861 | 2.81 | **30.509** |
| 3 | 4 | 3,725 | 1.79 | **31.104** |
| 4 | 17 | 5,486 | 2.63 | **30.515** |

**The two N=4 points differ by 0.72 QP; the two N=17 points differ by
0.006.** The N=4 self-variance is four times any N=4-vs-N=17 gap, so there is
again no resolvable quality cost — this arm's resolution is ~0.7 QP, weaker
than the 18 Mbps arm's ~0.36, because the "static" scene is not actually
static (AE/content wander).

**The one clean number comes from the adjacent pair.** Points 1 and 2 are 62 s
apart, so drift is minimal: +336 B for +13 slices at +0.13 QP → **≈26 bytes
of overhead per additional slice**. That is far below the ~100 B/slice the
CABAC-reset argument predicted, and it scales the whole question:

| bitrate | frame bytes | 4→17 overhead | as % of frame |
|---:|---:|---:|---:|
| 2829 kbps | 5,525 | 336 B | **6.1%** |
| 18025 kbps | 36,964 | 336 B | **0.9%** |

So the earlier prediction that 17 slices would be "wasteful" below the
one-slice-per-chunk knee is **quantified and much smaller than assumed**:
6% of the bit budget at 2.8 Mbps, under 1% at 18 Mbps. It is a real cost at
the low end but not a disqualifying one, and the low end is also where the
frame is only ~4 chunks so the granularity argument for a high N disappears
anyway — the case for a lower N down there now rests on "no benefit", not on
"large penalty".

## 2026-08-20 — the QP cost of slicing is BELOW the scene-drift floor: 17 slices is free at 18 Mbps

Moving scene (operator-supplied), 1080p60, CBR pinned at 18025 by
waybeam-link, hub consuming the ring, venc `a12ff32`. Per point: 60 s dwell,
record to SD, analyse a 14 MB tail taken from byte offset 60 MB so the sample
sits in steady state. Sweep order 4, 6, 9, 17, then **4 again as a drift
control**.

| N | mean AU | Mbps@60 | AUcv | **QP mean** | QP sd |
|---:|---:|---:|---:|---:|---:|
| 4 | 36,964 | 17.74 | 0.146 | **27.723** | 0.448 |
| 6 | 37,043 | 17.78 | 0.151 | 27.558 | 0.497 |
| 9 | 37,040 | 17.78 | 0.138 | 27.441 | 0.497 |
| 17 | 36,833 | 17.68 | 0.146 | **27.410** | 0.492 |
| 4 *(control repeat)* | 36,848 | 17.69 | 0.148 | **27.363** | 0.481 |

**Read the control, not the trend.** The two identical N=4 points differ by
**0.36 QP** with no configuration change between them — that is the scene
drifting over the five minutes of the sweep. The entire spread across
N=4→17 is 0.31 QP, smaller than the drift and pointing the same direction as
it, because N was swept upward while the scene drifted. So the slice-count
effect is **not resolvable by this measurement**: |ΔQP(4→17)| < 0.4 QP, and
the apparent "17 is better" is a time-ordering artifact, not a finding.

**Conclusion: 17 slices costs no measurable quality at the flagship operating
point.** The 0.5-1.5 QP predicted from HEVC slice-overhead literature did not
appear. Plausibly because this stream already breaks prediction along CTU-row
boundaries for GDR, and `loop_filter_across_slices=1` means the deblocking
filter still crosses the new boundaries.

**Scope: 18 Mbps only.** The low end is NOT measured — waybeam-link is the
rate controller and **reasserts its profile budget on every venc restart**
(observed directly: `video0.bitrate` set to 8000 read back as 18025 after the
next reinit), so the bitrate axis cannot be swept while the link runs. At
2500 kbps a 17-slice picture is ~306 B/slice, far past the one-slice-per-chunk
knee, where the CABAC-reset overhead is a much larger fraction — that arm
still needs either a link-profile lock or a link-stopped run with some other
ring consumer.

**Two measurement traps, both paid for here.**
1. **The first sweep was entirely invalid** and looked plausible: 22 s dwells
   meant every capture began inside the post-restart transient, where the
   link node has not re-attached to the freshly created ring. Symptoms —
   18-29 IDRs per 330 AUs on a craft that emits none unprompted, GDR refresh
   AUs absent, CBR undershooting 14.8-17.1 Mbps, QP sd ~3.5. The tell was the
   comparison against a known-good capture (QP sd **0.441**, AUcv 0.166,
   18.16 Mbps, 1 IDR). **Validity gates for any venc rate capture: IDR≈0,
   achieved bitrate on target, AUcv < 0.2, QP sd < 0.6.**
2. `wbnode_progress_proven=1` and a static `wblink_shm_full_drops` are *not*
   sufficient evidence that a capture is clean — both read healthy while the
   captures were garbage, because they were sampled after the transient had
   passed.

**Correction to an earlier claim in this file.** The GDR refresh AU has its
own geometry, **independent of `sliceCount`**, and it is not fixed: in these
evening captures it is consistently **9 slices of 2 CTU rows** (addresses
0,60,120,…,480) at every 2 s GOP boundary, at both N=4 and N=17 — where the
morning capture at the same sliceCount 4 showed **17 slices of 1 CTU row**
(0,30,…,480). What varies it is unknown. Consequence: the earlier "at N=17
the steady and refresh geometries coincide, so the two-frame adoption gate
never sees a shape change" argument is **withdrawn** — on this evidence it is
N=9 that coincides, and under any N the refresh AU generally presents a
different address list once per GOP. That is exactly what the two-frame gate
exists to refuse, and it does; it was never a reason to prefer 17.

## 2026-08-20 — x86 negative control: AMD VAAPI does not fail on a gap AU, and §6.3b's x86 value is DETERMINISM not dB

New tools: `tools/spatial_conceal/slice_drop.py` (builds the gap AU — the
stream a decoder would see *without* §6.3b) and `decode_compare.py` (decodes
two streams and compares the pictures — per-frame luma PSNR plus damaged
CTU-64 row extent, with `--decoder gst:<element>` so the same readout runs
against VAAPI here and MPP on rk3566 in Phase C).

Corpus: first 100 AUs of the real `.232` capture, AU-aligned (`ffmpeg -c copy
-frames:v 100` — an arbitrary `head -c` cut truncates the last AU and
manufactured a 11.6 dB artifact that briefly looked like a finding).
One slice (index 1, 9,827 B) removed from AU 50; the §6.3b repair of the same
AU/slice via `hevc_conceal_cli`.

| stream | decoder | worst frame | worst PSNR |
|---|---|---:|---:|
| gap AU (no §6.3b) | ffmpeg sw | 85 | **36.81 dB** |
| gap AU | AMD VAAPI `vah265dec` | 50 | 46.21 dB |
| §6.3b repaired | ffmpeg sw | 50 | **47.33 dB** |
| §6.3b repaired | AMD VAAPI | 50 | **47.33 dB** |

**Their VAAPI finding does not reproduce on AMD.** The external lab's Intel
iHD result was a *fatal* `vaSyncSurface` decoding error that made ffmpeg
discard a good surface. On this host (AMD Radeon, `va` plugin) the gap AU
decoded all 100 frames with no pipeline error and no dropped picture. That is
a driver-and-stack difference, not a contradiction — but it means their
libavutil patch is not something our x86 ground needs.

**The repaired stream decodes bit-identically on both decoders** (47.33 dB,
frame 50, rows 4-10 — the same numbers to two decimals). The gap AU does not:
36.81 dB on software against 46.21 on hardware, and the software damage
*peaks 35 frames later* (frame 85), i.e. it propagates rather than heals. So
on x86 the repair buys ~1 dB against AMD's own concealment but ~10 dB against
software, and — the part that actually matters — it makes the picture the
same everywhere. A conformant AU is decoder-independent; a gap AU is a
different picture on every stack. That is the argument against the external
lab's "ship the gap and let hardware conceal" simplification, and it is
stronger than the dB gap.

**Caveat on how far this generalises.** This is one missing slice in one AU.
Our reassembler never emits a gap AU — without §6.3b the frame is *dropped*,
so the operative comparison for the feature's value stays repair-vs-drop
(Phase D: 231 vs 50 of 362 at 20% loss). The gap AU exists only to price the
"don't repair, ship partial" alternative.

**Unrelated defect found by the do-nothing control: the craft's stream
decodes DIFFERENTLY on software and hardware, on every 5th picture.** Clean
capture, ffmpeg vs AMD VAAPI: 20 of 100 frames differ, at 41.7-43.1 dB over
CTU rows 3-16, and every one of them has frame index ≡ 4 (mod 5) — exactly
the SVC-T TRAIL_N pictures, which carry mixed TRAIL_N/TRAIL_R NAL types
inside a single picture (HEVC 7.4.2.2 non-conformance, already recorded as
the reason `-threads 1` is mandatory). It is now clear this is not merely an
ffmpeg threading artifact: two independent decoder implementations
reconstruct 20% of our pictures differently. Mild (~42 dB) but real, and it
means two ground stations can display different pixels from one stream.
**Filed against venc, not §6.3b** — every decode-compare must expect this
1-in-5 divergence as a baseline or it will be misattributed to concealment.

## 2026-08-20 — sliceCount 17 device-verified on .232 (structural half of the Phase B extension)

venc ceiling raised (`VENC_SLICE_COUNT_MAX` 32, venc `a12ff32`), deployed to
`.232` (md5-matched both ends), and the request that used to be refused now
lands exactly as the quantization model predicted:

```
VENC: H.265 slice split ON: sliceCount 17 -> 17 slices of 1 CTU-64 rows (SDK unit 2 32-px rows)
VENC: slice split readback: enable=1 rows=2
```

Recorded 8 MB from the SD at that setting: **247 of 249 AUs carry exactly 17
slices**, at CTU addresses **0,30,60,…,480** — one CTU-64 row each, the same
geometry the GDR refresh AU has always used. (The two outliers are the
partial opening AU and the AU my `dd` cut truncated.) `isp_fps` **60**,
`venc_bitrate_kbps` **18025** — both held. **Truncation WARN count: 0** at 60
frames/s, i.e. ~1,800 seventeen-NAL AUs per 30 s rather than the refresh AU's
0.5/s. That closes the per-pack question: a 17-NAL access unit is delivered
as multiple packs of ≤8 and the walker handles it.

Mean AU 33,634 B (1,978 B/slice = 1.39 chunks) against 37,836 B at
sliceCount 4 — **this says nothing about coding cost**. Different scene
moment on a static bench, where CBR undershoots on easy content; the QP
comparison still needs motion in frame. Craft restored to `sliceCount 4`,
config byte-identical to the pre-session backup.

Remaining before the default moves: the QP/quality cost at {4,6,9,17} across
the 2500-25000 kbps range, on a moving scene.

## 2026-08-20 — the sliceCount ceiling is 6 today, and the reason given for the cap is a misreading

Trying to set `video0.sliceCount=17` on the craft was refused by venc's own
validator: `video0.slice_count must be 1..8` (`src/venc_api.c:1257`), with
`venc_config.c:645` clamping to the same range. Since 7 and 8 both quantize
to 6 delivered slices, **the fleet's reachable maximum today is 6** — 9 and
17 cannot be requested at all.

The cap's stated rationale (`ui_slice_count`, `venc_api.c:475`) is *"Capped
at 8 by the SDK's 8-entry per-pack NAL table."* The table is real —
`packetInfo[8]` in `include/sigmastar_types.h:660/1051` — but it bounds NALs
**per pack**, and the output walker iterates packs
(`for (i = 0; i < stream->count; ++i)`, `star6e_output.c:918`); `packNum` is
per-pack. So it is not an AU-level slice limit.

**Measured, decisively:** the GDR refresh AU is a 17-slice picture (verified
in the capture at addresses 0,30,…,480), it is emitted every 2 s GOP at every
sliceCount, and `.232` has been running the abort-not-truncate build for 6 h
— roughly 10,800 such AUs through that exact walker — with the truncation
WARN count at **zero**. A 17-NAL access unit already flows; it arrives as
multiple packs of ≤8.

So raising the validator ceiling is well-founded rather than speculative.
Open before flipping any default: the ceiling change itself (venc, PR #236
territory), then a deploy and a structural check (delivered count really 17,
fps holds, WARN stays silent at 60 frames/s rather than 0.5/s).

**The quality half is blocked on scene motion, not on RF.** Slice overhead
under CBR surfaces as QP, and a static bench scene at 18 Mbps runs
quality-saturated, so a QP sweep taken now would measure nothing
([[feedback_moving_scene_required_for_venc_rate_tests]]). The
sliceCount x bitrate matrix needs motion in frame.

**Why a matrix and not a sweep (operator input 2026-08-20): the link runs
2500-25000 kbps**, and the optimum tracks frame size. At 60 fps the knee
`N* = frame_bytes/1424` moves from 3.7 (2.5 Mbps) to 36.6 (25 Mbps) — so no
single value serves the range. At 2.5 Mbps a 17-slice picture is ~306 B per
slice, far past the knee, where the CABAC-reset overhead is paid for zero
granularity gain; at 18 Mbps a 4-slice picture leaves 6.65 chunks per slice
and 77% of the picture synthesized at 20% loss. Indicative mapping at 60 fps
(`N*` snapped onto the achievable set {1,2,3,4,5,6,9,17}):

| bitrate | frame B | chunks | N\* | pick |
|---:|---:|---:|---:|---:|
| 2500 | 5,208 | 3.7 | 3.7 | 4 |
| 5000 | 10,417 | 7.3 | 7.3 | 6 |
| 8000 | 16,667 | 11.7 | 11.7 | 9 |
| 12000 | 25,000 | 17.6 | 17.6 | 17 |
| 18000-25000 | 37.5k-52k | 26-37 | capped | 17 |

That is either a per-mode static choice (modes already bundle fps and
resolution, so a slice count fits the same object) or a link-driven
actuation, since waybeam-link is already the sole rate controller and would
be the only thing that knows the current bitrate. Two things make the
dynamic version cheap if it is wanted: slice layout is per-picture syntax, so
changing it needs **no IDR and no SPS/PPS change**, and the repair layer
already refuses to adopt a new geometry until two consecutive pictures agree,
so a switch costs a couple of frames of salvage refusal and nothing else.
Open ruling: static-per-mode vs link-actuated, and whether the actuation is
restart-class (it is today — `MUT_RESTART`, `venc_api.c:640`) or can be made
live via `MI_VENC_SetH265SliceSplit` on a running channel.

## 2026-08-20 — how many slices 1080p can actually have, and why the FPV default should be 17 not 4

Measured on the real `.232` capture (362 AUs, `tools/` parser); craft live
config read the same day: 1920x1080, 60 fps, CBR **18025 kbps**, gop 2.0 s,
`sliceCount: 4`, `resilience: "racing"`.

**The achievable set is not 1..N — it is {1,2,3,4,5,6,9,17}.** venc maps
`sliceCount` to the SDK's 32-px `u32SliceRowCount`, which the SDK rounds UP
to whole CTU-64 rows; 1080p is 17 CTU rows, so the delivered count is
`ceil(17 / r)` for `r = ceil(ceil(1080/N/32)/2)` CTU rows per slice. 7, 8 and
10..16 are unreachable — they collapse onto 6 and 9. This predicts both
measured data points exactly: request 4 → 32-px unit 9 → SDK stores 10 →
r=5 → **4**; request 8 → unit 5 → stores 6 → r=3 → **6**.

**Per-slice size, measured.** Mean AU 37,836 B (= the 37,552 B CBR budget).
FEC chunk payload is 1424 B (§3.2), so a frame is **26.6 chunks**.

| N | CTU rows/slice | slice px | mean slice B | chunks/slice | 1/N |
|---:|---|---:|---:|---:|---:|
| 4 | 5,5,5,2 | 320/128 | 9,464 *(meas.)* | **6.65** *(meas.)* | 25% |
| 6 | 3x5, 2 | 192/128 | 6,306 | 4.43 | 16.7% |
| 9 | 2x8, 1 | 128/64 | 4,204 | 2.95 | 11.1% |
| 17 | 1x17 | 64 | 2,101 *(meas.)* | **1.47** *(meas.)* | 5.9% |

The N=17 row is measured, not extrapolated: the GDR refresh AU **already is**
a 17-slice picture, and this capture holds three of them (AU 120/240/360) at
addresses **0,30,60,…,480** — one CTU row each, 34.6–36.5 kB total.

**Why more slices pays.** A lost chunk destroys every slice it touches, so
the concealed area is `bytes_lost_fraction + bursts/N`: **1/N is a pure
granularity penalty** paid on top of the real loss. Expected intact picture
fraction `(1-q)^(chunks per slice)`:

| chunk loss q | N=4 | N=9 | N=17 |
|---:|---:|---:|---:|
| 10% | 49.6% | 73.3% | 84.8% |
| 20% | **22.7%** | 51.8% | **70.6%** |
| 30% | 9.3% | 34.9% | 59.2% |

**Where it stops paying: one slice per chunk.** Below ~1424 B/slice a single
lost chunk spans two slices and finer buys nothing but coding cost. That knee
is at `N* = frame_bytes / 1424` = **26.6** here — *above* the encoder's 17
ceiling, so at this operating point more slices pays monotonically all the
way up. N\* is bitrate-dependent: 12 Mbps@60 → 17.6 (still 17); 8 Mbps@60 →
11.7 (→ 9); 18 Mbps@100 fps → 15.8 (17, at the knee). **Rule for the mode
store: N = clamp(frame_bytes/1424) onto the achievable set, capped at the
picture's CTU-row count** (1080p 17, 720p 12).

**One design simplification falls out at 17:** slices become uniform — N=4's
last slice is 2 rows against the others' 5, so today both the loss exposure
and the concealed area are lopsided.

> ~~(a) The steady AU and the GDR refresh AU become the *same* 17-address
> geometry, so the shape change that forced the two-frame adoption gate never
> occurs again.~~ **WITHDRAWN** by the QP-sweep entry above (same date): the
> refresh AU's geometry is independent of `sliceCount` and was measured at
> **9 slices of 2 CTU rows** at both N=4 and N=17 that evening, having been
> 17 slices of 1 row that morning. Under any N the refresh AU generally
> presents a different address list once per GOP — which is what the
> two-frame gate is for.

**Costs, priced.** Per-slice NAL overhead ~0.4% of the frame. Our repair
CPU scales with N (57.6 µs salvage at 4 → ~200 µs at 17, against a 16.7 ms
frame). Plumbing is **already proven at 17 NALs/AU** — the refresh AUs flow
through the frame ring and the venc packetInfo walker today with no
truncation WARN — but at 0.5/s, not 60/s.

**Open — the one unmeasured number.** Slice breaks reset CABAC and cut
prediction, which under CBR surfaces as QP (quality), not bitrate. Measured
only at N=4 (**≤0.9 QP**). Expect 3–8% BD-rate for 1080p HEVC at 16 slices,
i.e. ~0.5–1.5 QP. **Phase B extension** (short craft run, no RF judgment):
same scene, hold `resilience`/bitrate/fps fixed, sweep `sliceCount` ∈
{4, 6, 9, 17}, record delivered count (confirm 17 is really 17), achieved
QP, achieved bitrate, fps hold, and that the packetInfo WARN stays silent at
60 frames/s. Also watch `maxPBytes` (31,291 B) for interaction with the
extra overhead. **Ship 17 unless its QP cost exceeds ~1 step over 4; fall
back to 9 if it does.** The trade is clean-frame quality against damaged-frame
area, and an FPV link at range lives near the cliff where the damaged frames
are the ones being flown by.

## 2026-08-20 — external cross-check: rkvdec-slice-lab (RK3588 MPP / Intel VAAPI fed damaged bitstreams) vs §6.3b

An independent lab (github.com/josephnef/rkvdec-slice-lab) fed slice-dropped,
truncated and replayed H.264/H.265 to RK3588 vendor MPP and Intel iHD VAAPI.
Corpus: majestic SSC30KQ camera streams, 4-slice HEVC, **IDR every 20
frames** — no SVC-T, no LT refs, no GDR. Checked against our solution.

**Confirms four §6.3b decisions independently.**
- Truncated slice corrupts *following* slices (their 28.5 dB, worse than a
  clean drop's 29.4) → the venc abort-not-truncate fix is right.
- H.265 first-slice loss discards the whole picture (MPP parser refuses) →
  our first-slice synthesis is load-bearing, not optional.
- A mid-picture slice gap decodes **silently wrong on MPP HEVC**: errinfo 0
  on all 161 frames, deterministic, ~8% of the picture damaged → never ship
  a gap AU. Our repair emits complete AUs or drops whole frames; MPP never
  sees a gap from us.
- Verbatim slice replay is decoder-specific: helps on MPP, **rejected**
  (H.264 frame_num validation) or **−20 dB** (H.265, 42.95 vs 62.97 drop) on
  VAAPI. Their own close: portable repair "requires rewriting slice headers
  with current picture parameters… not merely copying them" — which is
  exactly what §6.3b does, and why our repaired streams pass HM + vah265dec.

**Their central recommendation is already in our ground stack.** "Fix the
error policy, don't repair the bitstream" = `MPP_DEC_SET_DISABLE_ERROR
0xffff` + present-don't-discard: waybeam-hub has shipped both for a while
(`src/pixelpilot/video_decoder.c:1055`, PARTIAL mode + errinfo→IDR-request
at `:1349-1360`). Their H.264 freeze-latch root cause (ref_err gates HW
submission; parser/callback race under back-pressure; sticky dpb_err_flag
clears only on I-slices — bad news for a GDR stream) is the mechanism
*behind* why that flag matters; the class is defused on our ground.

**The "let hardware conceal" simplification is NOT available to us.** Their
result leans on the corpus: damage self-heals at the next IDR, ≤20 frames
away. Our craft emits none unprompted, and the errinfo-silent failure means
the hub's IDR requester would never fire — a gap AU would smear via TMVP
with no signal and no bound. First-slice losses (1-in-4 at our geometry)
lose the picture outright, and the x86 ground (GStreamer vah265dec) and
Android MediaCodec would need their patched-decoder treatment per stack.
Phase D already priced repair vs drop on our stream: 231 vs 50 of 362.

**Residue worth keeping (parked, operator call): replay-with-rewrite.** On
camera content their replay measured 64.1 dB against a 31.6 dB ideal
temporal copy — last frame's *motion+residuals* against shifted refs beats
a pure freeze (H.264/MPP numbers). §6.3b already owns the hard half (donor
header rewrite: POC, RPS span, first-slice); the variant would splice the
previous picture's co-located slice *payload* under the rewritten header
instead of synthesizing all-skip. Open risks before it is worth building:
CABAC payload assumes the donor's ref-list length and slice-QP (must match
or carry qp_delta/cabac_init from the donor header); our SVC-T LT-ref
ladder shifts what "previous picture at this address" references; and their
only HEVC replay datapoint was *negative*. Our all-skip with TMVP merge
already extrapolates motion, so our baseline sits above a plain temporal
copy. Measure against the real capture before believing the +dB.

**Phase C methodology note.** MPP errinfo silence is necessary but NOT
sufficient (their errinfo-0-while-8%-wrong result): pair the rk3566
acceptance with a decoded-output compare or visual check, not log absence
alone. Their per-MB-row damage-localization diff (compare_yuv.py) is a
good shape to borrow for that readout.

## 2026-08-20 — Phases B/C1/D on the real capture: 4 is the fleet default, and SVC-T narrows freeze to ~3/5 of positions

Continuation of the Phase A bench session (same craft, same 18.0 Mbps CBR
1080p60 scene, hub running so the ring has its consumer — see the trap
below).

**Phase B — encoder cost of `sliceCount` ∈ {1,2,4,8}** (6 s capture each,
md5-verified, hub running):

| requested | delivered | mean frame | mean SliceQP | max slice | p99 slice | rate |
|---|---|---|---|---|---|---|
| 1 | 1 | 36.8 KB | 26.3 | 60.2 KB | 42.7 KB | 18.1 Mbps |
| 2 | 2 (9/8 CTU rows) | 36.9 KB | 26.7 | 43.9 KB | 33.0 KB | 18.2 Mbps |
| 4 | 4 (5/5/5/2) | 36.8 KB | 26.8 | 24.6 KB | 18.1 KB | 18.2 Mbps |
| 8 | **6** (3 rows each) | 37.1 KB | 27.2 | 15.7 KB | 11.5 KB | 18.3 Mbps |

60 fps at every level, CBR holds target, QP cost ≤ 0.9 across the whole
range. **N=8 quantizes to 6** (the SDK rounds the 32-px row request up to
whole CTU-64 rows), so request what divides: **fleet default = 4** —
delivered exactly, max slice 25 KB, QP cost 0.5. The 17-slice GDR refresh
AU appears at EVERY sliceCount including 1 — it is `intraRefresh`'s own
geometry, so today's fleet stream already carries multi-NAL refresh packs
through the frame ring.

**The no-consumer trap.** With the hub stopped (ring unconsumed), venc
degrades to an IDR storm — one IDR every ~7 frames, GDR suppressed, CBR
undershooting a static scene at 2.7 Mbps. A first sweep taken that way was
same-conditions-valid but flight-unreal; discard rate numbers captured with
no ring consumer.

**Phase C step 1** (production chain `spatial_conceal_bench`, real capture,
200‰ seed 42): 262/362 delivered (fast 2, fec 50, salvaged 180, frozen 30,
dropped 100), salvage cost **avg 57.6 µs, max 129 µs** (release, x86). HM
zero-assert on the output; **vah265dec (VAAPI hardware) accepts it** —
first hardware-decoder acceptance of repaired real-content streams. The
plan's "confirm dropped=0" was a synthetic-content expectation: on this
stream freeze is only eligible where the last two delivered pictures carry
identical RPS bits, and the enhance slices' long-term entry names the
*current base* POC, so RPS bits change every 5 frames — freeze eligibility
is ~3/5 of positions, and the new sub-layer non-reference donor refusal
removes the slot right after each TRAIL_N. dropped=0 is not reachable at
20% on SVC-T content by design; widening freeze eligibility would need
position-relative treatment of `poc_lsb_lt` (an operator ruling, not a
knob).

**Phase D re-run with the real capture as `CONCEAL_ES`** (udp-air, release
binaries, 60 fps, 362 frames, p_rate 100‰):

| loss | conceal | delivered | salvaged/frozen | salvage_failed |
|---|---|---|---|---|
| 20% | slice-skip | **231/362** | 144 / 38 | 131 |
| 20% | off | **50/362** | — | — |
| 30% | slice-skip | 136/362 | 93 / 39 | 226 |

4.6× the control at 20%. Egress judgment: HM zero-assert, GStreamer clean,
ffmpeg shows only the expected missing-ref lines from wholly-dropped frames
(the ride-the-gap behaviour; on this GDR craft they heal asymptotically —
there is no next IDR unless requested). Absolute delivery is below the
synthetic 996/1000 for the same three reasons: bigger frames (37 KB vs
20 KB → more chunks in danger per picture), narrower freeze eligibility,
and the TRAIL_N donor refusal.

**Whole-frame freeze vs plain drop (operator question 2026-08-20,
decision).** The freeze is NOT only pacing on the shipped ground stack:
waybeam-hub's MPP decoder defaults to `DECODER_ERROR_MODE_PARTIAL`
(`src/pixelpilot/config.c:263`) — after a dropped reference it **presents**
the dependent errinfo frames (real corruption on screen, not a skip) and
every errinfo fires `idr_requester_handle_warning`
(`src/pixelpilot/video_decoder.c:1358`), i.e. sustained drops become IDR
request storms exactly at the loss levels §6.3b operates in. A freeze keeps
the DPB conformant: no errinfo on the followers, no corrupt presentation,
no IDR bandwidth spike; the residual heals over GDR. Decision: **keep
`freeze_frame` default true**; it is fail-safe gated and costs ~58 µs.
Confirming A/B for Phase C/E (knob already exists, no code needed): same
loss run with `freeze_frame` on vs off on the rk3566 hub, count
`MPP: presenting partial frame errinfo` lines and IDR requests.

Decoded-frame A/B on the capture (same AU 83 lost both ways, ffmpeg
`-threads 1`): freeze emits an imperceptible repeat (luma mean 0.99 vs
truth) and the stream is **bit-exact again 2 frames later** at the next
base picture; a plain drop makes the next frame decode against a
substituted reference — one visibly smeared frame (mean 19.7, max 123,
worst in the moving band) that is precisely the errinfo frame MPP
presents — then heals at the same base. Both heal fast because this
craft's SVC-T base frames reference only the long-term chain, so
enhance-frame damage cannot cross a base boundary. Bonus control: a
freeze from a *base* donor (CLI-only, gate bypassed) evicts the live LT
base from the DPB and desyncs the whole picture permanently (mean 69) —
empirical proof the RPS-steady-state gate is load-bearing.

**Open:** Phase C MPP (rk3566) and Android deferred by operator ruling
2026-08-20. Phase E (live RF walk-down) needs an operator-attended RF
session — numeric prep is done, the visual judgment is the operator's.

## 2026-08-20 — Phase A: the real SSC338Q 4-slice stream, its shape, and two freeze defects it exposed

Bench session, craft `.232` (SSC338Q, venc 0.66.0 `video0.sliceCount: 4`,
1080p60, `intraRefresh balanced lines/P=2`, SVC-T `refPred base=1 enhance=4`).
Boot log printed `VENC: H.265 slice split ON: 9 CTU rows/slice (34 rows -> 4
slices)` and the packetInfo-cap WARN never fired. 6 s raw ES captured with
the on-board recorder (`record.format=hevc`, byte-identical packetInfo walk
to the frame-ring blob), 362 AUs, md5-verified both ends.

**The measured stream shape** (`capture_232_4slice.265`, kept on the bench):

- **CTU is 64, not 32.** SPS says 30×17 CTBs. The SDK read our "9 rows" in
  32-px units and rounded up to 5 CTU rows: slice addresses {0,150,300,450}
  = pixel bands {0,320,640,960} (5/5/5/2 CTU rows) — 4 slices requested, 4
  delivered, at fixed addresses on every AU shape. The venc log line's "CTU
  rows" wording is off by the unit but the outcome is right; higher
  sliceCounts will quantize (Phase B measures).
- **Independent slices** (`dependent_slice_segment_flag=0` everywhere)
  though the PPS *enables* dependent slices. No tiles, no WPP. SAO ON,
  TMVP ON, `lists_modification_present=1`, `long_term_ref_pics_present=1`.
- **SVC-T rides long-term refs**: base pictures (POC%5==0) carry an empty
  st RPS + LT(prev base, used=1); enhance pictures carry st(prev, used=1) +
  LT(cur base, used=0); every fifth picture (POC%5==4) is **TRAIL_N** —
  and only its FIRST slice: the encoder mixes TRAIL_N/TRAIL_R inside one
  picture, which is non-conformant (H.265 7.4.2.2) — HM tolerates it,
  ffmpeg warns and its **frame-threaded decode goes non-deterministic**
  (two decodes of the same capture diverge). Judge fix: `validate.py` now
  decodes `-threads 1`.
- **GDR refresh AUs have their own geometry**: every 120th AU (the 2 s GOP
  boundary) is a 17-slice picture, one CTU row per slice, preceded by
  re-emitted VPS/SPS/PPS. Only the first AU (recorder-forced) is an IDR;
  IDR AU = 7 NALs (VPS+SPS+PPS+4 slices), inside the 8-entry table.
  Delivery is multi-pack: the capture holds all 17 NALs, so no pack
  exceeded the table.

**Judging on the real capture** (hevc_conceal_cli + validate.py + HM-18.0 +
ffmpeg + GStreamer): single-slice and short-last-slice conceal pass — frame
counts equal, pre-frames identical, untouched rows byte-identical once the
validator uses the real {0,320,640,960} geometry, HM zero-assert. TMVP-on
means the concealed interior is motion-extrapolated, not frozen (as
documented). GDR wash-out is asymptotic: repair error mean 0.30→0.12,
p99 2 gray levels after one refresh wave; exact resync never happens
without an IDR — on this craft IDRs are on-demand only.

**Defect 1 — unused long-term pics counted into NumPicTotalCurr.** The
parser summed ALL long-term entries into `num_used_refs`; a TRAIL_N donor
(LT used=0, real NumPicTotalCurr=1) made parser and writer disagree with
the decoder over the `lists_modification` bit — one-bit shift, HM
`readByteAlignment` assert. Fixed: only `used_by_curr_pic_lt_flag=1`
entries count (SPS-level flags now stored). Pinned in `hevc_conceal_test`
with the real capture's SPS/PPS/TRAIL_N header bytes.

**Defect 2 — whole-frame freeze from a TRAIL_N donor is structurally
unsound.** The synth inherits the donor's `nal_unit_type` (later real
pictures then reference a sub-layer non-reference picture) and its copied
RPS names the donor's own TRAIL_N picture as used — HEVC 7.4.2.2 both
ways; HM asserts in `applyReferencePictureSet`. Production-reachable: the
RPS-steady-state gate passes inside a base period (AUs 81–84 carry
identical RPS bits). Ruled on-branch (§6.3b freeze paragraph amended):
freeze refuses sub-layer non-reference donors — on this craft 1 in 5
whole-frame losses falls back to the pre-§6.3b drop; slice salvage is
unaffected.

**Open:** Phase B (encoder cost of sliceCount ∈ {1,2,4,8}); Phase D re-run
with this capture as `CONCEAL_ES`; whether waybeam-link's RX parser needs
anything for the 17-slice refresh geometry (learned-geometry rule says a
refresh AU's survivors mismatch the 4-slice geometry → salvage refuses →
frozen/dropped; measure how often that bites in Phase D/E).

## 2026-08-20 — CTU32 content, TMVP conformance, RPS-gated freeze, GDR convergence

Four §6.3b results from the x86 continuation session, all offline/loopback.

**CTU32 (the SSC338Q shape) validated on real coded content.** HM-18.0,
1280×720, `MaxCUWidth=32`, 40×23 CTU grid with a partial bottom row, 4
slices, SAO on, TMVP on: single/bottom/pair concealment all decode clean on
HM + ffmpeg with pixel-exact frozen regions and byte-exact untouched slices.
The forced-split partial-CTU path now has real-encoder coverage, not just
synthesis-side.

**TMVP conformance bug found and fixed.** The synthesized header used to
force a 1-entry L0 list; HEVC §7.4.7.1 requires `collocated_ref_idx` (and
the list shape it indexes) to agree across a picture's slices, so on a
TMVP-on stream with >1 ref (HM lowdelay, 4 refs) the mix was nonconformant —
every decoder stayed SILENT while *other* slices of the picture decoded
wrong (untouched-rows maxdiff 148). Pixel assertions caught what three
decoders' error paths did not. Fix: mirror the donor's L0 count and
collocated index; TMVP-on concealment is motion-extrapolated rather than
frozen, TMVP-off unchanged.

**Freeze is now RPS-steady-state-gated.** Whole-frame freeze replays the
donor's RPS at POC+1, which HM's cycling GOP-4 sets made invalid ("Could
not find ref" on the freeze output). Guard: freeze only when the last two
delivered P pictures carried identical RPS bits. A first "any change ever"
latch was wrong the other way — x265's first P after each IDR legitimately
shrinks the set (smaller DPB) and the latch killed freeze forever (udp-air
20%: frozen 150 → 0, failed 4 → 177). The per-frame form restores it
(frozen 153, failed 6 = the transients, delivered 994/1000).

**GDR × concealment, measured.** x265 `--intra-refresh` (wave period 100):
a 10-frame burst concealing one slice band propagates slowly (luma MAD
0.44 → 1.05 over 90 frames), the first refresh wave cuts it ~10× (→0.39),
and later waves keep shrinking it — but convergence is asymptotic (~0.2 MAD
residual creeping with motion), because x265's PIR does not perfectly
isolate the dirty region. Control on the IDR stream: identical burst, exact
**0.000** at the next IDR — the concealment slices are byte-clean; the
residual is the encoder's PIR looseness. Under *sustained* 20% i.i.d. loss a
GDR stream sits at an error equilibrium (~45 MAD on moving test content) —
convergence needs loss-free wave windows; IDR streams resync exactly
regardless. Also observed: with no IDR ever delivered (all four died at
20%), salvage correctly refuses everything until the first header-carrying
AU lands (219 refusals, all in the first 2 s, zero after) — those frames are
undecodable downstream anyway; §3.9 recovery shortens the window in
production. SigmaStar's own GDR enforcement may be tighter or looser than
x265's — measure on device (runbook Phase F).

---

## 2026-08-20 — Phase D: the two-node link over udp-air holds the timeline through 30% loss

**Setup.** Real TX + RX `waybeam-link` processes on one x86 host, udp-air on
loopback, single path (one tx socket, one rx listen — the drop knob IS the
symbol loss). Ingress: 1000 frames of 1920×1080@100 4-slice x265 played into
the TX `venc_frame` ring at wire rate (`frame_shm_feed play`, new); egress:
the RX `venc_frame_out` ring dumped back to Annex-B (`frame_shm_feed dump`,
new) and judged by ffmpeg + GStreamer `h265parse ! avdec_h265`. FEC
p_rate 100‰, min_k 3, arq_mode idr-only; `air.rx_drop_permille` swept.
Runner: `tools/spatial_conceal/udp_air_run.sh`.

| drop | conceal | delivered | fast | FEC | salvaged | frozen | failed | decode |
|---|---|---|---|---|---|---|---|---|
| 0 | on | **1000**/1000 | 999 | 1 | 0 | 0 | 0 | 1000, clean, byte-total exact |
| 10% | on | **999** | 218 | 528 | 205 | 48 | 1 | 999, 0 errors, 2 ref-misses |
| 15% | on | **999** | 88 | 437 | 383 | 91 | 1 | clean |
| 20% | on | **996** | 32 | 262 | 552 | 150 | 4 | 996, 0 errors, 6 ref-misses |
| 30% | on | **992** | 3 | 52 | 651 | 286 | 8 | 992, 0 errors, 14 ref-misses |
| 20% | **off** | **313** | 32 | 281 | — | — | — | 313, 436 ref-misses |

`salvage_failed` stays in single digits and every one becomes a plain drop
(fail-safe held); GStreamer accepted every conceal-on egress silently.

**The IDR observation** (matters for Phase E): delivered-as-IDR count falls
with loss (10 → 9 → 6 → 2 of 10). §6.3a's zero-reorder rule gives IDR ARQ
only until the next block's first symbol arrives (~one frame period), so at
blanket 20–30% loss some IDR blocks finalize below k and the freeze path
stands in with a P — the stream then rides on reference gaps (single-digit
ref-misses end-to-end) until the next IDR. Pre-existing supersession
behaviour interacting with §6.3b, not introduced by it; production has §3.9
recovery + venc recovery-IDR on top. Watch it on RF; if the ride-through
looks bad visually, the lever is i_rate/min_r for IDR blocks, not the
concealment.

**Open.** Phases A/B/E (craft `.232` stream shape, slice-count cost, live
RF) still pending — this host cannot reach the bench LAN. rk3566 and
Android deliberately deferred.

---

## 2026-08-19 — slice-skip concealment measured offline: the FEC cliff becomes a slope

**Setup.** Offline, no radios: 512×512 @ 100 fps test content, 4 independent
slices/picture, P-only, 1 ref. Encoders: x265 3.5 (WPP forced — x265 refuses
multi-slice without it) and HM-18.0 (`SliceMode=1`, no WPP, SAO **on**,
TMVP both off and on — the shapes a hardware encoder emits). Decoders:
ffmpeg 6.1 (`framemd5`) and libde265 1.0.15. Harness:
`tools/spatial_conceal/` (this branch).

**Substitution proof.** Replacing any 1, any 2 (adjacent and scattered), or
all 4 slices of a P picture with synthesized all-skip slices decodes clean on
both decoders; the concealed region's interior is byte-equal to the previous
picture, untouched slices byte-equal to the reference decode, and the next
IDR resyncs exactly. A 41–1460 B coded slice is replaced by a **10–12 B**
concealment slice. Error propagates only inside the concealed region (plus
motion bleed) until the next IDR/GDR pass — the desired HDZero-like shape.

**Full-chain sim** (blob → s-byte chunks → i.i.d. loss → §14.1 model:
full decode iff received ≥ k, else salvage → §6.3b rebuild → decode;
s=500, r=2, IRAP protected as ARQ would):

| loss | clean | FEC | salvaged | frozen | dropped (was) |
|---|---|---|---|---|---|
| 15% | 41 | 65 | 11 | 3 | **0** (14) |
| 25% | 18 | 53 | 34 | 15 | **0** (49) |
| 40% | 6 | 22 | 57 | 34 | **1** (92) |

All emitted streams decode to full frame count; the only decoder complaints
are refs to genuinely dropped frames.

**Production-chain confirmation** (`tools/spatial_conceal_bench`: real
FrameFramer → xorshift symbol loss → FrameReassembler → SpatialRepair, IDR
loss exempt as ARQ would make it; 1080p 4-slice x265, p_rate 100‰):
16% source-symbol loss → 0/40 dropped (23 salvaged, 3 frozen); 31% → 0/40
(27 salvaged, 11 frozen); every output decodes fully on ffmpeg, GStreamer
`h265parse ! avdec_h265` (the ground hub's x86 graph), and HM-18.0. Salvage
cost avg 97 µs / max 211 µs per repaired frame (x86 release); bare synthesis
21.6 µs per quarter-1080p CTU32 slice. HM in the loop caught two conformance
bugs (extra end_of_subset alignment bit; missing WPP entry points) that
ffmpeg/libde265 tolerated — keep it in every writer change.

**Open.** SSC338Q slice-split output shape (packs vs `packetInfo[8]` clamp),
per-slice bitrate overhead at 4/8 slices, on-device repair latency, RK3566 /
Android MediaCodec acceptance of repaired AUs (VAAPI-class ffmpeg decode is
covered above), GDR × concealment convergence measurement. Closed to contract
by Pass 185 (§6.3b); these numbers stay Tier-2.

---

## 2026-08-16 — the RTL8733BU actuator lands, and all three fleet floors are measured

**Setup.** devourer re-vendored `c8f3531` → `5bf059a` (#399), which gives the
RTL8733BU a §10.5 TX-power actuator on the TSSI target table. Nothing on this
side changed: `io/src/air_radio.cpp` sets
`power_actuator_ok = dev->GetTxPowerCaps().supported`, that flips false→true on
the vendor bump, and the CV610 craft's `/api/v1/tx/power` went from
`"actuator":"none"` to `"actuator":"offset"` on the first restart.

**Method.** Each node's offset was driven from its own control plane and judged
at the FAR END — ground RSSI for a craft's TX, craft RSSI for the ground's
uplink. The API echo is deliberately not the evidence: the pre-#399 8733BU
failure was an endpoint reporting `applied_qdb` correctly while writing
nothing. Each sweep carries the opposite direction as a control.

| node | adapter | floor | travel | far-end RSSI, 0 → floor |
|---|---|---|---|---|
| `.181` craft | RTL8733BU | **−96 qdb** | 24 dB | −32 → −56 |
| `.232` craft | RTL8812EU | **−48 qdb** | 14 dB | −13 → −27 |
| `.242` ground | RTL8812AU | **−72 qdb** | 18 dB | −45 → −63 |

The 8733BU sweep (0/−16/−32/−48/−64/−80/−96 → −32/−38/−40/−42/−46/−54/−56)
is **96 qdb commanded, 24 dB delivered — dead-on the 0.25 dB/qdB nominal**,
with delivered video flat at ~100 fps and loss flat at 7‰ at every step, and
the craft's own RX of the uplink flat at −49/−50 throughout. −112 and −128 add
nothing; past −128 `saturated_low` goes true and `applied_qdb` clamps at the
int8 rail. This is cleaner than the 2026-08-14 register-level measurement on
the x86 host (0.222–0.231 dB/qdB with a half-slope knee below −52) — different
unit and geometry, so the knee is a per-unit property, not a family one.

The 8812AU held exactly 0.25 dB/qdB to −72 and saturated at −96; its config
ladder's existing bottom rung is therefore precisely its floor. The 8812EU is
the outlier: only 14 dB, pinned from −72 down, so two of its five ladder rungs
sit below the floor (they saturate honestly, so they were left alone).

**A hypothesis that did NOT survive.** During the 8812EU sweep, delivered fps
climbed 30 → 100 monotonically as power dropped, which looks exactly like
near-field RX compression. Re-running 0 / −48 / 0 / −48 gave 87.3 / 74.8 /
93.5 / 99.8 with loss flat at 11‰ — no correlation with power at all. The climb
was the craft settling into its 100 fps mode after the claim. **RSSI was
reversible within 2 dB; fps was not a function of power.** Near-field
compression may still be real at this geometry, but this run is not evidence
for it.

**Consequence.** All three `deploy/*.json` now pin `power_offset_qdb` at the
measured floor, because bench sessions restart the link constantly and a
runtime-only backoff is lost every time. `deploy/README.md` carries the
raise-before-flight warning and the table.

**A second hypothesis that did not survive, and the real cause behind it.** At
−96 the CV610 craft was missed by 2 of 4 scout sweeps that found the 8812EU
craft every time, which looked like bench-low power costing discovery. It is
not power. Pooled over every level tested, all with the 8812EU craft live:

| craft TX offset | scout detections |
|---|---|
| −96 qdb | 12/16 (75%) |
| −72 qdb | 10/12 (83%) |
| −48 qdb | 3/4 (75%) |
| 8812EU craft, any level | **28/28 (100%)** |

Nor is it dwell: at −96, `dwell_ms` 300 and 500 both gave 5/6.

**It is the other craft.** Operator hypothesis, tested by unplugging the 8812EU
craft and repeating the sweep by hand at the CV610's *lowest* setting (−96):

| 8812EU craft | CV610 detections | CV610 beacon frames | CV610 RSSI |
|---|---|---|---|
| live, 5745, −16 dBm | ~78% pooled | ~820–840 | −50…−54 |
| **unplugged** | **10/10** | **~2046** | −48, ±0 |

The CV610 was on 5660 and the 8812EU on 5745 — **85 MHz apart, and the strong
craft still cost the weak one 60% of its received beacon frames and a fifth of
its detections.** This is the same desense as the adjacency wall above but
reaching far beyond a guard band: it is the ground's single 8812AU receiver
being pulled down by a signal 36 dB above the one it is trying to hear, not a
channel-overlap effect.

**Consequence: the 60 MHz separation rule is necessary but NOT sufficient.**
What governs is the *power imbalance at the receiver*, and on this bench it
cannot be fixed with the §10.5 knob alone — at both crafts' measured floors the
8812EU still lands ~20 dB above the CV610 at the ground (−28 vs −48). Balancing
a mixed-family pair needs physical attenuation on the loud craft, much more
separation, or not running both at once. `emptiest_channel`'s guard band
(2026-08-16, above) does not address this and was never meant to.

**Open.** (a) The 8812EU's 14 dB is short of the 8812AU's 18 dB on the same
ladder — unexplained, and worth a register-level look before trusting the EU
ladder's lower rungs. (b) How far the cross-craft desense reaches is not
bounded: 85 MHz was measured, the band edge was not. (c) The 8812EU floor is
what blocks a balanced two-craft bench; whether the unported flat TXAGC index
or a physical pad is the answer is undecided.
## 2026-08-16 — quickconnect parked a craft one channel from a live craft; adjacency is as fatal as co-channel

**Setup.** Two crafts live at once for the first time: `.232` (SSC338Q,
8812EU, originator 17) and `.181` (CV610, 8733BU, originator 18, the weaker
radio by ~14–16 dB at the ground). Ground `.242`, single 8812AU TX/RX combo,
in-process `mod_wblink`. Channel lists widened from 7 to the full 25-channel
5 GHz band on all three nodes. Craft 18 running `imx662-060fps-mcs1`, nominal
60 fps; delivered fps read at the ground sink over 4–8 s windows.

**Measured.** `POST /api/v1/scout/quickconnect {"originator":18}` committed
craft 18 onto **5700 while craft 17 was transmitting on 5720**. The link
reported `state: committed`, `HOLD`, `rssi_best −30`, and carried
**0.2 fps at 0.02 Mbps** — a dead video link the control plane called healthy.

Full 25-channel CSA walk of each craft, one at a time:

| craft | result |
|---|---|
| 17 (8812EU) | **25/25** channels carried video, 96.5–100.2 fps, RSSI −16…−32 |
| 18 (8733BU) | 23/25; the two failures were 5745 and 5700 — the channels adjacent to craft 17, then parked on 5720 |

Controls, same craft, same ground, same channels, only craft 17's transmitter
changing:

| ch (craft 18) | Δf from craft 17 | 17 transmitting | 17 stopped |
|---|---|---|---|
| 5700 | 20 MHz | 1.9 fps | **59.9** |
| 5745 | 25 MHz | 0.4 fps | **59.9** |
| 5500 | 220 MHz | 50.9 fps | 60.4 |

and the offset curve with craft 17 back on its home 5805 (nominal 60):

| Δf | ±20 | ±40 | ±60 | ±85 | ≥105 |
|---|---|---|---|---|---|
| fps | 0.0–0.9 | 20.8 | 54.5 | 54.8 | 37.5–50.4 |

**What it means.** Two things, and only the second is a bug in this repo.

1. **Minimum craft separation is 60 MHz (three HT20 slots).** 20 MHz is
   indistinguishable from co-channel; 40 MHz gives a third of nominal.
   This is a deployment rule, not something software can fix.
2. **`emptiest_channel()` ranked channels as independent bins.** It correctly
   scored 5720 occupied (`util_permille` 1000, its traffic decodes) and then
   handed out the channel next to it. The 25-channel sweep shows why nothing
   stopped it: every *empty* channel reads 891–968 permille — a 77-permille
   spread with no signal in it — so which empty channel wins is decided by
   noise, and one of the noise winners is 20 MHz from a live craft. Fixed by
   ranking on the worst utilisation within `policy.csa.adjacent_guard_mhz`
   (seed **40**, 0 = old behaviour), tie-broken on the channel's own value.

**Also measured, not yet acted on.** `rssi_best` (−24…−32) and `loss_milli`
(213–260) were **flat** across the whole range from 0 fps to 60 fps, and
`/link/health` reported `HOLD` throughout. No field in the health contract
distinguishes a working link from a jammed one here; delivered frame rate at
the sink was the only signal that moved.

**How much the guard actually buys — measured, not assumed.** The bad pick
was seen once and could NOT be reproduced on demand: with craft 17 parked at
5280 (mid-band, so its neighbours are in the low-scoring cluster) and the
guard disabled, five consecutive quick-connects all landed ≥60 MHz away, and
so did three with the guard on. The pick is noise-driven, so the bench cannot
be made to fail on cue. Replaying `emptiest_channel()` over a real
`/api/v1/scout/results` snapshot is what shows the exposure:

```
craft 17 live on 5280; craft 18 being moved off 5220
chan   util   guard40   
5240   938    1000     ADJACENT (40 MHz)
5260   932    1000     ADJACENT (20 MHz)
5300   928    1000     ADJACENT (20 MHz)
5320   891    1000     ADJACENT (40 MHz)   <- 7th lowest of 24 unguarded
5600   821    881      <- picked, either way
```

Two things follow. **The interference score does not see the neighbour**: the
four channels flanking a live craft read 891–938, squarely inside the
empty-band population (847–953) — an earlier guess that adjacent-channel
energy would already deprioritise them is wrong. And **the guard removes a
class of picks rather than changing this one**: 5320 at 891 beats seven other
allowlisted channels and is a live candidate on any sweep whose noise favours
it; guarded, it cannot be chosen at all. Both configurations picked 5600 here.

**One correction to the guard itself, found the same way.** The first version
let `except` — the craft being relocated — guard its own neighbours. Replayed
against the snapshot above that excluded 5180/5200/5240/5260 because craft 18
was sitting on 5220, which it is about to leave. The mover's emission travels
with it and must not guard; another craft's must. Fixed, with a test that
separates the two cases.

**Open.** (a) The 40 MHz seed comes from one power ratio on one pair — a
higher-power craft plausibly needs more, and an HT40 deployment certainly
does. (b) `emptiest()` still ignores the scout's own candidate list, which
*names* the other craft and its channel. That list is strictly better
evidence than occupancy here — it survives a craft whose traffic the ground
cannot decode, and it is the obvious next step. (c) The empty-band floor of
~850–950 permille makes `ranking.recommendation` return `BROAD_DEGRADATION`
on a genuinely empty band — the `#173` FA-index-vs-occupancy question,
unchanged by this finding. (d) The health-contract blind spot above.

---

## 2026-08-15 — 8733BU rung-5 drain ceiling is ~73% of the table-derived target; and one unsupported venc field (501) starves ALL bitrate pushes

**Setup.** CV610 craft (.181, 8733BU, 5745/HT20, table-8733b all long-GI,
rung 5 = MCS5 lgi, quiet_gap on, FEC 300/200) feeding 1080p100 frame-shm;
ground = .242 in-process mod_wblink, near-field (RSSI −30, SNR 31). venc
initially static 16 Mbps with `venc.enabled:false`. 20–30 s counter-delta
windows on both ends.

**Measured.** At 16 Mbps offered: link drains ~14.4 Mbps video at ~1740 pps,
ring chronically full — `shm_full_drops` 12.4 fps (12.5% of frames dropped
whole at the source), air loss only 1.1% pre-FEC, `tx_failed` 0. At 8 Mbps:
0 source drops, 98.8 fps. `derive_bitrate_kbps` on table-8733b rung 5 says
19.6 Mbps (52000 × 0.463 airtime × 0.82 FEC − 96 reserve) — the real
8733BU drain is ~73% of that. The airtime/FEC constants are 8812/SSC338Q
Pass-111 seeds; the 8733BU's per-packet cost (USB2, long-GI only, no
aggregation tuning) eats the difference. **Open:** §17 re-derivation for the
8733b hardware class; until then the derived target on EVERY rung overshoots
by a similar factor, and `max_bitrate_kbps` only clamps the top — a demote to
rung 2 (derived 9.3 Mbps, real ~6.8) will source-drop again at range.
Source-side ring overflow produces NO air loss, so the §9.5 selector never
sees it — it is invisible to demotion logic by construction.

**Actuation fix + a trap.** Flipping `venc.enabled:true` with
`max_bitrate_kbps:13000` (ceiling minus margin) holds 13.1 Mbps / 98.8 fps /
0 source drops at rung 5. But the first attempt silently actuated NOTHING:
CV610's venc returns **501** on `video0.maxIBytes`/`maxPBytes`, and the
caps txn's failure hold-off (`no_retry_until_ms_`, shared across txn kinds in
`io/src/venc_http.cpp`) starved the bitrate txn forever — 53 pushes, 53
failures, `commanded_bitrate_kbps` 0, no log line. `venc.frame_caps:false`
worked around it. **CLOSED 2026-08-29**: the caps and that config key are
removed on both sides, so the specific trigger is gone — a CV610 craft has no
cap txn left to 501. The general hazard is NOT closed and keeps its own
follow-up: the failure hold-off is still shared across txn kinds, so any 4xx
on one kind still starves the others. **Open:** the actuator should latch an
unsupported txn kind on 501/4xx the way it latches `live_fallback_` on 404,
and a persistent
all-pushes-failing state deserves a `wb_logf` line; residual ~1.2% pre-FEC
air loss at SNR 31 near-field is a floor across 8–16 Mbps offered rates —
unexplained, and where ground RX diversity would actually help.

**In-process addendum (same day).** After the craft migrated to waybeam-hub's
in-process `mod_wblink` (tx role, hub PR #197 path — first RF deployment of
it), the 13 Mbps clamp that measured ZERO source drops as a standalone
process dropped 0.3→2.3 fps across three windows (7/19/70 per window,
worsening), with the SoC at ~50% idle — not CPU starvation but TX-pump
scheduling margin lost to hub residency (the hub's own threads; its
`:8091` stats scrape is already 1 Hz-limited and is not the driver).
12000 kbps restores steady zero (0/0 drops across consecutive 30 s
windows, 99.5 fps, 12.2 Mbps). **Rule of thumb until the §17 re-derivation:
an in-process CV610 craft prices ~1 Mbps of clamp below the
dedicated-process ceiling.**

## 2026-08-15 — 8733B refuses SGI at the TX-descriptor gate: a short-GI profile rung silently kills every DATA frame; and two crafts on one channel starve each other regardless of net_id

**Setup.** First device deployment of `waybeam-link tx` on the CV610 craft
(.181, RTL8733BU, devourer, 5805/HT20, 1080p100 16 Mbps frame-shm ingest;
`deploy/vehicle-192.168.2.181.json`). Symptom: `tx_submitted` climbing at
~4.5 k/s with `tx_failed` = submitted − 190 — exactly 190 frames ever
succeeded, then a hard stop, with **zero log lines** (the reject is
`build_tx_block` returning 0, before the logging bulk-send path).

**Mechanism.** `profiles/table.example.json` rungs 3+ carry
`guard_interval: "short"`; the vendored 8733B TX path refuses HT with
SGI/LDPC/STBC outright (`ht_request_supported_8733b`:
`mcs<=7 && (bw 20|40) && !sgi && !ldpc && !stbc`). The craft boots on a
long-GI rung (the 190 good frames), the selector commits an SGI rung, and
every subsequent frame dies silently. Fix that works:
`profiles/table-8733b.json` — the same table with every rung long-GI —
verified 49k+ submits, 0 failed, sustained.

**What it means / what stays open.**
- The link commits a `TxRate` the die cannot fly and the failure is a
  per-frame silent counter, not a refusal. The right fix is caps-shaped
  (like `fastretune`): surface no-SGI in `AdapterCapsView` and clamp at
  rate commit with a stat — filed as an issue.
- A craft on the 8733B table has a different `table_version` than the
  fleet (0xA4 vs 0x5B) — fine solo (the §3.4 mismatch rule makes a future
  mismatched consumer best-effort), but a dual-craft ground consuming both
  needs a ruling on per-craft tables before it can run profile logic on
  this craft.
- **Co-channel occupancy is real and immediate:** with the CV610 at its
  §9.8 lost-feedback floor (MCS2 long-GI, 16 Mbps offered) on 5805, the
  .232 craft's link demoted 5→2 within a minute — `net_id` separates
  identity, not airtime. Moving the CV610 to 5745 restored .232 to
  profile 5 in seconds. Dual-craft-on-one-channel needs an airtime story
  (or per-craft channels) before it is a supported shape.

## 2026-08-15 — Retune settle is a per-HOST quantity, not a per-chip one: the same jaguar1 family reads ~42 ms on x86 and ~250 ms on Android, and the 8733BU's *call* blocks 345 ms

**Setup.** `hwtrial_bringup --settle` (new in this branch): DUT adapter
RX-only, continuous ~1 kHz emitter (`--tx` loop, second unit) on 5805/20,
quiet channel 5180, 20 cycles + 2 unsummarized warm-ups per arm. Deafness is
anchored at retune-call START and polled from a second thread, because the
first AU run proved the single-threaded, return-anchored definition
unmeasurable: on x86 the jaguar1 full retune blocks ~130 ms and the radio is
already live at return (deaf-from-return = 0.0 on all 20 cycles), while the
2026-08-13 Android scout measurement had a ~5 ms call and ~250 ms of
post-return silence. Same code, same chip family — the settle sits on a
different side of the call boundary per platform, so only the start-anchored
number compares across rigs.

**Numbers** (p50 over 20 clean cycles; spreads were tight, ≤2 ms except BU ≤10):

| chip | host | retune class | call blocks | radio live (start→first frame) |
|---|---|---|---|---|
| 8812AU (jaguar1) | x86 .242 | full | ~130 ms | **42.4 ms** (41.9–43.0) |
| 8812AU | x86 .242 | fast | ~41 ms | 42.2 ms |
| 8812CU (jaguar3) | x86 .242 | full | ~32 ms | **21.1 ms** (20.5–23.7) |
| 8812CU | x86 .242 | fast | ~13 ms | 12.6 ms |
| 8733BU | CV610 .181 | full | **~345 ms** | **70.1 ms** (61–80) |
| 8733BU | CV610 .181 | fast | ~277 ms | 70.2 ms |
| 8812EU (jaguar3) | SSC338Q .232 | full | ~21 ms | **8.1 ms** (6.1–11.3) |
| 8812EU | SSC338Q .232 | fast | (short) | 8.3 ms |
| 8812AU | Android S22 | full | ~5 ms | ~250 ms (2026-08-13 scout inference) |

Controls: no-emitter arm → exit 1, 0 clean samples, 20 honest TIMEOUTs;
quiet=emitter-channel arm → every sample flagged TAINTED and refused. Both
guards demonstrated firing, not just present.

**What it means.**

1. **The 8733BU retunes** (the fail-early question answered), and its fast
   class works: intrinsic settle ~70 ms either way.
2. **The ~250 ms "AU settle" was never a chip constant.** x86 shows the same
   silicon family live at 42 ms. The dominant term moves between the blocking
   call (x86: 130 ms; CV610+BU: 345 ms) and post-return silence (Android:
   ~250 ms) depending on the host's USB stack. A `retune_settle_ms` keyed on
   chip generation in `AdapterCapsView` would be wrong on every host it
   wasn't measured on.
3. **The scout charges the call against the dwell.** `ScoutEngine::
   enter_channel` sets `dwell_deadline = now + dwell_ms` with `now` sampled
   BEFORE the blocking retune, and `retune_one`'s post-call `flush_rx`
   discards every frame that arrived mid-call. On the BU that leaves
   ~155 ms of listening out of the shipped 500 ms dwell; a 300 ms dwell
   would listen for **zero** and miss every craft on every channel. The
   knee memory `scout_retune_radio_silence` found at 250→300 ms on Android
   is this same arithmetic with that platform's numbers.
4. Anchoring the dwell deadline AFTER the retune returns would make
   `dwell_ms` mean *listening time* on every chip/host without any per-chip
   constant — the scout would charge itself the measured cost instead of
   guessing it — leaving only genuinely post-return deafness (observed only
   on Android so far) to a settle allowance.

**Closed by Passes 181/182 (same day):** dwell re-anchoring + caps-gated
fast sweep; the per-chip caps constant was rejected (per-host dominance).
Device A/B on the .181 8733BU, old vs new binary: detection 10/10 at both
300 and 500 ms dwells, wall +0.8 s = the restored listening time. Two
additions from that session: the BU's **"fast" path still blocks ~277 ms**
(vs 345 full, radio-live ~60-70 ms either way) — devourer's
`fastretune:false` for the die is honest and the caps gate self-disables
correctly (`/api/v1/info` observed); and the A/B's intended 3 Hz idle-craft
arm was **contaminated by a real powered craft** (originator 17, net_id 0,
5805, ~1.3 kHz, −1 dBm — a bench vehicle running its flight stack,
IP-unreachable), so detection was saturated in every arm and the
listening-time claim rests on the harness + unit test, not on detection
deltas. The net_id-7 idle emitter itself was measured honest by a
filtered ear: 6 frames/3 s accepted at net_id 7.

**EU arm measured 2026-08-15 (same day, .232 powered on):** the 8812EU on
the SSC338Q craft is the FASTEST of the fleet — full call ~21 ms, radio
live 8.1 ms p50 — on an embedded host. That answers the chip-vs-host
question for the BU without moving any die: the second embedded host is
fast, so the 8733B's 345 ms call is predominantly the chip (11n HALMAC
init sequence), not the CV610's USB stack. The per-host spread the entry
opened with still stands — the same jaguar silicon reads 42 ms (x86 AU),
21 ms (x86 CU), 8 ms (SSC338Q EU) and ~250 ms (S22 AU) — so the Pass 181
re-anchoring remains the right mechanism: it charges whatever the local
cost is, measured or not. One warm-up cycle was flagged TAINTED by the
widened full-dwell taint window (a stray 5180 frame) — the review-round
guard catching a real event on its first field run.

**Still open.** Android re-measurement with the start-anchored harness
(needs the wrapped-fd path; R4's open Android half).

**Setup.** CV610 (192.168.2.181) transmitting on 5805/20 through the integrated
waybeam-hub `mod_wblink` node, adapter `cv610-8733b` (`0bda:f72b`, bus 1-1.2).
Ground receiver: x86 dev host, RTL8812AU on bus 8-1, `role:"tx"` uplink,
kernel driver unloaded. Offsets commanded with `POST /api/v1/tx/power` on the
craft's `127.0.0.1:8091`; five samples of the ground's own `adapters[0]`
`rssi_best` / `evm` read one second apart at each rung, after a 6 s settle.

**Positive control first, same session, same receiver, same bench geometry.**
The craft at 192.168.2.232 (8822EU) swept over the identical rungs immediately
before, so the question "does this receiver track a working actuator at this
range?" is answered by measurement rather than assumption — the near-field
compression trap flattens RSSI on its own and would otherwise make a flat
result unreadable:

| cmd (qdb) | ground `rssi_best` (5 samples) | mean | craft reports |
|---|---|---|---|
| −24 | −26 −26 −26 −26 −24 | −25.6 | `applied_qdb:-24`, `saturated_low:false` |
| −48 | −32 −32 −30 −30 −32 | −31.2 | `applied_qdb:-48`, `saturated_low:false` |
| −96 | −32 −32 −32 −30 −32 | −31.6 | `applied_qdb:-96`, `saturated_low:**true**` |
| −24 | −24 −24 −24 −24 −24 | −24.0 | back, fully reversible |

**5.6 dB of air for a 6 dB command** on the first rung, then the §10.5 rail
with `saturated_low` set — the receiver reads this actuator cleanly at this
range.

**The 8733BU, immediately after, on the same ground adapter.** EVM is carried
too, because it is the tell that survives receiver compression:

| cmd (qdb) | ground `rssi_best` (5 samples) | mean | `evm` | POST |
|---|---|---|---|---|
| −24 | −30 −28 −26 −28 −28 | −28.0 | −22 ×5 | `{"ok":true}` |
| −48 | −28 −28 −30 −28 −28 | −28.4 | −22 ×5 | `{"ok":true}` |
| −96 | −28 −26 −28 −26 −26 | −26.8 | −22 ×5 | `{"ok":true}` |
| −24 | −28 −28 −28 −28 −26 | −27.6 | −22 ×5 | `{"ok":true}` |

**72 qdb — 18 dB — of commanded range, and the air did not move.** The 1.6 dB
spread between rung means is smaller than the within-rung scatter and is not
monotonic in the command; EVM is bit-identical at −22 across every rung, where
the 2026-08-14 AU measurement in this same file saw EVM travel 14 dB for 6 dB
of real power. Nothing was actuated.

**Why, from source — this is a certainty, not an inference.**
`src/rtl8733b/Rtl8733bDevice.{h,cpp}` overrides **none** of the `IRtlDevice`
runtime-power family: not `SetTxPowerOffsetQdb`, not `GetTxPowerCaps`, not
`SetTxPowerIndexOverride`, not `GetTxPowerState`, not `ReApplyTxPower`. Only
`jaguar1`, `jaguar2`, `jaguar3` and `kestrel` do. So the call lands on
`third_party/devourer/src/IRtlDevice.h:134`, whose whole body is
`(void)qdb; return 0;` — the documented "unsupported" answer. **Zero register
writes occur.** There is no mechanism by which the sweep above could have
moved anything, and the sweep is the confirmation, not the proof.

**What the chip is actually running at.** Not an uncharacterised maximum —
`Rtl8733bDevice::configure_tx_power` puts a TSSI-offset PG unit into
closed-loop TSSI against `kSafeTssiTargetQdbm8733b` (**64 qdBm = 16 dBm**), and
a unit without TSSI calibration onto the flat `kSafeTxAgcIndex8733b`. Both are
deliberately conservative devourer bring-up values. The §10.5 safety argument
is therefore **not** "this craft is airing at full power"; it is "this craft is
pinned at a fixed power that no waybeam-link knob can move, while every
waybeam-link surface says the knob worked."

**What a consumer sees — both fields read fine.** After the sweep:

```
{"override_active":true,"qdb":-24,"applied_qdb":0,
 "saturated_low":false,"saturated_high":false,"backend":"radio"}
```

§10.5 tells consumers the saturation flag is the load-bearing field and
`applied_qdb` is not. Here the flag says "travel remains" and `applied_qdb`
says "0 applied", which is exactly what a *successful* zero-offset apply looks
like. The only discriminator on the whole surface is the one line
`radio: adapter "cv610-8733b" power offset -24 qdb applied as 0 qdb` that Pass
169 added to the log — and a log line is not something a controller reads.
§10.5's own sentence "all three are absent on a backend with no power actuator"
was written for the `udp` backend and does not reach a *radio* backend whose
chip has no actuator.

**Blocked on this adapter, all reporting success:** `adapters[].power_offset_qdb`
(the §10.5 safe boot backoff — the seed of −24 qdb never reaches the chip),
the §11.7 `0x0A` power tier, and any §10.6/§10.7 calibration, which would walk
rungs in offset space and attribute whatever the air did to a knob that is not
connected.

**Upstream, not here.** `third_party/devourer` is vendored at `a0dfd17`, which
**is** upstream `OpenIPC/devourer` HEAD as of today — there is no fix waiting to
be pulled. The actuator is missing in the backend, not in the silicon: the
8733B has a TXAGC block devourer already writes and reads back
(`Phy8733b::set_flat_tx_power` / `read_txagc_state`, refs at BB `0x4308` plus
the `0x3a00..0x3a10` per-rate diffs), and on a TSSI unit an absolute qdBm
target the loop already drives to. `TxPowerCaps` even reserves `index_max = 0`
for exactly that shape ("dBm model"). This is an upstream feature request, and
we do not edit `third_party/`.

**Open — needs an operator ruling (Tier 1, §10.5/§10.3).** What should a node
do when a `role:"tx"` adapter's chip reports no power actuator? "Refuse false
success" is settled practice, so the reporting half is not really in doubt —
`GetTxPowerCaps().supported` is the clean per-adapter discriminator and would
let the three fields be omitted, as §10.5 already specifies for an
actuator-less backend. What needs ruling is the **operational** half: does a
node with an unactuatable TX adapter refuse to start, or start with an
announcement? Refusing would today take the only CV610 link off the air. This
is the second instance of the same question as #180 item 4.

## 2026-08-10 — Kernel-monitor retirement audit: the code is clean, the RECORD is not — a whole bench campaign and one bench-gate verdict rest on the deleted backend

**What was audited.** Every reference to the `kernel-monitor` / MonAir backend
in this repo outside `third_party/`, after the Pass 164 deletion, asking one
question per site: does anything **live** still depend on it?

**Code — clean, confirmed by re-derivation, not by trusting the Pass.**
`io/src/air_mon*`, `scripts/mon-up.sh` and the `deploy/` monitor configs are
gone. `air.kind: "kernel-monitor"` is rejected at `io/src/config.cpp:1203-1208`
with an explanatory message and tested at `tests/config_test.cpp:513`.
`adapters[].ifname` — MonAir's only config key — is registered `never_live` at
`io/src/config_registry.cpp:163` with inert-verdict tests for both the radio
and udp paths (`tests/config_strict_test.cpp:269,274`). **Zero references
exist outside this repo** — hub, builder, sbc-groundstations, venc and Android
never named the backend.

**Find 1 — `radiotap.h`'s RX half is dead code with a live test suite.** The
retirement notes record radiotap.h as still in use, and it is: `radiotap_tx_ht`
and the `kRxMcs*` buckets are live via `radio_decode.h`, `dot11.h`, `stats.h`,
`air_iface.h`. But `radiotap_parse()` and `RadiotapRx` — the **RX** half — have
**no production caller**; the only callers left are `tests/radiotap_test.cpp`.
MonAir was the sole consumer. Devourer takes FCS state from `RxAtrib.crc_err`
and length from `mpdu_len_without_fcs()`, never from radiotap. So a passing
test suite currently guards code no shipping path reaches, and
`docs/verification-hardware.md`'s "monitor-frame FCS rule" documents its
contract as if live. Flagged rather than taken at first, because deleting tests
that pass is a reachability change and this one had no defect driving it.

**RULED (operator, 2026-08-10): delete.** Landed as **PR #170** — the parser,
the struct and the four `test_rx_*` cases go; `radiotap_tx_ht` and the
`kRxMcs*` buckets stay, and the FCS-at-end rule survives as documentation in
`verification-hardware.md` for anyone who rebuilds a radiotap-RX path.
Verified there: `dev` 73/73, and `scripts/gates.sh` 28/0/**0 skipped** with
both cross toolchains, so the reduced header genuinely compiled on
ssc338q{,-au,-eu} and android-arm64.

**Find 2 — the record overstates coverage, and by more than one campaign.**
`docs/followup-plan.md` carried DONE rows whose RF was collected on
kernel-monitor. The scope was **larger than the four rows previously
identified**: reading `review-log-archive-p001-152.md` shows the entire
2026-07-19 sequence — Passes 47 through 52 — ran on kernel-monitor. Passes
47–50 name the ground monitor netdevs `229b`/`2308`; Pass 52 used a different
pair (`:1292` — "8812EU TX→8812CU RX monitor link"); Pass 51 names no netdev.
The row labelled "Stationary N=2 **radio** soak" is included: "radio" there
means real RF, not `air.kind: "radio"`. Rows are now marked
`[HISTORICAL — kernel-monitor]` with what carries over stated per row. Two
that survive intact: Pass 50's 3 ms first-NACK grace measures a **localhost**
UDP/IP round trip, not the air — and the loss it was measured under was a
**deterministic synthetic 150‰ post-radio drop** (`:1124`, `:1221`), not RF
loss, which if anything strengthens the transfer — and Pass 52's controller
gates ran on **UDP**, not RF. One that is void: Pass 52's actuation result —
Pass 164 deleted the `iw`-forked actuator it exercised.

**Pass 51 is the one whose *conclusion* hangs off the backend, not just its
measurement.** It ruled that waybeam-link runs as root and creates
`/venc_frame_out` `0666` because *"the kernel-monitor backend requires raw
packet access"* (`:1241`, `:1256`). That premise is void — devourer uses
libusb, not `AF_PACKET`. Whether the privilege posture should now change is an
open question this audit does not answer.

**Find 3 — §17 bench gate 2 is marked PASSED on evidence from the deleted
backend, and a live seed hangs off it.** The only physical gate-2 measurement
is the Pass 47 walk fade, run entirely on kernel-monitor. `step11-bench.md`
§4.1 records it COMPLETED, and `frame-fec-plan.md:382` checks the **10% FEC
operating point** off against it. The case for transfer is real — gate 2
measures cross-adapter loss correlation ρ, a property of antenna geometry and
the channel, and the backend moves *absolute* loss rather than the
*correlation* between two co-located ears. The case against is precedent: the
monitor-era "devourer cannot transmit MCS4+" premise was refuted on hardware
in Pass 139, so monitor-era conclusions have transferred badly here before.
**RULED TRANSFERABLE (operator, 2026-08-10).** ρ is geometric: the backend
changes absolute loss, not how two co-located ears correlate. Pass 139 was
weighed and set aside — it refuted a devourer *TX capability* claim, not a
property measured identically either way. The verdict and the 10% FEC seed
stand on devourer. Provenance is recorded at `step11-bench.md` §2 and §4.1,
`frame-fec-plan.md:382`, `README.md`'s status block, `mon-air-verification.md`
§"Gate 2" (which `step11-bench.md` §4.1 forwards readers to), and the
`followup-plan.md` register. `devourer-integration-analysis.md:384,394` cite
the walk's numbers as FEC design evidence and need no flag — under the ruling
those numbers carry.

**Find 4 — PROTOCOL.md §11's CSA machinery IS monitor-derived, and one live
guard may now shield against a failure mode that cannot occur. TIER 1 — RULED:
MEASURE.** This entry originally cleared §11 on the reasoning that
`FastRetune`/`SetMonitorChannel` are devourer API names and the craft never
ran a kernel netdev. **The first half is true and the second is false**, and
the pre-merge review caught it. The repo's own record: *"the craft ran
kernel-monitor before Pass 145 and now runs devourer on the same adapter"*
(`review-log-archive-p001-152.md:6818`, which is Pass **146**). The craft's
backend then moved *back*: **Pass 149** (`:7189`) measured its devourer TX path
as **10× worse** than monitor on the same channel/MCS/RSSI — post-diversity
loss 17–19‰ vs 1–2‰ — and *"everything below was therefore re-measured on the
monitor backend"*. So the craft was on kernel-monitor later than Pass 146
suggests, which **strengthens** this find rather than weakening it. The exact
date it settled on devourer for good is not pinned here; Pass 164 is the upper
bound, because after it there was no alternative. What follows:

- **§11.6's craft post-retune RX-liveness guard (Pass 80) is a live spec
  mechanism characterised entirely on the retired path.** The half-retune it
  defends against was found on the craft 8812EU and is attributed explicitly
  to *"the in-place `iw set freq` retune path"*, recovered by *"full monitor
  bring-up"* (`archive:2345-2354`). `iw set freq` **is** the kernel netdev.
  Devourer retunes through `FastRetune`/`SetMonitorChannel` instead, so the
  open question is sharp: **does the half-retune failure mode exist on
  devourer at all?** If it does not, a live guard is defending against
  something that can no longer happen. If it does, it has never been confirmed
  there. Either answer is a spec-relevant fact, and neither is in evidence.
- **§11.2's `dt_to_switch_ms` class-0/1 budgets (150/500 ms) are not
  monitor-derived either — they are unmeasured.** `step11-bench.md` §4.3 says
  they were *"derived on paper against wfb_ng precedent, not measured against
  this radio's actual hardware TSF latch behaviour"*, and `preflight-open-
  issues.md` C3 records that run as **never done**. So the correct statement
  is "unvalidated", not "devourer-measured".
- **§18's "measured monitor-mode retunes (§11.2)"** therefore is not the
  harmless wording ambiguity first recorded here. It may be citing the retired
  backend literally.

**This is the Tier-1 trigger, and the ruling is to measure.** **Operator,
2026-08-10: run the devourer CSA retune trial** rather than rule §11 from the
armchair — queued as an RF leg on issue #134. It answers three things at once:
whether the half-retune failure mode exists on devourer, a real devourer
retune cost to size §11.2 against, and `preflight-open-issues.md` **C3**, open
since 2026-07-24. No spec text is amended until it reports.

**The rest of PROTOCOL.md is clean.** The eleven sites naming the backend
(~201/202/210 as one, ~2145, ~2326, ~2382, ~2472, ~2508, ~2698, ~2789, ~4730,
~5161, ~5383) all state the retirement and what went with it. §9.10's wedge
detector (~2145) was monitor-measured, but its defence — the failure is the
chip's, not the backend's — is now **independently confirmed on devourer** by
the entry below (induced wedge, 5/5 cleared by backend rebuild, 0/5 do-nothing
control, **PR #168**), so that seed is no longer monitor-only evidence. The
equivalent "monitor" wording in `README.md`, which is not spec, was fixed
directly.

**Find 5 — the per-MCS PER ladder's blocker was already lifted, by a route
the plan filed as a fallback. RULED (operator, 2026-08-10): sequence-derived.**
`docs/per-mcs-per-ladder-plan.md` §6 recorded a STOP: neither backend could
deliver bad-FCS frames on the fleet-default chips, so the ladder had no
numerator. Retiring kernel-monitor removes the *symmetry* constraint that made
bad-FCS the choice in the first place — but the real answer is that Pass 163
already closed §9.2's numerator by computation: rate is a pure function of
`seq`, so a missing probe-slot seq's rate is known without any signalling.
That is the plan's own option 3.

**Verified against the shipped code, and the plan is NOT fully closed by it.**
`core/src/mcs_probe.cpp` + `core/include/wblink/mcs_probe.h` implement the
schedule (`probe_slot_hit`), which rides `ProfileTable` as
`probe_period`/`probe_slot` (`core/include/wblink/table.h:48-49`).

**"Rate is a pure function of `seq`" is Pass 163's shorthand, and it is
looser than it sounds.** The probe rate resolves through
`probe_up_candidate_mcs(table, active_profile)` (`mcs_probe.cpp:10-26`) — a
function of `seq` **and** the sender's active profile **and** `table_version`.
That is exactly why the RX window carries four guards rather than trusting the
schedule: successes must be **rate-verified** against the candidate, gap losses
are **epoch-gated** to windows where non-probe frames confirm the TX is flying
the commanded rate, and at least one direct candidate-rate observation is
required or a non-probing TX on the fleet-shared schedule would manufacture a
phantom veto. The numerator is real; it is derived-and-verified, not derived
alone. Two further limits: the probe is **up-candidate only** (Pass 163's second operator ruling
— no down-slot, downshift stays loss-driven), and its evidence is a **veto,
never a warrant**. So it gives PER at *one adjacent rate*, not the 8-rung
PER-versus-RSSI waterfall the plan set out to build. Two other pieces already
ship toward that: `rx_crc_mcs[]` (per-MCS CRC-error counts, rate-attributed
pre-FCS, `io/src/air_radio.cpp:385-391`) and the Pass 158 windowed SNR/EVM
accumulator. **The plan's Parts A and B are superseded and need re-scoping
against those three surfaces; §2's bad-FCS path should not be implemented.**

**Out of scope, ruled at the start:** the wfb_ng residue (waybeam-hub 18
files, sbc-groundstations 15, builder 17, waybeam_venc 7). A separate
retirement, already executed in hub #153 and sbc #16, related to monitor mode
but not to MonAir.

---

## 2026-08-10 — A §9.10 wedge clears by DESTROYING AND RECONSTRUCTING the backend in-process; re-enumeration alone never clears it (0/5 control)

**Pass 148's "in-process re-init is impossible" is true of the thing it tested
and silent about the thing that was never tried.** It rests on devourer's
`InitWrite` unconditionally assigning `_coex_thread` and `std::terminate`-ing on
a second call — a statement about calling it twice **on one live object**. A
freshly constructed device has a default-constructed, non-joinable
`_coex_thread`, so the assignment at
`third_party/devourer/src/jaguar3/RtlJaguar3Device.cpp:900` is legal, and both
`Stop()` (`:684`) and the destructor (`:386-395`) set `_coex_stop` and join
first. Destroy-the-object-and-construct-a-new-one had never been measured.

Harness: `tools/hwtrial_reinit` (new). It uses the **production** detector
(`wblink::TxWedge` is pure and clock-injected, so it polls what `run_tx` polls,
with the craft's own policy) and **induces the fault itself** (Pass 147's usbfs
deauthorize/reauthorize), so no interval below contains an operator's reaction
time.

### The control comes first, because without it the recycle arm proves nothing

The device is reauthorized ~3 s after induction and the §9.10 verdict lands
~0.3 s later, so a recycle arm on its own cannot separate *"the rebuild healed
it"* from *"re-enumeration plus a few seconds healed it"* — both predict the
same timing. `--on-wedge wait` changes nothing and watches the same object.

**Craft 8812EU, 5 episodes: RECOVERED WITHOUT RECYCLE 0/5.**
`tx_reports` froze at **1102** and stayed there for the rest of the run while
`tx_submitted` climbed 1610 → 7710 and `tx_failed` 508 → 6608 — submissions
advancing, zero backend progress, permanently, across ~2.6 minutes and five
re-enumerations. Episode 1 is the clean replicate (induced, reauthorized,
watched 25 s); episodes 2-5 are that same unhealed wedge continuing, because
the arm never rebuilds the object and so cannot produce independent replicates.
**Effective n for "a fresh wedge does not self-heal" is 1**; the other four say
"a 30-160 s old wedge does not self-heal", which is weaker but still evidence.
This reproduces Pass 147's "USB re-enumeration does not heal the dead libusb
handle" on this unit, and it is what makes the next table attributable to the
rebuild.

The two arms also started from a matched state, which is worth more than the
episode count: control ep1 reached its verdict at `submitted=1610 failed=508
reports=1102`, A1 ep1 at `submitted=1605 failed=505 reports=1100` — within
0.3 %.

### Step 0 — healthy teardown/rebuild cycles (the gate)

| unit | cycles | frames | fd | task | RSS |
|---|---|---|---|---|---|
| bench 8812AU, bus 5-1 (Jaguar1) | **20/20** | 4000/4000, `tx_failed 0`, 4000 reports | 4 → 4 | 1 → 1 | 5464 → 5484 kB (**+20 kB**) |
| craft 8812EU, bus 1-1 (**Jaguar3/8822e**) | **5/5** | 500/500, `tx_failed 0` | 4 → 4 | 1 → 1 | 2568 → 2588 kB (**+20 kB**) |

The EU row is the load-bearing replicate: it is the **Jaguar3** path Pass 148
actually cited. `task 1 → 1` after every destructor is direct evidence that
`_coex_thread` is joined.

### Step 1 — clearing a real wedge (craft 8812EU, ch 5805, MCS 0, −72 qdb)

| arm | episodes | t(induce→restored) | mean |
|---|---|---|---|
| **A0** live supervised deployment, `exit 9` → re-exec | 5 | 13.82 / 13.29 / 13.05 / 13.35 / 13.77 s | **13.46 s** |
| **A1** in-process destroy + reconstruct, no unbind | 5 | 9.86 / 10.00 / 9.88 / 9.95 / 9.86 s | **9.91 s** |
| **CONTROL** no recycle | 1 clean + 4 persistence (see below) | never (0/5) | — |
| **A2** with the sysfs unbind | not run — see the open list |

**A1 cleared 5/5**, and the restoration criterion has a **dwell**: first
progress arms a 2 s hold, and the episode only scores when reports are still
advancing at the end of it (`+374..378` reports since the rebuild baseline, of
which ~364-369 land inside the dwell itself; `tx_failed 0`,
`tx_report_fails 0`). An earlier "8 reports, ever" criterion would have scored a
burst-then-re-wedge as recovered. A1's 9.91 s therefore *includes* 2 s of
proving durability; first progress arrives at **7.91 s** mean.

**Do not read 13.46 − 9.91 as the cost of `exec`.** The two arms reach
different milestones and both carry arbitrary constants: A0's respawn path has a
hardcoded `sleep 2` plus `free_adapter`'s `sleep 2`, then process start, config
load and control-server bind, and its poll granularity is 200 ms; A1 carries
`--settle-ms 2000`. What is comparable is the *shape*: a rebuild that reaches
first CCX progress in 7.91 s versus a re-exec that reaches a serving daemon in
13.46 s.

**Control-plane downtime, measured rather than asserted.** In A0, `:8091`
answers until the daemon's own §9.10 verdict at ~3.55 s and is then silent for
**9.24-9.96 s (mean 9.62 s)** — not the whole 13.46 s. In A1 a listening socket
held across the recycle was **STILL ACCEPTING 5/5**.

**The fault-path teardown does not leak.** Step 0 only ever tears down a
*healthy* adapter; the one that matters is the yanked-device teardown, and it
is now sampled per episode: **fd 12 → 12 and task 4 → 4 on all five**, RSS
+100 kB on episode 1, +4 kB on episode 2, then flat.

### Step 2 — the hard-fail fallback still fires

Adapter left deauthorized: the rebuild retried for its bounded window, reported
`reconstruct FAILED for 8000 ms: adapter "bus-1-1" claim/reset failed (in
use?)`, ended NOT CLEARED and exited **non-zero** — the daemon's `exit 9` path.
**37,457 bytes** of log for the episode against Pass 147's ~700 kB bound;
`/tmp` 4680K → 4720K with 40.7 MB free.

### What this supports, and what it does not

In-process recovery **works and is attributable to the rebuild** (the control
settles that), it keeps the control plane bound, and it reaches first progress
sooner than a re-exec. It is **not** a replacement for `exit 9`: Step 2 is why
the fallback must remain, and the recommendation is an in-process attempt
**first**, `exit 9` **after** a bounded number of failures.

### Open

- **Nothing is wired into the flying path.** `run_tx`, `node/` and
  `deploy/vehicle-waybeam-link.init` are untouched; §9.10's exit contract does
  not change here. Adoption means a config knob defaulting off, then one
  Tier-1 amendment once the mechanism settles.
- **H2 is not excluded.** The finding shows the unbind is not *required*; it
  does not show the kernel driver was competing. A control run — no waybeam
  process, deauthorize/reauthorize — had `rtl88x2eu` bound within ~4 s, but A1
  begins its rebuild ~2.3 s after reauthorize, and during A1 the old object
  still held the libusb handle across the fault. Whether devourer's
  `libusb_detach_kernel_driver` (`UsbOpen.cpp:105`, read in source, not
  observed) was exercised is unknown. **A2 was not run.**
- **The §15.5 server itself is untested.** The probe is a bare listening fd
  with no serving thread, no handler holding a reference to the `RadioAir` that
  the recycle destroys, and no request issued during the ~7 s with no radio
  object. It proves the teardown closes no stray descriptor; it does not model
  concurrent access to a destroyed backend, which is the real adoption hazard.
- **One unit per chip family**, one 8812AU and one 8812EU (Pass 139: two of the
  same part number are not a replicate — these are one each).
- **One induction mechanism.** Every wedge here is a usbfs
  deauthorize/reauthorize. A thermal or firmware wedge with the device still
  enumerated may behave differently.
- **The sanitized path is not the path under test.** The ASan/UBSan run was the
  bench **Jaguar1** AU (no AddressSanitizer error, no leak report, only the
  three vendored Jaguar1 items `CLAUDE.md` already calls noise) — and that run
  ended FAIL on the harness's own RSS guard, which is ASan quarantine (~3.64
  MB/cycle), not a leak. The Jaguar3 EU was release-only. **No TSan run
  anywhere**, on a thread-lifetime question.
- **No over-air confirmation.** Restoration is CCX reports — §9.10's own
  progress signal — not a receiving node; the ground was not running
  (`rssi_best` −128 throughout).
- **Four instrument bugs were found and fixed before any number here was
  believed**, every one of which first produced a confident FALSE result: a
  stale timestamp that underflowed `uint64` into an instant "NOT RESTORED"; a
  restoration test keyed on a detector transition a freshly-reset detector can
  never emit (reported 0/2 while its own log showed 5058 reports on the
  reconstructed object); a mutation test that "passed" because the mutant had
  not compiled and the stale binary ran; and the missing control above, without
  which A1 and "re-enumeration heals it" were indistinguishable. The harness's
  own guards are mutation-verified — a deliberate fd leak, a tightened RSS
  slack and a bogus bus path each turn it red.

---

## 2026-08-08 — Legs A4 and A6 closed: §15.2 mac-pin re-bind, and recover() observed to perform no USB reset

Issue #140. Run on the bench rig alongside PR #146, both dongles, kernel
drivers unloaded and restored. Closes everything in #140 that does not need an
Android phone.

### A4 — §15.2 mac-pin re-bind

Pass 154's two-pass claim/re-bind, which #139 modified (it added the `by_fd`
guard to the bus pass). Three cases, each a spectator config so the node
cannot transmit:

| case | config | result |
|---|---|---|
| listing order vs enumeration order | both stanzas mac-pinned, listed AU-then-CU | `pin-au` → 8-1 (AU mac), `pin-cu` → 5-1 (CU mac) |
| mac pin vs bus pin on the same unit | stanza 0 mac-pinned to the CU, stanza 1 bus-pinned to 5-1 (the CU) | `DISPLACED ... (§15.2 precedence)` fires on the bus-pinned stanza, which then takes the AU |
| pinned mac absent | stanza 0 pinned to `02:00:00:00:00:01` | `NOT PRESENT ... at the safe boot offset; identity-bound calibration is withheld (§10.6 D2)` |

The first case is the substance. Enumeration order on this host is 5-1 (CU)
then 8-1 (AU) — established by case 3, where the unmatched stanza fell to the
first free unit, 5-1. So the provisional by-index claim would have given
`pin-au` the CU and `pin-cu` the AU, and the re-bind corrected **both**. That
is the same input the issue's "dongles swapped between ports" produces:
the pinned MAC is not at the position the claim assigned.

**Stated plainly: the dongles were not physically moved.** The condition was
created by making listing order disagree with enumeration order. The re-bind
sees only enumeration order and EFUSE MACs, so a physical swap is the same
input by a slower route — but if anyone wants the literal test, it is still
unrun.

### A6 — §11.6 recover() performs no USB reset

#139 asserted this from reading the code (`recover()` is StopRxLoop /
SetMonitorChannel / StartRxLoop, no reset anywhere). Now observed.

Method matters here, because the first attempt proved nothing: with
`do_reset` at its default `true`, bring-up resets the device and puts
`usb 8-1: reset SuperSpeed USB device` in `dmesg` — which is exactly the line
A6 is looking for, from the wrong cause. The probe therefore sets
**`do_reset=false`**, so any reset in the log must be `recover()`'s.

Measured, with a second process injecting at MCS 0 / −24 qdb so there was
real traffic to lose:

```
before: rx=5346   →  recover() -> true  →  after: rx=12400
RX DELTA ACROSS recover(): 7054 frames
dmesg for the ear's bus (5-1): nothing
```

`recover()` returns true, the RX loop restarts (devourer re-submits its URB
ring), and **7054 frames arrive across and after the recovery** with zero
resets, disconnects or re-enumeration on that adapter. An earlier run without
a transmitter showed the loop restarting but `rx` flat at 0 — correct, and
worthless as evidence, since nothing was being sent. A6 needed traffic to say
anything.

Incidentally re-confirms `do_reset=false` on the *enumerated* path (not just
the wrapped-fd path #139 needed it for): bring-up succeeded and the EFUSE MAC
read correctly without the reset.

### Two bench notes

- The 8812CU's kernel driver is **`88x2cu`**, not `8812eu`. `8812eu` loads
  with zero users and rmmod'ing it does nothing for the CU. `CLAUDE.md`
  already names `88x2cu` — this is a note for anyone who guesses from
  `lsmod` instead.
- After a devourer run ends with the card disabled, `modprobe rtw88_8812au`
  alone did **not** bring the AU's netdev back; `modprobe -r` then `modprobe`
  did. Check `ip -br link` after restoring drivers rather than assuming.

**Open:** B/6 (a real Android `UsbManager` fd) is the only remaining #140 leg
and is genuinely phone-blocked. The composite-dongle question needs an
RTL8822BU, not a phone.

## 2026-08-08 — Leg A5: §15.3 stats from a real node, and the fd separation the log sinks depend on

Run alongside PR #146 (issue #144, B8) to check the injected sinks against a
real node rather than only against `ctest`. Issue #140 leg A5.

Setup: bench host, both dongles, kernel drivers unloaded (`rtw88_8812au`,
`88x2cu` — note the CU is `88x2cu`, not `8812eu`, which loads with zero users
and is the wrong module to rmmod). Config: the spectator template
`deploy/ground-192.168.2.199.json` with `air.kind:"radio"`, both units as
`role:"rx"` on 5805/20, `stats.hz=2`, `stats.stdout=true`. **Spectator, so
§15.2 withholds ARQ/NACK/LINK_REPORT and the node provably cannot transmit** —
this is a non-radiating measurement.

Measured:

- Both units bind and report their EFUSE identities unchanged from every prior
  run — `20:0d:b0:c4:a7:6a` (Jaguar1, `8-1`), `40:a5:ef:2f:23:08` (Jaguar3,
  `5-1`).
- §15.3 emits with a populated `adapters[]` — two entries carrying real
  per-unit counters (`rx`, `filtered`, `kernel_drop`, `rssi_best`,
  `adapter_stalled`, `rx_dead`, `tx_wedged`), every line parsing as JSON.
- Cadence exact: `t_ms` deltas 500/501 ms at `stats.hz=2` over 15 lines.
- **Complete fd separation**: stats on fd 1, every diagnostic on fd 2, zero
  crossover in either direction. This is the property B8's log sink depends
  on — a diagnostic reaching stdout corrupts the NDJSON stream a consumer is
  parsing, and it would do so only under the conditions that produce
  diagnostics, i.e. when something is already wrong.

One measurement artifact worth not rediscovering: bring-up of two devourer
adapters takes ~7 s, so a `timeout 8` run yields **one** stats line and looks
like a broken emitter. It is not — the same config at `timeout 15` gives 15
lines at the correct interval. Size the run past bring-up before concluding
anything about cadence.

**Open:** nothing here blocks. What A5 does *not* cover is a node with a live
peer — `adapters[].rx` is 0 throughout because nothing was transmitting, so
the counters are structurally-correct-but-zero. The schema question in §5 of
`docs/library-extraction-plan.md` (per-backend counter dispatch, `MonAir`
leaving the tree under #120) is unaffected either way and stays for Phase 2a.

## 2026-08-08 — TX confirmed over the air on merged Phase-1 code: 500/500 submitted, 500/500 reported, 499 received

Follows the RX-only run below (#140 legs A1/A3/B1–B5), which deliberately never
transmitted. This is the TX half, on `main` at `e65047b`.

Setup: one host, two dongles, kernel drivers unloaded. **Two processes**,
because a single `RadioAir` drops frames stamped with its own originator and so
can never hear itself — 8812CU at `5-1` as an RX ear (`originator 2`), 8812AU
at `8-1` as the uplink (`originator 1`), both `net_id 7`, ch 5805 MHz. Sweeping
from the safe end per repo law: **MCS 0** and a **−24 qdb power offset**, i.e.
the most robust rate well below the die default.

```
TX:  inject_ok=500  submitted=500  failed=0  reports=500  report_fails=0
RX:  rx_frames=499  filtered=0  dropped=0
```

**Every frame was submitted, every frame got a CCX TX-status report, none
failed, and the ear accepted 499 of 500 (99.8%).** The single loss is ordinary
air loss at a bench gap. So the whole §3.0 encapsulation → radiotap → inject →
air → decap → filter → accept chain is intact on the merged Phase 1a/1b/1a′
code, which until now had only been verified by compiling it.

**Two harness traps found, both worth knowing before writing another one.**

- **A §3.0 payload is not free-form.** `dot11_parse`'s pre-check requires the
  §3.1 magic `0x57 0x42` as the first two payload bytes. A filler burst without
  it put **236 frames in the ear's `rx_filtered` counter and zero in
  `rx_frames`** — which reads exactly like a TX failure and is not. The filter
  was doing its job; that run had already proven frames were crossing the air.
- **CCX reports arrive asynchronously.** Reading `tx_reports` immediately after
  the inject loop gave 353/500; the same run read after a 3 s settle gives
  500/500. A counter read too early looks like a TX-wedge signal (§15.3 Pass
  8's detector is exactly "reports stalling while `tx_submitted` advances").

**Open:** this is one direction, one rate, one power, at bench range, with no
ARQ, no FEC and no video. It confirms the TX path is alive on merged code; it
is not a link-quality measurement and must not be quoted as one.

## 2026-08-08 — Phase 1b on hardware: B11's last leg PASSES, and wrapped-fd identity is source-independent

First hardware run of anything from #109's Phase 1 (issues #140 legs A1, A3,
B2–B5). x86 bench rig, `tools/hwtrial_bringup`, RX-only bring-up — nothing
injected, kernel drivers unloaded and restored around the runs. Units: 8812AU
(Jaguar1) at bus path `8-1`, 8812CU (Jaguar3) at `5-1`.

**B11's unproven leg passes.** Wrapped fd + `do_reset=false` + `InitWrite` +
EFUSE walk — the combination Pass 154 narrowed B11 to and which had never
executed anywhere — returns an identity on both dies, and returns **the same
MAC as the enumerated path**: `20:0d:b0:c4:a7:6a` (Jaguar1) and
`40:a5:ef:2f:23:08` (Jaguar3), byte-identical across the two device sources.
Independently corroborated: after the run the kernel's own netdev for the AU
came back as `wlx200db0c4a76a`. So **§10.6 calibration identity is available
under a wrapped fd**, D3 fail-closed is not needed on that account, and B11
closes. `docs/library-extraction-plan.md` filed this leg under Phase 3 as
phone-blocked; it never was — `libusb_wrap_sys_device` takes any usbfs fd on
Linux.

**The Phase 1b duplicate-device guard is correct in both limbs**, which had
only been argued from source. Two distinct units bring up clean (no false
positive — the failure mode that would have refused a valid two-adapter node).
The same dongle claimed both ways is refused **immediately**:
`adapters "bus-8-1" and "fd-8/4" resolve to the same USB device (8:4)`, instead
of 1.25 s of BUSY retries blaming a process that does not exist.

**Why devourer's advisory lock could not have caught that** is now visible in
the filesystem: the enumerated claim writes `/tmp/devourer-usb-8-1.lock` (port
path) and the wrapped claim writes `devourer-usb-8-a4.lock` (bus+devaddr,
because a wrapped device has no port numbers). Two different keys, same
dongle.

**Mixed sources in one process work** — one enumerated adapter beside one
wrapped fd, both up with correct identities. That is the per-context
`NO_DEVICE_DISCOVERY` claim (Phase 1b) confirmed on hardware rather than from
the libusb source.

**`lock_dir` is honoured**: `--lock-dir /run/wblink-locktest` put the lock file
there and nowhere else.

**fd ownership holds.** The harness `fcntl(F_GETFD)`s its descriptors *after*
`RadioAir` teardown and they are still open — libusb's `fd_keep` contract, and
the reason a caller must close them itself.

**BUSY retry behaves, with one nuance worth writing down.** Against a held
adapter it prints exactly `retrying (1/5)` … `(5/5)` — six attempts, no
`(6/6)` line, then the accurate `claim/reset failed (in use?)`. But the 1.25 s
window does **not** cover a live peer's full bring-up plus teardown (measured
longer than that), so the retry cannot mask a genuinely contending process.
That is the intended property, not a shortfall: it exists to ride out a *dead*
owner's lingering advisory lock and kernel claim across a supervisor re-exec.

**Open:** nothing here exercised TX, ARQ, or a second unit per die (Pass 139).
Legs A4 (mac-pin re-bind with the dongles swapped between ports), A5 (stats
schema from a real node) and A6 (§11.6 recovery) remain, and B/6 — a real
Android `UsbManager` fd — stays blocked on a phone.

## 2026-08-08 — Pass 163 probe window: two known evidence biases (both fail toward "no opinion" or optimism, never a wrong veto)

- **ARQ resend masking (optimistic).** A lost probe-slot first-send whose
  §12 resend arrives before the gap walk settles is marked seen — the
  candidate failure is never counted. Mis-credit is impossible (resends fly
  the committed rate; same-MCS adjacency is disarmed), so the bias only
  under-counts candidate failures on ARQ-repaired streams, weakening the
  veto. Importance-gated video ARQ keeps the volume low. Revisit if flight
  data shows the veto missing real walls.
- **Blackout skip (conservative).** A seq jump ≥ the 1024-bit seen-window
  discards attribution across the gap entirely (nothing during an outage
  confirmed the commanded rate). Long outages therefore contribute no
  evidence — by design.

## 2026-08-08 — bench-gate campaign: stage 0 clean on all three dies; four gates measured; two pinned to geometry

One session, x86 rig (8812AU `20:0d:b0:c4:a7:6a` bus 8-1, 8812CU
`40:a5:ef:2f:23:08` bus 5-1) + craft .232 (8822EU `dc:57:5b:00:d0:57`,
devourer via tmpfs binary, kernel driver rmmod'd for the run and restored).
Channel 5805/HT20 throughout; every process SIGTERM-stopped and both ends
verified silent after. Apparatus: two env knobs in this branch —
`WBLINK_MCS_CYCLE` (TX: DATA radiotap MCS = wire seq % 8, the harshest
per-packet mix) and `WBLINK_MCS_TRACE` (RX: per-frame `seq/rx_mcs/adapter/
rssi/sid` lines) — plus `scratchpad-link/stage0_correlate.py` offline.
Attribution caveat for lossy re-runs: §12 resends reuse the wire seq and
fly the COMMITTED rate (inject_resend is deliberately outside the cycle
knob), so duplicate-seq trace lines are resends, never rate mismatches —
this campaign's runs had ARQ off and 0–2‰ loss, so none occurred.

**#101 stage 0 — PASS on every die present; the premise holds.**
Per-packet commanded rate flies frame-for-frame on all three fleet dies:
AU→CU 3600/3600 rate-verified (0 lost, mismatch matrix EMPTY), CU→AU
3590/3590 (10 lost = 2‰, spread across rates — and their rates are known
by computation, which IS the §9.2 numerator working), EU(craft)→dual ears
au 3593/3593 + cu 3600/3600 (both ears independently agree). CCX
cross-check on every TX: `tx_reports == tx_submitted` exactly (3737,
3741), `tx_report_fails = 0` across ~11k broadcast frames — the Jaguar
retry rate-walk is dormant on the no-ACK path, confirmed on air.
Kernel-monitor leg: MOOT (ruling #120). ~~**Open:** per-unit coverage is one
unit per die — Pass 139's lesson wants a second unit of at least the CU/EU
parts on the rig before probing is enabled fleet-wide (fail-closed default
stands).~~ **CLOSED by operator ruling 2026-08-30 (Pass 196):** the property
measured here is a property of the die and its HAL path, not of a dongle, and
it passed on every die present. The second-unit requirement was Pass 139
caution carried onto a per-die measurement; it is withdrawn and probing is
licensed fleet-wide on these dies. See §9.4 AMENDED (Pass 196).

**#97 LDPC/STBC — proof-of-flight PASS both codings (AU TX → CU ear).**
`air.ldpc`: rx_ldpc 1878/1878 received frames; control (T1, ldpc off)
rx_ldpc 0/3738. `air.stbc`: rx_stbc 1878/1878. No caps refusal on the
Jaguar1 TX die. **Open:** the cliff A/B (PER shift at range) — needs
attenuation the bench can't produce at 30 cm.

**#98/#125 saturation knee — instrument PASS, knee not reached at the
default offset cap.** MCS7 pinned, offset swept −24→0 qdb (safe end
first): peak RSSI −17→−12 tracked the commanded 6 dB, SNR 33–36, EVM
−30..−34 (valid throughout), PER 0‰ at every dwell. *Corrected
2026-08-08:* the original "unreachable in-law" conclusion was wrong —
offset 0 is only the `power_offset_max_qdb` **default**, an
operator-authored key, and the calibration-v2 window spans [−24,+24].
**Open:** config-only rerun first (max raised to +24, sweep 0→+24 from
the safe end, issue #134); physical geometry only if that still doesn't
reach compression.

**#96 unicast A/B — mechanism PASS; retry distribution degenerate at
bench SNR.** A-leg: 236 unicast returns, fallback 0 (SA latched from
first frame), `tx_report_fails` 0 → the retry-8 ceiling never touched;
craft `reports_received` 211/211. B-leg (broadcast): same 211/211.
**Open:** the retry *distribution* only becomes non-trivial on a marginal
link — same geometry limit as #98.

**#99 aim A/B — the AU-uplink rule double-confirmed on this host's own
units.** Ground uplink = c812: release-lateness mean 2261 µs (n=1373,
max 27 ms, ZERO releases under 1 ms), driven by `ReadTsf` mean 1234 µs —
the ±1000 µs window is structurally unreachable. Ground uplink = AU:
`ReadTsf` mean 184 µs (max 441), lateness mean 1462 µs with a healthy
sub-50 µs population (102) and tail bounded at 7.4 ms. The c812 number
matches the 2026-08-07 Jaguar3 finding (2.2 ms class). **Caveat:** the
AU-leg absolute lateness (1462 µs mean) does NOT reconcile with the
2026-08-07 Jaguar1 p99 ≈ 101 µs — pacer parameters were not matched
between runs, so only the relative die comparison is quotable until a
matched-methodology rerun (issue #134).

**#95/#100 scout on-air — Pass 161 machinery verified; out-ranking is
geometry-limited; craft-home non-inflation CONFIRMED.** Leg 1 (CU flood
400 pps on 5785, net_id 1 = undecodable): at 30 cm the flood leaks FA
into EVERY bin (util 833–890 band-wide) and the ranking correctly refuses
with `BROAD_DEGRADATION` — the swamped near-field genuinely is not
channel-attributable. The discriminator that survives: **burstiness** —
5785 reads q90−q50 = 72 vs ≤5 on every other bin; the second axis sees
the interferer when the first saturates. Leg 2 (decodable net-0 craft on
5805 at 120 pps): 5805 `wifi_util` 165 with the **lowest** interference
index of all bins (363 vs 620–715) — decodable home traffic lands in the
wifi axis and does NOT inflate the FA index. Implication for the #95
out-ranking gate: an in-band decodable interferer *depresses* its own
channel's FA index (valid PHY detections are not false alarms), so
out-ranking must be judged on **total util** (both axes), never on the
FA/interference axis alone. #100 mechanics on air: rounds folded (3–4),
domain = the scout's EFUSE MAC, rejects gauges all zero, confidence
seeded correctly. **Open:** true out-ranking (loaded bin worse than
quiet bins from the same ear) needs physical separation — judged on
total util per the above.

**Pass 162 RX-only bring-up (B2 follow-up) — PASS on hardware.** CU
brought up RX-only (full Jaguar3 init + IQK), EFUSE MAC read, 8 s stats
with `tx_submitted` pinned 0 (heartbeat guard live), then ingested 3600
frames as the T1 ear — the success-path contract holds. Cosmetic: boot
restore prints `uplink: artifact STALE (stored mac/..., live udp)` on an
uplink-free node — "no uplink" would read better; harmless.

**Defect found and fixed by the campaign** (commit in this branch):
RadioAir teardown use-after-free — `~Impl` closed the libusb handle
before the devourer device destructor ran its `rtw_hal_deinit` power-down
writes; ASan flagged it on every radio teardown. `dev.reset()` now
precedes `libusb_close`.

## 2026-08-07 — first frame-free occupancy sweep: the two axes are demonstrably independent on ambient air

**Setup.** x86 devourer ground (8812AU scout), Pass 155 build, 7-channel
allowlist sweep at 300 ms dwells, craft link stopped (zero waybeam traffic
anywhere).

**Measured.** `wifi_util_permille` 0 on all seven bins (correct — nothing
decodable of ours on air) while `interference_util_permille` independently
ranked them: 5180 = 638, 5220 = 590, 5825 = 468, 5805 = 311, 5745 = 249,
5765 = 176, 5785 = 175. 5180/5220 are where this bench's household APs
live. `noise_dbm` filled only on 5180 (−81, passive floor — the one bin
with decodable foreign frames); null elsewhere, no fake zeros.

**Means.** The pre-155 ranking would have scored all seven bins identically
pristine (wifi_util 0 everywhere); the interference-inclusive ranking picks
5785/5765 over the AP-occupied bins. Ambient "quiet" UNII-3 bins read
~175–300 on the index — the fa-half seed (200 FA/s) puts the ambient FA
floor mid-scale, which is fine for ranking (monotone within the adapter)
but is a reminder the index is not a duty cycle.

**Open.** The #95 operator bench gate (a *known controlled* interferer
out-ranking quiet bins; craft video on its home channel not inflating its
own bin) — needs a hand on the signal generator. Whether the fa-half seed
wants re-derivation per §17 once #100's rank normalisation lands.

## 2026-08-07 — CU RF re-baseline after the #384 re-vendor (rfe_type 0→3): placements within flat-field noise, wall pattern unchanged

**Setup.** Same rig as the entry below (x86 devourer ground: 8812AU TX
`mac/20:0d:b0:c4:a7:6a`, 8812CU diversity ear `40:a5:ef:2f:23:08`; craft .232
8822EU `dc:57:5b:00:d0:57`, 5805/HT20, dwells 500/1000). Ground running the
Pass-154 branch with devourer re-vendored to `5a5dd62` — the first run on
this rig where the 8812CU's PHY tables load with EFUSE `rfe_type` 3 (the
#384 walk fix; the measured delta vs the old column is 7 RF register values,
see `docs/devourer-revendor-review.md`). Craft on the pre-bump deployed
binary (8822E tables are untouched by #384).

**Measured.** Bi-directional `start_both` completed (after a §11.5a claim —
the first attempt without one failed `downlink_no_ack` with the craft never
starting, which is the binding working, not a defect). Downlink placements
`[0, 0, 0, +14, +10, +2, −6, +4]` (all 0‰), brackets booked on rungs 3–7
(first_bad_rssi −33/−36/−36/−41/−38, placement RSSI −31..−40); uplink MCS0
flat to +24 (no bracket), capped placement 0 @ 4‰, RSSI −53. Morning
pre-bump baseline (below): downlink `[−8, 0, 0, +8, +8, 0, −8, 0]` with 5
rungs bracketed, uplink capped −8 @ 2‰ (RSSI −48).

**Means.** Same shape either side of the bump: flat clean low rungs, walls
on the top half, uplink wall outside the window. Placement deltas are
one-to-two seek steps inside a 0‰ flat field — the entry below already
concluded such differences are noise, and geometry/power state moved between
sessions too (uplink RSSI −53 vs −48). No gross CU RF regression: the
diversity ear delivers, the link held 1‰ at HOLD pre-run, calibration
completes. **The rig's baseline is now these post-bump numbers.** Nothing
measured pre-bump survives as an artifact either way: the Pass 154 identity
change re-keys every stored artifact (`id/radio/…` reads STALE), so no
pre-bump RF state can silently apply.

**Open.** A same-session A/B (old vs new devourer on the CU, rig unmoved)
was not run — placements are noise-bounded at this range, so only a
purpose-built RX-sensitivity A/B would resolve the CU delta finer. The
8822EU per-unit 64-QAM early-wall note (below) stands.

## 2026-08-07 — flat-field verify selection is noise; widening the offset window recovers real walls

**Setup.** x86 devourer ground (8812AU TX `ground-au-1`, 8812CU diversity) +
.232 craft (8822EU `craft-eu-1`), 10 m, 5805/HT20, calibration v2 dwells
(500/1000 frames).

**Measured.** With the original [−24, 0] offset window the whole field is
flat (1–10‰ everywhere, no bracket bookable), so the §10.7 verify walk's
"best" is noise-selected: morning runs placed (−8 @ 1‰, then 0 @ 6‰),
midday runs refused `no_wall_found` **six consecutive times** (verify at the
ceiling kept reading 1–3‰ vs 4‰ one step down) — the outcome tracked slow
RF drift, not the link. After the same-day rulings (offset-space exemption +
window widened to [−24, +24] with the unbracketed-placement cap) the same
bench books **real walls on 5 of 8 downlink rungs** (fp=133 placements
`[-8, 0, 0, +8, +8, 0, -8, 0]`, brackets at first_bad_rssi −66/−41): the
walls were simply above the old window's ceiling. Uplink at MCS0 stays
wall-less even at +24 (RSSI −48, 4‰) — capped placement −8 @ 2‰.

**Means.** Within a flat region, placement differences of one seek step are
not reproducible measurements; only a booked bracket makes a placement a
property of the channel. The window should be wide enough to contain the
wall, and the reference cap handles the case where it is not.

**Open.** Uplink MCS0 wall not yet within [−24, +24] at 10 m — either a
longer placement or a higher-MCS uplink rung would book it. The craft's
rung-6 (64-QAM) early wall (first_bad −41) matches the known per-unit
8822EU 64-QAM TX weakness; unify with that finding when the unit is
re-characterised.

## 2026-08-07 — ground binary wedges on SIGTERM after an in-process calibration run

Twice this session the x86-ground process ignored SIGTERM (stop script +
direct kill; REST already dead, process alive until SIGKILL) — both times
after it had completed at least one §10.7 run in-process; a fresh instance
stops cleanly. Suspect a teardown path wedged in devourer USB close while
calibration-era actuator state is present. Bench impact only (SIGKILL is
acceptable on x86, never on SigmaStar). Open: reproduce under gdb / with
devourer verbose teardown logging; check whether the §10.7 restore path
leaves an actuator thread parked.

## 2026-08-07 — §7.2 aim error budget: instruments landed, numbers owed (issue #99)

**What exists now (bench knob, no spec surface):** `WBLINK_AIM_LOG=1`
histograms two of issue #99's three error terms — (a) *release lateness*,
how late past the computed `QuietGap::return_deadline` the host loop
actually fired the return window, and (b) the `ReadTsf()` control-transfer
cost, the §7.2 term with **no measured number at all** (devourer bounds it
0.5–1.2 ms on Jaguar3; a bulk-flooded adapter additionally starves the
read). Dumped to stderr every 30 s as bucketed distributions
(<50/<100/<200/<500/<1k/<2k/<5k/≥5k µs) — the tail is the contract, means
hide it. **rx role only**: the dump lives in the ground loop; on a tx node
the flag collects and never prints (the gate-4 campaign is a ground-side
evaluation — extend the dump if a craft-side number is ever wanted). The third term (craft-side arrival phase relative to its own gap)
is **deliberately not instrumented yet**: it needs either a host↔TSF fit
(devourer tdma example) or a host-time proxy whose error is exactly the
terms under study — that placement choice is part of the §17 gate-4
evaluation itself.

**What the vendored bench already says (act on it now):** submit→air p99 is
**101 µs on Jaguar1 (8812AU, async USB2)** vs **2.2 ms on Jaguar3 (8822CU,
sync USB3)** against a ±1000 µs window budget — on a Jaguar3 uplink, that
one term alone blows the budget ~1 % of the time, and the failure is
*correlated* return loss inside a window (defeating Pass 78's redundancy,
which assumes independence). **The ground's `role:"tx"` adapter should be
the 8812AU** wherever the rig has a choice; the x86 bench rig already
complies (AU = `au-uplink`), now as a rule rather than an accident (also
noted in `deploy/README.md`).

**Open:** the gate-4 campaign — run the instruments per uplink generation
(AU vs CU), measure end-to-end aim as a distribution, report the miss rate
against `[eob+guard, eob+guard+window]`, then recommend re-derived
`guard_us`/`return_window_us` seeds or a documented miss budget.
`disable_cca` is NOT a lever (Pass 139: clearing it costs ~45 % of the
uplink). TDMA stays deferred per the issue's own assessment.

## ~~2026-08-07 — §10.7 walls referenced to a measured at-rest floor~~ CLOSED by Pass 153

The floor mechanism (and its `uplink_floor_min_samples` knob) is deleted:
calibration v2 pauses the craft's video for the run, so the contention floor
the walls were being referenced against is structurally zero and the walls are
absolute again. See `review-log.md` Pass 153.

## ~~2026-08-06 — §10.7 report-loss is an under-powered observable~~ CLOSED by Pass 153

Resolved in the direction the entry proposed: §10.7 (and §10.6) measure with
dedicated MTU-padded §3.16 PROBE bursts and per-dwell TALLYs — probe density
is no longer capped by the 10 Hz report cadence, so the n≈1500-per-dwell
sample the estimator arithmetic demanded is cheap. The at-rest σ evidence
(21 windows of n≈150: sd 23.3‰ vs binomial 22.1‰) lives on in the archived
Pass 152 addendum and the Pass 153 entry. See `review-log.md` Pass 153.

## ~~2026-08-06 — §10.7 spec/code drift: `uplink_verify_epochs` 400 vs 200~~ CLOSED by Pass 153

Dissolved: the key is retired; the v2 dwell knobs are `dwell_probe_frames`
(500) / `dwell_verify_frames` (1000). See `review-log.md` Pass 153.

## 2026-08-09 — a tier below the sweep floor collapses an offset-space calibration and reports success

Bench: `.242` ground (8812AU, bus `8-1`) ↔ `.232` craft, 5805→5765 after claim,
both ends on the Pass 166 branch. Fleet ladder
`power_offset_presets_qdb: [-72,-48,-24,0,24]`, `power_offset_qdb: -24`,
`power_offset_max_qdb: 24`.

With §11.7 `0x0A` tier 1 in force (`ceiling_qdb: -48`), `POST /api/v1/calibration
{"action":"start"}` on the ground uplink:

```
state done   phase idle   fail_reason null   probes 1500
placements: [{"mcs":0,"placement_qdb":-24,"last_clean_qdb":-24,
              "first_bad_qdb":null,"placement_loss_milli":7}]
```

One point. The previous artifact — `fingerprint 94`, `last_clean_qdb: 24`,
i.e. a sweep that had climbed to the configured bound cleanly — was
**overwritten**. Restored by hand afterwards; the degraded copy is kept as
evidence.

Mechanism: the ground's *startup* window fold is branched by space and takes
`[power_offset_qdb, power_offset_max_qdb]`, but the §15.5 tier handler's
*runtime* fold was unconditional — `seek.max_qdb = min(cp_max_qdb,
ceiling_qdb)` — so an offset ceiling of −48 landed under the −24 floor. The
craft half refuses this case (`offset_window()` returns nullopt and §11.7
CALIBRATE gates on it); the ground half had no equivalent, so a degenerate
sweep looked like a successful one.

Ruled Tier-1 the same day (Pass 167): in offset space a tier does not narrow
the calibration window at all. That removes the mechanism rather than adding a
refusal to it — but the asymmetry is worth remembering, because the ground
still has no "refuse an empty window" guard of its own and a config with
`power_offset_max_qdb == power_offset_qdb` falls to the ABSOLUTE startup arm
(`app/main.cpp`, the `else if (upwr.ceiling_qdb)` after the relative window
fold). Since Pass 166 the number folded there is an **offset**, not the 108
`max_power_qdb` it used to be, so that arm mixes spaces and can reproduce the
same one-point run from config alone. A future pass picking this up should
start from that description, not from the pre-Pass-166 one.

## 2026-08-09 — the TX half of `node/` cannot ship in a receive-only archive (#109 Phase 3)

Measured while adding the `wblink_tx_*` C ABI, not reasoned from the code.

`examples/node-linkcheck` builds `wblink::node` with `WBLINK_FRAME_SHM`,
`WBLINK_CONTROL_SERVER` and `WBLINK_VENC` **off** — the phone's configuration —
and was green on the branch that had just lifted `run_tx` into
`node/src/tx_node.cpp`. Adding one C caller of `wblink_tx_run` turned it red
with **~22 undefined references**: `FrameShmRing::attach/read_frame/stats/…`,
`ControlServer::create/service/publish_stats`, `VencActuator::set_fps/
set_bitrate/request_idr/…`.

Nothing had broken. `run_tx` uses all three subsystems unconditionally — the
same three `WBLINK_BUILD_APP` already refuses to build without — and a static
archive extracts a member only when something references it. Until the C ABI
existed, nothing referenced `run_tx` in that configuration, so the member was
never extracted and the gate never looked inside it.

That is the failure shape `node-linkcheck` was written to end (its CMakeLists
records the nine references B10 removed), reappearing one level deeper: the
gate proves the archive *links*, which is not the same as proving the archive
*resolves*. A link gate can only see the members its consumer pulls in.

Fix: `node/src/tx_node.cpp` and `node/src/tx_node_c.cpp` compile into
`wblink_node` only when all three subsystems are ON. A receive-only consumer
(Android `:wifi`, bionic, no `shm_open`) gets an archive that resolves; a
transmitter configures what a transmitter needs. The `node-linkcheck` project
now asserts against the target's SOURCES property rather than inferring it
from the options, and deleting the guard upstream makes it fail at configure
time (verified by mutation).

Two things this does NOT claim. It is not a bug in the lift — the references
were latent, never live, and no shipped build was affected. And it is not
proof that no other member of `wblink_node` carries the same latency: the check
is specific to the TX sources, and the general property still has no gate.

## 2026-08-09 — first coordinated RADIO decode through the in-process node; and the loss is not power

**The decode.** waybeam-hub `ground_x86` with `WBLINK=1`, `pixelpilot.frame_shm.source=wblink`,
running `wblink_rx_run()` in-process against a real craft (`.232`, 8812EU) over
RF at 5805 MHz. Ground was a **single 8812AU on bus 5-1 acting as the TX/RX
combo** (operator rule 2026-08-09: do not pair a second adapter for the ground
role; `RadioAir`'s `role:"tx"` adapter is duplex and receives too).

Sustained **27829 frames, 1920x1080 @ ~60 fps, ~10.2 Mbps**, with
`shm_gate_bypasses 0` (the gate opened on a real IDR, not a bypass),
`shm_reattach_count 0`, no pipeline rebuild, and no GStreamer error. Operator
confirmed the picture on screen. This is the first end-to-end proof over a
radio rather than the localhost `udp` air backend.

**The loss, and what it is NOT.** The link sat at profile 2 with
`transition_reason LOSS_PERSISTENT` and a ground-side `loss_ewma_milli` in the
20-60 range. The obvious suspect at bench range was receiver overload: the
ground read **RSSI -6..-8 dBm** while every rung of the craft's calibration
artifact was measured with `last_clean_rssi` between **-22 and -35** — i.e. we
were operating 14-27 dB hotter than anything the curve covers, which is the
regime where PA compression normally shows up.

It is not that. An ordered sweep (-72/-48/-24/0 qdb, one 18 s dwell each)
suggested -48 was best (25 vs 49 milli), but an **alternating A/B** of 0 vs -48,
three passes, 20 s dwell, reversed it:

| pass | 0 qdb | -48 qdb |
|---|---|---|
| 1 | 6 | 44 |
| 2 | 22 | 50 |
| 3 | 63 | 62 |

Cutting 14 dB never helped. RSSI -6 vs -20 against noise -38 vs -52 leaves
**SNR ~32 dB either way**, so the link is not SNR-limited and power is the wrong
knob. The single ordered sweep was noise; only the alternation showed it.

**What the numbers actually say.** On the craft:
`lockout_latched true`, `lockout_profile 3`, `lockout_strikes 4`,
`lockout_active_mask 8` — **profile 3 was tried, failed four times and is
latched out**, which is what pins `lockout_ceiling_profile` to 2. Separately
`promote_blocked_saturated 18339`. The calibration itself is clean:
`calib_stale false`, fingerprint 53, and all eight rungs report
`placement_loss_milli 0`. So "the calibration is bad" is not supported —
a runtime lockout plus saturation is.

**Unexplained, and the reason this is a finding and not a ruling.** Loss rose
monotonically across the ~8-minute A/B *regardless of the setting* — 6 -> 22 ->
63 at 0 qdb and 44 -> 50 -> 62 at -48. Some time-dependent factor dominates both
arms and no measurement here isolates it. Ruled out on the spot: the ground
host's own WiFi (`wlp2s0` is on 5180 MHz, 625 MHz away). Not yet excluded:
channel occupancy at 5805, craft thermal, venc rate behaviour under a held
profile. **Chase this before trusting any loss number from this bench**, and
note that an ordered sweep will lie about it — alternate.

## 2026-08-12 — Scout is a craft finder; its dwell was priced for occupancy

**Tier 2.** Dwell counts, per `CLAUDE.md`. No spec text, no Pass.

#173 established that ScoutEngine cannot report generic RF occupancy at all:
FA/CCA are event counters with no duration semantics, and the one
duration-capable primitive (phydm CLM) is unimplemented in the vendored
devourer, which is off-limits. Occupancy is therefore not a goal the sweep can
serve — which removes the reason the sweep was slow.

**What the base dwell was buying.** `finalize_current` divides accumulated
decoded airtime by the *elapsed* dwell to get `wifi_util_permille`. Leaving a
channel early shortens that denominator and inflates the result, so `tick()`
held the full base dwell on every channel and only broke out early on a channel
whose dwell had *already* been extended. Every empty channel paid a full dwell
to protect a denominator feeding a number that is published `duty_cycle_known:
false` and rendered nowhere.

**Measured, Android + RTL8812CU (Jaguar3), craft 17 @ 5805 MHz:**

| | before | after (projected) |
|---|---|---|
| base dwell | 1000 ms | 250 ms |
| `kExtendMs` | 1200 | 1500 |
| full 38-ch sweep | ~50 s measured (38 s floor, 83.6 s ceiling) | ~12-14 s projected |

**The change.** The base dwell becomes a short presence probe, and *anything
heard* extends the dwell once — where the old form extended only when no
candidate had resolved yet. An empty channel costs one base dwell; a channel
with waybeam traffic gets base + `kExtendMs` to cover the announce cadence.
The 250 ms base is safe *because* of that extension, not in spite of it: video
is high-rate, so presence trips `frames > 0` long before an ANNOUNCE arrives.

**A rejected design, recorded because it nearly shipped.** The first cut ended
the dwell on the first *resolved candidate*, which is faster still. It is also
wrong: `accum_.candidates` is non-empty at the FIRST announce, so a second
craft sharing that channel — announcing independently, up to a second later —
is silently dropped. It cost ~3 s of a ~12 s sweep and paid in missed craft,
which is the worst failure available to a craft finder and one that leaves no
trace, because the sweep still completes and still finds *a* craft. Note the
old 1000 ms base dwell truncated the same way; always-extending is strictly
better co-channel coverage than the behaviour being replaced, not merely equal
to it. `test_a_second_craft_on_one_channel_is_not_truncated_away` pins it.

**What did NOT change, deliberately.** The 30 ms sense barrier stays. It reads
as an occupancy device, but `on_frame` gates on `barrier_done_`, so it is also
the retune-leak guard that stops a settling frame being attributed to the
channel just entered. That attribution is the finder's entire job, and #173
records a still-open CU/Jaguar3 retune misattribution defect — removing the
barrier for ~150 ms across a 14 s sweep would have traded the core function for
1% of the runtime. The per-channel sense *read* was also kept for the same
reason it is cheap: two register reads against a dwell budget it cannot
meaningfully dent.

**Known cost.** The airtime denominator is no longer uniform across a sweep:
an empty channel is measured over ~250 ms and a channel with traffic over
~1750 ms. `wifi_util_permille` was never comparable across channels in a
strong sense, but it is now visibly less so, and a short quiet window makes a
single stray frame read as a larger fraction of it. This is acceptable only
because the number is published `duty_cycle_known:false` and rendered nowhere.
If a future CLM primitive (#173) makes occupancy real, this pacing must be
revisited first — a duration-based measurement needs a fixed window back.

## 2026-08-13 — the radio is deaf for ~250 ms after every retune

**Tier 2.** Measured, not derived. Changes no code in this repo; it prices the
`dwell_ms` a caller may safely ask for.

Hardware trial of the craft-finder pacing (previous entry) on Waybeam-android,
Samsung S22 + **RTL8812AU (Jaguar1)**, craft 17 transmitting video on
5805 MHz (~1000 decoded frames/s on its channel).

**The sweep worked and then intermittently did not.** Three consecutive sweeps
found the craft; later sweeps missed it entirely, reporting
`candidates=[none]`. Detection came out **14/18** at a 250 ms base dwell.

**Mechanism, from the per-retune log timestamps.** devourer logs every channel
set, so the dwell actually spent on the craft's channel is directly
observable:

| sweep | result | ch161 dwell |
|---|---|---|
| hit ×4 | `17@5805` | 1.74-1.75 s (extension fired) |
| miss ×2 | `[none]` | **0.255 / 0.267 s** (extension never fired) |

On the misses the scout heard **nothing at all** on a channel carrying ~1000
frames/s, so `frames > 0` never tripped and the extension never armed. The
craft was not idle and was not weak (-12 dBm).

Channel-to-channel gaps track `dwell_ms` almost exactly (250 ms dwell -> 255 ms
gap), so the `retune()` call itself costs only ~5 ms. **The dead time is inside
the dwell**: the adapter delivers no frames for roughly **220-270 ms** after
retuning, presumably AGC/BB reconvergence plus USB URB pipeline refill. The
first half of any short dwell is silent regardless of what is on the channel.

**Detection vs base dwell**, same craft, same session, 10 sweeps per point:

| base dwell | detection | sweep |
|---|---|---|
| 250 ms | 14/18 | 12.9 s |
| 300 ms | 10/10 | ~15.5 s |
| 400 ms | 10/10 | ~17.8 s |
| 500 ms | 10/10 | 21.4 s |
| 1000 ms (pre-change control) | 3/3 | 38.5 s |

The knee is sharp and sits between 250 and 300 ms — i.e. exactly where the
listening window after the silent period goes to zero. Android ships **500 ms**
(~2x the knee) rather than 300 ms, because the knee is adjacent and only one
chip family was measured.

**`kSenseSettleMs` is unrelated and must not be "fixed" for this.** Its 30 ms
models the retune-leak barrier for frame *attribution*, not the radio's
time-to-first-frame. Raising it would shorten listening and make this worse.

**Open, and the reason the trial did not settle it.** An *idle* craft
(ANNOUNCE 2 Hz + HEARTBEAT 1 Hz, no video) emits roughly one frame per 333 ms.
After ~250 ms of silence a 500 ms dwell leaves ~250 ms of listening, so
P(hear >=1) is only ~50% per sweep, against ~90% for the old 1000 ms dwell.
**A powered craft that is not streaming is therefore less reliably found than
before, and pressing Scan twice is the current answer.** Not measured — the
craft could not be made announce-only without stopping venc on the SigmaStar,
which risks the known mi_* close-deadlock reboot. Measure it before quoting an
idle-craft number.

Untested: Jaguar3/CU at these dwells. Two adapters of one part number are not a
replicate, and two chip families certainly are not.

## 2026-08-13 — a ground's own discovery polling deleted its claim key

**Symptom.** A craft resolved by a scout sweep was unclaimable seconds later:
`do_claim` refused with "no CSA key for craft". It worked only for a craft
sitting on the ground's *resting* channel, which read as "the §11.4a token is
only cached from resting-channel discovery".

**That reading is wrong, and it cost two bench rounds and one unnecessary code
change.** `discovery.observe()` is on the sweep path (`rx_node.cpp`, next to
`scout.on_frame`), so a dwell **does** cache the token. The token was stored in
`DiscoveryCatalog::nodes_` — the *presence* view — and `json()` ages that at
5 s. Taking a discovery snapshot is what ages it, so the ground's own UI poll
was deleting the key its claim needed. On the resting channel the craft
re-announces at 2 Hz and the entry never ages out; that asymmetry is the whole
illusion.

**Fix.** The token cache is its own map keyed by originator, not aged, bounded
at 64 (`node/include/wblink/node/discovery.h`). A token is key material, not
evidence of presence — the claim path has its own liveness guards
(stale-session, then the §11.6 `CSA_ARMED` confirm that rolls a failed campaign
back cleanly). Reproduction:
`tests/node_discovery_test.cpp::test_an_announced_token_survives_the_presence_view_aging_out`,
which fails against the pre-fix catalog.

**Two wrong theories, recorded because each was plausible and each was
disproved by measurement rather than by reading:**

1. *"Occupancy ties at zero, so `emptiest()` degenerates to first-in-list."*
   Disproved on an S22: with `channel_allowlist` reordered to lead with UNII-3
   and the new order verified on the device, `target_chan 0` still chose 5180 —
   twice. Occupancy **is** ranked; it ranked a live AP channel emptier than four
   quiet UNII-3 channels. That is issue #173 demonstrated on hardware.
2. *"The token can only be heard on the craft's channel, so the claim must
   retune before keying."* This became an operator ruling and shipped. The
   retune-first build **still refused on hardware** — `token_for` is synchronous
   and cannot wait for a 2 Hz announce — which is what finally localised the
   cause to aging. The ordering has been reverted now that the cache is fixed:
   keying is a local lookup with no radio effect, so doing it first keeps a
   key-less claim from paying a retune out and back. The §15.5a air-visible
   order is unchanged — the CSA is still issued after the retune.

**Method note.** The claim path's refusals went to `fprintf(stderr)`, invisible
to an in-process consumer, so a refused claim and a transmitted-but-ignored one
looked identical from the app. Routing the load-bearing one through `wb_logf`
is what made round three diagnosable.

## 2026-08-13 — at bench range, "heard-most frames" cannot pick a channel

**Measured on an S22 + RTL8812AU against craft 17 (~2 m, 5805 MHz), four
consecutive sweeps.** The scout resolved the craft's channel as **5745** on
one sweep and **5805** on the other three. Per-channel sightings from the same
sweep:

```
{"originator":17,"chan":5745,"frames":2299,"resolved":false}
{"originator":17,"chan":5805,"frames":2350,"resolved":true}
```

**2299 vs 2350 — 2%.** The craft is 60 MHz away from 5745 and still decodes
there: measured **-41 dBm** on 5745 against **-11..-18 dBm** on 5805, i.e.
~23-30 dB of adjacent-channel rejection, which at 2 m is nowhere near enough
to stop decoding.

**Amended 2026-08-14 — the mechanism above was wrong.** This entry first read
"both dwells run for the same time against a continuously transmitting craft,
so the frame count saturates on dwell duration rather than on signal". The
measurements are unchanged; the reading of them is not. Per
`third_party/devourer/docs/bench-testing-near-field.md`, the RTL88xx front end
**starts compressing around -10 to -20 dBm** and is linear around -40 to -70.
So -11 dBm on 5805 is not "a strong signal"; it is **a receiver in
compression**, while -41 dBm on 5745 sits in the linear sweet spot — the
adjacent-channel filter was acting as the 30 dB attenuator the bench did not
have. The true channel's frame count was not tied, it was **suppressed**, by
the EVM collapse that overload produces while SNR holds flat (measured in that
doc's power sweep: SNR 18 dB unchanged from low to full power, EVM -28 -> -13).

The two readings predict different things, and a second case discriminates
them. **Craft at 5180, ground resting at 5805 (far):** the scout answered
**5240 on four consecutive sweeps**, rejecting 5180 each time; a fifth sweep
resolved 5180 and the claim landed at once. Counts-saturate-on-dwell-time
predicts near-ties with a random winner (case 1 fits). Only
true-channel-decodes-worse-than-its-own-leakage predicts a **systematic** loss.

We already found this effect on the link path and compensated for it there:
`io/include/wblink/air_radio.h` Pass 158 — *"PEAK RSSI — saturation trashes a
fraction of frames to low apparent power, so a mean inverts the signal"*. The
scout is already peak-shaped (`best_rssi_by_orig` keeps the max), so its RSSI
is sound even under compression; only the frame count is not.

**Not settled:** case 2's candidate rows appear at **+60 and +120 MHz** but not
at +20/+40, which no filter skirt produces. An earlier draft of this entry
guessed that compression explains it by collapsing adjacent-channel rejection;
the 2026-08-14 power control below **failed to reproduce any bleed at all**, so
that guess is withdrawn and the shape is unexplained. Re-read the raw sweep JSON
before treating either the skirt or its span as measured.

**Consequences.** `channel_evidence_valid()` is derived FROM the resolver — a
channel is valid only if every candidate on it resolves to it — so the loser
is retroactively marked invalid and its occupancy sample is dropped from the
ranking store. A ground that resolves the wrong channel then retunes there to
claim, and the campaign goes nowhere (cleanly: no `CSA_ARMED`, rollback).

**RSSI separates these cases cleanly and the resolver does not use it**
(-41 vs -11 is unambiguous where 2299 vs 2350 is noise). Not changed here:
the ranking rule is Tier-1-adjacent and this is one chip family on one bench,
so it wants its own measurement before a rule change. Recorded so the next
bench does not read it as a claim bug.

**What the sensing surface can and cannot contribute** (checked against the
vendored `third_party/devourer/docs/rx-spectrum-sensing.md`, not from memory):
the resolver needs **no new devourer call** — `Candidate::rssi_dbm` already
holds the strongest per-originator, per-channel reading from the scout adapter
alone. Ruled out for this defect: normalising to a per-dwell absolute noise
floor (`abs_noise_floor_dbm` is measured **once at bring-up** on Jaguar1 — a
live read wedges RX — and is never available on Jaguar3, which is both of our
scout chips); and narrowband 5 MHz sensing (Jaguar3/8821C only, and it cannot
decode an ANNOUNCE, which is what a candidate *is*). The NHM histogram belongs
to #173, not here. Per-candidate EVM would need plumbing — `AirRxMeta` carries
`rssi` only; EVM exists in the per-adapter `RxQualityWindow`.

**Bench hygiene — lower the power, do not (only) add distance.** That doc puts
free-space loss at ~28 dB at 10 cm and ~57 dB at 3 m, and wants **>=40 dB of
loss between units**; ten feet in a reflective room is past hard clipping and
squarely in the multipath-desense regime, so "move it further away" does not
by itself buy a valid bench. The craft's power is reachable over RF as the
§11.7 `tx_power` tier (a tier can only ever LOWER power), so drop it to the
lowest preset and re-sweep. Failing that, verify the channel against the
craft's own `GET /api/v1/info`.

## 2026-08-14 — the power control: compression proven, adjacent-channel bleed NOT

**Rig:** x86 ground (`.2.242`, 8812AU on bus 8-1, dev build, scout dwell 300 ms
over 5745/5765/5785/5805/5825) against craft 17 (`.2.232`, 8812EU) transmitting
video on 5805 under its own supervisor. The craft's §10.5 offset was stepped
down over `POST /api/v1/tx/power` and restored to `auto` afterwards.

| craft power | RSSI dBm | SNR dB | **EVM dB** | passive noise dBm | 5805 frames |
|---|---:|---:|---:|---:|---:|
| auto (run 1) | -8 | 30 | **-13** | -54 | 2047 |
| auto (run 2) | -8 | 30 | **-15** | -55 | 2042 |
| -6 dB | -18 | 29 | **-27** | -64 | 3272 |
| -12 dB | -24 | 27 | -28 | -71 | 2473 |
| -18 dB | -26 | 25 | -27 | -71 | 2624 |
| -24 dB | -26 | 25 | -27 | -71 | 3249 |
| -30 dB | -26 | 25 | -27 | -71 | 2671 |

**Compression at bench range is proven, and it is invisible to SNR.** Backing
the craft off 6 dB moved SNR by **1 dB** and EVM by **14 dB**. That is the
`bench-testing-near-field.md` signature exactly, run in reverse: at its default
power this ground sits well past the saturation knee with a collapsed
constellation while SNR reports a healthy 30 dB. The passive floor rising 17 dB
with power (-71 -> -54) is the same doc's self-jamming tell.

**Frame suppression on the true channel: supported, not proven.** The `auto`
row reproduced to within 5 frames across two runs (2047, 2042) and is the
**lowest** of all seven points; every backed-off point is 2473-3272. Direction
consistent, but per-step n=1 and dwell length varies with extension, so this
corroborates the mechanism rather than measuring it.

**The adjacent-channel half did NOT reproduce — withdraw the ACR-collapse
reading.** At **no** power, including EVM -13 dB, did any channel other than
5805 produce a candidate row for craft 17 (`other_chans` empty on all seven
sweeps). The 2026-08-13 amendment above speculated that compression collapses
adjacent-channel rejection and so explains case 2's +60/+120 MHz rows; **this
control does not support that** and it should not be repeated. Compression is
real and present, but **on this adapter** it does not by itself manufacture a
neighbouring candidate.

**Read the 8812CU entry below before concluding from this one.** Swapping only
the ground adapter reproduces the bleed at every power and reproduces the
one-frame tie itself, so the negative here is a property of the Jaguar1 AU, not
evidence that the phone's rows were spurious.

**Consequence for the resolver fix.** Unchanged in direction: RSSI still
separates the cases the phone saw, and peak RSSI stays sound under compression.
But the "saturated dwell" refusal proposed on #178 must not be justified by the
bleed story — its justification is that a compressed dwell's *frame count* is
untrustworthy, which is what the table above shows.

## 2026-08-14 — same run on an 8812CU: the bleed IS reproducible, and it peaks at a mid power

Identical rig, craft and script; only the ground adapter changed, **8812AU
(Jaguar1, bus 8-1) -> 8812CU (Jaguar3, bus 1-1)**. This is the control the AU
run could not provide, because the AU never presented a second candidate.

| craft power | RSSI dBm | SNR dB | EVM dB | 5805 frames | **5745 frames** | 5745/5805 |
|---|---:|---:|---:|---:|---:|---:|
| auto | -15 | 36 | -18 | 1850 | **67** | 0.04 |
| -6 dB | -22 | 35 | -31 | 2128 | **2129** | **1.00** |
| -12 dB | -29 | 35 | -32 | 2656 | 2216 | 0.83 |
| -18 dB | -31 | 36 | -33 | 2729 | 1631 | 0.60 |
| -24 dB | -31 | 36 | -33 | 3033 | 1503 | 0.50 |
| -30 dB | -31 | 36 | -33 | 2328 | 1070 | 0.46 |

**The 2026-08-13 failure reproduced exactly.** At -6 dB the craft, transmitting
on 5805, was heard **2129 times on 5745 and 2128 times on 5805** — the neighbour
won the heard-most rule by one frame, against 2299-vs-2350 on the phone.

**It is chip-dependent.** The AU produced **zero** neighbouring candidates at
every power including EVM -13; the CU produces them at every power. The
2026-08-13 entry's "the craft is 60 MHz away and still decodes there" is
therefore a property of that receiver, not of the range.

**The bleed is NON-MONOTONIC in power, which no filter skirt explains.** It is
*lowest* at the highest power (0.04), peaks at -6 dB (1.00), then falls away. A
constant-ratio skirt plus a decode threshold gives a monotone decline; it cannot
put the minimum at maximum power. The mid-power peak is where the leakage lands
inside the receiver's linear window — above it the 5745 dwell is itself
overloaded and decodes badly, below it the leakage fades. So the earlier ACR
guess was wrong in mechanism but the right family: **overload governs the
neighbour dwell too**, and it suppresses rather than creates its decodes.

**Why the sweep still resolved correctly here.** `candidate_for()`'s 80%
resting-channel tie-break caught it: this ground rests on 5805 with
`preferred_originator: 17`, so the trusted prior returned the resting channel
despite 5745 leading on frames. That is exactly why #178 case 2 failed and case
1 mostly did not — there the ground rested on 5805 while the craft was at 5180,
so the prior was WRONG and the rule fell through to heard-most. **The tie-break
is load-bearing, and it only works when the ground is already resting on the
right channel.**

## 2026-08-14 — the craft's TX power rails below ~-12 dB of commanded offset

Two different receivers agree, so this is the transmitter, not a reporting
artifact. Commanded offset vs RSSI: **AU** -18, -24, -26, -26, -26; **CU** -22,
-29, -31, -31, -31 for -6, -12, -18, -24, -30 dB. Both track ~6-7 dB per 6 dB
commanded down to -12 dB, gain ~2 dB more by -18, then stop dead. EVM and SNR
flatten at the same point.

**Mechanism, from the vendored source.** devourer's model is
`effective = clamp(baseline + offset_steps, 0, index_max)`
(`third_party/devourer/src/TxPower.h:17`) — the TXAGC index rails at 0. It
reports both halves: `SetTxPowerOffsetQdb` **returns the qdb it actually
applied**, and `GetTxPowerState` carries `saturated_low` / `saturated_high`,
documented as *"the signal a closed-loop controller uses to know the knob has
run out of travel"*.

**We read neither.** `RadioAir::set_power_qdb` (`io/src/air_radio.cpp:1226`)
discards the return with `(void)` and its own comment says so — *"devourer
returns the qdb it applied; no caller has ever read it"* — and `saturated_low`
appears nowhere in the tree. `GET /api/v1/tx/power` answers
`{"override_active":false,"backend":"radio"}`, i.e. commanded state only.

So §10.5 reports success for an offset the chip never reached, and §10.6/§10.7
calibration places rungs on the assumption that the commanded rung is the
delivered one. Filed separately.

**Confirmed directly 2026-08-14 (Pass 169 / #181), and the rail is where the
RSSI said.** With the flags plumbed through §15.5, each unit was asked where
its own rail is instead of the rail being inferred from a plateau:

| unit | family | `index_max` | step | offset-range clamp | **per-rate rail (`saturated_low`)** |
|---|---|---:|---:|---|---|
| 8812AU ground | Jaguar1 | 63 | 2 qdb | -126 qdb (-63 steps) | not bisected |
| 8812CU ground | Jaguar3 | 127 | 1 qdb | -127 qdb (-127 steps) | **-41 qdb (-10.25 dB)** |
| 8822EU craft | Jaguar3 | 127 | 1 qdb | -127 qdb | **-64 qdb (-16 dB)** |

The craft's -16 dB lands inside the -12..-18 dB window the two-receiver RSSI
plateau bracketed, by a completely independent method. The AU and CU reaching
their offset-range clamp at -126 and -127 through different arithmetic (63x2
against 127x1) is the cross-family check that the reporting is not echoing a
constant.

**There are TWO rails, and the issue emphasised the wrong one.** The
offset-range clamp is what makes `applied_qdb != qdb`. The per-rate TXAGC rail
sets `saturated_low` **while `applied_qdb` still equals the request exactly** —
on the craft the flag fires at -64 while applied tracked the request all the
way to -126. A consumer diffing request against applied would have seen
perfect agreement across the entire dead region. **`applied_qdb` alone would
not have caught the floor this finding is about; the flag is the signal.**
