// SPDX-License-Identifier: GPL-2.0-or-later
// TxRuntimeInfo (Pass 174) — the TX snapshot mailbox. The buffer contract is
// shared with RxRuntimeControl and asserted here independently, because the
// two are separate code: a drift in one would otherwise only be visible to
// whichever embedder hit it first.
#include "wblink/node/tx_runtime_info.h"

#include <array>
#include <cstring>
#include <string>

#include "wbtest.h"

using wblink::node::TxRuntimeInfo;

// A read before publication reports not-ready — and unlike the RX snapshot
// calls there is deliberately NO snapshot-request edge to consume: adapters
// are static per die, and status republishes on the loop's own 1 Hz cadence.
static void test_not_ready_before_publication() {
    TxRuntimeInfo info;
    size_t required = 77;
    CHECK(info.copy_adapters(nullptr, 0, &required) == 3);
    CHECK_EQ_U(required, 0);
    required = 77;
    CHECK(info.copy_status(nullptr, 0, &required) == 3);
    CHECK_EQ_U(required, 0);
}

// Size query includes the NUL; an undersized buffer copies no partial JSON;
// bad arguments are refused. Same contract as the RX copies, asserted per
// call because the two slots are independent.
static void test_publish_and_copy_contract() {
    TxRuntimeInfo info;
    size_t required = 0;
    const std::string adapters =
        "{\"adapters\":[{\"name\":\"a0\",\"chip\":\"udp\"}]}";
    const std::string status = "{\"session\":1,\"wedge\":{\"wedged\":false}}";
    info.publish_adapters(adapters);
    info.publish_status(status);

    std::array<char, 64> out{};
    CHECK(info.copy_adapters(nullptr, 0, &required) == 0);
    CHECK_EQ_U(required, adapters.size() + 1);
    out.fill('x');
    CHECK(info.copy_adapters(out.data(), adapters.size(), &required) == 4);
    CHECK(out.front() == 'x');  // insufficient means no partial JSON
    CHECK(info.copy_adapters(out.data(), adapters.size() + 1, &required) == 0);
    CHECK(std::strcmp(out.data(), adapters.c_str()) == 0);

    CHECK(info.copy_status(nullptr, 0, &required) == 0);
    CHECK_EQ_U(required, status.size() + 1);
    out.fill('y');
    CHECK(info.copy_status(out.data(), status.size(), &required) == 4);
    CHECK(out.front() == 'y');
    CHECK(info.copy_status(out.data(), status.size() + 1, &required) == 0);
    CHECK(std::strcmp(out.data(), status.c_str()) == 0);

    CHECK(info.copy_status(nullptr, 1, &required) == 2);
    CHECK(info.copy_status(out.data(), out.size(), nullptr) == 2);
    CHECK(info.copy_adapters(nullptr, 1, &required) == 2);
    CHECK(info.copy_adapters(out.data(), out.size(), nullptr) == 2);
}

// A republish replaces the snapshot atomically — the reader sees either the
// old string whole or the new string whole, never a mixture, and the two
// slots do not bleed into each other.
static void test_republish_replaces_and_slots_are_independent() {
    TxRuntimeInfo info;
    size_t required = 0;
    std::array<char, 32> out{};
    info.publish_status("{\"v\":1}");
    info.publish_status("{\"v\":2}");
    CHECK(info.copy_status(out.data(), out.size(), &required) == 0);
    CHECK(std::strcmp(out.data(), "{\"v\":2}") == 0);
    // The adapters slot is still unpublished.
    CHECK(info.copy_adapters(nullptr, 0, &required) == 3);
}

int main() {
    test_not_ready_before_publication();
    test_publish_and_copy_contract();
    test_republish_replaces_and_slots_are_independent();
    return wbtest_finish("tx_runtime_info_test");
}
