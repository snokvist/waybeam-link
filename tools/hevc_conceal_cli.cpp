// SPDX-License-Identifier: GPL-2.0-or-later
// hevc_conceal_cli — decoder-in-the-loop harness for §6.3b concealment.
//
// Reads an Annex-B HEVC elementary stream, replaces the requested slices of
// the requested access unit with synthesized all-skip concealment slices
// (core/src/hevc_conceal.cpp — the exact production code), and writes the
// repaired stream. Feed the output to real decoders (ffmpeg, libde265,
// RK3566 MPP, Android MediaCodec) to validate the synthesis against hardware.
// tools/spatial_conceal/ holds the matching Python prototype + validators.
//
// Usage: hevc_conceal_cli <in.265> <au_index> <slice[,slice...]> <out.265>
//        au_index counts VCL access units from 0; "all" replaces every slice
//        (whole-frame freeze shape, donor from the same AU unless all slices
//        are torched, then the previous AU's slice 0 with POC advanced).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wblink/hevc_conceal.h"

namespace {

using wblink::hevc::SliceInfo;

struct Nal {
    size_t sc = 0;   // start-code offset
    size_t off = 0;  // NAL header offset
    size_t end = 0;
};

std::vector<Nal> scan(const std::vector<uint8_t>& d) {
    std::vector<Nal> out;
    for (size_t i = 0; i + 3 <= d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const size_t sc = (i > 0 && d[i - 1] == 0) ? i - 1 : i;
            out.push_back({sc, i + 3, 0});
            i += 2;
        }
    }
    for (size_t n = 0; n < out.size(); ++n) {
        out[n].end = n + 1 < out.size() ? out[n + 1].sc : d.size();
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s <in.265> <au_index> <slice[,..]|all> <out>\n",
                     argv[0]);
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::perror("open");
        return 1;
    }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        data.insert(data.end(), buf, buf + got);
    }
    std::fclose(f);

    const long want_au = std::strtol(argv[2], nullptr, 10);
    const bool all = std::string(argv[3]) == "all";
    std::vector<int> kill;
    if (!all) {
        for (const char* p = argv[3]; *p;) {
            kill.push_back(static_cast<int>(std::strtol(p, nullptr, 10)));
            while (*p && *p != ',') {
                ++p;
            }
            if (*p == ',') {
                ++p;
            }
        }
    }

    wblink::hevc::SpsInfo sps{};
    wblink::hevc::PpsInfo pps{};
    bool have_sps = false;
    bool have_pps = false;
    const std::vector<Nal> nals = scan(data);

    // Group VCL NALs into AUs by first_slice_segment_in_pic_flag.
    long au = -1;
    std::vector<size_t> au_slices;  // indices into nals for the target AU
    SliceInfo prev_slice0{};
    bool have_prev = false;
    long prev_au_of_slice0 = -1;
    for (size_t n = 0; n < nals.size(); ++n) {
        const uint8_t* p = data.data() + nals[n].off;
        const size_t len = nals[n].end - nals[n].off;
        const uint8_t t = wblink::hevc::nal_type(p);
        if (t == wblink::hevc::kNalSps) {
            have_sps = wblink::hevc::parse_sps(p, len, sps) || have_sps;
        } else if (t == wblink::hevc::kNalPps) {
            have_pps = wblink::hevc::parse_pps(p, len, pps) || have_pps;
        } else if (wblink::hevc::nal_is_vcl(t)) {
            const bool first = (p[2] >> 7) & 1;
            if (first) {
                ++au;
            }
            if (au == want_au) {
                au_slices.push_back(n);
            } else if (au == want_au - 1 && first && have_sps && have_pps) {
                SliceInfo si;
                if (wblink::hevc::parse_slice_header(p, len, sps, pps, si) &&
                    si.slice_type == 1) {
                    prev_slice0 = si;
                    have_prev = true;
                    prev_au_of_slice0 = au;
                }
            }
        }
    }
    (void)prev_au_of_slice0;
    if (!have_sps || !have_pps || au_slices.empty()) {
        std::fprintf(stderr, "AU %ld not found or no SPS/PPS\n", want_au);
        return 1;
    }

    std::vector<SliceInfo> headers(au_slices.size());
    for (size_t i = 0; i < au_slices.size(); ++i) {
        const Nal& nl = nals[au_slices[i]];
        if (!wblink::hevc::parse_slice_header(data.data() + nl.off,
                                              nl.end - nl.off, sps, pps,
                                              headers[i])) {
            std::fprintf(stderr, "slice %zu header parse failed\n", i);
            return 1;
        }
    }
    if (all) {
        for (size_t i = 0; i < au_slices.size(); ++i) {
            kill.push_back(static_cast<int>(i));
        }
    }

    const SliceInfo* donor = nullptr;
    int32_t poc_override = -1;
    for (size_t i = 0; i < headers.size(); ++i) {
        bool killed = false;
        for (const int ki : kill) {
            killed |= (static_cast<size_t>(ki) == i);
        }
        if (!killed) {
            donor = &headers[i];
            break;
        }
    }
    if (donor == nullptr) {  // whole frame: previous AU's slice 0, POC + 1
        if (!have_prev) {
            std::fprintf(stderr, "no donor available\n");
            return 1;
        }
        donor = &prev_slice0;
        poc_override = static_cast<int32_t>(
            (prev_slice0.poc_lsb + 1) & ((1u << sps.log2_max_poc_lsb) - 1));
    }

    // Rebuild the stream with replacements.
    std::vector<uint8_t> out;
    wblink::hevc::ConcealScratch scratch;
    size_t cursor = 0;
    for (size_t i = 0; i < au_slices.size(); ++i) {
        const Nal& nl = nals[au_slices[i]];
        out.insert(out.end(), data.begin() + static_cast<long>(cursor),
                   data.begin() + static_cast<long>(nl.sc));
        cursor = nl.end;
        bool killed = false;
        for (const int ki : kill) {
            killed |= (static_cast<size_t>(ki) == i);
        }
        if (!killed) {
            out.insert(out.end(), data.begin() + static_cast<long>(nl.sc),
                       data.begin() + static_cast<long>(nl.end));
            continue;
        }
        const uint32_t addr = headers[i].address;
        const uint32_t end_addr = i + 1 < headers.size()
                                      ? headers[i + 1].address
                                      : sps.pic_size_ctbs;
        const size_t before = out.size();
        if (!wblink::hevc::make_conceal_slice(sps, pps, *donor, addr,
                                              end_addr - addr, i == 0,
                                              poc_override, scratch, out)) {
            std::fprintf(stderr, "make_conceal_slice failed (slice %zu)\n", i);
            return 1;
        }
        std::fprintf(stderr, "slice %zu: %zu B -> %zu B conceal\n", i,
                     nl.end - nl.sc, out.size() - before);
    }
    out.insert(out.end(), data.begin() + static_cast<long>(cursor),
               data.end());

    FILE* of = std::fopen(argv[4], "wb");
    if (!of) {
        std::perror("open out");
        return 1;
    }
    std::fwrite(out.data(), 1, out.size(), of);
    std::fclose(of);
    return 0;
}
