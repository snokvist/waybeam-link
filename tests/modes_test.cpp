// SPDX-License-Identifier: GPL-2.0-or-later
// §15.5 Pass 104: GET /api/v1/modes catalog. modes_catalog_json enumerates a
// directory of modes/<name>.json (docs/venc-mode-matrix.md §16) into the wire
// object: valid modes surface name-sorted with their user-facing facts; missing,
// malformed, or partial files are skipped (never fatal); non-.json entries are
// ignored; an empty/unreadable dir yields an empty modes[].
#include "wblink/modes.h"

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "wbtest.h"

using namespace wblink;

namespace {

void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << body;
}

// A well-formed §16 mode file.
std::string mode_json(int fps, const char* size, int mn, int mx,
                      const char* fps_mode) {
    nlohmann::json j;
    j["venc"]["sensor"]["mode"] = 3;
    j["venc"]["video0"]["fps"] = fps;
    j["venc"]["video0"]["size"] = size;
    j["link"]["policy"]["select"]["min_profile"] = mn;
    j["link"]["policy"]["select"]["max_profile"] = mx;
    j["link"]["policy"]["fps_mode"] = fps_mode;
    return j.dump();
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/wblink_modes_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    CHECK(dir != nullptr);
    const std::string d = dir;

    // Two valid modes (one static, one variable/§9.11)...
    write_file(d + "/imx335-100fps-highrange.json",
               mode_json(100, "1024x576", 0, 2, "static"));
    write_file(d + "/imx335-variable.json",
               mode_json(100, "1280x720", 0, 5, "variable"));
    // ...a malformed file (skipped)...
    write_file(d + "/broken.json", "{ this is not json ");
    // ...a valid-JSON but incomplete mode, no link.policy.select (skipped)...
    write_file(d + "/partial.json", "{\"venc\":{\"video0\":{\"fps\":60}}}");
    // ...and a non-.json entry (ignored, never opened).
    write_file(d + "/README.md", "not a mode");

    // Catalog: active label + apply_configured pass through; only the two valid
    // modes appear, name-sorted (highrange < variable).
    {
        const auto j = nlohmann::json::parse(
            modes_catalog_json(d, "imx335-variable", true));
        CHECK(j.at("active").get<std::string>() == "imx335-variable");
        CHECK(j.at("apply_configured").get<bool>() == true);
        const auto& m = j.at("modes");
        CHECK_EQ_U(m.size(), 2);

        CHECK(m[0].at("name").get<std::string>() == "imx335-100fps-highrange");
        CHECK_EQ_U(m[0].at("fps").get<int>(), 100);
        CHECK(m[0].at("resolution").get<std::string>() == "1024x576");
        CHECK_EQ_U(m[0].at("mcs_min").get<int>(), 0);
        CHECK_EQ_U(m[0].at("mcs_max").get<int>(), 2);
        CHECK(m[0].at("fps_mode").get<std::string>() == "static");

        CHECK(m[1].at("name").get<std::string>() == "imx335-variable");
        CHECK_EQ_U(m[1].at("mcs_max").get<int>(), 5);
        CHECK(m[1].at("fps_mode").get<std::string>() == "variable");
    }

    // An empty dir string → empty catalog, but the label/flag still round-trip.
    {
        const auto j =
            nlohmann::json::parse(modes_catalog_json("", "whatever", false));
        CHECK(j.at("active").get<std::string>() == "whatever");
        CHECK(j.at("apply_configured").get<bool>() == false);
        CHECK_EQ_U(j.at("modes").size(), 0);
    }

    // A nonexistent dir → empty catalog, not a crash.
    {
        const auto j = nlohmann::json::parse(
            modes_catalog_json(d + "/does-not-exist", "x", true));
        CHECK_EQ_U(j.at("modes").size(), 0);
    }

    // fps_mode defaults to "static" when the mode file omits it (§9.11 default).
    {
        nlohmann::json j;
        j["venc"]["video0"]["fps"] = 30;
        j["venc"]["video0"]["size"] = "1920x1080";
        j["link"]["policy"]["select"]["min_profile"] = 2;
        j["link"]["policy"]["select"]["max_profile"] = 5;  // no fps_mode key
        write_file(d + "/imx335-30fps-lowrange.json", j.dump());
        const auto out = nlohmann::json::parse(
            modes_catalog_json(d, "", true));
        CHECK_EQ_U(out.at("modes").size(), 3);
        // Name-sorted by byte value: "100fps" < "30fps" ('1' < '3'), so the
        // 30fps mode lands at index 1, after imx335-100fps-highrange.
        CHECK(out["modes"][1].at("name").get<std::string>() ==
              "imx335-30fps-lowrange");
        CHECK(out["modes"][1].at("fps_mode").get<std::string>() == "static");
    }

    // §11.7 0x07 MODE (Pass 105): mode_name_at indexes the SAME name-sorted,
    // malformed-skipped enumeration the catalog is built from — so a ground
    // that picked ordinal i from GET /api/v1/modes and the craft that applies
    // it resolve to the same mode. An index past the end (and an empty/bad dir)
    // yields "" → the §11.7 range-error REJECTED.
    {
        const auto out = nlohmann::json::parse(modes_catalog_json(d, "", true));
        const auto& m = out.at("modes");
        CHECK_EQ_U(m.size(), 3);
        for (size_t i = 0; i < m.size(); ++i) {
            CHECK(mode_name_at(d, i) == m[i].at("name").get<std::string>());
        }
        CHECK(mode_name_at(d, m.size()).empty());  // one past the end
        CHECK(mode_name_at("", 0).empty());
        CHECK(mode_name_at(d + "/does-not-exist", 0).empty());
    }

    // §15.5 catalog_fingerprint (Pass 108): pins the index→name mapping a
    // no-IP ground has to hardcode. It covers the sorted NAMES only, so it
    // moves when the mapping moves and holds when it does not.
    {
        const auto fp = [&](const std::string& dd) {
            return nlohmann::json::parse(modes_catalog_json(dd, "", true))
                .at("catalog_fingerprint")
                .get<std::string>();
        };
        const std::string base = fp(d);
        CHECK(base.rfind("3-", 0) == 0);  // "<count>-<hex32>"
        CHECK_EQ_U(base.size(), 10);      // "3-" + 8 hex digits
        // Stable across calls, and independent of the active label.
        CHECK(fp(d) == base);
        CHECK(nlohmann::json::parse(modes_catalog_json(d, "imx335-variable",
                                                       false))
                  .at("catalog_fingerprint")
                  .get<std::string>() == base);
        // Editing a mode file's CONTENTS leaves the mapping — and so the
        // fingerprint — untouched: index i still resolves to the same name.
        write_file(d + "/imx335-variable.json",
                   mode_json(60, "1920x1080", 1, 4, "variable"));
        CHECK(fp(d) == base);
        // Adding a mode shifts later ordinals, so the fingerprint must move.
        write_file(d + "/imx335-00fps-extra.json",
                   mode_json(30, "640x360", 0, 1, "static"));
        const std::string grown = fp(d);
        CHECK(grown != base);
        CHECK(grown.rfind("4-", 0) == 0);
        // And removing it restores the original mapping exactly.
        ::unlink((d + "/imx335-00fps-extra.json").c_str());
        CHECK(fp(d) == base);
        // An empty catalog still yields a well-formed fingerprint.
        CHECK(fp("").rfind("0-", 0) == 0);
    }

    // Cleanup.
    ::unlink((d + "/imx335-100fps-highrange.json").c_str());
    ::unlink((d + "/imx335-variable.json").c_str());
    ::unlink((d + "/imx335-30fps-lowrange.json").c_str());
    ::unlink((d + "/broken.json").c_str());
    ::unlink((d + "/partial.json").c_str());
    ::unlink((d + "/README.md").c_str());
    ::rmdir(d.c_str());

    return wbtest_finish("modes_test");
}
