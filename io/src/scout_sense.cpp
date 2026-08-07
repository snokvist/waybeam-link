// SPDX-License-Identifier: GPL-2.0-or-later
// §15.5a (Pass 155) occupancy derivation — see scout_sense.h.
#include "wblink/scout_sense.h"

#include <algorithm>
#include <cmath>

namespace wblink {

OccupancyDerived derive_occupancy(
    const std::optional<AirIface::AirSense>& sense,
    uint16_t wifi_util_permille, uint64_t observe_us) {
    OccupancyDerived out;
    out.util_permille = wifi_util_permille;
    if (!sense) {
        return out;
    }
    if (sense->fa_valid && observe_us >= kMinObserveUs) {
        // Saturating index of non-decodable energy (spec §15.5a): the
        // chanmig-proven bounded form, an index comparable within one
        // adapter — not an absolute duty cycle (FA events carry no
        // duration). CCA is deliberately not in the numerator: it counts
        // successful decodes too, which wifi_util already accounts.
        const double r =
            static_cast<double>(sense->fa_ofdm) * 1e6 /
            static_cast<double>(observe_us);
        const double term = r / (r + kFaHalfRatePerSec);
        out.interference_valid = true;
        out.interference_util_permille =
            static_cast<uint16_t>(std::lround(term * 1000.0));
        out.util_permille = static_cast<uint16_t>(
            std::min<uint32_t>(1000u, static_cast<uint32_t>(
                                          wifi_util_permille) +
                                          out.interference_util_permille));
    }
    // Noise preference (spec §15.5a): the chip's absolute idle floor where
    // the generation provides it, else the passive rssi−snr floor. The
    // min-RSSI proxy fallback stays with the caller — it is frame-derived,
    // not a sensor reading.
    if (sense->abs_nf_valid) {
        out.noise_valid = true;
        out.noise_dbm = sense->abs_nf_dbm;
    } else if (sense->nf_valid) {
        out.noise_valid = true;
        out.noise_dbm = static_cast<int>(std::lround(sense->nf_dbm));
    }
    return out;
}

uint16_t emptiest_channel(const std::vector<ChannelUtil>& measured,
                          const std::vector<uint16_t>& allowlist,
                          uint16_t except) {
    uint16_t best = 0;
    uint32_t best_util = 1001;  // > any per-mille
    for (const uint16_t ch : allowlist) {
        if (ch == except) continue;
        for (const ChannelUtil& m : measured) {
            if (m.chan != ch) continue;
            if (m.util_permille < best_util) {
                best_util = m.util_permille;
                best = ch;
            }
        }
    }
    return best;
}

}  // namespace wblink
