// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: HEVC concealment-slice synthesis (PROTOCOL.md §6.3b).
//
// Parses just enough of a multi-slice HEVC stream (SPS/PPS + slice segment
// headers) to rewrite a surviving slice's header for an erased slice's
// address span and append an all-skip CABAC payload: every CTU is coded as a
// skip CU (MaxNumMergeCand=1, SAO off), so the decoder reconstructs the span
// from the reference picture with the merge candidate's motion — a frozen
// region, not a dropped picture. The output NAL is syntactically complete,
// standards-valid HEVC (validated offline against ffmpeg + libde265 on x265
// and HM-18.0 vectors; see docs/findings.md 2026-08-19).
//
// Scope guards (each unsupported shape fails the parse, never guesses):
// dependent slice segments, weighted prediction, tiles, palette/SCC,
// separate colour planes. WPP (entropy_coding_sync) IS supported — x265
// cannot emit multi-slice without it, so test vectors need it — hardware
// encoders (SigmaStar/HiSilicon) emit neither WPP nor tiles.
//
// Pure logic: no I/O, no clocks, no dependencies beyond the C++ stdlib.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wblink::hevc {

// NAL unit types (Table 7-1) — the few this module dispatches on.
inline constexpr uint8_t kNalSps = 33;
inline constexpr uint8_t kNalPps = 34;
inline bool nal_is_vcl(uint8_t t) { return t <= 31; }
inline bool nal_is_irap(uint8_t t) { return t >= 16 && t <= 23; }
inline bool nal_is_idr(uint8_t t) { return t == 19 || t == 20; }
// nal points at the 2-byte NAL header (after the start code).
inline uint8_t nal_type(const uint8_t* nal) { return (nal[0] >> 1) & 0x3F; }

struct SpsInfo {
    uint8_t sps_id = 0;
    uint8_t chroma_format_idc = 1;
    uint32_t pic_width = 0;
    uint32_t pic_height = 0;
    uint8_t log2_max_poc_lsb = 0;
    uint8_t log2_min_cb = 0;   // luma coding block
    uint8_t log2_ctb = 0;      // CTU
    bool sao_enabled = false;
    bool temporal_mvp_enabled = false;
    bool long_term_ref_pics_present = false;
    uint8_t num_long_term_ref_pics_sps = 0;
    uint8_t num_st_rps = 0;
    // per SPS short-term RPS: NumDeltaPocs and used-by-curr count, needed to
    // parse a slice header that references them.
    std::vector<uint16_t> st_rps_num_delta;
    std::vector<uint16_t> st_rps_num_used;
    // derived
    uint32_t pic_width_ctbs = 0;
    uint32_t pic_height_ctbs = 0;
    uint32_t pic_size_ctbs = 0;
    uint8_t addr_bits = 0;  // Ceil(Log2(PicSizeInCtbsY))
};

struct PpsInfo {
    uint8_t pps_id = 0;
    uint8_t sps_id = 0;
    bool dependent_slice_segments_enabled = false;
    bool output_flag_present = false;
    uint8_t num_extra_slice_header_bits = 0;
    bool cabac_init_present = false;
    uint16_t num_ref_idx_l0_default = 1;
    uint16_t num_ref_idx_l1_default = 1;
    int8_t init_qp = 26;
    bool cu_qp_delta_enabled = false;
    bool slice_chroma_qp_offsets_present = false;
    bool weighted_pred = false;
    bool weighted_bipred = false;
    bool transquant_bypass_enabled = false;
    bool tiles_enabled = false;
    bool entropy_coding_sync_enabled = false;
    bool loop_filter_across_slices = false;
    bool deblocking_filter_control_present = false;
    bool deblocking_filter_override_enabled = false;
    bool pps_deblocking_filter_disabled = false;
    bool lists_modification_present = false;
    bool slice_segment_header_extension_present = false;
};

// Parsed slice segment header — the fields concealment needs, plus the raw
// header RBSP prefix so picture-level bit spans (short_term_ref_pic_set +
// long-term block, identical across a picture's slices per §7.4.7.1) can be
// copied verbatim into a synthesized header.
struct SliceInfo {
    uint8_t nal_header[2] = {0, 0};
    uint8_t nal_unit_type = 0;
    bool first_slice = false;
    bool dependent = false;
    uint8_t pps_id = 0;
    uint32_t address = 0;
    uint8_t slice_type = 0;  // 0=B 1=P 2=I
    uint32_t poc_lsb = 0;
    bool has_poc = false;    // false on IDR
    bool temporal_mvp = false;
    int32_t qp_delta = 0;
    uint16_t num_used_refs = 0;  // NumPicTotalCurr for lists_modification
    // §7.4.7.1 consistency: collocated_ref_idx (and the ref list shape it
    // indexes) must match across a picture's slices, so the synthesized
    // header mirrors the donor's L0 count and collocated index when TMVP is
    // on instead of forcing a 1-entry list.
    uint16_t num_ref_l0 = 1;
    uint16_t collocated_ref_idx = 0;
    // bit span [begin, end) of st_rps + long-term block within header_rbsp
    uint32_t strps_bit_begin = 0;
    uint32_t strps_bit_end = 0;
    std::vector<uint8_t> header_rbsp;  // unescaped header prefix bytes
};

// All parsers take the NAL payload (EBSP, starting at the 2-byte NAL header,
// no start code) and return false on any unsupported or malformed syntax.
bool parse_sps(const uint8_t* nal, size_t len, SpsInfo& out);
bool parse_pps(const uint8_t* nal, size_t len, PpsInfo& out);
bool parse_slice_header(const uint8_t* nal, size_t len, const SpsInfo& sps,
                        const PpsInfo& pps, SliceInfo& out);

// Reusable scratch for make_conceal_slice (depth grid + bit buffer), so the
// steady state allocates nothing once warmed to the stream's geometry.
struct ConcealScratch {
    std::vector<uint8_t> depth;         // min-CB granularity depth grid
    std::vector<uint8_t> bits;          // header bit accumulator
    std::vector<uint8_t> payload_bits;  // slice-data bit accumulator
    std::vector<uint8_t> rbsp;          // packed header bytes
    std::vector<uint8_t> payload_rbsp;  // packed slice-data bytes
};

// Synthesize a complete concealment slice NAL for CTU raster addresses
// [address, address + num_ctbs). donor is a parsed slice header of the SAME
// picture (or, for §6.3b whole-frame freeze, of an earlier P picture with
// poc_lsb_override >= 0 supplying the new slice_pic_order_cnt_lsb).
// Appends a 4-byte start code + escaped NAL to out. Returns false when the
// stream shape is outside the supported envelope (caller falls back to drop).
bool make_conceal_slice(const SpsInfo& sps, const PpsInfo& pps,
                        const SliceInfo& donor, uint32_t address,
                        uint32_t num_ctbs, bool first_slice,
                        int32_t poc_lsb_override, ConcealScratch& scratch,
                        std::vector<uint8_t>& out);

}  // namespace wblink::hevc
