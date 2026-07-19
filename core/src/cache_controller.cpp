// SPDX-License-Identifier: GPL-2.0-or-later
// CacheController (PROTOCOL.md §14.3): close incomplete local blocks, issue
// bounded CACHE_REQUESTs, validate replies against the outstanding request.
#include "wblink/cache_controller.h"

#include <algorithm>
#include <climits>

#include "wblink/endian.h"

namespace wblink {

namespace {
inline int32_t bid_diff(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b);
}
inline bool bit_set(const std::array<uint8_t, 32>& bm, uint16_t i) {
    return i < 256 && (bm[i / 8] & (1u << (i % 8))) != 0;
}
// Outstanding requests older than this are unanswerable (§6.3a one-frame
// window is long gone) and are pruned regardless of request_timeout_ms.
constexpr uint64_t kOutstandingPruneMs = 1000;
constexpr size_t kTimingWindow = 512;
}  // namespace

void CacheController::TimingSeries::observe(uint64_t delta_us) {
    const uint32_t sample = static_cast<uint32_t>(
        std::min<uint64_t>(delta_us, UINT32_MAX));
    ++samples_;
    max_us_ = std::max(max_us_, sample);
    recent_.push_back(sample);
    if (recent_.size() > kTimingWindow) recent_.pop_front();
}

CacheTimingStats CacheController::TimingSeries::snapshot() const {
    CacheTimingStats out{samples_, 0, max_us_};
    if (recent_.empty()) return out;
    std::vector<uint32_t> sorted(recent_.begin(), recent_.end());
    std::sort(sorted.begin(), sorted.end());
    const size_t rank = (sorted.size() * 95 + 99) / 100;
    out.p95_us = sorted[rank - 1];
    return out;
}

void CacheController::TimingSeries::reset() {
    samples_ = 0;
    max_us_ = 0;
    recent_.clear();
}

void CacheController::on_status(const CacheStatus& st, uint64_t now_ms) {
    if (std::find(cfg_.caches.begin(), cfg_.caches.end(),
                  st.prefix.originator) == cfg_.caches.end()) {
        return;  // §13: only configured caches exist
    }
    std::vector<Registry>& entries = registry_[st.prefix.originator];
    auto it = std::find_if(
        entries.begin(), entries.end(), [&](const Registry& r) {
            return r.status.target_originator == st.target_originator &&
                   r.status.target_stream_id == st.target_stream_id;
        });
    if (it == entries.end()) {
        entries.push_back(Registry{});
        it = std::prev(entries.end());
    }
    Registry& r = *it;
    r.status = st;
    r.last_seen_ms = now_ms;
}

bool CacheController::eligible(const Registry& r, const StreamKey& target,
                               uint32_t block_id, uint64_t now_ms) const {
    if (now_ms - r.last_seen_ms > cfg_.status_timeout_ms) {
        return false;
    }
    const CacheStatus& s = r.status;
    if (s.target_originator != target.originator ||
        s.target_session != target.session_id ||
        s.target_stream_id != target.stream_id) {
        return false;
    }
    if (s.rx_health_permille < cfg_.health_floor_permille) {
        return false;
    }
    // §14.3 rule 6: only the oldest bound — newest_block is one status
    // interval stale, so the blocks needing repair always lie past it.
    return bid_diff(block_id, s.oldest_block) >= 0;
}

bool CacheController::close_due(const RepairCandidate& c,
                                uint64_t now_ms) const {
    // §14.3 local-collection close: earliest of tail-grace, quiet timeout
    // floored by min_collect, or hard close.
    if (c.have_eob && now_ms >= c.eob_ms + cfg_.tail_grace_ms) {
        return true;
    }
    const uint64_t quiet = std::max(c.first_ms + cfg_.min_collect_ms,
                                    c.last_new_ms + cfg_.local_quiet_ms);
    if (now_ms >= quiet) {
        return true;
    }
    return now_ms >= c.first_ms + cfg_.hard_close_ms;
}

std::vector<CacheRequestOut> CacheController::tick(uint64_t now_ms,
                                                   const StreamKey& target,
                                                   const RepairCandidate* cands,
                                                   size_t n) {
    std::vector<CacheRequestOut> out;

    // Prune state for blocks no longer open (emitted or finalized) and
    // expired outstanding requests (their late replies are kUnknownRequest).
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        bool open = false;
        for (size_t i = 0; i < n; ++i) {
            if (cands[i].block_id == it->first) {
                open = true;
                break;
            }
        }
        it = open ? std::next(it) : blocks_.erase(it);
    }
    for (auto it = outstanding_.begin(); it != outstanding_.end();) {
        it = (now_ms - it->second.issued_ms > kOutstandingPruneMs)
                 ? outstanding_.erase(it)
                 : std::next(it);
    }

    uint32_t fresh = 0;
    for (const uint16_t orig : cfg_.caches) {
        const auto rit = registry_.find(orig);
        if (rit == registry_.end()) {
            continue;
        }
        const bool found = std::any_of(
            rit->second.begin(), rit->second.end(), [&](const Registry& r) {
                return r.status.target_originator == target.originator &&
                       r.status.target_session == target.session_id &&
                       r.status.target_stream_id == target.stream_id &&
                       now_ms - r.last_seen_ms <= cfg_.status_timeout_ms;
            });
        fresh += found ? 1 : 0;
    }
    stats_.caches_fresh = fresh;

    for (size_t i = 0; i < n; ++i) {
        const RepairCandidate& c = cands[i];
        if (c.k == 0 || c.unique >= c.k) {
            continue;
        }
        BlockState& st = blocks_[c.block_id];
        if (st.futile) {
            continue;
        }
        if (!st.closed) {
            if (!close_due(c, now_ms)) {
                continue;
            }
            st.closed = true;
            ++stats_.blocks_closed_deficit;
        }
        const uint16_t deficit = static_cast<uint16_t>(c.k - c.unique);
        const uint16_t cap = std::min<uint16_t>(
            static_cast<uint16_t>(
                (static_cast<uint32_t>(c.k) * cfg_.repair_fraction_permille +
                 999) / 1000),
            cfg_.absolute_symbol_limit);
        if (st.attempts == 0 && deficit > cap) {
            st.futile = true;  // §14.3 rule 4 — vehicle ARQ is unaffected
            ++stats_.blocks_futile;
            continue;
        }
        if (st.attempts >= cfg_.max_cache_attempts) {
            continue;
        }
        if (st.attempts > 0 &&
            now_ms - st.last_request_ms < cfg_.request_timeout_ms) {
            continue;  // §14.3 rule 5: attempts are sequential
        }
        const uint16_t remaining =
            cap > st.budget_used
                ? static_cast<uint16_t>(cap - st.budget_used)
                : 0;
        if (remaining == 0) {
            continue;
        }

        // §14.3 rule 6 ranking among eligible, untried caches: health, then
        // freshness, then config order.
        const Registry* best = nullptr;
        uint16_t best_orig = 0;
        for (const uint16_t orig : cfg_.caches) {
            if (std::find(st.tried.begin(), st.tried.end(), orig) !=
                st.tried.end()) {
                continue;
            }
            const auto rit = registry_.find(orig);
            if (rit == registry_.end()) {
                continue;
            }
            const auto sit = std::find_if(
                rit->second.begin(), rit->second.end(),
                [&](const Registry& r) {
                    return eligible(r, target, c.block_id, now_ms);
                });
            if (sit == rit->second.end()) {
                continue;
            }
            const Registry& r = *sit;
            if (best == nullptr ||
                r.status.rx_health_permille >
                    best->status.rx_health_permille ||
                (r.status.rx_health_permille ==
                     best->status.rx_health_permille &&
                 r.last_seen_ms > best->last_seen_ms)) {
                best = &r;
                best_orig = orig;
            }
        }
        if (best == nullptr) {
            if (!st.suppressed) {
                st.suppressed = true;
                ++stats_.requests_suppressed;
            }
            continue;
        }
        st.suppressed = false;

        const uint8_t allowance = static_cast<uint8_t>(std::min<uint16_t>(
            {cfg_.reply_limit, deficit, remaining}));
        CacheRequestHeader hdr;
        hdr.prefix = CommonPrefix{cfg_.self_originator, best_orig,
                                  cfg_.self_session};
        hdr.target_originator = target.originator;
        hdr.target_session = target.session_id;
        hdr.target_stream_id = target.stream_id;
        hdr.target_cache = best_orig;
        hdr.request_id = next_request_id_++;
        hdr.block_id = c.block_id;
        hdr.window_len = c.k;
        hdr.max_symbols = allowance;
        // Trim repair_have to the last non-zero byte (§3.11: len <= 32).
        uint8_t rh_len = 32;
        while (rh_len > 0 && c.have_repairs[rh_len - 1] == 0) {
            --rh_len;
        }
        CacheRequestOut req;
        req.cache_originator = best_orig;
        req.request_id = hdr.request_id;
        req.block_id = c.block_id;
        req.frame.resize(kCacheRequestFixedSize + 32 + 32);
        const size_t sz = encode_cache_request(
            hdr, c.missing_sources.data(),
            rh_len > 0 ? c.have_repairs.data() : nullptr, rh_len,
            req.frame.data(), req.frame.size());
        if (sz == 0) {
            continue;  // structurally impossible; keep state untouched
        }
        req.frame.resize(sz);

        Outstanding o;
        o.cache_originator = best_orig;
        o.block_id = c.block_id;
        o.issued_ms = now_ms;
        o.allowance = allowance;
        o.k = c.k;
        o.missing_sources = c.missing_sources;
        o.have_repairs = c.have_repairs;
        outstanding_.emplace(hdr.request_id, o);

        ++st.attempts;
        st.budget_used = static_cast<uint8_t>(st.budget_used + allowance);
        st.last_request_ms = now_ms;
        st.tried.push_back(best_orig);
        ++stats_.requests;
        out.push_back(std::move(req));
    }
    return out;
}

void CacheController::note_request_sent(uint32_t request_id,
                                        uint64_t now_us) {
    const auto it = outstanding_.find(request_id);
    if (it == outstanding_.end()) return;
    Outstanding& o = it->second;
    if (!o.sent_us) o.sent_us = now_us;
    const auto bit = blocks_.find(o.block_id);
    if (bit != blocks_.end() && !bit->second.first_request_us) {
        bit->second.first_request_us = now_us;
    }
}

CacheController::ReplyVerdict CacheController::on_reply(
    uint16_t from_originator, uint32_t request_id, const DataView& wrapped,
    uint64_t now_us) {
    ++stats_.replies;
    const auto it = outstanding_.find(request_id);
    if (it == outstanding_.end()) {
        ++stats_.symbols_rejected;
        return ReplyVerdict::kUnknownRequest;
    }
    Outstanding& o = it->second;
    if (from_originator != o.cache_originator) {
        ++stats_.symbols_rejected;
        return ReplyVerdict::kWrongCache;
    }
    if (wrapped.hdr.block_id != o.block_id) {
        ++stats_.symbols_rejected;
        return ReplyVerdict::kWrongBlock;
    }
    // Unified §3.11 index from the wrapped symbol's subheader.
    uint16_t idx = 0;
    bool is_repair = (wrapped.hdr.data_flags & data_flags::kFecRepair) != 0;
    if (is_repair) {
        if (wrapped.payload_len <= kFecRepairSubheaderSize ||
            be16_read(wrapped.payload + kFecOffWindowLen) != o.k) {
            ++stats_.symbols_rejected;
            return ReplyVerdict::kMalformed;
        }
        idx = wrapped.payload[kFecOffRepairIdx];
        if (bit_set(o.have_repairs, idx)) {
            ++stats_.symbols_rejected;
            return ReplyVerdict::kNotRequested;
        }
    } else {
        if (wrapped.payload_len < kFecSourceSubheaderSize ||
            be16_read(wrapped.payload + kFecSrcOffWindowLen) != o.k) {
            ++stats_.symbols_rejected;
            return ReplyVerdict::kMalformed;
        }
        idx = be16_read(wrapped.payload + kFecSrcOffSymIndex);
        if (idx >= o.k || !bit_set(o.missing_sources, idx)) {
            ++stats_.symbols_rejected;
            return ReplyVerdict::kNotRequested;
        }
    }
    if (o.accepted >= o.allowance) {
        ++stats_.symbols_rejected;
        return ReplyVerdict::kOverAllowance;
    }
    ++o.accepted;
    ++stats_.symbols_accepted;
    if (!o.first_reply_seen) {
        o.first_reply_seen = true;
        if (o.sent_us && now_us >= *o.sent_us) {
            first_reply_timing_.observe(now_us - *o.sent_us);
        }
    }
    return ReplyVerdict::kAccept;
}

void CacheController::note_completed(uint32_t block_id, uint64_t now_us,
                                     bool before_nack) {
    ++stats_.blocks_repaired;
    const auto bit = blocks_.find(block_id);
    if (bit != blocks_.end() && bit->second.first_request_us &&
        now_us >= *bit->second.first_request_us) {
        completion_timing_.observe(now_us - *bit->second.first_request_us);
        if (before_nack) ++stats_.blocks_repaired_before_nack;
    }
    blocks_.erase(block_id);
    for (auto it = outstanding_.begin(); it != outstanding_.end();) {
        it = it->second.block_id == block_id ? outstanding_.erase(it)
                                             : std::next(it);
    }
}

CacheRepairStats CacheController::stats() const {
    CacheRepairStats out = stats_;
    out.request_to_first_reply = first_reply_timing_.snapshot();
    out.request_to_completion = completion_timing_.snapshot();
    return out;
}

void CacheController::reset_stats() {
    stats_ = {};
    first_reply_timing_.reset();
    completion_timing_.reset();
    // Do not let requests that crossed the reset boundary contribute partial
    // intervals to the fresh measurement window.
    for (auto& [id, o] : outstanding_) {
        (void)id;
        o.sent_us.reset();
        o.first_reply_seen = true;
    }
    for (auto& [id, b] : blocks_) {
        (void)id;
        b.first_request_us.reset();
    }
}

}  // namespace wblink
