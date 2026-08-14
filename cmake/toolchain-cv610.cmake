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
        get_filename_component(_wblink_tc_default
            "${CMAKE_CURRENT_LIST_DIR}/../../waybeam_venc/toolchain/toolchain.hisilicon-hi3516cv6xx"
            ABSOLUTE)
        set(WBLINK_CV610_TOOLCHAIN "${_wblink_tc_default}")
    endif()
endif()

set(_wblink_cv610_prefix
    "${WBLINK_CV610_TOOLCHAIN}/bin/arm-openipc-linux-musleabi")
if(NOT EXISTS "${_wblink_cv610_prefix}-g++")
    message(FATAL_ERROR
        "CV610 toolchain not found at '${WBLINK_CV610_TOOLCHAIN}'. "
        "Set -DWBLINK_CV610_TOOLCHAIN or env WBLINK_CV610_TOOLCHAIN to the "
        "OpenIPC toolchain.hisilicon-hi3516cv6xx root (see "
        "waybeam_venc/Makefile for the download recipe).")
endif()

set(CMAKE_C_COMPILER   "${_wblink_cv610_prefix}-gcc")
set(CMAKE_CXX_COMPILER "${_wblink_cv610_prefix}-g++")
set(CMAKE_STRIP        "${_wblink_cv610_prefix}-strip")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
