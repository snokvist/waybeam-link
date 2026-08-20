#!/usr/bin/env python3
"""hevc_ms.py — multi-slice HEVC toolkit for the Waybeam spatial-concealment
prototype.

Capabilities:
  * Annex-B NAL splitting, RBSP<->EBSP
  * SPS/PPS parsing (fields needed for slice-header parsing)
  * Full slice-segment-header parse (P/B/I, no SCC extensions)
  * Concealment slice generation: rewrite a donor slice header for a target
    slice-segment address and append an all-skip CABAC payload (optionally
    WPP-aware) so the erased region is reconstructed from the reference
    picture with zero motion.
  * AU model: group NALs into access units, list slices with byte ranges.

Kept dependency-free (stdlib only) so it can double as an offline test
harness component.
"""
import sys, struct

# ---------------------------------------------------------------- bit I/O

class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0  # bit position

    def bits_left(self):
        return len(self.data) * 8 - self.pos

    def u(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def ue(self):
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
            if zeros > 40:
                raise ValueError("bad ue(v)")
        return (1 << zeros) - 1 + (self.u(zeros) if zeros else 0)

    def se(self):
        k = self.ue()
        return (k + 1) >> 1 if (k & 1) else -(k >> 1)

    def byte_aligned(self):
        return (self.pos & 7) == 0


class BitWriter:
    def __init__(self):
        self.bits = []

    def u(self, v, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(0, n - 1)
        self.u(v, n)

    def se(self, v):
        self.ue(2 * v - 1 if v > 0 else -2 * v)

    def copy_bits(self, reader_data, start, end):
        for p in range(start, end):
            self.bits.append((reader_data[p >> 3] >> (7 - (p & 7))) & 1)

    def byte_align_rbsp_header(self):
        # slice header byte_alignment(): one 1 bit then 0s
        self.bits.append(1)
        while len(self.bits) & 7:
            self.bits.append(0)

    def to_bytes(self):
        assert len(self.bits) & 7 == 0
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            b = 0
            for j in range(8):
                b = (b << 1) | self.bits[i + j]
            out.append(b)
        return bytes(out)


# ---------------------------------------------------------------- Annex-B / RBSP

def split_annexb(data: bytes):
    """Return list of (sc_off, nal_off, nal_end) — nal bytes are data[nal_off:nal_end]."""
    out = []
    i = 0
    n = len(data)
    while i + 3 <= n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            sc = i
            # include a preceding zero byte (4-byte start code) in sc span
            if sc > 0 and data[sc - 1] == 0:
                sc -= 1
            out.append([sc, i + 3, None])
            i += 3
        else:
            i += 1
    for idx in range(len(out)):
        out[idx][2] = out[idx + 1][0] if idx + 1 < len(out) else n
    # strip trailing zero padding? leave as is.
    return [tuple(x) for x in out]


def ebsp_to_rbsp(b: bytes) -> bytes:
    out = bytearray()
    zeros = 0
    i = 0
    while i < len(b):
        c = b[i]
        if zeros >= 2 and c == 3:
            zeros = 0
            i += 1
            continue
        out.append(c)
        zeros = zeros + 1 if c == 0 else 0
        i += 1
    return bytes(out)


def rbsp_to_ebsp(b: bytes) -> bytes:
    out = bytearray()
    zeros = 0
    for c in b:
        if zeros >= 2 and c <= 3:
            out.append(3)
            zeros = 0
        out.append(c)
        zeros = zeros + 1 if c == 0 else 0
    return bytes(out)


def nal_type(nal: bytes) -> int:
    return (nal[0] >> 1) & 0x3F


IS_VCL = lambda t: t <= 31
IS_IRAP = lambda t: 16 <= t <= 23
IS_IDR = lambda t: t in (19, 20)


# ---------------------------------------------------------------- SPS/PPS

def parse_profile_tier_level(r: BitReader, max_sub_layers_minus1):
    r.u(2 + 1 + 5)  # general_profile_space, tier, idc
    r.u(32)  # compatibility flags
    r.u(1 + 1 + 1 + 1)  # progressive, interlaced, non_packed, frame_only
    r.u(43)  # reserved (we don't need range-ext flags individually)
    r.u(1)
    r.u(8)  # level_idc
    sub_profile = []
    sub_level = []
    for _ in range(max_sub_layers_minus1):
        sub_profile.append(r.u(1))
        sub_level.append(r.u(1))
    if max_sub_layers_minus1 > 0:
        for _ in range(max_sub_layers_minus1, 8):
            r.u(2)
    for i in range(max_sub_layers_minus1):
        if sub_profile[i]:
            r.u(2 + 1 + 5 + 32 + 4 + 43 + 1)
            r.u(8)
        if sub_level[i]:
            r.u(8)


def parse_st_rps(r: BitReader, idx, rps_list, num_sets=None):
    """Parse short_term_ref_pic_set. num_sets = sps num_short_term_ref_pic_sets;
    delta_idx_minus1 exists only in the slice-header call (idx == num_sets)."""
    if num_sets is None:
        num_sets = len(rps_list)  # slice-header call convention
    inter_pred = 0
    if idx != 0:
        inter_pred = r.u(1)
    if inter_pred:
        ref_idx = idx - 1
        if idx == num_sets:  # slice-header case only
            delta_idx_minus1 = r.ue()
            ref_idx = idx - 1 - delta_idx_minus1
        r.u(1)  # delta_rps_sign
        r.ue()  # abs_delta_rps_minus1
        ref = rps_list[ref_idx]
        num_delta = 0
        used = []
        for _ in range(ref["num_delta_pocs"] + 1):
            used_flag = r.u(1)
            use_delta = 1
            if not used_flag:
                use_delta = r.u(1)
            if used_flag or use_delta:
                num_delta += 1
            used.append(used_flag)
        return {"num_delta_pocs": num_delta, "num_used": sum(used)}
    else:
        num_neg = r.ue()
        num_pos = r.ue()
        used = 0
        for _ in range(num_neg):
            r.ue()  # delta_poc_s0_minus1
            used += r.u(1)
        for _ in range(num_pos):
            r.ue()
            used += r.u(1)
        return {"num_delta_pocs": num_neg + num_pos, "num_used": used}


def parse_sps(nal: bytes):
    r = BitReader(ebsp_to_rbsp(nal[2:]))
    s = {}
    r.u(4)  # vps id
    max_sub_layers_minus1 = r.u(3)
    r.u(1)  # temporal_id_nesting
    parse_profile_tier_level(r, max_sub_layers_minus1)
    s["sps_id"] = r.ue()
    s["chroma_format_idc"] = r.ue()
    if s["chroma_format_idc"] == 3:
        s["separate_colour_plane"] = r.u(1)
    else:
        s["separate_colour_plane"] = 0
    s["pic_width"] = r.ue()
    s["pic_height"] = r.ue()
    if r.u(1):  # conformance window
        r.ue(); r.ue(); r.ue(); r.ue()
    s["bit_depth_luma"] = r.ue() + 8
    s["bit_depth_chroma"] = r.ue() + 8
    s["log2_max_poc_lsb"] = r.ue() + 4
    sub_layer_ordering = r.u(1)
    for _ in range((0 if sub_layer_ordering else max_sub_layers_minus1), max_sub_layers_minus1 + 1):
        r.ue(); r.ue(); r.ue()
    s["log2_min_cb"] = r.ue() + 3
    s["log2_ctb"] = s["log2_min_cb"] + r.ue()
    s["log2_min_tb"] = r.ue() + 2
    s["log2_max_tb"] = s["log2_min_tb"] + r.ue()
    s["max_transform_hierarchy_depth_inter"] = r.ue()
    s["max_transform_hierarchy_depth_intra"] = r.ue()
    s["scaling_list_enabled"] = r.u(1)
    if s["scaling_list_enabled"]:
        if r.u(1):  # sps_scaling_list_data_present
            parse_scaling_list(r)
    s["amp_enabled"] = r.u(1)
    s["sao_enabled"] = r.u(1)
    s["pcm_enabled"] = r.u(1)
    if s["pcm_enabled"]:
        r.u(4); r.u(4)
        r.ue(); r.ue(); r.u(1)
    s["num_st_rps"] = r.ue()
    rps_list = []
    for i in range(s["num_st_rps"]):
        rps_list.append(parse_st_rps(r, i, rps_list, num_sets=s["num_st_rps"]))
    s["st_rps"] = rps_list
    s["long_term_ref_pics_present"] = r.u(1)
    if s["long_term_ref_pics_present"]:
        s["num_long_term_ref_pics_sps"] = r.ue()
        for _ in range(s["num_long_term_ref_pics_sps"]):
            r.u(s["log2_max_poc_lsb"])
            r.u(1)
    else:
        s["num_long_term_ref_pics_sps"] = 0
    s["temporal_mvp_enabled"] = r.u(1)
    s["strong_intra_smoothing"] = r.u(1)
    # ignore VUI + extensions
    ctb = 1 << s["log2_ctb"]
    s["pic_width_ctbs"] = (s["pic_width"] + ctb - 1) >> s["log2_ctb"]
    s["pic_height_ctbs"] = (s["pic_height"] + ctb - 1) >> s["log2_ctb"]
    s["pic_size_ctbs"] = s["pic_width_ctbs"] * s["pic_height_ctbs"]
    v = 0
    while (1 << v) < s["pic_size_ctbs"]:
        v += 1
    s["addr_bits"] = v
    return s


def parse_scaling_list(r: BitReader):
    for size_id in range(4):
        m = 1 if size_id == 3 else 0
        num = 6 if size_id < 3 else 2  # sizeId 3: matrixId 0..1 step... actually 6/ (3? ) — HEVC: for sizeId==3, matrixId in {0,3} → loop 2
        for _ in range(num):
            if not r.u(1):  # pred_mode_flag
                r.ue()
            else:
                coefs = min(64, 1 << (4 + (size_id << 1)))
                if size_id > 1:
                    r.se()
                for _ in range(coefs):
                    r.se()
        _ = m


def parse_pps(nal: bytes):
    r = BitReader(ebsp_to_rbsp(nal[2:]))
    p = {}
    p["pps_id"] = r.ue()
    p["sps_id"] = r.ue()
    p["dependent_slice_segments_enabled"] = r.u(1)
    p["output_flag_present"] = r.u(1)
    p["num_extra_slice_header_bits"] = r.u(3)
    p["sign_data_hiding"] = r.u(1)
    p["cabac_init_present"] = r.u(1)
    p["num_ref_idx_l0_default"] = r.ue() + 1
    p["num_ref_idx_l1_default"] = r.ue() + 1
    p["init_qp"] = r.se() + 26
    p["constrained_intra_pred"] = r.u(1)
    p["transform_skip_enabled"] = r.u(1)
    p["cu_qp_delta_enabled"] = r.u(1)
    if p["cu_qp_delta_enabled"]:
        p["diff_cu_qp_delta_depth"] = r.ue()
    p["pps_cb_qp_offset"] = r.se()
    p["pps_cr_qp_offset"] = r.se()
    p["slice_chroma_qp_offsets_present"] = r.u(1)
    p["weighted_pred"] = r.u(1)
    p["weighted_bipred"] = r.u(1)
    p["transquant_bypass_enabled"] = r.u(1)
    p["tiles_enabled"] = r.u(1)
    p["entropy_coding_sync_enabled"] = r.u(1)
    if p["tiles_enabled"]:
        p["num_tile_cols"] = r.ue() + 1
        p["num_tile_rows"] = r.ue() + 1
        p["uniform_spacing"] = r.u(1)
        if not p["uniform_spacing"]:
            for _ in range(p["num_tile_cols"] - 1):
                r.ue()
            for _ in range(p["num_tile_rows"] - 1):
                r.ue()
        p["loop_filter_across_tiles"] = r.u(1)
    p["loop_filter_across_slices"] = r.u(1)
    p["deblocking_filter_control_present"] = r.u(1)
    p["deblocking_filter_override_enabled"] = 0
    p["pps_deblocking_filter_disabled"] = 0
    if p["deblocking_filter_control_present"]:
        p["deblocking_filter_override_enabled"] = r.u(1)
        p["pps_deblocking_filter_disabled"] = r.u(1)
        if not p["pps_deblocking_filter_disabled"]:
            p["pps_beta_offset_div2"] = r.se()
            p["pps_tc_offset_div2"] = r.se()
    p["pps_scaling_list_data_present"] = r.u(1)
    if p["pps_scaling_list_data_present"]:
        parse_scaling_list(r)
    p["lists_modification_present"] = r.u(1)
    p["log2_parallel_merge_level"] = r.ue() + 2
    p["slice_segment_header_extension_present"] = r.u(1)
    return p


# ---------------------------------------------------------------- slice header

class SliceHeader:
    pass


def parse_slice_header(nal: bytes, sps, pps_map):
    """Parse a slice segment header. Returns SliceHeader with parsed fields and
    bit spans (positions are within the RBSP of nal[2:])."""
    t = nal_type(nal)
    rbsp = ebsp_to_rbsp(nal[2:])
    r = BitReader(rbsp)
    h = SliceHeader()
    h.nal_type = t
    h.nal_header = nal[0:2]
    h.rbsp = rbsp
    h.first_slice = r.u(1)
    if IS_IRAP(t):
        h.no_output_of_prior_pics = r.u(1)
    h.pps_id = r.ue()
    pps = pps_map[h.pps_id]
    h.dependent = 0
    h.address = 0
    if not h.first_slice:
        if pps["dependent_slice_segments_enabled"]:
            h.dependent = r.u(1)
        h.address = r.u(sps["addr_bits"])
    if h.dependent:
        raise ValueError("dependent slice segments unsupported")
    for _ in range(pps["num_extra_slice_header_bits"]):
        r.u(1)
    h.slice_type = r.ue()
    if pps["output_flag_present"]:
        h.pic_output_flag = r.u(1)
    if sps["separate_colour_plane"]:
        h.colour_plane_id = r.u(2)
    h.poc_lsb = None
    h.strps_span = None
    h.temporal_mvp = 0
    if not IS_IDR(t):
        h.poc_lsb = r.u(sps["log2_max_poc_lsb"])
        strps_start = r.pos
        h.st_rps_sps_flag = r.u(1)
        if not h.st_rps_sps_flag:
            h.st_rps = parse_st_rps(r, sps["num_st_rps"], sps["st_rps"])
        else:
            if sps["num_st_rps"] > 1:
                bits = max(1, (sps["num_st_rps"] - 1).bit_length())
                h.st_rps_idx = r.u(bits)
            else:
                h.st_rps_idx = 0
            h.st_rps = sps["st_rps"][h.st_rps_idx]
        # long-term pics
        if sps["long_term_ref_pics_present"]:
            num_lt_sps = num_lt_pics = 0
            if sps["num_long_term_ref_pics_sps"] > 0:
                num_lt_sps = r.ue()
            num_lt_pics = r.ue()
            for i in range(num_lt_sps + num_lt_pics):
                if i < num_lt_sps:
                    if sps["num_long_term_ref_pics_sps"] > 1:
                        r.u((sps["num_long_term_ref_pics_sps"] - 1).bit_length())
                else:
                    r.u(sps["log2_max_poc_lsb"])
                    r.u(1)
                if r.u(1):  # delta_poc_msb_present
                    r.ue()
            h.num_lt = num_lt_sps + num_lt_pics
        else:
            h.num_lt = 0
        h.strps_span = (strps_start, r.pos)  # covers st_rps + lt (all must-match)
        if sps["temporal_mvp_enabled"]:
            h.temporal_mvp = r.u(1)
    h.sao_luma = h.sao_chroma = 0
    if sps["sao_enabled"]:
        h.sao_luma = r.u(1)
        h.sao_chroma = r.u(1)
    h.num_ref_l0 = pps["num_ref_idx_l0_default"]
    h.num_ref_l1 = pps["num_ref_idx_l1_default"]
    h.cabac_init = 0
    h.max_merge = 5
    h.collocated_from_l0 = 1
    if h.slice_type in (0, 1):  # B or P
        if r.u(1):  # num_ref_idx_active_override
            h.num_ref_l0 = r.ue() + 1
            if h.slice_type == 0:
                h.num_ref_l1 = r.ue() + 1
        num_pic_total_curr = h.st_rps["num_used"] + h.num_lt
        if pps["lists_modification_present"] and num_pic_total_curr > 1:
            # ref_pic_list_modification
            nbits = max(1, (num_pic_total_curr - 1).bit_length())
            # list_entry uses Ceil(Log2(NumPicTotalCurr)) bits
            nbits = (num_pic_total_curr - 1).bit_length()
            if r.u(1):
                for _ in range(h.num_ref_l0):
                    r.u(nbits)
            if h.slice_type == 0:
                if r.u(1):
                    for _ in range(h.num_ref_l1):
                        r.u(nbits)
        if h.slice_type == 0:
            h.mvd_l1_zero = r.u(1)
        if pps["cabac_init_present"]:
            h.cabac_init = r.u(1)
        if h.temporal_mvp:
            if h.slice_type == 0:
                h.collocated_from_l0 = r.u(1)
            if (h.collocated_from_l0 and h.num_ref_l0 > 1) or \
               (not h.collocated_from_l0 and h.num_ref_l1 > 1):
                h.collocated_ref_idx = r.ue()
        if (pps["weighted_pred"] and h.slice_type == 1) or \
           (pps["weighted_bipred"] and h.slice_type == 0):
            raise ValueError("weighted prediction unsupported")
        h.max_merge = 5 - r.ue()
    h.qp_delta = r.se()
    h.cb_qp_offset = h.cr_qp_offset = 0
    if pps["slice_chroma_qp_offsets_present"]:
        h.cb_qp_offset = r.se()
        h.cr_qp_offset = r.se()
    h.deblocking_override = 0
    h.deblocking_disabled = pps["pps_deblocking_filter_disabled"]
    if pps["deblocking_filter_override_enabled"]:
        h.deblocking_override = r.u(1)
    if h.deblocking_override:
        h.deblocking_disabled = r.u(1)
        if not h.deblocking_disabled:
            h.beta_offset_div2 = r.se()
            h.tc_offset_div2 = r.se()
    if pps["loop_filter_across_slices"] and \
       (h.sao_luma or h.sao_chroma or not h.deblocking_disabled):
        h.loop_filter_across_slices = r.u(1)
    h.num_entry_points = 0
    if pps["tiles_enabled"] or pps["entropy_coding_sync_enabled"]:
        h.num_entry_points = r.ue()
        if h.num_entry_points > 0:
            olen = r.ue() + 1
            for _ in range(h.num_entry_points):
                r.u(olen)
    if pps["slice_segment_header_extension_present"]:
        ext_len = r.ue()
        for _ in range(ext_len):
            r.u(8)
    # byte_alignment
    h.header_end_before_align = r.pos
    if r.u(1) != 1:
        raise ValueError("bad slice header alignment bit")
    while not r.byte_aligned():
        if r.u(1) != 0:
            raise ValueError("bad slice header alignment padding")
    h.data_start_byte = r.pos >> 3
    h.slice_qp = 26 + pps["init_qp"] - 26 + h.qp_delta  # = init_qp + delta
    h.slice_qp = pps["init_qp"] + h.qp_delta
    return h


# ---------------------------------------------------------------- CABAC encoder

LPS_TABLE = [
    [128,176,208,240],[128,167,197,227],[128,158,187,216],[123,150,178,205],
    [116,142,169,195],[111,135,160,185],[105,128,152,175],[100,122,144,166],
    [95,116,137,158],[90,110,130,150],[85,104,123,142],[81,99,117,135],
    [77,94,111,128],[73,89,105,122],[69,85,100,116],[66,80,95,110],
    [62,76,90,104],[59,72,86,99],[56,69,81,94],[53,65,77,89],
    [51,62,73,85],[48,59,69,80],[46,56,66,76],[43,53,63,72],
    [41,50,59,69],[39,48,56,65],[37,45,54,62],[35,43,51,59],
    [33,41,48,56],[32,39,46,53],[30,37,43,50],[29,35,41,48],
    [27,33,39,45],[26,31,37,43],[24,30,35,41],[23,28,33,39],
    [22,27,32,37],[21,26,30,35],[20,24,29,33],[19,23,27,31],
    [18,22,26,30],[17,21,25,28],[16,20,23,27],[15,19,22,25],
    [14,18,21,24],[14,17,20,23],[13,16,19,22],[12,15,18,21],
    [12,14,17,20],[11,14,16,19],[11,13,15,18],[10,12,15,17],
    [10,12,14,16],[9,11,13,15],[9,11,12,14],[8,10,12,14],
    [8,9,11,13],[7,9,11,12],[7,9,10,12],[7,8,10,11],
    [6,8,9,11],[6,7,9,10],[6,7,8,9],[2,2,2,2],
]
NEXT_STATE_MPS = [
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63,
]
NEXT_STATE_LPS = [
    0,0,1,2,2,4,4,5,6,7,8,9,9,11,11,12,
    13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,
    33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63,
]

# context init values (initType indexed where relevant)
INIT_SPLIT_CU = {0: [139,141,157], 1: [107,139,126], 2: [107,139,126]}
INIT_CU_SKIP = {1: [197,185,201], 2: [197,185,201]}
INIT_SAO_MERGE = {0: 153, 1: 153, 2: 153}
INIT_SAO_TYPE = {0: 200, 1: 185, 2: 160}


def ctx_init(init_value, qp):
    qp = max(0, min(51, qp))
    slope = (init_value >> 4) * 5 - 45
    offset = ((init_value & 15) << 3) - 16
    pre = ((slope * qp) >> 4) + offset
    pre = max(1, min(126, pre))
    if pre <= 63:
        return (63 - pre, 0)  # (state, mps)
    return (pre - 64, 1)


class CabacEnc:
    """HEVC CABAC encoder, bit-exact per spec 9.3.4."""
    def __init__(self, bw: BitWriter):
        self.bw = bw
        self.reset_engine()

    def reset_engine(self):
        self.low = 0
        self.range = 510
        self.bits_outstanding = 0
        self.first_bit = True

    def _write(self, b):
        self.bw.bits.append(b)

    def _put_bit(self, b):
        if self.first_bit:
            self.first_bit = False
        else:
            self._write(b)
        while self.bits_outstanding > 0:
            self._write(1 - b)
            self.bits_outstanding -= 1

    def _renorm(self):
        while self.range < 256:
            if self.low < 256:
                self._put_bit(0)
            elif self.low >= 512:
                self.low -= 512
                self._put_bit(1)
            else:
                self.low -= 256
                self.bits_outstanding += 1
            self.range <<= 1
            self.low <<= 1

    def encode_bin(self, ctx, bin_val):
        state, mps = ctx
        lps = LPS_TABLE[state][(self.range >> 6) & 3]
        self.range -= lps
        if bin_val != mps:
            self.low += self.range
            self.range = lps
            if state == 0:
                mps = 1 - mps
            state = NEXT_STATE_LPS[state]
        else:
            state = NEXT_STATE_MPS[state]
        self._renorm()
        return (state, mps)

    def encode_terminate(self, bin_val):
        self.range -= 2
        if bin_val:
            self.low += self.range
            self.range = 2
            self._flush()
        else:
            self._renorm()

    def _flush(self):
        self.range = 2
        self._renorm()
        self._put_bit((self.low >> 9) & 1)
        # WriteBits(((low>>7)&3)|1, 2) — direct write
        v = ((self.low >> 7) & 3) | 1
        self._write((v >> 1) & 1)
        self._write(v & 1)


# ---------------------------------------------------------------- skip-slice generation

def write_conceal_header(bw, sps, pps, donor, address, is_first_slice,
                         poc_lsb=None):
    """Emit a P-slice concealment header derived from donor (a SliceHeader of
    the same picture, or of a previous P picture with poc_lsb overridden)."""
    bw.u(1 if is_first_slice else 0, 1)
    bw.ue(donor.pps_id)
    if not is_first_slice:
        if pps["dependent_slice_segments_enabled"]:
            bw.u(0, 1)
        bw.u(address, sps["addr_bits"])
    for _ in range(pps["num_extra_slice_header_bits"]):
        bw.u(0, 1)
    bw.ue(1)  # slice_type P
    if pps["output_flag_present"]:
        bw.u(getattr(donor, "pic_output_flag", 1), 1)
    if sps["separate_colour_plane"]:
        bw.u(donor.colour_plane_id, 2)
    # non-IDR only (we never conceal IDR pictures)
    bw.u(donor.poc_lsb if poc_lsb is None else poc_lsb, sps["log2_max_poc_lsb"])
    # copy st_rps + long-term block verbatim from donor (must-match fields)
    bw.copy_bits(donor.rbsp, donor.strps_span[0], donor.strps_span[1])
    if sps["temporal_mvp_enabled"]:
        bw.u(donor.temporal_mvp, 1)
    if sps["sao_enabled"]:
        bw.u(0, 1)  # sao_luma off
        bw.u(0, 1)  # sao_chroma off
    # P slice part
    num_ref_l0 = 1
    if num_ref_l0 != pps["num_ref_idx_l0_default"]:
        bw.u(1, 1)  # override
        bw.ue(num_ref_l0 - 1)
    else:
        bw.u(0, 1)
    num_pic_total_curr = donor.st_rps["num_used"] + donor.num_lt
    if pps["lists_modification_present"] and num_pic_total_curr > 1:
        bw.u(0, 1)  # ref_pic_list_modification_flag_l0 = 0
    if pps["cabac_init_present"]:
        bw.u(0, 1)
    if donor.temporal_mvp:
        # P slice: collocated_from_l0 inferred = 1; collocated_ref_idx
        # present only if num_ref_l0 > 1 — ours is 1, so nothing.
        pass
    bw.ue(4)  # five_minus_max_num_merge_cand -> MaxNumMergeCand = 1
    bw.se(donor.qp_delta)
    if pps["slice_chroma_qp_offsets_present"]:
        bw.se(0)
        bw.se(0)
    deblocking_disabled = pps["pps_deblocking_filter_disabled"]
    if pps["deblocking_filter_override_enabled"]:
        bw.u(1, 1)  # override
        bw.u(1, 1)  # slice_deblocking_filter_disabled = 1
        deblocking_disabled = 1
    if pps["loop_filter_across_slices"] and (0 or 0 or not deblocking_disabled):
        bw.u(0, 1)  # slice_loop_filter_across_slices_enabled = 0
    if pps["tiles_enabled"] or pps["entropy_coding_sync_enabled"]:
        bw.ue(0)  # num_entry_point_offsets = 0
    if pps["slice_segment_header_extension_present"]:
        bw.ue(0)
    bw.byte_align_rbsp_header()
    return pps["init_qp"] + donor.qp_delta


def gen_skip_slice_data(bw, sps, pps, slice_qp, first_ctb, num_ctbs):
    """Append CABAC all-skip slice data for CTUs [first_ctb, first_ctb+num_ctbs)."""
    init_type = 1  # P slice, cabac_init_flag = 0
    enc = CabacEnc(bw)

    def fresh_contexts():
        return {
            "split": [ctx_init(v, slice_qp) for v in INIT_SPLIT_CU[init_type]],
            "skip": [ctx_init(v, slice_qp) for v in INIT_CU_SKIP[init_type]],
        }

    ctxs = fresh_contexts()
    wpp = pps["entropy_coding_sync_enabled"]
    width = sps["pic_width_ctbs"]
    snapshot = None
    for n in range(num_ctbs):
        addr = first_ctb + n
        x = addr % width
        y = addr // width
        # availability within slice (slices assumed CTU-raster contiguous)
        left_in_slice = x > 0 and n > 0
        above_in_slice = addr - width >= first_ctb
        # WPP: at start of a CTU row (not the first CTU of slice), engine was
        # re-initialized after byte alignment; contexts restored from snapshot
        # if the above-right CTU is in the slice, else fresh.
        # split_cu_flag: neighbors all depth 0 -> ctxInc 0
        ctxs["split"][0] = enc.encode_bin(ctxs["split"][0], 0)
        skip_inc = (1 if left_in_slice else 0) + (1 if above_in_slice else 0)
        ctxs["skip"][skip_inc] = enc.encode_bin(ctxs["skip"][skip_inc], 1)
        # merge_idx not coded (MaxNumMergeCand == 1)
        last = n == num_ctbs - 1
        enc.encode_terminate(1 if last else 0)
        if last:
            # flush already done in encode_terminate(1); bits end with rbsp
            # stop bit ('|1'), now byte-align with zeros
            while len(bw.bits) & 7:
                bw.bits.append(0)
            break
        # WPP snapshot: after CTU with x == 1 (second CTU of a row)
        if wpp and x == 1:
            snapshot = {k: list(v) for k, v in ctxs.items()}
        # WPP row transition inside the slice
        if wpp and (addr + 1) % width == 0:
            # end_of_subset_one_bit (terminate, value 1) + byte alignment.
            # The flush's final |1 bit IS the alignment_bit_equal_to_one —
            # only zero-pad after it (HM asserts on an extra 1 bit).
            enc.encode_terminate(1)
            while len(bw.bits) & 7:
                bw.bits.append(0)
            enc.reset_engine()
            # context sync from snapshot if above-right CTU in same slice
            next_addr = addr + 1
            above_right = next_addr - width + 1
            if snapshot is not None and width >= 2 and above_right >= first_ctb:
                ctxs = {k: list(v) for k, v in snapshot.items()}
            else:
                ctxs = fresh_contexts()
    return bw


def make_conceal_slice(sps, pps, donor, address, num_ctbs, is_first_slice,
                       poc_lsb=None):
    bw = BitWriter()
    slice_qp = write_conceal_header(bw, sps, pps, donor, address, is_first_slice,
                                    poc_lsb=poc_lsb)
    gen_skip_slice_data(bw, sps, pps, slice_qp, address, num_ctbs)
    rbsp = bw.to_bytes()
    # NAL header: reuse donor's (same type/layer/tid). Concealment output is a
    # referenced trailing picture slice — donor of same picture guarantees it.
    nal = donor.nal_header + rbsp_to_ebsp(rbsp)
    return b"\x00\x00\x00\x01" + nal


# ---------------------------------------------------------------- AU model

class AccessUnit:
    def __init__(self):
        self.nals = []          # list of (nal_bytes, sc_len)
        self.slices = []        # indices into nals that are VCL

    def rebuild(self):
        out = bytearray()
        for nal, sc in self.nals:
            out += b"\x00\x00\x00\x01" if sc == 4 else b"\x00\x00\x01"
            out += nal
        return bytes(out)


def split_aus(data: bytes):
    """Group an Annex-B elementary stream into access units. New AU starts at a
    VCL NAL with first_slice_segment_in_pic_flag=1 (first bit of slice header),
    with any immediately preceding non-VCL NALs (VPS/SPS/PPS/AUD/SEI prefix)."""
    nals = split_annexb(data)
    aus = []
    cur = AccessUnit()
    pending_prefix = []
    for sc, off, end in nals:
        nal = data[off:end]
        sclen = off - sc
        t = nal_type(nal)
        if IS_VCL(t):
            first = (nal[2] >> 7) & 1
            if first and cur.nals:
                aus.append(cur)
                cur = AccessUnit()
            cur.nals.extend(pending_prefix)
            pending_prefix = []
            cur.slices.append(len(cur.nals))
            cur.nals.append((nal, sclen))
        else:
            # suffix SEI etc. belong to current AU; prefix types to next
            if t in (35, 32, 33, 34, 39):  # AUD, VPS, SPS, PPS, prefix SEI
                pending_prefix.append((nal, sclen))
            else:
                cur.nals.append((nal, sclen))
    if pending_prefix:
        cur.nals.extend(pending_prefix)
    if cur.nals:
        aus.append(cur)
    return aus


def stream_context(data: bytes):
    """Return (sps_map, pps_map) from the stream."""
    sps_map = {}
    pps_map = {}
    for sc, off, end in split_annexb(data):
        nal = data[off:end]
        t = nal_type(nal)
        if t == 33:
            s = parse_sps(nal)
            sps_map[s["sps_id"]] = s
        elif t == 34:
            p = parse_pps(nal)
            pps_map[p["pps_id"]] = p
    return sps_map, pps_map
