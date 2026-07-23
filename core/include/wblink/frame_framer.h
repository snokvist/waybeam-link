// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: per-stream frame framer for frame-shm ingress
// (PROTOCOL.md §5.1a). Replaces Framer (§5.1) when the ingress binding kind is
// frame-shm: the ingress unit is one whole encoded frame carrying an 8-byte
// VencFrameMeta prefix (§15.4), which FrameFramer fragments into k source
// symbols and (per the §14.1 adaptive policy) r Cauchy-RS repair symbols.
//
// One frame = one block_id (§4). IDR importance is taken directly from the
// metadata flag; optional all-frame P-ARQ is an explicit config mode (§4.1).
// No NAL parsing: the [VencFrameMeta][Annex-B] blob remains opaque.
//
// Pure logic: time injected, emission is a callback, no sockets/clocks. A
// reusable scratch buffer holds the zero-padded source symbols for the repair
// computation (amortised, not per-symbol).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "wblink/table.h"  // FecScheme
#include "wblink/types.h"
#include "wblink/wire.h"

namespace wblink {

// §14.1 per-stream FEC policy (mapped from the config fec block).
struct FrameFecConfig {
    FecScheme scheme = FecScheme::kNone;
    uint16_t i_rate_permille = 250;  // IDR repair overhead
    uint16_t p_rate_permille = 100;  // P-frame repair overhead
    uint16_t min_k = 3;              // k <= min_k => ARQ-only (r = 0)
};

struct FrameFramerConfig {
    uint16_t originator = 0;
    uint32_t session_id = 0;
    uint8_t stream_id = 0;
    uint8_t stream_type = stream_type::kRtp;
    uint16_t destination = 0;  // §3.1 advisory; 0 = broadcast
    FrameArqMode arq_mode = FrameArqMode::kIdrOnly;
    FrameFecConfig fec;
};

struct FrameFramerStats {
    uint64_t frames = 0;
    uint64_t source_symbols = 0;
    uint64_t repair_symbols = 0;
    uint64_t malformed_frame = 0;   // < 8 B (no VencFrameMeta prefix): dropped
    uint64_t fec_oversize_k = 0;    // k + r_target > 256 => FEC disabled (§14.1)
    uint64_t idr_frames = 0;
    uint64_t arq_frames = 0;
    uint64_t arq_cutoff_frames = 0;  // §4.1 Pass 40 high-cadence suppression
};

class FrameFramer {
  public:
    // emit(frame, frame_len, hdr, now_ms): frame is valid only during the call;
    // hdr carries the stamped fields for resend-ring bookkeeping (§5.2). Same
    // contract as Framer::Emit.
    using Emit = std::function<void(const uint8_t* frame, size_t frame_len,
                                    const DataHeader& hdr, uint64_t now_ms)>;

    explicit FrameFramer(const FrameFramerConfig& cfg) : cfg_(cfg) {}

    // §9/§3.2: operating point stamped on every packet + the air MTU budget
    // (max_payload) that sizes source/repair symbols (§5.1a). Static until the
    // §9 adaptive selector drives it.
    void set_operating_point(uint8_t active_profile, uint8_t table_version,
                             uint16_t max_payload) {
        active_profile_ = active_profile;
        table_version_ = table_version;
        max_payload_ = max_payload;
    }

    // §11.6 CSA_ARMED etc. — OR'd into every outgoing DATA header while set.
    void set_extra_flags(uint8_t f) { extra_flags_ = f; }

    // §14.1 live FEC-rate retune (control plane §15.5). The scheme is fixed at
    // construction (rlc256 vs none is structural); only the per-mille repair
    // overheads and the ARQ-only threshold move. Effective on the next frame.
    void set_fec_rates(uint16_t i_permille, uint16_t p_permille,
                       uint16_t min_k) {
        cfg_.fec.i_rate_permille = i_permille;
        cfg_.fec.p_rate_permille = p_permille;
        cfg_.fec.min_k = min_k;
    }
    const FrameFecConfig& fec() const { return cfg_.fec; }
    FrameArqMode arq_mode() const { return cfg_.arq_mode; }

    // §15.5 stats/reset: zero the cumulative counters (fresh measurement
    // window). State (seq, block id, operating point) is untouched.
    void reset_stats() { stats_ = {}; }

    // Fragment + FEC one whole frame blob ([VencFrameMeta][Annex-B]). Returns
    // false iff the frame was dropped (malformed / empty).
    bool on_frame(const uint8_t* blob, size_t len, uint64_t now_ms,
                  const Emit& emit);

    const FrameFramerStats& stats() const { return stats_; }
    uint32_t next_seq() const { return next_seq_; }
    uint32_t current_block() const { return block_id_; }

    // Source-symbol size s for the active MTU (§5.1a): max_payload - 26 - 11.
    uint16_t symbol_size() const;

    // §14.2 enforcement (Pass 38): one-shot override consumed by the NEXT
    // on_frame. parity_symbols replaces the fixed §14.1 rate (GF(256)- and
    // min_k-clamped); allow_pframe_arq=false clears PFRAME_ARQ stamping for
    // that frame only. The IDR ARQ bit is never affected.
    void set_next_frame_override(uint16_t parity_symbols,
                                 bool allow_pframe_arq) {
        override_parity_ = parity_symbols;
        override_allow_parq_ = allow_pframe_arq;
    }

    // §4.1 Pass 40 high-cadence ARQ cutoff: while set, frames are stamped
    // with neither ARQ nor PFRAME_ARQ (counted in arq_cutoff_frames).
    // Sticky, driven from the TX cadence estimate each tick.
    void set_arq_suppressed(bool on) { arq_suppressed_ = on; }

    // §11.7 ARQ command — an independent cause from the Pass 40 cutoff so
    // off/on composes with (never clears) the cadence suppression. Off ⇒
    // neither ARQ nor PFRAME_ARQ is stamped; on restores boot behaviour.
    void set_arq_enabled(bool on) { arq_enabled_ = on; }

  private:
    // r for a frame of k symbols per the §14.1 adaptive policy; 0 if FEC off,
    // ARQ-only (k <= min_k), or the k+r>256 cap trips (records fec_oversize_k).
    uint16_t repair_count(uint16_t k, bool is_idr);

    FrameFramerConfig cfg_;
    FrameFramerStats stats_;
    uint8_t active_profile_ = 0;
    uint8_t table_version_ = 0;
    uint8_t extra_flags_ = 0;
    uint16_t max_payload_ = kDefaultMaxPayload;

    uint32_t next_seq_ = 0;
    uint32_t block_id_ = 0;
    std::optional<uint16_t> override_parity_;  // §14.2 one-shot (Pass 38)
    bool override_allow_parq_ = true;
    bool arq_suppressed_ = false;  // §4.1 Pass 40 (sticky)
    bool arq_enabled_ = true;      // §11.7 ARQ command

    // Reusable scratch (amortised across frames): zero-padded source symbols
    // (k*s) for the repair computation, and one encode buffer.
    std::vector<uint8_t> src_pad_;
    std::vector<const uint8_t*> src_ptrs_;
    std::vector<uint8_t> src_payload_;     // 4-B source subheader + chunk
    std::vector<uint8_t> repair_payload_;  // 11-B subheader + s coded bytes
    uint8_t frame_buf_[kDataHeaderSize + kMaxDataPayload];
};

}  // namespace wblink
