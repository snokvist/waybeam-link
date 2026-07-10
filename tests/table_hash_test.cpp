// SPDX-License-Identifier: GPL-2.0-or-later
// §3.6 table_version content hash: canonical-form correctness, insertion-order
// invariance, sensitivity to every field, and a pinned golden hash for the
// repo's profiles/table.example.json semantics (the Android vendored copy must
// reproduce this exact value).
#include "wblink/table.h"

#include <cstdio>

#include "wblink/crc8.h"
#include "wbtest.h"

using namespace wblink;

namespace {

// The §9.3 seed table, transcribed from profiles/table.example.json.
Profile example_profile(uint8_t id) {
    // Per-id fields from the example: mcs == id; gi long for 0-1, short 2-7;
    // power 4,4,3,3,2,2,1,1; deadlines descend.
    static constexpr uint16_t kIframe[] = {80, 70, 60, 55, 50, 45, 42, 40};
    static constexpr uint16_t kPframe[] = {25, 22, 20, 18, 16, 14, 13, 12};
    static constexpr uint8_t kPower[] = {4, 4, 3, 3, 2, 2, 1, 1};
    Profile p;
    p.id = id;
    p.mcs = id;
    p.gi = id < 2 ? GuardInterval::kLong : GuardInterval::kShort;
    p.tx_power_level = kPower[id];
    p.airtime_budget_permille = 600;  // 0.60
    p.fec_scheme = FecScheme::kNone;
    p.fec_overhead_permille = 0;
    p.arq_deadline_iframe_ms = kIframe[id];
    p.arq_deadline_pframe_ms = kPframe[id];
    p.bitrate_min_kbps = 2200;
    p.reserve_control_bps = 64000;
    p.reserve_telemetry_bps = 32000;
    return p;
}

// Pinned golden hash of the example table. Every vendored copy of the codec
// (e.g. Waybeam-android :wifi) must reproduce this value for this table;
// recompute only on a deliberate §3.6 canonical-form revision.
constexpr uint8_t kGoldenExampleHash = 0x2B;

ProfileTable example_table() {
    ProfileTable t;
    for (uint8_t id = 0; id < 8; ++id) {
        t.profiles.push_back(example_profile(id));
    }
    t.floor_profile = 0;
    return t;
}

}  // namespace

int main() {
    const ProfileTable table = example_table();

    // Canonical size: count + 8*25 + floor.
    const auto canon = canonical_serialize(table);
    CHECK_EQ_U(canon.size(), 2 + 8 * kCanonicalProfileSize);
    CHECK_EQ_U(canon[0], 8);                  // count
    CHECK_EQ_U(canon[canon.size() - 1], 0);   // floor_profile
    // First profile's first bytes: id 0, mcs 0, gi long(0), power 4, 0x0258=600.
    CHECK_EQ_U(canon[1], 0);
    CHECK_EQ_U(canon[2], 0);
    CHECK_EQ_U(canon[3], 0);
    CHECK_EQ_U(canon[4], 4);
    CHECK_EQ_U(canon[5], 0x02);
    CHECK_EQ_U(canon[6], 0x58);

    // Golden hash for the example table — pinned so every vendored copy of
    // the codec must reproduce it. (Recompute only on a deliberate §3.6 rev.)
    const uint8_t golden = table_version(table);
    std::fprintf(stderr, "table.example.json table_version = 0x%02X\n", golden);
    CHECK_EQ_U(golden, kGoldenExampleHash);

    // Insertion order must not matter (canonical form sorts by id).
    {
        ProfileTable shuffled;
        shuffled.floor_profile = 0;
        for (int id : {5, 0, 7, 3, 1, 6, 2, 4}) {
            shuffled.profiles.push_back(
                example_profile(static_cast<uint8_t>(id)));
        }
        CHECK_EQ_U(table_version(shuffled), golden);
    }

    // Every field must be hash-visible: mutate one field at a time.
    {
        auto mutated = [&](auto&& fn) {
            ProfileTable t = example_table();
            fn(t);
            return table_version(t);
        };
        CHECK(mutated([](ProfileTable& t) { t.profiles[3].mcs = 7; }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles[0].gi = GuardInterval::kShort;
              }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles[5].tx_power_level = 3;
              }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles[2].airtime_budget_permille = 601;
              }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles[7].fec_scheme = FecScheme::kRlc256;
              }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles[7].fec_overhead_permille = 100;
              }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles[1].arq_deadline_iframe_ms = 71;
              }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles[1].arq_deadline_pframe_ms = 23;
              }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles[4].bitrate_min_kbps = 2201;
              }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles[6].reserve_control_bps = 64001;
              }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles[6].reserve_telemetry_bps = 32001;
              }) != golden);
        CHECK(mutated([](ProfileTable& t) { t.floor_profile = 1; }) != golden);
        CHECK(mutated([](ProfileTable& t) {
                  t.profiles.pop_back();
              }) != golden);
    }

    // Duplicate-id detection (the loader rejects before hashing).
    {
        ProfileTable dup = example_table();
        CHECK(!has_duplicate_ids(dup));
        dup.profiles[7].id = 3;
        CHECK(has_duplicate_ids(dup));
    }

    // Empty table: count 0 + floor byte only.
    {
        ProfileTable empty;
        const auto c = canonical_serialize(empty);
        CHECK_EQ_U(c.size(), 2);
        const uint8_t expect[2] = {0x00, 0x00};
        CHECK_EQ_U(table_version(empty), crc8_dvbs2(expect, 2));
    }

    return wbtest_finish("table_hash_test");
}
