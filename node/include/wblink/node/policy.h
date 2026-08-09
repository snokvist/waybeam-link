// SPDX-License-Identifier: GPL-2.0-or-later
// §15.2 Config → core engine parameter blocks (#109 Phase 2c).
//
// Siblings of the five adapters already in tx_core.h (`s_to_ms`,
// `selector_policy`, `calib_params_from`, `bw_code`, `scheduler_policy`).
// These three are the ones BOTH loops need, so they cannot live in a header
// named for the TX half. `channel_allowed` rides along because the list it
// tests is `policy.csa.channel_allowlist` — the same block `csa_params`
// translates.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "wblink/config.h"
#include "wblink/csa.h"
#include "wblink/quietgap.h"
#include "wblink/vehicle_cmd.h"

namespace wblink {
namespace node {

// §15.2 policy.csa → the core engine's parameter block (string PSK to raw
// bytes, seconds to ms).
inline CsaParams csa_params(const Config& cfg) {
    const CsaPolicy& c = cfg.policy.csa;
    CsaParams p;
    p.psk.assign(c.psk.begin(), c.psk.end());
    p.settle_ms = static_cast<uint32_t>(c.settle_s * 1000.0);
    p.verify_timeout_ms = c.verify_timeout_ms;
    p.min_interval_ms = c.min_interval_s * 1000;
    p.ack_timeout_ms = c.ack_timeout_ms;
    p.bind_release_ms = c.bind_release_s * 1000;
    p.allowlist = c.channel_allowlist;
    // §11.4a (Pass 85): only a passive spectator may follow an unauthenticated
    // CSA. Craft/ground with an empty key are FAULTED, not unauthenticated,
    // and fail closed — the permission rides the role, never an empty buffer.
    p.allow_unauthenticated = cfg.node.spectator;
    return p;
}

// §15.2 policy.cmd → the §11.7 engine parameter block. The key follows the
// §11.4a CSA provenance (secret here; announced token keyed by the caller).
inline VcmdParams vcmd_params(const Config& cfg) {
    const CmdPolicy& c = cfg.policy.cmd;
    VcmdParams p;
    p.psk.assign(cfg.policy.csa.psk.begin(), cfg.policy.csa.psk.end());
    p.copies = static_cast<uint8_t>(c.copies);
    p.copy_interval_ms = c.copy_interval_ms;
    p.echo_copies = static_cast<uint8_t>(c.echo_copies);
    p.ack_timeout_ms = c.ack_timeout_ms;
    p.retry_cap = static_cast<uint8_t>(c.retry_cap);
    p.min_interval_ms = c.min_interval_ms;
    return p;
}

inline bool channel_allowed(const std::vector<uint16_t>& allowlist, uint16_t chan) {
    return std::find(allowlist.begin(), allowlist.end(), chan) !=
           allowlist.end();
}

inline QuietGapPolicy quietgap_policy(const Config& cfg) {
    QuietGapPolicy p;
    p.enabled = cfg.policy.ret.quiet_gap;
    p.guard_us = cfg.policy.ret.guard_us;
    p.window_us = cfg.policy.ret.return_window_us;
    return p;
}

}  // namespace node
}  // namespace wblink
