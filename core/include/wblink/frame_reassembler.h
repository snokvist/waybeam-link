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

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
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
    uint64_t frames_egress_rejected = 0;  // reconstructed, local sink refused
    // Recovery attribution for successfully delivered frames only. A symbol
    // is counted once, when its block completes; duplicate retransmits do not
    // inflate these counters.
    uint64_t fec_recovered_source_symbols = 0;
    uint64_t arq_recovered_source_symbols = 0;
    uint64_t arq_recovered_repair_symbols = 0;
    uint64_t frames_with_arq = 0;
    uint64_t frames_fec_only = 0;
    uint64_t frames_fec_after_arq = 0;
    uint64_t frames_superseded = 0;
    uint64_t frames_deadline = 0;
    uint64_t frames_unrecoverable = 0;  // finalized with < k, no way to decode
    // §6.3b outcomes. The reassembler itself counts frames_salvaged as "the
    // hook emitted" (salvaged + frozen combined); the node layer overwrites
    // all four from SpatialRepairStats at snapshot time, which splits
    // salvaged (>= 1 surviving slice) from frozen (whole-frame synthesis).
    uint64_t frames_salvaged = 0;
    uint64_t frames_frozen = 0;
    uint64_t salvage_failed = 0;
    uint64_t slices_synthesized = 0;
    uint64_t decode_failures = 0;
    uint64_t malformed = 0;
    uint64_t jscc_shadow_blocks = 0;
    uint16_t jscc_predicted_loss_symbols = 0;
    uint16_t jscc_observed_loss_symbols = 0;
    uint64_t jscc_underpredicted_blocks = 0;
    uint64_t jscc_predicted_parity_symbols = 0;
    uint16_t jscc_predicted_repair_symbols = 0;
    uint16_t jscc_observed_repair_symbols = 0;
    uint64_t jscc_repair_underpredicted_blocks = 0;
    uint64_t jscc_repair_demand_censored_blocks = 0;
    uint64_t jscc_repair_predicted_parity_symbols = 0;
};

struct JsccRepairFeedbackState {
    uint16_t repair_demand_permille = 0;
    uint16_t repair_samples = 0;
    uint32_t observed_block_id = 0;
    bool repair_ready = false;
    bool have_observation = false;
};

// §14.3 view of one open (incomplete, unfinalized) block, consumed by the
// CacheController's close/deficit logic. Bitmaps are index-per-bit over the
// §3.11 spaces: missing_sources bit i => source i absent (i < k);
// have_repairs bit r => repair_idx r held.
struct RepairCandidate {
    uint32_t block_id = 0;
    uint16_t k = 0;
    uint16_t unique = 0;  // distinct symbols held (sources + repairs)
    bool have_eob = false;
    uint64_t first_ms = 0;
    uint64_t last_new_ms = 0;
    uint64_t eob_ms = 0;  // valid iff have_eob
    std::array<uint8_t, 32> missing_sources{};
    std::array<uint8_t, 32> have_repairs{};
};

// §6.3b: view of a block being finalized below k — its verified received
// source chunks (chunk i = blob bytes [i*s, i*s+size)) and known geometry.
// s/frame_len are 0 when no symbol named them. Valid ONLY for the duration
// of the SalvageHook call: the block (and the map sources points into) is
// erased as soon as the hook returns.
struct SalvageView {
    uint32_t block_id = 0;
    uint16_t k = 0;
    uint16_t s = 0;
    uint32_t frame_len = 0;
    const std::map<uint16_t, std::vector<uint8_t>>* sources = nullptr;
};

class FrameReassembler {
  public:
    // emit(frame, len): one whole [VencFrameMeta][Annex-B] blob, valid only
    // during the call. True means the local egress accepted responsibility
    // for it; false is a terminal local drop (§6.3a outcome 7).
    using Emit = std::function<bool(const uint8_t* frame, size_t len)>;

    // §6.3b salvage hook: called when a block finalizes unrecoverable (and no
    // newer block has already emitted). Returns true iff it emitted a
    // repaired frame through the provided Emit. Unset = pre-§6.3b behaviour.
    using SalvageHook = std::function<bool(const SalvageView&, const Emit&)>;

    explicit FrameReassembler(const FrameReassemblerConfig& cfg) : cfg_(cfg) {}

    void set_salvage_hook(SalvageHook hook) { salvage_ = std::move(hook); }

    // Feed one deduped DATA symbol of this stream. is_repair from
    // data_flags & FEC_REPAIR; eob from data_flags & END_OF_BLOCK.
    // Returns true exactly when this symbol completes and attempts to emit the
    // block. A rejected local egress still retires packet-level ARQ because
    // retransmitting an already-complete radio block cannot repair it.
    bool push(uint32_t block_id, uint8_t flags, const uint8_t* payload,
              size_t payload_len, uint64_t now_ms, const Emit& emit,
              bool air_path = true);

    // Drop blocks past their deadline (§8). Call from the RX tick.
    void tick(uint64_t now_ms, const Emit& emit);

    const FrameReassemblerStats& stats() const { return stats_; }
    JsccRepairFeedbackState jscc_feedback() const;

    // §14.3: snapshot every open block with a known k that is still below k
    // unique symbols. Returns the number written (<= cap).
    size_t repair_candidates(RepairCandidate* out, size_t cap) const;

    // §15.5 stats/reset: zero the cumulative counters (in-flight blocks and
    // the finalized watermark are untouched).
    void reset_stats();

    // A sender session namespaces block IDs. Clear the old session's block
    // watermark and in-flight equations before accepting a new one.
    void reset_stream();

  private:
    struct Block {
        uint16_t k = 0;           // symbol count (from any subheader); 0 = unknown
        uint16_t s = 0;           // coded symbol size (from repair, or full chunk)
        uint32_t frame_len = 0;   // total blob length (from repair; 0 = unknown)
        uint64_t first_ms = 0;
        uint64_t last_new_ms = 0;  // §14.3 quiet-timeout anchor
        uint64_t eob_ms = 0;       // §14.3 tail-grace anchor (valid iff have_eob)
        bool have_eob = false;
        bool shadow_armed = false;
        uint16_t shadow_prediction = 0;
        uint16_t repair_prediction = 0;
        // index -> chunk bytes (source i; last chunk may be < s, unpadded).
        std::map<uint16_t, std::vector<uint8_t>> sources;
        // repair_idx -> s coded bytes.
        std::map<uint8_t, std::vector<uint8_t>> repairs;
        // Unique rows first admitted with the RETRANSMIT flag. These are kept
        // separate from packet-sequence recovery so frame completion can
        // attribute the exact source/repair rows that contributed.
        std::set<uint16_t> arq_sources;
        std::set<uint8_t> arq_repairs;
        // Air-only attribution for the §14.2 demand estimator. Cache symbols
        // still complete `sources`/`repairs`, but must remain losses here.
        std::set<uint16_t> air_sources;
        std::set<uint8_t> air_repairs;
    };

    // Try to finalize block b (id): emit if complete, return true if finalized
    // (emitted OR proven unrecoverable) so the caller erases it.
    bool try_complete(uint32_t id, Block& b, const Emit& emit);
    void supersede(uint32_t new_highest, const Emit& emit);
    void finalize(uint32_t id);  // advance the done/dropped watermark
    void observe_shadow(uint32_t id, Block& b);
    enum class SalvageOutcome { kUnavailable, kAccepted, kRejected };
    // §6.3b: last chance for a block finalizing below k. Ordering-guarded so a
    // salvage never emits behind a newer accepted block (§6.3a zero-reorder).
    SalvageOutcome try_salvage(uint32_t id, const Block& b, const Emit& emit);
    void note_emitted(uint32_t id);

    FrameReassemblerConfig cfg_;
    FrameReassemblerStats stats_;
    SalvageHook salvage_;
    std::map<uint32_t, Block> blocks_;
    uint32_t highest_block_ = 0;
    bool have_highest_ = false;
    uint32_t last_emitted_ = 0;
    bool have_emitted_ = false;
    uint32_t finalized_upto_ = 0;   // block_ids <= this are done/dropped
    bool have_finalized_ = false;
    std::vector<uint8_t> scratch_;  // reused decode/concat buffer
    JsccLossEstimator loss_estimator_;
    JsccLossEstimator repair_estimator_{
        JsccLossEstimatorConfig{120, 1000, 20, 100}};
    uint32_t latest_observed_block_ = 0;
    bool have_observed_block_ = false;
};

}  // namespace wblink
