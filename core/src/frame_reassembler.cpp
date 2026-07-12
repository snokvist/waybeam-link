// SPDX-License-Identifier: GPL-2.0-or-later
// FrameReassembler (PROTOCOL.md §6.3a): collect a block's source + repair
// symbols and emit one whole frame blob (fast path / FEC decode / drop).
#include "wblink/frame_reassembler.h"

#include <algorithm>
#include <cstring>

#include "wblink/endian.h"
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
        if (k == 0 || flen == 0 || flen > cfg_.max_frame_bytes) {
            ++stats_.malformed;
            return;
        }
        b.k = k;
        b.frame_len = flen;
        b.s = static_cast<uint16_t>(coded);
        b.repairs.emplace(ridx, std::vector<uint8_t>(
                                    payload + kFecRepairSubheaderSize,
                                    payload + payload_len));  // dup: no-op
    } else {
        if (payload_len < kFecSourceSubheaderSize) {
            ++stats_.malformed;
            return;
        }
        const uint16_t k = be16_read(payload + kFecSrcOffWindowLen);
        const uint16_t idx = be16_read(payload + kFecSrcOffSymIndex);
        if (k == 0 || idx >= k) {
            ++stats_.malformed;
            return;
        }
        b.k = k;
        const size_t clen = payload_len - kFecSourceSubheaderSize;
        // A non-last source symbol carries the full coded size s (§5.1a).
        if (idx != k - 1 && clen > b.s) {
            b.s = static_cast<uint16_t>(clen);
        }
        if (eob) {
            b.have_eob = true;
        }
        b.sources.emplace(idx, std::vector<uint8_t>(
                                   payload + kFecSourceSubheaderSize,
                                   payload + payload_len));  // dup: no-op
    }

    // §6.2 supersession: newer block advances the window.
    if (!have_highest_ || bid_diff(block_id, highest_block_) > 0) {
        highest_block_ = block_id;
        have_highest_ = true;
        supersede(highest_block_, emit);
        it = blocks_.find(block_id);  // supersede() never drops the newest
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
            if (static_cast<uint16_t>(it->second.sources.size() +
                                      it->second.repairs.size()) >= it->second.k &&
                it->second.k != 0) {
                ++stats_.frames_unrecoverable;  // had >= k but couldn't decode
            }
            ++stats_.frames_superseded;
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
            ++stats_.frames_deadline;
            finalize(it->first);
            it = blocks_.erase(it);
        } else {
            ++it;
        }
    }
}

void FrameReassembler::finalize(uint32_t id) {
    if (!have_finalized_ || bid_diff(id, finalized_upto_) > 0) {
        finalized_upto_ = id;
        have_finalized_ = true;
    }
}

}  // namespace wblink
