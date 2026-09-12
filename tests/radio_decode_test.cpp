// SPDX-License-Identifier: GPL-2.0-or-later
// G8: the devourer RX descriptor decode rules. Pure functions of a buffer and
// a few descriptor fields, so no fake adapter is needed — which is the whole
// reason they were lifted out of RadioAir::on_packet.
#include "wblink/radio_decode.h"

#include <cstdio>

// The ID table is header-only and present in the vendored tree whether or not
// the MT7612U BACKEND is compiled, so this check runs under the default
// `fleet` merge gate too. Guarding it on DEVOURER_HAVE_MT7612U would have made
// it dead code exactly where it needs to run: `dev` builds chips=fleet.
#include "mt7612u/Mt7612uUsbIds.h"

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

// §3.0 enumeration. A `true` here puts the device on the claim path, which
// DETACHES its kernel driver — so a false positive takes the host's own radio
// off the air rather than merely failing an open. The stub stands in for
// devourer's mt7612u::is_usb_id table so both build arms run in one binary.
bool stub_mt7612u_id(uint16_t vid, uint16_t pid) {
    return vid == 0x0e8d && (pid == 0x7612 || pid == 0x7632);
}

void test_radio_vendor_ok() {
    // Realtek is accepted whether or not MT7612U was built — the MediaTek
    // clause is additive, never a replacement.
    CHECK(radio_vendor_ok(0x0bda, 0x8812, false, stub_mt7612u_id));
    CHECK(radio_vendor_ok(0x0bda, 0x8812, true, stub_mt7612u_id));
    CHECK(radio_vendor_ok(0x0bda, 0xa81b, false, stub_mt7612u_id));

    // MT7612U only when the family is actually compiled in. Without the
    // backend a claimed dongle has no driver, so accepting it would detach a
    // kernel driver to reach a device nothing can talk to.
    CHECK(!radio_vendor_ok(0x0e8d, 0x7612, false, stub_mt7612u_id));
    CHECK(radio_vendor_ok(0x0e8d, 0x7612, true, stub_mt7612u_id));
    CHECK(radio_vendor_ok(0x0e8d, 0x7632, true, stub_mt7612u_id));

    // THE one that matters: match the ID TABLE, never MediaTek's vendor id.
    // 0e8d:0616 is an internal laptop combo radio and is present on the dev
    // host — a vendor-wide test would take the machine's own WiFi off the air.
    CHECK(!radio_vendor_ok(0x0e8d, 0x0616, true, stub_mt7612u_id));
    CHECK(!radio_vendor_ok(0x0e8d, 0x0616, false, stub_mt7612u_id));
    // Neither vendor, either arm.
    CHECK(!radio_vendor_ok(0x1234, 0x7612, true, stub_mt7612u_id));

    // And against devourer's REAL table, not just the stub: a stub that
    // drifted from the vendored table would otherwise let this pass while the
    // shipping predicate matched something else.
    CHECK(mt7612u::is_usb_id(0x0e8d, 0x7612));
    CHECK(!mt7612u::is_usb_id(0x0e8d, 0x0616));
    CHECK(!mt7612u::is_usb_id(0x0bda, 0x8812));  // disjoint from Realtek
    // The table must stay disjoint from Realtek's VID entirely, or the two
    // clauses of radio_vendor_ok() would overlap and the MediaTek arm could
    // silently widen the Realtek one.
    for (uint16_t pid = 0x8000; pid < 0x9000; ++pid) {
        if (mt7612u::is_usb_id(0x0bda, pid)) {
            CHECK(false);  // a Realtek-VID entry in the MediaTek table
            break;
        }
    }
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
    test_radio_vendor_ok();
    test_rssi_dbm_from_chains();
    return wbtest_finish("radio_decode_test");
}
