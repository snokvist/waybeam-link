# Upstream request: expose the EFUSE MAC as a per-unit adapter identity

Material for an issue/PR against OpenIPC **devourer**. Written to stand alone —
it describes a general need (deterministically re-detecting the same physical
adapter) and does not depend on how any particular consumer stores state.

## The need

A consumer that keeps per-adapter state — a calibration, a measured power
curve, anything measured against one specific dongle — must be able to answer
"is this the same physical adapter I measured last time?" across a re-plug, a
reboot, and a port change. Applying one unit's measurement to another silently
is the failure to avoid.

devourer currently offers no per-unit identifier. The two available keys both
fail:

- **USB bus path** (`1-1`, `3-1.2`) — identifies a *port*, not a device. It
  changes whenever the dongle moves, so the state is orphaned on any re-plug.
- **USB serial descriptor** — not unique. See the measurement below.

## Measured: the serial is a burned-in constant, the MAC is per-unit

Both values live in the same EFUSE. Dumped through the vendor kernel driver
(`/proc/net/rtl88x2{eu,cu}/<iface>/efuse_map`) on two different adapters of two
different chip families:

RTL8822EU — netdev `wlx84fc1450bcde`, MAC `84:fc:14:50:bc:de`:

```
0x150   DA 0B 1A A8 FF 7E 02 84   FC 14 50 BC DE 09 03 52
0x160   65 61 6C 74 65 6B 0E 03   38 30 32 2E 31 31 61 63
0x170   20 4E 49 43 08 03 31 32   33 34 35 36 FF FF FF FF
```

RTL8812CU — netdev `wlx40a5ef2f2308`, MAC `40:a5:ef:2f:23:08`:

```
0x150   DA 0B 12 C8 FF 7E 02 40   A5 EF 2F 23 08 09 03 52
0x160   65 61 6C 74 65 6B 0E 03   38 30 32 2E 31 31 61 63
0x170   20 4E 49 43 08 03 31 32   33 34 35 36 FF FF FF FF
```

Reading those:

| logical offset | contents | per-unit? |
|---|---|---|
| **0x157** | the 6-byte MAC — `84 FC 14 50 BC DE` / `40 A5 EF 2F 23 08` | **yes** |
| 0x15D | `09 03 "Realtek"` — USB manufacturer string descriptor | no |
| 0x166 | `0E 03 "802.11ac NIC"` — product string | no |
| 0x174 | `08 03 "123456"` — USB **serial** string descriptor | **no — identical on both** |

So the serial number every RTL88x2 dongle reports is a placeholder burned
identically into every unit, while the MAC at 0x157 is the per-unit value. This
is also exactly where Linux gets it: the vendor driver reads the EFUSE MAC and
sets the netdev address, and systemd/udev then derives the predictable
interface name `wlx<mac>` from it. That name is stable across re-plug and port
change *because* it is derived from the chip, not the bus.

A devourer consumer, having taken the adapter away from the kernel driver, has
no way to reach the same value.

## What devourer already has

Almost all of it:

- **Jaguar1 already implements this.** `EepromManager::GetMacAddress(uint8_t
  out[6])` (`jaguar1/EepromManager.cpp:1030`) reads the EFUSE MAC with per-chip
  offsets cited from upstream `hal_pg.h` (8812AU `0xD7`, 8814AU `0xD8`, 8821AU
  `0x107`), and rejects all-`0xFF` (unburnt) and all-`0x00` (not yet read). It
  is simply not reachable — `IRtlDevice` exposes no such method, so no consumer
  can call it.
- **Jaguar3 already decodes the logical EFUSE map.**
  `HalJaguar3::read_efuse_logical_map(map, len, upto)` takes an explicit `upto`
  bound, and `probe_efuse_map()` fills a caller buffer.
- **Kestrel already names the offset** — `EFUSE_USB_MAC_ADDR_8852B = 0x488`
  (`kestrel/MacRegAx.h:179`).

## What is missing

1. **No interface method.** `IRtlDevice` has no MAC accessor, so even Jaguar1's
   working implementation is unreachable.
2. **Jaguar3 caches too little.** `HalJaguar3::_efuse_cache` is `uint8_t[0x100]`
   and `RtlJaguar3Device.cpp:1602` uses `kMapLen = 0x100`. The MAC at 0x157 is
   past the end of that cache, so a read must either extend the cache or decode
   to a higher `upto` on demand. The decoder already accepts the bound.

## Proposed change

A default-false virtual on `IRtlDevice`, so unimplemented chips degrade
gracefully and no existing consumer changes behaviour:

```cpp
/* Per-unit hardware identity burned in the EFUSE, as the vendor drivers use
 * for the netdev MAC. Consumers that must re-detect the same physical adapter
 * across a re-plug need a stable key; the USB serial descriptor is not one
 * (it is a constant placeholder on RTL88x2 parts). Returns false when the
 * chip is unsupported or the EFUSE value is unburnt/unread. */
virtual bool GetPermanentMacAddress(uint8_t out[6]) { return false; }
```

Per family:

- **Jaguar1** — forward to the existing `EepromManager::GetMacAddress`. No new
  logic.
- **Jaguar3 (8822B/C/E)** — read 6 bytes at logical `0x157`, decoding the map to
  at least `0x15D`. Reuse Jaguar1's validity rule (reject all-`0xFF` and
  all-`0x00`).
- **Kestrel (8852B)** — read at the already-defined
  `EFUSE_USB_MAC_ADDR_8852B = 0x488`.
- **Jaguar2** — same shape; offset to be confirmed against its `hal_pg.h`.

## Caveats worth stating in the PR

- `0x157` is **measured** on the two adapters above, not read from a datasheet.
  Before merging it should be cross-checked against the vendor `hal_pg.h`
  constant for each chip (`EEPROM_MAC_ADDR_8822BU` and friends) — the value is
  believed to be the USB-variant offset, and the S/E/PCIe variants differ.
- The dumps come from the vendor kernel driver's logical map. devourer's own
  logical decode should produce the same layout, but a first implementation
  should be verified against a known adapter (compare with the `wlx<mac>`
  interface name the kernel gives the same dongle).
- A MAC is a hardware identity, not a secret, but it is a stable device
  identifier — consumers logging it should treat it the way they treat any
  other adapter identity.
