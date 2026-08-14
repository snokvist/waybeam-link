/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Lifecycle state shared by both C handles (Pass 177). One header rather
 * than a copy in each, because the two node headers can be edited
 * independently and these three values must not drift: an embedder's
 * supervisor switches on them.
 *
 * The state machine is deliberately three-valued. There is no WEDGED state —
 * a wedge is EXITED with WBLINK_TX_WEDGED as the exit rc, because the
 * role-specific return spaces are already the law (tx_node_c.h note 2) and a
 * duplicate state would be a second decoder for the same fact. And there is
 * no reason STRING — the rc is the machine-actionable reason; human-readable
 * causes are log lines and reach the embedder through wb_log_set_sink.
 *
 * Transitions are owned by the run wrapper: CREATED until the one-run claim
 * succeeds, RUNNING while the run call is in flight (config load included),
 * EXITED on every return after the claim — a stop-before-start run exits 0,
 * a config-load failure exits with the role's error code. A NULL-argument or
 * reused-handle refusal never ran and never transitions.
 */
#ifndef WBLINK_NODE_NODE_STATE_C_H
#define WBLINK_NODE_NODE_STATE_C_H

enum {
    WBLINK_NODE_CREATED = 0, /* handle exists; run has not been claimed */
    WBLINK_NODE_RUNNING = 1, /* the run call is in flight */
    WBLINK_NODE_EXITED = 2   /* the run returned; its rc is readable */
};

#endif /* WBLINK_NODE_NODE_STATE_C_H */
