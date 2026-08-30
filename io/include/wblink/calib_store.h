// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §10.6 (Pass 120) calibration artifact persistence.
//
// One last-good artifact PER ADAPTER IDENTITY under
// policy.calibration.artifact_dir (§10.6 AMENDED Pass 195):
//   artifact-<identity>.json — canonical serialization (identity +
//                   placements + ceilings + curve); its CRC-8 (§3.6 idiom) is
//                   the fingerprint carried in the §3.15 word.
//   curve-<identity>.txt — the same curve in the §10.2 PHY_REG_PG-subset
//                   format, for operator eyes and offline tooling; the json is
//                   what the boot auto-load reads.
//
// Keyed by identity, not by node, because a node that runs two units in turn
// otherwise keeps only the last one's measurement: swap A->B and B overwrites
// A, swap back and A reads STALE forever with a full re-run as the only
// remedy. The identity gate was always correct — it refused to apply B's curve
// to A — but a correct refusal on data that should not have been lost is still
// a loss, and under §15.2 auto, where the TX unit is elected rather than
// pinned, it is the normal case rather than bad luck.
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
// Every other backend → "udp": none has a power actuator, so none has an
// identity. The Pass 146 "id/monitor/<calib_id>" / "ifname/<mac>" / "bus/<path>"
// tiers existed only for kernel-monitor and were deleted with it (Pass 164).
std::string calib_identity(const AdapterCfg& adapter, AirCfg::Kind backend,
                           const std::string& efuse_mac);

// An adapter identity as a FILENAME component (§10.6 Pass 195): lowercased,
// with everything outside [0-9a-z._-] mapped to '_'. Shared with the §10.7
// uplink store rather than copied — the two write into ONE directory, so a
// second private copy of this rule is a drift hazard, and an allowlist (not a
// blocklist) is what keeps a future identity tier from walking out of that
// directory.
std::string calib_identity_slug(const std::string& identity);

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

// Load this identity's persisted artifact (empty Result on none/parse
// failure). Falls back to the pre-Pass-195 fixed `artifact.json` when no
// per-identity file exists, and the caller's existing identity check is what
// gates it — so a deployed node upgrades in place, with no migration step and
// no re-run, while a legacy file belonging to a different unit is refused
// exactly as it was before.
Result<CalibStored> calib_store_load(const std::string& dir,
                                     const std::string& identity);

// §15.5 GET /api/v1/calibration body.
std::string calib_store_json(const std::string& state, uint8_t rung,
                             uint8_t fingerprint, bool stale,
                             const char* fail_reason,
                             const CalibArtifact* artifact);

}  // namespace wblink
