# SPDX-License-Identifier: GPL-2.0-or-later
# The smallest project that proves waybeam-link is embeddable (B7).
#
#   cmake -S examples/embed-consumer -B build/embed
#   cmake --build build/embed
#
# What it must demonstrate, and what regressed before it existed:
#   - add_subdirectory + link wblink::io is enough — no hand-copied libusb
#     force-block, no devourer chip selection, no archive link order;
#   - the daemon, the tests and the bench tools do NOT get built here;
#   - this project's own pkg-config still works (waybeam-link used to
#     redirect PKG_CONFIG_EXECUTABLE for the whole tree, so every
#     pkg_check_modules() below it failed);
#   - this project's own BUILD_SHARED_LIBS is not flipped underneath it.
cmake_minimum_required(VERSION 3.22)
project(wblink_embed_consumer CXX)

set(BUILD_SHARED_LIBS ON)
add_subdirectory(../.. wblink)

add_library(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE wblink::io)

# Assert the four properties above at configure time, so a regression is a
# configure failure and not something a reader has to notice.
# The cache entry, not the target type: line 18 sets BUILD_SHARED_LIBS as a
# NORMAL variable, which shadows the cache in this scope — so `consumer` comes
# out SHARED even on the old code that wrote OFF into the cache. The type
# check alone was vacuous; the cache write is the actual damage, and it is
# what a consumer that did NOT set the variable locally would suffer.
if(DEFINED CACHE{BUILD_SHARED_LIBS})
    message(FATAL_ERROR
        "embedding wrote BUILD_SHARED_LIBS into this project's cache")
endif()
get_target_property(_type consumer TYPE)
if(NOT _type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "embedding flipped BUILD_SHARED_LIBS: consumer is ${_type}")
endif()
# Not exhaustive by construction: the vendored trees still leak ~70
# EXCLUDE_FROM_ALL targets (devourer's example executables, libusb's
# selftests) that we cannot gate without editing third_party/. These five are
# ours, and ours are the ones that would actually build.
foreach(_t waybeam-link app_test wire_test frame_shm_feed frame_shm_gst_bench)
    if(TARGET ${_t})
        message(FATAL_ERROR "embedding built the daemon's world: target ${_t} exists")
    endif()
endforeach()
# REQUIRED on purpose: without it, a host lacking pkg-config would skip this
# guard silently — and devourer's own find_package(PkgConfig REQUIRED) always
# succeeds through the shim, so nothing else would notice either.
find_package(PkgConfig REQUIRED)
if(PKG_CONFIG_EXECUTABLE MATCHES "pkgconf-libusb")
    message(FATAL_ERROR
        "embedding hijacked this project's pkg-config: ${PKG_CONFIG_EXECUTABLE}")
endif()
message(STATUS "embed-consumer: all four embedding properties hold")
