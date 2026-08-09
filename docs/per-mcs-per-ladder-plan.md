# Per-MCS PER ladder — hardware verification + Pass 119 plan

**Status:** plan only. No code in this branch beyond this document.
**Audience:** a Claude Code session with the bench rig attached.
**Predecessor:** PR #79 (Pass 118) — must be verified on hardware first (Part A).

---

## 0. Read this first

This repo has a **law** (`CLAUDE.md`). The parts that bite hardest on this work:

- **`PROTOCOL.md` v1 is LAW.** Where the spec has a gap, **STOP and ask the
  operator**. Do not pick silently. This plan deliberately leaves several
  questions open (§4) — they are operator rulings, not implementation details
  for you to resolve.
- **Spec amendments commit FIRST**, as their own commit; implementing code
  follows in the same PR as a separate commit.
- **Every spec ruling gets a numbered Pass entry in `docs/review-log.md`.**
  The last entry is Pass 118. Read Passes 116–118 before touching anything
  spec-adjacent.
- **Never commit to main.** Feature branch → PR → squash-merge, and merge only
  on the operator's explicit word.
- **Never edit vendored code under `third_party/`.** Several findings below are
  *in* vendored code. They get reported upstream or worked around from our
  side — never patched locally.

Rig gotchas that have already cost debugging time (full list in `CLAUDE.md`):
binary needs repo root as cwd; kill bench processes **only from a script file**
with `pkill -TERM` (typing it interactively kills your own shell, exit 144);
SIGTERM only, never SIGKILL; USB bus paths **shuffle** after any re-plug, so
re-check `lsusb -t`; unload kernel drivers before devourer runs
(`sudo rmmod 88x2cu rtw88_8812au`); RTL88x2 USB wedges need a physical re-plug,
not a driver reload.

---

## 1. Why this direction

§9.4 gates *promotion* on RSSI margin. `docs/step11-bench.md` §4.8 is a
recorded instance of that gate being wrong in a way that cost real link
quality: on 5220 MHz the selector kept climbing back to MCS5 at **−26 dBm**
RSSI — far above the rung's −73 dBm floor — into a genuine sustained 2–4%
loss. RSSI said there was enormous margin. There wasn't. The fix at the time
was to widen `demote_milli` 20 → 45, which tolerates the symptom rather than
addressing the gate.

The dataset this plan builds is precisely the one that gate is guessing at:

> **for each MCS, the probability a frame fails FCS, as a function of RSSI**

That is the modulation waterfall curve, measured on the actual hardware, on
the actual channel. It is what should replace §9.4's hardcoded per-rung RSSI
thresholds — and, unlike EVM or CFO, **it is obtainable identically on both
air backends** (§2), which is the operator's stated constraint.

Every frame the receiver demodulates gives one sample: `(MCS, RSSI, ok|badfcs)`.
No probe traffic, no wire change, no extra airtime.

**Scope discipline:** Pass 119 collects and displays this dataset. It does
**not** change adaptation. Rewiring §9.4 to consume it is a later pass with
its own ruling — and it should not even be designed until the curves have been
looked at.

---

## 2. Why bad-FCS and not EVM (the symmetry argument)

Checked field by field. `radiotap` is a standard; the Realtek RX descriptor is
not. Where the standard has no field, the kernel-monitor backend has no path.

| Signal | kernel-monitor | devourer | symmetric |
|---|---|---|---|
| MCS of received frame | radiotap bit 19 | `RxAtrib.data_rate` | ✅ (Pass 118) |
| RSSI | radiotap `DBM_ANTSIGNAL` (bit 5) | `RxAtrib.rssi[4]` | ✅ (already used) |
| **Frame failed FCS** | radiotap `FLAGS` bit `F_BADFCS` (0x40) | `RxAtrib.crc_err` | ✅ **by standard** |
| Bad PLCP | radiotap `RX_FLAGS` `F_RX_BADPLCP` | — | monitor only |
| Noise floor → SNR | radiotap `DBM_ANTNOISE` (bit 6) | `RxAtrib.snr[4]` | ⚠️ both exist, different quality |
| A-MPDU grouping | radiotap `AMPDU_STATUS` (bit 20) | `paggr` / `ppdu_cnt` | ✅ roughly |
| **EVM** | *no radiotap field exists* | `RxAtrib.evm[4]` | ❌ devourer only |
| **CFO** | *no radiotap field exists* | `RxAtrib.cfo_tail` | ❌ devourer only |
| FA / CCA / IGI, windowed fusion | — | `GetRxQuality()` | ❌ devourer only |

EVM is the physically superior metric — it is what actually determines whether
a constellation decodes — but it has no standard radiotap field, so carrying it
on the monitor path would need a vendor namespace and a driver patch we do not
control. It stays available as an *opportunistic enhancement on devourer nodes*
once the backend moves that way; the ladder must not depend on it.

`F_BADFCS` (`third_party/devourer/src/ieee80211_radiotap.h:88`) sits directly
beside the `F_FCS` bit `radiotap_parse` already reads, and our parser already
branches on FLAGS at bit 1 for `0x10`. Adding `0x40` is one line.

---

## Part A — verify Pass 118 on hardware

**Gate: none of Part B starts until Part A passes.** Pass 119 measures a rate
attribution that Pass 118 introduced; if that attribution is wrong, every
number Part B produces is wrong in the same direction and will look plausible.

Standard two-process rig from `docs/step11-bench.md` §1 (craft `role:"tx"`,
ground diversity pair), with `tools/rtp_feed.py` driving a real feed. The 1 Hz
HEARTBEAT / 2 Hz ANNOUNCE alone will populate counters but far too slowly to
read.

### A0. `ssc338q` cross-build — **not yet run at all**

The dev container had no SigmaStar toolchain, so PR #79 has never been
compiled for the vehicle target. Header-and-io only, no new dependencies, so
it is *expected* clean — but that is an expectation, not a result.

```
cmake --build --preset ssc338q     # must be green AND warning-free
```

Warning-free applies to **our** targets (`wblink_core`, `wblink_io`,
`waybeam-link`); vendored subdirectories build under their own flags.

### A1. The radiotap rate is actually honoured on the devourer path

**This is the whole point of Pass 118 and it is booby-trapped. Read this
before running it.**

Pass 118 kept `SetTxMode` committed in lockstep as a deliberate fallback. That
means if devourer were ignoring our radiotap MCS *entirely*, the link would
still air the correct rate, throughput would be normal, and every counter
would stay green. **"Nothing broke" is not evidence.**

Confirm positively:

1. Ground card → "Link & adapters" tab → the per-MCS panel
   (`tools/link_monitor.html`).
2. The populated bucket must match the craft's committed rung (the panel
   highlights `link.mcs`, which on an RX card is the craft's rung via §3.15).
3. Drive a rung change — a real §9.5 transition, or pin via
   `min_profile`/`max_profile` — and the mass **must move with it**, interval
   to interval.
4. Repeat on the **devourer** backend specifically, not only kernel-monitor.
   Kernel-monitor passing proves nothing about the path this pass changed.

If there is any doubt, the sharp version: build a throwaway diagnostic binary
that injects with a radiotap MCS deliberately disagreeing with the last
`SetTxMode` commit, and confirm the **radiotap** value is what the receiver
reports. That is a scratch build, never a config knob, never committed.

### A2. `rx_mcs_unknown` stays at zero

Against a conforming peer, on both backends. Non-zero means the rate did not
resolve — on kernel-monitor a missing or `!HAVE_MCS` radiotap field, on
devourer a rate code outside `DESC_RATEMCS0..+7`. Either is a real finding
about the driver, not noise. Investigate before proceeding.

### A3. Buckets sum to `rx`

On every adapter, every snapshot: `sum(rx_mcs[0..7]) + rx_mcs_unknown == rx`.
Cheap invariant; catches a miscount at the accept boundary.

### A4. Backward interop, both directions

Pass-118 craft ↔ pre-Pass-118 ground, and the reverse. Video unaffected in
both. The old node reports no `rx_mcs` and the dashboard renders no panel for
it (dashboard-tested, but worth seeing once on real hardware).

### A5. No airtime regression

The injection prefix grew 10 → 13 bytes, host-side only, never aired. §14.2
estimates and measured throughput must be unchanged. Movement here means
buffer sizing is wrong, not the radio.

### A6. Jaguar1 unicast return still ACKs

Not caused by Pass 118, but it rebuilt the unicast prefix. Re-confirm
`unicast_sent` vs `unicast_fallback` behaves as before. See §5 for the open
Jaguar1 `BMC` question this may interact with.

### A7. Report back

Record results in `docs/step11-bench.md` §4.9 (which currently reads
"CODE COMPLETE, BENCH PENDING"), and post the outcome on PR #79. If A1 fails,
**stop** — that is a Pass 118 defect, not a Pass 119 input.

---

## Part B — Pass 119: capability probe, then dataset

### B1. Establish whether each backend can deliver bad-FCS frames *at all*

This is a measurement, not an implementation. **Do it before designing
anything.** Three separate gates, none of them open today, and the
fleet-default chip is the uncertain one.

**B1a — kernel-monitor — DROPPED (Pass 164).** The backend and
`scripts/mon-up.sh` are deleted, so this leg has no subject. It asked whether
`iw dev "$IF" set monitor otherbss fcsfail` would make mac80211 deliver
FCS-failed frames upward; the question dies with the backend. Only the
devourer leg remains.

**B1b — devourer Jaguar1/Jaguar2.** `rx.keep_corrupted`
(`third_party/devourer/src/DeviceConfig.h:85`) sets `RCR_ACRC32|RCR_AICV` and
defaults **false**. Our `RadioAirCfg` does not expose it at all, so it is
currently unreachable from config. Exposing it is ours to do (`io/`), not a
vendored edit.

**B1c — devourer Jaguar3 (8812CU/EU — the fleet default). Comment and code
disagree; resolve empirically.** `third_party/devourer/src/jaguar3/HalJaguar3.cpp:463`
documents the monitor RX config as *"Accept all frames incl. CRC/ICV errors …
RCR bits: AAP/APM/AM/AB/ACF/AICV/ACRC32"*, but the value written at `:484` is
`0xF410400F`, whose low half `0x400F` sets bits 0–3 and 14 — **not** BIT8
(`ACRC32`) or BIT9 (`AICV`) per `hal_com_reg.h:1114-1115`. So the comment
claims corrupted frames are kept and the register value says they are not.
Measure which is true. **Do not patch vendored code** — if the comment is
wrong, that is an upstream report.

**B1d — our own code drops them regardless.** Even with every RCR gate open,
`io/src/air_radio.cpp:235-237` discards `crc_err || icv_err` frames
unconditionally as the very first thing `on_packet` does. This is the concrete
change point on the devourer side.

**Exit criterion for B1:** a table of backend × chip × "does a bad-FCS frame
reach userspace, yes/no", with the `iw` flags and config that were needed. If
the answer is "monitor yes, Jaguar3 no", the symmetry constraint is not met by
this approach and you should **stop and report** rather than build half of it.

### B2. Quantify the identification bias — the load-bearing unknown

A bad-FCS frame is, by definition, one whose bytes are not trustworthy. We
identify our frames by reading bytes: §3.0 filters on `SA[0..1] == 56:42` and
optionally `SA[2] == net_id`. **Those bytes may themselves be corrupted.**

So bad-FCS counting is biased toward *lightly* damaged frames and undercounts
badly damaged ones — which is precisely the tail that matters near a rung's
cliff, the exact regime the ladder exists to characterise.

Worse on the monitor path: `attach_bpf_filter` (`io/src/air_mon.cpp:74`)
applies the SA match **in kernel**, so a frame with a corrupted SA is dropped
before userspace ever sees it and cannot even be counted as "unattributable".

**Note `air.rx_drop_permille` cannot be used for this.** It is a synthetic
*post-reception* drop in our own code (`io/src/air_radio.cpp:253`,
`io/src/air_mon.cpp:428`), applied to frames that already passed FCS. It
manufactures loss, not FCS errors. Real degradation is required: attenuators,
distance, or pinning a rung above what the link budget supports (the last is
cheapest on a bench — pin `min_profile == max_profile` at MCS7 and walk the
craft away until errors appear).

Measure: against a link degraded into genuine FCS errors, compare
counted bad-FCS frames against the independent loss the §3.1 sequence gaps
report. The shortfall is the bias. **Quantify it before anyone builds a
control loop on the ratio.**

If the bias turns out large and RSSI-dependent — plausible, since worse RSSI
means more corruption means more SA misses — then the PER curve is distorted
in exactly the axis being measured, and that is a finding worth reporting
rather than a number to paper over. Options to raise with the operator at that
point: count *all* bad-FCS frames on the channel without SA matching (no bias,
but includes foreign traffic), or accept and document the bias, or use both as
bracketing bounds.

### B3. The dataset

Only once B1 and B2 are answered. The target is a 2-D histogram per adapter:

```
    MCS (0..7)  ×  RSSI bucket  ×  {ok, badfcs}
```

RSSI bucketing is an open question (§4). A first cut of 5 dB bins from −90 to
−20 dBm gives 14 bins → 8 × 14 × 2 = 224 counters per adapter. That is a lot of
JSON at `stats.hz`; see §4 for the emission-shape ruling needed.

Also confirm early: **is RSSI valid on a bad-FCS frame?** The PHY status is
computed at reception, independent of the FCS outcome, so it should be — but on
devourer the phy-status is only attached to the first subframe of an A-MPDU,
and on monitor it depends on the driver populating `DBM_ANTSIGNAL` for failed
frames. If RSSI is absent or garbage on failures, the whole 2-D idea collapses
to a 1-D per-MCS PER and the plan needs re-scoping. **Check this in B1, not
in B3.**

### B4. Visualisation

Extend the panel added in PR #79 (`tools/link_monitor.html`,
`mcsHistogram()`). The natural presentation is a small waterfall: PER on the
y-axis, RSSI on the x-axis, one line per MCS. Keep the existing "advisory,
no control path reads this" framing — it will still be true in Pass 119.

Note the existing quirk documented in that function: `renderCard` also runs on
tab clicks and tooltip unlocks, re-rendering the *same* snapshot, so any
per-interval delta must be held (`st.mcsDeltas` pattern) or the panel silently
reverts to lifetime on interaction.

Dashboard changes are testable dry — `tests/link_monitor_test.py`, no rig
needed.

---

## 4. Open questions — operator rulings, do not guess

Per `CLAUDE.md`: raise these, do not resolve them.

1. **Does a bad-FCS frame count toward `rx`?** It should not — `rx` is
   §3.0-accepted frames — but that breaks the Pass 118 invariant
   `sum(rx_mcs) + rx_mcs_unknown == rx` unless the new counters are a separate
   family. Schema shape needed.

2. **The tri-state.** A `badfcs` count of zero means either "clean link" or
   "this backend/chip is not delivering bad-FCS frames". Pass 117 hit exactly
   this with the report holder and solved it with an explicit
   `report_latch_known` beside the value rather than overloading 0. A
   capability flag is almost certainly right here too — name and semantics are
   the operator's call. Getting this wrong puts a reassuring flat-zero PER
   ladder on screen over a node that is simply not measuring.

3. **SA matching on untrustworthy bytes** (§B2). The central design question.
   Filter and accept the bias, count everything on-channel, or emit both?

4. **RSSI bucket width and range**, and whether §15.3 emits the full 2-D grid,
   a per-MCS summary (PER + mean RSSI + n), or a rolling window with only
   occupied cells. Bandwidth at `stats.hz` is the constraint.

5. **Does this eventually replace §9.4's RSSI-margin promote gate?** Not a
   Pass 119 decision — but the answer shapes whether the data needs to be
   good enough to *steer* on (tight, per-rung, low-latency) or only good enough
   to *look at* (loose, aggregated, offline). Worth asking before choosing the
   emission shape in (4).

---

## 5. Carried-over findings (recorded, not scheduled)

From `docs/step11-bench.md` §4.10 — do not lose these:

- **Jaguar1 `BMC` hardcode.** Jaguar3 derives descriptor `BMC` from addr1's
  I/G bit (`RtlJaguar3Device.cpp:1981`); Jaguar1 hardcodes it to 1
  (`RtlJaguarDevice.cpp:1156`) outside the NDPA path. If that reading holds,
  the Pass-12 unicast return is descriptor-marked broadcast on 8812AU, which
  would undercut its hardware ACK. Bench question → upstream report. Never a
  local edit under `third_party/`.

- **Upstream NOACK (OpenIPC/devourer#334).** In our vendored pin the radiotap
  NOACK flag is decorative — parsed into an unused local on Jaguar1, not
  parsed at all on Jaguar2/3. What suppresses ACK/retry on our broadcast
  frames is the broadcast DA. If #334 lands, verify it is **gated** on the
  radiotap flag as its reviewer asked: applied unconditionally it zeroes
  `RTS_DATA_RTY_LMT` on all frames and kills the Pass-12 unicast return's
  descriptor-driven retries.

- **Per-path `snr[4]` / `evm[4]` / `cfo_tail`** are parsed by devourer and
  discarded by us; `GetRxQuality()` already fuses them with a passive noise
  floor (`rssi_dbm − snr_db`). Devourer-only, so out of scope while symmetry
  is the constraint — but this is the first thing to revisit if the backend
  moves fully to devourer.

---

## 6. Findings (2026-08-01 bench session — Part A + B1 executed)

Rig: craft `.232` (SSC338Q, 8812EU) ↔ ground `.242` (x86, EU `0bda:a81a`
bus 1-1 + CU `0bda:c812` bus 5-1), ch 5805/HT20, 1 dBm floor. Full Part A
numbers recorded in `docs/step11-bench.md` §4.9.

### Part A — PASS (A6 not run)

A0 clean; A1 confirmed **positively** including the sharp variant (throwaway
diag build, `SetTxMode` pinned MCS0 vs radiotap rung 7 → receiver bucketed
~22k @7, zero @0 — radiotap wins on Jaguar3); A2 zero everywhere; A3 held on
every snapshot; A4 both directions; A5 no movement. A6 skipped: the operating
ground is MonAir, no Jaguar1 in config. Pass 118 is hardware-verified.

### Part B1 — capability table, and the exit criterion FIRED

| Path | chip / driver | flag accepted | bad-FCS delivered |
|---|---|---|---|
| kernel-monitor | CU `rtl88x2cu` | `otherbss fcsfail` ✅, `fcsfail` ✅ (rc 0) | **NO** — 0 in 126k frames across 5805 / 5180 / 2412 (busy 2.4 incl.) |
| kernel-monitor | EU `rtl88x2eu` | ✅ (rc 0) | inconclusive — EU heard only ~40 ambient 2.4 frames in 30 s (weak 2.4 RX); 0 bad-FCS observed |
| devourer | CU — enumerates **Jaguar3** | `rx.keep_corrupted` set in scratch build | **NO** — 0 corrupted in ~22k ambient frames |
| devourer | Jaguar1 / Jaguar2 | — | untested: no such hardware on the desk (AU unplugged) |

Control observations: the FLAGS radiotap field is present on 100% of
delivered frames with `fcs_at_end` set, so the `F_BADFCS` bit path exists
and the zero is a real zero, not a parsing miss.

**B1c resolved (comment vs register): the register wins.** `keep_corrupted`
is plumbed in `jaguar1/RadioManagementModule.cpp` and
`jaguar2/HalJaguar2.cpp:2573` only; Jaguar3 ignores the flag entirely and
`monitor_rx_cfg` writes RCR `0xF410400F | (1<<28)` — ACRC32 (BIT8) and AICV
(BIT9) both clear, contradicting the block comment at
`jaguar3/HalJaguar3.cpp:463`. Not patched locally (vendored); this is an
upstream report: (a) the comment misdocuments the RCR value, (b)
`rx.keep_corrupted` silently no-ops on Jaguar3.

**Consequence: STOP per the B1 exit criterion.** Neither backend can deliver
bad-FCS frames on the fleet-default chips today. The symmetry argument in §2
holds *by standard* but fails *by implementation* — the standard defines
`F_BADFCS`, and neither the out-of-tree kernel drivers nor devourer Jaguar3
deliver the frames that would carry it. B2/B3/B4 not started. RSSI-validity
on bad-FCS frames (§B3 early check) is unobservable for the same reason.

### What unblocks Pass 119, if the operator wants it

1. **Upstream devourer change** (smallest lever): make Jaguar3
   `monitor_rx_cfg` honour `rx.keep_corrupted` (set BIT8|BIT9 when
   requested). Whether the 8822C-family firmware then actually forwards
   CRC-failed MPDUs to the host is the follow-up empirical question — the
   RCR bits are necessary, not proven sufficient.
2. Out-of-tree kernel-driver `fcsfail` support would open the monitor path,
   but that tree is not ours either.
3. Alternatively, re-scope the numerator: the sequence-gap inference Pass 118
   originally recorded (with its known promoted-burst blind spot), or accept
   a devourer-only ladder once EVM/`snr[4]` become available after a full
   backend move — both are §4-style operator decisions.

### §4 rulings — now needed only if an unblock path is chosen

The five §4 questions stand, but none is actionable until bad-FCS delivery
exists on at least one symmetric path. Raised to the operator with this
report rather than resolved here.
