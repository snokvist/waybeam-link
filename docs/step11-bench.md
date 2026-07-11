# Step 11 — hardware bring-up bench report + forward plan

Step 11 (`docs/build-order.md` §19 step 11): "Field bring-up; run gates 1–4. FEC
(§14) only if gate 2's P95 says so," plus the Pass 8 bench slots (§3.0 smoke,
uplink HW-ACK A/B, TX-wedge detector). Gates are defined in PROTOCOL.md §17;
`docs/review-log.md` Pass 9 (CSA MAC) and Pass 10 (gate-3 estimator) are the two
most recent spec rulings feeding this bench. All numbers below are
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

### Gate 2 — machinery **VALIDATED** (synthetic); real-fade verdict **desk-partial**

Synthetic, 60 s @ `rx_drop_permille=150` on both ground adapters: measured
15.3%/14.0% per-adapter, joint post-diversity **2.10%** vs independence-product
**2.07%**, Pearson **ρ ≈ −0.36** (n=59 windows) — decorrelated. Accounting
check: `diversity`/`uniq` = 0.744 vs theoretical 0.739.

Desk-partial real-fade: hand-fade at pinned MCS7 on one ground adapter →
**68% mean loss** on that adapter (P95 windows = 100% blackout), sibling
**0.3%**, joint **0.22%** vs **0.21%** independence — ρ ≈ **−0.07**, textbook
diversity behaviour.

**Why this is only desk-partial, not the gate-2 verdict:** craft-side fades
decorrelate at desk range because the ground receivers sit **35–45 dB over
sensitivity** (RSSI −30…−42 dBm measured) while a hand only costs 10–15 dB —
only the margin-poorest adapter ever fades. The correlated-fade tail that §14
cares about (the whole-link fade a banking turn produces) needs **real range**
to appear at all. The gate-2 verdict (no-FEC vs GF(256) RLC) is therefore
**deferred to the vehicle deploy** (§4.1).

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

### Gate 4 — observables **live**, seeds **not yet re-derived**

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

1. **`fix/step11-rx-fcs-strip`** (base `main`) — FCS-strip fix (§3.0) + §7.2
   window counters (gate-4 observables) + gate-2 observables/knob/tool
   (`uniq`/`diversity` stats, `air.rx_drop_permille`, `tools/gate2_rho.py`).
   **OPEN, awaiting operator merge word.** Do not merge without it.
2. **`impl/step11-gate3-rtt`** (base = `fix/step11-rx-fcs-strip`'s branch) —
   Pass 10 spec ruling (§17 gate-3 two-anchor estimator, §15.3 schema) +
   `nack_rtt_*`/`arq_rec_*` histogram instrumentation + `tools/gate3_rtt.py` +
   `tools/rtp_feed.py`. **Stacked**: once #1 squash-merges into `main`,
   **rebase this branch onto `main` and retarget its PR base to `main`** —
   never merge it into the (now-deleted) stack base branch.
3. **`docs/step11-wrapup`** (base = `impl/step11-gate3-rtt`'s branch, current
   branch) — this document + `README.md` bench summary + `CLAUDE.md`
   bring-up notes. Same rebase-and-retarget rule applies once #2 merges.

Squash-merge each only on the operator's explicit word (`CLAUDE.md` "The
law"). Never merge a stacked PR into its stack-mate's branch after that
branch has been squash-merged away — retarget to `main` first.

---

## 4. Remaining work

Each item below is independently startable once its stated prerequisite PR
has merged. None require re-reading this whole document — jump to the
subsection.

### 4.1 Real-fade gate-2 verdict (the premise-critical one)

**Why:** §2 above proved the gate-2 *machinery* correct but could not produce
a real correlated-fade sample — desk range gives the ground receivers too
much margin (35–45 dB) for anything but the single poorest adapter to fade.
The whole "diversity primary, no FEC" thesis (PROTOCOL.md §14, §1 invariants)
rests on this measurement.

**Method:**
- Deploy to the star6e bench vehicle, `root@192.168.1.201`, RTL8812EU
  (`DEVOURER_JAGUAR3_8822E` already compiled in). 8812EU RX+TX is field-proven
  on wfb_ng at 20 MHz — **stay at 20 MHz**, the 40 MHz bug is out of scope
  (§1 ruling-3 physics, `docs/build-order.md` "non-negotiable operational
  rules").
- Cross-build: `cmake --build --preset ssc338q`, deploy the binary + configs
  to `.201`.
- Generate real distance/orientation fades on the craft antenna (walk-out,
  body-blocking, banking-turn simulation) instead of a hand at the ground end.
- Analyze with `tools/gate2_rho.py`; the estimator is windowed joint-P95 vs
  single-adapter-P95 (PROTOCOL.md §17 gate 2).

**Success criteria / decision:** if the correlated-fade P95 tail keeps joint
loss ≪ single-adapter loss, §14 stays `fec_scheme: none`. If the tail is high,
FEC becomes justified — and per the Pass 3 ruling, the only real choice is
**GF(256) RLC**, never XOR (XOR only recovers what diversity already handles
and fails the burst that would motivate FEC in the first place).

### 4.2 Gate-4 seed re-derivation

**Why:** `guard_us` (seed 300), `return_window_us` (seed 2000), and the §9.8
fail-safe seeds are wfb_ng-derived placeholders (`docs/groundwork.md`) pending
bench re-derivation; the quiet-gap A/B in §2 is confounded by a USB event and
needs a clean repeat.

**Method:**
- Re-run several 3000 pps quiet-gap ON/OFF pairs on the x86 bench (no
  vehicle needed — this is desk-measurable) without a mid-run adapter fault;
  use the runner skeleton in §1.2.
- Sweep `guard_us`/`return_window_us` around the seeds, optimizing the §7.2
  paced-hit ratio against the gate-3 NACK service rate (a wider window catches
  more returns but eats into video airtime).
- While in this data, also check whether the §6.2-3 dwell-ceiling backstop
  seed needs re-deriving from the observed cross-adapter delivery-jitter
  histogram (PROTOCOL.md §17 knob table).

**Success criteria:** a clean ON vs OFF pair with no confound, plus a
`guard_us`/`return_window_us` pair that keeps the gate-4 hit ratio flat (not
degrading) up to the 3000 pps operating point identified in gate 3.

### 4.3 CSA on real TSF

**Why:** §11.2's `settle_s`/`verify_timeout_ms`/`dt_to_switch_ms` seeds were
derived on paper against wfb_ng precedent, not measured against this radio's
actual hardware TSF latch behaviour.

**Method:** ground-triggered CSA campaign via stdin `csa <mhz> [class]`
(PROTOCOL.md §11 build-order Pass 8 note); measure actual retune + re-acquire
time per class (0 = fast intra-band `FastRetune`, seeded ~0.5–2.5 ms; 1 =
cross-band full `SetMonitorChannel`, seeded up to ~277 ms on 8812AU) against
the `dt_to_switch_ms` campaign-span assumption (§11.2).

**Success criteria:** measured retune time stays under the seeded
`dt_to_switch_ms` budget for its class with margin; if not, the class 0/1
seeds (150 ms / 500 ms) need raising.

### 4.4 Uplink HW-ACK hybrid A/B

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

Land the PR stack in §3, in order, each only on the operator's explicit word.
Rebase-and-retarget each stacked branch onto `main` immediately after its
predecessor squash-merges — do not let a branch sit stacked on a
now-deleted base.
