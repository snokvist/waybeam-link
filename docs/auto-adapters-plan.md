# Auto-adapters: automatic radio adapter allocation (spec draft)

Status: **DRAFT — plan only, no Pass number yet.** The PROTOCOL.md amendment
below is proposed wording; per repo law it commits first (with its Pass
entry) in the implementing PR, once the operator rules on the open decisions
at the end.

Operator direction (2026-07-30): keep the config file for everything else —
node identity, streams, policy, channel, PSK, profile table — but make
**adapter allocation and init automatic** on the devourer (radio) backend.
Monitor-backend setup legitimately needs setup scripts (kernel modules,
monitor-mode init); the radio backend needs none of that, so it is the
natural zero-setup path. Full config-less startup was considered and
deliberately **deferred**: with auto-adapters a working config is ~10 lines,
which removes most of the demand for whole-config synthesis (originator,
streams, and channel have no sane universal defaults — see the 2026-07-30
config review in the coordination repo).

## 1. Config shape

`adapters` gains a second, mutually exclusive form. Array form is unchanged.

```json
"adapters": { "auto": { "channel": 5805 } }
```

Auto-object fields:

| field | default | meaning |
|---|---|---|
| `channel` | **required** | center MHz for every probed adapter (same slot as the array form's per-adapter `channel`) |
| `bw` | `20` | 20/40/80, all adapters |
| `max_adapters` | `0` = no cap | stop claiming after N devices (probe order after ranking) |
| `power_map` | `""` | applied to the elected TX adapter only (§10.2; rx-node restriction unchanged) |
| `max_power_qdb` | unset | §10.3 ceiling, TX adapter only |

Everything else in the config is untouched: `node.*`, `streams`, `policy.*`
(csa allowlist/home_chan/psk), `control`, `profile_table`, `stats`, `venc`.
`air.kind` must be `"radio"` — auto with `kernel-monitor` is a config error
(monitor init is owned by setup scripts by design; that split is the point
of the feature). Loopback/udp kinds ignore `adapters` as today.

## 2. Probe and election algorithm

At `RadioAir::create` time, when the auto form is present:

1. **Enumerate** the USB bus for VID `0x0bda` devices whose PID is in the
   **known-candidate set** (cribbed from the vendored examples' tables):
   unambiguous EU (`881a 881b 881c a81a`), unambiguous CU
   (`c812 c82c c82e`), Jaguar1-or-EU (`8812`), Jaguar1 (`0811 a811 b811
   8813`). Anything else — ZeroCD `1a2b`, BT interfaces, unknown Realtek —
   is **skipped, never opened**. (Today's array-form scan has no PID filter
   at all and would happily open a ZeroCD storage device; the filter is
   what makes probing safe on a dev host.)
2. **Open + claim** each candidate with the advisory `UsbDeviceLock` —
   lock-busy means another process owns it: skip silently (this is what
   lets two instances coexist on one host). Kernel-owned devices are
   detached (existing `claim_interface_then_reset` behavior — that is the
   zero-setup story: no rmmod, no scripts). The probe uses
   **`do_reset=false`** (or the `claim_interface_reset_reopen` wrapper):
   a reset can re-enumerate the device and hand it back to an auto-probing
   in-tree kernel driver mid-probe.
3. **Identify** each opened device via `CreateRtlDevice` +
   `GetAdapterCaps()`. This is mandatory post-open work: **RTL8812EU shares
   PID `0x8812` with RTL8812AU** — family is only knowable from the
   SYS_CFG2 register, not the descriptor.
4. **Rank**: EU > CU > AU, then any other supported family (see open
   decision D2). Tiebreak between same-family devices by **EFUSE MAC
   ascending** — never by USB bus path, which shuffles across re-plugs;
   the TX election must be boot-stable.
5. **Assign**: rank-0 → the one `role:"tx"` adapter (also RXes — the
   exactly-one-tx invariant is unchanged); every other claimed device →
   `role:"rx"` diversity. There is **no cache adapter role** — cache is a
   node-level function (`cache.store`, Ethernet repair) on a different
   host; extra local adapters are strictly more valuable as diversity RX.
6. **Declare**: log the full synthesized adapter set (name, chip family,
   MAC, usb path, role) at startup, and expose it in `GET /api/v1/info`
   exactly as the array form does. Synthesized names: `auto0-eu`,
   `auto1-cu`, … This keeps the feature on the right side of the standing
   ruling against *silent* inference (the spectator auto-detect rejection):
   auto is an explicitly configured mode whose outcome is fully declared,
   not a guess inside an operator-authored adapter list.
7. **Fail**: zero claimed devices → hard create error (same posture as
   "no adapters configured"). Devices that open but fail
   `CreateRtlDevice`/Init are released and logged, not fatal, as long as
   at least one adapter survives.

## 3. Interactions checked

- **§9.10 wedge watchdog / §11 CSA / §10 power**: all operate on the
  post-create adapter list; nothing downstream cares whether the list came
  from the array or the probe.
- **§10.5 TX-power override**: applies to the elected TX adapter as normal.
- **Spectator / cache nodes (radio)**: still blocked on the "RX-only radio
  operation" parity item — auto mode inherits that limit and always elects
  a TX adapter. Not made worse by this feature.
- **Config validation**: auto form + non-empty `power_map` on an rx *node*
  stays an error (existing rule).
- **`bus` pinning**: gone by definition in auto mode. Operators who need a
  specific physical adapter as TX (e.g. the one with the good antenna) keep
  the array form; that is the documented trade.

## 4. Open operator decisions (rulings needed before the Pass lands)

- **D1 — invocation**: is the auto object the only trigger, or should a
  fully absent `adapters` key with `air.kind:"radio"` also mean auto?
  Draft says: auto object only; absent stays an error (explicit beats
  implicit, and it preserves the current error for a genuinely forgotten
  adapters section).
- **D2 — non-priority families**: 8822BU/8821CU etc. probe successfully but
  are outside the EU>CU>AU ruling. Rank them below AU as diversity-only, or
  skip them entirely? Draft says: rank below AU, claim as rx.
- **D3 — probe reset posture**: `do_reset=false` (fastest, risks stale chip
  state) vs `claim_interface_reset_reopen` (slower, survives re-enumeration).
  Draft says: reopen wrapper for the elected set, no reset for probed-and-
  rejected devices.
- **D4 — `max_adapters` semantics**: cap applied after ranking (keep the
  best N) — draft assumes yes.

## 5. Verification plan (for the implementing PR)

- Unit: probe ranking/tiebreak logic factored pure (fed synthetic
  (family, MAC) lists), ctest-covered including the 0-device and
  tx-election-stability cases.
- Bench x86: 8812AU + 2×8812CU rig — auto must elect CU as tx (AU present),
  claim the rest as diversity, skip the machine's own non-candidate WiFi,
  and coexist with a second instance holding one adapter via UsbDeviceLock.
- Craft .232 (EU): single-adapter auto = tx elected, full frame-shm video
  session against an array-form ground, then a mixed EU+CU two-adapter run.
- Cross-check `ssc338q` build + the standing clean-rebuild rule.
