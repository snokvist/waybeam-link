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
    core/src/table.cpp)
target_include_directories(wblink_core PUBLIC core/include)

if(WBLINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
