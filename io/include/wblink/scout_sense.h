// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §15.5a (Pass 155) frame-free occupancy derivation and the
// quick-connect ranking selection — pure rules a test can reach with injected
// sensor values, no radio (the Pass 144 radio_decode.h pattern).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "wblink/air_iface.h"  // AirIface::AirSense

namespace wblink {

// §15.5a Tier-2 seeds (§17 RE-DERIVE; constants until a bench derivation
// demands config). kFaHalfRate is the chanmig-proven half-rate of the
// saturating false-alarm term; kSenseSettleMs the retune settle before the
// discard barrier.
inline constexpr double kFaHalfRatePerSec = 200.0;
inline constexpr uint32_t kSenseSettleMs = 30;
// Minimum observe window before an FA delta is trusted as a rate: an
// abandoned dwell (the Pass 144 claim path) can finalize milliseconds after
// its barrier, where a single FA event reads as hundreds of permille. Too
// short to rate = invalid (JSON null), never a high-variance guess. At the
// 200 FA/s half-rate, 100 ms ≈ 20 ambient events of statistics.
inline constexpr uint64_t kMinObserveUs = 100000;

struct OccupancyDerived {
    bool interference_valid = false;
    uint16_t interference_util_permille = 0;
    bool noise_valid = false;
    int noise_dbm = 0;
    // Composite ranking score (§15.5a): min(1000, decoded airtime +
    // frame-free interference score). This is NOT RF duty cycle: FA events
    // carry no duration. The legacy JSON name is util_permille; consumers
    // should use ranking_score_permille for the same value.
    uint16_t util_permille = 0;
};

// Fold one dwell's frame-free sensor delta into the §15.5a ranking fields.
// `observe_us` is the barrier→read window (the interference denominator —
// NOT the full dwell, which charges the bin with its own retune). A missing
// sensor, an invalid FA leg, or an observe window under kMinObserveUs
// leaves interference invalid (JSON null, never a fake zero or a
// high-variance guess).
OccupancyDerived derive_occupancy(
    const std::optional<AirIface::AirSense>& sense,
    uint16_t wifi_util_permille, uint64_t observe_us);

// §15.5a (Pass 155) ranking selection: lowest util_permille among the
// allowlisted channels, skipping `except` (the craft's current channel).
// 0 when nothing measured for any allowed channel.
struct ChannelUtil {
    uint16_t chan = 0;
    uint16_t util_permille = 0;
};

// A 20 MHz emitter does not stop at its channel edge, so ranking channels as
// independent bins picks neighbours of a busy channel as if they were clear.
// Measured on the two-craft bench 2026-08-16 (8812EU craft ~16 dB above an
// 8733BU craft, both HT20): with the strong craft transmitting, the weak one
// delivered 0.0-0.9 fps of a nominal 60 at +/-20 MHz, 20.8 fps at +/-40, and
// 54.5 fps at +/-60. Silencing the strong craft restored the same channels to
// 59.9 fps, so the cause is adjacency and nothing else. quickconnect had put
// the weak craft on 5700 with the strong one on 5720 — the ranker correctly
// scored 5720 occupied and then handed out the channel next to it.
//
// `guard_mhz` (policy.csa.adjacent_guard_mhz, Tier-2 seed 40) ranks a channel
// on the WORST utilisation within +/-guard_mhz of it, breaking ties on the
// channel's own utilisation so equally-guarded channels still order by what
// was measured on them. 0 is the pre-guard per-channel ranking.
uint16_t emptiest_channel(const std::vector<ChannelUtil>& measured,
                          const std::vector<uint16_t>& allowlist,
                          uint16_t except, int guard_mhz);

}  // namespace wblink
