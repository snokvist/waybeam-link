// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §10.6 (Pass 120) calibration artifact persistence.
//
// One last-good artifact per node under policy.calibration.artifact_dir:
//   artifact.json — canonical serialization (identity + placements +
//                   ceilings + curve); its CRC-8 (§3.6 idiom) is the
//                   fingerprint carried in the §3.15 word.
//   curve.txt     — the same curve in the §10.2 PHY_REG_PG-subset format,
//                   for operator eyes and offline tooling; artifact.json is
//                   what the boot auto-load reads.
// Writes are atomic (tmp + rename). Boot auto-load applies the curve ONLY
// when the stored identity matches the live tx adapter (§10.6: a curve
// calibrated for different hardware is never silently applied — the caller
// surfaces CALIBRATION STALE instead).
#pragma once

#include <string>

#include "wblink/calibrate.h"
#include "wblink/config.h"
#include "wblink/power.h"

namespace wblink {

// Stable identity for the tx adapter the curve was calibrated against:
// kernel-monitor → "ifname/<mac>" (sysfs), devourer → "bus/<path>",
// udp → "udp". Weak identities beat silent misapplication.
//
// `backend` scopes the operator-declared `calib_id` tier. The derived tiers
// distinguish backends for free — an ifname only exists on kernel-monitor, a
// bus path only on devourer — but `calib_id` is a name the operator chose and
// would otherwise be identical under both, which would let a monitor-measured
// curve be applied to devourer's very different actuator without ever reading
// STALE. That is the failure §10.7 exists to prevent, so the declared tier is
// scoped too. Pass `AirCfg::Kind`; see `calib_backend_tag`.
std::string calib_identity(const AdapterCfg& adapter, AirCfg::Kind backend);

// Short stable token for an air backend, used only to scope calibration
// identities. Deliberately not the config spelling: these strings land in a
// persisted artifact, so they must not move when config wording does.
const char* calib_backend_tag(AirCfg::Kind kind);

// Persist the artifact; returns the CRC-8 fingerprint (never 0 on success —
// 0 is the §3.15 "no artifact" sentinel), or 0 on write failure.
uint8_t calib_store_write(const std::string& dir, const std::string& identity,
                          const CalibArtifact& a);

struct CalibStored {
    PowerCurve curve;
    CalibArtifact artifact;
    std::string identity;
    uint8_t fingerprint = 0;
};

// Load the persisted artifact (empty Result on none/parse failure).
Result<CalibStored> calib_store_load(const std::string& dir);

// §15.5 GET /api/v1/calibration body.
std::string calib_store_json(const std::string& state, uint8_t rung,
                             uint8_t fingerprint, bool stale,
                             const char* fail_reason,
                             const CalibArtifact* artifact);

}  // namespace wblink
