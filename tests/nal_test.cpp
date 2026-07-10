// SPDX-License-Identifier: GPL-2.0-or-later
// §4.1 NAL-type ARQ classifier: single NAL / STAP / AP / FU packetizations
// for H.264 (RFC 6184) and H.265 (RFC 7798), malformed-input hardening, and
// the framer integration (importance stamped from the FIRST packet of the
// block, sticky for the block, size heuristic inert in NAL modes).
#include "wblink/nal.h"

#include <cstring>
#include <vector>

#include "wblink/framer.h"
#include "wblink/rtp.h"
#include "wbtest.h"

using namespace wblink;

namespace {

// ---- payload builders -------------------------------------------------------

uint8_t h264_hdr(uint8_t type) { return static_cast<uint8_t>(0x60 | type); }

// H.265 2-byte NAL header: type in bits 6..1 of the first byte, TID=1.
void h265_hdr(uint8_t type, uint8_t* out) {
    out[0] = static_cast<uint8_t>(type << 1);
    out[1] = 1;
}

std::vector<uint8_t> h264_single(uint8_t type, size_t body = 4) {
    std::vector<uint8_t> p{h264_hdr(type)};
    p.insert(p.end(), body, 0xAA);
    return p;
}

std::vector<uint8_t> h264_stap_a(std::initializer_list<uint8_t> types) {
    std::vector<uint8_t> p{h264_hdr(24)};
    for (uint8_t t : types) {
        p.push_back(0);
        p.push_back(3);  // unit size
        p.push_back(h264_hdr(t));
        p.push_back(0xBB);
        p.push_back(0xCC);
    }
    return p;
}

std::vector<uint8_t> h264_fu_a(uint8_t type, bool start) {
    return {h264_hdr(28),
            static_cast<uint8_t>((start ? 0x80 : 0x00) | type), 0xDD, 0xEE};
}

std::vector<uint8_t> h265_single(uint8_t type, size_t body = 4) {
    std::vector<uint8_t> p(2);
    h265_hdr(type, p.data());
    p.insert(p.end(), body, 0xAA);
    return p;
}

std::vector<uint8_t> h265_ap(std::initializer_list<uint8_t> types) {
    std::vector<uint8_t> p(2);
    h265_hdr(48, p.data());
    for (uint8_t t : types) {
        p.push_back(0);
        p.push_back(4);  // unit size (2B header + 2B body)
        uint8_t hdr[2];
        h265_hdr(t, hdr);
        p.push_back(hdr[0]);
        p.push_back(hdr[1]);
        p.push_back(0xBB);
        p.push_back(0xCC);
    }
    return p;
}

std::vector<uint8_t> h265_fu(uint8_t type, bool start) {
    std::vector<uint8_t> p(2);
    h265_hdr(49, p.data());
    p.push_back(static_cast<uint8_t>((start ? 0x80 : 0x00) | type));
    p.push_back(0xDD);
    return p;
}

// RTP datagram wrapping a payload; marker/ts drive the framer's block logic.
std::vector<uint8_t> rtp_pkt(const std::vector<uint8_t>& payload, bool marker,
                             uint32_t ts, uint16_t seq) {
    std::vector<uint8_t> d(kRtpFixedHeaderSize);
    d[0] = 0x80;  // v2
    d[1] = static_cast<uint8_t>((marker ? 0x80 : 0) | 96);
    d[2] = static_cast<uint8_t>(seq >> 8);
    d[3] = static_cast<uint8_t>(seq & 0xFF);
    d[4] = static_cast<uint8_t>(ts >> 24);
    d[5] = static_cast<uint8_t>(ts >> 16);
    d[6] = static_cast<uint8_t>(ts >> 8);
    d[7] = static_cast<uint8_t>(ts & 0xFF);
    // ssrc = 0
    d.insert(d.end(), payload.begin(), payload.end());
    return d;
}

bool imp264(const std::vector<uint8_t>& p) {
    return h264_payload_important(p.data(), p.size());
}
bool imp265(const std::vector<uint8_t>& p) {
    return h265_payload_important(p.data(), p.size());
}

}  // namespace

int main() {
    // ---- H.264 single NAL ---------------------------------------------------
    CHECK(imp264(h264_single(5)));    // IDR slice
    CHECK(imp264(h264_single(7)));    // SPS
    CHECK(imp264(h264_single(8)));    // PPS
    CHECK(!imp264(h264_single(1)));   // non-IDR slice
    CHECK(!imp264(h264_single(6)));   // SEI is NOT in the §4.1 list
    CHECK(!imp264(h264_single(9)));   // AUD
    CHECK(!imp264(h264_single(12)));  // filler

    // ---- H.264 STAP ---------------------------------------------------------
    CHECK(imp264(h264_stap_a({7, 8})));       // classic SPS+PPS opener
    CHECK(imp264(h264_stap_a({6, 5})));       // important unit not first
    CHECK(!imp264(h264_stap_a({6, 1})));      // nothing important inside
    {
        // STAP-B: 2-byte DON before the units.
        auto stap = h264_stap_a({7});
        stap[0] = h264_hdr(25);
        stap.insert(stap.begin() + 1, {0x00, 0x07});
        CHECK(imp264(stap));
    }

    // ---- H.264 FU -----------------------------------------------------------
    CHECK(imp264(h264_fu_a(5, true)));    // IDR start fragment
    CHECK(imp264(h264_fu_a(5, false)));   // type present in EVERY fragment
    CHECK(!imp264(h264_fu_a(1, true)));   // non-IDR fragment
    {
        // FU-B carries the FU header in the same position.
        auto fu = h264_fu_a(5, true);
        fu[0] = h264_hdr(29);
        CHECK(imp264(fu));
    }

    // ---- H.264 malformed ----------------------------------------------------
    CHECK(!h264_payload_important(nullptr, 0));
    {
        const uint8_t only_indicator[] = {h264_hdr(28)};  // FU with no header
        CHECK(!h264_payload_important(only_indicator, 1));
        const uint8_t zero_type[] = {0x00, 0xFF};  // forbidden type 0
        CHECK(!h264_payload_important(zero_type, 2));
        // STAP-A whose declared unit size overruns the payload.
        const uint8_t overrun[] = {h264_hdr(24), 0xFF, 0xFF, h264_hdr(5)};
        CHECK(!h264_payload_important(overrun, sizeof(overrun)));
        // STAP-A with a zero unit size (would loop forever if unchecked).
        const uint8_t zero_sz[] = {h264_hdr(24), 0x00, 0x00, h264_hdr(5)};
        CHECK(!h264_payload_important(zero_sz, sizeof(zero_sz)));
    }

    // ---- H.265 single NAL ---------------------------------------------------
    CHECK(imp265(h265_single(19)));   // IDR_W_RADL
    CHECK(imp265(h265_single(20)));   // IDR_N_LP
    CHECK(imp265(h265_single(21)));   // CRA_NUT
    CHECK(imp265(h265_single(32)));   // VPS
    CHECK(imp265(h265_single(33)));   // SPS
    CHECK(imp265(h265_single(34)));   // PPS
    CHECK(!imp265(h265_single(1)));   // TRAIL_R
    CHECK(!imp265(h265_single(16)));  // BLA_W_LP: deliberately NOT in §4.1
    CHECK(!imp265(h265_single(39)));  // PREFIX_SEI

    // ---- H.265 AP / FU ------------------------------------------------------
    CHECK(imp265(h265_ap({32, 33, 34})));  // VPS+SPS+PPS opener
    CHECK(imp265(h265_ap({39, 19})));      // important unit not first
    CHECK(!imp265(h265_ap({39, 1})));
    CHECK(imp265(h265_fu(19, true)));
    CHECK(imp265(h265_fu(19, false)));  // any fragment reveals the type
    CHECK(!imp265(h265_fu(1, true)));

    // ---- H.265 malformed ----------------------------------------------------
    CHECK(!h265_payload_important(nullptr, 0));
    {
        const uint8_t one_byte[] = {19 << 1};
        CHECK(!h265_payload_important(one_byte, 1));
        auto fu = h265_fu(19, true);
        CHECK(!h265_payload_important(fu.data(), 2));  // FU w/o FU header
        // AP whose unit size overruns.
        uint8_t ap[6];
        h265_hdr(48, ap);
        ap[2] = 0xFF; ap[3] = 0xFF; ap[4] = 19 << 1; ap[5] = 1;
        CHECK(!h265_payload_important(ap, sizeof(ap)));
    }

    // ---- classification through rtp_payload (CSRC/extension/padding) --------
    {
        auto d = rtp_pkt(h264_single(5), true, 1000, 1);
        d[0] |= 0x10;  // extension flag
        const uint8_t ext[] = {0xBE, 0xDE, 0x00, 0x01, 0, 0, 0, 0};
        d.insert(d.begin() + kRtpFixedHeaderSize, ext, ext + sizeof(ext));
        d[0] |= 0x20;  // padding flag
        d.push_back(0);
        d.push_back(0);
        d.push_back(3);  // 3 bytes of padding
        const auto h = parse_rtp_header(d.data(), d.size());
        CHECK(h.has_value());
        const auto pl = rtp_payload(d.data(), d.size(), *h);
        CHECK(pl.has_value());
        CHECK(pl->len == h264_single(5).size());
        CHECK(h264_payload_important(pl->data, pl->len));

        // Padding larger than the datagram => reject, classify best-effort.
        auto bad = rtp_pkt(h264_single(5), true, 1000, 2);
        bad[0] |= 0x20;
        bad.back() = 0xFF;
        const auto hb = parse_rtp_header(bad.data(), bad.size());
        CHECK(hb.has_value());
        CHECK(!rtp_payload(bad.data(), bad.size(), *hb).has_value());
    }

    // ---- framer integration: stamp from packet 1, sticky, size inert -------
    {
        FramerConfig fc;
        fc.stream_type = stream_type::kRtp;
        fc.classifier = RtpClassifier::kH265;
        fc.classifier_size_threshold = 1;  // would flag EVERYTHING in kSize
        Framer f(fc);

        struct Got {
            uint32_t block;
            bool arq;
            bool eob;
        };
        std::vector<Got> got;
        const auto emit = [&](const uint8_t*, size_t, const DataHeader& h,
                              uint64_t) {
            got.push_back({h.block_id,
                           (h.data_flags & data_flags::kArq) != 0,
                           (h.data_flags & data_flags::kEndOfBlock) != 0});
        };

        // Block 0: IDR frame as 3 FU fragments — ARQ from the FIRST packet.
        uint16_t sq = 0;
        auto p0 = rtp_pkt(h265_fu(19, true), false, 100, sq++);
        auto p1 = rtp_pkt(h265_fu(19, false), false, 100, sq++);
        auto p2 = rtp_pkt(h265_fu(19, false), true, 100, sq++);
        CHECK(f.on_datagram(p0.data(), p0.size(), 0, emit));
        CHECK(f.on_datagram(p1.data(), p1.size(), 0, emit));
        CHECK(f.on_datagram(p2.data(), p2.size(), 0, emit));

        // Block 1: P-frame — never important despite threshold=1 (size inert).
        auto p3 = rtp_pkt(h265_fu(1, true), false, 200, sq++);
        auto p4 = rtp_pkt(h265_fu(1, false), true, 200, sq++);
        CHECK(f.on_datagram(p3.data(), p3.size(), 0, emit));
        CHECK(f.on_datagram(p4.data(), p4.size(), 0, emit));

        // Block 2: parameter sets in an AP, then a trailing slice packet —
        // importance is sticky for the rest of the block.
        auto p5 = rtp_pkt(h265_ap({32, 33, 34}), false, 300, sq++);
        auto p6 = rtp_pkt(h265_single(1), true, 300, sq++);
        CHECK(f.on_datagram(p5.data(), p5.size(), 0, emit));
        CHECK(f.on_datagram(p6.data(), p6.size(), 0, emit));

        CHECK_EQ_U(got.size(), 7u);
        CHECK_EQ_U(got[0].block, 0u);
        CHECK(got[0].arq);  // first packet already stamped
        CHECK(got[1].arq);
        CHECK(got[2].arq);
        CHECK(got[2].eob);
        CHECK_EQ_U(got[3].block, 1u);
        CHECK(!got[3].arq);
        CHECK(!got[4].arq);
        CHECK_EQ_U(got[5].block, 2u);
        CHECK(got[5].arq);
        CHECK(got[6].arq);  // sticky across the AU's non-important tail

        // H.264 mode sanity through the framer.
        FramerConfig fc4;
        fc4.stream_type = stream_type::kRtp;
        fc4.classifier = RtpClassifier::kH264;
        Framer f4(fc4);
        got.clear();
        auto q0 = rtp_pkt(h264_stap_a({7, 8}), false, 400, 0);
        auto q1 = rtp_pkt(h264_fu_a(5, true), true, 400, 1);
        auto q2 = rtp_pkt(h264_single(1), true, 500, 2);
        CHECK(f4.on_datagram(q0.data(), q0.size(), 0, emit));
        CHECK(f4.on_datagram(q1.data(), q1.size(), 0, emit));
        CHECK(f4.on_datagram(q2.data(), q2.size(), 0, emit));
        CHECK(got[0].arq);
        CHECK(got[1].arq);
        CHECK(!got[2].arq);
    }

    return wbtest_finish("nal_test");
}
