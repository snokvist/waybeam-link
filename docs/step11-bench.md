# Step 11 — hardware bring-up bench report + forward plan

Step 11 (`docs/build-order.md` §19 step 11): "Field bring-up; run gates 1–4. FEC
(§14) only if gate 2's P95 says so," plus the Pass 8 bench slots (§3.0 smoke,
uplink HW-ACK A/B, TX-wedge detector). Gates are defined in PROTOCOL.md §17;
`docs/review-log.md` Passes 9–12 (CSA MAC, gate-3 estimator, §9.10 wedge
watchdog, §3.0 unicast returns) are the spec rulings feeding this bench — the
machinery for the last two landed after the first bench session (§4.4/§4.5
status notes). All numbers below are
**session-verified 2026-07-11 on the x86 dev bench** — not simulated, not
projected.

**If you are a fresh agent picking this up:** read `CLAUDE.md` ("The law") and
the last three `docs/review-log.md` passes first — this doc assumes you already
know PROTOCOL.md is LAW and spec amendments commit before code. §4 below is a
self-contained punch list; you do not need to re-derive anything in §1–§3 to
start on it, but read §2 before touching gate-2/3/4 seeds — the numbers explain
*why* the current seeds are provisional.

---

## 1. Bench rig

x86 dev box, three USB WiFi adapters, channel **5805, 20 MHz**, `net_id 0`,
craft `originator 17`, ground `originator 9`:

| adapter | chip | USB bus | role |
|---|---|---|---|
| ground uplink | 8812AU (Jaguar1) | `6-1` | duplex tx+rx — designated NACK/LINK_REPORT uplink |
| ground diversity sibling | 8812CU (Jaguar3) | `1-1.2` | rx only |
| craft | 8812CU (Jaguar3) | `1-1.1` | duplex — TX video + RX returns, single radio (§1) |

**Gotchas** (also in `CLAUDE.md`, repeated here because this doc is meant to
stand alone):

- Bus paths **shuffle after any re-plug** — re-check with `lsusb -t` before
  trusting a path in a config.
- Kernel drivers must be unloaded first: `sudo rmmod 88x2cu rtw88_8812au`. The
  host's own MediaTek WiFi (`wlp2s0`) is untouched.
- Ground egress is a UDP socket sink on `127.0.0.1:5700` (delivered RTP, counted
  there). Craft ingest is fed by `tools/rtp_feed.py` on `127.0.0.1:5600`.
- Kill bench processes **only** from a script file:
  `pkill -TERM -f 'build/dev/waybeam-link'` — typed directly into an
  interactive shell, the pattern matches the shell's own cmdline too and kills
  the shell (exit 144). SIGTERM only, never SIGKILL.

### 1.1 Configs (reconstructed, inline)

**Craft — `gate1-tx`:**

```json
{
  "node": { "originator": 17, "role": "tx", "net_id": 0 },
  "profile_table": "profiles/table.example.json",
  "adapters": [
    { "name": "cu-craft", "bus": "1-1.1", "role": "tx", "channel": 5805, "bw": 20 }
  ],
  "streams": [
    { "stream_id": 0, "stream_type": "RTP", "dir": "in", "classifier": "h265",
      "bind": { "kind": "udp", "listen": "127.0.0.1:5600" } }
  ],
  "air": { "kind": "radio" },
  "policy": {
    "return": { "quiet_gap": true, "guard_us": 300, "return_window_us": 2000 },
    "csa": {
      "psk": "CHANGE-ME-craft-and-ground-only",
      "settle_s": 3.0, "verify_timeout_ms": 150, "min_interval_s": 5,
      "ack_timeout_ms": 1000, "rendezvous_timeout_s": 5,
      "home_chan": 5745, "channel_allowlist": [5745, 5805, 5825]
    }
  },
  "venc": { "enabled": false },
  "stats": { "hz": 1 }
}
```

For bench pinning, add `policy.select: { "min_profile": N, "max_profile": N }`
(min==max pins the operating point — §9.7; stats `link.state` reports
`"PINNED"`) and optionally `adapters[0].power_map` (a PHY_REG_PG.txt-format
subset, §10.2) to push a fixed TX-power table instead of the profile-table
curve.

**Ground — `gate1-rx`:**

```json
{
  "node": { "originator": 9, "role": "rx" },
  "profile_table": "profiles/table.example.json",
  "adapters": [
    { "name": "au-uplink", "bus": "6-1", "role": "tx", "channel": 5805, "bw": 20 },
    { "name": "cu-div0", "bus": "1-1.2", "role": "rx", "channel": 5805, "bw": 20 }
  ],
  "streams": [
    { "stream_id": 0, "stream_type": "RTP", "dir": "out",
      "bind": { "kind": "udp", "send": "127.0.0.1:5700" } }
  ],
  "air": { "kind": "radio" },
  "policy": {
    "return": { "quiet_gap": true, "guard_us": 300, "return_window_us": 2000 },
    "csa": {
      "psk": "CHANGE-ME-craft-and-ground-only",
      "settle_s": 3.0, "verify_timeout_ms": 150, "min_interval_s": 5,
      "ack_timeout_ms": 1000, "rendezvous_timeout_s": 5,
      "home_chan": 5745, "channel_allowlist": [5745, 5805, 5825]
    }
  },
  "stats": { "hz": 1 }
}
```

**Drop-knob variant** (gate-2 synthetic loss) — same ground config plus
`"air": { "kind": "radio", "rx_drop_permille": 150 }` (per-adapter independent
synthetic RX drop, 0–1000 ‰; bench-only, `io/include/wblink/config.h`).

### 1.2 Runner-script skeleton

```sh
#!/usr/bin/env bash
# 1. background UDP sink counting delivered RTP on :5700
python3 tools/udp_sink.py 127.0.0.1:5700 > sink.log &
SINK=$!

# 2. start ground rx first (it must be listening before the craft transmits)
build/dev/waybeam-link rx -c gate1-rx.json > ground-stats.jsonl &
GROUND=$!
sleep 8   # let radios settle, adapters latch, CSA/venc paths init

# 3. start craft tx
build/dev/waybeam-link tx -c gate1-tx.json > craft-stats.jsonl &
CRAFT=$!
sleep 5

# 4. drive the saturating feed: <duration-s> <pps> <fps>
python3 tools/rtp_feed.py 60 3000 60

# 5. teardown — SIGTERM only, from this script (see §1 gotcha)
pkill -TERM -f 'build/dev/waybeam-link'
wait "$GROUND" "$CRAFT" 2>/dev/null
kill "$SINK"

# 6. stats are the two *-stats.jsonl files; feed to tools/gate2_rho.py /
#    tools/gate3_rtt.py per §2 below.
```

`tools/udp_sink.py` is a stand-in name for "whatever counts arrived
datagrams on :5700" — the bench used a one-line `socket.recvfrom` loop, not a
committed tool; write one before the next run if it doesn't exist yet.

---

## 2. Verified results (per PROTOCOL.md §17 gate)

### §3.0 on-air encapsulation smoke — **PASSED**

Pinned ToDS=0, non-QoS Data frame airs byte-exact. Found + fixed: devourer's
monitor RX appends the chip-validated 4-byte FCS trailer to the captured MPDU;
this is now stripped in `RadioAir` before the §3.0 parse (`kFcsLen`, spec-noted
in §3.0, code in `2bc6c6f`/`3f233b4`). TX path verified byte-exact: 1298-byte
USB bulk transfer = 48-byte devourer txdesc + 24-byte dot11 header +
1226-byte wire payload.

### Gate 1 — **PASSED**

Injector + monitor siblings mixed in one process, both chip families:

- Ground single process: AU duplex uplink (975 rx + 200 tx, **200/200** CCX
  `tx.report` — Jaguar1 sensor exact) + CU RX sibling (974 rx).
- Diversity delivered **981/982** (post-merge).
- Craft heard **198/200** LINK_REPORTs, age 21 ms, held **MCS7**.

Residual unknown from `docs/build-order.md` gate 1 ("injector + monitors mix")
is closed for both Jaguar1 and Jaguar3.

### Gate 2 — **PASSED** (synthetic accounting + physical vehicle walk fade)

Synthetic, 60 s @ `rx_drop_permille=150` on both ground adapters: measured
15.3%/14.0% per-adapter, joint post-diversity **2.10%** vs independence-product
**2.07%**, Pearson **ρ ≈ −0.36** (n=59 windows) — decorrelated. Accounting
check: `diversity`/`uniq` = 0.744 vs theoretical 0.739.

Desk-partial real-fade: hand-fade at pinned MCS7 on one ground adapter →
**68% mean loss** on that adapter (P95 windows = 100% blackout), sibling
**0.3%**, joint **0.22%** vs **0.21%** independence — ρ ≈ **−0.07**, textbook
diversity behaviour.

The missing vehicle-range sample was completed 2026-07-19 using kernel monitor
mode, channel 161/HT20, MCS5, N=2 ground receive, and 10% GF(256) RLC. Across
179.4 s, pre-diversity loss was 86‰ and post-diversity/pre-ARQ loss was 24‰;
FEC recovered 599 source symbols and ARQ 52. There were 45.7 s with neither
receiver delivering a packet, including a 37.1 s continuous blackout. This
closes the gate in favor of light GF(256) FEC: diversity remained the primary
recovery mechanism, FEC dominated residual repair, and ARQ remained a backup.
Static 33% FEC cannot repair the measured whole-site blackout and is not the
base operating point; retain 10%, with 15–20% reserved for a future adaptive
edge experiment.

Incidental: commanded TX-power floor (`power_map` all-1-dBm →
`applied qdb=-20` confirmed in link stats) barely moved received RSSI on the
CU under devourer — flagged as its own follow-up (§4.6), not folded into the
gate-2 verdict.

### Gate 3 — **PASSED within the airtime budget**

MCS7 pinned, 15%/adapter synthetic drop, 40 ms I-frame deadline (profile 7,
`profiles/table.example.json`), gate-3 **recovery** latency (first-NACK →
RETRANSMIT arrival, Pass 10 estimator):

| load | delivered | samples | P50 | P90 | max |
|---|---|---|---|---|---|
| 500 pps / 4.8 Mb/s | 96.3% | 8 | ≤4 ms | ≤8 ms | 5 ms |
| 1500 pps / 14.4 Mb/s | 96.0% | 26 | ≤2 ms | ≤4 ms | 4 ms |
| 3000 pps / 29 Mb/s (~65% airtime) | 94.7% | 72 | ≤2 ms | ≤4 ms | 6 ms |

RTT *improves* with load — denser blocks mean more return-window
opportunities per second. All three are comfortably inside the 40 ms deadline.

**Past the ceiling** — 4500 pps / 44 Mb/s: delivered drops to **87.3%** and ARQ
**ceases entirely**: the craft's single RX adapter is deaf while its TX owns
airtime (heard 363 frames total in the window, LINK_REPORTs 46 s stale, 175
NACKs never heard, 0 resends). This is the §1 single-radio physics the
`airtime_budget_frac: 0.60` ceiling and §9.8 fail-safe demote exist to prevent
in normal operation — the selector was **pinned** for this test, so the demote
never fired. Not a bug; a deliberately-disabled guardrail during a controlled
overload test.

Note on sample counts: at 60 fps with small access units, §6.2-2 block
supersession retires most losses (advances the cursor, no NACK) before they
become NACK-eligible — gate-3 sample counts are inherently modest at any load.
The population that matters is the RETRANSMIT-recovered subset, not raw loss
count.

### Gate 4 — observables **live**, desk seeds **re-derived**; range validation remains

Return-window paced-hit-vs-miss counters (§7.2), real 60 fps EOB cadence:

| load | hit:miss | hit ratio |
|---|---|---|
| 500 pps | 614:16 | 97% |
| 1500 pps | 620:30 | 95% |
| 3000 pps | 641:86 | 88% |
| 4500 pps | 489:147 | 77% |

Quiet-gap ON vs OFF A/B @3000 pps: **ON** 94.7% delivered, 72/116 NACKs
recovered via ARQ; **OFF** 83.7% delivered, 40/198 recovered — but the OFF run
was confounded by a mid-run USB event (§2 incidental (c) below); the
pre-event segment of OFF alone reads ≈93.5%. Directionally ON wins but this
A/B needs a clean re-run before it's a real gate-4 number (§4.2).

### Incidental findings

1. **CCX `tx.report` saturates under load** — 100% of submitted reports
   returned @≤500 pps, dropping to 41% @3000 pps and 25% @4500 pps purely from
   report-channel contention, not a TX fault. The §9 TX-wedge detector must key
   on **zero reports over a window while `tx_submitted` keeps advancing**, not
   on any report-count deficit — a deficit alone is normal high-load behaviour
   (§4.5).
2. **Correlated loss appears only at high airtime** — at 65% airtime, joint
   loss was 4.9% vs 2.6% independence-product even though the *windowed* ρ
   read ≈0; this is collision-driven (shared-medium contention), a different
   mechanism from RF-fade correlation and not what gate 2 is measuring.
3. **Mid-run xHCI resets hit all three radios** — 5 s of total ground silence;
   the ground CU **hard-wedged** (RX counter frozen, needed a physical
   re-plug — the known RTL88x2 USB-wedge failure mode, `CLAUDE.md`). The §6.5
   stall watchdog + 5 s idle-teardown + relatch recovered cleanly at full rate
   once the adapter was back. This run also exposed and fixed a relatch bug in
   `tools/gate3_rtt.py` (now segment-aware, `6ca10fa`).

---

## 3. PR stack (merge order matters — do not reorder)

**RESOLVED 2026-07-11**: the full stack landed on `main` in order on the
operator's word — PR #9 (`fix/step11-rx-fcs-strip`), PR #10
(`impl/step11-gate3-rtt`, rebased + retargeted after #9), PR #11
(`docs/step11-wrapup`, rebased + retargeted after #10). What each carried,
for the record:

1. **`fix/step11-rx-fcs-strip`** — FCS-strip fix (§3.0) + §7.2
   window counters (gate-4 observables) + gate-2 observables/knob/tool
   (`uniq`/`diversity` stats, `air.rx_drop_permille`, `tools/gate2_rho.py`).
2. **`impl/step11-gate3-rtt`** —
   Pass 10 spec ruling (§17 gate-3 two-anchor estimator, §15.3 schema) +
   `nack_rtt_*`/`arq_rec_*` histogram instrumentation + `tools/gate3_rtt.py` +
   `tools/rtp_feed.py`.
3. **`docs/step11-wrapup`** — this document + `README.md` bench summary +
   `CLAUDE.md` bring-up notes.

The standing rule for any future stack: squash-merge each only on the
operator's explicit word (`CLAUDE.md` "The law"), and never merge a stacked
PR into its stack-mate's branch after that branch has been squash-merged
away — rebase onto `main` and retarget first.

---

## 4. Remaining work

Each item below is independently startable once its stated prerequisite PR
has merged. None require re-reading this whole document — jump to the
subsection.

### 4.1 Real-fade gate-2 verdict — **COMPLETED 2026-07-19**

The physical walk result is recorded in §2 and
`docs/mon-air-verification.md`: N=2 removed about 72% of pre-diversity loss,
10% GF(256) supplied 92% of explicit post-diversity source-symbol recovery,
and ARQ supplied the remaining 8%. The 37.1 s joint blackout establishes the
correlated tail that neither more same-channel parity nor ARQ can cross.

The immediate stationary follow-up is not another FEC-rate sweep. Run a long
N=2/MCS5/FEC10 soak for decoder/adapter stability, exercise one receiver at a
time, diagnose the `229b` return-TX failure, then verify the independent ARQ
cache on UDP/IP.

**Completed (Pass 48):** the 9,000-frame N=2 soak and explicit decoder EOS
passed; RX failover/failback passed with a required full CU monitor
reinitialization; `229b` silent return TX was isolated below AF_PACKET; and a
real independent monitor cache using UDP/IP repair reduced the matched 150‰
N=1-stress unrecoverable count from 534 to 119. Cache and vehicle ARQ remain
parallel, so RF resend load was unchanged; cache timing/ordering evidence is
the prerequisite to any RF cache proposal.

### 4.2 Gate-4 seed re-derivation

**RESULT 2026-07-11 (desk, Opus):** clean confound-free quiet-gap ON/OFF @3000 pps
— ON **95.56%** delivered vs OFF **94.81%** (+0.75 pts; resolves the §2 confounded
A/B), 91.4% paced-hit ratio (627/686). OFF has fresher LINK_REPORTs (13 vs 91 ms)
because ON delays returns into the §7.2 window — freshness↔reliability tradeoff,
muted at desk range (craft hears returns while TXing at RSSI −24). Guard/window
sweep (guard 300, 3000 pps): hit ratio PEAKS at the seed window (1000→89.3%,
**2000→91.4%**, 3000→89.6%); delivered% window-insensitive at desk range (~95.6%).
**Seeds 300/2000 STAND** — near-optimal among swept values; residual load-degradation
vs §2's 97%@500pps is single-radio contention, not knob mis-tuning. The desk result
does not close range-sensitive return-path or adaptive-loop stability; those remain
the real-RF gate-4 follow-up.

**Original rationale:** `guard_us` (seed 300), `return_window_us` (seed 2000),
and the §9.8 fail-safe seeds were wfb_ng-derived placeholders
(`docs/groundwork.md`); the quiet-gap A/B in §2 was confounded by a USB event
and needed a clean repeat.

**Desk method used:**
- Re-run several 3000 pps quiet-gap ON/OFF pairs on the x86 bench (no
  vehicle needed — this is desk-measurable) without a mid-run adapter fault;
  use the runner skeleton in §1.2.
- Sweep `guard_us`/`return_window_us` around the seeds, optimizing the §7.2
  paced-hit ratio against the gate-3 NACK service rate (a wider window catches
  more returns but eats into video airtime).
- While in this data, also check whether the §6.2-3 dwell-ceiling backstop
  seed needs re-deriving from the observed cross-adapter delivery-jitter
  histogram (PROTOCOL.md §17 knob table).

**Desk success criteria (met):** a clean ON vs OFF pair with no confound, plus
a `guard_us`/`return_window_us` pair that keeps the gate-4 hit ratio flat (not
degrading) up to the 3000 pps operating point identified in gate 3. This does
not replace the remaining range-sensitive real-RF validation above.

### 4.3 CSA on real TSF

**Why:** §11.2's `settle_s`/`verify_timeout_ms`/`dt_to_switch_ms` seeds were
derived on paper against wfb_ng precedent, not measured against this radio's
actual hardware TSF latch behaviour.

**Method:** ground-triggered CSA campaign via
`POST /api/v1/csa {"mhz":5805,"class":0}` (the old stdin trigger was removed
in Pass 16); measure actual retune + re-acquire time per class (0 = fast
intra-band `FastRetune`, seeded ~0.5–2.5 ms; 1 = cross-band full
`SetMonitorChannel`, seeded up to ~277 ms on 8812AU) against the
`dt_to_switch_ms` campaign-span assumption (§11.2).

**Success criteria:** measured retune time stays under the seeded
`dt_to_switch_ms` budget for its class with margin; if not, the class 0/1
seeds (150 ms / 500 ms) need raising.

### 4.4 Uplink HW-ACK hybrid A/B

**STATUS 2026-07-11: groundwork LANDED** (spec Pass 12 + implementation,
`impl/step11-wedge-hwack`): §3.0 pins the unicast QoS-Data return shape;
`return.unicast` (ground) sends NACK/LINK_REPORT unicast to the target's
last-heard SA with `unicast_sent`/`unicast_fallback` in the stats return
block; `air.ack_responder` (craft) arms the TX adapter's hardware ACK
responder with its own SA; the RX filter accepts both shapes (Retry bit
masked) so the halves deploy independently. **VALIDATED 2026-07-11 (desk, Opus):**
A/B at 3000 pps (15%/adapter drop, MCS7) — craft return-frame delivery **86.9%
(622/716) broadcast → 99.9% (716/717) unicast HW-ACK**; NACK service 84%→100%+;
LINK_REPORT age 97→89 ms; `unicast_sent`=717 with `unicast_fallback`=0 (the
per-originator SA latch is exact, never fell back); downlink video 95.47%→95.46%
(no regression). Past saturation (4500 pps): radio-layer return reception
**57.9%→98.5%** (retries punch through), but ARQ is dead in BOTH arms (0 resends;
the single-radio craft main loop is starved — only 4 of ~60 stats emits) — the §1
airtime-ceiling physics, not a return-reliability problem. Success criteria met.

**Why:** Pass 8 adopted a bench slot (not a redesign) to test whether arming
devourer's `SetAckResponder` on the craft — so ground→craft returns
(LINK_REPORT/NACK) become SIFS-timed hardware-ACKed unicast QoS-Data — beats
today's plain broadcast §7.2 opportunistic return. Downlink video stays
broadcast either way (Pass 8 ruled against A-MPDU/aggregation for the data
path).

**Method:** ground returns switch from broadcast to unicast QoS-Data addressed
to the craft SA; craft arms `SetAckResponder`. Measure LINK_REPORT delivery
ratio and NACK service rate at 3000 pps against the current baseline — today,
broadcast returns land the craft ~80% of LINK_REPORTs at 3000 pps and near-0%
past saturation (consistent with the 88% gate-4 hit ratio at 3000 pps and the
total collapse at 4500 pps in §2).

**Success criteria:** meaningful lift in delivery ratio at 3000 pps and/or a
non-zero delivery rate past today's saturation point, without regressing
downlink video airtime.

### 4.5 TX-wedge detector redesign

**STATUS 2026-07-11: detector LANDED** (spec Pass 11 + implementation,
`impl/step11-wedge-hwack`): §9.10 pins the zero-reports-over-window
trigger; `io/include/wblink/txwedge.h` implements it (knobs
`air.wedge_window_ms`/`air.wedge_min_submits`, seeds 1000/8), both mode
loops run it and surface `tx_wedged` per adapter + a stderr transition
line. **VALIDATED 2026-07-11 (desk, Opus):** SILENT through the 500–4500 pps
sweep — craft CCX report ratio falls 100%→98.7%→41.5%→24.5% with load, yet ZERO
windows hit the trigger (Δsub≥8 & Δrep==0) and `tx_wedged` stays False (the
`min_submits=8` gate filters idle boundary windows; the old deficit design would
have misfired at 24.5%). FIRES within one window of a real induced wedge:
`authorized=0` deauthorize of the craft CU at t≈12 s (USBDEVFS_RESET was too
gentle — auto-recovered) → submits keep advancing (+1500/win, `inject` increments
before the failing send) while reports flatline (+0), `tx_wedged`=True + the §9.10
stderr line, latched. All adapters recovered on teardown. Success criteria met.

**Why:** §2 incidental (1) — the current design's implicit assumption (a
report-count deficit signals a wedge) is wrong; reports legitimately drop to
25% of submitted at 4500 pps under normal contention.

**Method:** implement "**zero** CCX `tx.report` returns over a window while
`tx_submitted` keeps advancing" as the trigger. Validate by inducing the
known RTL88x2 USB wedge (unplug/replug cycling, or letting an xHCI reset hit
the adapter as happened incidentally in §2) and confirming the detector fires
only on true wedges, not high-load report contention.

**Success criteria:** detector stays silent through the 500–4500 pps sweep
(§2 gate-3 data, no wedge present) and fires within one window of an induced
wedge.

### 4.6 TX-power actuation range check

**RESULT 2026-07-11 (desk, Opus):** CU (Jaguar3) craft TX **CONFIRMED effective** —
uniform 5G power map at 1/10/19 dBm (MCS7 pinned): applied qdb −20/16/52, received
RSSI at ground −46.8/−37.5/−25.9 (AU path) and −42.8/−34.9/−25.0 (CU path). An 18 dB
command swing → ~18–21 dB received swing, near-linear ~0.9–1.16 dB/dB. **The §10.2
curve is usable as authored**; §2's "floor barely moved RSSI" was a single-point /
driver-default-baseline artifact, not a devourer defect. AU (Jaguar1) NOT
characterized: the rx-node never runs the §10 power-commit path on its uplink (no
`power:` apply line; AU transmits returns at devourer efuse default MCS7 base=40/FCC),
so sweeping the ground `power_map` is a no-op — needs the AU run as a craft-role TX,
or the vehicle. **Open §10 scope question raised:** should the ground return uplink
(role tx) be under power-curve control, or should a `power_map` on an rx-node uplink
be rejected? (Raised to operator, not silently changed.)

**Why:** §2 gate-2 incidental — a commanded −20 qdb power change (confirmed
applied in link stats) barely moved received RSSI on the CU under devourer.
This matters both for §10 power control generally and specifically for the
vehicle deploy in §4.1, where TX power may need to compensate for range.

**Method:** sweep `SetTxPowerOffsetQdb` across its range on a fixed adapter/
MCS, log applied qdb (from stats) against received RSSI on the far end;
characterize actual dB-per-qdb-step per chip (Jaguar1 vs Jaguar3) — the §10.2
curve assumes a roughly linear, effective mapping, which this data will
confirm or contradict.

**Success criteria:** either confirms the curve is usable as authored, or
produces a corrected per-chip qdb→dB slope to feed back into
`profiles/table.example.json` and the per-adapter power-map files.

### 4.7 Merge hygiene

**DONE 2026-07-11** — the §3 stack landed in order (see §3). The
rebase-and-retarget rule stands for any future stack.
