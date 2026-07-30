# SPDX-License-Identifier: GPL-2.0-or-later
#
# Cross file for Rockchip RK3566 ground stations: aarch64 glibc, built with
# the distro aarch64-linux-gnu cross toolchain (Ubuntu/Debian package
# gcc-aarch64-linux-gnu). Override the prefix with
# -DWBLINK_RK3566_PREFIX=<triple-> or env WBLINK_RK3566_PREFIX if a
# device-matched toolchain is preferred; watch the device glibc version when
# deploying a binary built against a newer host cross libc.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(WBLINK_RK3566_PREFIX "" CACHE STRING
    "Cross-compiler prefix, e.g. aarch64-linux-gnu-")
if(WBLINK_RK3566_PREFIX STREQUAL "")
    if(NOT "$ENV{WBLINK_RK3566_PREFIX}" STREQUAL "")
        set(WBLINK_RK3566_PREFIX "$ENV{WBLINK_RK3566_PREFIX}")
    else()
        set(WBLINK_RK3566_PREFIX "aarch64-linux-gnu-")
    endif()
endif()

find_program(_wblink_rk_gcc "${WBLINK_RK3566_PREFIX}gcc")
if(NOT _wblink_rk_gcc)
    message(FATAL_ERROR
        "aarch64 cross compiler '${WBLINK_RK3566_PREFIX}gcc' not found. "
        "Install gcc-aarch64-linux-gnu / g++-aarch64-linux-gnu or set "
        "-DWBLINK_RK3566_PREFIX / env WBLINK_RK3566_PREFIX.")
endif()

set(CMAKE_C_COMPILER   "${WBLINK_RK3566_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${WBLINK_RK3566_PREFIX}g++")
set(CMAKE_STRIP        "${WBLINK_RK3566_PREFIX}strip")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
