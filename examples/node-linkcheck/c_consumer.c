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

#include <stdio.h>

static void on_frame(uint8_t stream_id, const uint8_t *frame, size_t len,
                     void *user) {
    (void)frame;
    unsigned long *count = (unsigned long *)user;
    ++*count;
    printf("c_consumer: stream %u, %zu bytes\n", (unsigned)stream_id, len);
}

/* Exercises the handle's lifetime without opening a radio: create, ask to
 * stop, destroy. `wblink_rx_run` is referenced but never called — it blocks. */
int wblink_c_consumer_check(void) {
    wblink_rx *rx = wblink_rx_create();
    if (rx == NULL) {
        return 1;
    }
    wblink_rx_request_stop(rx); /* legal before run, per the header */
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
    return 0;
}
