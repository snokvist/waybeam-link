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
#include <cstdio>
#include <string>
#include <utility>

#include "wblink/config.h"
#include "wblink/table.h"

namespace wblink {
namespace node {

struct Loaded {
    Config cfg;
    ProfileTable table;
    bool have_table = false;
    uint8_t tv = 0;
};

inline int load_all(const std::string& config_path, Loaded& out) {
    auto cfg = load_config(config_path);
    if (!cfg) {
        std::fprintf(stderr, "config error: %s\n", cfg.error.c_str());
        return 1;
    }
    out.cfg = std::move(*cfg.value);
    std::fputs(dump_config_summary(out.cfg).c_str(), stderr);
    if (!out.cfg.profile_table_path.empty()) {
        auto table = load_profile_table(out.cfg.profile_table_path);
        if (!table) {
            std::fprintf(stderr, "profile table error: %s\n",
                         table.error.c_str());
            return 1;
        }
        out.table = std::move(*table.value);
        out.have_table = true;
        out.tv = table_version(out.table);
        std::fprintf(stderr,
                     "profile table: %zu profiles, table_version=0x%02X\n",
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
                std::fprintf(stderr,
                             "config error: policy.select.%s = %u is not a "
                             "profile id in %s (§9.7 ids, not indices)\n",
                             name, static_cast<unsigned>(id),
                             out.cfg.profile_table_path.c_str());
                return 1;
            }
        }
    }
    return 0;
}

}  // namespace node
}  // namespace wblink
