// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the shared radiotap helpers (io/include/wblink/radiotap.h):
// the HT-MCS TX header builder every injected frame carries (§3.0 Pass 118),
// and the HT20 airtime model. The RX parser was deleted in the kernel-monitor
// retirement close-out — MonAir was its only caller; devourer takes FCS state
// from RxAtrib.crc_err and length from mpdu_len_without_fcs().
#include "wblink/radiotap.h"
#include "wblink/airtime.h"

#include <cstdint>

#include "wbtest.h"

using namespace wblink;

namespace {

void test_tx_ht_bytes() {
    uint8_t b[kRadiotapTxHtLen];
    const size_t n = radiotap_tx_ht(b, 7, /*sgi=*/false, /*bw=*/20);
    CHECK_EQ_U(n, kRadiotapTxHtLen);
    // version/pad/len
    CHECK_EQ_U(b[0], 0x00);
    CHECK_EQ_U(b[1], 0x00);
    CHECK_EQ_U(b[2], 0x0d);
    CHECK_EQ_U(b[3], 0x00);
    // it_present = TX_FLAGS|MCS = 0x00088000 (LE)
    CHECK_EQ_U(b[4], 0x00);
    CHECK_EQ_U(b[5], 0x80);
    CHECK_EQ_U(b[6], 0x08);
    CHECK_EQ_U(b[7], 0x00);
    // tx_flags = NOACK (0x0008)
    CHECK_EQ_U(b[8], 0x08);
    CHECK_EQ_U(b[9], 0x00);
    // MCS known = BW|MCS|GI|FEC|STBC = 0x37
    CHECK_EQ_U(b[10], 0x37);
    CHECK_EQ_U(b[11], 0x00);  // flags: 20 MHz, long GI
    CHECK_EQ_U(b[12], 7);     // MCS index

    // SGI + 40 MHz sets flags 0x04|0x01.
    (void)radiotap_tx_ht(b, 5, /*sgi=*/true, /*bw=*/40);
    CHECK_EQ_U(b[11], 0x05);
    CHECK_EQ_U(b[12], 5);

    // Pass-12 unicast return: TX_FLAGS cleared so the frame solicits an ACK,
    // while the MCS field is carried exactly as on the broadcast frame.
    (void)radiotap_tx_ht(b, 3, /*sgi=*/false, /*bw=*/20, kTxFlagsAck);
    CHECK_EQ_U(b[8], 0x00);
    CHECK_EQ_U(b[9], 0x00);
    CHECK_EQ_U(b[10], 0x37);
    CHECK_EQ_U(b[12], 3);

    // §3.0 Pass 157 coding: FEC=LDPC is flags bit 4, STBC stream count is
    // bits 6:5 — the known mask already claimed both, so only the flags
    // byte moves, and the default (off) stays byte-identical to Pass 118.
    (void)radiotap_tx_ht(b, 5, /*sgi=*/false, /*bw=*/20, kTxFlagsNoAck,
                         /*ldpc=*/true, /*stbc=*/0);
    CHECK_EQ_U(b[10], 0x37);
    CHECK_EQ_U(b[11], 0x10);  // LDPC alone
    (void)radiotap_tx_ht(b, 5, /*sgi=*/true, /*bw=*/20, kTxFlagsNoAck,
                         /*ldpc=*/true, /*stbc=*/1);
    CHECK_EQ_U(b[11], 0x34);  // SGI | LDPC | one STBC stream
    (void)radiotap_tx_ht(b, 5, /*sgi=*/false, /*bw=*/20, kTxFlagsNoAck,
                         /*ldpc=*/false, /*stbc=*/3);
    CHECK_EQ_U(b[11], 0x60);  // stream count saturates the 2-bit field
}

void test_ht20_service_time() {
    // 1000 B at MCS0/LGI 6.5 Mbps = ceil(8,000,000 / 6500) us.
    auto us = ht20_service_time_us(1000, 0, false, 1000);
    CHECK(us.has_value());
    CHECK_EQ_U(*us, 1231);
    // MCS3/SGI is conservatively floored to 28,888 kbps, then a measured
    // 600-permille service calibration yields 17,332 kbps.
    us = ht20_service_time_us(1424, 3, true, 600);
    CHECK(us.has_value());
    CHECK_EQ_U(*us, 658);
    CHECK(!ht20_service_time_us(1000, 8, false, 600).has_value());
    CHECK(!ht20_service_time_us(1000, 3, true, 0).has_value());
}

}  // namespace

int main() {
    test_tx_ht_bytes();
    test_ht20_service_time();
    return wbtest_finish("radiotap_test");
}
