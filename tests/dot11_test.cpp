// SPDX-License-Identifier: GPL-2.0-or-later
// §3.0 on-air encapsulation: TX prefix layout is exactly the pinned frame,
// build→parse roundtrips, the RX filter accepts/rejects on Frame Control /
// SA prefix / net_id / payload magic, and short frames never parse.
#include "wblink/dot11.h"

#include <cstring>
#include <vector>

#include "wbtest.h"

using namespace wblink;

namespace {

// A §3.0 MPDU carrying `payload`, as an RX would see it (radiotap stripped).
std::vector<uint8_t> mpdu_of(uint8_t net_id, uint16_t orig, uint8_t adapter,
                             uint16_t seq, const std::vector<uint8_t>& pay) {
    std::vector<uint8_t> f(kDot11TxPrefixLen + pay.size());
    dot11_tx_prefix(f.data(), net_id, orig, adapter, seq);
    std::memcpy(f.data() + kDot11TxPrefixLen, pay.data(), pay.size());
    f.erase(f.begin(), f.begin() + kRadiotapTxLen);
    return f;
}

const std::vector<uint8_t> kPay = {0x57, 0x42, 0x01, 0xaa, 0xbb};  // magic…

}  // namespace

int main() {
    // --- TX prefix byte layout (the normative §3.0 table) -------------------
    {
        uint8_t buf[kDot11TxPrefixLen];
        const size_t n = dot11_tx_prefix(buf, 5, 0x1234, 2, 0x0abc);
        CHECK_EQ_U(n, kDot11TxPrefixLen);
        // Radiotap: version 0, len 10, present = TX_FLAGS, flags = NOACK.
        CHECK_EQ_U(buf[0], 0x00);
        CHECK_EQ_U(buf[2], 0x0a);
        CHECK_EQ_U(buf[5], 0x80);
        CHECK_EQ_U(buf[8], 0x08);
        const uint8_t* h = buf + kRadiotapTxLen;
        CHECK_EQ_U(h[0], 0x08);  // Data, not QoS
        CHECK_EQ_U(h[1], 0x00);  // ToDS=0 FromDS=0
        CHECK_EQ_U(h[2], 0x00);  // duration
        for (int i = 4; i < 10; ++i) CHECK_EQ_U(h[i], 0xff);  // DA broadcast
        CHECK_EQ_U(h[10], 0x56);  // SA prefix "VB" — unicast TA (Pass 8)
        CHECK_EQ_U(h[11], 0x42);
        CHECK_EQ_U(h[12], 5);     // net_id
        CHECK_EQ_U(h[13], 0x12);  // originator BE
        CHECK_EQ_U(h[14], 0x34);
        CHECK_EQ_U(h[15], 2);     // adapter idx
        CHECK_EQ_U(h[16], 0x56);  // BSSID "VBLK"
        CHECK_EQ_U(h[17], 0x42);
        CHECK_EQ_U(h[18], 0x4c);
        CHECK_EQ_U(h[19], 0x4b);
        CHECK_EQ_U(h[22], 0xc0);  // seq 0x0abc << 4, fragment 0
        CHECK_EQ_U(h[23], 0xab);
    }

    // --- roundtrip + filter -------------------------------------------------
    {
        const auto f = mpdu_of(7, 0xbeef, 1, 1, kPay);
        // No net_id configured: accept any.
        auto r = dot11_parse(f.data(), f.size());
        CHECK(r.has_value());
        if (r) {
            CHECK_EQ_U(r->net_id, 7);
            CHECK_EQ_U(r->originator, 0xbeef);
            CHECK_EQ_U(r->adapter_idx, 1);
            CHECK_EQ_U(r->payload_len, kPay.size());
            CHECK(std::memcmp(r->payload, kPay.data(), kPay.size()) == 0);
        }
        // Matching net_id accepted; mismatch rejected.
        CHECK(dot11_parse(f.data(), f.size(), uint8_t{7}).has_value());
        CHECK(!dot11_parse(f.data(), f.size(), uint8_t{8}).has_value());
    }

    // --- rejections ----------------------------------------------------------
    {
        auto f = mpdu_of(0, 1, 0, 0, kPay);
        // Wrong Frame Control (QoS Data, beacon, ToDS set).
        auto qos = f;
        qos[0] = 0x88;
        CHECK(!dot11_parse(qos.data(), qos.size()).has_value());
        auto beacon = f;
        beacon[0] = 0x80;
        CHECK(!dot11_parse(beacon.data(), beacon.size()).has_value());
        auto tods = f;
        tods[1] = 0x01;
        CHECK(!dot11_parse(tods.data(), tods.size()).has_value());
        // Foreign SA prefix (ambient traffic).
        auto foreign = f;
        foreign[10] = 0x00;
        CHECK(!dot11_parse(foreign.data(), foreign.size()).has_value());
        // Payload without the wire magic.
        auto nomagic = f;
        nomagic[24] = 0x00;
        CHECK(!dot11_parse(nomagic.data(), nomagic.size()).has_value());
        // Too short: bare header, header+1, null.
        CHECK(!dot11_parse(f.data(), kDot11HdrLen).has_value());
        CHECK(!dot11_parse(f.data(), kDot11HdrLen + 1).has_value());
        CHECK(!dot11_parse(nullptr, 100).has_value());
    }

    return wbtest_finish("dot11_test");
}
