// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/rx.h"

#include <algorithm>
#include <iterator>

#include "wblink/clamp.h"

namespace wblink {

namespace {

uint64_t pack_key(const StreamKey& k) {
    return (static_cast<uint64_t>(k.originator) << 40) |
           (static_cast<uint64_t>(k.session_id) << 8) |
           static_cast<uint64_t>(k.stream_id);
}

uint64_t pack_key(const CommonPrefix& p, uint8_t stream_id) {
    return pack_key(StreamKey{p.originator, p.session_id, stream_id});
}

// §17 gate-3 histogram bucket for a latency in ms: upper bounds
// 1,2,4,8,16,32,64,+inf (index 0..7).
size_t rtt_bucket(uint64_t ms) {
    size_t b = 0;
    while (b < RxStreamCounters::kRttBuckets - 1 && ms > (1ull << b)) {
        ++b;
    }
    return b;
}

uint32_t p95_us(const std::deque<uint32_t>& samples) {
    if (samples.empty()) return 0;
    std::deque<uint32_t> ordered = samples;
    std::sort(ordered.begin(), ordered.end());
    const size_t rank = std::max<size_t>(
        1, (ordered.size() * 950u + 999u) / 1000u);
    return ordered[rank - 1] * 1000u;
}

}  // namespace

RxEngine::RxEngine(const RxPolicy& policy, std::vector<WantSpec> wants,
                   const ProfileTable* table,
                   std::optional<uint8_t> local_table_version)
    : policy_(policy),
      wants_(std::move(wants)),
      table_(table),
      local_table_version_(local_table_version) {}

bool RxEngine::adapter_stalled(const Adapter& a, uint64_t now_ms) const {
    // §6.5: zero frames for stall_timeout while a sibling still delivers.
    if (now_ms - a.last_rx_ms <= policy_.stall_timeout_ms) {
        return false;
    }
    for (const auto& [id, sib] : adapters_) {
        if (&sib != &a && now_ms - sib.last_rx_ms <= policy_.stall_timeout_ms) {
            return true;
        }
    }
    return false;  // everyone quiet = link quiet, not a stalled adapter
}

std::optional<uint32_t> RxEngine::min_live_adapter_seq(const Stream& s,
                                                       uint64_t now_ms) const {
    std::optional<uint32_t> min_seq;
    for (const auto& [id, last_seq] : s.adapter_last_seq) {
        const auto ait = adapters_.find(id);
        if (ait == adapters_.end() || adapter_stalled(ait->second, now_ms)) {
            continue;  // §6.5: stalled adapters leave the fast path
        }
        if (!min_seq || last_seq < *min_seq) {
            min_seq = last_seq;
        }
    }
    return min_seq;
}

uint64_t RxEngine::block_deadline(const Stream& s, uint64_t first_seen_ms,
                                  bool arq) const {
    // §8: budget(profile, importance). ARQ-class blocks (I-frame) get the
    // longer budget (§4.1 deadline coupling).
    uint16_t budget = arq ? policy_.default_deadline_iframe_ms
                          : policy_.default_deadline_pframe_ms;
    if (table_ != nullptr) {
        for (const Profile& p : table_->profiles) {
            if (p.id == s.active_profile) {
                budget = arq ? p.arq_deadline_iframe_ms
                             : p.arq_deadline_pframe_ms;
                break;
            }
        }
    }
    return first_seen_ms + budget;
}

RxEngine::Stream* RxEngine::try_latch(const DataView& v, uint64_t now_ms) {
    // Find an unsatisfied want matching this tuple.
    const WantSpec* want = nullptr;
    std::optional<uint64_t> replace_key;
    for (const WantSpec& w : wants_) {
        if (w.stream_type != v.hdr.stream_type) {
            continue;
        }
        if (w.originator && *w.originator != v.hdr.prefix.originator) {
            continue;
        }
        const Stream* incumbent = nullptr;
        uint64_t incumbent_key = 0;
        for (const auto& [key, s] : streams_) {
            if (s.local_stream_id == w.local_stream_id) {
                incumbent = &s;
                incumbent_key = key;
                break;
            }
        }
        if (incumbent == nullptr) {
            want = &w;
            break;
        }
        // A sender reboot creates a new session for the same physical
        // originator. Admit it while the old tuple remains live, then replace
        // the incumbent atomically; do not give a different originator this
        // immediate-preemption path.
        if (incumbent->key.originator == v.hdr.prefix.originator &&
            incumbent->key.session_id != v.hdr.prefix.session_id) {
            want = &w;
            replace_key = incumbent_key;
            break;
        }
    }
    if (want == nullptr) {
        return nullptr;
    }

    // §2 admission control: N_admit packets within T_admit before the tuple
    // qualifies. A one-shot forged packet never qualifies.
    const uint64_t key = pack_key(v.hdr.prefix, v.hdr.stream_id);
    Candidate& c = discovery_[key];
    if (c.count == 0 || now_ms - c.first_ms > policy_.admit_window_ms) {
        c = Candidate{};
        c.first_ms = now_ms;
    }
    ++c.count;
    c.last_ms = now_ms;
    c.stream_type = v.hdr.stream_type;  // most-recent traffic wins (§2)
    c.stream_id = v.hdr.stream_id;

    // §13 enumeration-flood cap: LRU-age the discovery cache. Never evict
    // the tuple just touched (ties on last_ms are possible).
    while (discovery_.size() > policy_.discovery_cache_cap) {
        auto oldest = discovery_.end();
        for (auto it = discovery_.begin(); it != discovery_.end(); ++it) {
            if (it->first == key) {
                continue;
            }
            if (oldest == discovery_.end() ||
                it->second.last_ms < oldest->second.last_ms) {
                oldest = it;
            }
        }
        if (oldest == discovery_.end()) {
            break;
        }
        discovery_.erase(oldest);
    }

    const auto cit = discovery_.find(key);
    if (cit == discovery_.end() || cit->second.count < policy_.admit_n) {
        return nullptr;
    }
    discovery_.erase(cit);

    if (replace_key) {
        streams_.erase(*replace_key);
    }

    // Latch (§2): adopt the first-seen seq as the startup floor.
    Stream s;
    s.key = StreamKey{v.hdr.prefix.originator, v.hdr.prefix.session_id,
                      v.hdr.stream_id};
    s.local_stream_id = want->local_stream_id;
    s.stream_type = v.hdr.stream_type;
    s.cursor = v.hdr.seq;
    s.max_seq = v.hdr.seq;
    s.max_block = v.hdr.block_id;
    s.last_delivered_block = v.hdr.block_id;
    s.last_activity_ms = now_ms;
    return &streams_.emplace(key, std::move(s)).first->second;
}

void RxEngine::on_data(uint8_t adapter_id, const DataView& v, uint64_t now_ms,
                       const Deliver& deliver, int8_t rssi,
                       const EarlyDeliver& early_deliver) {
    Adapter& a = adapters_[adapter_id];
    a.last_rx_ms = now_ms;
    ++a.rx;
    if (rssi != 0) {  // 0 = meta carried no RSSI (§7.3)
        a.rssi_last = rssi;
        const int32_t q4 = static_cast<int32_t>(rssi) * 16;
        a.rssi_ewma_q4 = a.have_rssi ? a.rssi_ewma_q4 + (q4 - a.rssi_ewma_q4) / 4
                                     : q4;
        a.have_rssi = true;
    }

    const uint64_t key = pack_key(v.hdr.prefix, v.hdr.stream_id);
    Stream* s = nullptr;
    if (const auto it = streams_.find(key); it != streams_.end()) {
        s = &it->second;
    } else {
        s = try_latch(v, now_ms);
    }
    if (s == nullptr) {
        return;
    }

    s->last_activity_ms = now_ms;
    s->active_profile = v.hdr.active_profile;

    // §3.4: table_version mismatch drops the stream to the best-effort
    // default profile — deliver by diversity, never NACK, no supersession or
    // deadline logic. Sticky once seen (conservative).
    if (local_table_version_ &&
        v.hdr.table_version != *local_table_version_) {
        if (!s->best_effort) {
            s->best_effort = true;
        }
        ++s->counters.table_mismatch;
    }

    // §6.6 plausible-forward clamps. The block clamp references max_block
    // (newest legitimately heard block), NOT the last delivered block: in a
    // deep fade the cursor advances by deadline-skips without delivering,
    // and a delivered-block reference would freeze and clamp-reject the
    // entire recovering stream. Ratcheting max_block costs an attacker one
    // accepted in-clamp packet per +K step — the §6.6 accepted residual.
    if (!plausible_forward(s->cursor, v.hdr.seq, policy_.fwd_clamp_pkts) ||
        !plausible_forward(s->max_block, v.hdr.block_id,
                           policy_.fwd_clamp_blocks)) {
        ++s->counters.clamp_rejected;
        // Sustained-clamp escape: if NOTHING has passed the clamp for
        // clamp_resync_ms, the stream is desynced by a real outage — adopt
        // this packet as a fresh floor (like a re-latch, §2 startup floor).
        if (s->first_clamp_ms == 0) {
            s->first_clamp_ms = now_ms;
            return;
        }
        // Underflow guard: a now_ms at or before the window start counts as
        // zero elapsed. Without it, a 1 ms backward step in injected time
        // would make the elapsed u64 huge and fire the resync (= flush) off
        // a single clamp-rejected packet — exactly what §6.6 must prevent.
        if (now_ms <= s->first_clamp_ms ||
            now_ms - s->first_clamp_ms < policy_.clamp_resync_ms) {
            return;
        }
        ++s->counters.resyncs;
        s->cursor = v.hdr.seq;
        s->max_seq = v.hdr.seq;
        s->max_block = v.hdr.block_id;
        s->last_delivered_block = v.hdr.block_id;
        s->held.clear();
        s->gaps.clear();
        s->blocks.clear();
        s->completed_blocks.clear();
        s->adapter_last_seq.clear();
        // fall through: this packet is accepted under the new floor
    }
    s->first_clamp_ms = 0;  // any accepted packet ends the storm window

    if ((v.hdr.data_flags & data_flags::kRetransmit) == 0) {
        note_adapter_seq(*s, adapter_id, v.hdr.seq);
    }

    // §6.1 dedup: first copy wins; later copies feed the diversity gauge.
    if (v.hdr.seq < s->cursor || s->held.count(v.hdr.seq) != 0) {
        ++s->counters.diversity;
        auto& als = s->adapter_last_seq[adapter_id];
        als = std::max(als, v.hdr.seq);
        return;
    }

    // A declared-lost gap filled late (normally by a RETRANSMIT).
    if (const auto git = s->gaps.find(v.hdr.seq); git != s->gaps.end()) {
        if (git->second.declared_lost) {
            ++s->counters.recovered_arq;
            // §17 gate-3 samples: only a RETRANSMIT-flagged fill of a seq
            // we actually NACKed measures the loop; a late original closes
            // the gap without sampling. Injected time may step backward
            // between build_nacks and here — clamp to zero.
            const Gap& g = git->second;
            if (g.last_nack_ms != 0 &&
                (v.hdr.data_flags & data_flags::kRetransmit) != 0) {
                const uint64_t rtt =
                    now_ms > g.last_nack_ms ? now_ms - g.last_nack_ms : 0;
                const uint64_t rec =
                    now_ms > g.first_nack_ms ? now_ms - g.first_nack_ms : 0;
                ++s->counters.nack_rtt_hist[rtt_bucket(rtt)];
                s->counters.nack_rtt_max_ms =
                    std::max(s->counters.nack_rtt_max_ms, rtt);
                s->nack_rtt_ms.push_back(static_cast<uint32_t>(
                    std::min<uint64_t>(rtt, UINT32_MAX)));
                if (s->nack_rtt_ms.size() > 120) {
                    s->nack_rtt_ms.pop_front();
                }
                ++s->counters.arq_rec_hist[rtt_bucket(rec)];
                s->counters.arq_rec_max_ms =
                    std::max(s->counters.arq_rec_max_ms, rec);
            }
        }
        s->gaps.erase(git);
    }

    ++s->counters.uniq;
    s->max_seq = std::max(s->max_seq, v.hdr.seq);
    s->counters.highest_seq = s->max_seq;
    auto& als = s->adapter_last_seq[adapter_id];
    als = std::max(als, v.hdr.seq);

    if (!s->best_effort) {
        BlockInfo& b = s->blocks[v.hdr.block_id];
        if (b.first_seen_ms == 0) {
            b.first_seen_ms = now_ms;
        }
        b.arq = b.arq ||
                (v.hdr.data_flags &
                 (data_flags::kArq | data_flags::kPframeArq)) != 0;
        b.iframe_class =
            b.iframe_class || (v.hdr.data_flags & data_flags::kArq) != 0;
        b.deadline_ms = block_deadline(*s, b.first_seen_ms, b.iframe_class);
        s->max_block = std::max(s->max_block, v.hdr.block_id);
    }

    Held h;
    h.block_id = v.hdr.block_id;
    h.flags = v.hdr.data_flags;
    if (v.payload_len > 0) {
        h.payload.assign(v.payload, v.payload + v.payload_len);
    }
    auto [held_it, inserted] = s->held.emplace(v.hdr.seq, std::move(h));
    (void)inserted;  // uniqueness was established before constructing Held

    note_gaps(*s, now_ms);
    if (early_deliver) {
        const EarlyDeliverResult early = early_deliver(
            s->key, s->local_stream_id, v.hdr.block_id,
            v.hdr.data_flags, v.payload, v.payload_len);
        held_it->second.delivered_early = early.handled;
        if (early.handled) {
            ++s->counters.delivered;
        }
        if (early.block_complete) {
            mark_frame_complete(*s, v.hdr.block_id);
        }
    }
    evaluate_gaps(*s, now_ms);
    advance_cursor(*s, now_ms, deliver);
}

void RxEngine::complete_frame(uint8_t local_stream_id, uint32_t block_id,
                              uint64_t now_ms, const Deliver& deliver) {
    for (auto& [key, s] : streams_) {
        (void)key;
        if (s.local_stream_id != local_stream_id) {
            continue;
        }
        mark_frame_complete(s, block_id);
        evaluate_gaps(s, now_ms);
        advance_cursor(s, now_ms, deliver);
        return;
    }
}

bool RxEngine::defer_first_nack(uint8_t local_stream_id, uint32_t block_id,
                                uint64_t not_before_ms) {
    for (auto& [key, s] : streams_) {
        (void)key;
        if (s.local_stream_id != local_stream_id) continue;
        const auto bit = s.blocks.find(block_id);
        if (bit == s.blocks.end()) return false;
        if (bit->second.nack_attempted) return false;
        BlockInfo& block = bit->second;
        const uint64_t bounded = block.deadline_ms != 0
                                     ? std::min(not_before_ms,
                                                block.deadline_ms)
                                     : not_before_ms;
        block.first_nack_not_before_ms =
            std::max(block.first_nack_not_before_ms, bounded);
        for (auto& [seq, gap] : s.gaps) {
            const auto owner = gap_block(s, seq);
            if (owner && *owner == block_id && gap.nack_attempts == 0) {
                gap.next_nack_ms = std::max(gap.next_nack_ms, bounded);
            }
        }
        return true;
    }
    return false;
}

bool RxEngine::block_had_nack(uint8_t local_stream_id,
                              uint32_t block_id) const {
    for (const auto& [key, s] : streams_) {
        (void)key;
        if (s.local_stream_id != local_stream_id) continue;
        const auto bit = s.blocks.find(block_id);
        return bit != s.blocks.end() && bit->second.nack_attempted;
    }
    return false;
}

void RxEngine::note_adapter_seq(Stream& s, uint8_t adapter_id, uint32_t seq) {
    AdapterSeq& a = s.adapter_seq[adapter_id];
    if (!a.have) {
        a.have = true;
        a.highest = seq;
        ++a.expected;
        ++a.received;
        return;
    }
    if (seq > a.highest) {
        const uint32_t delta = seq - a.highest;
        if (delta > policy_.fwd_clamp_pkts) {
            // A recovered/stalled adapter may legitimately jump far ahead.
            // Re-anchor rather than charging its silent interval as RF loss.
            a.highest = seq;
            a.missing.clear();
            ++a.expected;
            ++a.received;
            return;
        }
        for (uint32_t m = a.highest + 1; m < seq; ++m) {
            a.missing.insert(m);
        }
        a.expected += delta;
        ++a.received;
        a.highest = seq;
        return;
    }
    if (a.missing.erase(seq) != 0) {
        ++a.received;  // bounded out-of-order fill, exactly once
    }
}

void RxEngine::note_gaps(Stream& s, uint64_t now_ms) {
    // Bounded by the §6.6 clamp: max_seq - cursor <= fwd_clamp_pkts.
    for (uint32_t m = s.cursor; m < s.max_seq; ++m) {
        if (s.held.count(m) == 0 && s.gaps.count(m) == 0) {
            Gap g;
            g.first_missing_ms = now_ms;
            if (const auto block = gap_block(s, m);
                block && s.completed_blocks.count(*block) != 0) {
                g.fec_satisfied = true;
            }
            s.gaps.emplace(m, g);
        }
    }
}

std::optional<uint32_t> RxEngine::gap_block(const Stream& s,
                                            uint32_t seq) const {
    const auto upper = s.held.lower_bound(seq);
    const auto lower = upper == s.held.begin() ? s.held.end()
                                                : std::prev(upper);
    if (upper != s.held.end() && upper->first == seq) {
        return upper->second.block_id;
    }
    if (upper != s.held.end() && lower != s.held.end()) {
        if (upper->second.block_id == lower->second.block_id) {
            return upper->second.block_id;
        }
        // EOB now closes the repair tail. A gap after a received EOB belongs
        // to the following block; otherwise it is the preceding block's tail.
        return (lower->second.flags & data_flags::kEndOfBlock) != 0
                   ? upper->second.block_id
                   : lower->second.block_id;
    }
    if (upper != s.held.end()) {
        return upper->second.block_id;
    }
    if (lower != s.held.end()) {
        return lower->second.block_id;
    }
    return std::nullopt;
}

void RxEngine::mark_frame_complete(Stream& s, uint32_t block_id) {
    s.completed_blocks.insert(block_id);
    // Keep only a tiny recent watermark set; the plausible-forward window is
    // four blocks and a completed block older than that cannot acquire a new
    // in-window gap.
    while (s.completed_blocks.size() >
           static_cast<size_t>(policy_.fwd_clamp_blocks + 2)) {
        s.completed_blocks.erase(s.completed_blocks.begin());
    }
    for (auto& [seq, gap] : s.gaps) {
        const auto owner = gap_block(s, seq);
        if (owner && *owner == block_id) {
            gap.fec_satisfied = true;
            gap.nack_eligible = false;
        }
    }
}

void RxEngine::evaluate_gaps(Stream& s, uint64_t now_ms) {
    const std::optional<uint32_t> min_live = min_live_adapter_seq(s, now_ms);
    for (auto& [m, g] : s.gaps) {
        if (g.fec_satisfied) {
            g.nack_eligible = false;
            continue;
        }
        // §6.2-2 supersession: the nearest received seq above bounds the
        // gap's block from above; if even that is older than the newest
        // block, the gap's block is superseded. best-effort streams skip
        // all block logic (§3.4).
        std::optional<uint32_t> block_above;
        if (const auto ub = s.held.upper_bound(m); ub != s.held.end()) {
            block_above = ub->second.block_id;
        }
        if (!s.best_effort && !g.superseded && block_above &&
            *block_above < s.max_block) {
            g.superseded = true;
            g.nack_eligible = false;
        }

        // Declaration short-circuits, priority order (§6.2):
        bool declare = false;
        if (!g.declared_lost) {
            if (min_live && *min_live > m) {
                declare = true;  // 1. all live adapters advanced past it
            } else if (now_ms - g.first_missing_ms >=
                       policy_.dwell_ceiling_ms) {
                declare = true;  // 3. dwell-ceiling backstop
            }
        }
        if (declare) {
            g.declared_lost = true;
            ++s.counters.lost_declared;
            if (!s.best_effort && !g.superseded) {
                // §6.4 eligibility: ARQ-flagged block, within deadline.
                bool arq = false;
                uint64_t deadline = 0;
                const uint32_t probe_block =
                    block_above ? *block_above : s.max_block;
                if (const auto bit = s.blocks.find(probe_block);
                    bit != s.blocks.end()) {
                    arq = bit->second.arq;
                    deadline = bit->second.deadline_ms;
                }
                if (const auto bb = s.blocks.find(s.last_delivered_block);
                    bb != s.blocks.end()) {
                    arq = arq || bb->second.arq;
                }
                if (arq && (deadline == 0 || now_ms < deadline)) {
                    g.nack_eligible = true;
                    g.next_nack_ms = now_ms;
                    if (const auto bit = s.blocks.find(probe_block);
                        bit != s.blocks.end()) {
                        g.next_nack_ms = std::max(
                            g.next_nack_ms,
                            bit->second.first_nack_not_before_ms);
                    }
                }
            }
        }
    }
}

void RxEngine::advance_cursor(Stream& s, uint64_t now_ms,
                              const Deliver& deliver) {
    while (s.cursor <= s.max_seq) {
        if (const auto h = s.held.find(s.cursor); h != s.held.end()) {
            if (!h->second.delivered_early) {
                deliver(s.local_stream_id, h->second.block_id,
                        h->second.flags, h->second.payload.data(),
                        h->second.payload.size());
                ++s.counters.delivered;
            }
            s.last_delivered_block = h->second.block_id;
            s.held.erase(h);
            s.gaps.erase(s.cursor);
            ++s.cursor;
            // Prune block info older than what we can still reference.
            while (!s.blocks.empty() &&
                   s.blocks.begin()->first < s.last_delivered_block) {
                s.blocks.erase(s.blocks.begin());
            }
            continue;
        }
        // Missing at the cursor: "drop" = stop recovering + advance (§6.3),
        // decided by supersession / deadline / unrecoverable-loss.
        const auto git = s.gaps.find(s.cursor);
        if (git == s.gaps.end()) {
            break;  // not yet noted (max_seq == cursor); nothing to decide
        }
        Gap& g = git->second;
        bool skip = false;
        if (g.fec_satisfied) {
            skip = true;  // frame already emitted; no packet-level drop stat
        } else if (g.superseded) {
            ++s.counters.dropped_superseded;
            skip = true;
        } else if (!s.best_effort) {
            // Deadline: bound the gap's block by the nearest held above.
            std::optional<uint64_t> deadline;
            if (const auto ub = s.held.upper_bound(s.cursor);
                ub != s.held.end()) {
                if (const auto bit = s.blocks.find(ub->second.block_id);
                    bit != s.blocks.end()) {
                    deadline = bit->second.deadline_ms;
                }
            }
            if (deadline && now_ms >= *deadline) {
                ++s.counters.dropped_deadline;
                skip = true;
            } else if (g.declared_lost && !g.nack_eligible) {
                ++s.counters.dropped_unrecoverable;
                skip = true;
            }
        } else if (g.declared_lost) {
            ++s.counters.dropped_unrecoverable;  // best-effort: no recovery
            skip = true;
        }
        if (!skip) {
            break;
        }
        s.gaps.erase(git);
        ++s.cursor;
    }
}

void RxEngine::tick(uint64_t now_ms, const Deliver& deliver) {
    for (auto it = streams_.begin(); it != streams_.end();) {
        Stream& s = it->second;
        // Underflow guard (see the §6.6 resync): a tick whose now_ms lags a
        // packet's stamp by 1 ms must not read as 584 My of idleness.
        if (now_ms > s.last_activity_ms &&
            now_ms - s.last_activity_ms > policy_.idle_teardown_ms) {
            it = streams_.erase(it);  // §2 implicit teardown
            continue;
        }
        evaluate_gaps(s, now_ms);
        advance_cursor(s, now_ms, deliver);
        ++it;
    }
    // Age the discovery cache alongside the admission window.
    for (auto it = discovery_.begin(); it != discovery_.end();) {
        if (now_ms - it->second.last_ms > 4ull * policy_.admit_window_ms) {
            it = discovery_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<NackRequest> RxEngine::build_nacks(uint64_t now_ms) {
    std::vector<NackRequest> out;
    for (auto& [key, s] : streams_) {
        if (s.best_effort) {
            continue;  // §3.4: never NACK
        }
        // Collect due, eligible seqs.
        std::vector<uint32_t> due;
        for (auto& [m, g] : s.gaps) {
            if (g.declared_lost && g.nack_eligible && !g.superseded &&
                !g.fec_satisfied &&
                g.nack_attempts < policy_.renack_attempts &&
                g.next_nack_ms <= now_ms) {
                due.push_back(m);
            }
        }
        if (due.empty()) {
            continue;
        }
        // One coalesced bitmap per stream per return window (§6.4).
        const uint32_t base = due.front();
        NackRequest req;
        req.target_originator = s.key.originator;
        req.target_session = s.key.session_id;
        req.target_stream_id = s.key.stream_id;
        req.base_seq = base;
        for (const uint32_t m : due) {
            const uint32_t bit = m - base;
            if (bit >= 255u * 8u) {
                break;  // bitmap_len is u8; the rest goes next window
            }
            if (req.bitmap.size() <= bit / 8) {
                req.bitmap.resize(bit / 8 + 1, 0);
            }
            req.bitmap[bit / 8] =
                static_cast<uint8_t>(req.bitmap[bit / 8] | (1u << (bit % 8)));
            Gap& g = s.gaps[m];
            ++g.nack_attempts;
            if (const auto owner = gap_block(s, m); owner) {
                if (const auto block_it = s.blocks.find(*owner);
                    block_it != s.blocks.end()) {
                    block_it->second.nack_attempted = true;
                }
            }
            g.next_nack_ms =
                now_ms + policy_.renack_backoff_ms * g.nack_attempts;
            // §17 gate-3 anchors. Build time, not air time: the §7.2 pacer
            // may hold the batch up to one return window, and that hold is
            // part of the recovery latency being measured.
            if (g.first_nack_ms == 0) {
                g.first_nack_ms = now_ms;
            }
            g.last_nack_ms = now_ms;
        }
        ++s.counters.nacks_sent;
        out.push_back(std::move(req));
    }
    return out;
}

std::vector<RxStreamInfo> RxEngine::streams() const {
    std::vector<RxStreamInfo> out;
    for (const auto& [key, s] : streams_) {
        RxStreamInfo info;
        info.key = s.key;
        info.local_stream_id = s.local_stream_id;
        info.stream_type = s.stream_type;
        info.best_effort = s.best_effort;
        info.active_profile = s.active_profile;
        info.counters = s.counters;
        info.counters.nack_rtt_samples = static_cast<uint16_t>(
            std::min<size_t>(s.nack_rtt_ms.size(), UINT16_MAX));
        info.counters.nack_rtt_p95_us = p95_us(s.nack_rtt_ms);
        for (const auto& [adapter, a] : s.adapter_seq) {
            (void)adapter;
            info.counters.prediv_expected += a.expected;
            info.counters.prediv_lost += a.expected - a.received;
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::map<uint8_t, RxAdapterCounters> RxEngine::adapters() const {
    std::map<uint8_t, RxAdapterCounters> out;
    // Note: stalled evaluation needs "now"; callers get last_rx and compute,
    // but we also expose the verdict against the freshest sibling activity.
    uint64_t newest = 0;
    for (const auto& [id, a] : adapters_) {
        newest = std::max(newest, a.last_rx_ms);
    }
    for (const auto& [id, a] : adapters_) {
        RxAdapterCounters c;
        c.rx = a.rx;
        c.last_rx_ms = a.last_rx_ms;
        c.stalled = (newest > a.last_rx_ms) &&
                    (newest - a.last_rx_ms > policy_.stall_timeout_ms);
        c.rssi_last = a.rssi_last;
        c.rssi_ewma = static_cast<int8_t>(a.rssi_ewma_q4 / 16);
        c.have_rssi = a.have_rssi;
        out.emplace(id, c);
    }
    return out;
}

uint8_t RxEngine::live_adapter_count() const {
    uint8_t live = 0;
    for (const auto& [id, c] : adapters()) {
        live = static_cast<uint8_t>(live + (c.stalled ? 0 : 1));
    }
    return live;
}

void RxEngine::reset_stats() {
    // Zero the observability counters only; latch/cursor/gap machinery and
    // per-adapter liveness timing (last_rx_ms, RSSI EWMA) are preserved so a
    // reset mid-flight cannot perturb delivery or the §6.5 stall verdict.
    for (auto& [key, s] : streams_) {
        s.counters = {};
        s.nack_rtt_ms.clear();
        for (auto& [adapter, a] : s.adapter_seq) {
            (void)adapter;
            a.expected = 0;
            a.received = 0;
            a.missing.clear();
        }
    }
    for (auto& [id, a] : adapters_) {
        a.rx = 0;
    }
}

}  // namespace wblink
