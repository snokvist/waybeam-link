# Vendored third-party trees

Plain-tree vendored copies (not submodules). **Never edit vendored code** —
portability/build issues are handled from the top-level CMakeLists (target
scoping, options, the pkg-config shim), not by patching. To update, re-copy a
newer tree and update the provenance below.

| tree | upstream | vendored commit | copied from |
|---|---|---|---|
| `devourer/` | https://github.com/snokvist/devourer (fork of OpenIPC/devourer) | `a353a9c` = OpenIPC `3025e2d` (#239) + our 8822E TX fixes | Pinned at the same upstream base (`3025e2d`) as before; the ONLY delta vs upstream is 3 cherry-picked fix commits (see below). `reference/` and `.gitmodules` excluded. Upstream TX capabilities (usb-agg, tx.report, A-MPDU, ACK responder) stay off-by-default; waybeam-link enables only `tx.report` (Pass 8). **Local fix delta** (snokvist/devourer `master`, pending upstream via OpenIPC/devourer#238): (1) 8822E MCS4+/64-QAM TX — program the OFDM TXAGC ref upper field `0x18e8`/`0x41e8` at end of `InitWrite` (`apply_ofdm_ref_upper_8822e`, gated by `DEVOURER_8822E_OFDM_REF_FIX_OFF`); (2) 8822E RF-reg-`0x0` FON routing (`Halrf8822e::rf_write`); (3) `DEVOURER_FORCE_PATH_B_REF` + `BB_OVERRIDE`/`BB_DUMP` debug knobs. Deliberately pinned at `3025e2d` (not upstream HEAD) so the craft's driver changes by exactly the fix for the field gate — rebasing onto upstream is a separate, deliberate follow-up. |
| `libusb-cmake/` | https://github.com/libusb/libusb-cmake | `c8477c1` (includes the `libusb/` sources) | same Android checkout |
| `nlohmann/` | https://github.com/nlohmann/json | 3.12.0 single-header | (io layer only — core stays zero-dep) |

Build integration (top-level `CMakeLists.txt`, `WBLINK_RADIO=ON`):
- libusb is built static from `libusb-cmake` (examples/tests off, udev off —
  we never hotplug; the SSC338Q cross toolchain has no libudev).
- devourer's `pkg_check_modules(libusb-1.0)` is satisfied by
  `cmake/pkgconf-libusb.sh` (answers only libusb-1.0: cflags → the vendored
  headers, libs → empty); the real static target `usb-1.0` is linked directly,
  devourer before usb-1.0 (static archive order).
- Chip options match the fleet exactly: JAGUAR1 (8812AU) +
  JAGUAR3_8822C (8812CU) + JAGUAR3_8822E (8812EU); 8814/Jaguar2/PCIe off.
