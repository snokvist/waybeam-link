// SPDX-License-Identifier: GPL-2.0-or-later
// §6.3b concealment synthesis: parse the embedded HM vector, synthesize a
// replacement slice, and pin it byte-exact against the golden output that
// ffmpeg + libde265 + HM-18.0 decoded clean (tests/hevc_conceal_vector.h).
#include "wblink/hevc_conceal.h"

#include <cstring>
#include <vector>

#include "hevc_conceal_vector.h"
#include "wbtest.h"

using namespace wblink::hevc;

namespace {

struct Nal {
    size_t off;
    size_t end;
};

std::vector<Nal> scan(const unsigned char* d, size_t n) {
    std::vector<Nal> out;
    for (size_t i = 0; i + 3 <= n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            out.push_back({i + 3, 0});
            i += 2;
        }
    }
    for (size_t k = 0; k < out.size(); ++k) {
        size_t end = k + 1 < out.size() ? out[k + 1].off - 3 : n;
        if (end > out[k].off && d[end - 1] == 0) {
            --end;  // 4-byte start code's leading zero
        }
        out[k].end = end;
    }
    return out;
}

}  // namespace

int main() {
    const unsigned char* v = kTinyHevcStream;
    const size_t vn = sizeof(kTinyHevcStream);
    const std::vector<Nal> nals = scan(v, vn);
    CHECK(nals.size() > 10);

    SpsInfo sps{};
    PpsInfo pps{};
    bool have_sps = false;
    bool have_pps = false;
    std::vector<SliceInfo> slices;
    for (const Nal& n : nals) {
        const uint8_t t = nal_type(v + n.off);
        if (t == kNalSps) {
            have_sps = parse_sps(v + n.off, n.end - n.off, sps);
        } else if (t == kNalPps) {
            have_pps = parse_pps(v + n.off, n.end - n.off, pps);
        } else if (nal_is_vcl(t) && have_sps && have_pps) {
            SliceInfo si;
            CHECK(parse_slice_header(v + n.off, n.end - n.off, sps, pps, si));
            slices.push_back(si);
        }
    }

    // vector shape: 192x192, CTU 64 -> 3x3 CTUs, 3 slices at 0/3/6, 8 AUs
    CHECK(have_sps && have_pps);
    CHECK_EQ_U(sps.pic_width, 192);
    CHECK_EQ_U(sps.pic_height, 192);
    CHECK_EQ_U(sps.pic_size_ctbs, 9);
    CHECK(!sps.temporal_mvp_enabled);
    CHECK(sps.sao_enabled);
    CHECK(!pps.tiles_enabled);
    CHECK(!pps.entropy_coding_sync_enabled);
    CHECK_EQ_U(slices.size(), 8 * 3);
    CHECK_EQ_U(slices[0].address, 0);
    CHECK_EQ_U(slices[1].address, 3);
    CHECK_EQ_U(slices[2].address, 6);
    CHECK(nal_is_irap(slices[0].nal_unit_type));   // AU 0 is the IDR
    CHECK(!slices[0].has_poc);
    CHECK(!nal_is_irap(slices[3].nal_unit_type));  // AU 1 on are P
    CHECK_EQ_U(slices[3].slice_type, 1);

    // AU 3 = slices[9..11]; conceal slice 1 with slice 0 as donor and pin
    // against the decoder-validated golden.
    const SliceInfo& donor = slices[9];
    CHECK_EQ_U(donor.poc_lsb, 3);
    ConcealScratch scratch;
    std::vector<uint8_t> out;
    CHECK(make_conceal_slice(sps, pps, donor, /*address=*/3, /*num_ctbs=*/3,
                             /*first_slice=*/false, /*poc_lsb_override=*/-1,
                             scratch, out));
    CHECK_EQ_U(out.size(), sizeof(kGoldenConcealAu3Slice1));
    CHECK(out.size() == sizeof(kGoldenConcealAu3Slice1) &&
          std::memcmp(out.data(), kGoldenConcealAu3Slice1, out.size()) == 0);

    // Determinism + scratch reuse: a second call appends identical bytes.
    std::vector<uint8_t> out2;
    CHECK(make_conceal_slice(sps, pps, donor, 3, 3, false, -1, scratch, out2));
    CHECK(out == out2);

    // First-slice form has no segment address and differs from the golden.
    out2.clear();
    CHECK(make_conceal_slice(sps, pps, donor, 0, 3, true, -1, scratch, out2));
    CHECK(out2 != out);

    // POC override lands in the header (whole-frame freeze path).
    out2.clear();
    CHECK(make_conceal_slice(sps, pps, donor, 3, 3, false, 4, scratch, out2));
    SliceInfo reparsed;
    CHECK(parse_slice_header(out2.data() + 4,
                             out2.size() - 4, sps, pps, reparsed));
    CHECK_EQ_U(reparsed.poc_lsb, 4);
    CHECK_EQ_U(reparsed.address, 3);
    CHECK_EQ_U(reparsed.slice_type, 1);

    // Refusals: IRAP donor, out-of-range span, zero CTUs.
    out2.clear();
    CHECK(!make_conceal_slice(sps, pps, slices[0], 3, 3, false, -1, scratch,
                              out2));
    CHECK(!make_conceal_slice(sps, pps, donor, 7, 3, false, -1, scratch,
                              out2));
    CHECK(!make_conceal_slice(sps, pps, donor, 3, 0, false, -1, scratch,
                              out2));
    CHECK(out2.empty());

    return wbtest_finish("hevc_conceal_test");
}
