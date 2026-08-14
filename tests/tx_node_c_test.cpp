// SPDX-License-Identifier: GPL-2.0-or-later
//
// The TX C ABI's link and runtime contract (#109 Phase 3).
//
// examples/node-linkcheck cannot hold this. That gate configures frame-SHM, the
// control server and venc OFF, and `run_tx` uses all three, so `wblink_tx_*` is
// not in that archive — it proves the header is C-clean and stops there. This
// test is the other half: it runs in the configuration a transmitter actually
// has, so it LINKS the four symbols and exercises the handle contract.
//
// Everything here is reachable without a radio, because a pre-stopped or
// mis-called handle opens nothing. That is deliberate: the rules below
// are exactly the ones that would otherwise only fail on hardware, at which
// point a dead node looks healthy.
#include "wblink/node/tx_node_c.h"

#include "wbtest.h"

namespace {
int g_applies = 0;

// A path that cannot exist: reaching it means a guard above did not fire, and
// the assertions below turn that into a failure rather than a silent pass.
constexpr const char* kNoSuchConfig = "/nonexistent/wblink-tx-c-test.json";
}  // namespace

// C language linkage on purpose, and at namespace scope because that is where
// it is legal: the parameter type is a C function pointer, and a C++-linkage
// function is a different type even where compilers accept the conversion.
extern "C" int wblink_tx_c_test_apply(const char* cmd, const char* name,
                                      void* user) {
    (void)cmd;
    (void)name;
    ++*static_cast<int*>(user);
    return 1;
}

int main() {
    // The status spaces must not overlap. WBLINK_TX_WEDGED is the §9.10 wedge
    // (Pass 148) that a supervisor answers by restarting the transmitter, so a
    // shim-level failure sharing its value would mean a NULL pointer triggers a
    // radio restart. Checked here as well as in the C gate because the two
    // headers can be edited independently.
    CHECK(WBLINK_TX_OK == 0);
    CHECK(WBLINK_TX_ERROR == 1);
    CHECK(WBLINK_TX_WEDGED == 2);
    CHECK(WBLINK_TX_BAD_ARG < 0);
    CHECK(WBLINK_TX_REUSED < 0);
    CHECK(WBLINK_TX_BAD_ARG != WBLINK_TX_REUSED);

    // NULL arguments are refused without touching anything.
    CHECK(wblink_tx_run(nullptr, kNoSuchConfig, wblink_tx_c_test_apply, &g_applies) ==
          WBLINK_TX_BAD_ARG);

    wblink_tx* tx = wblink_tx_create();
    CHECK(tx != nullptr);
    CHECK(wblink_tx_run(tx, nullptr, wblink_tx_c_test_apply, &g_applies) ==
          WBLINK_TX_BAD_ARG);
    // Pass 173 device-source contract, same shape as the RX twin: NULL handle
    // and NULL fds with n > 0 are refused (2); a valid set and an n == 0
    // clear both succeed before the run.
    {
        const int fds[2] = {-1, -1};
        CHECK(wblink_tx_set_adapter_fds(nullptr, fds, 2) == 2);
        CHECK(wblink_tx_set_adapter_fds(tx, nullptr, 1) == 2);
        CHECK(wblink_tx_set_adapter_fds(tx, fds, 2) == 0);
        CHECK(wblink_tx_set_adapter_fds(tx, nullptr, 0) == 0);
    }
    // Pass 174 snapshot calls: bad args → 2, and not-ready → 3 before any
    // run — no backend has published, so both surfaces must say so rather
    // than hand back an empty string that parses.
    {
        size_t required = 77;
        char buf[8];
        CHECK(wblink_tx_adapters(nullptr, nullptr, 0, &required) == 2);
        CHECK(wblink_tx_status(nullptr, nullptr, 0, &required) == 2);
        CHECK(wblink_tx_adapters(tx, buf, sizeof buf, nullptr) == 2);
        CHECK(wblink_tx_adapters(tx, nullptr, 0, &required) == 3);
        CHECK_EQ_U(required, 0);
        required = 77;
        CHECK(wblink_tx_status(tx, nullptr, 0, &required) == 3);
        CHECK_EQ_U(required, 0);
    }
    // A refused call must NOT have consumed the handle — otherwise a caller who
    // passed a null path once could never start this node, and the next run
    // would report reuse rather than the real mistake.
    wblink_tx_request_stop(tx);
    CHECK(wblink_tx_run(tx, kNoSuchConfig, wblink_tx_c_test_apply, &g_applies) ==
          WBLINK_TX_OK);
    // Stopped before it started: no config was read. If the pre-stop check had
    // not fired, a nonexistent path would have produced WBLINK_TX_ERROR.
    CHECK(wblink_tx_run(tx, kNoSuchConfig, wblink_tx_c_test_apply, &g_applies) ==
          WBLINK_TX_REUSED);
    // Pass 173: after the handle has run, the setter reports the violated
    // call-before-run contract (3) instead of accepting fds nothing will read.
    {
        const int fd = -1;
        CHECK(wblink_tx_set_adapter_fds(tx, &fd, 1) == 3);
    }
    // Pass 174: a run that never reached the backend published nothing —
    // still 3, not an empty success.
    {
        size_t required = 77;
        CHECK(wblink_tx_adapters(tx, nullptr, 0, &required) == 3);
        CHECK(wblink_tx_status(tx, nullptr, 0, &required) == 3);
    }
    wblink_tx_destroy(tx);

    // A fresh handle with a real (still nonexistent) path: load fails, and the
    // failure must be reported as ERROR, never as WEDGED. load_all returns 1
    // today and this would pass by coincidence if the code forwarded it, so the
    // value being checked is the one that matters if load_all's codes change.
    tx = wblink_tx_create();
    CHECK(tx != nullptr);
    CHECK(wblink_tx_run(tx, kNoSuchConfig, wblink_tx_c_test_apply, &g_applies) ==
          WBLINK_TX_ERROR);
    wblink_tx_destroy(tx);

    // Nothing above should have reached the flight loop, so the mode applier
    // must never have been called. A nonzero count here means one of the guards
    // let a run proceed.
    CHECK(g_applies == 0);

    // Destroying a null handle is a no-op, as free() is.
    wblink_tx_destroy(nullptr);
    wblink_tx_request_stop(nullptr);

    // NOT `return 0`. CHECK only counts failures (tests/wbtest.h); the exit code
    // comes from here alone, so a main() that returns 0 directly runs every
    // assertion, prints every failure, and reports success to CTest — a test
    // that cannot fail. Caught by pre-merge review on this very file.
    return wbtest_finish("tx_node_c_test");
}
