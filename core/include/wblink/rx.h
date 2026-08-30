// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: the merged RX state machine (PROTOCOL.md §6) — one per
// RX node. All local adapters feed this one pipeline; per-stream state is
// keyed by the full (originator, session_id, stream_id) tuple (§2).
//
// Covers: discovery + admission control (§2, §13), latch with startup floor,
// ingest/dedup with diversity accounting (§6.1), the three gap short-circuits
// in priority order (§6.2) guarded by the plausible-forward clamp (§6.6),
// in-order best-effort delivery (§6.3), NACK generation with coalescing and
// bounded re-NACK backoff (§6.4), the adapter liveness watchdog (§6.5),
// per-block deadlines from the profile table (§8), the best-effort fallback
// for unknown stream_type / table_version mismatch (§3.4), and implicit idle
// teardown (§2).
//
// Pure tick-driven logic: time is injected, delivery is a callback, NACKs
// are returned as build products for the caller to encode/inject. No clocks,
// no sockets, no threads. Injected now_ms SHOULD be nondecreasing across
// calls (take ONE timestamp per event-loop iteration); the destructive paths
// (idle teardown, §6.6 resync) are additionally guarded so a small backward
// step can never underflow u64 elapsed-time math into an instant flush.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "wblink/table.h"
#include "wblink/types.h"
#include "wblink/wire.h"

namespace wblink {

struct RxPolicy {
    // §6.6 clamps. fwd_clamp_pkts ≈ fwd_clamp_blocks × max packets/block.
    uint32_t fwd_clamp_pkts = 256;
    uint32_t fwd_clamp_blocks = 4;
    uint32_t stall_timeout_ms = 200;   // §6.5 seed
    uint32_t dwell_ceiling_ms = 20;    // §6.2-3 backstop seed, bench-gated
    uint8_t admit_n = 3;               // §2 N_admit
    uint32_t admit_window_ms = 1000;   // §2 T_admit
    uint8_t renack_attempts = 3;       // §6.4 bounded retries
    uint32_t renack_backoff_ms = 6;    // per-attempt backoff step
    uint32_t idle_teardown_ms = 5000;  // §2 implicit teardown
    // §6.6 escape hatch: a stream whose packets are ALL clamp-rejected for
    // this long is desynced by a real outage (the TX ran ahead more than
    // the clamp allows), not under attack — resync by adopting the next
    // packet as a fresh floor. A forger must now sustain a flood for this
    // whole window to force a flush, vs. one packet without the clamp.
    uint32_t clamp_resync_ms = 500;
    // §8 fallback budgets when no profile-table entry resolves.
    uint16_t default_deadline_iframe_ms = 50;
    uint16_t default_deadline_pframe_ms = 16;
    size_t discovery_cache_cap = 64;  // §13 enumeration-flood cap
};

// What this node wants to receive: latch the first admitted tuple whose
// stream matches. local_stream_id tags deliveries (the config out-stream).
struct WantSpec {
    uint8_t local_stream_id = 0;
    uint8_t stream_type = stream_type::kRtp;
    std::optional<uint16_t> originator;  // optional pin to a specific sender
};

struct RxStreamCounters {
    uint64_t uniq = 0;        // unique packets accepted (loss denominator)
    uint64_t diversity = 0;   // duplicate copies across adapters
    uint64_t delivered = 0;
    uint64_t lost_declared = 0;  // post-diversity, pre-ARQ (§3.7 numerator)
    uint64_t recovered_arq = 0;
    uint64_t dropped_superseded = 0;
    uint64_t dropped_deadline = 0;
    uint64_t dropped_unrecoverable = 0;  // lost + not ARQ-eligible
    uint64_t nacks_sent = 0;             // NACK packets built
    uint64_t clamp_rejected = 0;         // §6.6 hits
    uint64_t resyncs = 0;                // sustained-clamp re-floors
    uint64_t table_mismatch = 0;         // §3.4 fallback packets
    uint64_t prediv_expected = 0;        // §3.7 adapter opportunities
    uint64_t prediv_lost = 0;
    uint32_t highest_seq = 0;
    // §17 gate-3 estimator: NACK→RETRANSMIT latency samples, taken only when
    // a RETRANSMIT-flagged arrival fills a NACKed gap (late originals close
    // the gap but never sample). Cumulative histograms, ms upper bounds
    // 1,2,4,8,16,32,64,+inf. nack_rtt = most-recent-NACK anchor (pure link
    // round-trip, the §5 freshness input); arq_rec = first-NACK anchor (the
    // recovery latency compared against the I-frame deadline).
    static constexpr size_t kRttBuckets = 8;
    std::array<uint64_t, kRttBuckets> nack_rtt_hist{};
    uint64_t nack_rtt_max_ms = 0;
    uint16_t nack_rtt_samples = 0;
    uint32_t nack_rtt_p95_us = 0;
    std::array<uint64_t, kRttBuckets> arq_rec_hist{};
    uint64_t arq_rec_max_ms = 0;
};

struct RxAdapterCounters {
    uint64_t rx = 0;
    bool stalled = false;
    uint64_t last_rx_ms = 0;
    // §7.3 report inputs: last per-packet RSSI and an integer EWMA (α = 1/4,
    // q4 fixed point internally). 0 until the first packet carries RSSI.
    int8_t rssi_last = 0;
    int8_t rssi_ewma = 0;
    bool have_rssi = false;
};

// One latched stream's identity + counters, for stats/reporting.
struct RxStreamInfo {
    StreamKey key;
    uint8_t local_stream_id = 0;
    uint8_t stream_type = 0;
    bool best_effort = false;  // §3.4 fallback active
    uint8_t active_profile = 0;  // last seen from the TX
    // §3.4 diagnosis: the TX's own table_version, so a caller reporting the
    // fallback can name BOTH hashes. Knowing only "they differ" leaves the
    // operator to go and read two files on two hosts to find out which end
    // moved.
    uint8_t peer_table_version = 0;
    // §3.7 (Pass 198) per-EAR opportunities, alongside the summed pair in
    // `counters`. The sum is the MEAN per-ear loss, which on a diversity
    // receiver is not "how good is the air": measured on a two-ear ground, one
    // ear saw ~100% of unique packets and the other 2.7%, so the mean read 50%
    // while every packet arrived and the picture was clean. A consumer that
    // wants air quality wants the BEST ear; one that wants "is an ear failing"
    // wants the spread. Both need the ears kept apart.
    struct AdapterLoss {
        uint8_t adapter_id = 0;
        uint64_t expected = 0;
        uint64_t lost = 0;
    };
    std::vector<AdapterLoss> per_adapter;
    RxStreamCounters counters;
};

// A NACK ready for the caller to wrap in its own common prefix and inject.
struct NackRequest {
    uint16_t target_originator = 0;
    uint32_t target_session = 0;
    uint8_t target_stream_id = 0;
    uint32_t base_seq = 0;
    std::vector<uint8_t> bitmap;
};

class RxEngine {
  public:
    // block_id + data_flags ride along so a frame-shm egress reassembler
    // (§6.3a) can group a block's symbols; the UDP egress path ignores them.
    using Deliver = std::function<void(uint8_t local_stream_id,
                                       uint32_t block_id, uint8_t data_flags,
                                       const uint8_t* payload, size_t len)>;

    struct EarlyDeliverResult {
        bool handled = false;        // symbol consumed outside seq ordering
        bool block_complete = false; // that symbol completed the frame block
    };
    // Frame-SHM reassembly is equation-oriented, not packet-order-oriented:
    // feed each first-admitted symbol immediately after diversity dedup while
    // RxEngine retains the wire sequence for loss/ARQ accounting.
    using EarlyDeliver = std::function<EarlyDeliverResult(
        const StreamKey& source, uint8_t local_stream_id, uint32_t block_id,
        uint8_t data_flags, const uint8_t* payload, size_t len)>;

    // local_table_version: this node's §3.6 hash; packets carrying a
    // different table_version drop to the best-effort profile (§3.4).
    // nullopt disables the check (no local table — dev/bench).
    RxEngine(const RxPolicy& policy, std::vector<WantSpec> wants,
             const ProfileTable* table,
             std::optional<uint8_t> local_table_version);

    // Ingest one decoded DATA packet heard on adapter_id. Deliveries (this
    // packet and any it unblocks) happen synchronously via deliver. rssi is
    // the receive RSSI in dBm (AirRxMeta), 0 = unknown (§7.3 report input).
    void on_data(uint8_t adapter_id, const DataView& v, uint64_t now_ms,
                 const Deliver& deliver, int8_t rssi = 0,
                 const EarlyDeliver& early_deliver = {});

    // A frame-SHM/cache reassembler completed this block. Retire every
    // packet gap attributable to it so queued/later ARQ cannot repair an
    // already-complete frame, and advance the generic sequence cursor.
    void complete_frame(uint8_t local_stream_id, uint32_t block_id,
                        uint64_t now_ms, const Deliver& deliver);

    // §14.3 cache ordering: delay only the first NACK for one exact block.
    // Returns false when the stream/block is not live or a NACK already fired.
    bool defer_first_nack(uint8_t local_stream_id, uint32_t block_id,
                          uint64_t not_before_ms);
    bool block_had_nack(uint8_t local_stream_id, uint32_t block_id) const;

    // Timers: dwell-ceiling gaps, deadline expiry, stall watchdog, idle
    // teardown. Call at a few-ms cadence.
    void tick(uint64_t now_ms, const Deliver& deliver);

    // Coalesced NACKs due now (respects per-seq backoff + attempt caps).
    std::vector<NackRequest> build_nacks(uint64_t now_ms);

    // Introspection for stats / LINK_REPORT (step 8).
    std::vector<RxStreamInfo> streams() const;
    std::map<uint8_t, RxAdapterCounters> adapters() const;
    uint8_t live_adapter_count() const;  // latched, non-stalled (§6.5)

    // §15.5 stats/reset: zero every latched stream's counters and each
    // adapter's rx tally (fresh measurement window). Latch/cursor/gap state,
    // RSSI EWMA and the discovery table are untouched — observability only.
    void reset_stats();

    // §15.5a Pass 67: replace every output want's sender pin and tear down
    // the previous subscription. Normal admission gates the new tuple.
    void select_originator(uint16_t originator);
    std::optional<uint16_t> selected_originator() const;

  private:
    struct Held {
        std::vector<uint8_t> payload;
        uint32_t block_id = 0;
        uint8_t flags = 0;
        bool delivered_early = false;
    };
    struct BlockInfo {
        uint64_t first_seen_ms = 0;
        uint64_t deadline_ms = 0;
        bool arq = false;
        bool iframe_class = false;
        uint64_t first_nack_not_before_ms = 0;
        bool nack_attempted = false;
    };
    struct Gap {
        uint64_t first_missing_ms = 0;
        bool declared_lost = false;
        bool superseded = false;  // §6.2-2: lost AND not NACKed
        bool fec_satisfied = false;  // completed frame needs no packet repair
        uint8_t nack_attempts = 0;
        uint64_t next_nack_ms = 0;
        bool nack_eligible = false;
        // §17 gate-3 anchors, stamped at NACK build (0 = never NACKed).
        uint64_t first_nack_ms = 0;
        uint64_t last_nack_ms = 0;
    };
    struct AdapterSeq {
        bool have = false;
        uint32_t highest = 0;
        uint64_t expected = 0;
        uint64_t received = 0;
        std::set<uint32_t> missing;
    };
    struct Stream {
        StreamKey key;
        uint8_t local_stream_id = 0;
        uint8_t stream_type = 0;
        bool best_effort = false;
        uint8_t active_profile = 0;
        uint8_t peer_table_version = 0;
        uint32_t cursor = 0;    // next seq to deliver
        uint32_t max_seq = 0;
        uint32_t max_block = 0;
        uint32_t last_delivered_block = 0;
        uint64_t last_activity_ms = 0;
        uint64_t first_clamp_ms = 0;  // start of an unbroken clamp storm
        std::map<uint32_t, Held> held;
        std::map<uint32_t, BlockInfo> blocks;
        std::map<uint32_t, Gap> gaps;
        std::set<uint32_t> completed_blocks;
        // §6.1 per-adapter highest seq FOR THIS STREAM (the §6.2-1 fast path
        // must not mix streams sharing an adapter).
        std::map<uint8_t, uint32_t> adapter_last_seq;
        std::map<uint8_t, AdapterSeq> adapter_seq;
        std::deque<uint32_t> nack_rtt_ms;  // trailing §3.10 RTT window
        RxStreamCounters counters;
    };
    struct Adapter {
        uint32_t last_seq = 0;  // highest seq heard (any latched stream)
        uint64_t last_rx_ms = 0;
        uint64_t rx = 0;
        int8_t rssi_last = 0;
        int32_t rssi_ewma_q4 = 0;  // RSSI EWMA in quarter-dBm fixed point
        bool have_rssi = false;
    };
    struct Candidate {
        uint8_t count = 0;
        uint64_t first_ms = 0;
        uint64_t last_ms = 0;
        uint8_t stream_type = 0;
        uint8_t stream_id = 0;
    };

    bool adapter_stalled(const Adapter& a, uint64_t now_ms) const;
    // nullopt when no live adapter has heard this stream yet.
    std::optional<uint32_t> min_live_adapter_seq(const Stream& s,
                                                 uint64_t now_ms) const;
    uint64_t block_deadline(const Stream& s, uint64_t first_seen_ms,
                            bool arq) const;
    void note_gaps(Stream& s, uint64_t now_ms);
    void note_adapter_seq(Stream& s, uint8_t adapter_id, uint32_t seq);
    void evaluate_gaps(Stream& s, uint64_t now_ms);
    void advance_cursor(Stream& s, uint64_t now_ms, const Deliver& deliver);
    void refloor_on_wrap(Stream& s);  // §2.1 u32 cursor wrap = desync
    std::optional<uint32_t> gap_block(const Stream& s, uint32_t seq) const;
    void mark_frame_complete(Stream& s, uint32_t block_id);
    Stream* try_latch(const DataView& v, uint64_t now_ms);

    RxPolicy policy_;
    std::vector<WantSpec> wants_;
    const ProfileTable* table_;
    std::optional<uint8_t> local_table_version_;

    std::map<uint64_t, Stream> streams_;     // key packed from StreamKey
    std::map<uint8_t, Adapter> adapters_;
    std::map<uint64_t, Candidate> discovery_;  // unlatched tuples
};

}  // namespace wblink
