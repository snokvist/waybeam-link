# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22)
project(waybeam-link CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WBLINK_BUILD_TESTS "Build unit tests" ON)
option(WBLINK_SANITIZE "Build with ASan+UBSan (host dev builds)" OFF)
option(WBLINK_FUZZ "Build the libFuzzer wire fuzz target (clang only)" OFF)
option(WBLINK_RADIO "Build the devourer radio air backend (§3.0/§7)" ON)

# Our warning set is applied per-target (WBLINK_WARNINGS below), NOT via
# directory-level add_compile_options — the vendored third_party subdirectories
# (devourer, libusb) must build with their own flags, not ours.
set(WBLINK_WARNINGS -Wall -Wextra -Wshadow -Wconversion)
if(WBLINK_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# --- vendored radio stack (third_party/README.md): static libusb via
# libusb-cmake, then devourer. devourer hard-requires
# pkg_check_modules(libusb-1.0); the shim answers that query with the vendored
# headers and empty libs, and the real static target usb-1.0 is linked
# directly (devourer before usb-1.0 — static archive order).
if(WBLINK_RADIO)
    set(BUILD_SHARED_LIBS        OFF CACHE BOOL "" FORCE)
    set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LIBUSB_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
    set(LIBUSB_BUILD_TESTING     OFF CACHE BOOL "" FORCE)
    set(LIBUSB_INSTALL_TARGETS   OFF CACHE BOOL "" FORCE)
    # No hotplug enumeration needed and the SSC338Q cross toolchain has no
    # libudev — the netlink fallback covers both hosts.
    set(LIBUSB_ENABLE_UDEV       OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/libusb-cmake
                     ${CMAKE_BINARY_DIR}/libusb EXCLUDE_FROM_ALL)

    set(PKG_CONFIG_EXECUTABLE
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pkgconf-libusb.sh
        CACHE FILEPATH "" FORCE)
    # Chips = the fleet, exactly: 8812AU (Jaguar1), 8812CU (Jaguar3 rtl8822c),
    # 8812EU (Jaguar3 rtl8822e). Everything else off — drops its fw blobs.
    set(DEVOURER_JAGUAR1       ON  CACHE BOOL "" FORCE)
    set(DEVOURER_8814          OFF CACHE BOOL "" FORCE)
    set(DEVOURER_JAGUAR2_8822B OFF CACHE BOOL "" FORCE)
    set(DEVOURER_JAGUAR2_8821C OFF CACHE BOOL "" FORCE)
    set(DEVOURER_JAGUAR3_8822C ON  CACHE BOOL "" FORCE)
    set(DEVOURER_JAGUAR3_8822E ON  CACHE BOOL "" FORCE)
    set(DEVOURER_PCIE          OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/devourer
                     ${CMAKE_BINARY_DIR}/devourer EXCLUDE_FROM_ALL)
endif()

# --- wblink_core: zero-dependency portable wire/session library (PROTOCOL.md
# §2–3, §6.6). This is the target consumers (Waybeam-android :wifi) vendor so
# there is exactly ONE implementation of the packet format — it must never grow
# a dependency beyond the C++ standard library, and must stay 32-bit-clean
# (SSC338Q / ARMv7 is a first-class target).
add_library(wblink_core STATIC
    core/src/wire.cpp
    core/src/table.cpp
    core/src/gf256.cpp
    core/src/rlc.cpp
    core/src/framer.cpp
    core/src/frame_framer.cpp
    core/src/frame_reassembler.cpp
    core/src/cache_store.cpp
    core/src/cache_controller.cpp
    core/src/jscc_controller.cpp
    core/src/jscc_loss_estimator.cpp
    core/src/jscc_runtime_shadow.cpp
    core/src/ring.cpp
    core/src/rx.cpp
    core/src/scheduler.cpp
    core/src/reporter.cpp
    core/src/selector.cpp
    core/src/csa.cpp)
target_include_directories(wblink_core PUBLIC core/include)
target_compile_options(wblink_core PRIVATE ${WBLINK_WARNINGS})

# --- wblink_io: config/bindings/stats (§15). POSIX sockets + the vendored
# nlohmann/json (io layer only — core must stay dependency-free).
add_library(wblink_io STATIC
    io/src/config.cpp
    io/src/udp.cpp
    io/src/cache_udp.cpp
    io/src/frame_shm.cpp
    io/src/stats.cpp
    io/src/air_udp.cpp
    io/src/air_mon.cpp
    io/src/power_file.cpp
    io/src/venc_http.cpp
    io/src/control_server.cpp)
target_include_directories(wblink_io PUBLIC io/include)
target_include_directories(wblink_io SYSTEM PRIVATE third_party)
target_compile_options(wblink_io PRIVATE ${WBLINK_WARNINGS})
target_link_libraries(wblink_io PUBLIC wblink_core)
if(WBLINK_RADIO)
    target_sources(wblink_io PRIVATE io/src/air_radio.cpp)
    target_compile_definitions(wblink_io PUBLIC WBLINK_RADIO=1)
    target_include_directories(wblink_io SYSTEM PRIVATE
        third_party/devourer/src
        third_party/libusb-cmake/libusb/libusb)
    target_link_libraries(wblink_io PUBLIC devourer usb-1.0)
endif()

add_executable(waybeam-link app/main.cpp)
target_compile_options(waybeam-link PRIVATE ${WBLINK_WARNINGS})
target_link_libraries(waybeam-link PRIVATE wblink_io)

# Native-only real-video bench. Keep GStreamer out of the target binary and
# cross build; the tool still uses the production FrameShmRing implementation.
if(NOT CMAKE_CROSSCOMPILING AND EXISTS "/usr/bin/pkg-config")
    execute_process(
        COMMAND /usr/bin/pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0
        OUTPUT_VARIABLE WBLINK_GST_CFLAGS
        RESULT_VARIABLE WBLINK_GST_CFLAGS_RC
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND /usr/bin/pkg-config --libs gstreamer-1.0 gstreamer-app-1.0
        OUTPUT_VARIABLE WBLINK_GST_LIBS
        RESULT_VARIABLE WBLINK_GST_LIBS_RC
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(WBLINK_GST_CFLAGS_RC EQUAL 0 AND WBLINK_GST_LIBS_RC EQUAL 0)
        separate_arguments(WBLINK_GST_CFLAGS NATIVE_COMMAND "${WBLINK_GST_CFLAGS}")
        separate_arguments(WBLINK_GST_LIBS NATIVE_COMMAND "${WBLINK_GST_LIBS}")
        add_executable(frame_shm_gst_bench tools/frame_shm_gst_bench.cpp)
        target_compile_options(frame_shm_gst_bench PRIVATE
            ${WBLINK_WARNINGS} ${WBLINK_GST_CFLAGS})
        target_link_libraries(frame_shm_gst_bench PRIVATE
            wblink_io ${WBLINK_GST_LIBS})
    endif()
endif()

if(WBLINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
