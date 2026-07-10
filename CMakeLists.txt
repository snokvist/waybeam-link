# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22)
project(waybeam-link CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WBLINK_BUILD_TESTS "Build unit tests" ON)
option(WBLINK_SANITIZE "Build with ASan+UBSan (host dev builds)" OFF)
option(WBLINK_FUZZ "Build the libFuzzer wire fuzz target (clang only)" OFF)

add_compile_options(-Wall -Wextra -Wshadow -Wconversion)
if(WBLINK_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# --- wblink_core: zero-dependency portable wire/session library (PROTOCOL.md
# §2–3, §6.6). This is the target consumers (Waybeam-android :wifi) vendor so
# there is exactly ONE implementation of the packet format — it must never grow
# a dependency beyond the C++ standard library, and must stay 32-bit-clean
# (SSC338Q / ARMv7 is a first-class target).
add_library(wblink_core STATIC
    core/src/wire.cpp
    core/src/table.cpp
    core/src/framer.cpp
    core/src/ring.cpp
    core/src/rx.cpp
    core/src/scheduler.cpp
    core/src/reporter.cpp
    core/src/selector.cpp)
target_include_directories(wblink_core PUBLIC core/include)

# --- wblink_io: config/bindings/stats (§15). POSIX sockets + the vendored
# nlohmann/json (io layer only — core must stay dependency-free).
add_library(wblink_io STATIC
    io/src/config.cpp
    io/src/udp.cpp
    io/src/stats.cpp
    io/src/air_udp.cpp
    io/src/power_file.cpp
    io/src/venc_http.cpp)
target_include_directories(wblink_io PUBLIC io/include)
target_include_directories(wblink_io SYSTEM PRIVATE third_party)
target_link_libraries(wblink_io PUBLIC wblink_core)

add_executable(waybeam-link app/main.cpp)
target_link_libraries(waybeam-link PRIVATE wblink_io)

if(WBLINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
