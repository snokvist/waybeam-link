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

// Stable identity for the tx adapter the curve was calibrated against.
//
// radio (devourer), Pass 154: "mac/<efuse-mac>" — `efuse_mac` is the live
// per-unit identity the backend read at bring-up (AirIface::adapter_mac).
// EMPTY when the unit reports none: that is the D3 fail-closed answer — no
// artifact may be loaded or written and no absolute curve applied. There is
// deliberately no declared or bus-path fallback tier on this backend; a
// fallback is exactly the port-keyed misapplication §10.6 exists to prevent.
//
// kernel-monitor (frozen, ruling #120) keeps the Pass 146 tiers unchanged:
// "id/monitor/<calib_id>" when declared, else "ifname/<mac>" (sysfs), else
// "bus/<path>" (logged unstable). udp → "udp". `efuse_mac` is ignored there.
std::string calib_identity(const AdapterCfg& adapter, AirCfg::Kind backend,
                           const std::string& efuse_mac);

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
