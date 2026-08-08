// SPDX-License-Identifier: GPL-2.0-or-later
// The registry's own invariants, plus the serialiser's shape.
//
// There is deliberately no golden file. Every fact a golden would hold is
// (path, type) — precisely the two fields io/src/config_registry.cpp declares
// one per line, in the same order — so it could not detect any change the
// registry's own diff does not already show. It would only duplicate the
// merge-conflict surface and add a regenerate-the-file ritual. What a golden
// covered uniquely was the output FORMAT, which is asserted directly below.
//
// The invariants are the part that earns its keep: they caught three entries
// typed as scalars that the loader binds as containers.
#include "wblink/config_registry.h"

#include <cstdio>
#include <string>
#include <vector>

#include "wbtest.h"

using namespace wblink;

int main() {
    // Serialiser shape. config_schema_json() concatenates strings by hand, so
    // these pin the envelope a consumer parses.
    const std::string live = config_schema_json();
    CHECK(live.rfind("{\n  \"version\": 1,\n  \"keys\": [\n", 0) == 0);
    CHECK(live.size() > 6);
    CHECK(live.compare(live.size() - 6, 6, "  ]\n}\n") == 0);
    CHECK(live.find("\"path\": \"adapters\", \"type\": \"array\"") !=
          std::string::npos);
    CHECK(live.find('\t') == std::string::npos);

    std::size_t n = 0;
    const KeyEntry* keys = config_registry(&n);
    CHECK(n > 200);  // the surface is ~263; a collapse to a handful is a bug

    // Every entry reaches the output exactly once — the loop that builds it
    // is hand-written, so an off-by-one there would publish a short schema
    // that still parses as valid JSON.
    std::size_t emitted = 0;
    for (std::size_t at = live.find("{\"path\": \""); at != std::string::npos;
         at = live.find("{\"path\": \"", at + 1)) {
        ++emitted;
    }
    CHECK(emitted == n);

    // Sorted and unique: config_schema_json() promises a stable order, and a
    // duplicate path would make the loader/registry comparison in
    // config_registry_test.py pass while the schema published two entries.
    for (std::size_t i = 1; i < n; ++i) {
        const bool ordered = std::string(keys[i - 1].path) < std::string(keys[i].path);
        CHECK(ordered);
        if (!ordered) {
            std::fprintf(stderr, "  not sorted/unique: %s then %s\n",
                         keys[i - 1].path, keys[i].path);
        }
    }

    // Path well-formedness. config_schema_json() concatenates `path` into JSON
    // without escaping, and config_registry_test.py's `[^"]+` regex reads it
    // back, so restricting the alphabet is what makes both safe rather than
    // merely true today. Subsumes the "no space" and "no .." checks.
    for (std::size_t i = 0; i < n; ++i) {
        const std::string p = keys[i].path;
        CHECK(!p.empty());
        CHECK(p.front() != '.' && p.back() != '.');
        CHECK(p.find("..") == std::string::npos);
        bool charset_ok = true;
        for (const char c : p) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                            c == '[' || c == ']';
            if (!ok) charset_ok = false;
        }
        CHECK(charset_ok);
        if (!charset_ok) std::fprintf(stderr, "  bad charset: %s\n", p.c_str());
    }

    // Every parent of a dotted path is registered AND has a container type.
    //
    // The type half is not decoration: three entries shipped in review as
    // kNumber while the loader binds them as an object or requires an array
    // (policy.rx, loopback.rssi_fade, policy.select.rung_rssi_floor_dbm). Two
    // of the three had registered children, so this check alone would have
    // caught them — a container that something is nested inside cannot be a
    // scalar. Machine extraction defaults an unresolved type to kNumber, so
    // this is the guard against that whole class recurring.
    std::vector<std::string> paths;
    paths.reserve(n);
    for (std::size_t i = 0; i < n; ++i) paths.emplace_back(keys[i].path);
    for (std::size_t i = 0; i < n; ++i) {
        const std::string p = paths[i];
        const size_t dot = p.rfind('.');
        if (dot == std::string::npos) continue;
        std::string parent = p.substr(0, dot);
        // "adapters[].bus" -> the parent is the ARRAY "adapters"; a plain
        // "policy.rx.admit_n" -> the parent is the OBJECT "policy.rx".
        const bool parent_is_array =
            parent.size() > 2 && parent.compare(parent.size() - 2, 2, "[]") == 0;
        if (parent_is_array) parent.erase(parent.size() - 2);
        const KeyType want = parent_is_array ? KeyType::kArray : KeyType::kObject;

        bool found = false;
        for (std::size_t k = 0; k < n; ++k) {
            if (paths[k] != parent) continue;
            found = true;
            CHECK(keys[k].type == want);
            if (keys[k].type != want) {
                std::fprintf(stderr,
                             "  %s holds %s, so it must be declared %s\n",
                             parent.c_str(), p.c_str(),
                             parent_is_array ? "kArray" : "kObject");
            }
            break;
        }
        CHECK(found);
        if (!found) std::fprintf(stderr, "  orphan: %s (no parent %s)\n",
                                 p.c_str(), parent.c_str());
    }

    return wbtest_finish("config_schema");
}
