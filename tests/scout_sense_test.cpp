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
        CHECK_EQ_U(emptiest_channel(m, {5745, 5785, 5825}, 0, 40), 5825);
    }
    // `except` skips the craft's current channel even when it is emptiest.
    {
        const std::vector<ChannelUtil> m{{5745, 10}, {5785, 100}};
        CHECK_EQ_U(emptiest_channel(m, {5745, 5785}, 5745, 40), 5785);
    }
    // Channels outside the allowlist never win; nothing measured = 0.
    {
        const std::vector<ChannelUtil> m{{5180, 0}};
        CHECK_EQ_U(emptiest_channel(m, {5745, 5785}, 0, 40), 0);
        CHECK_EQ_U(emptiest_channel({}, {5745, 5785}, 0, 40), 0);
    }
    // Sensor-less rows (util == wifi_util) compete on equal terms — the
    // structural no-sensor fallback needs no special case. Both sit inside
    // each other's guard band, so the guard-band key ties and the tie-break
    // on own utilisation decides, exactly as it did before the guard existed.
    {
        const std::vector<ChannelUtil> m{{5745, 120}, {5785, 90}};
        CHECK_EQ_U(emptiest_channel(m, {5745, 5785}, 0, 40), 5785);
    }
    // The bench failure, 2026-08-16. A craft transmits on 5720, so 5720 reads
    // 1000 and is correctly ranked last. Every OTHER channel reads the
    // empty-band floor, which a real 25-channel sweep measured as 891-968 —
    // a 77-permille spread with nothing in it, so which empty channel wins is
    // decided by noise. Here the noise favours 5700, one slot from the live
    // craft, and the per-channel ranking hands it over. The guard band is what
    // makes 5720's occupancy reach the channels it actually jams.
    {
        const std::vector<ChannelUtil> m{
            {5660, 935}, {5680, 934}, {5700, 929}, {5720, 1000}, {5745, 936}};
        const std::vector<uint16_t> allow{5660, 5680, 5700, 5720, 5745};
        const uint16_t pick = emptiest_channel(m, allow, 0, 40);
        CHECK(pick != 5700);   // 20 MHz out — measured 0.0 fps of a nominal 60
        CHECK(pick != 5680);   // 40 MHz out — measured 20.8 fps
        CHECK(pick != 5745);   // 25 MHz out on the other side — measured 0.4
        CHECK_EQ_U(pick, 5660);  // 60 MHz out — the first separation that works
    }
    // ...and it must not overreach: a busy channel outside the guard band
    // still leaves its distant neighbours rankable on their own merit.
    {
        const std::vector<ChannelUtil> m{{5500, 1000}, {5560, 900}, {5580, 800}};
        CHECK_EQ_U(emptiest_channel(m, {5500, 5560, 5580}, 0, 40), 5580);
    }
    // An occupied channel excluded by `except` still guards its neighbours —
    // `except` means "the craft is here, move it", not "this energy is gone".
    {
        const std::vector<ChannelUtil> m{{5740, 0}, {5745, 1000}, {5765, 20},
                                         {5825, 30}};
        CHECK_EQ_U(emptiest_channel(m, {5765, 5825}, 5745, 40), 5825);
    }
    // guard 0 is the pre-guard per-channel ranking, byte for byte. A
    // non-DFS-only deployment has 9 channels and a 40 MHz guard costs it 5
    // per occupied channel, so turning the guard off has to give back exactly
    // the old behaviour rather than something almost like it.
    {
        const std::vector<ChannelUtil> m{
            {5660, 935}, {5680, 934}, {5700, 929}, {5720, 1000}, {5745, 936}};
        const std::vector<uint16_t> allow{5660, 5680, 5700, 5720, 5745};
        CHECK_EQ_U(emptiest_channel(m, allow, 0, 0), 5700);
        const std::vector<ChannelUtil> n{{5740, 0}, {5745, 1000}, {5765, 20},
                                         {5825, 30}};
        CHECK_EQ_U(emptiest_channel(n, {5765, 5825}, 5745, 0), 5765);
    }
}

}  // namespace

int main() {
    test_derivation();
    test_ranking();
    return wbtest_finish("scout_sense_test");
}
