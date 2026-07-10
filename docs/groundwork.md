# Groundwork — calibration source of truth

Every tunable in PROTOCOL.md §13–14 is traced here to the production code it was
lifted from, with file:line citations and the adversarial corrections that the
grounding pass produced. When a spec constant changes, change it here too.

Sources:
- `waybeam_wfb_ng/link_controller.c` @ `382d453` (active production controller)
- `waybeam_venc/src/venc_api.c`, `star6e_controls.c`, `include/venc_config.h` @ `7441f76`
- rtl88x2eu / rtl88x2cu driver TX-power tables (§ TX power below)

## Adversarial corrections (memory was wrong; code is right)

1. **No composite quality score.** `link_controller` uses a priority-ordered
   **rule cascade** (`selector_update`, link_controller.c:3605–3827), not a
   weighted metric. RSSI, loss, and probe PER drive separate prioritized rules.
2. **Promote is a V+2 boundary probe** (link_controller.c:3761–3821), not an
   RSSI-margin check. TX probes 2 rungs up; promotes only if that probe's
   PER ≤ `probe_clean_milli`.
3. **venc has no `bitrate_enabled` flag.** That flag lives in *waybeam-hub*
   `mod_venc.c`, not venc. venc's HTTP API is last-writer-wins with zero
   arbitration. Single-authority is a *deployment* rule, not a venc feature.
4. **venc bitrate floor is 1000 kbps** (venc_config.h:36), not 512 or 2200. The
   2200 was wfb_ng's *policy* floor (`fec.bitrate_min`), not a venc limit.
5. **wfb_ng FEC is not loss-reactive.** k/n is a static REDUNDANCY_CURVE sized
   from frame rate (link_controller.c:763–788); Adaptive-n was removed. Loss is
   handled by MCS demote alone — which validates waybeam-link's no-FEC stance.

## §13 constants (link_controller.c)

| spec ref | constant | value | cite |
|---|---|---|---|
| 13.1 | RSSI/loss EWMA α | 0.3 | 5703–5704 |
| 13.1 | slope EWMA α | 0.5 | (SLOPE_EWMA_ALPHA) 3329 |
| 13.1 | reactive-demote loss | `demote_per_milli` 80‰ (8%) | 3697, 5728 |
| 13.1 | demote rate-limit | `down_cooldown_s` 0.2 s | 5708 |
| 13.1 | RSSI floor demote | `rssi_floor_dbm` −85 dBm | 3708, 5747 |
| 13.1 | RSSI fade demote | `rssi_fade_db_per_s` −10 dB/s, arm ≤ −65 dBm, 3 ticks | 3688, 5748–5749 |
| 13.1 | backpressure escape | `pressure_escape_s` 2.0 s | 3720, 5718 |
| 13.4 | probe promote | `probe_clean_milli` 20‰ (2%) | 3790, 5719 |
| 13.4 | probe pre-demote | `probe_fail_milli` 200‰ (20%) | 5727 |
| 13.4 | promote dwell | `promote_dwell_s` 0.5 s | 3809, 5731 |
| 13.4 | RSSI-margin (v0 promote) | `rssi_floor_hyst_db` 6.0 dB | 5753 |
| 13.5 | demote bitrate lead | `bitrate_lead_s` 0.5 s (DOWN-only) | 5654, 3108 |
| 13.5 | promote bitrate hold | `mcs_up_grace_s` 0.25 s | 5655, 3137 |
| 13.5 | post-change settle | `mcs_settle_s` 5.0 s | 5653 |
| 13.7 | soft reentry | `reentry_backoff_s` 5.0 s / `reentry_dwell_s` 2.0 s | 5754–5755 |
| 13.7 | hard flap-freeze | 3 re-demotes / 10 s window / 10 s hold | 5766–5768 |
| 13.3 | MCS rungs | video 0–5, probe rungs 6–7 | 5711–5712 |
| 13.2 | feedback cadence | rx_ant ~10 Hz | 7319 |

### rx_ant feedback fields (link_controller.c:7319–7349) — the LINK_REPORT superset
`uniq`, `data`, `lost`, `fec_recovered`, `diversity`, `adapters`, and an `rssi[]`
array of `{id, avg, max}` per adapter. Probe record (7294–7316):
`{mcs, accounted, lost, ts_ms}`, per-rung `per_milli = (lost*1000 + acc/2)/acc`.

## §13.6 venc actuation (venc_api.c / star6e_controls.c)

| item | value | cite |
|---|---|---|
| endpoint | `GET /api/v1/set?video0.bitrate=<kbps>` | venc_api.c:2312 |
| dual (Star6E ch1) | `GET /api/v1/dual/set?bitrate=<kbps>` (501 on Maruko) | venc_api.c:2837 |
| units | **kbps** (`bits = kbps*1024`) | star6e_controls.c:208 |
| range | 1000–200000 | venc_config.h:36–37 |
| default | 8192 kbps | venc_config.c:113 |
| mutability | `MUT_LIVE`, sub-ms, no reinit | venc_api.c:407 |
| persistence | writes `/etc/waybeam.json` **every** set → write-on-change only | venc_api.c:1852 |
| rc_mode | cbr/vbr/avbr/qvbr, **restart-required** (not live) | venc_config.h:98 |
| overshoot | `rc_compensate_kbps` scales down above FPS max | star6e_controls.c:190 |
| IDR | forced + rate-limited (~100 ms) after each change | star6e_controls.c:246 |
| low-bitrate trap | SVC-T preset oscillates 120↔24; mitigate `video0.resilience=racing` | (memory: mcs0_svct) |

## §14 TX power — devourer side (devourer @ `73f1cb4`)

**Public API** (`src/IRtlDevice.h:87–129`), units **quarter-dB (qdB)**:
- `SetTxPowerOffsetQdb(int qdb)` — the knob we use.
- `SetTxPowerIndexOverride(int idx)`, `SetTxPower(uint8_t)`, `ReApplyTxPower()`,
  `GetTxPowerCaps()/GetTxPowerState()`.
- HW step: **0.5 dB (2 qdB) on Jaguar1/2, 0.25 dB (1 qdB) on Jaguar3**
  (`src/TxPower.h:21`); the qdB API is quantized internally.

**Per-packet power — Jaguar2 ONLY** (radiotap `DBM_TX_POWER` → coarse 6-step LUT
`{0,-3,-7,-11,+3,+6} dB`, `FrameParserJaguar2.h:91`). **Jaguar1 and Jaguar3 have
NO per-packet power** — TXAGC is static until the next `SetMonitorChannel` /
`SetTxPowerOffsetQdb`.

**Our hardware maps to the no-per-packet families:** 8812AU = Jaguar1, 8812CU =
Jaguar3 (8822C-class). So:

> **Design consequence (the important one):** power is a **per-operating-point
> scalar**, not per-frame. Each §13.3 profile pins one MCS, so it carries one
> `tx_power_qdb` offset, applied via `SetTxPowerOffsetQdb()` at the same commit as
> the MCS change (§13.5). The driver's per-*rate* TXT table (many rates at once,
> needed because an associated STA sends a mix) collapses for us to one value per
> profile — the power for that profile's single MCS. Devourer applying it
> **globally is correct**, because only one MCS is injected per profile. No
> per-rate TXAGC register writes (Jaguar3's 0x3a00 diff table is efuse-populated
> and not publicly writable anyway).

Per-chip per-rate detail (for reference; not needed by our per-profile model):
- Jaguar1: direct per-rate TXAGC registers (`RadioManagementModule.cpp:2383`).
- Jaguar3: per-rate diff table @ `0x3a00`, efuse-populated, no public setter
  (`RadioManagementJaguar3.cpp:636`).
- No REG_PG-style userspace table-load on any chip.
- Injection rate is chosen per-packet from the radiotap header
  (`RtlJaguarDevice.cpp:869`); power is decoupled from it on J1/J3.

## §14 TX power — driver TXT curve (rtl88x2eu/cu @ v5.15.0.1 stock)

**Adversarial correction:** this is **NOT a recent driver innovation** — it's stock
Realtek `PHY_REG_PG.txt` power-by-rate, imported as-is (rtl88x2eu `c0177e9`,
rtl88x2cu `5f7054f`). What "landed recently" was the *wfb_ng decision* to let the
driver curve own power (`rtw_tx_pwr_by_rate=1` + a custom `PHY_REG_PG.txt`, WCMD
becomes a no-op). We reuse the **file format** as our per-MCS power data source.

**File:** `/lib/firmware/rtlwifi/PHY_REG_PG.txt` (or embedded). Parser
`phy_ParseBBPgParaFile` (hal_com_phycfg.c:4822–4992). Row format:

```
#[v2][Exact]#                                   header (version + type)
#[2.4G]A                                         section: band + rf-path
[1]  0xc20  0xffffffff  20.0  20.5  18.0  19.0   [TxNum] Reg Mask P1 P2 P3 P4
...
0xffff                                           terminator
```

- 4 fractional-**dBm** values per row (0.1 dBm resolution), each = one of the 4
  rates packed in that register.
- In-memory: `TxPwrByRate[band(2)][rf_path(4)][rate_idx(84)]` — **per-individual
  MCS**, not per-group (hal_com_phycfg.c:2246).
- **Rate index enum** (hal_com_phycfg.c:2636): `MGN_1M=0 … MGN_MCS0=12,
  MCS7=19, MCS31=43, VHT1SS_MCS0=44 … VHT4SS_MCS9=83`. Our HT rungs MCS0–7 →
  indices 12–19.
- **`rtw_tx_pwr_by_rate`** (os_intfs.c:823): `0=disable(global), 1=enable(load
  file), 2=depend-on-efuse`.

### How §14 reuses it (per-profile scalar, not a table load)

Devourer can't load a REG_PG table (per groundwork above), and our chips have no
per-packet power — but we don't need either. Each §13.3 profile pins one MCS, so:

> Author a waybeam-link power table **in the PHY_REG_PG.txt row format** (band /
> rf-path / MCS → value). The controller indexes it by the *committed profile's
> MCS* to get one target, and sets `SetTxPowerOffsetQdb()` at profile-commit
> (§13.5). Recommend authoring values as **relative qdB trims per MCS** (the curve
> *shape*: higher MCS needs more SNR → more power), which map directly onto
> devourer's offset knob; the efuse/regdomain still owns the absolute ceiling
> (regulatory-safe). Absolute-dBm targeting is possible but needs a
> `GetTxPowerCaps/State` baseline read first.
