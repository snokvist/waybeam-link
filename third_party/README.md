# Vendored third-party trees

Plain-tree vendored copies (not submodules). **Never edit vendored code** —
portability/build issues are handled from the top-level CMakeLists (target
scoping, options, the pkg-config shim), not by patching. To update, re-copy a
newer tree and update the provenance below.

| tree | upstream | vendored commit | copied from |
|---|---|---|---|
| `devourer/` | https://github.com/OpenIPC/devourer | `9581f6a` (#400, **PR head — NOT yet merged**) | Plain-tree copy of upstream `master`; `reference/` and `.gitmodules` excluded. No local vendor delta. This includes the upstreamed RTL8822E MCS4+/48M+ TX, `0x41e8` RX-desense, eFEM/DPDT, calibration/runtime, and channel-switch fixes from `b5a6df7` (#238/#268), superseding the former `snokvist/devourer` cherry-pick pin; the per-unit EFUSE MAC identity (`IRtlDevice::GetPermanentMacAddress`, #383/#386) and the Jaguar3 EFUSE-walk append-order fix (#384 — flips 8822C `rfe_type` 0→3, a 7-RF-register delta; CU bench re-baselined, see `docs/findings.md` 2026-08-07); the RTL8733BU monitor RX / raw TX backend (#388) and its `FastRetune` port (#398, 345→55 ms per hop). The `5bf059a` bump adds the **RTL8733BU §10.5 TX-power actuator** (#399) — `GetTxPowerCaps().supported` flips false→true on that family, which is all `io/src/air_radio.cpp`'s `power_actuator_ok` reads, so the §10.5 announce and the §10.6/§10.7 calibration refusal self-enable with no consumer change. The `9581f6a` bump adds the **RTL8733BU USB TX aggregation port** (#400) — `send_packets` packs up to 3 `[txdesc][frame]` blocks per bulk-OUT URB, which is what `air.usb_tx_agg` + `RadioAir::inject_staged` exist to drive. Unlike the `5bf059a` bump this one is **not** self-enabling: `cfg.tx.usb_agg_max` defaults to 0 and the per-frame path stays byte-identical until a profile opts in. **This row is pinned to a PR head, not a merge commit** — #400 was still in review when the consumer side was written, and the operator accepted the pin rather than hold the consumer half. Re-syncing to the merged SHA is tracked as **issue #215**, which also says to DIFF the merged tree against `9581f6a` rather than assume it is unchanged: if the maintainer asked for edits during review, this copy is behind and the delta needs reading. Optional Kestrel 11ax and non-fleet Jaguar families are forced off by the top-level CMake; waybeam-link builds 8812AU/CU/EU and RTL8733BU (`WBLINK_DEVOURER_CHIPS`) and enables only the devourer features it explicitly configures. |
| `libusb-cmake/` | https://github.com/libusb/libusb-cmake | `c8477c1` (includes the `libusb/` sources) | same Android checkout |
| `nlohmann/` | https://github.com/nlohmann/json | 3.12.0 single-header | (io layer only — core stays zero-dep) |

Build integration (top-level `CMakeLists.txt`, `WBLINK_RADIO=ON`):
- libusb is built static from `libusb-cmake` (examples/tests off, udev off —
  we never hotplug; the SSC338Q cross toolchain has no libudev).
- devourer's `pkg_check_modules(libusb-1.0)` is satisfied by
  `cmake/pkgconf-libusb.sh` (answers only libusb-1.0: cflags → the vendored
  headers, libs → empty); the real static target `usb-1.0` is linked directly,
  devourer before usb-1.0 (static archive order).
- Chip options match the fleet exactly, selected by `WBLINK_DEVOURER_CHIPS`:
  `fleet` (default) = JAGUAR1 (8812AU) + JAGUAR3_8822C (8812CU) +
  JAGUAR3_8822E (8812EU); `au` / `eu` / `8733b` pin one family for a craft
  build; `all` adds RTL8733BU to the fleet trio for a ground that may meet any
  craft. 8814/Jaguar2/Kestrel/PCIe are off in every combination, and the
  top-level CMake **fails** if the vendored tree builds a family that was not
  asked for.
