// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: RX-side frame reassembler for frame-shm egress
// (PROTOCOL.md §6.3a). Collects a block's source + repair symbols (fed
// deduped by the RX engine, §6.1) and emits ONE whole [VencFrameMeta][Annex-B]
// blob per block — byte-identical to the producer's slot (§15.4) — via three
// outcomes (§5.3):
//
//   1. all k source symbols present  -> concatenate, no FEC decode (fast path)
//   2. >= k total symbols (src+repair) -> GF(256) Cauchy-RS decode (§14.1)
//   3. < k after deadline / superseded -> frame lost, nothing emitted
//
// Symbols self-describe (source subheader = k,index §5.1a; repair subheader =
// repair_idx,k,base_seq,frame_len §14.1), so reassembly never infers counts
// from seq gaps. Pure logic: time injected, emission is a callback, no I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "wblink/types.h"
#include "wblink/jscc_loss_estimator.h"

namespace wblink {

struct FrameReassemblerConfig {
    uint32_t deadline_ms = 50;       // §8 per-block budget; drop if unmet
    uint32_t max_blocks_ahead = 0;   // §6.3a: newer block supersedes all older
    uint32_t max_frame_bytes = 640 * 1024;  // sanity cap (venc slot is 512 KB)
};

struct FrameReassemblerStats {
    uint64_t frames_delivered = 0;
    uint64_t frames_fast = 0;      // all-sources, no decode
    uint64_t frames_fec = 0;       // recovered via FEC
    uint64_t frames_superseded = 0;
    uint64_t frames_deadline = 0;
    uint64_t frames_unrecoverable = 0;  // finalized with < k, no way to decode
    uint64_t decode_failures = 0;
    uint64_t malformed = 0;
    uint64_t jscc_shadow_blocks = 0;
    uint16_t jscc_predicted_loss_symbols = 0;
    uint16_t jscc_observed_loss_symbols = 0;
    uint64_t jscc_underpredicted_blocks = 0;
    uint64_t jscc_predicted_parity_symbols = 0;
};

class FrameReassembler {
  public:
    // emit(frame, len): one whole [VencFrameMeta][Annex-B] blob, valid only
    // during the call. The caller writes it to the frame-shm egress ring.
    using Emit = std::function<void(const uint8_t* frame, size_t len)>;

    explicit FrameReassembler(const FrameReassemblerConfig& cfg) : cfg_(cfg) {}

    // Feed one deduped DATA symbol of this stream. is_repair from
    // data_flags & FEC_REPAIR; eob from data_flags & END_OF_BLOCK.
    void push(uint32_t block_id, uint8_t flags, const uint8_t* payload,
              size_t payload_len, uint64_t now_ms, const Emit& emit);

    // Drop blocks past their deadline (§8). Call from the RX tick.
    void tick(uint64_t now_ms, const Emit& emit);

    const FrameReassemblerStats& stats() const { return stats_; }

    // §15.5 stats/reset: zero the cumulative counters (in-flight blocks and
    // the finalized watermark are untouched).
    void reset_stats();

  private:
    struct Block {
        uint16_t k = 0;           // symbol count (from any subheader); 0 = unknown
        uint16_t s = 0;           // coded symbol size (from repair, or full chunk)
        uint32_t frame_len = 0;   // total blob length (from repair; 0 = unknown)
        uint64_t first_ms = 0;
        bool have_eob = false;
        bool shadow_armed = false;
        uint16_t shadow_prediction = 0;
        // index -> chunk bytes (source i; last chunk may be < s, unpadded).
        std::map<uint16_t, std::vector<uint8_t>> sources;
        // repair_idx -> s coded bytes.
        std::map<uint8_t, std::vector<uint8_t>> repairs;
    };

    // Try to finalize block b (id): emit if complete, return true if finalized
    // (emitted OR proven unrecoverable) so the caller erases it.
    bool try_complete(uint32_t id, Block& b, const Emit& emit);
    void supersede(uint32_t new_highest, const Emit& emit);
    void finalize(uint32_t id);  // advance the done/dropped watermark
    void observe_shadow(Block& b);

    FrameReassemblerConfig cfg_;
    FrameReassemblerStats stats_;
    std::map<uint32_t, Block> blocks_;
    uint32_t highest_block_ = 0;
    bool have_highest_ = false;
    uint32_t finalized_upto_ = 0;   // block_ids <= this are done/dropped
    bool have_finalized_ = false;
    std::vector<uint8_t> scratch_;  // reused decode/concat buffer
    JsccLossEstimator loss_estimator_;
};

}  // namespace wblink
