#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# pkg-config shim for the vendored devourer build (same pattern as
# Waybeam-android :wifi). devourer's CMakeLists hard-requires
# pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0); in this tree
# libusb is the vendored static libusb-cmake target, so this shim answers ONLY
# libusb-1.0 queries: --cflags points at the vendored headers, --libs is empty
# (the real target usb-1.0 is linked directly by the top-level CMakeLists).
# Anything else fails, so an unexpected dependency query is loud, not silent.
HERE="$(cd "$(dirname "$0")/.." && pwd)"
INC="$HERE/third_party/libusb-cmake/libusb/libusb"
# Second include dir: maps <libusb-1.0/libusb.h> (devourer's desktop-Linux
# spelling) onto the vendored plain libusb.h via a forwarding header.
INC2="$HERE/cmake/libusb-include"

# find_package(PkgConfig) probes the executable itself first.
for a in "$@"; do
    [ "$a" = "--version" ] && { echo "1.8.1"; exit 0; }
done

want_libusb=0
for a in "$@"; do
    [ "$a" = "libusb-1.0" ] && want_libusb=1
done
[ $want_libusb -eq 1 ] || exit 1

# CMake's pkg_check_modules uses the split query forms (--cflags-only-I,
# --libs-only-l, ...), not plain --cflags/--libs — answer both.
for a in "$@"; do
    case "$a" in
        --exists) exit 0 ;;
        --modversion) echo "1.0.30"; exit 0 ;;
        --cflags|--cflags-only-I) echo "-I$INC -I$INC2"; exit 0 ;;
        --cflags-only-other) echo ""; exit 0 ;;
        --libs|--libs-only-l|--libs-only-L|--libs-only-other) echo ""; exit 0 ;;
    esac
done
exit 0
