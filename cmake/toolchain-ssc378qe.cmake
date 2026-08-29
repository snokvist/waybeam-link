# SPDX-License-Identifier: GPL-2.0-or-later
#
# Cross file for SigmaStar SSC378QE / Infinity6C ("maruko"): 32-bit hard-float
# ARM, OpenIPC arm-openipc-linux-musleabihf toolchain — the same tarball
# waybeam_venc vendors under toolchain/toolchain.sigmastar-infinity6c.
#
# This is a third SigmaStar ABI, distinct from both siblings, and the archives
# built here must never be mixed with theirs:
#   - Infinity6E (toolchain-ssc338q.cmake) is hard-float but glibc
#     (arm-openipc-linux-gnueabihf).
#   - CV610 (toolchain-cv610.cmake) is musl but SOFT-float
#     (arm-openipc-linux-musleabi).
# Infinity6C is the musl + hard-float corner.
#
# The core is a Cortex-A35 — ARMv8-A executing AArch32 — even though the
# device's /proc/cpuinfo reports "ARMv7 Processor ... architecture: 7". Read
# `CPU part` (0xd04 = A35) rather than that string; Infinity6E is 0xc07 (A7),
# the genuinely older core. The toolchain's ARMv8 codegen (stlex/ldaex) is
# therefore correct here and would fault on Infinity6E.
#
# Toolchain root resolution order:
#   1. -DWBLINK_SSC378QE_TOOLCHAIN=<path>
#   2. env WBLINK_SSC378QE_TOOLCHAIN
#   3. sibling waybeam_venc checkout

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(WBLINK_SSC378QE_TOOLCHAIN "" CACHE PATH
    "Root of the OpenIPC toolchain.sigmastar-infinity6c tree")
if(WBLINK_SSC378QE_TOOLCHAIN STREQUAL "")
    if(NOT "$ENV{WBLINK_SSC378QE_TOOLCHAIN}" STREQUAL "")
        set(WBLINK_SSC378QE_TOOLCHAIN "$ENV{WBLINK_SSC378QE_TOOLCHAIN}")
    else()
        get_filename_component(_wblink_tc_default
            "${CMAKE_CURRENT_LIST_DIR}/../../waybeam_venc/toolchain/toolchain.sigmastar-infinity6c"
            ABSOLUTE)
        set(WBLINK_SSC378QE_TOOLCHAIN "${_wblink_tc_default}")
    endif()
endif()

set(_wblink_ssc378qe_prefix
    "${WBLINK_SSC378QE_TOOLCHAIN}/bin/arm-openipc-linux-musleabihf")
if(NOT EXISTS "${_wblink_ssc378qe_prefix}-g++")
    message(FATAL_ERROR
        "SSC378QE toolchain not found at '${WBLINK_SSC378QE_TOOLCHAIN}'. "
        "Fetch it with waybeam_venc's recipe (its Makefile carries a "
        "sigmastar-infinity6c target) and point -DWBLINK_SSC378QE_TOOLCHAIN or "
        "env WBLINK_SSC378QE_TOOLCHAIN at the resulting "
        "toolchain.sigmastar-infinity6c directory.")
endif()

set(CMAKE_C_COMPILER   "${_wblink_ssc378qe_prefix}-gcc")
set(CMAKE_CXX_COMPILER "${_wblink_ssc378qe_prefix}-g++")
# The Infinity6C target roots on the same ~5.7 MB overlay the SSC338Q one does
# (measured on .233: 5.7 MB total, 2.6 MB free), so strip at build time.
set(CMAKE_STRIP        "${_wblink_ssc378qe_prefix}-strip")

# The Infinity6C rootfs ships libgcc_s.so.1 but NO libstdc++ (measured on .233
# 2026-08-29: /lib/libgcc_s.so.1 present, /usr/lib/libstdc++* absent), and the
# overlay has no room to add one. Static-link the C++ runtime for the same
# reason CV610 does. libgcc is pulled in statically alongside it so the binary
# does not depend on that one .so continuing to exist for some other package's
# sake.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
