// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/selector_state.h"

#include "wbtest.h"

using namespace wblink;

int main() {
    SelectorState state;
    state.prefix = {17, 0, 0x12345678};
    state.table_version = 0xB2;

    CHECK(selector_state_admissible(state, uint8_t{0xB2}, 17, 0x12345678));
    CHECK(!selector_state_admissible(state, std::nullopt, 17, 0x12345678));
    CHECK(!selector_state_admissible(state, uint8_t{0xB3}, 17, 0x12345678));
    CHECK(!selector_state_admissible(state, uint8_t{0xB2}, 18, 0x12345678));
    CHECK(!selector_state_admissible(state, uint8_t{0xB2}, 17, 0x12345679));
    state.prefix.destination = 9;
    CHECK(!selector_state_admissible(state, uint8_t{0xB2}, 17, 0x12345678));

    CHECK(selector_state_fresh(1000, 1000));
    CHECK(selector_state_fresh(2500, 1000));
    CHECK(!selector_state_fresh(2501, 1000));
    CHECK(!selector_state_fresh(999, 1000));

    return wbtest_finish("selector_state_test");
}
