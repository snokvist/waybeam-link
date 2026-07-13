// SPDX-License-Identifier: GPL-2.0-or-later
// Deterministic-PRNG property test: random field populations for every packet
// type must encode -> decode back to identical fields, including min/max
// boundary values, payload_len 0/1424 and bitmap_len 0/255.
#include <cstring>
#include <variant>

#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

// xorshift64* — deterministic across platforms, no <random> variability.
struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint8_t u8() { return static_cast<uint8_t>(next()); }
    uint16_t u16() { return static_cast<uint16_t>(next()); }
    uint32_t u32() { return static_cast<uint32_t>(next()); }
    // inclusive
    uint32_t range(uint32_t lo, uint32_t hi) {
        return lo + static_cast<uint32_t>(next() % (hi - lo + 1));
    }
};

CommonPrefix random_prefix(Rng& r) {
    return CommonPrefix{r.u16(), r.u16(), r.u32()};
}

constexpr int kIters = 20000;

}  // namespace

int main() {
    Rng rng;
    uint8_t buf[kDataHeaderSize + kMaxDataPayload];
    uint8_t payload[kMaxDataPayload];
    uint8_t bitmap[255];

    for (int i = 0; i < kIters; ++i) {
        // DATA — payload length sweeps boundaries then randoms.
        DataHeader h;
        h.prefix = random_prefix(rng);
        h.stream_id = rng.u8();
        h.stream_type = rng.u8();
        h.seq = rng.u32();
        h.block_id = rng.u32();
        h.data_flags = rng.u8();
        h.active_profile = rng.u8();
        h.table_version = rng.u8();
        uint16_t plen;
        if (i == 0) {
            plen = 0;
        } else if (i == 1) {
            plen = kMaxDataPayload;
        } else {
            plen = static_cast<uint16_t>(rng.range(0, kMaxDataPayload));
        }
        for (uint16_t j = 0; j < plen; ++j) {
            payload[j] = rng.u8();
        }
        const size_t n =
            encode_data(h, plen ? payload : nullptr, plen, buf, sizeof(buf));
        CHECK_EQ_U(n, kDataHeaderSize + plen);
        const Decoded d = decode(buf, n);
        const DataView* v = std::get_if<DataView>(&d);
        CHECK(v != nullptr);
        if (v != nullptr) {
            CHECK(v->hdr == h);
            CHECK_EQ_U(v->payload_len, plen);
            CHECK(plen == 0 || std::memcmp(v->payload, payload, plen) == 0);
        }
    }

    for (int i = 0; i < kIters; ++i) {
        // NACK — bitmap length sweeps boundaries then randoms.
        NackHeader h;
        h.prefix = random_prefix(rng);
        h.target_originator = rng.u16();
        h.target_session = rng.u32();
        h.target_stream_id = rng.u8();
        h.base_seq = rng.u32();
        uint8_t blen;
        if (i == 0) {
            blen = 0;
        } else if (i == 1) {
            blen = 255;
        } else {
            blen = static_cast<uint8_t>(rng.range(0, 255));
        }
        for (uint8_t j = 0; j < blen; ++j) {
            bitmap[j] = rng.u8();
        }
        const size_t n =
            encode_nack(h, blen ? bitmap : nullptr, blen, buf, sizeof(buf));
        CHECK_EQ_U(n, kNackFixedSize + blen);
        const Decoded d = decode(buf, n);
        const NackView* v = std::get_if<NackView>(&d);
        CHECK(v != nullptr);
        if (v != nullptr) {
            CHECK(v->hdr == h);
            CHECK_EQ_U(v->bitmap_len, blen);
            CHECK(blen == 0 || std::memcmp(v->bitmap, bitmap, blen) == 0);
        }
    }

    for (int i = 0; i < kIters; ++i) {
        LinkReport r;
        r.prefix = random_prefix(rng);
        r.target_originator = rng.u16();
        r.target_session = rng.u32();
        r.target_stream_id = rng.u8();
        r.report_epoch = rng.u32();
        r.table_version = rng.u8();
        r.rssi_best = static_cast<int8_t>(rng.u8());
        r.rssi_mean = static_cast<int8_t>(rng.u8());
        r.loss_postdiv_prearq = rng.u16();
        r.uniq = rng.u32();
        r.diversity = rng.u32();
        r.adapters = rng.u8();
        r.probe_per = rng.u16();
        r.recommended_prof = rng.u8();
        const size_t n = encode_link_report(r, buf, sizeof(buf));
        CHECK_EQ_U(n, kLinkReportSize);
        const Decoded d = decode(buf, n);
        const LinkReport* v = std::get_if<LinkReport>(&d);
        CHECK(v != nullptr);
        if (v != nullptr) {
            CHECK(*v == r);
        }
    }

    for (int i = 0; i < kIters; ++i) {
        RecoveryRequest r;
        r.prefix = random_prefix(rng);
        r.target_originator = rng.u16();
        r.target_session = rng.u32();
        r.target_stream_id = rng.u8();
        const size_t n = encode_recovery_request(r, buf, sizeof(buf));
        CHECK_EQ_U(n, kRecoveryRequestSize);
        const Decoded d = decode(buf, n);
        const RecoveryRequest* v = std::get_if<RecoveryRequest>(&d);
        CHECK(v != nullptr);
        if (v != nullptr) CHECK(*v == r);
    }

    for (int i = 0; i < kIters; ++i) {
        JsccFeedback f;
        f.prefix = random_prefix(rng);
        f.target_originator = rng.u16();
        f.target_session = rng.u32();
        f.target_stream_id = rng.u8();
        f.feedback_epoch = rng.u32();
        f.repair_demand_permille = rng.u16();
        f.rtt_p95_us = rng.u32();
        f.repair_samples = rng.u16();
        f.rtt_samples = rng.u16();
        f.valid_flags = rng.u8() & jscc_feedback_flags::kKnownMask;
        f.observed_block_id = rng.u32();
        const size_t n = encode_jscc_feedback(f, buf, sizeof(buf));
        CHECK_EQ_U(n, kJsccFeedbackSize);
        const Decoded d = decode(buf, n);
        const JsccFeedback* v = std::get_if<JsccFeedback>(&d);
        CHECK(v != nullptr);
        if (v != nullptr) CHECK(*v == f);
    }

    for (int i = 0; i < kIters; ++i) {
        Heartbeat hb;
        hb.prefix = random_prefix(rng);
        const size_t n = encode_heartbeat(hb, buf, sizeof(buf));
        CHECK_EQ_U(n, kHeartbeatSize);
        const Decoded d = decode(buf, n);
        const Heartbeat* v = std::get_if<Heartbeat>(&d);
        CHECK(v != nullptr);
        if (v != nullptr) {
            CHECK(*v == hb);
        }
    }

    for (int i = 0; i < kIters; ++i) {
        CsaPacket c;
        c.prefix = random_prefix(rng);
        c.csa_nonce = rng.u32();
        c.csa_seq = rng.u8();
        c.target_chan = rng.u16();
        c.target_bw = rng.u8();
        c.retune_class = rng.u8();
        c.dt_to_switch_ms = rng.u16();
        c.t_revert_ms = rng.u16();
        c.prev_chan = rng.u16();
        c.prev_bw = rng.u8();
        c.power_intent = rng.u8();
        c.csa_mac = rng.u32();
        const size_t n = encode_csa(c, buf, sizeof(buf));
        CHECK_EQ_U(n, kCsaSize);
        const Decoded d = decode(buf, n);
        const CsaPacket* v = std::get_if<CsaPacket>(&d);
        CHECK(v != nullptr);
        if (v != nullptr) {
            CHECK(*v == c);
        }
    }

    return wbtest_finish("wire_roundtrip_test");
}
