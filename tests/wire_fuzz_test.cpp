// SPDX-License-Identifier: GPL-2.0-or-later
// Decode-robustness sweep (run under ASan+UBSan in the dev preset):
//   1. random buffers must never crash the decoder;
//   2. every valid packet truncated at every length must yield a DecodeError;
//   3. single-byte mutations of valid packets must never crash, and anything
//      that still decodes must be length-consistent;
//   4. extended (trailing-byte) packets must be rejected.
#include <cstring>
#include <variant>
#include <vector>

#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

struct Rng {
    uint64_t s = 0xC0FFEE123456789ull;
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint8_t u8() { return static_cast<uint8_t>(next()); }
    uint32_t range(uint32_t lo, uint32_t hi) {
        return lo + static_cast<uint32_t>(next() % (hi - lo + 1));
    }
};

// A decoded packet must be internally consistent with the buffer it came
// from; called for every successful decode of a mutated buffer.
void check_consistency(const Decoded& d, const uint8_t* buf, size_t len) {
    if (const DataView* v = std::get_if<DataView>(&d)) {
        CHECK_EQ_U(len, kDataHeaderSize + v->payload_len);
        CHECK((v->payload_len == 0) == (v->payload == nullptr));
        if (v->payload != nullptr) {
            CHECK(v->payload == buf + kDataHeaderSize);
        }
    } else if (const NackView* v2 = std::get_if<NackView>(&d)) {
        CHECK_EQ_U(len, kNackFixedSize + v2->bitmap_len);
        CHECK((v2->bitmap_len == 0) == (v2->bitmap == nullptr));
    } else if (std::get_if<LinkReport>(&d)) {
        CHECK_EQ_U(len, kLinkReportSize);
    } else if (std::get_if<Heartbeat>(&d)) {
        CHECK_EQ_U(len, kHeartbeatSize);
    } else if (std::get_if<CsaPacket>(&d)) {
        CHECK_EQ_U(len, kCsaSize);
    } else if (std::get_if<RecoveryRequest>(&d)) {
        CHECK_EQ_U(len, kRecoveryRequestSize);
    } else if (std::get_if<JsccFeedback>(&d)) {
        CHECK_EQ_U(len, kJsccFeedbackSize);
    }
}

std::vector<std::vector<uint8_t>> golden_packets() {
    std::vector<std::vector<uint8_t>> out;
    uint8_t buf[2048];

    DataHeader h;
    h.prefix = {17, 0, 0x01020304};
    h.stream_id = 0;
    h.stream_type = stream_type::kRtp;
    h.seq = 90233;
    h.block_id = 4400;
    h.data_flags = data_flags::kEndOfBlock | data_flags::kArq;
    h.active_profile = 4;
    h.table_version = 0xB2;
    const uint8_t payload[64] = {1, 2, 3, 4};
    size_t n = encode_data(h, payload, sizeof(payload), buf, sizeof(buf));
    out.emplace_back(buf, buf + n);

    NackHeader nh;
    nh.prefix = {9, 17, 0xAABBCCDD};
    nh.target_originator = 17;
    nh.target_session = 0x01020304;
    nh.target_stream_id = 0;
    nh.base_seq = 90000;
    const uint8_t bitmap[8] = {0xFF, 0x01};
    n = encode_nack(nh, bitmap, sizeof(bitmap), buf, sizeof(buf));
    out.emplace_back(buf, buf + n);

    LinkReport r;
    r.prefix = {9, 0, 0xAABBCCDD};
    r.target_originator = 17;
    r.target_session = 0x01020304;
    r.target_stream_id = kNodeScopeStreamId;
    r.report_epoch = 1822;
    r.rssi_best = -58;
    r.rssi_mean = -63;
    r.loss_postdiv_prearq = 6;
    r.uniq = 90000;
    r.diversity = 812;
    r.adapters = 3;
    n = encode_link_report(r, buf, sizeof(buf));
    out.emplace_back(buf, buf + n);

    JsccFeedback jf;
    jf.prefix = {9, 0, 0xAABBCCDD};
    jf.target_originator = 17;
    jf.target_session = 0x01020304;
    jf.target_stream_id = 0;
    jf.feedback_epoch = 9;
    jf.repair_demand_permille = 125;
    jf.rtt_p95_us = 2000;
    jf.repair_samples = 30;
    jf.rtt_samples = 24;
    jf.valid_flags = jscc_feedback_flags::kKnownMask;
    jf.observed_block_id = 4400;
    n = encode_jscc_feedback(jf, buf, sizeof(buf));
    out.emplace_back(buf, buf + n);

    Heartbeat hb;
    hb.prefix = {17, 0, 0x01020304};
    n = encode_heartbeat(hb, buf, sizeof(buf));
    out.emplace_back(buf, buf + n);

    CsaPacket c;
    c.prefix = {9, 0, 0xAABBCCDD};
    c.csa_nonce = 7;
    c.csa_seq = 5;
    c.target_chan = 5805;
    c.dt_to_switch_ms = 150;
    c.t_revert_ms = 1000;
    c.prev_chan = 5745;
    c.power_intent = 2;
    c.csa_mac = 0xDEADBEEF;
    n = encode_csa(c, buf, sizeof(buf));
    out.emplace_back(buf, buf + n);

    return out;
}

}  // namespace

int main() {
    Rng rng;

    // 1. Pure random buffers — decode must survive anything.
    {
        uint8_t buf[2048];
        for (int i = 0; i < 200000; ++i) {
            const size_t len = rng.range(0, i % 100 == 0 ? 2048 : 64);
            for (size_t j = 0; j < len; ++j) {
                buf[j] = rng.u8();
            }
            const Decoded d = decode(buf, len);
            check_consistency(d, buf, len);
        }
        CHECK(true);  // reached without crash
    }

    const auto goldens = golden_packets();
    CHECK_EQ_U(goldens.size(), 6);

    // 2. Every truncation of every valid packet must be an error.
    for (const auto& pkt : goldens) {
        for (size_t len = 0; len < pkt.size(); ++len) {
            const Decoded d = decode(pkt.data(), len);
            CHECK(std::get_if<DecodeError>(&d) != nullptr);
        }
        // The full packet must decode.
        const Decoded ok = decode(pkt.data(), pkt.size());
        CHECK(std::get_if<DecodeError>(&ok) == nullptr);
    }

    // 3. Trailing garbage must be rejected (strict-length contract).
    for (const auto& pkt : goldens) {
        std::vector<uint8_t> extended = pkt;
        extended.push_back(0x00);
        const Decoded d = decode(extended.data(), extended.size());
        CHECK(std::get_if<DecodeError>(&d) != nullptr);
    }

    // 4. Single-byte mutations: never crash; survivors stay consistent.
    for (const auto& pkt : goldens) {
        std::vector<uint8_t> m = pkt;
        for (int i = 0; i < 20000; ++i) {
            const size_t pos = rng.range(0, static_cast<uint32_t>(m.size() - 1));
            const uint8_t orig = m[pos];
            m[pos] = rng.u8();
            const Decoded d = decode(m.data(), m.size());
            check_consistency(d, m.data(), m.size());
            m[pos] = orig;
        }
    }

    return wbtest_finish("wire_fuzz_test");
}
