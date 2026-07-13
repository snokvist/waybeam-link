// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/scheduler.h"

#include <algorithm>

namespace wblink {

namespace {

unsigned popcount_bitmap(const uint8_t* bitmap, uint8_t len) {
    unsigned n = 0;
    for (uint8_t i = 0; i < len; ++i) {
        uint8_t b = bitmap[i];
        while (b != 0) {
            n += b & 1u;
            b = static_cast<uint8_t>(b >> 1);
        }
    }
    return n;
}

}  // namespace

uint64_t ResendScheduler::entry_deadline(const RingEntry& e) const {
    const bool iframe_class = (e.data_flags & data_flags::kArq) != 0;
    uint16_t budget = iframe_class ? 50 : 16;  // fallback mirrors RxPolicy seeds
    if (table_ != nullptr) {
        for (const Profile& p : table_->profiles) {
            if (p.id == active_profile_) {
                budget = iframe_class ? p.arq_deadline_iframe_ms
                                      : p.arq_deadline_pframe_ms;
                break;
            }
        }
    }
    return e.first_tx_ms + budget;
}

void ResendScheduler::arbitrate_lock(uint16_t requester, uint64_t now_ms) {
    last_nack_ms_[requester] = now_ms;
    // §12: preferred preempts immediately and unconditionally.
    if (policy_.preferred_originator != 0 &&
        requester == policy_.preferred_originator) {
        lock_holder_ = requester;
    } else if (lock_holder_ == 0) {
        lock_holder_ = requester;  // first latcher
    } else if (lock_holder_ != requester &&
               lock_holder_ != policy_.preferred_originator) {
        // Contested-only release among non-preferred: holder silent AND
        // someone else actively asking (this call IS the contest).
        const auto it = last_nack_ms_.find(lock_holder_);
        if (it == last_nack_ms_.end() ||
            now_ms - it->second >= policy_.release_timeout_ms) {
            lock_holder_ = requester;
        }
    }
    counters_.lock_holder = lock_holder_;
    // Bound the evidence map (semi-anarchy: arbitrary originators on-air).
    while (last_nack_ms_.size() > 64) {
        auto oldest = last_nack_ms_.begin();
        for (auto it = last_nack_ms_.begin(); it != last_nack_ms_.end(); ++it) {
            if (it->second < oldest->second) {
                oldest = it;
            }
        }
        if (oldest->first == lock_holder_) {
            break;  // never forget the holder's evidence
        }
        last_nack_ms_.erase(oldest);
    }
}

void ResendScheduler::on_nack(const NackView& nack, ResendRing& ring,
                              uint64_t now_ms) {
    // §13 bitmap sanity clamp: reject popcount > one block's worth, or a
    // base_seq with no overlap with the ring window. (Bit 0 covers base_seq
    // itself — nothing is implied outside the bitmap.)
    const unsigned popcount = popcount_bitmap(nack.bitmap, nack.bitmap_len);
    if (popcount == 0 || popcount > policy_.max_block_pkts) {
        ++counters_.nacks_rejected_sanity;
        return;
    }
    if (ring.empty()) {
        ++counters_.nacks_rejected_sanity;
        return;
    }
    const uint64_t span_end =
        static_cast<uint64_t>(nack.hdr.base_seq) + nack.bitmap_len * 8u;
    if (nack.hdr.base_seq > ring.newest_seq() ||
        span_end < ring.oldest_seq()) {
        ++counters_.nacks_rejected_sanity;
        return;
    }

    const uint16_t requester = nack.hdr.prefix.originator;
    arbitrate_lock(requester, now_ms);
    ++counters_.nacks_accepted;

    for (unsigned i = 0; i < nack.bitmap_len * 8u; ++i) {
        if ((nack.bitmap[i / 8] & (1u << (i % 8))) == 0) {
            continue;
        }
        const uint32_t seq = nack.hdr.base_seq + i;
        RingEntry* e = ring.find(seq);
        if (e == nullptr) {
            ++counters_.dropped_evicted;
            continue;
        }
        // §5.3 importance gate: only ARQ blocks are ever resent.
        if ((e->data_flags & (data_flags::kArq |
                              data_flags::kPframeArq)) == 0) {
            ++counters_.dropped_not_arq;
            continue;
        }
        auto [it, inserted] = pending_.try_emplace(seq);
        if (inserted) {
            it->second.requester = requester;
            it->second.requested_ms = now_ms;
        } else if (policy_.preferred_originator != 0 &&
                   requester == policy_.preferred_originator) {
            it->second.requester = requester;  // preferred adopts the seq
        }
    }
}

void ResendScheduler::drain(ResendRing& ring, uint64_t now_ms,
                            const EmitResend& emit) {
    // Budget window rollover.
    if (now_ms - window_start_ms_ >= policy_.interval_ms) {
        window_start_ms_ = now_ms;
        live_bytes_window_ = 0;
        spent_window_.clear();
    }
    // Prune stale hold-down entries.
    for (auto it = last_resend_.begin(); it != last_resend_.end();) {
        if (now_ms - it->second > policy_.holddown_ms) {
            it = last_resend_.erase(it);
        } else {
            ++it;
        }
    }

    if (pending_.empty()) {
        return;
    }

    // Gate + collect candidates.
    struct Candidate {
        uint32_t seq;
        uint16_t requester;
        uint64_t deadline;
        RingEntry* entry;
    };
    std::vector<Candidate> cands;
    std::map<uint16_t, bool> active;
    for (auto it = pending_.begin(); it != pending_.end();) {
        const uint32_t seq = it->first;
        RingEntry* e = ring.find(seq);
        if (e == nullptr) {
            ++counters_.dropped_evicted;
            it = pending_.erase(it);
            continue;
        }
        if (e->attempts >= policy_.attempt_cap) {
            ++counters_.dropped_attempts;
            it = pending_.erase(it);
            continue;
        }
        const uint64_t deadline = entry_deadline(*e);
        if (now_ms >= deadline ||
            (policy_.min_recoverable_ms != 0 &&
             deadline - now_ms < policy_.min_recoverable_ms)) {
            ++counters_.dropped_deadline;
            it = pending_.erase(it);
            continue;
        }
        // §5.3 global per-seq hold-down: an in-window earlier resend
        // already served every requester (resends are broadcast, §12).
        if (const auto hit = last_resend_.find(seq);
            hit != last_resend_.end() &&
            now_ms - hit->second < policy_.holddown_ms) {
            ++counters_.holddown_suppressed;
            it = pending_.erase(it);
            continue;
        }
        cands.push_back(Candidate{seq, it->second.requester, deadline, e});
        active[it->second.requester] = true;
        ++it;
    }
    if (cands.empty()) {
        return;
    }

    // §5.3 freshness-priority within the budget; §12 the lock is a tiebreak
    // within the partition — the holder's requests go first.
    std::sort(cands.begin(), cands.end(),
              [&](const Candidate& a, const Candidate& b) {
                  const bool ah = a.requester == lock_holder_;
                  const bool bh = b.requester == lock_holder_;
                  if (ah != bh) {
                      return ah;
                  }
                  if (a.deadline != b.deadline) {
                      return a.deadline < b.deadline;
                  }
                  return a.seq < b.seq;
              });

    // §5.3 airtime cap, partitioned per originator (§12): resend bytes may
    // be at most airtime_frac of the total, i.e. f/(1-f) of live bytes.
    const double ratio = policy_.airtime_frac / (1.0 - policy_.airtime_frac);
    const size_t total_budget =
        std::max(policy_.budget_floor_bytes,
                 static_cast<size_t>(ratio *
                                     static_cast<double>(live_bytes_window_)));
    const size_t share = total_budget / active.size();

    for (const Candidate& c : cands) {
        size_t& spent = spent_window_[c.requester];
        const size_t cost = c.entry->frame.size();
        if (spent + cost > share) {
            ++counters_.budget_deferred;
            continue;  // fenced partition: never spills into another share
        }
        std::vector<uint8_t> frame = c.entry->frame;
        ResendRing::mark_retransmit(frame.data(), frame.size());
        emit(frame.data(), frame.size());
        spent += cost;
        ++c.entry->attempts;
        last_resend_[c.seq] = now_ms;
        ++counters_.resends_sent;
        pending_.erase(c.seq);
    }
}

}  // namespace wblink
