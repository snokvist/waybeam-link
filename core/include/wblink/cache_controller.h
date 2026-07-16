// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: aggregator-side Cache Controller (PROTOCOL.md §14.3).
//
// Watches the §6.3a reassembler's open blocks (RepairCandidate snapshots),
// applies the §14.3 local-collection close rules, and issues bounded §3.11
// CACHE_REQUESTs to eligible caches. Replies are validated against the
// outstanding request (§13) before the caller merges the wrapped symbol into
// the reassembler. Runs in parallel with the §6.4 NACK path (§14.3 rule 8).
// Pure logic: time injected, transport lives in io.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "wblink/frame_reassembler.h"
#include "wblink/types.h"
#include "wblink/wire.h"

namespace wblink {

struct CacheControllerConfig {
    uint16_t self_originator = 0;
    uint32_t self_session = 0;
    std::vector<uint16_t> caches;  // configured order = final ranking tiebreak
    // §14.3 close timers + budgets (RE-DERIVE seeds, §17).
    uint32_t tail_grace_ms = 1;
    uint32_t local_quiet_ms = 2;
    uint32_t min_collect_ms = 4;
    uint32_t hard_close_ms = 8;
    uint32_t request_timeout_ms = 4;
    uint16_t repair_fraction_permille = 200;
    uint8_t absolute_symbol_limit = 8;
    uint8_t max_cache_attempts = 2;
    uint8_t reply_limit = 4;
    uint16_t health_floor_permille = 800;
    uint32_t status_timeout_ms = 1500;
};

struct CacheRepairStats {
    uint64_t requests = 0;
    uint64_t replies = 0;
    uint64_t symbols_accepted = 0;
    uint64_t symbols_rejected = 0;
    uint64_t blocks_closed_deficit = 0;
    uint64_t blocks_repaired = 0;  // via note_completed()
    uint64_t blocks_futile = 0;    // §14.3 rule 4
    uint64_t requests_suppressed = 0;  // no eligible cache at attempt time
    uint32_t caches_fresh = 0;         // gauge
};

// One encoded request ready to send; io resolves originator -> endpoint.
struct CacheRequestOut {
    uint16_t cache_originator = 0;
    std::vector<uint8_t> frame;
};

class CacheController {
  public:
    explicit CacheController(const CacheControllerConfig& cfg) : cfg_(cfg) {}

    // Caller has already verified the source endpoint is a configured cache.
    void on_status(const CacheStatus& st, uint64_t now_ms);

    // Evaluate close rules + issue requests for the current open blocks.
    // target = the latched stream key the candidates belong to.
    std::vector<CacheRequestOut> tick(uint64_t now_ms, const StreamKey& target,
                                      const RepairCandidate* cands, size_t n);

    enum class ReplyVerdict : uint8_t {
        kAccept,
        kUnknownRequest,  // request_id not outstanding (incl. late/expired)
        kWrongCache,      // sender is not the addressed cache
        kWrongBlock,
        kNotRequested,    // symbol not in the requested set
        kOverAllowance,
        kMalformed,       // wrapped subheader unparseable
    };
    // Validate one wrapped DATA symbol against the outstanding request. On
    // kAccept the caller pushes it into the reassembler.
    ReplyVerdict on_reply(uint16_t from_originator, uint32_t request_id,
                          const DataView& wrapped);

    // Attribution: the caller observed the block emit during a cache merge.
    void note_completed(uint32_t block_id);

    CacheRepairStats& stats() { return stats_; }
    const CacheRepairStats& stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

  private:
    struct Registry {
        CacheStatus status;
        uint64_t last_seen_ms = 0;
    };
    struct Outstanding {
        uint16_t cache_originator = 0;
        uint32_t block_id = 0;
        uint64_t issued_ms = 0;
        uint8_t allowance = 0;
        uint8_t accepted = 0;
        uint16_t k = 0;
        std::array<uint8_t, 32> missing_sources{};
        std::array<uint8_t, 32> have_repairs{};
    };
    struct BlockState {
        bool closed = false;
        bool futile = false;
        bool suppressed = false;  // counted once while no cache is eligible
        uint8_t attempts = 0;
        uint8_t budget_used = 0;  // requested allowances (§14.3 rule 3)
        uint64_t last_request_ms = 0;
        std::vector<uint16_t> tried;  // caches already addressed
    };

    bool eligible(const Registry& r, const StreamKey& target,
                  uint32_t block_id, uint64_t now_ms) const;
    bool close_due(const RepairCandidate& c, uint64_t now_ms) const;

    CacheControllerConfig cfg_;
    CacheRepairStats stats_;
    std::map<uint16_t, Registry> registry_;
    std::map<uint32_t, BlockState> blocks_;
    std::map<uint32_t, Outstanding> outstanding_;
    uint32_t next_request_id_ = 1;
};

}  // namespace wblink
