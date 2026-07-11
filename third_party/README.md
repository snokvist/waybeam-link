# Vendored third-party trees

Plain-tree vendored copies (not submodules). **Never edit vendored code** —
portability/build issues are handled from the top-level CMakeLists (target
scoping, options, the pkg-config shim), not by patching. To update, re-copy a
newer tree and update the provenance below.

| tree | upstream | vendored commit | copied from |
|---|---|---|---|
| `devourer/` | https://github.com/OpenIPC/devourer | `3025e2d` ("Packet aggregation and hardware ACKs in userspace", #239) | upstream clone; `reference/` (kernel-driver submodules) and `.gitmodules` excluded. All new TX capabilities (usb-agg, tx.report, A-MPDU, ACK responder) are off-by-default byte-identical; waybeam-link enables only `tx.report` (Pass 8) |
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
