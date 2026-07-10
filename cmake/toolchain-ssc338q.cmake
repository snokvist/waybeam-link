# SPDX-License-Identifier: GPL-2.0-or-later
#
# Cross file for SigmaStar SSC338Q / Infinity6E: 32-bit ARMv7-A hard-float,
# OpenIPC arm-openipc-linux-gnueabihf toolchain (g++ 13.3) — the same tarball
# waybeam_venc vendors under toolchain/toolchain.sigmastar-infinity6e.
#
# Toolchain root resolution order:
#   1. -DWBLINK_SSC338Q_TOOLCHAIN=<path>
#   2. env WBLINK_SSC338Q_TOOLCHAIN
#   3. sibling checkout: ../waybeam-coordination/waybeam_venc/toolchain/...

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(WBLINK_SSC338Q_TOOLCHAIN "" CACHE PATH
    "Root of the OpenIPC toolchain.sigmastar-infinity6e tree")
if(WBLINK_SSC338Q_TOOLCHAIN STREQUAL "")
    if(NOT "$ENV{WBLINK_SSC338Q_TOOLCHAIN}" STREQUAL "")
        set(WBLINK_SSC338Q_TOOLCHAIN "$ENV{WBLINK_SSC338Q_TOOLCHAIN}")
    else()
        get_filename_component(_wblink_tc_default
            "${CMAKE_CURRENT_LIST_DIR}/../../waybeam-coordination/waybeam_venc/toolchain/toolchain.sigmastar-infinity6e"
            ABSOLUTE)
        set(WBLINK_SSC338Q_TOOLCHAIN "${_wblink_tc_default}")
    endif()
endif()

if(NOT EXISTS "${WBLINK_SSC338Q_TOOLCHAIN}/bin/arm-openipc-linux-gnueabihf-g++")
    message(FATAL_ERROR
        "SSC338Q toolchain not found at '${WBLINK_SSC338Q_TOOLCHAIN}'. "
        "Set -DWBLINK_SSC338Q_TOOLCHAIN or env WBLINK_SSC338Q_TOOLCHAIN to the "
        "OpenIPC toolchain.sigmastar-infinity6e root (see waybeam_venc/Makefile "
        "for the download recipe).")
endif()

set(CMAKE_C_COMPILER   "${WBLINK_SSC338Q_TOOLCHAIN}/bin/arm-openipc-linux-gnueabihf-gcc")
set(CMAKE_CXX_COMPILER "${WBLINK_SSC338Q_TOOLCHAIN}/bin/arm-openipc-linux-gnueabihf-g++")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
