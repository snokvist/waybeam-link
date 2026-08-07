# Devourer re-vendor review — MCS4+/txpower + new-feature survey (2026-07-24)

**Investigation only. No spec ruling is made here, so there is no `docs/review-log.md`
Pass entry attached.** Every item in "Open decisions" below is deferred to an
operator ruling; when one is made it commits first as a spec amendment + numbered
Pass per the repo law. This document exists to capture the state of the vendored
driver so the next person does not re-derive it.

> **Devourer is a moving target.** It is under active upstream development. The
> feature surface below is a snapshot of the currently vendored commit; the next
> time we bump the vendor, re-read this document *and* re-run the survey — new or
> changed capabilities (and new fixes/regressions) are expected on every bump, and
> may supersede what is written here.

## Vendoring state

| Bump | Date | Devourer commit | What it brought |
|---|---|---|---|
| `4f20e27` (#14) | 2026-07-11 | `3025e2d` + fix `a353a9c` | The 8822E/8812EU MCS4+ TX fix (see below) |
| `73d83a4` | 2026-07-22 | **`7a6541f`** | Large jump: whole new subsystems (channel migration, FHSS/keyed hopping, FW channel-switch offload, HE/Wi-Fi-6 Kestrel, scheduled MAC, time distribution) |
| (#111) | 2026-08-02 | `800c3c8` | #378/#379 carrier-sense defaults, #380 `DEVOURER_ACK_TIMEOUT_US`, #377 MCS retry ladder |
| Pass 154 PR | 2026-08-07 | **`5a5dd62`** | **#383** `IRtlDevice::GetPermanentMacAddress` (per-unit EFUSE MAC — the §10.6 Pass 154 identity primitive); **#384** Jaguar3 EFUSE-walk append-order fix — **flips 8822C `rfe_type` 0→3**, a 7-RF-register delta (radioa `0x52/0x63/0xb3/0xb6/0xdd`, radiob `0x52/0x63`; BB/AGC/cal-init untouched, measured offline via `scratchpad/rfe_delta.cpp`) so CU RF now matches the vendor kernel driver — CU bench re-baselined, `docs/findings.md` 2026-08-07; **#386** wires the MAC accessor on Jaguar2/Kestrel too (compiled out of our builds — fleet is Jaguar1 + Jaguar3; noted because it moots carrying a D3 `0x107` fallback ourselves) |

The MCS4+ fix has been in-tree since #14. The `7a6541f` bump is what pulled in the
broad feature surface this document surveys.

## 1. MCS4+ / txpower — the fix is vendored, but not yet re-verified at 5805

Background: the vendored devourer historically **could not transmit 16‑QAM+ (MCS4+)
on the 8822E family in UNII‑3 — including 5805 MHz, the §4.1 gate channel** — even
with correct per-channel TXAGC (`docs/mon-air-verification.md:8-16`). That defect
forced the pivot to the kernel-monitor (`MonAir`) backend, and the whole deployment
rig now runs the kernel `8812eu`/`rtl88x2eu` driver for the video path, not devourer
RadioAir (`docs/verification-hardware.md`).

The RTL8812EU is **Jaguar3, `ChipVariant::C8822E`, chip-id `0x17`**
(`third_party/devourer/src/jaguar3/ChipVariant.h:12`) — the exact silicon the
limitation was measured on. Both claimed fixes are present in the vendored source:

- **TXAGC re-apply as the last bring-up step** —
  `third_party/devourer/src/jaguar3/RtlJaguar3Device.cpp:860-865`. The FW
  power-mode/coex H2Cs were rewriting the OFDM TXAGC refs (`0x18e8`/`0x41e8`)
  wholesale, so construction-time TX power was inert and the PA rode into
  compression, shredding 16/64‑QAM while BPSK/QPSK (MCS0–3) leaked through.
- **DPDT / eFEM pin-mux fix** — `RtlJaguar3Device.cpp:600`.

On our side, RadioAir passes MCS (`SetTxMode`, HT/20) and power
(`SetTxPowerOffsetQdb`) straight through with **no clamp** (`io/src/air_radio.cpp:564,573`);
the only numeric cap in code is the opt-in `max_power_qdb` PA-safety ceiling
(`core/include/wblink/power.h:43`, explicitly *not* regulatory). The craft's MCS
ceiling is **config-only** — `policy.select.max_profile` (README pins it to `0`),
not a source constant.

### Verify-current-state probe plan (must run on the bench — not reproducible in CI)

Both the vehicle (SSC338Q, 8812EU `0bda:a81a`) and the x86 host (8812EU `0bda:a81a`
+ 8812CU `0bda:c812`) carry the EU. The x86 host is self-contained for a 2-adapter
A/B; the vehicle can only drive a real link.

**Correction that matters:** the devourer probe scripts default to **ch36 (5180 MHz,
UNII‑1)**. The original block was at **5805 MHz (ch161, UNII‑3)** — the §4.1 gate
channel. A clean ch36 run is necessary but **not sufficient**; the close-out
measurement is at `CH=161`.

x86 host (`third_party/devourer`):

```sh
cmake -S . -B build && cmake --build build -j

# A. sanity at the script's home channel — confirms the fix is live in the binary
sudo -v && tests/eu_mcs7_txagc_fix.sh
#    expect: MCS0 all cells; MCS7@default ~0 (PA physics unchanged);
#    MCS7@TX_PWR=39/28 clean delivery; stderr "TXAGC refs re-applied post-coex"

# B. THE test — same probe at the gate channel (closes mon-air-verification.md:8)
sudo -v && CH=161 tests/eu_mcs7_txagc_fix.sh

# C. power characterization at 5805
sudo -v && CH=161 TX_PID=0xa81a GROUND_PID=0xc812 tests/per_mcs_power_ceiling.sh 0xa81a 0xc812
#    -> mcs->safe-power table (law: ceiling non-increasing as MCS rises; the E's
#       step is non-linear / step_measured=false, so this table matters)

# D. kernel ground-truth cross-check (needs vendor rtl88x2eu_ohd + uhubctl)
sudo -v && CH=161 tests/eu_kernel_mcs_probe.sh
```

`tests/eu_matched_power_evm.sh` (devourer `TX_PWR_OFFSET_QDB=-44` vs kernel
`iw ... fixed 500`, the "txpower 500" question) **hardcodes `CH=36` at line 13** —
edit to `161` for a gate-channel EVM comparison; it does not read a `CH` env var.

Vehicle (8812EU only, no self-A/B): real link — vehicle EU as devourer TX
(`streamtx`, `CH=161`, MCS7) → x86 ground RX, measure delivery/EVM at the ground.
Then the product-level A/B: a RadioAir craft config with `max_profile` lifted 0→7,
vehicle EU → x86 diversity, compared against the current kernel-monitor `MonAir`
baseline (`docs/mon-air-verification.md:44-54` gives the 99.97% MCS7 numbers to beat).

Bench caveats: near-field saturation on the x86 two-adapter A/B (back power off with
`DEVOURER_TX_PWR=12` or separate antennas — EVM, not RSSI, is the saturation tell);
sustained 5 GHz Jaguar3 TX needs the coex runtime thread (RadioAir keeps it); 20 MHz
only (8812EU 40 MHz bug).

## 2. New-feature survey — what the `7a6541f` bump exposes, vs waybeam's current use

Reachability note: almost all of this is only usable if waybeam runs the **devourer
RadioAir** path (craft or ground). Today the deployment is kernel-monitor `MonAir`
end-to-end, so most items are conditional on the §1 verification. The exceptions
(LDPC, thermal, per-packet radiotap) are partly testable on the kernel-monitor path
too.

RadioAir's current devourer surface (`io/src/air_radio.cpp`): `claim_interface_then_reset`,
`CreateRtlDevice`, `InitWrite`, `StartRxLoop`/`StopRxLoop`/`Stop`, `send_packet`,
`SetTxMode`, `SetTxPowerOffsetQdb`, `FastRetune`, `SetMonitorChannel`,
`ReApplyTxPower`, `ReadTsf`, `SetAckResponder`, plus RX `Packet` metadata
(`rssi[]`, `tsfl`, `crc_err`/`icv_err`) and the `tx.report`/CCX event harvest.

### Tier A — quick wins, no spec conflict, testable soon

- **LDPC coding — the standout omission.** Devourer measures **≈ +3 dB at the
  10%-delivery crossing (MCS7/20)** (`tests/ldpc_waterfall.sh`); every 11ac chip we
  run decodes HT/VHT LDPC (only the 8821A, unused, is broken). **Not mentioned
  anywhere** in the waybeam docs. It is a per-packet radiotap flag (`/LDPC`) and the
  kernel-monitor path can carry it too — so A/B it on the *current* deployment,
  independent of §1. Cheapest range on offer for a range-limited broadcast link.
- **Thermal polling** (`DEVOURER_THERMAL_POLL_MS`) — cheap craft-side early
  TX-degradation warning (RF 0x42 meter; raw 0..63 units ≈ 1.5–2 °C each; rising
  delta precedes EVM collapse). Not mentioned in waybeam.
- **LinkHealth classification** (`DEVOURER_LINKHEALTH`) — SATURATED / INTERFERENCE /
  WEAK / … with the insight that **EVM, not SNR, is the saturation tell**. The
  §9.4 selector drives on RSSI margin; this distinguishes "near-field saturated →
  back *off*" from "weak → add power," which RSSI alone cannot. Candidate selector input.

### Tier B — high value, each needs an operator ruling + Pass entry

- **DIS_CCA (+1.5–2.2×)** (`SetCcaMode` / `DEVOURER_DIS_CCA`). Devourer defaults
  carrier-sense *off* on its streamtx FPV downlink. Waybeam *relies* on CCA for
  opportunistic returns (`docs/findings-pass3.md:150`). Resolution is asymmetric and
  worth a ruling: **craft broadcast downlink → DIS_CCA on; ground uplink/returns →
  CCA on.** Airtime win on the one-way leg without touching the shared return path.
  Touches §9.
- **Per-packet TX power is now possible on the craft.** §10.1 rules "no per-packet
  TX power in devourer" — measured on the **8812AU (Jaguar1)**. The craft is now
  **8812EU (Jaguar3)**, which has programmable BB offset banks (`TXPWR_OFSET_TYPE`,
  ~1 dB/step, zero USB cost per frame). The §10.1 premise no longer holds on the EU;
  this unlocks per-frame UEP power (boost I-frame-class, trim P-frame-class) paired
  with the importance classes. Touches §10.
- **`FASTRETUNE_FW=2` cross-band offload** — **~2–2.6 ms vs ~90 ms** full retune
  cross-band, ~3× less host CPU (matters on the SSC338Q). We already use host
  `FastRetune` for class-0 hops. Touches §11.

### Tier C — architecture decision, not a knob

- **Adaptive channel migration subsystem** (`src/chanmig/`, `chanscout`,
  `ChannelScore`, `MigGate`; upstream issues #278–#280). Devourer now ships a
  complete authenticated ground-proposes / drone-commits whole-link channel-move
  system: passive scout survey on a second adapter, explainable scoring, automation
  gate (cooldown/probation/kill-switch), SipHash-MAC'd wire codec — video PSDUs
  untouched. This **substantially overlaps waybeam's own §11.6 + `docs/scout-design.md`**,
  which the docs describe as "mostly orchestration over existing primitives."
  Decision to make: **converge waybeam §11 onto devourer's chanmig, or keep the
  waybeam wire protocol and cherry-pick only the scout survey + scoring engine?**
  Avoids building the same thing twice. Touches §11 — needs a design ruling + Pass.

### Tier D — reconsider-given-new-hardware / building blocks

- **A-MPDU** (+30% goodput) — rejected Pass 8 partly because Jaguar1/8812AU collapses
  under the deep feed. The craft is now Jaguar3, so that premise changed; still, the
  0.8–3 ms aggregate-fill latency adder argues against it for a latency-first link.
  Premise noted, no reversal recommended.
- **Fused-FEC salvage** (`DEVOURER_RX_KEEP_CORRUPTED`) — RadioAir currently *drops*
  crc/icv-err frames (`io/src/air_radio.cpp:218`). The Gate-2 walk showed **232
  partially-observed unrecoverable frames**; keeping corrupt bodies for an outer-code
  salvage could shave that tail. Different mechanism from our GF(256) RLC — a building
  block to note, not adopt wholesale.

### Tier E — out of scope, plus one housekeeping action

- **Out of scope for a broadcast downlink:** the whole Kestrel/Wi-Fi-6 stack (HE
  trigger UL + TWT, ER SU / DCM extended range), AP mode, scheduled MAC, multi-AP
  cellular, LA capture. All new in this bump, all for connection-oriented or AX
  hardware we do not run.
- **Housekeeping (independent of everything above):** the `7a6541f` bump pulled in the
  full Kestrel `halbb`/`halrf` firmware + PHY-table tree. Per devourer's CMake,
  per-chip groups default **ON**; turning off the unused ones (`DEVOURER_KESTREL*`,
  and `DEVOURER_8814` if unused) drops those blobs and shrinks the binary — confirm
  our CMake disables them so the SSC338Q image does not balloon after the bump.

## Open decisions (deferred — each needs an operator ruling + a numbered Pass)

1. DIS_CCA asymmetry (craft downlink off / ground returns on) — §9.
2. Per-packet TX power on the EU craft for UEP — §10 (the §10.1 "no per-packet"
   premise is 8812AU-specific and no longer holds on Jaguar3).
3. `FASTRETUNE_FW` cross-band offload for follow-me — §11.
4. chanmig convergence vs. keep-ours-and-cherry-pick — §11.
5. Whether the §1 5805 verification result reopens the devourer-RadioAir craft path
   (lifting the `max_profile` pin) — §10, and would supersede the `MonAir` pivot
   rationale in `docs/mon-air-verification.md`.

## Suggested order

1. LDPC A/B (both backends) — biggest cheap win, no spec entanglement.
2. Confirm the CMake chip-group trim (Tier E housekeeping) from the vendor bump.
3. Thermal + LinkHealth wiring into stats/selector.
4. Gated on the §1 5805 result: the Tier B rulings and the Tier C architecture call,
   each as its own review-log Pass.
