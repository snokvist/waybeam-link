// SPDX-License-Identifier: GPL-2.0-or-later
// The node's loaded configuration, and the loader that builds it.
//
// Moved out of `app/main.cpp` and out of `stats_fill.h` (#109 Phase 3 prep).
// `Loaded` landed in the §15.3 assembly header during Phase 2a because that is
// where `emit_stats` needed it, not because it belongs there — it is the input
// to `run_rx`, so every consumer of the layer builds one, and none of them
// should have to include the stats assembly to do it.
//
// `load_all` is the §9.7 pin validation as much as it is a loader: a
// `policy.select.min_profile`/`max_profile` naming an id the table does not
// contain is a config error, never a silent clamp onto a neighbouring rung.
// Keeping that beside the struct is the point of moving it.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/.
#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "wblink/config.h"
#include "wblink/log.h"
#include "wblink/table.h"

namespace wblink {
namespace node {

struct Loaded {
    Config cfg;
    ProfileTable table;
    bool have_table = false;
    uint8_t tv = 0;
};

// The half that runs once a Config exists, whatever produced it. Split out
// by Pass 179 so a config supplied as TEXT and a config read from a PATH are
// validated by the same code rather than by two that drift.
//
// Diagnostics go through wb_logf, not fprintf(stderr): on Android stderr is
// a place messages go to die, and an embedder that installed a log sink
// (#144) was still losing exactly the errors it most needs — a config that
// did not parse and a profile pin that names a rung the table lacks.
inline int load_finish(Loaded& out) {
    wb_logf("%s", dump_config_summary(out.cfg).c_str());
    if (!out.cfg.profile_table_path.empty()) {
        auto table = load_profile_table(out.cfg.profile_table_path);
        if (!table) {
            wb_logf("profile table error: %s\n", table.error.c_str());
            return 1;
        }
        out.table = std::move(*table.value);
        out.have_table = true;
        out.tv = table_version(out.table);
        wb_logf("profile table: %zu profiles, table_version=0x%02X\n",
                out.table.profiles.size(), out.tv);
        // §9.7 (Pass 83): min/max_profile are profile IDs. An id absent from
        // the table is a config error, not a silent clamp onto a neighbouring
        // rung — the operator asked for an operating envelope that this table
        // cannot express. 255 is the documented "unpinned top" sentinel.
        const SelectPolicy& sel = out.cfg.policy.select;
        const auto has_id = [&out](uint8_t id) {
            for (const Profile& p : out.table.profiles) {
                if (p.id == id) return true;
            }
            return false;
        };
        const std::pair<const char*, uint8_t> pins[] = {
            {"min_profile", sel.min_profile},
            {"max_profile", sel.max_profile}};
        for (const auto& [name, id] : pins) {
            if (id != 255 && !has_id(id)) {
                wb_logf("config error: policy.select.%s = %u is not a "
                        "profile id in %s (§9.7 ids, not indices)\n",
                        name, static_cast<unsigned>(id),
                        out.cfg.profile_table_path.c_str());
                return 1;
            }
        }
    }
    return 0;
}

inline int load_all(const std::string& config_path, Loaded& out) {
    auto cfg = load_config(config_path);
    if (!cfg) {
        wb_logf("config error: %s\n", cfg.error.c_str());
        return 1;
    }
    out.cfg = std::move(*cfg.value);
    return load_finish(out);
}

// Pass 179: the same loader, given the config as TEXT. An embedder that
// composes its config in memory (Android extracts one from its assets) does
// not have to own a file to start a node.
inline int load_all_json(const std::string& config_json, Loaded& out) {
    auto cfg = load_config_json(config_json);
    if (!cfg) {
        wb_logf("config error: %s\n", cfg.error.c_str());
        return 1;
    }
    out.cfg = std::move(*cfg.value);
    return load_finish(out);
}

// Pass 179: pin a scouted craft into a loaded config — the three fields a
// §15.5a selection resolves. Applied after load and before the run, so a
// consumer never has to rewrite the config JSON (and never has to know that
// these are called preferred_originator / net_id / channel).
//
// EVERY adapter moves, not adapters[0]: the runtime select retunes all of a
// spectator's ears onto the craft, and a pre-start selection that moved one
// would build a diversity node listening in two places for a craft that is
// only ever in one.
struct Selection {
    uint16_t originator = 0;
    uint8_t net_id = 0;
    uint16_t channel_mhz = 0;
};

inline void apply_selection(const Selection& sel, Loaded& out) {
    out.cfg.node.preferred_originator = sel.originator;
    out.cfg.node.net_id = sel.net_id;
    for (AdapterCfg& a : out.cfg.adapters) a.channel_mhz = sel.channel_mhz;
}

}  // namespace node
}  // namespace wblink
