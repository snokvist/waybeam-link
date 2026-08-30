# Hardware-ACK hybrid — bring-up and verification on hardware

A runbook for the bench session that turns Pass 198 on for the first time.
Everything here is desk-verified only: the code builds, gates pass, and the
config shapes below `--check --strict` clean. **No part of Pass 198 has been on
air.** The §4.4 numbers everyone quotes (86.9 % → 99.9 % at 3000 pps) were
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

## 2. Two hardware gates — read before touching a config

Both are properties of the dies in the current fleet, not of the code, and both
were established by reading devourer's descriptor builders. Neither has been
confirmed on air.

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
A/B on**. Filed upstream as snokvist/devourer#2 (which also covers that die
hardcoding REG_ACKTO to 33 µs and having no CCX `tx.report` path).

Use **`.232`** — the 8812EU craft (Jaguar3). devourer's own responder matrix
measures the 8812EU at 98 % on / 0 % off, the joint-best cell in the table.

### 2b. The `.242` ground's uplink is the one die with the BMC bug

This is the gate that decides whether the whole A/B measures anything.

`.242` is the only ground in the fleet with a `role:"tx"` adapter (`.199` is
RX-only and will *refuse bring-up* if you set `policy.return.unicast` on it,
per §3.11). Its uplink is `au-uplink`, an **8812AU — Jaguar1**. Jaguar1 is the
only generation that does not derive the descriptor BMC bit from addr1:

| generation | descriptor BMC |
|---|---|
| Jaguar1 (8812AU/8821AU) | **hardcoded `1`** — `RtlJaguarDevice.cpp:1224`, cleared only in the `bf.ndpa_period > 0` branch |
| Jaguar2 | `dot11[4] & 0x01` — `RtlJaguar2Device.cpp:1536` |
| Jaguar3 | `dot11[4] & 0x01` — `RtlJaguar3Device.cpp:2028` |
| RTL8733B | `dot11[4] & 1u` — `Rtl8733bDevice.cpp:729` |

BMC=1 tells the MAC the frame is broadcast/multicast, i.e. **no ACK expected**.
If that reading holds on air, every unicast return the `.242` ground sends is
descriptor-marked broadcast, solicits no ACK, and gets no retries — while
`unicast_sent` climbs happily and the config looks enabled. That is precisely
the silently-inert failure the Pass 156 coupling law exists to prevent, and it
slips past that law because the law checks the retry limit, not the descriptor.

**So settle §5 (the BMC probe) before running any A/B on `.242`.** A "the
hybrid does nothing" result on that ground is more likely to be this bug than a
verdict on the hybrid.

If the BMC reading is confirmed, the options are, in order of preference:

1. Report it upstream (issue on the devourer fork, same shape as #2) and A/B
   against a Jaguar3 ground uplink if one can be plugged. The `.242` box
   already carries an 8812CU ear.
2. Elect the CU as uplink instead of the AU. **This costs the §7.2 quiet gap**
   — finding #99 measured the CU's `ReadTsf` at ~1234 µs mean against the AU's
   184 µs, which puts the ±1000 µs release window structurally out of reach.
   Arguably acceptable *for this test specifically*, since hardware retries are
   what the quiet gap was compensating for, but that is a real trade and an
   operator call, not a bench shortcut. Do not do it silently.

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

## 5. Probe first: does the AU actually solicit an ACK?

Do this before any A/B. It is the cheapest way to avoid measuring nothing.

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
compare `reports_received` and downlink airtime. If 3 costs real delivery, say
so — the ruled band is 3–5 and 4/5 are unmeasured, so there is room to move
without reopening the ruling.

## 7. Proving the storm guard

This is the part that has never run, and it is the reason Pass 198 exists.

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

**Also measure the thing we could not:** with a spectrum analyser or a witness
capture, compare channel occupancy from the ground during 10 s of "craft gone"
with and without the guard (`policy.return.unicast_stale_ms: 0` restores the old
never-expiring behaviour and exists for exactly this A/B). The predicted
difference is the storm — bounded per frame by the retry limit, unbounded in
time without the guard. Put the measured number in `findings.md`; it is
currently an argument, not a measurement.

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
