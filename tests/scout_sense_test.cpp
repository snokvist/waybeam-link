// SPDX-License-Identifier: GPL-2.0-or-later
// §15.5a (Pass 155): occupancy derivation + ranking selection with injected
// sensor values — the acceptance harness for issue #95's code gate. Pure, no
// radio (the radio_decode_test pattern).
#include "wblink/scout_sense.h"

#include <cstdio>

#include "wbtest.h"

namespace {

using wblink::AirIface;
using wblink::ChannelUtil;
using wblink::derive_occupancy;
using wblink::emptiest_channel;
using wblink::kFaHalfRatePerSec;

AirIface::AirSense fa_sense(uint32_t fa) {
    AirIface::AirSense s;
    s.fa_valid = true;
    s.fa_ofdm = fa;
    return s;
}

void test_derivation() {
    // No sensor: the structural sensor-less fallback — util aliases
    // wifi_util, interference and noise stay invalid (JSON null).
    {
        const auto d = derive_occupancy(std::nullopt, 123, 300000);
        CHECK(!d.interference_valid);
        CHECK(!d.noise_valid);
        CHECK_EQ_U(d.util_permille, 123);
    }
    // Sensor present but the FA leg invalid (a generation without the
    // counter): null, never a fake zero.
    {
        AirIface::AirSense s;  // fa_valid=false
        const auto d = derive_occupancy(s, 50, 300000);
        CHECK(!d.interference_valid);
        CHECK_EQ_U(d.util_permille, 50);
    }
    // A zero observe window (dwell shorter than its settle) cannot rate the
    // delta — invalid, not zero.
    {
        const auto d = derive_occupancy(fa_sense(500), 50, 0);
        CHECK(!d.interference_valid);
        CHECK_EQ_U(d.util_permille, 50);
    }
    // A window under the floor (an abandoned dwell finalizing just after
    // its barrier) is too short to rate — one FA event must not read as
    // hundreds of permille.
    {
        const auto d =
            derive_occupancy(fa_sense(1), 50, wblink::kMinObserveUs / 50);
        CHECK(!d.interference_valid);
        CHECK_EQ_U(d.util_permille, 50);
    }
    // At the floor exactly, the rate is trusted.
    {
        const auto d = derive_occupancy(fa_sense(0), 50, wblink::kMinObserveUs);
        CHECK(d.interference_valid);
    }
    // Quiet channel: zero false alarms derive a valid zero.
    {
        const auto d = derive_occupancy(fa_sense(0), 10, 300000);
        CHECK(d.interference_valid);
        CHECK_EQ_U(d.interference_util_permille, 0);
        CHECK_EQ_U(d.util_permille, 10);
    }
    // The half-rate point: r == H reads exactly 500 permille.
    {
        const auto fa = static_cast<uint32_t>(kFaHalfRatePerSec);  // over 1 s
        const auto d = derive_occupancy(fa_sense(fa), 100, 1000000);
        CHECK(d.interference_valid);
        CHECK_EQ_U(d.interference_util_permille, 500);
        CHECK_EQ_U(d.util_permille, 600);
    }
    // Saturation: a screaming emitter approaches (and the sum clamps at)
    // 1000 — never overflows the per-mille domain.
    {
        const auto d = derive_occupancy(fa_sense(2000000), 900, 250000);
        CHECK(d.interference_valid);
        CHECK(d.interference_util_permille > 990);
        CHECK_EQ_U(d.util_permille, 1000);
    }
    // Noise preference: absolute idle floor beats the passive floor; the
    // passive floor stands alone when absolute is absent.
    {
        AirIface::AirSense s = fa_sense(0);
        s.nf_valid = true;
        s.nf_dbm = -93.6;
        const auto passive = derive_occupancy(s, 0, 1000);
        CHECK(passive.noise_valid);
        CHECK(passive.noise_dbm == -94);
        s.abs_nf_valid = true;
        s.abs_nf_dbm = -101;
        const auto absolute = derive_occupancy(s, 0, 1000);
        CHECK(absolute.noise_valid);
        CHECK(absolute.noise_dbm == -101);
    }
}

void test_ranking() {
    // The issue's defining case: channel 5745 is quiet to the decoder
    // (wifi_util low) but saturated by a non-decodable emitter; ranking on
    // the interference-inclusive total must NOT pick it.
    {
        const std::vector<ChannelUtil> m{{5745, 810}, {5785, 100}, {5825, 40}};
        CHECK_EQ_U(emptiest_channel(m, {5745, 5785, 5825}, 0), 5825);
    }
    // `except` skips the craft's current channel even when it is emptiest.
    {
        const std::vector<ChannelUtil> m{{5745, 10}, {5785, 100}};
        CHECK_EQ_U(emptiest_channel(m, {5745, 5785}, 5745), 5785);
    }
    // Channels outside the allowlist never win; nothing measured = 0.
    {
        const std::vector<ChannelUtil> m{{5180, 0}};
        CHECK_EQ_U(emptiest_channel(m, {5745, 5785}, 0), 0);
        CHECK_EQ_U(emptiest_channel({}, {5745, 5785}, 0), 0);
    }
    // Sensor-less rows (util == wifi_util) compete on equal terms — the
    // structural no-sensor fallback needs no special case.
    {
        const std::vector<ChannelUtil> m{{5745, 120}, {5785, 90}};
        CHECK_EQ_U(emptiest_channel(m, {5745, 5785}, 0), 5785);
    }
}

}  // namespace

int main() {
    test_derivation();
    test_ranking();
    return wbtest_finish("scout_sense_test");
}
