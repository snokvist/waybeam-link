// SPDX-License-Identifier: GPL-2.0-or-later
// FrameReassembler (PROTOCOL.md §6.3a): collect a block's source + repair
// symbols and emit one whole frame blob (fast path / FEC decode / drop).
#include "wblink/frame_reassembler.h"

#include <algorithm>
#include <cstring>

#include "wblink/endian.h"
#include "wblink/frame_shm_format.h"
#include "wblink/rlc.h"

namespace wblink {

namespace {
// Wrap-safe "is a strictly after b" for monotonic u32 block ids.
inline int32_t bid_diff(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b);
}
}  // namespace

void FrameReassembler::push(uint32_t block_id, uint8_t flags,
                            const uint8_t* payload, size_t payload_len,
                            uint64_t now_ms, const Emit& emit) {
    // Drop late/duplicate symbols for already-finalized blocks.
    if (have_finalized_ && bid_diff(block_id, finalized_upto_) <= 0) {
        return;
    }

    const bool is_repair = (flags & data_flags::kFecRepair) != 0;
    const bool eob = (flags & data_flags::kEndOfBlock) != 0;

    auto it = blocks_.find(block_id);
    const bool is_new = (it == blocks_.end());
    if (is_new) {
        it = blocks_.emplace(block_id, Block{}).first;
        it->second.first_ms = now_ms;
    }
    Block& b = it->second;

    if (is_repair) {
        if (payload_len <= kFecRepairSubheaderSize) {
            ++stats_.malformed;
            return;
        }
        const uint16_t k = be16_read(payload + kFecOffWindowLen);
        const uint32_t flen = be32_read(payload + kFecOffFrameLen);
        const uint8_t ridx = payload[kFecOffRepairIdx];
        const size_t coded = payload_len - kFecRepairSubheaderSize;  // = s
        if (k == 0 || k > kFecMaxSymbols || flen < kVencFrameMetaSize ||
            flen > cfg_.max_frame_bytes || coded == 0 ||
            coded > kMaxDataPayload ||
            static_cast<uint32_t>(k) + ridx >= kFecMaxSymbols) {
            ++stats_.malformed;
            return;
        }
        if ((b.k != 0 && b.k != k) ||
            (b.frame_len != 0 && b.frame_len != flen) ||
            (b.s != 0 && b.s != coded)) {
            ++stats_.malformed;
            return;
        }
        b.k = k;
        b.frame_len = flen;
        b.s = static_cast<uint16_t>(coded);
        if (b.repairs.emplace(ridx, std::vector<uint8_t>(
                                        payload + kFecRepairSubheaderSize,
                                        payload + payload_len))
                .second) {  // dup: no-op
            b.last_new_ms = now_ms;  // §14.3 quiet-timeout anchor
        }
    } else {
        if (payload_len < kFecSourceSubheaderSize) {
            ++stats_.malformed;
            return;
        }
        const uint16_t k = be16_read(payload + kFecSrcOffWindowLen);
        const uint16_t idx = be16_read(payload + kFecSrcOffSymIndex);
        if (k == 0 || k > kFecMaxSymbols || idx >= k ||
            (b.k != 0 && b.k != k)) {
            ++stats_.malformed;
            return;
        }
        b.k = k;
        const size_t clen = payload_len - kFecSourceSubheaderSize;
        if (clen == 0 || clen > kMaxDataPayload ||
            (b.s != 0 && ((idx != k - 1 && clen != b.s) ||
                          (idx == k - 1 && clen > b.s)))) {
            ++stats_.malformed;
            return;
        }
        // A non-last source symbol carries the full coded size s (§5.1a).
        if (idx != k - 1 && b.s == 0) {
            b.s = static_cast<uint16_t>(clen);
        }
        if (eob && !b.have_eob) {
            b.have_eob = true;
            b.eob_ms = now_ms;  // §14.3 tail-grace anchor
        }
        if (b.sources.emplace(idx, std::vector<uint8_t>(
                                       payload + kFecSourceSubheaderSize,
                                       payload + payload_len))
                .second) {  // dup: no-op
            b.last_new_ms = now_ms;
        }
    }

    // §6.2 supersession: newer block advances the window.
    if (!have_highest_ || bid_diff(block_id, highest_block_) > 0) {
        highest_block_ = block_id;
        have_highest_ = true;
        supersede(highest_block_, emit);
        it = blocks_.find(block_id);  // supersede() never drops the newest
    }

    if (it != blocks_.end() && !it->second.shadow_armed) {
        it->second.shadow_prediction = static_cast<uint16_t>(
            std::min<uint32_t>(loss_estimator_.predict(), UINT16_MAX));
        const uint32_t rate = repair_estimator_.predict();
        it->second.repair_prediction = static_cast<uint16_t>(
            std::min<uint64_t>((static_cast<uint64_t>(rate) * it->second.k +
                                999) / 1000, UINT16_MAX));
        it->second.shadow_armed = true;
    }

    if (it != blocks_.end() && try_complete(block_id, it->second, emit)) {
        blocks_.erase(it);
    }
}

bool FrameReassembler::try_complete(uint32_t id, Block& b, const Emit& emit) {
    if (b.k == 0) {
        return false;  // no subheader parsed yet
    }
    const uint16_t k = b.k;

    // (1) Fast path — all k source symbols present: concatenate, no decode.
    if (b.sources.size() == k) {
        scratch_.clear();
        for (const auto& kv : b.sources) {  // ordered 0..k-1
            scratch_.insert(scratch_.end(), kv.second.begin(), kv.second.end());
        }
        emit(scratch_.data(), scratch_.size());
        observe_shadow(id, b);
        ++stats_.frames_delivered;
        ++stats_.frames_fast;
        finalize(id);
        return true;
    }

    // (2) FEC path — >= k total symbols and s / frame_len known: GF(256) decode.
    if (static_cast<uint16_t>(b.sources.size() + b.repairs.size()) >= k &&
        b.s > 0 && b.frame_len > 0 && b.frame_len <= cfg_.max_frame_bytes) {
        const uint16_t s = b.s;
        RlcDecoder dec(k, s);
        std::vector<uint8_t> pad(s);
        for (const auto& kv : b.sources) {
            std::fill(pad.begin(), pad.end(), uint8_t{0});
            std::memcpy(pad.data(), kv.second.data(),
                        std::min<size_t>(kv.second.size(), s));
            dec.add_source(kv.first, pad.data());
        }
        for (const auto& kv : b.repairs) {
            if (kv.second.size() >= s) {
                dec.add_repair(kv.first, kv.second.data());
            }
        }
        if (dec.can_decode()) {
            scratch_.assign(static_cast<size_t>(k) * s, 0);
            if (dec.decode(scratch_.data())) {
                scratch_.resize(b.frame_len);  // trim last-symbol padding
                emit(scratch_.data(), scratch_.size());
                observe_shadow(id, b);
                ++stats_.frames_delivered;
                ++stats_.frames_fec;
                finalize(id);
                return true;
            }
            ++stats_.decode_failures;
        }
    }
    return false;
}

void FrameReassembler::supersede(uint32_t new_highest, const Emit& /*emit*/) {
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        // Blocks still in the map are incomplete (completed ones are erased).
        if (bid_diff(new_highest, it->first) >
            static_cast<int32_t>(cfg_.max_blocks_ahead)) {
            const Block& b = it->second;
            if (b.k == 0 || b.sources.size() + b.repairs.size() < b.k) {
                ++stats_.frames_unrecoverable;
            }
            ++stats_.frames_superseded;
            observe_shadow(it->first, it->second);
            finalize(it->first);
            it = blocks_.erase(it);
        } else {
            ++it;
        }
    }
}

void FrameReassembler::tick(uint64_t now_ms, const Emit& /*emit*/) {
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        if (now_ms >= it->second.first_ms + cfg_.deadline_ms) {
            const Block& b = it->second;
            if (b.k == 0 || b.sources.size() + b.repairs.size() < b.k) {
                ++stats_.frames_unrecoverable;
            }
            ++stats_.frames_deadline;
            observe_shadow(it->first, it->second);
            finalize(it->first);
            it = blocks_.erase(it);
        } else {
            ++it;
        }
    }
}

void FrameReassembler::observe_shadow(uint32_t id, Block& b) {
    if (!b.shadow_armed || b.k == 0) {
        return;
    }
    const uint16_t observed = static_cast<uint16_t>(
        b.k - std::min<size_t>(b.sources.size(), b.k));
    const uint16_t repairs_seen = static_cast<uint16_t>(b.repairs.size());
    const uint16_t emitted_so_far = b.repairs.empty()
        ? 0 : static_cast<uint16_t>(b.repairs.rbegin()->first + 1);
    const bool demand_censored = repairs_seen < observed;
    const uint16_t repair_demand = demand_censored
        ? static_cast<uint16_t>(std::min<uint32_t>(
              UINT16_MAX, emitted_so_far + observed - repairs_seen))
        : (observed == 0 ? 0 : emitted_so_far);
    stats_.jscc_predicted_loss_symbols = b.shadow_prediction;
    stats_.jscc_observed_loss_symbols = observed;
    stats_.jscc_underpredicted_blocks += b.shadow_prediction < observed;
    stats_.jscc_predicted_parity_symbols += b.shadow_prediction;
    stats_.jscc_predicted_repair_symbols = b.repair_prediction;
    stats_.jscc_observed_repair_symbols = repair_demand;
    stats_.jscc_repair_underpredicted_blocks +=
        b.repair_prediction < repair_demand;
    stats_.jscc_repair_demand_censored_blocks += demand_censored;
    stats_.jscc_repair_predicted_parity_symbols += b.repair_prediction;
    ++stats_.jscc_shadow_blocks;
    loss_estimator_.observe(observed);
    const uint32_t demand_rate = b.k == 0
        ? 0 : (static_cast<uint32_t>(repair_demand) * 1000 + b.k - 1) / b.k;
    repair_estimator_.observe(demand_rate);
    latest_observed_block_ = id;
    have_observed_block_ = true;
    b.shadow_armed = false;
}

size_t FrameReassembler::repair_candidates(RepairCandidate* out,
                                           size_t cap) const {
    size_t n = 0;
    for (const auto& [id, b] : blocks_) {
        if (n >= cap) {
            break;
        }
        // Only blocks whose k is known and that still sit below k unique
        // symbols are candidates (§14.3 local-collection close applies to an
        // incomplete MERGED block; complete blocks were erased on emit).
        const size_t unique = b.sources.size() + b.repairs.size();
        if (b.k == 0 || b.k > kFecMaxSymbols || unique >= b.k) {
            continue;
        }
        RepairCandidate& c = out[n++];
        c = RepairCandidate{};
        c.block_id = id;
        c.k = b.k;
        c.unique = static_cast<uint16_t>(unique);
        c.have_eob = b.have_eob;
        c.first_ms = b.first_ms;
        c.last_new_ms = b.last_new_ms;
        c.eob_ms = b.eob_ms;
        for (uint16_t i = 0; i < b.k; ++i) {
            if (b.sources.find(i) == b.sources.end()) {
                c.missing_sources[i / 8] |=
                    static_cast<uint8_t>(1u << (i % 8));
            }
        }
        for (const auto& kv : b.repairs) {
            c.have_repairs[kv.first / 8] |=
                static_cast<uint8_t>(1u << (kv.first % 8));
        }
    }
    return n;
}

JsccRepairFeedbackState FrameReassembler::jscc_feedback() const {
    JsccRepairFeedbackState out;
    out.repair_demand_permille = static_cast<uint16_t>(
        std::min<uint32_t>(repair_estimator_.predict(), UINT16_MAX));
    out.repair_samples = static_cast<uint16_t>(
        std::min<size_t>(repair_estimator_.sample_count(), UINT16_MAX));
    out.observed_block_id = latest_observed_block_;
    out.repair_ready = repair_estimator_.sample_count() >= 20;
    out.have_observation = have_observed_block_;
    return out;
}

void FrameReassembler::reset_stats() {
    stats_ = {};
    loss_estimator_.reset();
    repair_estimator_.reset();
    latest_observed_block_ = 0;
    have_observed_block_ = false;
    for (auto& [id, block] : blocks_) {
        (void)id;
        block.shadow_prediction = 0;
        block.repair_prediction = static_cast<uint16_t>(
            (100u * block.k + 999u) / 1000u);
        block.shadow_armed = true;
    }
}

void FrameReassembler::finalize(uint32_t id) {
    if (!have_finalized_ || bid_diff(id, finalized_upto_) > 0) {
        finalized_upto_ = id;
        have_finalized_ = true;
    }
}

}  // namespace wblink
