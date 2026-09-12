// SPDX-License-Identifier: GPL-2.0-or-later
// G8: the devourer RX descriptor decode rules. Pure functions of a buffer and
// a few descriptor fields, so no fake adapter is needed — which is the whole
// reason they were lifted out of RadioAir::on_packet.
#include "wblink/radio_decode.h"

#include <cstdio>

#include "wbtest.h"

namespace {

using namespace wblink;

void test_desc_rate_to_mcs() {
    // Below DESC_RATEMCS0 = 0x0c the code is CCK or legacy OFDM, not HT.
    CHECK_EQ_U(desc_rate_to_mcs(0x00), kRxMcsUnknown);   // CCK 1M
    CHECK_EQ_U(desc_rate_to_mcs(0x03), kRxMcsUnknown);   // CCK 11M
    CHECK_EQ_U(desc_rate_to_mcs(0x04), kRxMcsUnknown);   // OFDM 6M
    CHECK_EQ_U(desc_rate_to_mcs(0x0b), kRxMcsUnknown);   // OFDM 54M — last
    // The §9.3 ladder: MCS0..7 map one-for-one off the base.
    CHECK_EQ_U(desc_rate_to_mcs(0x0c), 0u);
    CHECK_EQ_U(desc_rate_to_mcs(0x0d), 1u);
    CHECK_EQ_U(desc_rate_to_mcs(0x13), 7u);
    // MCS8+ is a second spatial stream — real on a 2T2R part, off the v0
    // single-stream ladder, so it must read unresolved rather than alias
    // back onto a bucket. This is the case that makes rx_mcs sum to rx_frames.
    CHECK_EQ_U(desc_rate_to_mcs(0x14), kRxMcsUnknown);
    CHECK_EQ_U(desc_rate_to_mcs(0x1b), kRxMcsUnknown);   // MCS15
    CHECK_EQ_U(desc_rate_to_mcs(0xffff), kRxMcsUnknown);
}

void test_mpdu_len_without_fcs() {
    // A frame that is all trailer, or shorter, is not a frame.
    CHECK(!mpdu_len_without_fcs(0, true).has_value());
    CHECK(!mpdu_len_without_fcs(kFcsLen - 1, true).has_value());
    CHECK(!mpdu_len_without_fcs(kFcsLen, true).has_value());
    // One byte of MPDU behind the trailer is still a length the parser can
    // reject on its own terms rather than one that underflows on the way in.
    const auto one = mpdu_len_without_fcs(kFcsLen + 1, true);
    CHECK(one.has_value());
    if (one) CHECK_EQ_U(*one, 1u);
    const auto typical = mpdu_len_without_fcs(1500, true);
    CHECK(typical.has_value());
    if (typical) CHECK_EQ_U(*typical, 1500u - kFcsLen);

    // fcs_present=false (MediaTek MT7612U: the MAC strips the FCS and the
    // four bytes past the MPDU are an FCE info trailer). Every delivered byte
    // is MPDU — taking four off deletes real payload, and decode_data's
    // length-exact check then rejects EVERY frame that ear hears, silently.
    const auto no_fcs = mpdu_len_without_fcs(1500, false);
    CHECK(no_fcs.has_value());
    if (no_fcs) CHECK_EQ_U(*no_fcs, 1500u);
    // The two arms must disagree by exactly the trailer — a fix that fed the
    // flag through but kept subtracting would still pass the arms above.
    CHECK(mpdu_len_without_fcs(1500, false).value() -
              mpdu_len_without_fcs(1500, true).value() ==
          kFcsLen);
    // Only "no bytes at all" is not a frame when nothing is stripped; a
    // length inside the old trailer window is now a legitimate short frame.
    CHECK(!mpdu_len_without_fcs(0, false).has_value());
    const auto tiny = mpdu_len_without_fcs(kFcsLen, false);
    CHECK(tiny.has_value());
    if (tiny) CHECK_EQ_U(*tiny, kFcsLen);
}

void test_rssi_dbm_from_chains() {
    // 0 on every chain = no PHY report on this frame. The previous reading is
    // kept: a report-less frame says nothing about signal, and inventing a
    // -110 floor here would read as a link that just collapsed.
    const uint8_t none[2] = {0, 0};
    CHECK(static_cast<int>(rssi_dbm_from_chains(none, 2, -47)) == -47);
    CHECK(static_cast<int>(rssi_dbm_from_chains(none, 2, -128)) == -128);
    CHECK(static_cast<int>(rssi_dbm_from_chains(nullptr, 0, -60)) == -60);  // no chains at all

    // dBm = value - 110, best chain wins regardless of order.
    const uint8_t one[1] = {60};
    CHECK(static_cast<int>(rssi_dbm_from_chains(one, 1, -128)) == -50);
    const uint8_t asc[2] = {40, 90};
    const uint8_t desc[2] = {90, 40};
    CHECK(static_cast<int>(rssi_dbm_from_chains(asc, 2, -128)) == -20);
    CHECK(static_cast<int>(rssi_dbm_from_chains(desc, 2, -128)) == -20);
    // One live chain beside a silent one is not "no report" — best-of, not
    // any-of, so a single-chain report still updates.
    const uint8_t half[2] = {0, 100};
    CHECK(static_cast<int>(rssi_dbm_from_chains(half, 2, -128)) == -10);

    // Upper clamp: a chain byte above 110 would report positive dBm.
    const uint8_t hot[1] = {111};
    CHECK(static_cast<int>(rssi_dbm_from_chains(hot, 1, -128)) == 0);
    const uint8_t max[1] = {255};
    CHECK(static_cast<int>(rssi_dbm_from_chains(max, 1, -128)) == 0);
    // The floor is -110 for a reachable input (chain byte 1), NOT -128: the
    // input is unsigned, so the lower clamp in the implementation cannot fire.
    // Pinned so a future signed input does not silently change the range.
    const uint8_t faint[1] = {1};
    CHECK(static_cast<int>(rssi_dbm_from_chains(faint, 1, -128)) == -109);
}

}  // namespace

int main() {
    test_desc_rate_to_mcs();
    test_mpdu_len_without_fcs();
    test_rssi_dbm_from_chains();
    return wbtest_finish("radio_decode_test");
}
