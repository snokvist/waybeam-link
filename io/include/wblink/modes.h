// SPDX-License-Identifier: GPL-2.0-or-later
// §15.5 Pass 104: operating-mode catalog for GET /api/v1/modes. Enumerates the
// modes/<name>.json files (docs/venc-mode-matrix.md §16) under a directory and
// returns the catalog as one JSON object — the link is the single source of
// truth for which modes exist, so a menu (the hub) need not carry its own copy.
#ifndef WBLINK_MODES_H
#define WBLINK_MODES_H

#include <string>

namespace wblink {

// Build the GET /api/v1/modes body:
//   {active, apply_configured, catalog_fingerprint,
//    modes:[{name, fps, resolution, mcs_min, mcs_max, fps_mode}]}
// `catalog_fingerprint` (§15.5 Pass 108) is "<count>-<hex32>", FNV-1a-32 over
// the name-sorted mode names joined by '\n'. A ground with no IP path to the
// craft must hardcode its own catalog copy, and §11.7 MODE addresses it by
// index; pinning this string lets the ground detect that the craft's catalog
// has drifted the moment it does get a path. Names only, in catalog order —
// editing a mode file's contents keeps it stable.
// `dir` is scanned for *.json entries; each is parsed for the user-facing facts
// (.venc.video0.fps/.size, .link.policy.select.min_profile/max_profile,
// .link.policy.fps_mode). `name` is the file stem. Entries that are missing,
// unparseable, or missing a required field are skipped — never fatal. modes[]
// is name-sorted for a stable menu. An empty or unreadable `dir` yields an
// empty modes[] (a misconfigured craft, not an error). The wire carries the raw
// facts, not UI vocabulary; the caller renders the latency×range grid.
std::string modes_catalog_json(const std::string& dir, const std::string& active,
                               bool apply_configured);

// §11.7 0x07 MODE (Pass 105): resolve a catalog index to its mode name. `index`
// is an ordinal into the SAME name-sorted, malformed-skipped enumeration
// modes_catalog_json() is built from, so a ground that picked the index from
// GET /api/v1/modes and the craft that applies it agree on which mode it is.
// Returns "" when `index` is past the catalog end (a §11.7 range-error REJECTED)
// or `dir` is empty/unreadable.
std::string mode_name_at(const std::string& dir, size_t index);

}  // namespace wblink

#endif  // WBLINK_MODES_H
