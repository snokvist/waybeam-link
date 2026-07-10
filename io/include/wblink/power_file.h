// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: loader for the node-local per-adapter power map (§10.2,
// groundwork §14) — a SUBSET of the stock Realtek PHY_REG_PG.txt row format:
//
//   #[v2][Exact]#                              (optional header)
//   #[2.4G]A                                   section: band + rf-path A
//   [1]  0xc20  0xffffffff  20.0  20.5  18.0  19.0
//   ...
//   0xffff                                     terminator
//
// Rows contribute 4 fractional-dBm values each, assigned SEQUENTIALLY in the
// driver's rate-index enum order per section (MGN_1M=0 …). The loader keeps
// only HT rate indices 12–19 (MCS0–7) from the requested band's path-A
// section and converts dBm -> qdb (×4). Register address and mask columns
// are carried by the format but not interpreted (we index by position, the
// way hal_com_phycfg fills TxPwrByRate).
//
// The resulting PowerCurve is the §10.2 level-4 baseline; resolve_power_qdb
// (core/power.h) applies the level offset + ceiling.
#pragma once

#include <string>

#include "wblink/config.h"  // Result<>
#include "wblink/power.h"

namespace wblink {

// band_5g selects the "#[5G]A" section, else "#[2.4G]A".
Result<PowerCurve> load_power_curve(const std::string& path, bool band_5g);

// Parse from an in-memory buffer (the file loader + tests share this).
Result<PowerCurve> parse_power_curve(const std::string& text, bool band_5g);

}  // namespace wblink
