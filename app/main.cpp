// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link — one portable binary, modes tx / rx / loopback (PROTOCOL.md
// §16.1). Build-order state: steps 1–2 (codec + I/O/config/stats) are live;
// the mode engines land in steps 3–6. Until then each mode validates its
// config end-to-end and reports itself unimplemented.
#include <cstdio>
#include <cstring>
#include <string>

#include "wblink/binding.h"
#include "wblink/config.h"
#include "wblink/table.h"

namespace {

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <tx|rx|loopback> -c <config.json> [--check]\n"
                 "  --check  validate config + bindings and exit\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage(argv[0]);
    }
    const std::string mode = argv[1];
    if (mode != "tx" && mode != "rx" && mode != "loopback") {
        return usage(argv[0]);
    }
    std::string config_path;
    bool check_only = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else {
            return usage(argv[0]);
        }
    }
    if (config_path.empty()) {
        return usage(argv[0]);
    }

    auto cfg = wblink::load_config(config_path);
    if (!cfg) {
        std::fprintf(stderr, "config error: %s\n", cfg.error.c_str());
        return 1;
    }

    std::fputs(wblink::dump_config_summary(*cfg.value).c_str(), stderr);

    if (!cfg.value->profile_table_path.empty()) {
        auto table = wblink::load_profile_table(cfg.value->profile_table_path);
        if (!table) {
            std::fprintf(stderr, "profile table error: %s\n",
                         table.error.c_str());
            return 1;
        }
        std::fprintf(stderr, "profile table: %zu profiles, table_version=0x%02X\n",
                     table.value->profiles.size(),
                     wblink::table_version(*table.value));
    }

    // Open the bindings so a config that names an unbindable port fails here,
    // not at first traffic.
    auto bindings = wblink::BindingSet::create(*cfg.value);
    if (!bindings) {
        std::fprintf(stderr, "binding error: %s\n", bindings.error.c_str());
        return 1;
    }
    std::fprintf(stderr, "bindings: OK\n");

    if (check_only) {
        return 0;
    }

    const char* step = mode == "tx" ? "3" : mode == "rx" ? "4" : "6";
    std::fprintf(stderr,
                 "mode '%s' is not implemented yet (build-order step %s); "
                 "config OK\n",
                 mode.c_str(), step);
    return 0;
}
