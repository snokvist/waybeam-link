# SPDX-License-Identifier: GPL-2.0-or-later
#
# Cross file for HiSilicon Hi3516CV610: 32-bit ARMv7 soft-float, OpenIPC
# arm-openipc-linux-musleabi toolchain.  This ABI deliberately differs from
# the SSC338Q hard-float target even though both processors are ARMv7.
#
# Toolchain root resolution order:
#   1. -DWBLINK_CV610_TOOLCHAIN=<path>
#   2. env WBLINK_CV610_TOOLCHAIN
#   3. sibling waybeam_venc checkout

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(WBLINK_CV610_TOOLCHAIN "" CACHE PATH
    "Root of the OpenIPC toolchain.hisilicon-hi3516cv6xx tree")
if(WBLINK_CV610_TOOLCHAIN STREQUAL "")
    if(NOT "$ENV{WBLINK_CV610_TOOLCHAIN}" STREQUAL "")
        set(WBLINK_CV610_TOOLCHAIN "$ENV{WBLINK_CV610_TOOLCHAIN}")
    else()
        # The upstream tarball unpacks to
        # arm-openipc-linux-musleabi_sdk-buildroot/, NOT to a
        # toolchain.hisilicon-hi3516cv6xx/ directory — measured 2026-08-14, so
        # the previous default named a path nothing produces.
        get_filename_component(_wblink_tc_default
            "${CMAKE_CURRENT_LIST_DIR}/../../waybeam_venc/toolchain/arm-openipc-linux-musleabi_sdk-buildroot"
            ABSOLUTE)
        set(WBLINK_CV610_TOOLCHAIN "${_wblink_tc_default}")
    endif()
endif()

set(_wblink_cv610_prefix
    "${WBLINK_CV610_TOOLCHAIN}/bin/arm-openipc-linux-musleabi")
if(NOT EXISTS "${_wblink_cv610_prefix}-g++")
    message(FATAL_ERROR
        "CV610 toolchain not found at '${WBLINK_CV610_TOOLCHAIN}'. "
        "Download "
        "https://github.com/openipc/firmware/releases/download/toolchain/"
        "toolchain.hisilicon-hi3516cv6xx.tgz and unpack it — it creates "
        "arm-openipc-linux-musleabi_sdk-buildroot/. Point "
        "-DWBLINK_CV610_TOOLCHAIN or env WBLINK_CV610_TOOLCHAIN at that "
        "directory. (waybeam_venc/Makefile has no CV610 recipe; it carries "
        "sigmastar-infinity6e and -infinity6c only.)")
endif()

set(CMAKE_C_COMPILER   "${_wblink_cv610_prefix}-gcc")
set(CMAKE_CXX_COMPILER "${_wblink_cv610_prefix}-g++")
set(CMAKE_STRIP        "${_wblink_cv610_prefix}-strip")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
