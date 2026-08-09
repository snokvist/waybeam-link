# SPDX-License-Identifier: GPL-2.0-or-later
# The smallest project that proves `wblink::node` LINKS on a build without the
# optional subsystems (#109 B10).
#
#   cmake -S examples/node-linkcheck -B build/nodelink
#   cmake --build build/nodelink
#
# Why this is a separate gate and not a preset. `android-arm64` — the preset
# that exists to prove the Phase 3 consumer — is compile-only, and a static
# archive does not resolve its own undefined symbols. Before B10 that preset
# was GREEN while `libwblink_node.a` carried nine unresolvable references to
# FrameShmRing and ControlServer, which would have surfaced in the consumer's
# link and nowhere earlier. A build cannot see that. A link can.
#
# It is deliberately NOT part of examples/embed-consumer: that gate holds a
# different property (B7 — embedding does not build the daemon's world), and
# widening it would blur which failure a red gate means. The two are cheap.
#
# The bionic axis is covered separately by the android-arm64 preset. What this
# adds is the axis a compile cannot cover — symbol resolution — and it runs on
# the host, so it needs no NDK and every developer gets it.
cmake_minimum_required(VERSION 3.22)
project(wblink_node_linkcheck C CXX)

# The configuration a phone builds: no frame-SHM (bionic has no shm_open), no
# REST server, no venc actuation, no daemon.
set(WBLINK_FRAME_SHM OFF)
set(WBLINK_CONTROL_SERVER OFF)
set(WBLINK_VENC OFF)
set(WBLINK_BUILD_APP OFF)
set(WBLINK_BUILD_TESTS OFF)
add_subdirectory(../.. wblink)

# Assert the configuration, the way embed-consumer does. Without this the gate
# passes whenever the link succeeds — including if these options silently
# stopped being OFF (a CACHE ... FORCE upstream, a policy change, a rename), at
# which point frame_shm.cpp is back in the archive and the link proves nothing.
# That silent-green shape is the exact reason gates.sh exists; see its header.
foreach(_opt WBLINK_FRAME_SHM WBLINK_CONTROL_SERVER WBLINK_VENC)
    if(${_opt})
        message(FATAL_ERROR
            "node-linkcheck requires ${_opt}=OFF — it is the whole point of "
            "this gate. Something upstream is forcing it ON.")
    endif()
endforeach()
if(TARGET waybeam-link)
    message(FATAL_ERROR "node-linkcheck built the daemon; WBLINK_BUILD_APP leaked ON")
endif()

# And assert the TX half stayed OUT of the archive (#109 Phase 3). `run_tx`
# uses all three subsystems above, so compiling it here would put ~22
# unresolvable references into libwblink_node.a — invisible to this gate,
# because a static archive extracts a member only when something references it
# and nothing here does. That is the same silent-green shape the gate exists to
# end, one level deeper, so it is checked against the target rather than
# inferred from the options: delete the guard upstream and this goes red.
get_target_property(_wblink_node_srcs wblink_node SOURCES)
if(_wblink_node_srcs MATCHES "tx_node")
    message(FATAL_ERROR
        "wblink_node contains the TX sources in a build with FRAME_SHM/"
        "CONTROL_SERVER/VENC off: ${_wblink_node_srcs}. run_tx needs all "
        "three, so this archive would carry unresolvable references.")
endif()

# c_consumer.c is compiled as C on purpose: rx_node_c.h must be C-clean
# for waybeam-hub and for JNI, and no C++ build can demonstrate that.
add_executable(node_linkcheck main.cpp c_consumer.c)
set_source_files_properties(c_consumer.c PROPERTIES LANGUAGE C)
# A dialect and warnings, or the C half proves very little: without -pedantic
# an empty struct and an unprototyped function are silent GCC extensions —
# both invalid or unsafe ISO C, both valid C++, both otherwise green here.
# -Werror because gates.sh treats a warning from our targets as a failure and
# this file is the gate.
set_source_files_properties(c_consumer.c PROPERTIES COMPILE_OPTIONS
    "-std=c11;-Wall;-Wextra;-Wpedantic;-Wstrict-prototypes;-Werror")
target_link_libraries(node_linkcheck PRIVATE wblink::node)
# Belt and braces: an executable link already resolves everything, but a
# future shared-library variant of this check would not without it.
target_link_options(node_linkcheck PRIVATE -Wl,--no-undefined)
