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
