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
Kernel-monitor leg: MOOT (ruling #120). **Open:** per-unit coverage is one
unit per die — Pass 139's lesson wants a second unit of at least the CU/EU
parts on the rig before probing is enabled fleet-wide (fail-closed default
stands).

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
