// SPDX-License-Identifier: GPL-2.0-or-later
// §10.7 (Pass 125) uplink artifact persistence: round trip, the pinned
// binary fingerprint, tamper rejection, the identity gate (which must NOT
// include the craft session), and forward compatibility of the placements
// list — a two-entry artifact from a future multi-rung uplink must parse
// under schema 1, which is the whole reason it is a list from v1.
#include "wblink/calib_store.h"
#include "wblink/uplink_calib_store.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "wbtest.h"

using namespace wblink;

namespace {

std::string temp_dir() {
    char tmpl[] = "/tmp/wblink-ucalXXXXXX";
    const char* d = ::mkdtemp(tmpl);
    return d != nullptr ? std::string(d) : std::string();
}

UplinkArtifact make_artifact() {
    UplinkArtifact a;
    a.local_adapter_identity = "bus/1-1.2";
    a.craft_originator = 17;
    a.craft_adapter_fingerprint = 0x5A;
    a.channel_mhz = 5805;
    a.bw_mhz = 20;
    a.t_unix = 1750000000;
    UplinkPlacement p;
    p.mcs = 0;
    p.short_gi = false;
    p.placement_qdb = 72;
    p.placement_rssi_dbm = -32;
    p.placement_loss_milli = 4;
    p.last_clean_qdb = 72;
    p.has_first_bad = true;
    p.first_bad_qdb = 88;
    a.placements.push_back(p);
    return a;
}

std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void test_round_trip() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    const UplinkArtifact a = make_artifact();
    const uint8_t fp = uplink_calib_store_write(dir, a);
    CHECK(fp != 0);  // 0 is the "no artifact" sentinel, never a real value

    auto loaded = uplink_calib_store_load(dir);
    CHECK(static_cast<bool>(loaded));
    if (!loaded) return;
    const UplinkArtifact& b = *loaded.value;
    CHECK(b.local_adapter_identity == a.local_adapter_identity);
    CHECK(b.craft_originator == a.craft_originator);
    CHECK(b.craft_adapter_fingerprint == a.craft_adapter_fingerprint);
    CHECK(b.channel_mhz == a.channel_mhz);
    CHECK(b.bw_mhz == a.bw_mhz);
    CHECK(b.t_unix == a.t_unix);
    CHECK(b.placements.size() == 1);
    if (b.placements.size() == 1) {
        const UplinkPlacement& p = b.placements[0];
        CHECK(p.mcs == 0);
        CHECK(!p.short_gi);
        CHECK(p.placement_qdb == 72);
        CHECK(p.placement_rssi_dbm == -32);
        CHECK(p.placement_loss_milli == 4);
        CHECK(p.last_clean_qdb == 72);
        CHECK(p.has_first_bad);
        CHECK(p.first_bad_qdb == 88);
    }
    CHECK(uplink_calib_fingerprint(b) == fp);

    // Rate identity lives in the entry, so lookup is per-rung.
    CHECK(uplink_calib_placement_for(b, 0, false) != nullptr);
    CHECK(uplink_calib_placement_for(b, 3, false) == nullptr);
    CHECK(uplink_calib_placement_for(b, 0, true) == nullptr);

    // Atomic write leaves no .tmp behind.
    CHECK(slurp(dir + "/uplink-artifact.json.tmp").empty());
    std::remove((dir + "/uplink-artifact.json").c_str());
    std::remove(dir.c_str());
}

// The fingerprint covers a PINNED BINARY form, not the JSON text. Reformat
// the file and it must still load; change a value and it must not.
void test_fingerprint_is_binary_not_text() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;
    const std::string path = dir + "/uplink-artifact.json";

    CHECK(uplink_calib_store_write(dir, make_artifact()) != 0);
    const std::string pretty = slurp(path);
    CHECK(!pretty.empty());

    // Strip every newline: same values, completely different bytes. A
    // text-derived fingerprint would reject this; a binary one must not.
    std::string flat;
    for (char c : pretty) {
        if (c != '\n') flat.push_back(c);
    }
    CHECK(flat != pretty);
    {
        std::ofstream f(path, std::ios::trunc);
        f << flat;
    }
    CHECK(static_cast<bool>(uplink_calib_store_load(dir)));

    std::remove(path.c_str());
    std::remove(dir.c_str());
}

// A hand-edited placement must not reach an RF power actuator just because
// it parses.
void test_tamper_rejected() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;
    const std::string path = dir + "/uplink-artifact.json";

    CHECK(uplink_calib_store_write(dir, make_artifact()) != 0);
    std::string body = slurp(path);
    const size_t at = body.find("\"placement_qdb\": 72");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        body.replace(at, 19, "\"placement_qdb\": 99");
        std::ofstream f(path, std::ios::trunc);
        f << body;
    }
    auto loaded = uplink_calib_store_load(dir);
    CHECK(!loaded);  // fingerprint disagrees

    // A truncated file is not applied either.
    {
        std::ofstream f(path, std::ios::trunc);
        f << "{\"schema\":1,";
    }
    CHECK(!uplink_calib_store_load(dir));
    // ...and neither is an absent one.
    std::remove(path.c_str());
    CHECK(!uplink_calib_store_load(dir));
    std::remove(dir.c_str());
}

// §10.7 identity gate. The craft SESSION is deliberately absent: a craft
// reboot changes it but not the hardware, and staling a valid measurement on
// every reboot would make the feature useless in practice.
void test_identity_gate() {
    const UplinkArtifact a = make_artifact();
    CHECK(uplink_calib_matches(a, "bus/1-1.2", 17, 0x5A, 5805, 20));
    // Every identity component is load-bearing.
    CHECK(!uplink_calib_matches(a, "bus/1-1.3", 17, 0x5A, 5805, 20));
    CHECK(!uplink_calib_matches(a, "bus/1-1.2", 18, 0x5A, 5805, 20));
    CHECK(!uplink_calib_matches(a, "bus/1-1.2", 17, 0x5B, 5805, 20));
    CHECK(!uplink_calib_matches(a, "bus/1-1.2", 17, 0x5A, 5745, 20));
    CHECK(!uplink_calib_matches(a, "bus/1-1.2", 17, 0x5A, 5805, 40));

    // Provenance is not identity: two runs of the same hardware at the same
    // operating point are the same artifact, so t_unix must not be hashed.
    UplinkArtifact later = a;
    later.t_unix += 86400;
    CHECK(uplink_calib_fingerprint(later) == uplink_calib_fingerprint(a));
}

// The list shape is v1 law so a future multi-rung uplink APPENDS rather than
// bumping the schema and migrating every deployed artifact and Hub parser.
// Prove a two-entry artifact round-trips under schema 1 today.
void test_multi_rung_forward_compat() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    UplinkArtifact a = make_artifact();
    UplinkPlacement second;
    second.mcs = 3;
    second.short_gi = true;
    second.placement_qdb = 60;
    second.placement_rssi_dbm = -38;
    second.placement_loss_milli = 9;
    second.last_clean_qdb = 60;
    second.has_first_bad = false;
    a.placements.push_back(second);

    CHECK(uplink_calib_store_write(dir, a) != 0);
    auto loaded = uplink_calib_store_load(dir);
    CHECK(static_cast<bool>(loaded));
    if (loaded) {
        CHECK(loaded.value->placements.size() == 2);
        const UplinkPlacement* p3 =
            uplink_calib_placement_for(*loaded.value, 3, true);
        CHECK(p3 != nullptr);
        if (p3 != nullptr) {
            CHECK(p3->placement_qdb == 60);
            CHECK(!p3->has_first_bad);  // null round-trips as absent
        }
        // The entry count and every field are inside the canonical form, so
        // a trimmed artifact is a DIFFERENT artifact. Note what is not
        // asserted: that its fingerprint differs. CRC-8 is 8 bits, so ~1 in
        // 256 distinct artifacts collide — this exact pair does. That is
        // fine for what the fingerprint is (a corruption check and a short
        // operator-visible identity) and it is not a tamper seal: anyone who
        // can edit the file can edit the fingerprint field too. Asserting
        // inequality here would be a coin-flip test.
        UplinkArtifact trimmed = *loaded.value;
        trimmed.placements.pop_back();
        CHECK(trimmed.placements.size() == 1);
        // A changed VALUE in a kept entry does move it, which is what the
        // round-trip integrity check actually relies on.
        UplinkArtifact edited = *loaded.value;
        edited.placements[1].placement_qdb += 1;
        CHECK(uplink_calib_fingerprint(edited) !=
              uplink_calib_fingerprint(*loaded.value));
    }
    std::remove((dir + "/uplink-artifact.json").c_str());
    std::remove(dir.c_str());
}

// The other half of the §10.7 discriminator: the CRAFT response at the same
// path must say downlink. Without this a Hub holding one body cannot tell
// which direction it describes, and the single bi-directional action would
// render the wrong phase's state.
void test_craft_response_declares_downlink() {
    const std::string j =
        calib_store_json("idle", 0, 0, false, nullptr, nullptr);
    CHECK(j.find("\"direction\":\"downlink\"") != std::string::npos);
    CHECK(j.find("\"uplink\"") == std::string::npos);
}

}  // namespace

int main() {
    test_craft_response_declares_downlink();
    test_round_trip();
    test_fingerprint_is_binary_not_text();
    test_tamper_rejected();
    test_identity_gate();
    test_multi_rung_forward_compat();
    return wbtest_finish("uplink_calib_store_test");
}
