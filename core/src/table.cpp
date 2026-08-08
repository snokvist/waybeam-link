// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/table.h"

#include <algorithm>

#include "wblink/crc8.h"
#include "wblink/endian.h"

namespace wblink {

bool has_duplicate_ids(const ProfileTable& table) {
    std::vector<uint8_t> ids;
    ids.reserve(table.profiles.size());
    for (const Profile& p : table.profiles) {
        ids.push_back(p.id);
    }
    std::sort(ids.begin(), ids.end());
    return std::adjacent_find(ids.begin(), ids.end()) != ids.end();
}

std::vector<uint8_t> canonical_serialize(const ProfileTable& table) {
    // §3.6: profiles sorted ascending by id, pinned field order, big-endian.
    std::vector<const Profile*> sorted;
    sorted.reserve(table.profiles.size());
    for (const Profile& p : table.profiles) {
        sorted.push_back(&p);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const Profile* a, const Profile* b) { return a->id < b->id; });

    std::vector<uint8_t> out;
    out.reserve(2 + kCanonicalProfileSize * sorted.size());
    out.push_back(static_cast<uint8_t>(sorted.size()));
    for (const Profile* p : sorted) {
        uint8_t f[kCanonicalProfileSize];
        f[0] = p->id;
        f[1] = p->mcs;
        f[2] = static_cast<uint8_t>(p->gi);
        f[3] = p->tx_power_level;
        be16_write(f + 4, p->airtime_budget_permille);
        f[6] = static_cast<uint8_t>(p->fec_scheme);
        be16_write(f + 7, p->fec_overhead_permille);
        be16_write(f + 9, p->arq_deadline_iframe_ms);
        be16_write(f + 11, p->arq_deadline_pframe_ms);
        be32_write(f + 13, p->bitrate_min_kbps);
        be32_write(f + 17, p->reserve_control_bps);
        be32_write(f + 21, p->reserve_telemetry_bps);
        be16_write(f + 25, p->max_payload);  // §9.3 appended field
        out.insert(out.end(), f, f + kCanonicalProfileSize);
    }
    out.push_back(table.floor_profile);
    // §3.6 Pass 163: the probe schedule is a both-ends protocol invariant —
    // appended after floor_profile so a schedule difference rotates the hash.
    uint8_t sched[4];
    be16_write(sched, table.probe_period);
    be16_write(sched + 2, table.probe_slot);
    out.insert(out.end(), sched, sched + 4);
    return out;
}

uint8_t table_version(const ProfileTable& table) {
    const std::vector<uint8_t> canon = canonical_serialize(table);
    return crc8_dvbs2(canon.data(), canon.size());
}

}  // namespace wblink
