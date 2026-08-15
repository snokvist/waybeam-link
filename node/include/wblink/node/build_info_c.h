/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What this build of the library can actually do (Pass 180).
 *
 * A capability answer, not a version. There is deliberately no version
 * string anywhere in this project — the install rules refuse to fabricate
 * one because "a fabricated one would let a consumer write a compatibility
 * constraint that means nothing" — and the same reasoning applies here, with
 * a sharper edge: the failures a consumer needs to prevent are CAPABILITY
 * failures, not vintage mismatches.
 *
 * The one that has actually bitten: waybeam-hub configured
 * `pixelpilot.frame_shm.source: wblink` against a library built
 * WBLINK_FRAME_SHM=OFF. Every counter reports healthy and the screen stays
 * black forever, because the source that was selected cannot exist. A hub
 * that reads this at startup can refuse that configuration by name instead.
 *
 * THE STRING IS BAKED INTO THE ARCHIVE, not recomputed from the caller's
 * macros — that is the whole point. It describes the library a consumer
 * LINKED, not the headers it compiled against. waybeam-hub consumes this
 * tree by sibling path and its Makefile passes only `-I.../node/include`, so
 * its own translation units see none of the feature macros and could not
 * compute an expectation to compare; what it can do is ask the archive.
 *
 * What it does NOT catch, stated so nobody assumes otherwise: waybeam-hub's
 * `WBLINK=1` stale-object trap. A `hub_main.o` compiled without the define
 * registers no module at all, and nothing is left to call this.
 */
#ifndef WBLINK_NODE_BUILD_INFO_C_H
#define WBLINK_NODE_BUILD_INFO_C_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A JSON object, NUL-terminated, with static storage — never freed, always
 * valid, safe from any thread and before any handle exists:
 *
 *   {"frame_shm":B,"control_server":B,"venc":B,"radio":B,"node_tx":B}
 *
 * Field meanings are the CMake options of the same name.
 * `node_tx` is the one that is not an option but a consequence: the TX half
 * of the archive (`wblink_tx_*`, run_tx) is compiled only when frame_shm,
 * control_server and venc are ALL on, so a consumer that needs a
 * transmitter should read this field rather than infer it from the other
 * three and get the rule subtly wrong.
 */
const char *wblink_build_info(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WBLINK_NODE_BUILD_INFO_C_H */
