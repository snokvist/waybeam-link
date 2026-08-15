# SPDX-License-Identifier: GPL-2.0-or-later

# add_wb_test(<name>) — builds tests/<name>.cpp against wblink_core and
# registers it with CTest. Extra link targets may follow the name.
function(add_wb_test name)
    add_executable(${name} ${name}.cpp)
    target_compile_options(${name} PRIVATE ${WBLINK_WARNINGS})
    target_link_libraries(${name} PRIVATE wblink_core ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_wb_test(crc8_test)
add_wb_test(wire_vectors_test)
add_wb_test(wire_roundtrip_test)
add_wb_test(wire_fuzz_test)
add_wb_test(table_hash_test)
add_wb_test(mcs_probe_test)
add_wb_test(gf256_test)
add_wb_test(rlc_test)
add_wb_test(frame_framer_test)
add_wb_test(frame_reassembler_test)
add_wb_test(cache_store_test)
add_wb_test(cache_controller_test)
add_wb_test(cache_assignment_test)
add_wb_test(frame_caps_test)
add_wb_test(fps_ladder_test)
add_wb_test(report_gate_test)
add_wb_test(jscc_controller_test)
add_wb_test(jscc_loss_estimator_test)
add_wb_test(jscc_runtime_shadow_test)
add_wb_test(clamp_test)
add_wb_test(framer_test)
add_wb_test(nal_test)
add_wb_test(ring_test)
add_wb_test(rx_test)
add_wb_test(scheduler_test)
add_wb_test(selector_test)
add_wb_test(selector_state_test)
add_wb_test(reporter_test)
add_wb_test(quietgap_test)
add_wb_test(video_slot_cadence_test)
add_wb_test(hmac_test)
add_wb_test(csa_test)
add_wb_test(vehicle_cmd_test)
add_wb_test(uplink_calibrate_test)
add_wb_test(calibrate_test)
add_wb_test(calib_dwell_test)
add_wb_test(recovery_test)
add_wb_test(loopback_test)

add_wb_test(txwedge_test wblink_io)
add_wb_test(dot11_test wblink_io)
add_wb_test(radio_decode_test wblink_io)
if(WBLINK_RADIO)
    add_wb_test(radio_rx_only_test wblink_io)
    add_wb_test(radio_fd_source_test wblink_io)
    # The Config -> AirBackend -> RadioAirCfg fd seam and its C ABI setter.
    # Radio-only for the same reason as the two above: with WBLINK_RADIO off
    # AirBackend::create's kRadio branch does not exist to test.
    add_wb_test(node_fd_source_test wblink_io wblink_node)
    add_wb_test(build_info_test wblink_io wblink_node)
    # Pass 180: the test's expectation comes from CMake's variables, through a
    # DIFFERENT target than the library's own definitions. Same authority, two
    # independent paths — so a derivation that goes wrong inside the library TU
    # (as it did, reading a WBLINK_VENC macro that does not exist) fails here
    # instead of agreeing with itself.
    target_compile_definitions(build_info_test PRIVATE ${WBLINK_BI_EXPECT_DEFS})
endif()

# Pass 179, deliberately OUTSIDE the WBLINK_RADIO gate: the fixture is
# air.kind=udp and every run in it is refused before a backend exists, so
# gating it left the exactly-one-config-source contract untested in exactly
# the receive-only builds that motivated a text config.
add_wb_test(node_config_api_test wblink_io wblink_node)

add_wb_test(scout_sense_test wblink_io)
add_wb_test(scout_store_test wblink_io)
add_wb_test(radiotap_test wblink_io)
add_wb_test(config_test wblink_io)
add_wb_test(uplink_calib_store_test wblink_io)
target_compile_definitions(config_test PRIVATE
    WBLINK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_wb_test(config_schema_test wblink_io)
add_wb_test(config_strict_test wblink_io)
add_wb_test(udp_test wblink_io)
if(WBLINK_FRAME_SHM)
    add_wb_test(frame_shm_test wblink_io)
    add_wb_test(frame_shm_loopback_test wblink_io)
endif()
add_wb_test(power_test wblink_io)
add_wb_test(stats_test wblink_io)
add_wb_test(log_sink_test wblink_io)
if(WBLINK_CONTROL_SERVER)
    add_wb_test(control_server_test wblink_io)
endif()
if(WBLINK_VENC)
    add_wb_test(venc_actuator_test wblink_io)
endif()
add_wb_test(modes_test wblink_io)
# modes_test parses the catalog JSON to verify sort order + fields; give it the
# vendored nlohmann header (wblink_io keeps its third_party include PRIVATE).
target_include_directories(modes_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party)

# node_rx_core_test reaches RxCore DIRECTLY — no app/main.cpp include, no
# suppressed main(), no -Wno-unused-function. That is the whole point of the
# node/ layer (#109 Phase 2a): before the move, the only way to touch this
# code was app_test's whole-TU include.
add_wb_test(node_rx_core_test wblink_io wblink_node)
add_wb_test(node_discovery_test wblink_io wblink_node)
add_wb_test(rx_runtime_control_test wblink_io wblink_node)
add_wb_test(node_air_backend_test wblink_io wblink_node)
add_wb_test(node_tx_core_test wblink_io wblink_node)
add_wb_test(node_uplink_power_test wblink_io wblink_node)
# TxRuntimeInfo is header-only and part of the RX-half of the archive's
# include set, so this runs in EVERY build — including receive-only ones
# where the wblink_tx_* symbols that feed it do not exist (Pass 174).
add_wb_test(tx_runtime_info_test wblink_io wblink_node)

# The TX C ABI (#109 Phase 3). Guarded on the same condition the top-level
# CMakeLists uses to compile the TX half into wblink_node at all: run_tx needs
# frame-SHM, the control server and venc, so with any of them OFF these four
# symbols do not exist and this would be an unresolved reference rather than a
# skipped test. examples/node-linkcheck covers the OFF side — it proves the
# header is C-clean, which is all a build without the symbols can prove.
#
# Asserted, not assumed: an undefined WBLINK_NODE_TX is falsy, so a rename or a
# typo upstream would silently unregister this test from every build — a green
# suite with one fewer test in it, which is the failure mode this whole commit
# is about.
if(NOT DEFINED WBLINK_NODE_TX)
    message(FATAL_ERROR
        "WBLINK_NODE_TX is not defined. The top-level CMakeLists sets it "
        "ON or OFF alongside wblink_node's source list; if it has been "
        "renamed, fix it here rather than letting tx_node_c_test vanish.")
endif()
if(WBLINK_NODE_TX)
    add_wb_test(tx_node_c_test wblink_io wblink_node)
endif()

# app_test #includes app/main.cpp whole (that file's contents are one anonymous
# namespace, so there is nothing to link against) with WBLINK_APP_TEST
# suppressing its entry point. It therefore needs the app's own include path
# and the vendored third_party headers wblink_io keeps PRIVATE.
# Guarded on WBLINK_BUILD_APP for the same reason the daemon is: this TU IS
# app/main.cpp, so it needs every subsystem that file uses unconditionally.
if(WBLINK_BUILD_APP)
    add_wb_test(app_test wblink_io wblink_node)
    target_compile_definitions(app_test PRIVATE WBLINK_APP_TEST=1)
    target_include_directories(app_test PRIVATE ${CMAKE_SOURCE_DIR})
    target_include_directories(app_test SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party)
    # With main() suppressed, run_loopback/load_all/usage have no
    # caller in THIS TU and every build would warn. (run_rx and run_tx are no
    # longer among them — they live in node/src/{rx,tx}_node.cpp since #109
    # Phase 2c step 2 and Phase 3, and this TU reaches them by linking
    # wblink_node like any other consumer.) They are not dead — the
    # shipped binary calls all of them — so the warning is silenced here only,
    # never repo-wide, where it would hide genuinely unreachable code.
    target_compile_options(app_test PRIVATE -Wno-unused-function)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME expand_arq_topology_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/expand_arq_topology_test.py)
    add_test(NAME link_monitor_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/link_monitor_test.py)
    add_test(NAME plan_matrix_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/plan_matrix_test.py)
    add_test(NAME jscc_replay_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/jscc_replay_test.py)
    # Source-level check: the declared registry vs. the loader's accessor
    # sites. Reads io/src/*.cpp directly, so it needs no build artifact.
    add_test(NAME config_registry_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/config_registry_test.py)
    # The standing answer to #109 blocker B9: node/ owns no process, and
    # nothing below it depends on it. Source-level, no build artifact.
    add_test(NAME node_layering_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/node_layering_test.py)
    # Every wbtest.h suite must be able to FAIL. CHECK only counts; the exit
    # code comes from wbtest_finish() alone, so a main() ending in `return 0`
    # is a suite that runs every assertion and reports success regardless.
    # Two files shipped that way (tx_node_c_test in review, mcs_probe_test for
    # much longer) and nothing in the build could see either.
    add_test(NAME test_verdict_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_verdict_test.py)
else()
    # Registering nothing here leaves ctest GREEN on a host without python3
    # while seven suites silently did not run — among them config_registry_test,
    # which is the only thing keeping io/src/config_registry.cpp honest against
    # io/src/config.cpp. A registry whose drift guard can disappear is just a
    # second source of truth, so absence is failure, not silence.
    #
    # Measured with -DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON: without this
    # branch `ctest --preset dev` dropped from 73 tests to 67 and exited 0
    # (7 python suites lost, this 1 sentinel gained).
    # Recount when adding a python test — the 5/6 that stood here were both
    # stale, each left behind by a PR that added one and adjusted nothing.
    #
    # The name MUST end in "_test". The dev test preset filters on
    # `"name": "_test$"` (CMakePresets.json:101), so a sentinel named anything
    # else is dropped by the only ctest invocation any gate runs, leaving this
    # branch decorative in exactly the case it exists for. That is not
    # hypothetical: the first version was named `python_tests_unavailable` and
    # its negative control used `ctest -R <name>`, which bypasses the preset
    # filter and therefore "passed".
    message(WARNING
        "Python3 not found: 7 suites cannot run, including config_registry_test "
        "(the registry-vs-loader drift guard). ctest will FAIL rather than "
        "silently pass. Install python3 to run the real gate.")
    add_test(NAME python_tests_unavailable_test
             COMMAND ${CMAKE_COMMAND} -E false)
endif()
