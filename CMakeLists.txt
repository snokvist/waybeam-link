# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22)
project(waybeam-link CXX C)

# Early, not next to install(): CMAKE_INSTALL_INCLUDEDIR is expanded into
# wblink_core's INSTALL_INTERFACE below, which is evaluated where it is
# written — including GNUInstallDirs only at the install block would expand it
# to the empty string and point the exported target at the prefix root.
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Everything that is "the daemon's world" — the app, the tests, the bench
# tools — defaults to ON standalone and OFF when this tree is embedded in
# another project via add_subdirectory (B7). Measured before this existed: an
# embedding consumer got 133 targets, including the daemon and all 61 test
# binaries, none of which it asked for. The options stay individually
# settable; only their DEFAULT is derived.
option(WBLINK_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(WBLINK_SANITIZE "Build with ASan+UBSan (host dev builds)" OFF)
option(WBLINK_FUZZ "Build the libFuzzer wire fuzz target (clang only)" OFF)
option(WBLINK_RADIO "Build the devourer radio air backend (§3.0/§7)" ON)
# Library-extraction feature options (docs/library-extraction-plan.md B6).
# All default ON, so every existing preset compiles the same source set it
# always did. They exist for a consumer that links wblink_io WITHOUT the
# daemon — the frame-SHM ring needs shm_open and <linux/futex.h>, neither of
# which bionic provides, and the two servers are dead weight on a receiver.
option(WBLINK_FRAME_SHM "Build the frame-SHM ring (§15.4)" ON)
option(WBLINK_CONTROL_SERVER "Build the REST/SSE control server (§15.5)" ON)
option(WBLINK_VENC "Build venc HTTP actuation (§9.6)" ON)
option(WBLINK_BUILD_APP "Build the waybeam-link daemon" ${PROJECT_IS_TOP_LEVEL})
# Promote our own warnings to errors. OFF by default so a dev build still
# compiles with a squiggle; ON for the android-arm64 gate, whose whole purpose
# is catching portability diagnostics (nfds_t narrowing was the first). Without
# it that preset only PRINTS the bug, inside a log dominated by vendored
# devourer/libusb warnings — a human-attention gate, not a build gate.
option(WBLINK_WERROR "Treat WBLINK_WARNINGS as errors (our targets only)" OFF)

# app/main.cpp includes frame_shm.h, control_server.h and wblink/venc.h
# unconditionally, so the daemon cannot be built against a wblink_io that
# lacks any of them. Refuse at configure time rather than emit a compiler
# error a hundred lines deep — an option matrix with a silently broken cell
# is worse than no option.
if(WBLINK_BUILD_APP)
    foreach(_feat FRAME_SHM CONTROL_SERVER VENC)
        if(NOT WBLINK_${_feat})
            message(FATAL_ERROR
                "WBLINK_BUILD_APP=ON requires WBLINK_${_feat}=ON: "
                "app/main.cpp uses that subsystem unconditionally. Set "
                "WBLINK_BUILD_APP=OFF to build the libraries alone.")
        endif()
    endforeach()
endif()

# The bionic gate exists to match the consumer, so say something when it does
# not. First-non-empty-env-var-wins in the toolchain file means a stale
# ANDROID_NDK_HOME can otherwise silently build against a different NDK than
# Waybeam-android pins. Lives here, not in the toolchain file: that file is
# re-processed by every try_compile, which emitted the warning 18 times.
if(ANDROID)
    set(WBLINK_ANDROID_NDK_PIN "26.3.11579264")
    if(EXISTS "${WBLINK_ANDROID_NDK}/source.properties")
        file(STRINGS "${WBLINK_ANDROID_NDK}/source.properties" _wblink_ndk_rev
             REGEX "^Pkg\\.Revision *=")
        if(NOT _wblink_ndk_rev MATCHES "${WBLINK_ANDROID_NDK_PIN}")
            message(WARNING
                "NDK at '${WBLINK_ANDROID_NDK}' is ${_wblink_ndk_rev}, not the "
                "${WBLINK_ANDROID_NDK_PIN} that Waybeam-android's :wifi pins. "
                "The bionic gate still runs, but parity with the consumer is "
                "not guaranteed.")
        endif()
    endif()
endif()

# Our warning set is applied per-target (WBLINK_WARNINGS below), NOT via
# directory-level add_compile_options — the vendored third_party subdirectories
# (devourer, libusb) must build with their own flags, not ours.
set(WBLINK_WARNINGS -Wall -Wextra -Wshadow -Wconversion)
if(WBLINK_WERROR)
    # Narrow on purpose. Blanket -Werror would inherit every future
    # -Wall/-Wextra expansion, so an NDK bump could break the gate on code
    # that has nothing to do with portability. These three are the classes the
    # gate exists for and all three exist on GCC and clang, so WBLINK_WERROR
    # stays usable on the glibc presets. NOT -Werror=shorten-64-to-32: clang
    # folds it under -Wconversion and GCC rejects the unknown option outright.
    list(APPEND WBLINK_WARNINGS
         -Werror=conversion -Werror=shadow -Werror=sign-compare)
endif()
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
    # NORMAL variables, not `CACHE ... FORCE` (B7). CMP0077 is NEW at our
    # cmake_minimum_required, so `option()` in the vendored trees honours a
    # plain variable, and a plain variable shadows the cache for this
    # directory and everything below it — same effect on our build, no entry
    # written to the consumer's cache. With FORCE, embedding this tree
    # silently flipped the CONSUMER's BUILD_SHARED_LIBS to OFF, turning its
    # own shared libraries static (measured).
    set(BUILD_SHARED_LIBS        OFF)
    set(LIBUSB_BUILD_SHARED_LIBS OFF)
    set(LIBUSB_BUILD_EXAMPLES    OFF)
    set(LIBUSB_BUILD_TESTING     OFF)
    set(LIBUSB_INSTALL_TARGETS   OFF)
    # No hotplug enumeration needed and the SSC338Q cross toolchain has no
    # libudev — the netlink fallback covers both hosts.
    set(LIBUSB_ENABLE_UDEV       OFF)
    # CURRENT, not CMAKE_BINARY_DIR: embedded, the latter is the CONSUMER's
    # build root, so a consumer that also vendors libusb-cmake at the
    # conventional spot collides with "binary directory is already used".
    # Waybeam-android does exactly that today, which means its hand-copy must
    # be deleted in the same commit that adds add_subdirectory(waybeam-link) —
    # the migration cannot be staged.
    add_subdirectory(third_party/libusb-cmake
                     ${CMAKE_CURRENT_BINARY_DIR}/libusb EXCLUDE_FROM_ALL)

    # Likewise scoped. As a FORCE cache entry this redirected the ENTIRE
    # consuming project's pkg-config to a shim that answers libusb-1.0 and
    # exits 1 on anything else — so every other pkg_check_modules() in the
    # consumer failed. find_program() honours a normal variable and leaves
    # the cache untouched (verified: devourer still resolves libusb 1.0.30).
    set(PKG_CONFIG_EXECUTABLE
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pkgconf-libusb.sh)
    # Chip selection per build flavor. Each enabled family carries its fw
    # blobs, so vehicle builds trim (the SSC338Q overlay is ~5.7 MB) while
    # ground builds have no size pressure and enable everything USB:
    #   fleet (default) — 8812AU (Jaguar1) + 8812CU (rtl8822c) + 8812EU
    #                     (rtl8822e): exactly the adapters the fleet flies.
    #   au             — Jaguar1 only: smallest vehicle binary for
    #                     8812AU-equipped craft.
    #   eu             — rtl8822e only: smallest vehicle binary for
    #                     8812EU-equipped craft.
    #   all            — every USB family devourer supports (adds 8814AU,
    #                     Jaguar2 8822BU/8821CU, Kestrel 8852B/C 11ax) for
    #                     ground-side adapter tests. PCIe stays off.
    set(WBLINK_DEVOURER_CHIPS "fleet" CACHE STRING
        "devourer chip families: fleet | au | eu | all")
    if(WBLINK_DEVOURER_CHIPS STREQUAL "all")
        set(_wblink_j1 ON)
        set(_wblink_j3c ON)
        set(_wblink_j3e ON)
        set(_wblink_extra ON)
    elseif(WBLINK_DEVOURER_CHIPS STREQUAL "au")
        set(_wblink_j1 ON)
        set(_wblink_j3c OFF)
        set(_wblink_j3e OFF)
        set(_wblink_extra OFF)
    elseif(WBLINK_DEVOURER_CHIPS STREQUAL "eu")
        set(_wblink_j1 OFF)
        set(_wblink_j3c OFF)
        set(_wblink_j3e ON)
        set(_wblink_extra OFF)
    elseif(WBLINK_DEVOURER_CHIPS STREQUAL "fleet")
        set(_wblink_j1 ON)
        set(_wblink_j3c ON)
        set(_wblink_j3e ON)
        set(_wblink_extra OFF)
    else()
        message(FATAL_ERROR
            "WBLINK_DEVOURER_CHIPS must be fleet, au, eu, or all (got "
            "'${WBLINK_DEVOURER_CHIPS}')")
    endif()
    # Normal variables for the same reason as the libusb block above. The
    # Kestrel pair matters most: devourer defaults them ON, our fleet builds
    # force them OFF, and this is the exact wiring the Android consumer
    # hand-copied and will acquire silently when its devourer submodule is
    # bumped to ours (B7). Owning it here is what lets that copy be deleted.
    set(DEVOURER_JAGUAR1       ${_wblink_j1})
    set(DEVOURER_8814          ${_wblink_extra})
    set(DEVOURER_JAGUAR2_8822B ${_wblink_extra})
    set(DEVOURER_JAGUAR2_8821C ${_wblink_extra})
    set(DEVOURER_JAGUAR3_8822C ${_wblink_j3c})
    set(DEVOURER_JAGUAR3_8822E ${_wblink_j3e})
    set(DEVOURER_KESTREL_8852B ${_wblink_extra})
    set(DEVOURER_KESTREL_8852C ${_wblink_extra})
    set(DEVOURER_PCIE          OFF)
    add_subdirectory(third_party/devourer
                     ${CMAKE_CURRENT_BINARY_DIR}/devourer EXCLUDE_FROM_ALL)

    # Prove the normal variables above were actually honoured. They rely on
    # CMP0077 being NEW inside the vendored tree, which is true only while its
    # own cmake_minimum_required stays >= 3.13 — and a re-vendor could lower
    # it. Under CMP0077 OLD, option() would overwrite our value in devourer's
    # scope ONLY, so ours still reads OFF and checking it would prove nothing:
    # the honest witness is what devourer actually compiled. Silently gaining
    # two 11ax families in a vehicle build is the exact failure the old
    # `CACHE ... FORCE` made impossible, so it must not become possible now.
    get_target_property(_wblink_dv_defs devourer INTERFACE_COMPILE_DEFINITIONS)
    foreach(_k 8852B 8852C)
        if(NOT _wblink_extra AND "DEVOURER_HAVE_KESTREL_${_k}=1" IN_LIST
                                 _wblink_dv_defs)
            message(FATAL_ERROR
                "devourer built Kestrel ${_k} despite WBLINK_DEVOURER_CHIPS="
                "'${WBLINK_DEVOURER_CHIPS}'. The vendored tree is not honouring "
                "our option values — check CMP0077 (its cmake_minimum_required "
                "must stay >= 3.13) after any re-vendor.")
        endif()
    endforeach()
    message(STATUS
        "devourer chips '${WBLINK_DEVOURER_CHIPS}': jaguar1=${_wblink_j1} "
        "8822c=${_wblink_j3c} 8822e=${_wblink_j3e} extra=${_wblink_extra}")
endif()

# --- wblink_core: zero-dependency portable wire/session library (PROTOCOL.md
# §2–3, §6.6). This is the target consumers (Waybeam-android :wifi) vendor so
# there is exactly ONE implementation of the packet format — it must never grow
# a dependency beyond the C++ standard library, and must stay 32-bit-clean
# (SSC338Q / ARMv7 is a first-class target).
add_library(wblink_core STATIC
    core/src/wire.cpp
    core/src/table.cpp
    core/src/mcs_probe.cpp
    core/src/gf256.cpp
    core/src/rlc.cpp
    core/src/framer.cpp
    core/src/frame_framer.cpp
    core/src/frame_reassembler.cpp
    core/src/cache_store.cpp
    core/src/cache_controller.cpp
    core/src/cache_assignment.cpp
    core/src/jscc_controller.cpp
    core/src/jscc_loss_estimator.cpp
    core/src/jscc_runtime_shadow.cpp
    core/src/ring.cpp
    core/src/rx.cpp
    core/src/scheduler.cpp
    core/src/reporter.cpp
    core/src/selector.cpp
    core/src/csa.cpp
    core/src/vehicle_cmd.cpp
    core/src/recovery.cpp)
target_include_directories(wblink_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# C++20 is a usage requirement, not a local setting. CMAKE_CXX_STANDARD is
# directory-scoped and does NOT travel with an exported or embedded target, so
# without this a find_package() consumer at its compiler's default standard
# fails inside our own headers (measured: table.h's defaulted operator==).
target_compile_features(wblink_core PUBLIC cxx_std_20)
add_library(wblink::core ALIAS wblink_core)
# EXPORT_NAME, not just the ALIAS: install(EXPORT NAMESPACE wblink::) prefixes
# the TARGET name, so without this the installed package would offer
# wblink::wblink_core while an embedding consumer links wblink::core — the
# same library under two spellings depending on how it was consumed. (Found by
# building a find_package() consumer, not by reading.)
set_target_properties(wblink_core PROPERTIES EXPORT_NAME core)
target_compile_options(wblink_core PRIVATE ${WBLINK_WARNINGS})

# --- wblink_io: config/bindings/stats (§15). POSIX sockets + the vendored
# nlohmann/json (io layer only — core must stay dependency-free).
add_library(wblink_io STATIC
    io/src/config.cpp
    io/src/config_registry.cpp
    io/src/log.cpp
    io/src/udp.cpp
    io/src/cache_udp.cpp
    io/src/stats.cpp
    io/src/air_udp.cpp
    io/src/power_file.cpp
    io/src/calib_store.cpp
    io/src/scout_sense.cpp
    io/src/scout_store.cpp
    io/src/uplink_calib_store.cpp
    io/src/modes.cpp)
target_include_directories(wblink_io PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/io/include>)
target_compile_features(wblink_io PUBLIC cxx_std_20)
add_library(wblink::io ALIAS wblink_io)
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
# --- wblink_node: node behaviour (#109 Phase 2a). The layer above io/ that
# wires core/ into a running node. Header-only for now: RxCore is entirely
# inline, so this adds NO translation unit and the `dev` TU set is unchanged
# from before the move — the same no-change control Phase 1a used. It becomes
# a STATIC library the moment something here needs a .cpp.
#
# Layering rule: node/ may use core/ and io/; neither may use node/.
add_library(wblink_node INTERFACE)
target_include_directories(wblink_node INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/node/include>)
target_link_libraries(wblink_node INTERFACE wblink_io)
add_library(wblink::node ALIAS wblink_node)

# The three optional units. Only the target_sources half of the WBLINK_RADIO
# pattern above applies: none of these three has a single #if consumer, so a
# feature macro would reach every flying build's command line for nothing.
# Re-add one when a consumer needs to branch on it. All three are leaves —
# nothing else in io/ carries an undefined reference to them (verified with nm
# over libwblink_io.a).
if(WBLINK_FRAME_SHM)
    target_sources(wblink_io PRIVATE io/src/frame_shm.cpp)
endif()
if(WBLINK_CONTROL_SERVER)
    target_sources(wblink_io PRIVATE io/src/control_server.cpp)
endif()
if(WBLINK_VENC)
    target_sources(wblink_io PRIVATE io/src/venc_http.cpp)
endif()

if(WBLINK_BUILD_APP)
    add_executable(waybeam-link app/main.cpp)
    target_compile_options(waybeam-link PRIVATE ${WBLINK_WARNINGS})
    target_link_libraries(waybeam-link PRIVATE wblink_io wblink_node)
endif()

# Strip the shipped binary on cross builds — the SSC338Q craft roots on a
# ~5.7 MB overlay where an unstripped ~2.9 MB binary barely fits (and a second
# copy does not). Native builds stay unstripped so the bench keeps its symbols.
if(WBLINK_BUILD_APP AND CMAKE_CROSSCOMPILING AND CMAKE_STRIP)
    add_custom_command(TARGET waybeam-link POST_BUILD
        COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:waybeam-link>"
        COMMENT "Stripping waybeam-link (cross build; small target rootfs)")
endif()

# Native-only real-video bench. Keep GStreamer out of the target binary and
# cross build; the tool still uses the production FrameShmRing implementation.
if(WBLINK_FRAME_SHM AND PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING
   AND EXISTS "/usr/bin/pkg-config")
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

# Synthetic frame-SHM bench endpoint (deterministic pattern producer +
# verifying consumer) — no GStreamer, so it runs on any dev host. Used by
# tools/cache_repair_bench.sh.
if(WBLINK_FRAME_SHM AND PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
    add_executable(frame_shm_feed tools/frame_shm_feed.cpp)
    target_compile_options(frame_shm_feed PRIVATE ${WBLINK_WARNINGS})
    target_link_libraries(frame_shm_feed PRIVATE wblink_io)
endif()

# Hardware trial harness (issue #140). Top-level + radio only, and native
# only — it opens usbfs on the host it runs on. RX-only by construction, so it
# cannot transmit; see the file header.
if(WBLINK_RADIO AND PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
    add_executable(hwtrial_bringup tools/hwtrial_bringup.cpp)
    target_compile_options(hwtrial_bringup PRIVATE ${WBLINK_WARNINGS})
    target_include_directories(hwtrial_bringup SYSTEM PRIVATE
        third_party/libusb-cmake/libusb/libusb)
    target_link_libraries(hwtrial_bringup PRIVATE wblink_io)
endif()

if(WBLINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- install / export (docs/library-extraction-plan.md B7) -------------------
#
# ONLY wblink_core is installed and exported, and that asymmetry is deliberate
# rather than unfinished. wblink_io PUBLIC-links two vendored static libraries
# (devourer, usb-1.0); install(EXPORT) refuses a target whose link interface
# names targets outside the export set, and devourer has no install() rule of
# its own while libusb-cmake's are force-disabled here because we link its
# static target directly. Exporting wblink_io would therefore mean authoring
# install rules for third-party trees we may not edit, whose interface include
# directories point into OUR source tree. So:
#
#   find_package(wblink)   -> wblink::core, the dependency-free wire library
#   add_subdirectory(...)  -> wblink::core AND wblink::io, radio wiring included
#
# The second form is what closes B7: an embedding consumer links wblink::io and
# deletes its hand-copy of the libusb force-block, the devourer chip selection
# and the static archive link order. examples/embed-consumer/ is that consumer,
# reduced to the smallest thing that proves it.
if(PROJECT_IS_TOP_LEVEL)

    install(TARGETS wblink_core EXPORT wblinkTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY core/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT wblinkTargets
            FILE wblinkTargets.cmake
            NAMESPACE wblink::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wblink)

    # No version file: the library has no released version yet, and a
    # fabricated one would let a consumer write a compatibility constraint
    # that means nothing. Add it with the first tagged release; until then
    # find_package(wblink 1.0) fails loudly with "version: unknown".
    #
    # Component handling is not boilerplate here: the package deliberately
    # ships one target out of two, so without it find_package(wblink
    # COMPONENTS io) reports SUCCESS and leaves wblink::io undefined —
    # precisely the mistake this asymmetry invites.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wblinkConfig.cmake" [[
include("${CMAKE_CURRENT_LIST_DIR}/wblinkTargets.cmake")
# Only wblink::core is in this package. Answer a COMPONENTS request honestly
# instead of reporting success with the target undefined.
foreach(_wblink_c ${wblink_FIND_COMPONENTS})
    if(TARGET wblink::${_wblink_c})
        set(wblink_${_wblink_c}_FOUND 1)
    else()
        set(wblink_${_wblink_c}_FOUND 0)
        if(wblink_FIND_REQUIRED_${_wblink_c})
            set(wblink_FOUND 0)
            set(wblink_NOT_FOUND_MESSAGE
                "wblink component '${_wblink_c}' is not in the installed \
package; only 'core' is exported. wblink::io links vendored devourer/usb-1.0, \
which have no install rules -- consume it with add_subdirectory() instead.")
        endif()
    endif()
endforeach()
]])
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/wblinkConfig.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wblink)
endif()
