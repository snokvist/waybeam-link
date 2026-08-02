// SPDX-License-Identifier: GPL-2.0-or-later
// §3.16 UPLINK_QUALITY codec tests (Pass 125): exact 35-byte round trip
// including a negative RSSI sum and every last_rx_mcs shape, buffer/length
// refusal, the structural destination==target_originator invariant, and the
// MAC's coverage of bytes 0..30 — flipping the delivered-rung byte must
// invalidate the tag, which is the whole point of authenticating it.
#include <cstring>
#include <string>
#include <vector>

#include "wblink/hmac_sha256.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

constexpr uint16_t kCraft = 17;
constexpr uint16_t kGround = 9;
constexpr uint32_t kCraftSession = 0xA1B2C3D4;
constexpr uint32_t kGroundSession = 4242;

const std::vector<uint8_t> kPsk = {'s', 'e', 'c', 'r', 'e', 't'};

UplinkQuality make_quality() {
    UplinkQuality q;
    q.prefix = {kCraft, kGround, kCraftSession};
    q.target_originator = kGround;
    q.target_session = kGroundSession;
    q.last_report_epoch = 1000;
    q.reports_received = 990;
    q.rssi_sum_dbm = 0;
    q.craft_adapter_fingerprint = 0x5A;
    q.last_rx_mcs = 0;
    return q;
}

uint32_t mac_for(const UplinkQuality& q) {
    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    return quality_mac(kPsk.data(), kPsk.size(), buf);
}

// Case 1: exact 35-byte round trip, negative RSSI sum, last_rx_mcs 0/7/0xFF.
void test_roundtrip() {
    // §3.16 stores the two's-complement WIRE IMAGE, so a negative cumulative
    // sum must survive encode->decode bit-exact.
    const int32_t kNegSum = -1234567;
    for (uint8_t mcs : {uint8_t{0}, uint8_t{7}, kUplinkRxMcsUnknown}) {
        UplinkQuality q = make_quality();
        q.last_rx_mcs = mcs;
        q.rssi_sum_dbm = static_cast<uint32_t>(kNegSum);
        q.quality_mac = mac_for(q);

        uint8_t buf[kUplinkQualitySize];
        CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)),
                   kUplinkQualitySize);
        const Decoded d = decode(buf, kUplinkQualitySize);
        const UplinkQuality* v = std::get_if<UplinkQuality>(&d);
        CHECK(v != nullptr);
        if (v == nullptr) return;
        CHECK(*v == q);
        CHECK_EQ_U(v->last_rx_mcs, mcs);
        CHECK(static_cast<int32_t>(v->rssi_sum_dbm) == kNegSum);
    }

    // Field offsets are law (§3.16) — pin the delivered-rung byte and the
    // MAC's position so a future field insert cannot slide them silently.
    UplinkQuality q = make_quality();
    q.last_rx_mcs = 7;
    q.quality_mac = mac_for(q);
    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    CHECK_EQ_U(buf[29], 0x5A);  // craft_adapter_fingerprint
    CHECK_EQ_U(buf[30], 7);     // last_rx_mcs
}

// Case 2: encode refuses a short buffer; decode rejects short and long input.
void test_buffer_and_length() {
    UplinkQuality q = make_quality();
    q.quality_mac = mac_for(q);
    uint8_t buf[kUplinkQualitySize + 1];
    CHECK_EQ_U(encode_uplink_quality(q, buf, kUplinkQualitySize - 1), 0);
    CHECK_EQ_U(encode_uplink_quality(q, nullptr, sizeof(buf)), 0);
    CHECK_EQ_U(encode_uplink_quality(q, buf, kUplinkQualitySize),
               kUplinkQualitySize);

    const Decoded shorter = decode(buf, kUplinkQualitySize - 1);
    CHECK(std::get_if<DecodeError>(&shorter) != nullptr);
    // A trailing byte is an error, not padding — devourer hands us the exact
    // MAC payload boundary.
    buf[kUplinkQualitySize] = 0;
    const Decoded longer = decode(buf, kUplinkQualitySize + 1);
    const DecodeError* e = std::get_if<DecodeError>(&longer);
    CHECK(e != nullptr);
    if (e != nullptr) CHECK(*e == DecodeError::kLengthMismatch);
}

// §3.16: target_originator MUST be non-zero and equal `destination`. Both are
// packet-internal, so both encode and decode enforce them.
void test_structural_target() {
    UplinkQuality q = make_quality();
    q.target_originator = 0;
    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), 0);

    q = make_quality();
    q.target_originator = kGround + 1;  // disagrees with prefix.destination
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), 0);

    // Forge the mismatch on the wire — decode must still refuse it.
    q = make_quality();
    q.quality_mac = mac_for(q);
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    buf[11] = 0;
    buf[12] = 0;  // target_originator := 0
    const Decoded zero = decode(buf, kUplinkQualitySize);
    const DecodeError* ez = std::get_if<DecodeError>(&zero);
    CHECK(ez != nullptr);
    if (ez != nullptr) CHECK(*ez == DecodeError::kInvalidField);
}

// Case 3: a wrong MAC is rejected, and the MAC covers bytes 0..30 — flipping
// last_rx_mcs (offset 30) must invalidate the tag.
void test_mac_coverage() {
    UplinkQuality q = make_quality();
    q.last_rx_mcs = 0;
    const uint32_t good = mac_for(q);
    q.quality_mac = good;

    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    CHECK_EQ_U(quality_mac(kPsk.data(), kPsk.size(), buf), good);

    // A different key yields a different tag.
    const std::vector<uint8_t> other = {'o', 't', 'h', 'e', 'r'};
    CHECK(quality_mac(other.data(), other.size(), buf) != good);

    // Offset 30 is inside the MAC input: same counters, different rung.
    UplinkQuality rung = q;
    rung.last_rx_mcs = 7;
    CHECK(mac_for(rung) != good);

    // So is every counter field.
    UplinkQuality bumped = q;
    bumped.reports_received += 1;
    CHECK(mac_for(bumped) != good);
    UplinkQuality fp = q;
    fp.craft_adapter_fingerprint ^= 0xFF;
    CHECK(mac_for(fp) != good);
}

// Case 13 (codec half): an unknown type must not be mistaken for 0xF, and a
// 0xF-typed buffer of the wrong length must not decode as some other type.
void test_type_isolation() {
    UplinkQuality q = make_quality();
    q.quality_mac = mac_for(q);
    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    // ver_type low nibble is the type; 0xF is UPLINK_QUALITY.
    CHECK_EQ_U(buf[2] & 0x0F, 0x0F);
    // A 35-byte SELECTOR_STATE-typed buffer is a length error, not a quality
    // packet — the two shapes are 34/36 and must not alias.
    buf[2] = static_cast<uint8_t>((buf[2] & 0xF0) | 0x0E);
    const Decoded d = decode(buf, kUplinkQualitySize);
    CHECK(std::get_if<UplinkQuality>(&d) == nullptr);
    CHECK(std::get_if<DecodeError>(&d) != nullptr);
}

}  // namespace

int main() {
    test_roundtrip();
    test_buffer_and_length();
    test_structural_target();
    test_mac_coverage();
    test_type_isolation();
    return wbtest_finish("uplink_quality_test");
}
