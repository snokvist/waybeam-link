// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: cache-node symbol store (PROTOCOL.md §14.3).
//
// Retains the last `blocks` blocks of verbatim §3.2 DATA wire packets per
// tracked stream and answers §3.11 CACHE_REQUESTs with bounded subsets. The
// stored bytes are exactly what was heard on the air, so a reply can never
// hand an aggregator anything a radio could not (§3.11). Pure logic: time is
// injected, transport lives in io.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "wblink/types.h"
#include "wblink/wire.h"

namespace wblink {

struct CacheStoreConfig {
    uint16_t self_originator = 0;  // §3.11 target_cache match
    // Preferred target sender; 0 first-latches the initial originator.
    // Session changes from that originator are followed, while a different
    // sender cannot wipe the retained window for the same stream_id.
    uint16_t target_originator = 0;
    std::vector<uint8_t> stream_ids;
    uint16_t blocks = 96;              // retention window per stream
    uint8_t reply_limit = 4;           // §14.3 per-request symbol clamp
    uint16_t max_requests_per_s = 400; // §13 per-requester rate cap
    uint16_t health_window_blocks = 90;
};

struct CacheStoreStats {
    uint64_t requests_received = 0;
    uint64_t requests_answered = 0;
    uint64_t requests_rejected = 0;  // not-ours / unknown / no-window / limits
    uint64_t symbols_sent = 0;
    uint64_t status_sent = 0;  // maintained by the io sender
    uint32_t blocks_held = 0;        // gauge
    uint16_t health_permille = 0;    // gauge (first tracked stream)
};

class CacheStore {
  public:
    explicit CacheStore(const CacheStoreConfig& cfg) : cfg_(cfg) {}

    // Feed one heard DATA frame (already §3.1-decoded by the caller). Stores
    // a verbatim copy of the whole wire packet when the stream is tracked and
    // the symbol subheader parses. The configured/first-latched originator
    // owns each stream; a new session from that originator replaces the old
    // one, while packets from other originators are ignored.
    void note_data(const DataView& v, const uint8_t* frame, size_t frame_len);

    enum class Verdict : uint8_t {
        kAnswered,      // out filled (possibly empty => silent, §3.11)
        kNotOurs,       // target_cache != self
        kUnknownStream, // stream not tracked / session mismatch
        kNoWindow,      // block outside the retention window
        kRateLimited,   // §13 per-requester cap
        kDuplicate,     // §13 request_id dedup window
    };

    // Answer one decoded CACHE_REQUEST. On kAnswered, out holds pointers to
    // internal wire-packet storage, valid until the next note_data() call.
    Verdict answer(const CacheRequestView& req, uint64_t now_ms,
                   std::vector<const std::vector<uint8_t>*>& out);

    struct StatusEntry {
        StreamKey key;
        uint8_t stream_id = 0;
        uint32_t oldest_block = 0;
        uint32_t newest_block = 0;
        uint16_t rx_health_permille = 0;
    };
    // One entry per tracked stream with a non-empty window (§3.11).
    std::vector<StatusEntry> status() const;

    // §14.3 Pass 67: receiver-owned cache assignment. A changed target clears
    // the prior vehicle's retained and requester state before new admission.
    void assign_target(uint16_t originator);
    uint16_t target_originator() const { return cfg_.target_originator; }

    CacheStoreStats& stats() { return stats_; }
    const CacheStoreStats& stats() const { return stats_; }
    void reset_stats();

  private:
    struct BlockEntry {
        uint16_t k = 0;
        // unified §3.11 index (source i, or k + repair_idx) -> wire packet.
        std::map<uint16_t, std::vector<uint8_t>> symbols;
        uint16_t sources_held = 0;
    };
    struct StreamState {
        StreamKey key;
        std::map<uint32_t, BlockEntry> blocks;
        std::deque<uint32_t> order;  // retention eviction order
    };
    struct RequesterState {
        uint32_t session_id = 0;
        uint64_t window_start_ms = 0;
        uint16_t window_count = 0;
        std::deque<uint32_t> recent_ids;  // §13 request_id dedup (last 32)
    };

    bool tracked(uint8_t stream_id) const;
    uint16_t health_permille(const StreamState& st) const;

    CacheStoreConfig cfg_;
    CacheStoreStats stats_;
    // keyed by stream_id (one live session per tracked stream at a time).
    std::map<uint8_t, StreamState> streams_;
    // One bounded state per node; a session transition resets its per-boot
    // request-id and rate domain without growing state per reboot.
    std::map<uint16_t, RequesterState> requesters_;
};

}  // namespace wblink
