// SPDX-License-Identifier: GPL-2.0-or-later
// CacheStore (PROTOCOL.md §14.3): retain heard symbols, answer bounded
// CACHE_REQUESTs with verbatim wire packets.
#include "wblink/cache_store.h"

#include <algorithm>

#include "wblink/endian.h"

namespace wblink {

namespace {
inline bool bit_set(const uint8_t* bm, size_t len, uint16_t i) {
    return (i / 8) < len && (bm[i / 8] & (1u << (i % 8))) != 0;
}
}  // namespace

bool CacheStore::tracked(uint8_t stream_id) const {
    return std::find(cfg_.stream_ids.begin(), cfg_.stream_ids.end(),
                     stream_id) != cfg_.stream_ids.end();
}

void CacheStore::note_data(const DataView& v, const uint8_t* frame,
                           size_t frame_len) {
    if (frame == nullptr || frame_len == 0 || !tracked(v.hdr.stream_id)) {
        return;
    }
    // Unified §3.11 symbol index from the self-describing subheaders
    // (§5.1a source, §14.1 repair): source i, or k + repair_idx.
    uint16_t k = 0;
    uint16_t idx = 0;
    if ((v.hdr.data_flags & data_flags::kFecRepair) != 0) {
        if (v.payload_len <= kFecRepairSubheaderSize) {
            return;
        }
        k = be16_read(v.payload + kFecOffWindowLen);
        const uint8_t ridx = v.payload[kFecOffRepairIdx];
        if (k == 0 || k > kFecMaxSymbols ||
            static_cast<uint32_t>(k) + ridx >= kFecMaxSymbols) {
            return;
        }
        idx = static_cast<uint16_t>(k + ridx);
    } else {
        if (v.payload_len < kFecSourceSubheaderSize) {
            return;
        }
        k = be16_read(v.payload + kFecSrcOffWindowLen);
        idx = be16_read(v.payload + kFecSrcOffSymIndex);
        if (k == 0 || k > kFecMaxSymbols || idx >= k) {
            return;
        }
    }

    const StreamKey key{v.hdr.prefix.originator, v.hdr.prefix.session_id,
                        v.hdr.stream_id};
    StreamState& st = streams_[v.hdr.stream_id];
    if (cfg_.target_originator != 0 &&
        key.originator != cfg_.target_originator) {
        return;  // operator-pinned cache target (§3.5/§14.3)
    }
    if (st.key.originator != 0 && st.key.originator != key.originator) {
        return;  // first-latched sender owns this stream until restart
    }
    if (!(st.key == key)) {  // same sender's new session (or first packet)
        st.key = key;
        st.blocks.clear();
        st.order.clear();
    }

    auto bit = st.blocks.find(v.hdr.block_id);
    if (bit == st.blocks.end()) {
        bit = st.blocks.emplace(v.hdr.block_id, BlockEntry{}).first;
        st.order.push_back(v.hdr.block_id);
        while (st.order.size() > cfg_.blocks) {
            st.blocks.erase(st.order.front());
            st.order.pop_front();
        }
        // Eviction may have removed the block just inserted (blocks == 0 is
        // rejected at config load; guard anyway).
        bit = st.blocks.find(v.hdr.block_id);
        if (bit == st.blocks.end()) {
            return;
        }
    }
    BlockEntry& be = bit->second;
    if (be.k != 0 && be.k != k) {
        return;  // inconsistent subheaders — keep the first-seen geometry
    }
    be.k = k;
    if (be.symbols.emplace(idx, std::vector<uint8_t>(frame, frame + frame_len))
            .second &&
        idx < k) {
        ++be.sources_held;
    }
    uint64_t held = 0;
    for (const auto& s : streams_) {
        held += s.second.blocks.size();
    }
    stats_.blocks_held = static_cast<uint32_t>(held);
    if (!streams_.empty()) {
        stats_.health_permille = health_permille(streams_.begin()->second);
    }
}

uint16_t CacheStore::health_permille(const StreamState& st) const {
    // §3.11: rolling mean unique/k over the newest health_window_blocks
    // retained blocks with a known k.
    uint32_t sum = 0;
    uint32_t n = 0;
    for (auto it = st.order.rbegin();
         it != st.order.rend() && n < cfg_.health_window_blocks; ++it) {
        const auto bit = st.blocks.find(*it);
        if (bit == st.blocks.end() || bit->second.k == 0) {
            continue;
        }
        const uint32_t uniq = static_cast<uint32_t>(
            std::min<size_t>(bit->second.symbols.size(), bit->second.k));
        sum += uniq * 1000u / bit->second.k;
        ++n;
    }
    return n == 0 ? 0 : static_cast<uint16_t>(std::min(1000u, sum / n));
}

CacheStore::Verdict CacheStore::answer(
    const CacheRequestView& req, uint64_t now_ms,
    std::vector<const std::vector<uint8_t>*>& out) {
    out.clear();
    ++stats_.requests_received;
    if (req.hdr.target_cache != cfg_.self_originator) {
        ++stats_.requests_rejected;
        return Verdict::kNotOurs;
    }
    // §13 per-requester rate cap + request_id dedup window.
    RequesterState& rq = requesters_[req.hdr.prefix.originator];
    if (std::find(rq.recent_ids.begin(), rq.recent_ids.end(),
                  req.hdr.request_id) != rq.recent_ids.end()) {
        return Verdict::kDuplicate;  // §13: silent, not counted as rejected
    }
    if (now_ms - rq.window_start_ms >= 1000) {
        rq.window_start_ms = now_ms;
        rq.window_count = 0;
    }
    if (cfg_.max_requests_per_s != 0 &&
        rq.window_count >= cfg_.max_requests_per_s) {
        ++stats_.requests_rejected;
        return Verdict::kRateLimited;
    }
    ++rq.window_count;
    rq.recent_ids.push_back(req.hdr.request_id);
    while (rq.recent_ids.size() > 32) {
        rq.recent_ids.pop_front();
    }

    const auto sit = streams_.find(req.hdr.target_stream_id);
    const StreamKey want{req.hdr.target_originator, req.hdr.target_session,
                         req.hdr.target_stream_id};
    if (sit == streams_.end() || !(sit->second.key == want)) {
        ++stats_.requests_rejected;
        return Verdict::kUnknownStream;
    }
    const StreamState& st = sit->second;
    const auto bit = st.blocks.find(req.hdr.block_id);
    if (bit == st.blocks.end() || bit->second.k == 0 ||
        bit->second.k != req.hdr.window_len) {
        ++stats_.requests_rejected;
        return Verdict::kNoWindow;
    }
    const BlockEntry& be = bit->second;

    // §3.11 reply selection: requested missing sources first (ascending),
    // then held repairs whose repair_have bit is clear (ascending).
    const size_t src_bm_len = (static_cast<size_t>(req.hdr.window_len) + 7) / 8;
    const uint8_t allowance =
        std::min(req.hdr.max_symbols, cfg_.reply_limit);
    for (const auto& kv : be.symbols) {
        if (out.size() >= allowance) {
            break;
        }
        const uint16_t idx = kv.first;
        if (idx < be.k) {
            if (bit_set(req.missing_sources, src_bm_len, idx)) {
                out.push_back(&kv.second);
            }
        } else {
            const uint16_t ridx = static_cast<uint16_t>(idx - be.k);
            if (!bit_set(req.repair_have, req.repair_have_len, ridx)) {
                out.push_back(&kv.second);
            }
        }
    }
    ++stats_.requests_answered;
    stats_.symbols_sent += out.size();
    return Verdict::kAnswered;
}

std::vector<CacheStore::StatusEntry> CacheStore::status() const {
    std::vector<StatusEntry> out;
    for (const auto& [sid, st] : streams_) {
        if (st.order.empty()) {
            continue;  // §3.11: an empty window is silence
        }
        StatusEntry e;
        e.key = st.key;
        e.stream_id = sid;
        e.oldest_block = st.order.front();
        e.newest_block = st.order.back();
        e.rx_health_permille = health_permille(st);
        out.push_back(e);
    }
    return out;
}

void CacheStore::reset_stats() {
    const uint32_t held = stats_.blocks_held;
    const uint16_t health = stats_.health_permille;
    stats_ = {};
    stats_.blocks_held = held;        // gauges stay live (§15.3)
    stats_.health_permille = health;
}

}  // namespace wblink
