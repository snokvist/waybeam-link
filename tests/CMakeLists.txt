# SPDX-License-Identifier: GPL-2.0-or-later

# add_wb_test(<name>) — builds tests/<name>.cpp against wblink_core and
# registers it with CTest. Extra link targets may follow the name.
function(add_wb_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE wblink_core ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_wb_test(crc8_test)
add_wb_test(wire_vectors_test)
add_wb_test(wire_roundtrip_test)
add_wb_test(wire_fuzz_test)
add_wb_test(table_hash_test)
add_wb_test(clamp_test)
add_wb_test(framer_test)
add_wb_test(nal_test)
add_wb_test(ring_test)
add_wb_test(rx_test)
add_wb_test(scheduler_test)
add_wb_test(selector_test)
add_wb_test(reporter_test)
add_wb_test(loopback_test)

add_wb_test(config_test wblink_io)
target_compile_definitions(config_test PRIVATE
    WBLINK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_wb_test(udp_test wblink_io)
add_wb_test(power_test wblink_io)
add_wb_test(stats_test wblink_io)
