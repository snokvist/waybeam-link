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

// §10.7 (Pass 195): the store is keyed by identity, so every load names
// the one make_artifact() writes under.
constexpr const char* kIdentity = "bus/1-1.2";
// The artifact file this identity writes to. Derived, never spelled out:
// a literal here would keep passing while the store wrote somewhere else,
// which is exactly how the tamper and truncation cases below could go
// green against a file nobody reads.
std::string artifact_file(const std::string& dir) {
    return dir + "/uplink-artifact-" + calib_identity_slug(kIdentity) +
           ".json";
}

UplinkArtifact make_artifact() {
    UplinkArtifact a;
    a.local_adapter_identity = kIdentity;
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

    auto loaded = uplink_calib_store_load(dir, kIdentity);
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
    CHECK(slurp(artifact_file(dir) + ".tmp").empty());
    std::remove(artifact_file(dir).c_str());
    std::remove(dir.c_str());
}

// C3 regression. An absent bracket is serialized as JSON null and read back
// as 0, so the canonicalization must hash 0 for it regardless of what the
// in-memory field happens to hold. It held the PREVIOUS run's bracket
// (UplinkCalibrator did not reset it across start()), so every such artifact
// failed its own fingerprint on reload: the operator saw DONE and the next
// boot silently discarded the measurement.
void test_absent_bracket_round_trips() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    UplinkArtifact a = make_artifact();
    a.placements[0].has_first_bad = false;
    a.placements[0].first_bad_qdb = 88;  // stale in-memory residue

    const uint8_t fp = uplink_calib_store_write(dir, a);
    CHECK(fp != 0);
    auto loaded = uplink_calib_store_load(dir, kIdentity);
    CHECK(static_cast<bool>(loaded));  // must not be "fingerprint mismatch"
    if (loaded) {
        CHECK(!loaded.value->placements[0].has_first_bad);
        CHECK(loaded.value->placements[0].first_bad_qdb == 0);
    }
    // And the hash genuinely ignores the residue: the same artifact with the
    // field zeroed is the same artifact.
    UplinkArtifact z = a;
    z.placements[0].first_bad_qdb = 0;
    CHECK(uplink_calib_fingerprint(z) == fp);

    std::remove(artifact_file(dir).c_str());
    std::remove(dir.c_str());
}

// The fingerprint covers a PINNED BINARY form, not the JSON text. Reformat
// the file and it must still load; change a value and it must not.
void test_fingerprint_is_binary_not_text() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;
    const std::string path = artifact_file(dir);

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
    CHECK(static_cast<bool>(uplink_calib_store_load(dir, kIdentity)));

    std::remove(path.c_str());
    std::remove(dir.c_str());
}

// A hand-edited placement must not reach an RF power actuator just because
// it parses.
void test_tamper_rejected() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;
    const std::string path = artifact_file(dir);

    CHECK(uplink_calib_store_write(dir, make_artifact()) != 0);
    std::string body = slurp(path);
    const size_t at = body.find("\"placement_qdb\": 72");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        body.replace(at, 19, "\"placement_qdb\": 99");
        std::ofstream f(path, std::ios::trunc);
        f << body;
    }
    auto loaded = uplink_calib_store_load(dir, kIdentity);
    CHECK(!loaded);  // fingerprint disagrees

    // A truncated file is not applied either.
    {
        std::ofstream f(path, std::ios::trunc);
        f << "{\"schema\":1,";
    }
    CHECK(!uplink_calib_store_load(dir, kIdentity));
    // ...and neither is an absent one.
    std::remove(path.c_str());
    CHECK(!uplink_calib_store_load(dir, kIdentity));
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

// The list shape was v1 law so widening from one rung to eight would APPEND
// rather than bump the schema and migrate every deployed artifact and Hub
// parser. Pass 131 spends that allowance: a full eight-entry artifact is the
// shipping shape, and it round-trips under schema 1 unchanged.
//
// The one-entry case is NOT retired with it — a Pass 125 build wrote
// one-entry artifacts, and those files still exist on deployed grounds. The
// loader must accept any length (see test_roundtrip, which writes one).
void test_multi_rung_forward_compat() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    UplinkArtifact a = make_artifact();  // starts with rung 0
    for (uint8_t mcs = 1; mcs < 8; ++mcs) {
        UplinkPlacement p;
        p.mcs = mcs;
        p.short_gi = mcs >= 2;  // §9.3: long GI on 0/1, short on 2..7
        p.placement_qdb = 60 + mcs;
        p.placement_rssi_dbm = static_cast<int8_t>(-38 - mcs);
        p.placement_loss_milli = 9;
        p.last_clean_qdb = 60 + mcs;
        p.has_first_bad = false;
        a.placements.push_back(p);
    }

    CHECK(uplink_calib_store_write(dir, a) != 0);
    auto loaded = uplink_calib_store_load(dir, kIdentity);
    CHECK(static_cast<bool>(loaded));
    if (loaded) {
        CHECK(loaded.value->placements.size() == 8);
        // Every rung resolves by its own rate identity, which is what a rate
        // policy will index and what the boot-time resolve already uses.
        for (uint8_t mcs = 0; mcs < 8; ++mcs) {
            const UplinkPlacement* p =
                uplink_calib_placement_for(*loaded.value, mcs, mcs >= 2);
            CHECK(p != nullptr);
            if (p != nullptr) CHECK(p->mcs == mcs);
        }
        // A rung asked for at the WRONG guard interval must not resolve: the
        // placement would be a measurement of a different operating point.
        CHECK(uplink_calib_placement_for(*loaded.value, 5, false) == nullptr);
        const UplinkPlacement* p3 =
            uplink_calib_placement_for(*loaded.value, 3, true);
        CHECK(p3 != nullptr);
        if (p3 != nullptr) {
            CHECK(p3->placement_qdb == 63);
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
        CHECK(trimmed.placements.size() == 7);
        // A changed VALUE in a kept entry does move it, which is what the
        // round-trip integrity check actually relies on.
        UplinkArtifact edited = *loaded.value;
        edited.placements[1].placement_qdb += 1;
        CHECK(uplink_calib_fingerprint(edited) !=
              uplink_calib_fingerprint(*loaded.value));
    }
    std::remove(artifact_file(dir).c_str());
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

// §10.7 (Pass 195) legacy fallback: a pre-195 ground has one fixed
// uplink-artifact.json and must keep it across the upgrade — while the
// identity gate stays the thing that decides whether it may be APPLIED.
void test_legacy_fallback() {
    const std::string dir = temp_dir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    CHECK(uplink_calib_store_write(dir, make_artifact()) != 0);
    const std::string body = slurp(artifact_file(dir));
    CHECK(!body.empty());
    std::remove(artifact_file(dir).c_str());
    {
        std::ofstream f(dir + "/uplink-artifact.json", std::ios::trunc);
        f << body;
    }

    // Found through the fallback, and it parses + integrity-checks.
    auto loaded = uplink_calib_store_load(dir, kIdentity);
    CHECK(static_cast<bool>(loaded));
    if (loaded) {
        CHECK(loaded.value->local_adapter_identity == kIdentity);
        // The gate that matters: the SAME unit matches...
        CHECK(uplink_calib_matches(*loaded.value, kIdentity, 17, 0x5a, 5805, 20));
        // ...and a different local adapter does not, so a legacy file from
        // another dongle can be read but never applied.
        CHECK(!uplink_calib_matches(*loaded.value, "mac/00:11:22:33:44:55", 17,
                                    0x5a, 5805, 20));
    }
    std::remove((dir + "/uplink-artifact.json").c_str());
    std::remove(dir.c_str());
}

int main() {
    test_craft_response_declares_downlink();
    test_round_trip();
    test_absent_bracket_round_trips();
    test_fingerprint_is_binary_not_text();
    test_tamper_rejected();
    test_identity_gate();
    test_multi_rung_forward_compat();

    // §10.5 wire range on a value that auto-applies at boot with no operator
    // in the loop. The CRC-8 is documented as NOT a tamper seal, so an
    // out-of-range placement is corruption — refuse the artifact rather than
    // actuate part of one.
    {
        const std::string dir = temp_dir();
        UplinkArtifact a = make_artifact();
        a.placements[0].placement_qdb = 5000;
        CHECK(uplink_calib_store_write(dir, a) != 0);
        auto r = uplink_calib_store_load(dir, kIdentity);
        CHECK(!r);
        if (!r) CHECK(r.error.find("out of range") != std::string::npos);
    }
    // §10.7 Pass 146 (radio re-based Pass 154; tiers 2-4 deleted Pass 164):
    // identity resolution. The point is that an artifact survives a re-plug —
    // the old bus-path form did not — and that a unit with no MAC gets NO
    // identity rather than a weaker one.
    {
        AdapterCfg a;
        a.name = "uplink";
        // udp/bench adapter: no identity to derive.
        CHECK(calib_identity(a, AirCfg::Kind::kUdp, {}) == "udp");

        // Pass 154, radio: the EFUSE MAC is the single derived tier — the
        // identity follows the UNIT, wherever it is plugged.
        CHECK(calib_identity(a, AirCfg::Kind::kRadio,
                             "84:fc:14:50:bc:de") == "mac/84:fc:14:50:bc:de");
        // D3: an identity-less unit gets NO identity — never a weaker key.
        // Empty is the fail-closed answer the callers refuse curves on.
        CHECK(calib_identity(a, AirCfg::Kind::kRadio, {}).empty());
        // No fallback tier on radio: neither a bus path nor a declared
        // calib_id may stand in for a missing MAC (that is the port-keyed
        // misapplication §10.6 exists to prevent), and neither outranks a
        // present one.
        a.bus = "9-9";
        a.calib_id = "craft-eu-1";
        CHECK(calib_identity(a, AirCfg::Kind::kRadio, {}).empty());
        CHECK(calib_identity(a, AirCfg::Kind::kRadio,
                             "84:fc:14:50:bc:de") == "mac/84:fc:14:50:bc:de");
        a.bus.clear();
        a.calib_id.clear();

        // Pass 164: the declared / ifname / bus tiers went with
        // kernel-monitor. No non-radio backend has a power actuator, so none
        // of these keys resolves an identity any more — they are inert, and
        // the answer is the actuator-less "udp" regardless of what is set.
        a.ifname = "wlnosuchdev";
        a.bus = "9-9";
        a.calib_id = "craft-eu-1";
        CHECK(calib_identity(a, AirCfg::Kind::kUdp, {}) == "udp");
        CHECK(calib_identity(a, AirCfg::Kind::kUdpBroadcast, {}) == "udp");
        // ...and they still do not stand in for a missing MAC on radio.
        CHECK(calib_identity(a, AirCfg::Kind::kRadio, {}).empty());
    }

    test_legacy_fallback();
    return wbtest_finish("uplink_calib_store_test");
}
