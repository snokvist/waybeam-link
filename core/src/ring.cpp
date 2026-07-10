// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/ring.h"

#include <algorithm>

namespace wblink {

void ResendRing::push(const uint8_t* frame, size_t len, const DataHeader& hdr,
                      uint64_t now_ms) {
    // Byte budget: evict oldest first so the freshest packets survive.
    while (!entries_.empty() && bytes_ + len > cfg_.byte_budget) {
        bytes_ -= entries_.front().frame.size();
        entries_.pop_front();
    }
    if (len > cfg_.byte_budget) {
        return;  // pathological config; never store a frame we can't budget
    }
    RingEntry e;
    e.seq = hdr.seq;
    e.block_id = hdr.block_id;
    e.data_flags = hdr.data_flags;
    e.first_tx_ms = now_ms;
    e.frame.assign(frame, frame + len);
    bytes_ += len;
    entries_.push_back(std::move(e));
}

void ResendRing::evict(uint64_t now_ms) {
    while (!entries_.empty() &&
           now_ms - entries_.front().first_tx_ms > cfg_.window_ms) {
        bytes_ -= entries_.front().frame.size();
        entries_.pop_front();
    }
}

RingEntry* ResendRing::find(uint32_t seq) {
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), seq,
        [](const RingEntry& e, uint32_t s) { return e.seq < s; });
    if (it == entries_.end() || it->seq != seq) {
        return nullptr;
    }
    return &*it;
}

}  // namespace wblink
