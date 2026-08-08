// SPDX-License-Identifier: GPL-2.0-or-later
// Touches both layers so the link is real, not just a CMake dependency edge.
#include "wblink/air_radio.h"
#include "wblink/wire.h"

namespace {
int probe() {
    wblink::RadioAirCfg cfg;
    cfg.adapter_fds = {-1};
    return static_cast<int>(cfg.adapter_fds.size());
}
}  // namespace

extern "C" int wblink_embed_probe(void) { return probe(); }
