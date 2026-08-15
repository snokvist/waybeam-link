// SPDX-License-Identifier: GPL-2.0-or-later
// Pass 180: the build's own account of what it can do.
//
// The assertion that matters is NOT "the string is well formed" — it is that
// the string agrees with the macros this translation unit was compiled with.
// Both are compiled from the same CMake target, so a disagreement means the
// baked constant and the build no longer describe the same thing, which is
// precisely the class of bug the call exists to expose in consumers.
#include "wblink/node/build_info_c.h"

#include <cstring>
#include <string>

#include "wbtest.h"

namespace {

bool has_field(const std::string& info, const char* field, bool expected) {
    const std::string want =
        std::string("\"") + field + "\":" + (expected ? "true" : "false");
    return info.find(want) != std::string::npos;
}

}  // namespace

int main() {
    const char* raw = wblink_build_info();
    CHECK(raw != nullptr);
    const std::string info(raw);
    // Static storage: the same pointer every time, valid without a handle.
    CHECK(wblink_build_info() == raw);
    CHECK(!info.empty() && info.front() == '{' && info.back() == '}');

// Every field is checked against CMake's own variable, handed to this target
// separately from the library's (tests/CMakeLists.txt). Deriving the
// expectation from the feature macros a consumer sees is exactly the mistake
// this test exists to catch: WBLINK_VENC is not a macro at all, so both sides
// read it as OFF and agreed with each other while the string was wrong.
    CHECK(has_field(info, "frame_shm", WBLINK_BI_EXPECT_FRAME_SHM != 0));
    CHECK(has_field(info, "control_server",
                    WBLINK_BI_EXPECT_CONTROL_SERVER != 0));
    CHECK(has_field(info, "venc", WBLINK_BI_EXPECT_VENC != 0));
    CHECK(has_field(info, "radio", WBLINK_BI_EXPECT_RADIO != 0));
    CHECK(has_field(info, "node_tx", WBLINK_BI_EXPECT_NODE_TX != 0));

    // node_tx is a consequence, and the library must not claim a transmitter
    // it could not have compiled: TX sources exist only when all three of
    // frame_shm/control_server/venc are on.
    if (WBLINK_BI_EXPECT_NODE_TX != 0) {
        CHECK(WBLINK_BI_EXPECT_FRAME_SHM != 0);
        CHECK(WBLINK_BI_EXPECT_CONTROL_SERVER != 0);
        CHECK(WBLINK_BI_EXPECT_VENC != 0);
    }

    return wbtest_finish("build_info_test");
}
