// SPDX-License-Identifier: GPL-2.0-or-later
// §3.6 table_version content hash: canonical-form correctness, insertion-order
// invariance, sensitivity to every field, and a pinned golden hash for the
// repo's profiles/table.example.json semantics (the Android vendored copy must
// reproduce this exact value).
#include "wblink/table.h"

#include <cstdio>
#include <cstring>

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
    // Pass 111: retain the conservative lower rungs and cap MCS4-7 at 95% of
    // their measured clean local-service boundary.
    static constexpr uint16_t kAirtimeBudget[] = {600, 600, 600, 600,
                                                  510, 463, 438, 418};
    p.airtime_budget_permille = kAirtimeBudget[id];
    p.fec_scheme = FecScheme::kNone;
    // Pass 95: graduated parity budget, monotonically non-increasing with rung
    // (r = ceil(k*rate) inflates as k falls, and k falls with the rung). Must
    // stay identical to profiles/table.example.json — config_test pins the
    // same hash off the shipped file, which is what makes this a cross-check.
    static constexpr uint16_t kFecOverhead[] = {250, 250, 200, 200,
                                                180, 180, 180, 180};
    p.fec_overhead_permille = kFecOverhead[id];
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
constexpr uint8_t kGoldenExampleHash = 0xBF;  // Pass 111 calibrated airtime ceilings

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

    // Canonical size: count + 8*27 + floor (§3.6, 27 bytes/profile — Pass 82).
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
    // MCS4 airtime bytes: profile 4 begins at 1 + 4*27; bytes 4-5 inside it
    // are 0x01FE = 510 (Pass 111).
    CHECK_EQ_U(canon[1 + 4 * kCanonicalProfileSize + 4], 0x01);
    CHECK_EQ_U(canon[1 + 4 * kCanonicalProfileSize + 5], 0xFE);

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

    // §3.6 Pass 82: cross-check the SPEC, not just ourselves. The golden hash
    // above pins the implementation against itself, so a spec/code divergence
    // (which is exactly what Pass 82 found: §3.6 documented 25 bytes while the
    // code hashed 27) is invisible to it by construction. Here the byte layout
    // is hand-built straight from the §3.6 field list and compared.
    {
        ProfileTable t;
        Profile p;
        p.id = 0x11;
        p.mcs = 0x22;
        p.gi = GuardInterval::kShort;  // 1
        p.tx_power_level = 0x44;
        p.airtime_budget_permille = 0x5566;
        p.fec_scheme = FecScheme::kNone;  // 0
        p.fec_overhead_permille = 0x7788;
        p.arq_deadline_iframe_ms = 0x99AA;
        p.arq_deadline_pframe_ms = 0xBBCC;
        p.bitrate_min_kbps = 0xDDEEFF00;
        p.reserve_control_bps = 0x11223344;
        p.reserve_telemetry_bps = 0x55667788;
        p.max_payload = 0x99AA;
        t.profiles.push_back(p);
        t.floor_profile = 0x0F;

        // count u8 | 27 bytes big-endian, §3.6 order | floor_profile u8
        const uint8_t spec[] = {
            0x01,                                      // count
            0x11,                                      // id
            0x22,                                      // mcs
            0x01,                                      // gi (short)
            0x44,                                      // tx_power_level
            0x55, 0x66,                                // airtime_budget_permille
            0x00,                                      // fec_scheme (none)
            0x77, 0x88,                                // fec_overhead_permille
            0x99, 0xAA,                                // arq_deadline_iframe_ms
            0xBB, 0xCC,                                // arq_deadline_pframe_ms
            0xDD, 0xEE, 0xFF, 0x00,                    // bitrate_min_kbps
            0x11, 0x22, 0x33, 0x44,                    // reserve_control_bps
            0x55, 0x66, 0x77, 0x88,                    // reserve_telemetry_bps
            0x99, 0xAA,                                // max_payload (Pass 82)
            0x0F,                                      // floor_profile
        };
        const auto got = canonical_serialize(t);
        CHECK_EQ_U(got.size(), sizeof(spec));  // 1 + 27 + 1
        CHECK_EQ_U(std::memcmp(got.data(), spec, sizeof(spec)), 0);
        CHECK_EQ_U(table_version(t), crc8_dvbs2(spec, sizeof(spec)));
    }

    return wbtest_finish("table_hash_test");
}
