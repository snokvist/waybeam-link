// SPDX-License-Identifier: GPL-2.0-or-later
// SpatialRepair (PROTOCOL.md §6.3b): salvage verified source chunks of a
// failed block, decide HEVC slice completeness, synthesize replacements,
// egress a complete access unit. Every refusal returns false = drop.
#include "wblink/spatial_repair.h"

#include <algorithm>
#include <cstring>

#include "wblink/frame_shm_format.h"

namespace wblink {

namespace {

// The donor's short_term_ref_pic_set + long-term block as a canonical
// bit-aligned byte string, so successive P pictures' sets can be compared.
std::vector<uint8_t> rps_bits(const wblink::hevc::SliceInfo& si) {
    std::vector<uint8_t> out;
    uint8_t acc = 0;
    uint32_t n = 0;
    for (uint32_t p = si.strps_bit_begin; p < si.strps_bit_end; ++p) {
        const uint8_t bit = (si.header_rbsp[p >> 3] >> (7 - (p & 7))) & 1;
        acc = static_cast<uint8_t>((acc << 1) | bit);
        if (++n % 8 == 0) {
            out.push_back(acc);
            acc = 0;
        }
    }
    if (n % 8) {
        out.push_back(static_cast<uint8_t>(acc << (8 - n % 8)));
    }
    out.push_back(static_cast<uint8_t>(n));  // disambiguate lengths
    return out;
}

uint32_t rd_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void wr_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

}  // namespace

void SpatialRepair::learn(const uint8_t* blob, size_t len) {
    if (len < kVencFrameMetaSize + 4 || blob[4] != kFrameCodecH265) {
        return;
    }
    std::memcpy(meta_template_, blob, kVencFrameMetaSize);
    have_meta_ = true;
    const uint32_t pts = rd_le32(blob);
    if (last_pts_ != 0 && pts > last_pts_) {
        pts_delta_ = pts - last_pts_;
    }
    last_pts_ = pts;

    const uint8_t* p = blob + kVencFrameMetaSize;
    const size_t n = len - kVencFrameMetaSize;
    std::vector<uint32_t> addrs;
    bool geometry_ok = true;
    bool saw_vcl = false;
    size_t i = 0;
    while (i + 3 <= n) {
        if (!(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)) {
            ++i;
            continue;
        }
        const size_t nal_off = i + 3;
        // NAL extent: to the next start code (minus a 4-byte-code zero).
        size_t j = nal_off;
        while (j + 3 <= n && !(p[j] == 0 && p[j + 1] == 0 && p[j + 2] == 1)) {
            ++j;
        }
        size_t end = (j + 3 <= n) ? j : n;
        if (end > nal_off && end < n && p[end - 1] == 0) {
            --end;  // leading zero of a 4-byte start code
        }
        if (end - nal_off >= 2) {
            const uint8_t t = hevc::nal_type(p + nal_off);
            if (t == hevc::kNalSps) {
                hevc::SpsInfo s;
                if (hevc::parse_sps(p + nal_off, end - nal_off, s)) {
                    if (have_sps_ && (s.pic_size_ctbs != sps_.pic_size_ctbs ||
                                      s.log2_ctb != sps_.log2_ctb)) {
                        geometry_.clear();  // geometry changed with the SPS
                        geometry_pending_.clear();
                        have_donor_ = false;
                    }
                    sps_ = s;
                    have_sps_ = true;
                }
            } else if (t == hevc::kNalPps) {
                hevc::PpsInfo pp;
                if (hevc::parse_pps(p + nal_off, end - nal_off, pp)) {
                    pps_ = pp;
                    have_pps_ = true;
                }
            } else if (hevc::nal_is_vcl(t) && have_sps_ && have_pps_) {
                saw_vcl = true;
                if (hevc::parse_slice_header(p + nal_off, end - nal_off, sps_,
                                             pps_, parsed_slice_)) {
                    addrs.push_back(parsed_slice_.address);
                    if (parsed_slice_.has_poc) {
                        last_poc_ = parsed_slice_.poc_lsb;
                        have_poc_ = true;
                    } else {
                        last_poc_ = 0;  // IDR resets POC
                        have_poc_ = true;
                    }
                    if (parsed_slice_.slice_type == 1 &&
                        !hevc::nal_is_irap(t) && addrs.size() == 1) {
                        std::vector<uint8_t> rps = rps_bits(parsed_slice_);
                        rps_stable_ = have_donor_ && rps == donor_rps_bits_;
                        donor_rps_bits_ = std::move(rps);
                        donor_ = parsed_slice_;  // vector copy; small header
                        have_donor_ = true;
                    }
                } else {
                    geometry_ok = false;  // unparseable stream: fail safe
                }
            }
        }
        i = nal_off;
    }
    if (saw_vcl) {
        if (geometry_ok && !addrs.empty() && addrs.front() == 0 &&
            std::is_sorted(addrs.begin(), addrs.end()) &&
            std::adjacent_find(addrs.begin(), addrs.end()) == addrs.end()) {
            // Adopt only a shape two consecutive delivered pictures agree
            // on: a one-picture geometry (SSC338Q 17-slice GDR refresh AU,
            // addresses a superset of the steady shape) must not become the
            // expectation for the very next picture.
            if (addrs == geometry_pending_ && addrs != geometry_) {
                geometry_ = addrs;
            }
            geometry_pending_ = std::move(addrs);
        } else if (!geometry_ok) {
            geometry_.clear();
            geometry_pending_.clear();
            have_donor_ = false;
        }
    }
}

void SpatialRepair::append_conceal_meta(uint32_t /*poc*/) {
    uint8_t meta[kVencFrameMetaSize];
    std::memcpy(meta, meta_template_, kVencFrameMetaSize);
    wr_le32(meta, last_pts_ + pts_delta_);
    meta[5] &= kFrameFlagGdr;  // never IDR/ENHANCE on a synthesized meta
    rebuilt_.insert(rebuilt_.end(), meta, meta + kVencFrameMetaSize);
}

bool SpatialRepair::freeze(uint32_t total_ctbs, const Emit& emit) {
    if (!cfg_.freeze_frame || !have_donor_ || !have_poc_ || !have_meta_ ||
        geometry_.empty() || !rps_stable_) {
        return fail();
    }
    const uint32_t poc =
        (last_poc_ + 1) & ((1u << sps_.log2_max_poc_lsb) - 1);
    rebuilt_.clear();
    append_conceal_meta(poc);
    for (size_t gi = 0; gi < geometry_.size(); ++gi) {
        const uint32_t addr = geometry_[gi];
        const uint32_t end_addr =
            gi + 1 < geometry_.size() ? geometry_[gi + 1] : total_ctbs;
        if (end_addr <= addr ||
            !hevc::make_conceal_slice(sps_, pps_, donor_, addr,
                                      end_addr - addr, gi == 0,
                                      static_cast<int32_t>(poc),
                                      conceal_scratch_, rebuilt_)) {
            return fail();
        }
    }
    if (rebuilt_.size() > cfg_.max_frame_bytes) {
        return fail();
    }
    last_pts_ += pts_delta_;
    last_poc_ = poc;
    stats_.slices_synthesized += geometry_.size();
    ++stats_.frames_frozen;
    emit(rebuilt_.data(), rebuilt_.size());
    return true;
}

bool SpatialRepair::repair(uint16_t k, uint16_t s, uint32_t frame_len,
                           const std::map<uint16_t, std::vector<uint8_t>>&
                               sources,
                           const Emit& emit) {
    if (!have_sps_ || !have_pps_ || geometry_.empty() || k == 0 || s == 0 ||
        sources.empty()) {
        return fail();
    }
    // Present byte ranges from the received chunk indices.
    uint32_t total = frame_len;
    if (total == 0) {
        const auto& last = *sources.rbegin();
        total = static_cast<uint32_t>(last.first) * s +
                static_cast<uint32_t>(last.second.size());
    }
    if (total <= kVencFrameMetaSize || total > cfg_.max_frame_bytes) {
        return fail();
    }
    present_.assign(total, 0);
    std::vector<uint8_t> assembly(total);  // NOLINT: scratch, reused sizes
    for (const auto& [idx, chunk] : sources) {
        const uint64_t off = static_cast<uint64_t>(idx) * s;
        if (off >= total) {
            continue;
        }
        // total is capped at max_frame_bytes, so these fit size_t on 32-bit.
        const size_t nbytes = static_cast<size_t>(
            std::min<uint64_t>(chunk.size(), total - off));
        std::memcpy(assembly.data() + off, chunk.data(), nbytes);
        std::memset(present_.data() + off, 1, nbytes);
    }
    const auto span_present = [&](uint64_t off, uint64_t nbytes) {
        if (off + nbytes > total) {
            return false;
        }
        for (uint64_t b = off; b < off + nbytes; ++b) {
            if (!present_[static_cast<size_t>(b)]) {
                return false;
            }
        }
        return true;
    };

    // VencFrameMeta (chunk 0 head). IDR frames are never concealed.
    const bool meta_present = span_present(0, kVencFrameMetaSize);
    if (meta_present) {
        if (assembly[4] != kFrameCodecH265 ||
            (assembly[5] & kFrameFlagIdr) != 0) {
            return fail();
        }
    }

    // Start-code scan inside present ranges only.
    found_.clear();
    const uint8_t* p = assembly.data();
    for (uint32_t i = kVencFrameMetaSize; i + 3 <= total; ++i) {
        if (!(present_[i] && present_[i + 1] && present_[i + 2] &&
              p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)) {
            continue;
        }
        FoundNal f;
        f.sc_off = i;
        f.nal_off = i + 3;
        found_.push_back(f);
        i += 2;  // resume after the start code
    }
    if (found_.empty()) {
        return freeze(sps_.pic_size_ctbs, emit);
    }
    // Extents + completeness: complete iff every byte from the start code to
    // the next found start code (or frame end) is present with no erasure.
    for (size_t fi = 0; fi < found_.size(); ++fi) {
        FoundNal& f = found_[fi];
        const uint32_t next_sc =
            fi + 1 < found_.size() ? found_[fi + 1].sc_off : total;
        f.end_off = next_sc;
        f.end_known = true;
        f.complete = span_present(f.sc_off, next_sc - f.sc_off) &&
                     (fi + 1 < found_.size() || frame_len != 0);
        if (f.end_off > f.nal_off && f.end_off < total &&
            p[f.end_off - 1] == 0) {
            --f.end_off;  // leading zero of the next 4-byte start code
        }
        if (f.end_off < f.nal_off + 2) {
            f.complete = false;
            continue;
        }
        const uint8_t t = hevc::nal_type(p + f.nal_off);
        if (hevc::nal_is_irap(t)) {
            return fail();  // never conceal an intra picture
        }
        f.is_vcl = hevc::nal_is_vcl(t);
        if (f.is_vcl && f.complete) {
            f.header_ok = hevc::parse_slice_header(
                p + f.nal_off, f.end_off - f.nal_off, sps_, pps_, f.slice);
            if (!f.header_ok || f.slice.dependent) {
                return fail();  // stream shape outside the learned envelope
            }
        }
    }

    // Map complete survivors onto the learned geometry.
    const FoundNal* by_addr[64] = {nullptr};
    if (geometry_.size() > 64) {
        return fail();
    }
    const hevc::SliceInfo* donor = nullptr;
    bool any_survivor = false;
    for (const FoundNal& f : found_) {
        if (!f.is_vcl || !f.complete || !f.header_ok) {
            continue;
        }
        const auto gi = std::find(geometry_.begin(), geometry_.end(),
                                  f.slice.address);
        if (gi == geometry_.end() ||
            by_addr[gi - geometry_.begin()] != nullptr ||
            f.slice.first_slice != (f.slice.address == 0)) {
            return fail();  // geometry mismatch: fail toward drop
        }
        by_addr[gi - geometry_.begin()] = &f;
        any_survivor = true;
        if (donor == nullptr && f.slice.slice_type == 1) {
            donor = &f.slice;
        }
    }
    if (!any_survivor) {
        return freeze(sps_.pic_size_ctbs, emit);
    }
    if (donor == nullptr) {
        return fail();  // survivors exist but none is a P slice (B: scope)
    }

    // Rebuild: meta, leading complete non-VCL NALs, then the slice sequence.
    rebuilt_.clear();
    if (meta_present) {
        rebuilt_.insert(rebuilt_.end(), assembly.data(),
                        assembly.data() + kVencFrameMetaSize);
    } else {
        if (!have_meta_) {
            return fail();
        }
        append_conceal_meta(donor->poc_lsb);
    }
    const uint32_t first_vcl_sc = [&] {
        for (const FoundNal& f : found_) {
            if (f.is_vcl) {
                return f.sc_off;
            }
        }
        return total;
    }();
    for (const FoundNal& f : found_) {
        if (!f.is_vcl && f.complete && f.sc_off < first_vcl_sc) {
            rebuilt_.insert(rebuilt_.end(), p + f.sc_off, p + f.end_off);
        }
    }
    uint32_t synthesized = 0;
    for (size_t gi = 0; gi < geometry_.size(); ++gi) {
        const uint32_t addr = geometry_[gi];
        const uint32_t end_addr = gi + 1 < geometry_.size()
                                      ? geometry_[gi + 1]
                                      : sps_.pic_size_ctbs;
        if (end_addr <= addr) {
            return fail();
        }
        if (const FoundNal* f = by_addr[gi]) {
            rebuilt_.insert(rebuilt_.end(), p + f->sc_off, p + f->end_off);
        } else {
            if (!hevc::make_conceal_slice(sps_, pps_, *donor, addr,
                                          end_addr - addr, gi == 0, -1,
                                          conceal_scratch_, rebuilt_)) {
                return fail();
            }
            ++synthesized;
        }
    }
    if (rebuilt_.size() > cfg_.max_frame_bytes) {
        return fail();
    }
    stats_.slices_synthesized += synthesized;
    ++stats_.frames_salvaged;
    if (donor->has_poc) {
        last_poc_ = donor->poc_lsb;
        have_poc_ = true;
    }
    emit(rebuilt_.data(), rebuilt_.size());
    return true;
}

void SpatialRepair::reset_stream() {
    have_sps_ = false;
    have_pps_ = false;
    geometry_.clear();
    have_donor_ = false;
    donor_rps_bits_.clear();
    rps_stable_ = false;
    have_poc_ = false;
    have_meta_ = false;
    last_pts_ = 0;
    pts_delta_ = 0;
}

}  // namespace wblink
