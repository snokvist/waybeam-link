# Hardware-ACK hybrid — bring-up and verification on hardware

A runbook for turning Pass 198 on. **First bench session ran 2026-08-30**
(ground `.242` 8812AU ↔ craft `.232` 8812EU, 5540 MHz): the §2b die question
is settled, the §6 A/B is measured, and the §7 storm guard is proven. Sections
carry their results inline; what remains open is named in §9. The §4.4 numbers
everyone quotes (86.9 % → 99.9 % at 3000 pps) were
measured with retry 8, a never-expiring latch, and NACK/LINK_REPORT only —
all three have since changed, so §4.4 is context, not a baseline you can
compare against.

Read `docs/findings.md` 2026-08-30 ("hardware-ACK hybrid seeds") first; it
records which numbers are seeds and which are measured.

## 1. What you are enabling

Two knobs on **opposite** nodes. They are independent by design — receivers
always accept both frame shapes (§3.0), so you can deploy one half at a time.

| node | key | effect |
|---|---|---|
| craft | `air.ack_responder: true` | arms the chip's ACK responder on its own §3.0 SA — the craft starts answering unicast frames with SIFS-timed ACKs |
| ground | `policy.return.unicast: true` | returns go out as unicast QoS-Data to the craft's latched SA instead of broadcast |

Three supporting knobs already have working defaults; you should not need to
touch them on a first run:

- `air.tx_retry_limit: 3` — how many hardware retries an unACKed return gets.
- `air.ack_timeout_us: 128` — the ACK window, i.e. the range budget (~15 km
  round trip).
- `policy.return.unicast_stale_ms: 1000` — the storm guard (§3).

**Storm avoidance is automatic and needs no configuration.** That is the whole
point of the stale-latch clause: enable the two knobs and an out-of-range craft
stops being unicast-addressed after one second. Section 3 is how you *prove*
it, not how you turn it on.

## 2. Two die questions — one still real, one settled

Both were raised by reading devourer's descriptor builders. **§2b has since
been settled on air and is NOT a gate** — the 8812AU solicits correctly. §2a
is still real today, but has an upstream fix in flight.

### 2a. The `.181` craft cannot be a responder at all

`.181` is the CV610 craft on an **RTL8733BU**, and `SetAckResponder` is
unported on that die — it is listed under "Not ported" in
`third_party/devourer/src/rtl8733b/CLAUDE.md` and inherits `IRtlDevice`'s
`false`. Setting `air.ack_responder: true` there is not an error; it logs

```
radio: ack responder unsupported on "bu-craft"
```

and the run continues with returns received as broadcast. That is §3.0's
loud-degrade path working correctly, but it means **`.181` is not the craft to
A/B on** — today. Filed upstream as snokvist/devourer#2 (which also covers that
die hardcoding REG_ACKTO to 33 µs and having no CCX `tx.report` path), and
**closed by OpenIPC/devourer#406**, which ports `SetAckResponder` and applies
`tx.ack_timeout_us` on the 8733B and measures the responder at ack_rate 1.00
armed / 0.00 disarmed. Once #406 lands upstream and is vendored here, re-run
§6 with `.181` as the craft and delete this subsection.

Use **`.232`** — the 8812EU craft (Jaguar3). devourer's own responder matrix
measures the 8812EU at 98 % on / 0 % off, the joint-best cell in the table.

### 2b. The `.242` ground's uplink AU — SETTLED 2026-08-30, the concern was wrong

This was raised as the gate that decided whether the whole A/B measured
anything. **It is not a gate. The 8812AU solicits ACKs correctly.**

The concern was structural and worth raising: `.242`'s uplink is an
**8812AU — Jaguar1**, the only generation that does not derive the descriptor
BMC bit from addr1 but hardcodes it:

| generation | descriptor BMC |
|---|---|
| Jaguar1 (8812AU/8821AU) | **hardcoded `1`** — `src/jaguar1/RtlJaguarDevice.cpp:1224`, cleared only in the `bf.ndpa_period > 0` branch |
| Jaguar2 | `dot11[4] & 0x01` — `src/jaguar2/RtlJaguar2Device.cpp:1536` |
| Jaguar3 | `dot11[4] & 0x01` — `src/jaguar3/RtlJaguar3Device.cpp:2028` |
| RTL8733B | `dot11[4] & 1u` — `src/rtl8733b/Rtl8733bDevice.cpp:729` |

BMC=1 reads as "broadcast/multicast, no ACK expected", so the prediction was
that `.242`'s unicast returns would solicit nothing while `unicast_sent`
climbed and the config looked enabled.

**Measured, and the prediction failed.** Ground `.242` (8812AU uplink) against
craft `.232` (8812EU), 90 s arms, `.232` on 5540 MHz:

| craft responder | `unicast_sent` | `tx_report_fails` |
|---|---|---|
| **off** | 2796 | **2796 — 1:1** |
| **on** | 2790 | **48 (1.7 %)** |

A frame the MAC really treated as broadcast carries no ACK policy and cannot
report a retry-exhaustion failure at all. 1:1 failures with nothing answering
mean the AU requested an ACK on **every** frame and exhausted its retry limit
every time; arming the craft's responder collapsing that to 1.7 % means the
loop closes. So the hardcoded descriptor BMC bit does **not** gate the MAC's
ACK-policy decision on this die — the two are less tightly coupled than the
descriptor docs imply.

This closes a question open since the Pass 12 notes. `.242`'s 8812AU is a
valid A/B ground: no CU fallback, no §7.2 quiet-gap trade, no upstream issue
needed. Independently corroborated by OpenIPC/devourer#406, whose responder
matrix uses an **8812AU as the soliciting side** and sees retries pinned at
the limit in its disarmed control.

## 3. The config edits

Desk-validated: both shapes below `--check --strict` clean with no new unknown
or inert keys, and the coupling law passes because `air.tx_retry_limit` now
defaults to 3.

Pair on `net_id 0`: ground `.242` (originator 9) ↔ craft `.232` (originator 17).
`.181` is `net_id 1` and is a different pairing.

**Ground — `deploy/ground-192.168.2.242.json`:**

```json
  "policy": {
    "return": { "quiet_gap": true, "guard_us": 300, "return_window_us": 2000,
                "unicast": true }
  }
```

**Craft — `deploy/vehicle-192.168.2.232.json`:**

```json
  "air": { "kind": "radio", "mcs_probe": true, "ack_responder": true }
```

Validate before deploying:

```
build/dev/waybeam-link rx -c deploy/ground-192.168.2.242.json --check --strict
build/dev/waybeam-link tx -c deploy/vehicle-192.168.2.232.json --check --strict
```

Both must exit 0. A ground whose resolved TX die reports
`caps.tx_retry_limit_ok = false` refuses bring-up (§3.0 capability leg) — that
is a real refusal, not a warning, and the 8812AU passes it (only the 8814A die
reads false).

## 4. Bench hygiene

From `CLAUDE.md`, each of these cost real debugging time before:

- Run from the repo root — the binary loads `profiles/` by relative path.
- Unload kernel drivers first: `sudo rmmod 88x2cu rtw88_8812au`.
- **Bus paths shuffle after any re-plug.** Re-check `lsusb -t` before assuming
  `1-1` / `8-1` still point where the config says.
- Kill bench processes **only from a script file**:
  `pkill -TERM -f 'build/dev/waybeam-link'` typed in an interactive shell
  matches its own cmdline and kills the shell. SIGTERM only, never SIGKILL.
- A wedged RTL88x2 (RX counter frozen) needs a physical re-plug, not a driver
  reload.
- Two adapters of the same part number are not a replicate (Pass 139). If a
  result looks like a broken chip, try the other unit before believing it.

## 5. Does the uplink actually solicit an ACK?

**Answered for the 8812AU (§2b): yes.** Keep this method for any NEW uplink
die — it is still the cheapest way to avoid measuring nothing — but it is no
longer a gate on `.242`.

The cheapest form needs no capture at all: run one arm with
`policy.return.unicast` ON and the craft's responder OFF, and read
`tx_report_fails`. Climbing 1:1 with `unicast_sent` proves the MAC is
soliciting (and exhausting the limit, since nothing is answering). Staying ~0
while `unicast_sent` climbs is the silently-inert signature — that die is
descriptor-marking the return as broadcast.

The direct read is a capture: put a third adapter in monitor mode, or use
devourer's own witness tooling, and look at a ground→craft return frame. What
you need to know is whether the craft answers it with an ACK. Two signatures,
either sufficient:

- **On air:** an ACK frame from the craft's SA `56:42:00:00:11:xx` following
  the return at SIFS. Present = the loop closes.
- **On the ground:** `adapters[].tx_reports` climbing while
  `tx_report_fails` stays ~0 means frames are being ACKed;
  `tx_report_fails` climbing 1:1 with unicast returns means the retry limit is
  being exhausted every time — no responder, or no ACK solicited.

Note what our stats **cannot** tell you: `RadioAir` counts `tx.report` events
and the `ok:false` subset, and **drops the retry count**
(`io/src/air_radio.cpp` `ev_write`). So you get "did it deliver" but not the
retry distribution. For the distribution use devourer's own harness
(`tests/ack_txreport_matrix.sh` shape). This is the same limit finding #96
already records as open.

If the AU turns out to solicit correctly despite the hardcode — possible, since
BMC and the ACK-policy decision may not be as tightly coupled as the descriptor
docs imply — **write that down in `docs/findings.md` immediately**. It closes a
question that has been open since the Pass 12 notes.

## 6. The A/B

Only once §5 says the loop can close. Method follows §4.4 so the two are
comparable where they can be.

Four arms, interleaved rather than run in sequence (ambient drifts):

| arm | ground `return.unicast` | craft `ack_responder` |
|---|---|---|
| A | off | off | baseline — today's shipped behaviour |
| B | on | off | isolates the wire-shape change from the ACK loop |
| C | off | on | should be identical to A; catches a responder that misbehaves as a receiver |
| D | on | on | the hybrid |

B and C are not padding: B on its own is the arm that tells you whether unicast
addressing costs anything when nobody answers, and C is how you find out
whether arming the responder hurts a craft that is mostly transmitting.

Drive a real RTP feed (`tools/rtp_feed.py`) — the Pass 70 issuer video-confirm
needs one, and a craft with no feed sends no return-eliciting traffic.

What to record per arm, all from the §15.3 line:

- `return.reports_expected` / `reports_received` — the delivery ratio §4.4
  moved from 86.9 % to 99.9 %. This is the headline.
- `return.unicast_sent` / `unicast_fallback` / `unicast_stale` — in a healthy
  arm D, `unicast_sent` should dominate and both fallbacks should be ~0.
  A nonzero `unicast_stale` on a link you consider healthy means the 1000 ms
  seed is too tight — that is a finding, record the number.
- `adapters[].tx_reports` / `tx_report_fails` — the delivery sensor from §5.
- Downlink video delivered % — the regression guard. §4.4 saw 95.47 % → 95.46 %,
  i.e. no cost. If the downlink moves this time, retry airtime is the first
  suspect and `air.tx_retry_limit` is the knob.

Also worth one run: **retry 3 vs 8**, since Pass 198 changed the default and
nothing has measured the difference on our return path (only in devourer's
collision regime). Set `air.tx_retry_limit: 8` on the ground for one arm and
compare `reports_received` and downlink airtime.

### Results, 2026-08-30

Ground `.242` (8812AU uplink, elected by `adapters:{auto}` over a CU and a BU)
↔ craft `.232` (8812EU, originator 17, 5540 MHz, live ~58 fps venc feed),
~-22 dBm bench geometry, 90 s per arm:

| arm | gnd `return.unicast` | craft `ack_responder` | reports received | `tx_report_fails` | downlink loss |
|---|---|---|---|---|---|
| A | off | off | 820/900 = **91.11 %** | 0 / 2796 | 3 ‰ |
| B | on | off | 902/902 = **100.00 %** | **2796 / 2796** | 14 ‰ |
| C | off | on | 829/901 = **92.01 %** | 0 / 2793 | 3 ‰ |
| D | on | on | 900/900 = **100.00 %** | 48 / 2790 = **1.7 %** | 5 ‰ |

Arm B is the informative one: unicast **alone** reaches 100 % by brute
hardware retransmission with nothing ACKing, at 4.7× the downlink loss. Arm C
≈ arm A, so arming a responder costs a mostly-transmitting craft nothing.

**Retry 3 vs 8**, interleaved, in the doomed-frame regime (unicast on,
responder off — where retry depth actually costs):

| retry limit | downlink loss | reports received |
|---|---|---|
| 3 | **13–14 ‰** (two runs) | 100 % |
| 8 | **29 ‰** | 100 % |

So 8 costs ~2.2× the airtime of an unanswered return and buys no delivery.
That supports the ruling; the band stays 3–5 and 4/5 remain unmeasured. In the
healthy hybrid (arm D) the difference is inside noise: 5 ‰ @3 vs 6 ‰ @8,
`tx_report_fails` 1.7 % @3 vs 0.14 % @8.

**Watch the channel.** The live ground config's `policy.csa.home_chan` was
5700, which silently latched craft **19** (`.181`) with `table_version 164`
against the ground's 242 — that forces §3.4 BEST-EFFORT, which disables ARQ,
and the return path measures nothing. Pin `adapters.auto.channel` and
`node.preferred_originator` to the craft you mean.

## 7. Proving the storm guard

**Run 2026-08-30. It holds — numbers at the end of this section.** Repeat it
on any new ground/craft pair; the method below is what was actually used.

A note on provocation: you do not need to kill the craft. `POST
/api/v1/bench/rx-drop {"permille":1000}` on the ground drops frames at
`io/src/air_radio.cpp:650`, which is **before** `latch_sa` at `:673`, so the
latch starves exactly as it would for a departed craft — with no SoC stop, no
overlay risk, and instant release, because the craft never stopped
transmitting. Check that drop-vs-stamp ordering before reusing the trick for
any other latch.

**Setup:** ground and craft linked, arm D, confirmed healthy (`unicast_sent`
climbing, `unicast_stale` 0).

**Provoke:** kill the craft, or power it down, or walk it out of range. Do not
just stop the video feed — the craft must stop *transmitting*, because the latch
is fed by accepted frames, not by video.

**Expect, within ~1 s of the last heard frame:**

1. `return.unicast_stale` starts climbing, once per return the ground tries.
2. `return.unicast_sent` stops climbing.
3. `tx_report_fails` stops climbing shortly after — the ground is no longer
   soliciting ACKs, so there are no retry-drops to report.

**The failure this catches:** without the guard, `unicast_sent` would keep
climbing forever and every one of those frames would cost 4 copies plus 3 ACK
windows. If you see `unicast_sent` still advancing a few seconds after the craft
is gone, the guard is not working — capture the config and the stats line.

**Then bring the craft back.** `unicast_stale` must stop and `unicast_sent`
resume, with no operator action and no campaign: the first accepted frame
re-latches. Re-acquisition being automatic is half the design; a guard that
needed a manual re-arm would be worse than none.

**Measured 2026-08-30** (ground `.242` 8812AU ↔ craft `.232` 8812EU, retry 3,
provoked with `bench/rx-drop` as above):

| | `unicast_sent` after last heard frame | `tx_report_fails` | `unicast_stale` |
|---|---|---|---|
| guarded, `unicast_stale_ms: 1000` | **+2**, then frozen | froze at the same instant | 84 (broadcast fallback) |
| control, `unicast_stale_ms: 0` | **+83** | climbed 1:1 to 355 | 0 |

Age-out landed ~1.0 s after the last accepted frame; on release `unicast_sent`
resumed within 1 s with no operator action. Derived post-loss airtime (4
airings per unanswered unicast at retry 3, 1 per broadcast fallback): ~92 vs
~332 airings, **3.6×**; soliciting frames 2 vs 83, **41×**.

**One correction to the argument this pass was built on:** the unguarded storm
is *not* unbounded in time. It self-terminated after ~5 s, when the ground's
own return generation stopped for want of RX. The guard is still the right
fix — it turns an incidental ~5 s tail into a controlled ~1 s one — but the
"forever" framing was wrong and §3.0 now says so.

## 8. The live toggle

`POST /api/v1/air/ack_responder {"armed": true|false}` arms/disarms without a
restart, and `GET` returns `{armed, supported, configured, mac}`. Use it to run
arms C and D back to back on the craft without a restart between them.

Two things to know:

- **It is the responder half only.** The ground's `air.tx_retry_limit` and
  `air.ack_timeout_us` are consumed at devourer bring-up from a `const`
  `DeviceConfig` and have no live form, so any arm that changes those still
  costs a ground restart. A/B design should put the craft-side switching on the
  endpoint and accept restarts on the ground.
- `supported` reads `true` until an arm has actually been **refused** — it is
  "not known to be unsupported", not a capability read, because devourer
  publishes no flag for the responder. On the `.181` craft it will read `true`
  until you POST once, then `false`. That is by design, and §15.5 says so.

## 9. What to write back

- **`docs/findings.md`** — every number from §6 and §7, dated, with the
  geometry and the arm. The three seeds (retry 3, stale 1000 ms, window 128 µs)
  each have an entry there already; amend or supersede rather than adding a
  fourth.
- **The BMC verdict from §5**, whichever way it goes. If confirmed, open an
  issue on the devourer fork.
- **`docs/step11-bench.md` §4.4** — its STATUS line still describes the Pass 12
  measurement. Once this runs, it should say what the current shape measures.
- **Do not amend `PROTOCOL.md`** for a measurement. If a mechanism settles and
  you want the seed to become law, that is a new Pass under the tier-1 process
  (`CLAUDE.md`, "The law"), not an edit to Pass 198.
