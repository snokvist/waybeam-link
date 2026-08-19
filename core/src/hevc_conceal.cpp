// SPDX-License-Identifier: GPL-2.0-or-later
// HEVC concealment-slice synthesis (PROTOCOL.md §6.3b). Bit-exact against the
// offline Python prototype in tools/spatial_conceal/ (whose output was
// validated by ffmpeg + libde265 decode; docs/findings.md 2026-08-19).
#include "wblink/hevc_conceal.h"

#include <algorithm>
#include <cstring>

namespace wblink::hevc {

namespace {

// ------------------------------------------------------------- bit reader
// Sticky-fail reader over unescaped RBSP. Overrun => fail(), parse aborts.
class BitReader {
  public:
    BitReader(const uint8_t* d, size_t n)
        : d_(d), bits_(static_cast<uint32_t>(n) * 8) {}
    bool failed() const { return fail_; }
    uint32_t pos() const { return pos_; }

    uint32_t u(uint32_t n) {
        uint32_t v = 0;
        while (n--) {
            if (pos_ >= bits_) {
                fail_ = true;
                return 0;
            }
            v = (v << 1) | ((d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1);
            ++pos_;
        }
        return v;
    }

    uint32_t ue() {
        uint32_t zeros = 0;
        while (!fail_ && u(1) == 0) {
            if (++zeros > 31) {
                fail_ = true;
                return 0;
            }
        }
        if (fail_) {
            return 0;
        }
        return (1u << zeros) - 1 + (zeros ? u(zeros) : 0);
    }

    int32_t se() {
        const uint32_t k = ue();
        return (k & 1) ? static_cast<int32_t>((k + 1) >> 1)
                       : -static_cast<int32_t>(k >> 1);
    }

  private:
    const uint8_t* d_;
    uint32_t bits_;
    uint32_t pos_ = 0;
    bool fail_ = false;
};

// EBSP -> RBSP into out (bounded by cap bytes of output).
void unescape(const uint8_t* p, size_t n, size_t cap,
              std::vector<uint8_t>& out) {
    out.clear();
    size_t zeros = 0;
    for (size_t i = 0; i < n && out.size() < cap; ++i) {
        const uint8_t c = p[i];
        if (zeros >= 2 && c == 3) {
            zeros = 0;
            continue;  // emulation prevention byte
        }
        out.push_back(c);
        zeros = (c == 0) ? zeros + 1 : 0;
    }
}

// short_term_ref_pic_set (§7.3.7). idx == num_sets only in a slice header.
// Returns false on malformed syntax; fills NumDeltaPocs / used count.
bool parse_st_rps(BitReader& r, uint32_t idx, uint32_t num_sets,
                  const std::vector<uint16_t>& num_delta_list,
                  uint16_t& num_delta, uint16_t& num_used) {
    bool inter_pred = false;
    if (idx != 0) {
        inter_pred = r.u(1) != 0;
    }
    if (inter_pred) {
        uint32_t ref_idx = idx - 1;
        if (idx == num_sets) {  // slice-header case: delta_idx_minus1
            const uint32_t d = r.ue();
            if (d >= idx) {
                return false;
            }
            ref_idx = idx - 1 - d;
        }
        if (ref_idx >= num_delta_list.size()) {
            return false;
        }
        r.u(1);   // delta_rps_sign
        r.ue();   // abs_delta_rps_minus1
        uint16_t nd = 0;
        uint16_t nu = 0;
        for (uint32_t j = 0; j <= num_delta_list[ref_idx]; ++j) {
            const uint32_t used = r.u(1);
            uint32_t use_delta = 1;
            if (!used) {
                use_delta = r.u(1);
            }
            if (used || use_delta) {
                ++nd;
            }
            nu = static_cast<uint16_t>(nu + used);
        }
        num_delta = nd;
        num_used = nu;
    } else {
        const uint32_t num_neg = r.ue();
        const uint32_t num_pos = r.ue();
        if (num_neg + num_pos > 16) {
            return false;
        }
        uint16_t nu = 0;
        for (uint32_t j = 0; j < num_neg + num_pos; ++j) {
            r.ue();  // delta_poc_minus1
            nu = static_cast<uint16_t>(nu + r.u(1));
        }
        num_delta = static_cast<uint16_t>(num_neg + num_pos);
        num_used = nu;
    }
    return !r.failed();
}

void skip_scaling_list(BitReader& r) {
    for (int size_id = 0; size_id < 4; ++size_id) {
        for (int m = 0; m < (size_id == 3 ? 2 : 6); ++m) {
            if (!r.u(1)) {         // scaling_list_pred_mode_flag
                r.ue();            // pred_matrix_id_delta
            } else {
                const int coefs = std::min(64, 1 << (4 + (size_id << 1)));
                if (size_id > 1) {
                    r.se();        // dc coef
                }
                for (int i = 0; i < coefs; ++i) {
                    r.se();
                }
            }
        }
    }
}

void skip_profile_tier_level(BitReader& r, uint32_t max_sub_layers_minus1) {
    r.u(8);   // profile_space/tier/idc
    r.u(32);  // compatibility
    r.u(4);   // progressive/interlaced/non_packed/frame_only
    r.u(32);  // reserved 43 bits + 1 (split for 32-bit reads)
    r.u(12);
    r.u(8);   // level_idc
    if (max_sub_layers_minus1 > 8) {
        return;  // reader will have failed on garbage anyway
    }
    uint32_t sub_profile = 0;
    uint32_t sub_level = 0;
    for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        sub_profile |= r.u(1) << i;
        sub_level |= r.u(1) << i;
    }
    if (max_sub_layers_minus1 > 0) {
        for (uint32_t i = max_sub_layers_minus1; i < 8; ++i) {
            r.u(2);
        }
    }
    for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        if (sub_profile & (1u << i)) {
            r.u(32);
            r.u(32);
            r.u(24);
        }
        if (sub_level & (1u << i)) {
            r.u(8);
        }
    }
}

// ------------------------------------------------------------- bit writer
class BitWriter {
  public:
    explicit BitWriter(std::vector<uint8_t>& bits) : bits_(bits) {
        bits_.clear();
    }
    void bit(uint8_t b) { bits_.push_back(b); }
    void u(uint32_t v, uint32_t n) {
        while (n--) {
            bits_.push_back((v >> n) & 1);
        }
    }
    void ue(uint32_t v) {
        ++v;
        uint32_t n = 0;
        while ((v >> n) > 1) {
            ++n;
        }
        u(0, n);
        u(v, n + 1);
    }
    void se(int32_t v) {
        ue(v > 0 ? static_cast<uint32_t>(2 * v - 1)
                 : static_cast<uint32_t>(-2 * v));
    }
    void copy_bits(const std::vector<uint8_t>& rbsp, uint32_t begin,
                   uint32_t end) {
        for (uint32_t p = begin; p < end; ++p) {
            bits_.push_back((rbsp[p >> 3] >> (7 - (p & 7))) & 1);
        }
    }
    // slice-header byte_alignment(): one 1 bit then 0s.
    void align_header() {
        bits_.push_back(1);
        while (bits_.size() % 8) {
            bits_.push_back(0);
        }
    }
    void align_zero() {
        while (bits_.size() % 8) {
            bits_.push_back(0);
        }
    }
    size_t size() const { return bits_.size(); }

  private:
    std::vector<uint8_t>& bits_;
};

// ---------------------------------------------------------------- CABAC
// Tables 9-46/9-47 + context init (§9.3.2.2). Values cross-checked against
// libde265 1.0.15 (cabac.cc / contextmodel.cc).
constexpr uint8_t kLpsTable[64][4] = {
    {128, 176, 208, 240}, {128, 167, 197, 227}, {128, 158, 187, 216},
    {123, 150, 178, 205}, {116, 142, 169, 195}, {111, 135, 160, 185},
    {105, 128, 152, 175}, {100, 122, 144, 166}, {95, 116, 137, 158},
    {90, 110, 130, 150},  {85, 104, 123, 142},  {81, 99, 117, 135},
    {77, 94, 111, 128},   {73, 89, 105, 122},   {69, 85, 100, 116},
    {66, 80, 95, 110},    {62, 76, 90, 104},    {59, 72, 86, 99},
    {56, 69, 81, 94},     {53, 65, 77, 89},     {51, 62, 73, 85},
    {48, 59, 69, 80},     {46, 56, 66, 76},     {43, 53, 63, 72},
    {41, 50, 59, 69},     {39, 48, 56, 65},     {37, 45, 54, 62},
    {35, 43, 51, 59},     {33, 41, 48, 56},     {32, 39, 46, 53},
    {30, 37, 43, 50},     {29, 35, 41, 48},     {27, 33, 39, 45},
    {26, 31, 37, 43},     {24, 30, 35, 41},     {23, 28, 33, 39},
    {22, 27, 32, 37},     {21, 26, 30, 35},     {20, 24, 29, 33},
    {19, 23, 27, 31},     {18, 22, 26, 30},     {17, 21, 25, 28},
    {16, 20, 23, 27},     {15, 19, 22, 25},     {14, 18, 21, 24},
    {14, 17, 20, 23},     {13, 16, 19, 22},     {12, 15, 18, 21},
    {12, 14, 17, 20},     {11, 14, 16, 19},     {11, 13, 15, 18},
    {10, 12, 15, 17},     {10, 12, 14, 16},     {9, 11, 13, 15},
    {9, 11, 12, 14},      {8, 10, 12, 14},      {8, 9, 11, 13},
    {7, 9, 11, 12},       {7, 9, 10, 12},       {7, 8, 10, 11},
    {6, 8, 9, 11},        {6, 7, 9, 10},        {6, 7, 8, 9},
    {2, 2, 2, 2}};

constexpr uint8_t kNextStateMps[64] = {
    1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 62, 63};

constexpr uint8_t kNextStateLps[64] = {
    0,  0,  1,  2,  2,  4,  4,  5,  6,  7,  8,  9,  9,  11, 11, 12,
    13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
    24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
    33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63};

// init values, P slice initType 1 (cabac_init_flag = 0): only the syntax
// elements an all-skip slice codes.
constexpr uint8_t kInitSplitCu[3] = {107, 139, 126};
constexpr uint8_t kInitCuSkip[3] = {197, 185, 201};

struct Ctx {
    uint8_t state = 0;
    uint8_t mps = 0;
};

Ctx ctx_init(uint8_t init_value, int32_t qp) {
    qp = std::clamp<int32_t>(qp, 0, 51);
    const int32_t slope = (init_value >> 4) * 5 - 45;
    const int32_t offset = ((init_value & 15) << 3) - 16;
    const int32_t pre =
        std::clamp<int32_t>(((slope * qp) >> 4) + offset, 1, 126);
    Ctx c;
    if (pre <= 63) {
        c.state = static_cast<uint8_t>(63 - pre);
        c.mps = 0;
    } else {
        c.state = static_cast<uint8_t>(pre - 64);
        c.mps = 1;
    }
    return c;
}

struct CtxSet {
    Ctx split[3];
    Ctx skip[3];
};

CtxSet fresh_contexts(int32_t slice_qp) {
    CtxSet s;
    for (int i = 0; i < 3; ++i) {
        s.split[i] = ctx_init(kInitSplitCu[i], slice_qp);
        s.skip[i] = ctx_init(kInitCuSkip[i], slice_qp);
    }
    return s;
}

// §9.3.4 arithmetic encoder (put-bit form with bits-outstanding).
class CabacEnc {
  public:
    explicit CabacEnc(BitWriter& bw) : bw_(bw) { reset(); }

    void reset() {
        low_ = 0;
        range_ = 510;
        outstanding_ = 0;
        first_ = true;
    }

    void encode_bin(Ctx& c, uint8_t bin) {
        const uint32_t lps = kLpsTable[c.state][(range_ >> 6) & 3];
        range_ -= lps;
        if (bin != c.mps) {
            low_ += range_;
            range_ = lps;
            if (c.state == 0) {
                c.mps = 1 - c.mps;
            }
            c.state = kNextStateLps[c.state];
        } else {
            c.state = kNextStateMps[c.state];
        }
        renorm();
    }

    // end_of_slice_segment_flag / end_of_subset_one_bit (§9.3.4.3.4-5).
    void encode_terminate(uint8_t bin) {
        range_ -= 2;
        if (bin) {
            low_ += range_;
            range_ = 2;
            renorm();
            put_bit((low_ >> 9) & 1);
            const uint32_t v = ((low_ >> 7) & 3) | 1;  // rbsp stop bit rides here
            bw_.bit((v >> 1) & 1);
            bw_.bit(v & 1);
        } else {
            renorm();
        }
    }

  private:
    void put_bit(uint32_t b) {
        if (first_) {
            first_ = false;
        } else {
            bw_.bit(static_cast<uint8_t>(b));
        }
        while (outstanding_ > 0) {
            bw_.bit(static_cast<uint8_t>(1 - b));
            --outstanding_;
        }
    }

    void renorm() {
        while (range_ < 256) {
            if (low_ < 256) {
                put_bit(0);
            } else if (low_ >= 512) {
                low_ -= 512;
                put_bit(1);
            } else {
                low_ -= 256;
                ++outstanding_;
            }
            range_ <<= 1;
            low_ <<= 1;
        }
    }

    BitWriter& bw_;
    uint32_t low_ = 0;
    uint32_t range_ = 510;
    uint32_t outstanding_ = 0;
    bool first_ = true;
};

// RBSP -> EBSP with 4-byte start code, appended to out.
void escape_append(const std::vector<uint8_t>& rbsp, const uint8_t nal_hdr[2],
                   std::vector<uint8_t>& out) {
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    out.push_back(nal_hdr[0]);
    out.push_back(nal_hdr[1]);
    size_t zeros = (nal_hdr[1] == 0) ? (nal_hdr[0] == 0 ? 2 : 1) : 0;
    for (const uint8_t c : rbsp) {
        if (zeros >= 2 && c <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(c);
        zeros = (c == 0) ? zeros + 1 : 0;
    }
}

void pack_bits(const std::vector<uint8_t>& bits, std::vector<uint8_t>& bytes) {
    bytes.clear();
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t b = 0;
        for (size_t j = 0; j < 8; ++j) {
            b = static_cast<uint8_t>((b << 1) | bits[i + j]);
        }
        bytes.push_back(b);
    }
}

}  // namespace

// ------------------------------------------------------------------ SPS

bool parse_sps(const uint8_t* nal, size_t len, SpsInfo& out) {
    if (len < 4 || nal_type(nal) != kNalSps) {
        return false;
    }
    std::vector<uint8_t> rbsp;
    unescape(nal + 2, len - 2, 512, rbsp);
    BitReader r(rbsp.data(), rbsp.size());
    r.u(4);  // vps id
    const uint32_t max_sub_layers_minus1 = r.u(3);
    r.u(1);  // temporal_id_nesting
    skip_profile_tier_level(r, max_sub_layers_minus1);
    out = SpsInfo{};
    out.sps_id = static_cast<uint8_t>(r.ue());
    out.chroma_format_idc = static_cast<uint8_t>(r.ue());
    if (out.chroma_format_idc == 3 && r.u(1)) {
        return false;  // separate colour planes unsupported
    }
    out.pic_width = r.ue();
    out.pic_height = r.ue();
    if (r.u(1)) {  // conformance window
        r.ue();
        r.ue();
        r.ue();
        r.ue();
    }
    r.ue();  // bit_depth_luma_minus8
    r.ue();  // bit_depth_chroma_minus8
    out.log2_max_poc_lsb = static_cast<uint8_t>(r.ue() + 4);
    const bool sub_layer_ordering = r.u(1) != 0;
    for (uint32_t i = sub_layer_ordering ? 0 : max_sub_layers_minus1;
         i <= max_sub_layers_minus1; ++i) {
        r.ue();
        r.ue();
        r.ue();
    }
    out.log2_min_cb = static_cast<uint8_t>(r.ue() + 3);
    out.log2_ctb = static_cast<uint8_t>(out.log2_min_cb + r.ue());
    r.ue();  // log2_min_tb
    r.ue();  // log2_diff_max_min_tb
    r.ue();  // max_transform_hierarchy_depth_inter
    r.ue();  // max_transform_hierarchy_depth_intra
    if (r.u(1) && r.u(1)) {  // scaling_list_enabled && data_present
        skip_scaling_list(r);
    }
    r.u(1);  // amp_enabled
    out.sao_enabled = r.u(1) != 0;
    if (r.u(1)) {  // pcm_enabled
        r.u(8);
        r.ue();
        r.ue();
        r.u(1);
    }
    const uint32_t num_st = r.ue();
    if (num_st > 64) {
        return false;
    }
    out.num_st_rps = static_cast<uint8_t>(num_st);
    out.st_rps_num_delta.reserve(num_st);
    out.st_rps_num_used.reserve(num_st);
    for (uint32_t i = 0; i < num_st; ++i) {
        uint16_t nd = 0;
        uint16_t nu = 0;
        if (!parse_st_rps(r, i, num_st, out.st_rps_num_delta, nd, nu)) {
            return false;
        }
        out.st_rps_num_delta.push_back(nd);
        out.st_rps_num_used.push_back(nu);
    }
    out.long_term_ref_pics_present = r.u(1) != 0;
    if (out.long_term_ref_pics_present) {
        const uint32_t n = r.ue();
        if (n > 32) {
            return false;
        }
        out.num_long_term_ref_pics_sps = static_cast<uint8_t>(n);
        for (uint32_t i = 0; i < n; ++i) {
            r.u(out.log2_max_poc_lsb);
            r.u(1);
        }
    }
    out.temporal_mvp_enabled = r.u(1) != 0;
    r.u(1);  // strong_intra_smoothing (VUI + extensions not needed)
    if (r.failed()) {
        return false;
    }
    if (out.pic_width == 0 || out.pic_height == 0 ||
        out.pic_width > 8192 || out.pic_height > 8192 ||
        out.log2_ctb < 4 || out.log2_ctb > 6 ||
        out.log2_max_poc_lsb < 4 || out.log2_max_poc_lsb > 16) {
        return false;
    }
    const uint32_t ctb = 1u << out.log2_ctb;
    out.pic_width_ctbs = (out.pic_width + ctb - 1) >> out.log2_ctb;
    out.pic_height_ctbs = (out.pic_height + ctb - 1) >> out.log2_ctb;
    out.pic_size_ctbs = out.pic_width_ctbs * out.pic_height_ctbs;
    uint8_t v = 0;
    while ((1u << v) < out.pic_size_ctbs) {
        ++v;
    }
    out.addr_bits = v;
    return true;
}

// ------------------------------------------------------------------ PPS

bool parse_pps(const uint8_t* nal, size_t len, PpsInfo& out) {
    if (len < 3 || nal_type(nal) != kNalPps) {
        return false;
    }
    std::vector<uint8_t> rbsp;
    unescape(nal + 2, len - 2, 256, rbsp);
    BitReader r(rbsp.data(), rbsp.size());
    out = PpsInfo{};
    const uint32_t pps_id = r.ue();
    const uint32_t sps_id = r.ue();
    if (pps_id > 63 || sps_id > 15) {
        return false;
    }
    out.pps_id = static_cast<uint8_t>(pps_id);
    out.sps_id = static_cast<uint8_t>(sps_id);
    out.dependent_slice_segments_enabled = r.u(1) != 0;
    out.output_flag_present = r.u(1) != 0;
    out.num_extra_slice_header_bits = static_cast<uint8_t>(r.u(3));
    r.u(1);  // sign_data_hiding
    out.cabac_init_present = r.u(1) != 0;
    out.num_ref_idx_l0_default = static_cast<uint16_t>(r.ue() + 1);
    out.num_ref_idx_l1_default = static_cast<uint16_t>(r.ue() + 1);
    out.init_qp = static_cast<int8_t>(r.se() + 26);
    r.u(1);  // constrained_intra_pred
    r.u(1);  // transform_skip
    out.cu_qp_delta_enabled = r.u(1) != 0;
    if (out.cu_qp_delta_enabled) {
        r.ue();  // diff_cu_qp_delta_depth
    }
    r.se();  // pps_cb_qp_offset
    r.se();  // pps_cr_qp_offset
    out.slice_chroma_qp_offsets_present = r.u(1) != 0;
    out.weighted_pred = r.u(1) != 0;
    out.weighted_bipred = r.u(1) != 0;
    out.transquant_bypass_enabled = r.u(1) != 0;
    out.tiles_enabled = r.u(1) != 0;
    out.entropy_coding_sync_enabled = r.u(1) != 0;
    if (out.tiles_enabled) {
        return false;  // §6.3b scope guard: no tiled streams in the fleet
    }
    out.loop_filter_across_slices = r.u(1) != 0;
    out.deblocking_filter_control_present = r.u(1) != 0;
    if (out.deblocking_filter_control_present) {
        out.deblocking_filter_override_enabled = r.u(1) != 0;
        out.pps_deblocking_filter_disabled = r.u(1) != 0;
        if (!out.pps_deblocking_filter_disabled) {
            r.se();
            r.se();
        }
    }
    if (r.u(1)) {  // pps_scaling_list_data_present
        skip_scaling_list(r);
    }
    out.lists_modification_present = r.u(1) != 0;
    r.ue();  // log2_parallel_merge_level_minus2
    out.slice_segment_header_extension_present = r.u(1) != 0;
    return !r.failed();
}

// ---------------------------------------------------------- slice header

bool parse_slice_header(const uint8_t* nal, size_t len, const SpsInfo& sps,
                        const PpsInfo& pps, SliceInfo& out) {
    if (len < 3) {
        return false;
    }
    out = SliceInfo{};
    out.nal_header[0] = nal[0];
    out.nal_header[1] = nal[1];
    out.nal_unit_type = nal_type(nal);
    if (!nal_is_vcl(out.nal_unit_type)) {
        return false;
    }
    // Headers are small; 256 unescaped bytes covers any supported shape.
    unescape(nal + 2, len - 2, 256, out.header_rbsp);
    BitReader r(out.header_rbsp.data(), out.header_rbsp.size());
    out.first_slice = r.u(1) != 0;
    if (nal_is_irap(out.nal_unit_type)) {
        r.u(1);  // no_output_of_prior_pics
    }
    const uint32_t pps_id = r.ue();
    if (pps_id != pps.pps_id) {
        return false;  // caller resolved the wrong PPS
    }
    out.pps_id = static_cast<uint8_t>(pps_id);
    if (!out.first_slice) {
        if (pps.dependent_slice_segments_enabled) {
            out.dependent = r.u(1) != 0;
        }
        out.address = r.u(sps.addr_bits);
    }
    if (out.dependent) {
        return false;  // §6.3b scope guard
    }
    for (uint32_t i = 0; i < pps.num_extra_slice_header_bits; ++i) {
        r.u(1);
    }
    out.slice_type = static_cast<uint8_t>(r.ue());
    if (out.slice_type > 2) {
        return false;
    }
    if (pps.output_flag_present) {
        r.u(1);
    }
    uint16_t st_used = 0;
    uint16_t num_lt = 0;
    if (!nal_is_idr(out.nal_unit_type)) {
        out.poc_lsb = r.u(sps.log2_max_poc_lsb);
        out.has_poc = true;
        out.strps_bit_begin = r.pos();
        if (!r.u(1)) {  // short_term_ref_pic_set_sps_flag == 0: explicit set
            uint16_t nd = 0;
            if (!parse_st_rps(r, sps.num_st_rps, sps.num_st_rps,
                              sps.st_rps_num_delta, nd, st_used)) {
                return false;
            }
        } else {
            uint32_t idx = 0;
            if (sps.num_st_rps > 1) {
                uint8_t bits = 0;
                while ((1u << bits) < sps.num_st_rps) {
                    ++bits;
                }
                idx = r.u(bits);
            }
            if (idx >= sps.st_rps_num_used.size()) {
                return false;
            }
            st_used = sps.st_rps_num_used[idx];
        }
        if (sps.long_term_ref_pics_present) {
            uint32_t num_lt_sps = 0;
            if (sps.num_long_term_ref_pics_sps > 0) {
                num_lt_sps = r.ue();
            }
            const uint32_t num_lt_pics = r.ue();
            if (num_lt_sps + num_lt_pics > 32) {
                return false;
            }
            for (uint32_t i = 0; i < num_lt_sps + num_lt_pics; ++i) {
                if (i < num_lt_sps) {
                    if (sps.num_long_term_ref_pics_sps > 1) {
                        uint8_t bits = 0;
                        while ((1u << bits) <
                               static_cast<uint32_t>(
                                   sps.num_long_term_ref_pics_sps)) {
                            ++bits;
                        }
                        r.u(bits);
                    }
                } else {
                    r.u(sps.log2_max_poc_lsb);
                    r.u(1);
                }
                if (r.u(1)) {  // delta_poc_msb_present
                    r.ue();
                }
            }
            num_lt = static_cast<uint16_t>(num_lt_sps + num_lt_pics);
        }
        out.strps_bit_end = r.pos();
        if (sps.temporal_mvp_enabled) {
            out.temporal_mvp = r.u(1) != 0;
        }
    }
    out.num_used_refs = static_cast<uint16_t>(st_used + num_lt);
    if (sps.sao_enabled) {
        r.u(1);
        r.u(1);
    }
    uint16_t num_ref_l0 = pps.num_ref_idx_l0_default;
    uint16_t num_ref_l1 = pps.num_ref_idx_l1_default;
    if (out.slice_type == 0 || out.slice_type == 1) {  // B or P
        if (r.u(1)) {  // num_ref_idx_active_override
            num_ref_l0 = static_cast<uint16_t>(r.ue() + 1);
            if (out.slice_type == 0) {
                num_ref_l1 = static_cast<uint16_t>(r.ue() + 1);
            }
        }
        if (num_ref_l0 > 16 || num_ref_l1 > 16) {
            return false;
        }
        if (pps.lists_modification_present && out.num_used_refs > 1) {
            uint8_t bits = 0;
            while ((1u << bits) < out.num_used_refs) {
                ++bits;
            }
            if (r.u(1)) {
                for (uint32_t i = 0; i < num_ref_l0; ++i) {
                    r.u(bits);
                }
            }
            if (out.slice_type == 0 && r.u(1)) {
                for (uint32_t i = 0; i < num_ref_l1; ++i) {
                    r.u(bits);
                }
            }
        }
        if (out.slice_type == 0) {
            r.u(1);  // mvd_l1_zero
        }
        if (pps.cabac_init_present) {
            r.u(1);
        }
        if (out.temporal_mvp) {
            bool collocated_from_l0 = true;
            if (out.slice_type == 0) {
                collocated_from_l0 = r.u(1) != 0;
            }
            if ((collocated_from_l0 && num_ref_l0 > 1) ||
                (!collocated_from_l0 && num_ref_l1 > 1)) {
                r.ue();  // collocated_ref_idx
            }
        }
        if ((pps.weighted_pred && out.slice_type == 1) ||
            (pps.weighted_bipred && out.slice_type == 0)) {
            return false;  // §6.3b scope guard: no weighted prediction
        }
        r.ue();  // five_minus_max_num_merge_cand
    }
    out.qp_delta = r.se();
    return !r.failed();
}

// -------------------------------------------------------- synthesis

namespace {

// Emit header; entry_points holds the escaped byte length of every WPP
// substream except the last (empty when not WPP / single row).
void write_conceal_header(BitWriter& bw, const SpsInfo& sps,
                          const PpsInfo& pps, const SliceInfo& donor,
                          uint32_t address, bool first_slice,
                          int32_t poc_lsb_override,
                          const std::vector<uint32_t>& entry_points) {
    bw.u(first_slice ? 1 : 0, 1);
    bw.ue(donor.pps_id);
    if (!first_slice) {
        if (pps.dependent_slice_segments_enabled) {
            bw.u(0, 1);
        }
        bw.u(address, sps.addr_bits);
    }
    for (uint32_t i = 0; i < pps.num_extra_slice_header_bits; ++i) {
        bw.u(0, 1);
    }
    bw.ue(1);  // slice_type P
    if (pps.output_flag_present) {
        bw.u(1, 1);
    }
    // non-IDR by construction (concealment never targets IRAP pictures)
    const uint32_t poc = poc_lsb_override >= 0
                             ? static_cast<uint32_t>(poc_lsb_override) &
                                   ((1u << sps.log2_max_poc_lsb) - 1)
                             : donor.poc_lsb;
    bw.u(poc, sps.log2_max_poc_lsb);
    // §7.4.7.1: st_rps + long-term block must match the picture's other
    // slices — copy the donor's bits verbatim.
    bw.copy_bits(donor.header_rbsp, donor.strps_bit_begin,
                 donor.strps_bit_end);
    if (sps.temporal_mvp_enabled) {
        bw.u(donor.temporal_mvp ? 1 : 0, 1);
    }
    if (sps.sao_enabled) {
        bw.u(0, 1);  // slice_sao_luma off
        bw.u(0, 1);  // slice_sao_chroma off
    }
    // P slice, one active ref
    if (pps.num_ref_idx_l0_default != 1) {
        bw.u(1, 1);  // num_ref_idx_active_override
        bw.ue(0);    // num_ref_idx_l0_active_minus1
    } else {
        bw.u(0, 1);
    }
    if (pps.lists_modification_present && donor.num_used_refs > 1) {
        bw.u(0, 1);  // ref_pic_list_modification_flag_l0
    }
    if (pps.cabac_init_present) {
        bw.u(0, 1);
    }
    // temporal_mvp: P slice, collocated_from_l0 inferred, num_ref_l0 == 1
    // => no collocated syntax.
    bw.ue(4);  // five_minus_max_num_merge_cand => MaxNumMergeCand = 1
    bw.se(donor.qp_delta);
    if (pps.slice_chroma_qp_offsets_present) {
        bw.se(0);
        bw.se(0);
    }
    bool deblocking_disabled = pps.pps_deblocking_filter_disabled;
    if (pps.deblocking_filter_override_enabled) {
        bw.u(1, 1);  // deblocking_filter_override
        bw.u(1, 1);  // slice_deblocking_filter_disabled
        deblocking_disabled = true;
    }
    if (pps.loop_filter_across_slices && !deblocking_disabled) {
        bw.u(0, 1);  // slice_loop_filter_across_slices_enabled
    }
    if (pps.entropy_coding_sync_enabled) {
        bw.ue(static_cast<uint32_t>(entry_points.size()));
        if (!entry_points.empty()) {
            uint32_t max_off = 1;
            for (const uint32_t ep : entry_points) {
                max_off = std::max(max_off, ep);  // offsets are >= 1 byte
            }
            uint32_t len = 1;
            while ((1u << len) < max_off) {
                ++len;
            }
            bw.ue(len - 1);  // offset_len_minus1
            for (const uint32_t ep : entry_points) {
                bw.u(ep - 1, len);  // entry_point_offset_minus1
            }
        }
    }
    if (pps.slice_segment_header_extension_present) {
        bw.ue(0);
    }
    bw.align_header();
}

struct EmitState {
    CabacEnc* enc;
    CtxSet* ctxs;
    const SpsInfo* sps;
    uint8_t* depth;         // min-CB grid, pic_w_mincb * pic_h_mincb
    uint32_t w_mincb;
    uint32_t first_ctb;     // slice start, CTU raster
    uint32_t cur_ctb;       // current CTU raster addr
};

// Availability of the min-CB whose top-left luma sample is (x, y), for the
// current CU: inside picture and in a CTU of this slice at or before cur.
bool nb_avail(const EmitState& st, int64_t x, int64_t y) {
    if (x < 0 || y < 0 || x >= st.sps->pic_width || y >= st.sps->pic_height) {
        return false;
    }
    const uint32_t addr =
        (static_cast<uint32_t>(y) >> st.sps->log2_ctb) * st.sps->pic_width_ctbs +
        (static_cast<uint32_t>(x) >> st.sps->log2_ctb);
    return addr >= st.first_ctb && addr <= st.cur_ctb;
}

uint8_t nb_depth(const EmitState& st, int64_t x, int64_t y) {
    const uint32_t mx = static_cast<uint32_t>(x) >> st.sps->log2_min_cb;
    const uint32_t my = static_cast<uint32_t>(y) >> st.sps->log2_min_cb;
    return st.depth[my * st.w_mincb + mx];
}

void mark_depth(const EmitState& st, uint32_t x, uint32_t y, uint32_t size,
                uint8_t depth) {
    const uint8_t sh = st.sps->log2_min_cb;
    const uint32_t x1 = std::min<uint32_t>(x + size, st.sps->pic_width);
    const uint32_t y1 = std::min<uint32_t>(y + size, st.sps->pic_height);
    for (uint32_t my = y >> sh; my < ((y1 + (1u << sh) - 1) >> sh); ++my) {
        for (uint32_t mx = x >> sh; mx < ((x1 + (1u << sh) - 1) >> sh); ++mx) {
            st.depth[my * st.w_mincb + mx] = depth;
        }
    }
}

// coding_quadtree for one (sub-)CU of the all-skip slice.
void emit_cu(EmitState& st, uint32_t x, uint32_t y, uint8_t log2size,
             uint8_t depth) {
    if (x >= st.sps->pic_width || y >= st.sps->pic_height) {
        return;  // entirely outside the picture: nothing coded
    }
    const uint32_t size = 1u << log2size;
    const bool inside = (x + size <= st.sps->pic_width) &&
                        (y + size <= st.sps->pic_height);
    if (!inside) {
        // Boundary CTU: split inferred (not coded), recurse into quadrants.
        const uint8_t half = static_cast<uint8_t>(log2size - 1);
        const uint32_t hs = 1u << half;
        emit_cu(st, x, y, half, depth + 1);
        emit_cu(st, x + hs, y, half, depth + 1);
        emit_cu(st, x, y + hs, half, depth + 1);
        emit_cu(st, x + hs, y + hs, half, depth + 1);
        return;
    }
    if (log2size > st.sps->log2_min_cb) {
        // split_cu_flag = 0; ctxInc = sum of (neighbor depth > depth)
        const bool avail_l = nb_avail(st, static_cast<int64_t>(x) - 1, y);
        const bool avail_a = nb_avail(st, x, static_cast<int64_t>(y) - 1);
        uint32_t inc = 0;
        if (avail_l && nb_depth(st, static_cast<int64_t>(x) - 1, y) > depth) {
            ++inc;
        }
        if (avail_a && nb_depth(st, x, static_cast<int64_t>(y) - 1) > depth) {
            ++inc;
        }
        st.enc->encode_bin(st.ctxs->split[inc], 0);
    }
    // cu_skip_flag = 1; ctxInc = availL(skip) + availA(skip) — every coded CU
    // in this slice is skip, so availability alone decides.
    const uint32_t inc =
        (nb_avail(st, static_cast<int64_t>(x) - 1, y) ? 1u : 0u) +
        (nb_avail(st, x, static_cast<int64_t>(y) - 1) ? 1u : 0u);
    st.enc->encode_bin(st.ctxs->skip[inc], 1);
    // MaxNumMergeCand == 1 => merge_idx not coded.
    mark_depth(st, x, y, size, depth);
}

}  // namespace

bool make_conceal_slice(const SpsInfo& sps, const PpsInfo& pps,
                        const SliceInfo& donor, uint32_t address,
                        uint32_t num_ctbs, bool first_slice,
                        int32_t poc_lsb_override, ConcealScratch& scratch,
                        std::vector<uint8_t>& out) {
    if (pps.tiles_enabled || pps.transquant_bypass_enabled ||
        donor.dependent || !donor.has_poc || donor.slice_type == 2 ||
        nal_is_irap(donor.nal_unit_type)) {
        return false;
    }
    if (sps.pic_size_ctbs == 0 || num_ctbs == 0 ||
        address + num_ctbs > sps.pic_size_ctbs ||
        donor.strps_bit_end < donor.strps_bit_begin ||
        donor.strps_bit_end > donor.header_rbsp.size() * 8) {
        return false;
    }
    if ((pps.weighted_pred && donor.slice_type == 1) ||
        (pps.weighted_bipred && donor.slice_type == 0)) {
        return false;
    }

    const int32_t slice_qp = pps.init_qp + donor.qp_delta;

    // Slice data first: WPP entry point offsets go in the header, and they
    // are the escaped byte lengths of the payload's substream segments.
    BitWriter bw(scratch.payload_bits);
    const uint32_t w_mincb =
        (sps.pic_width + (1u << sps.log2_min_cb) - 1) >> sps.log2_min_cb;
    const uint32_t h_mincb =
        (sps.pic_height + (1u << sps.log2_min_cb) - 1) >> sps.log2_min_cb;
    scratch.depth.assign(static_cast<size_t>(w_mincb) * h_mincb, 0);

    CabacEnc enc(bw);
    CtxSet ctxs = fresh_contexts(slice_qp);
    CtxSet snapshot{};
    bool have_snapshot = false;
    EmitState st{&enc, &ctxs, &sps, scratch.depth.data(), w_mincb, address,
                 address};

    const bool wpp = pps.entropy_coding_sync_enabled;
    const uint32_t width = sps.pic_width_ctbs;
    std::vector<uint32_t> segment_starts{0};  // payload byte offsets
    for (uint32_t n = 0; n < num_ctbs; ++n) {
        const uint32_t addr = address + n;
        st.cur_ctb = addr;
        const uint32_t cx = (addr % width) << sps.log2_ctb;
        const uint32_t cy = (addr / width) << sps.log2_ctb;
        emit_cu(st, cx, cy, sps.log2_ctb, 0);
        const bool last = (n == num_ctbs - 1);
        enc.encode_terminate(last ? 1 : 0);
        if (last) {
            bw.align_zero();  // terminate flush carried the rbsp stop bit
            break;
        }
        if (wpp && addr % width == 1) {
            snapshot = ctxs;  // §9.3.2.3 storage after the row's second CTU
            have_snapshot = true;
        }
        if (wpp && (addr + 1) % width == 0) {
            // end_of_subset_one_bit + byte_alignment + engine re-init. The
            // §9.3.4.3.5 flush already ends in the required 1 bit (the |1 of
            // its final write serves as alignment_bit_equal_to_one) — only
            // zero-pad after it. An extra 1 here trips strict decoders
            // (HM asserts) while lenient ones mis-sync one substream.
            enc.encode_terminate(1);
            bw.align_zero();
            segment_starts.push_back(static_cast<uint32_t>(bw.size() / 8));
            enc.reset();
            const uint32_t above_right = addr + 2 - width;
            if (have_snapshot && width >= 2 && above_right >= address) {
                ctxs = snapshot;  // §9.3.2.4 sync from stored contexts
            } else {
                ctxs = fresh_contexts(slice_qp);
            }
        }
    }
    pack_bits(scratch.payload_bits, scratch.payload_rbsp);

    // Escaped byte length per substream segment (except the last). Each
    // segment ends in a nonzero byte (the flush's final 1 bit lives in it),
    // so zero-runs never span segments and per-segment counts are exact.
    std::vector<uint32_t> entry_points;
    if (wpp && segment_starts.size() > 1) {
        for (size_t i = 0; i + 1 < segment_starts.size(); ++i) {
            uint32_t esc = 0;
            uint32_t zeros = 0;
            for (uint32_t b = segment_starts[i]; b < segment_starts[i + 1];
                 ++b) {
                const uint8_t c = scratch.payload_rbsp[b];
                if (zeros >= 2 && c <= 3) {
                    ++esc;
                    zeros = 0;
                }
                zeros = (c == 0) ? zeros + 1 : 0;
            }
            entry_points.push_back(segment_starts[i + 1] - segment_starts[i] +
                                   esc);
        }
    }

    BitWriter hbw(scratch.bits);
    write_conceal_header(hbw, sps, pps, donor, address, first_slice,
                         poc_lsb_override, entry_points);
    pack_bits(scratch.bits, scratch.rbsp);
    scratch.rbsp.insert(scratch.rbsp.end(), scratch.payload_rbsp.begin(),
                        scratch.payload_rbsp.end());
    escape_append(scratch.rbsp, donor.nal_header, out);
    return true;
}

}  // namespace wblink::hevc
