// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: profile table (PROTOCOL.md §9.3) + the table_version
// content hash (§3.6). Fractions are stored as integer per-mille — the JSON
// loader (io/) scales with llround(frac * 1000); core never touches floats,
// so the canonical bytes are identical on every platform.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wblink {

enum class GuardInterval : uint8_t { kLong = 0, kShort = 1 };

// §9.3 / §14 — schemes beyond kNone exist in the enum for the canonical
// serialization only; the base table always ships fec_scheme = none.
enum class FecScheme : uint8_t {
    kNone = 0,
    kRlc256 = 1,
    kRlc256Iframe = 2,
    kTetrysReactive = 3,
};

struct Profile {
    uint8_t id = 0;
    uint8_t mcs = 0;
    GuardInterval gi = GuardInterval::kLong;
    uint8_t tx_power_level = 0;  // portable power INTENT (§10.2), not qdb
    uint16_t airtime_budget_permille = 0;
    uint16_t max_payload = 1424;  // §3.2 air MTU budget; drives FrameFramer s (§5.1a)
    FecScheme fec_scheme = FecScheme::kNone;
    uint16_t fec_overhead_permille = 0;
    uint16_t arq_deadline_iframe_ms = 0;
    uint16_t arq_deadline_pframe_ms = 0;
    uint32_t bitrate_min_kbps = 0;  // policy floor >= venc hard floor 1000
    uint32_t reserve_control_bps = 0;
    uint32_t reserve_telemetry_bps = 0;
    friend bool operator==(const Profile&, const Profile&) = default;
};

struct ProfileTable {
    std::vector<Profile> profiles;
    uint8_t floor_profile = 0;
};

// Canonical serialization size: count u8 + 27 B per profile + floor u8.
// (25 B pinned fields + max_payload u16 appended, §9.3.)
inline constexpr size_t kCanonicalProfileSize = 27;

// True if two profiles share an id — a config error the loader must reject
// before hashing (§3.6: profiles are sorted by id in the canonical form).
bool has_duplicate_ids(const ProfileTable& table);

// §3.6 canonical binary serialization: count u8, then each profile sorted
// ascending by id in the pinned big-endian field order, then floor_profile.
// Precondition: unique ids, profiles.size() <= 255.
std::vector<uint8_t> canonical_serialize(const ProfileTable& table);

// table_version = CRC-8/DVB-S2 over canonical_serialize() (§3.6).
uint8_t table_version(const ProfileTable& table);

}  // namespace wblink
