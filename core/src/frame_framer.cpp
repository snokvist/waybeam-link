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

uint16_t FrameFramer::class_rate(FrameFecClass cls) const {
    switch (cls) {
        case FrameFecClass::kIdr:
            return cfg_.fec.i_rate_permille;
        // §14.1a: UNSET inherits the P rate, so a config predating Pass 149
        // is bit-for-bit unchanged. Not a 0 default — see the header.
        case FrameFecClass::kEnhance:
            return cfg_.fec.e_rate_permille.value_or(cfg_.fec.p_rate_permille);
        case FrameFecClass::kP:
            break;
    }
    return cfg_.fec.p_rate_permille;
}

uint16_t FrameFramer::repair_count(uint16_t k, FrameFecClass cls) {
    // §14.1 adaptive policy. Pass 205 (O1): the min_k ARQ-only gate is gone —
    // every referenced frame gets max(ceil(k·rate), min_r), so small frames
    // are no longer shipped bare on the strength of a removed ARQ.
    if (cfg_.fec.scheme != FecScheme::kRlc256) {
        return 0;
    }
    const uint32_t rate = class_rate(cls);
    if (rate == 0) {
        return 0;  // e_rate=0 / fec off: deliberately bare
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
    override_parity_.reset();
    if (blob == nullptr || len < kVencFrameMetaSize) {
        ++stats_.malformed_frame;  // no VencFrameMeta prefix — drop, never send
        return false;
    }

    const uint16_t s = symbol_size();
    const size_t k_sz = 1u + (len - 1u) / s;
    if (k_sz > 0xFFFFu) {  // window_len is u16 (§14.1) — unreachable at sane MTU
        ++stats_.malformed_frame;
        return false;
    }
    const uint16_t k = static_cast<uint16_t>(k_sz);

    const uint16_t default_symbol = std::min<uint16_t>(
        s, static_cast<uint16_t>(kDefaultMaxPayload - kDataHeaderSize -
                                 kFecRepairSubheaderSize));
    const size_t default_k = 1u + (len - 1u) / default_symbol;
    const bool jumbo_fec_guard =
        cfg_.fec.scheme == FecScheme::kRlc256 &&
        s > default_symbol && k < mtu_tier::kFecProtectionK &&
        default_k >= mtu_tier::kFecProtectionK;

    VencFrameMeta meta;
    read_frame_meta(blob, len, &meta);
    const bool is_idr = (meta.flags & kFrameFlagIdr) != 0;
    // §14.1a priority order IDR -> enhance -> P. No producer marks an IDR
    // non-referenced, but do not rely on it: resolve so IDR wins if one ever
    // does — more protection, never less.
    const bool is_enhance = !is_idr && (meta.flags & kFrameFlagEnhance) != 0;
    const FrameFecClass cls = is_idr      ? FrameFecClass::kIdr
                              : is_enhance ? FrameFecClass::kEnhance
                                           : FrameFecClass::kP;

    const uint32_t block_id = block_id_++;
    const uint32_t base_seq = next_seq_;
    // §3.2 (Pass 205): no ARQ flag is ever stamped; extra_flags_ carries only
    // §11.6 CSA_ARMED.
    const uint8_t base_flags = extra_flags_;

    uint16_t r = repair_count(k, cls);
    // §14.2 rule 1: a valid enforced decision replaces the fixed rate, still
    // GF(256)-clamped. §14.2 (Pass 149): kEnhance is exempt from enforcement
    // ENTIRELY — e_rate exists to leave those frames alone.
    if (ov_parity && !is_enhance && cfg_.fec.scheme == FecScheme::kRlc256) {
        const uint32_t cap =
            k < kFecMaxSymbols ? kFecMaxSymbols - k : 0;
        r = static_cast<uint16_t>(std::min<uint32_t>(*ov_parity, cap));
    }
    // §9.3a Pass 124: match the configured erasure depth at k=16 without
    // refragmenting jumbo frames into extra source packets. Applied after a
    // §14.2 override so the robustness floor cannot be silently bypassed.
    if (jumbo_fec_guard) {
        const uint32_t rate = class_rate(cls);
        const uint32_t cap = kFecMaxSymbols - k;
        const uint16_t guard_r = static_cast<uint16_t>(std::max<uint32_t>(
            cfg_.fec.min_r,
            (mtu_tier::kFecProtectionK * rate + 999u) / 1000u));
        // The guard is subordinate to §14.1's absolute GF(256) capacity check:
        // do not resurrect a block repair_count() rejected as impossible.
        if (rate != 0 && guard_r <= cap && r < guard_r) {
            r = guard_r;
            ++stats_.mtu_fec_guard_frames;
        }
    }

    ++stats_.frames;
    if (is_idr) {
        ++stats_.idr_frames;
    }
    if (is_enhance) {
        ++stats_.fec_enhance_frames;  // §14.1a observed droppable density
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
