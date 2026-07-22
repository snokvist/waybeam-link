// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the kernel-monitor radiotap helpers (io/include/wblink/
// radiotap.h): the HT-MCS TX header builder and the RX parser that extracts
// RSSI + TSFT and reports the header length to strip before dot11_parse.
#include "wblink/radiotap.h"
#include "wblink/airtime.h"

#include <cstdint>
#include <vector>

#include "wblink/dot11.h"
#include "wbtest.h"

using namespace wblink;

namespace {

// Build a realistic monitor-RX radiotap header carrying TSFT, FLAGS, RATE,
// CHANNEL, DBM_ANTSIGNAL, ANTENNA (present mask 0x82F) — 24 bytes total.
std::vector<uint8_t> make_rx_radiotap(uint64_t tsf, int8_t rssi,
                                      bool fcs_at_end = false) {
    std::vector<uint8_t> h(24, 0);
    h[0] = 0x00;  // version
    h[1] = 0x00;  // pad
    h[2] = 24;    // it_len lo
    h[3] = 0;     // it_len hi
    // present = TSFT|FLAGS|RATE|CHANNEL|DBM_ANTSIGNAL|ANTENNA = 0x0000082F
    const uint32_t present = 0x0000082Fu;
    h[4] = static_cast<uint8_t>(present & 0xff);
    h[5] = static_cast<uint8_t>((present >> 8) & 0xff);
    h[6] = static_cast<uint8_t>((present >> 16) & 0xff);
    h[7] = static_cast<uint8_t>((present >> 24) & 0xff);
    // TSFT u64 LE at 8..15
    for (int i = 0; i < 8; ++i) {
        h[8 + static_cast<size_t>(i)] =
            static_cast<uint8_t>((tsf >> (8 * i)) & 0xff);
    }
    h[16] = fcs_at_end ? 0x10 : 0x00;      // FLAGS: FCS-at-end
    h[17] = 0x0c;                          // RATE
    h[18] = 0x6c;                          // CHANNEL freq lo (5180 & 0xff)
    h[19] = 0x14;                          // CHANNEL freq hi (5180 >> 8)
    h[20] = 0x00;                          // CHANNEL flags lo
    h[21] = 0x01;                          // CHANNEL flags hi
    h[22] = static_cast<uint8_t>(rssi);    // DBM_ANTSIGNAL (s8)
    h[23] = 0x00;                          // ANTENNA
    return h;
}

void test_tx_ht_bytes() {
    uint8_t b[kMonRadiotapHtLen];
    const size_t n = mon_radiotap_ht(b, 7, /*sgi=*/false, /*bw=*/20);
    CHECK_EQ_U(n, kMonRadiotapHtLen);
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
    (void)mon_radiotap_ht(b, 5, /*sgi=*/true, /*bw=*/40);
    CHECK_EQ_U(b[11], 0x05);
    CHECK_EQ_U(b[12], 5);
}

void test_rx_parse() {
    const uint64_t tsf = 0x0123456789ABCDEFull;
    const int8_t rssi = -55;
    auto rt = make_rx_radiotap(tsf, rssi);
    auto r = radiotap_parse(rt.data(), rt.size());
    CHECK(r.has_value());
    CHECK_EQ_U(r->hdr_len, 24);
    CHECK(r->tsf_us.has_value());
    CHECK_EQ_U(*r->tsf_us, tsf);
    CHECK(r->rssi_dbm.has_value());
    CHECK(*r->rssi_dbm == rssi);
    CHECK(!r->fcs_at_end);

    rt = make_rx_radiotap(tsf, rssi, /*fcs_at_end=*/true);
    r = radiotap_parse(rt.data(), rt.size());
    CHECK(r.has_value());
    CHECK(r->fcs_at_end);
}

void test_rx_strip_then_dot11() {
    // radiotap(24) + §3.0 header(24) + magic(2) + payload + FCS(4).
    auto frame = make_rx_radiotap(42, -60, /*fcs_at_end=*/true);
    std::vector<uint8_t> mpdu(kDot11HdrLen, 0);
    dot11_hdr24(mpdu.data(), /*net_id=*/0, /*orig=*/0x1234, /*adapter=*/0,
                /*seq=*/9);
    mpdu.push_back(0x57);  // payload magic
    mpdu.push_back(0x42);
    const uint8_t extra[3] = {0xAA, 0xBB, 0xCC};
    for (uint8_t x : extra) {
        mpdu.push_back(x);
    }
    frame.insert(frame.end(), mpdu.begin(), mpdu.end());
    for (int i = 0; i < 4; ++i) {
        frame.push_back(0xFF);  // fake FCS
    }
    auto r = radiotap_parse(frame.data(), frame.size());
    CHECK(r.has_value());
    const size_t rlen = r->hdr_len;
    const size_t fcs_len = r->fcs_at_end ? kFcsLen : 0;
    CHECK(rlen + fcs_len < frame.size());
    const uint8_t* body = frame.data() + rlen;
    const size_t body_len = frame.size() - rlen - fcs_len;
    auto d = dot11_parse(body, body_len, std::nullopt);
    CHECK(d.has_value());
    CHECK_EQ_U(d->originator, 0x1234);
    CHECK_EQ_U(d->payload_len, 5);  // magic(2) + 3 payload bytes

    // Drivers such as mt7921 may omit the FCS and clear the radiotap flag.
    auto no_fcs = make_rx_radiotap(42, -60);
    no_fcs.insert(no_fcs.end(), mpdu.begin(), mpdu.end());
    r = radiotap_parse(no_fcs.data(), no_fcs.size());
    CHECK(r.has_value());
    CHECK(!r->fcs_at_end);
    body = no_fcs.data() + r->hdr_len;
    d = dot11_parse(body, no_fcs.size() - r->hdr_len, std::nullopt);
    CHECK(d.has_value());
    CHECK_EQ_U(d->payload_len, 5);
}

void test_rx_malformed() {
    // Too short.
    const uint8_t tiny[4] = {0, 0, 4, 0};
    CHECK(!radiotap_parse(tiny, sizeof(tiny)).has_value());
    // it_len larger than the buffer.
    uint8_t bad[8] = {0, 0, 0xff, 0x00, 0, 0, 0, 0};
    CHECK(!radiotap_parse(bad, sizeof(bad)).has_value());
    // null.
    CHECK(!radiotap_parse(nullptr, 32).has_value());
    // Header with NO signal/tsf fields present (present=0): parses, both absent.
    uint8_t empty[8] = {0, 0, 8, 0, 0, 0, 0, 0};
    auto r = radiotap_parse(empty, sizeof(empty));
    CHECK(r.has_value());
    CHECK(!r->rssi_dbm.has_value());
    CHECK(!r->tsf_us.has_value());
    CHECK_EQ_U(r->hdr_len, 8);
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
    test_rx_parse();
    test_rx_strip_then_dot11();
    test_rx_malformed();
    test_ht20_service_time();
    return wbtest_finish("radiotap_test");
}
