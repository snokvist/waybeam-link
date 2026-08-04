# Pass 126/127/128 — §10.7 hardware campaign evidence (2026-08-02)

Topology (measured, not assumed):

| role | node | originator | notes |
|---|---|---|---|
| craft | 192.168.2.232 (SSC338Q) | 17 | `waybeam-link tx`, announced mode |
| ground | x86 dev host (192.168.2.242) | 9 | `waybeam-link rx`, holds the §3.5 report latch, owns `eu-uplink` role:"tx" |
| watcher | 192.168.2.199 (RK3566) | 10 | spectator, receive-only — deliberately untouched |

Channel 5805/HT20 throughout. Neither node configures `csa_psk` — the fleet
default — so every §3.16 packet here is keyed off the **announced token**
(Pass 127).

## Completed

| gate | result | evidence |
|---|---|---|
| P0a §10.6 regression | PASS — 8/8 placements identical, brackets 2 dB / 4 dB wide (no W9 zero-width) | `p0a/`, `baseline-craft-artifact.json` |
| P0b config load | PASS — both live configs load unchanged under the new binary | journal (not committed) |
| P0c uplink rate | PASS — craft sees 100% MCS0 before and after, `rx_mcs_unknown=0` | `p0c-craft-rxmcs-{BEFORE,AFTER}.txt` |
| D5 report epochs | PASS — 100.0% of the epoch span observed (was 99.6%), 0 backward, deltas +1 only | epoch dump (not committed) |
| D3 pairing gate | PASS — matching artifact applies (radio at 1.00 dBm = 4 qdb); a validly-signed artifact naming a different craft loads, reports `stale:true`, and applies nothing (radio stays 19 dBm) | `p5-artifact-good.json` |
| §10.7 end to end | PASS — 36 s, `done`, fp=5, artifact persisted with full pairing identity | journal (not committed) |
| P1 interlocks (partial) | PASS — malformed 400; scout-running 409; §10.5 latch 409 (and the latch moved the radio to 15 dBm and released to 19) | journal (not committed) |

## Defects found ON HARDWARE

1. **Pass 127** — §3.16 keyed off a configured `csa_psk` that the fleet never
   sets, so the craft emitted nothing and the ground refused everything.
   §10.7 was inoperable in announced mode and failed silently as "no fresh
   feedback".
2. **Pass 128** — the blackout fallback scored a flat 1000permille. Measured
   `received=198 emitted=199` against a 200-epoch target — clean — scored
   1000permille and failed the run. Now scored against the local span.
3. **Packaging** — `/etc/waybeam-link/calibration` absent ⇒ `artifact write
   FAILED` while the API still reported `state:"done"`, `fail_reason:null`.
   Persistence failure is invisible to the Hub menu row. **Open question for
   the operator** (see below).

## Open

- A failed artifact write leaves `state:"done"` with `fail_reason:null` and
  only `fingerprint:0` to betray it. For a step whose premise is "one-time,
  persisted on both sides", that reads as success. Needs an operator ruling:
  fail the run, or add an explicit `persisted` field.
- The cap wall fires at 20 qdb on this bench — a commanded +4 dB moves the
  craft's RSSI by under 1 dB, so the placement lands at `min_qdb`. Honest for
  ~1 m, but it means P8 (range A/B) is the gate that decides whether a
  calibrated placement is worth anything.
- Remaining: P1 (craft-calibrating + CSA-active interlocks, mid-run cancels),
  P2 guard-cost trace, P3 ×10, P4 blacked-out floor, P6 failure injection,
  P7 `start_both`, P8 range, P9 soak.

## P3 — ten consecutive §10.7 runs (Pass 130 sweep)

`tools/hw/p3_soak.sh`. The ten run captures and their summary were
ephemeral and are not committed — rerun the script to regenerate them.

| metric | result | gate |
|---|---|---|
| completed | **10/10** | 10/10 |
| placement spread | **0 qdb** (all 108) | <= 1 step (16 qdb) |
| duration | 57-66 s, mean 61 | — |
| placement loss | 5-15permille | <= loss_ok 15 |
| probe/verify dwells | 80 / 10 — exactly 8 probes + 1 verify per run | — |
| aborts, liveness losses, write failures | **0** | 0 |
| radio after each run | 27.00 dBm every time | restored |

**Caveat that matters more than the numbers.** Every run placed at `max_qdb`,
because at ~1 m the link is clean at full power and no loss wall exists to find.
So P3 demonstrates the mechanism is repeatable — same answer, same duration,
same dwell count, ten times — but it does **not** demonstrate repeatability of a
*wall-finding* placement, because the answer is pinned at the ceiling. A spread
of 0 is partly an artifact of that. P3 must be re-run at 2-10 m before the
"placement spread <= one step" gate means what it is meant to mean.

**Observation worth carrying to P8.** `placement_rssi` stepped -28 -> -18 -> -13
between runs 6 and 8 at unchanged commanded power, while the cumulative
`quality_rssi` stayed near -25. A 15 dB swing in the dwell mean with no
commanded change is either bench movement or receiver AGC, and it is the kind of
drift that decides whether a placement measured once stays valid. Worth watching
during the soak (P9).
