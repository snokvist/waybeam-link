# SPDX-License-Identifier: GPL-2.0-or-later
#
# Cross file for Android arm64 (bionic) — compile-only verification that
# wblink_core + wblink_io still build for the Waybeam-android consumer.
# See docs/library-extraction-plan.md B12: ssc338q already proves ARMv7 and
# 32-bit, but it is glibc, so bionic was untested until this preset existed.
#
# API 26 and arm64-v8a match Waybeam-android's :wifi module exactly
# (wifi/build.gradle.kts: minSdk 26, abiFilters "arm64-v8a").
#
# NDK root resolution order (mirrors toolchain-ssc338q.cmake):
#   1. -DWBLINK_ANDROID_NDK=<path>
#   2. env WBLINK_ANDROID_NDK, then env ANDROID_NDK_HOME / ANDROID_NDK_ROOT
#
# This file is a THIN WRAPPER around the NDK's own android.toolchain.cmake,
# and must stay one. The NDK file is what defines ANDROID, and both vendored
# trees branch on it: third_party/devourer/CMakeLists.txt links `log`, and
# third_party/libusb-cmake/CMakeLists.txt links `android log`. A hand-rolled
# CMAKE_SYSTEM_NAME=Android file would silently fall into libusb's Linux
# udev/netlink branch and fail to link.

set(WBLINK_ANDROID_NDK "" CACHE PATH "Root of the Android NDK")
if(WBLINK_ANDROID_NDK STREQUAL "")
    foreach(_wblink_ndk_env WBLINK_ANDROID_NDK ANDROID_NDK_HOME ANDROID_NDK_ROOT)
        if(NOT "$ENV{${_wblink_ndk_env}}" STREQUAL "")
            # FORCE into the cache: an env-resolved path held only as a normal
            # variable makes the build dir single-use, since any re-configure
            # from a shell without the env var loses it.
            set(WBLINK_ANDROID_NDK "$ENV{${_wblink_ndk_env}}" CACHE PATH
                "Root of the Android NDK" FORCE)
            break()
        endif()
    endforeach()
endif()

if(NOT EXISTS "${WBLINK_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    message(FATAL_ERROR
        "Android NDK not found at '${WBLINK_ANDROID_NDK}'. Set "
        "-DWBLINK_ANDROID_NDK or env WBLINK_ANDROID_NDK (ANDROID_NDK_HOME / "
        "ANDROID_NDK_ROOT are also honoured) to an NDK root — e.g. "
        "~/Android/Sdk/ndk/26.3.11579264, the revision Waybeam-android pins.")
endif()

# The gate exists to match the consumer, so say something when it does not.
# First-non-empty-env-var-wins means a stale ANDROID_NDK_HOME can silently
# build against a different NDK than the one Waybeam-android pins.
set(WBLINK_ANDROID_NDK_PIN "26.3.11579264")
if(EXISTS "${WBLINK_ANDROID_NDK}/source.properties")
    file(STRINGS "${WBLINK_ANDROID_NDK}/source.properties" _wblink_ndk_rev
         REGEX "^Pkg.Revision *=")
    if(NOT _wblink_ndk_rev MATCHES "${WBLINK_ANDROID_NDK_PIN}")
        message(WARNING
            "NDK at '${WBLINK_ANDROID_NDK}' is ${_wblink_ndk_rev}, not the "
            "${WBLINK_ANDROID_NDK_PIN} that Waybeam-android's :wifi pins. The "
            "bionic gate still runs, but parity with the consumer is not "
            "guaranteed.")
    endif()
endif()

set(ANDROID_ABI arm64-v8a)
set(ANDROID_PLATFORM android-26)
# c++_static: this preset builds static archives with no APK to carry a shared
# STL, and the consumer links wblink_io into its own .so.
set(ANDROID_STL c++_static)

include("${WBLINK_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
