// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §10.7 (Pass 125) ground-uplink calibration artifact.
//
// A DISTINCT schema from §10.6's craft artifact, in the same directory under
// policy.calibration.artifact_dir — one is an 8-rung TX curve, the other a
// per-rung uplink placement list. Serialising the second as a degenerate
// first would make both harder to reason about and neither correct.
//
// `placements` is a list from v1 carrying exactly one entry, with the rate
// identity (mcs/short_gi) INSIDE the entry rather than in the top-level
// identity block. A future multi-rung uplink then appends entries instead of
// bumping the schema and migrating every deployed artifact and Hub parser.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wblink/config.h"           // Result<>
#include "wblink/uplink_calibrate.h" // UplinkPlacement

namespace wblink {

struct UplinkArtifact {
    // Pairing identity. Deliberately EXCLUDES the craft session: a craft
    // reboot changes the session but not the hardware, and staling a valid
    // measurement on a reboot would make the feature useless in practice.
    std::string local_adapter_identity;
    uint16_t craft_originator = 0;
    uint8_t craft_adapter_fingerprint = 0;
    uint16_t channel_mhz = 0;
    uint8_t bw_mhz = 20;
    int64_t t_unix = 0;
    std::vector<UplinkPlacement> placements;
};

// CRC-8/DVB-S2 over a PINNED BINARY serialization — not over the JSON text.
// JSON key order, number formatting and whitespace are library-version
// artifacts; hashing them would let a dependency bump silently invalidate
// every stored fingerprint. Never returns 0 (the "no artifact" sentinel).
uint8_t uplink_calib_fingerprint(const UplinkArtifact& a);

// Atomic write (tmp + rename) of uplink-artifact.json. Returns the
// fingerprint, or 0 on failure.
uint8_t uplink_calib_store_write(const std::string& dir,
                                 const UplinkArtifact& a);

// Load and integrity-check. Fails when absent, unparsable, or when the
// stored fingerprint disagrees with the recomputed one.
Result<UplinkArtifact> uplink_calib_store_load(const std::string& dir);

// §10.7 apply gate: same local adapter, same craft, same band/bandwidth.
// A mismatch is surfaced as stale and never applied — the hardware stays at
// the higher-precedence source.
bool uplink_calib_matches(const UplinkArtifact& a,
                          const std::string& local_identity,
                          uint16_t craft_originator,
                          uint8_t craft_fingerprint, uint16_t channel_mhz,
                          uint8_t bw_mhz);

// The placement for one rung, or nullptr when this artifact has none.
const UplinkPlacement* uplink_calib_placement_for(const UplinkArtifact& a,
                                                  uint8_t mcs, bool short_gi);

}  // namespace wblink
