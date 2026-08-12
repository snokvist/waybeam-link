/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The C half of the link gate (#109 Phase 3 prep).
 *
 * waybeam-hub is a C daemon and Android reaches native code through JNI, so
 * `rx_node_c.h` has to be C-clean — and nothing in a C++ build can show that.
 * A `std::`, a default argument, a `bool` without <stdbool.h>, an `extern "C"`
 * forgotten around a declaration: every one of them compiles fine in C++ and
 * fails here.
 *
 * Compiled as C, linked into the same executable as the C++ side, which also
 * proves the two link together — the name mangling half of the ABI.
 */
#include "wblink/node/rx_node_c.h"
#include "wblink/node/tx_node_c.h"

#include <stdio.h>

static void on_frame(uint8_t stream_id, const uint8_t *frame, size_t len,
                     void *user) {
    (void)frame;
    unsigned long *count = (unsigned long *)user;
    ++*count;
    printf("c_consumer: stream %u, %zu bytes\n", (unsigned)stream_id, len);
}

/* The other direction of the ABI: the type a TX consumer passes IN, carrying
 * the double-fork B9 keeps out of node/. Defined so the typedef has something
 * to bind, never called — see the note on the TX half below. */
static int on_mode_apply(const char *cmd, const char *name, void *user) {
    (void)cmd;
    (void)name;
    unsigned long *count = (unsigned long *)user;
    ++*count;
    return 1;
}

/* Exercises the handle's lifetime without opening a radio: create, ask to
 * stop, destroy. `wblink_rx_run` is referenced but never called — it blocks. */
int wblink_c_consumer_check(void) {
    wblink_rx *rx = wblink_rx_create();
    if (rx == NULL) {
        return 1;
    }
    wblink_rx_request_stop(rx); /* legal before run, per the header */
    /* The Android device source. Referenced here because this gate is a LINK,
     * not a build: a static archive does not resolve its own undefined
     * symbols, so a function nothing calls is a function nothing proves. Both
     * argument-validation paths, neither of which allocates or opens
     * anything. */
    if (wblink_rx_set_adapter_fds(NULL, NULL, 0) != 2) {
        wblink_rx_destroy(rx);
        return 1; /* NULL handle must be refused */
    }
    if (wblink_rx_set_adapter_fds(rx, NULL, 1) != 2) {
        wblink_rx_destroy(rx);
        return 1; /* NULL array with n > 0 must be refused */
    }
    if (wblink_rx_set_adapter_fds(rx, NULL, 0) != 0) {
        wblink_rx_destroy(rx);
        return 1; /* clearing is legal */
    }
    /* Runtime control: reference every additive symbol in the C translation
     * unit and prove an inactive handle fails without touching node state. */
    {
        uint64_t generation = 0;
        size_t required = 0;
        if (wblink_rx_scout_start(NULL, NULL, 0, 0, &generation) != 2 ||
            wblink_rx_scout_start(rx, NULL, 0, 0, &generation) != 3 ||
            wblink_rx_scout_stop(rx, &generation) != 3 ||
            wblink_rx_scout_select(rx, 1, &generation) != 3 ||
            wblink_rx_scout_results(rx, NULL, 0, &required,
                                    &generation) != 3 ||
            wblink_rx_discovery(rx, NULL, 0, &required) != 3 ||
            wblink_rx_selection(rx, NULL, 0, &required,
                                &generation) != 3) {
            wblink_rx_destroy(rx);
            return 1;
        }
    }
    wblink_rx_destroy(rx);

    /* Two contract rules, asserted here because a PRE-STOPPED handle runs
     * nothing: no config is read and no radio is opened, so the gate can
     * exercise them without hardware. Both were review findings — a re-run
     * used to return a healthy-looking 0 after claiming the adapter, and a
     * pre-stop used to open the radio before noticing.
     *
     * The config path is deliberately nonexistent: reaching it would mean the
     * pre-stop check did not fire, and this turns that into a failure. */
    rx = wblink_rx_create();
    if (rx == NULL) {
        return 1;
    }
    unsigned long frames = 0;
    wblink_rx_request_stop(rx);
    if (wblink_rx_run(rx, "/nonexistent/wblink-linkcheck.json", on_frame,
                      &frames) != 0) {
        wblink_rx_destroy(rx);
        return 1; /* pre-stop must return 0 without touching the config */
    }
    if (wblink_rx_run(rx, "/nonexistent/wblink-linkcheck.json", on_frame,
                      &frames) != 3) {
        wblink_rx_destroy(rx);
        return 1; /* a handle runs once; reuse must be refused, not ignored */
    }
    if (frames != 0) {
        wblink_rx_destroy(rx);
        return 1;
    }
    wblink_rx_destroy(rx);

    /* The TX half of the ABI is DECLARATION-ONLY here, and deliberately so.
     * This project configures frame-SHM, the control server and venc OFF —
     * that is its whole purpose — and `run_tx` uses all three, so `wblink_tx_*`
     * is not in this archive at all. Calling it would not test the TX ABI; it
     * would test that the OFF configuration is not really OFF.
     *
     * What still belongs here is everything a C compiler can check without a
     * link: that the header parses as C11 with -Wpedantic, that the callback
     * type is C-nameable, and that the shim's failure codes stay OUT of
     * run_tx's status space — 2 there is the §9.10 wedge a supervisor restarts
     * the radio on, so a bad argument reported as 2 would be a power-cycle in
     * response to a NULL pointer.
     *
     * The link and the runtime contract are proven where the symbols exist:
     * tests/tx_node_c_test.cpp, guarded on the same three subsystems. */
    {
        /* Both types must be nameable in C, and the callback must be
         * assignable to the typedef — that is what -Wstrict-prototypes and
         * -Wpedantic check here, and it is a compile-time claim, not a runtime
         * one. Deliberately NOT calling `apply`: it is this file's own static
         * function, so calling it would exercise the test rather than the
         * library. */
        wblink_mode_apply_cb apply = on_mode_apply;
        wblink_tx *tx = NULL;
        (void)apply;
        (void)tx;
    }
    if (WBLINK_TX_BAD_ARG >= 0 || WBLINK_TX_REUSED >= 0 ||
        WBLINK_TX_BAD_ARG == WBLINK_TX_REUSED) {
        return 1;
    }
    return 0;
}
