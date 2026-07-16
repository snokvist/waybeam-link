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
add_wb_test(gf256_test)
add_wb_test(rlc_test)
add_wb_test(frame_framer_test)
add_wb_test(frame_reassembler_test)
add_wb_test(cache_store_test)
add_wb_test(cache_controller_test)
add_wb_test(frame_caps_test)
add_wb_test(fps_ladder_test)
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
add_wb_test(reporter_test)
add_wb_test(quietgap_test)
add_wb_test(hmac_test)
add_wb_test(csa_test)
add_wb_test(loopback_test)

add_wb_test(txwedge_test wblink_io)
add_wb_test(dot11_test wblink_io)
add_wb_test(radiotap_test wblink_io)
add_wb_test(config_test wblink_io)
target_compile_definitions(config_test PRIVATE
    WBLINK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_wb_test(udp_test wblink_io)
add_wb_test(frame_shm_test wblink_io)
add_wb_test(frame_shm_loopback_test wblink_io)
add_wb_test(power_test wblink_io)
add_wb_test(stats_test wblink_io)
add_wb_test(control_server_test wblink_io)

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
endif()
