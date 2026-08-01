// SPDX-License-Identifier: GPL-2.0-or-later
// FrameFramer (PROTOCOL.md §5.1a): fragment a whole frame blob into k source
// symbols + r Cauchy-RS repair symbols (§14.1).
#include "wblink/frame_framer.h"

#include <algorithm>
#include <cstring>

#include "wblink/endian.h"
#include "wblink/frame_shm_format.h"
#include "wblink/rlc.h"

namespace wblink {

uint16_t FrameFramer::symbol_size() const {
    // §5.1a: s = max_payload - 26 (header) - 11 (repair subheader), so source
    // and repair symbols are the same size and both fit one MPDU. Config
    // guarantees max_payload >= kDataHeaderSize + 32, so s >= 21.
    const int s = static_cast<int>(effective_packet_budget()) -
                  static_cast<int>(kDataHeaderSize) -
                  static_cast<int>(kFecRepairSubheaderSize);
    return s > 0 ? static_cast<uint16_t>(s) : 1;
}

uint16_t FrameFramer::repair_count(uint16_t k, bool is_idr, bool arq_eligible) {
    // §14.1 adaptive policy.
    if (cfg_.fec.scheme != FecScheme::kRlc256) {
        return 0;
    }
    // §14.1 (Pass 94): the min_k gate is an OPTIMISATION — don't spend parity
    // where ARQ will recover the frame anyway — so it holds only where that
    // ARQ exists. Under arq_mode idr-only a P-frame has none, and an
    // unconditional gate handed it neither FEC nor ARQ. That was B11: at the
    // §9.8 floor rung, derived_bitrate/fps lands frames under min_k*s and they
    // shipped bare. Above the §4.1 cadence cutoff nothing is eligible and the
    // gate is inert for every class.
    if (k <= cfg_.fec.min_k && arq_eligible) {
        return 0;  // ARQ-only at small k
    }
    const uint32_t rate =
        is_idr ? cfg_.fec.i_rate_permille : cfg_.fec.p_rate_permille;
    if (rate == 0) {
        return 0;
    }
    uint32_t r = (static_cast<uint32_t>(k) * rate + 999u) / 1000u;  // ceil
    // §14.1 (Pass 98) minimum repair floor: never fewer than min_r symbols on
    // a FEC'd frame, so a small frame is not left one loss from death. Never
    // lowers the rate-derived r (large frames keep ceil(k·rate)).
    if (r < cfg_.fec.min_r) {
        r = cfg_.fec.min_r;
    }
    if (r == 0) {
        return 0;
    }
    // GF(256) capacity: k + r <= 256, else FEC off for this frame (§14.1).
    if (static_cast<uint32_t>(k) + r > kFecMaxSymbols) {
        ++stats_.fec_oversize_k;
        return 0;
    }
    return static_cast<uint16_t>(r);
}

bool FrameFramer::on_frame(const uint8_t* blob, size_t len, uint64_t now_ms,
                           const Emit& emit) {
    // §14.2 enforcement (Pass 38): the override is one-shot — consumed (and
    // cleared) by this frame regardless of outcome.
    const std::optional<uint16_t> ov_parity = override_parity_;
    const bool ov_allow_parq = override_allow_parq_;
    override_parity_.reset();
    override_allow_parq_ = true;
    if (blob == nullptr || len < kVencFrameMetaSize) {
        ++stats_.malformed_frame;  // no VencFrameMeta prefix — drop, never send
        return false;
    }

    const uint16_t s = symbol_size();
    size_t k_sz = (len + s - 1) / s;
    if (k_sz == 0) {
        k_sz = 1;
    }
    if (k_sz > 0xFFFFu) {  // window_len is u16 (§14.1) — unreachable at sane MTU
        ++stats_.malformed_frame;
        return false;
    }
    const uint16_t k = static_cast<uint16_t>(k_sz);

    VencFrameMeta meta;
    read_frame_meta(blob, len, &meta);
    const bool is_idr = (meta.flags & kFrameFlagIdr) != 0;

    const uint32_t block_id = block_id_++;
    const uint32_t base_seq = next_seq_;
    // §4.1 Pass 40: above the cadence cutoff nothing is ARQ-class; §14.2
    // rule 3: a valid enforced decision may additionally clear PFRAME_ARQ
    // for this frame. The IDR ARQ bit is only ever removed by the cutoff.
    const bool arq_class =
        is_idr || cfg_.arq_mode == FrameArqMode::kAllFrames;
    const bool idr_arq = is_idr && !arq_suppressed_ && arq_enabled_;
    const bool pframe_arq = !is_idr &&
                            cfg_.arq_mode == FrameArqMode::kAllFrames &&
                            ov_allow_parq && !arq_suppressed_ && arq_enabled_;
    const uint8_t base_flags = static_cast<uint8_t>(
        (idr_arq ? data_flags::kArq
                 : (pframe_arq ? data_flags::kPframeArq : 0)) |
        extra_flags_);

    uint16_t r = repair_count(k, is_idr, idr_arq || pframe_arq);
    // §14.2 rule 1: a valid enforced decision replaces the fixed rate, still
    // GF(256)-clamped and still subject to the min_k ARQ-only rule.
    // §14.2 rule 1 keeps the override "subject to the §14.1 min_k ARQ-only
    // rule", so it inherits Pass 94's condition: the gate blocks the override
    // only where ARQ could have carried the frame instead.
    if (ov_parity && cfg_.fec.scheme == FecScheme::kRlc256 &&
        (k > cfg_.fec.min_k || !(idr_arq || pframe_arq))) {
        const uint32_t cap =
            k < kFecMaxSymbols ? kFecMaxSymbols - k : 0;
        r = static_cast<uint16_t>(std::min<uint32_t>(*ov_parity, cap));
    }

    ++stats_.frames;
    if (is_idr) {
        ++stats_.idr_frames;
    }
    if (idr_arq || pframe_arq) {
        ++stats_.arq_frames;
    }
    if (arq_suppressed_ && arq_class) {
        ++stats_.arq_cutoff_frames;
    }

    // --- source symbols: k DATA packets, tail unpadded (§5.1a). EOB closes
    // the whole FEC block, so with parity it moves to the final repair row.
    // Payload = [4-B source subheader (k, i)][chunk].
    src_payload_.resize(kFecSourceSubheaderSize + s);
    for (uint16_t i = 0; i < k; ++i) {
        const size_t off = static_cast<size_t>(i) * s;
        const size_t chunk = std::min<size_t>(s, len - off);
        be16_write(src_payload_.data() + kFecSrcOffWindowLen, k);
        be16_write(src_payload_.data() + kFecSrcOffSymIndex, i);
        std::memcpy(src_payload_.data() + kFecSourceSubheaderSize, blob + off, chunk);
        const size_t plen = kFecSourceSubheaderSize + chunk;
        DataHeader hdr;
        hdr.prefix.originator = cfg_.originator;
        hdr.prefix.destination = cfg_.destination;
        hdr.prefix.session_id = cfg_.session_id;
        hdr.stream_id = cfg_.stream_id;
        hdr.stream_type = cfg_.stream_type;
        hdr.seq = next_seq_++;
        hdr.block_id = block_id;
        hdr.data_flags = static_cast<uint8_t>(
            base_flags |
            (r == 0 && i == k - 1 ? data_flags::kEndOfBlock : 0));
        hdr.active_profile = active_profile_;
        hdr.table_version = table_version_;
        const size_t fl = encode_data(hdr, src_payload_.data(),
                                      static_cast<uint16_t>(plen), frame_buf_,
                                      sizeof(frame_buf_));
        if (fl > 0) {
            emit(frame_buf_, fl, hdr, now_ms);
            ++stats_.source_symbols;
        }
    }

    // --- repair symbols (§14.1), source-first already done above ---
    if (r == 0) {
        return true;
    }

    // Zero-padded source symbols for the Cauchy computation; the padding is
    // never on the wire (RX re-pads a FEC-recovered last symbol via frame_len).
    src_pad_.assign(static_cast<size_t>(k) * s, 0);
    std::memcpy(src_pad_.data(), blob, len);
    src_ptrs_.resize(k);
    for (uint16_t i = 0; i < k; ++i) {
        src_ptrs_[i] = src_pad_.data() + static_cast<size_t>(i) * s;
    }

    repair_payload_.assign(kFecRepairSubheaderSize + s, 0);
    for (uint16_t j = 0; j < r; ++j) {
        uint8_t* sub = repair_payload_.data();
        sub[kFecOffRepairIdx] = static_cast<uint8_t>(j);
        be16_write(sub + kFecOffWindowLen, k);
        be32_write(sub + kFecOffWindowBaseSeq, base_seq);
        be32_write(sub + kFecOffFrameLen, static_cast<uint32_t>(len));
        rlc_encode_repair(k, static_cast<uint8_t>(j), src_ptrs_.data(), s,
                          sub + kFecRepairSubheaderSize);

        DataHeader hdr;
        hdr.prefix.originator = cfg_.originator;
        hdr.prefix.destination = cfg_.destination;
        hdr.prefix.session_id = cfg_.session_id;
        hdr.stream_id = cfg_.stream_id;
        hdr.stream_type = cfg_.stream_type;
        hdr.seq = next_seq_++;
        hdr.block_id = block_id;
        hdr.data_flags = static_cast<uint8_t>(
            base_flags | data_flags::kFecRepair |
            (j == r - 1 ? data_flags::kEndOfBlock : 0));
        hdr.active_profile = active_profile_;
        hdr.table_version = table_version_;
        const size_t fl =
            encode_data(hdr, repair_payload_.data(),
                        static_cast<uint16_t>(repair_payload_.size()),
                        frame_buf_, sizeof(frame_buf_));
        if (fl > 0) {
            emit(frame_buf_, fl, hdr, now_ms);
            ++stats_.repair_symbols;
        }
    }
    return true;
}

}  // namespace wblink
