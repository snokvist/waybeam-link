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
get_target_property(_type consumer TYPE)
if(NOT _type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "embedding flipped BUILD_SHARED_LIBS: consumer is ${_type}")
endif()
foreach(_t waybeam-link app_test frame_shm_feed)
    if(TARGET ${_t})
        message(FATAL_ERROR "embedding built the daemon's world: target ${_t} exists")
    endif()
endforeach()
find_package(PkgConfig)
if(PKG_CONFIG_FOUND AND NOT PKG_CONFIG_EXECUTABLE STREQUAL "PKG_CONFIG_EXECUTABLE-NOTFOUND")
    execute_process(COMMAND ${PKG_CONFIG_EXECUTABLE} --version
                    OUTPUT_VARIABLE _v RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(PKG_CONFIG_EXECUTABLE MATCHES "pkgconf-libusb")
        message(FATAL_ERROR
            "embedding hijacked this project's pkg-config: ${PKG_CONFIG_EXECUTABLE}")
    endif()
endif()
message(STATUS "embed-consumer: all four embedding properties hold")
