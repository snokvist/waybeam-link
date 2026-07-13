// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: TX resend ring (PROTOCOL.md §5.2).
//
// Recently sent DATA frames for a bounded window, looked up by seq for NACK
// service. Stores the fully encoded frame — a resend does not re-encode, it
// copies the frame and sets the RETRANSMIT bit (mark_retransmit) so the
// stored original stays pristine. Eviction by age (ring_window_ms) and by a
// byte budget; seqs are monotonic so lookup is a binary search over a deque.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "wblink/types.h"
#include "wblink/wire.h"

namespace wblink {

struct RingConfig {
    uint32_t window_ms = 50;            // §5.2 seed, bench-gated (§17)
    size_t byte_budget = 256 * 1024;    // ~125 KB @ 20 Mbps/50 ms, with slack
};

struct RingEntry {
    uint32_t seq = 0;
    uint32_t block_id = 0;
    uint8_t data_flags = 0;  // original ARQ class gates resend/deadline
    uint64_t first_tx_ms = 0;
    uint8_t attempts = 0;  // resend attempts so far (§5.3 attempt cap)
    std::vector<uint8_t> frame;
};

class ResendRing {
  public:
    explicit ResendRing(const RingConfig& cfg) : cfg_(cfg) {}

    void push(const uint8_t* frame, size_t len, const DataHeader& hdr,
              uint64_t now_ms);

    // Drop entries older than window_ms. Byte budget is enforced in push.
    void evict(uint64_t now_ms);

    // nullptr if evicted or never sent.
    RingEntry* find(uint32_t seq);

    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }
    size_t bytes() const { return bytes_; }
    uint32_t oldest_seq() const { return entries_.front().seq; }
    uint32_t newest_seq() const { return entries_.back().seq; }

    // Flip the RETRANSMIT bit on a copied frame before injection (§5.3).
    static void mark_retransmit(uint8_t* frame, size_t len) {
        if (len > 21) {
            frame[21] |= data_flags::kRetransmit;
        }
    }

  private:
    RingConfig cfg_;
    std::deque<RingEntry> entries_;  // ascending seq
    size_t bytes_ = 0;
};

}  // namespace wblink
