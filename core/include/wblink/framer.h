// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: per-stream TX framer (PROTOCOL.md §5.1).
//
// One ingress datagram -> one DATA frame (no fragmentation, §5.1 invariant;
// oversize is dropped with a stat, never truncated). Block boundaries:
//
//  - RTP streams (§4): a block is one RTP frame. The RTP **marker bit** marks
//    this packet as END_OF_BLOCK; a **timestamp change** without a preceding
//    marker starts a new block at this packet — the previous block then never
//    carried EOB, which is harmless: RX supersession keys off block_id change
//    (§6.2-2); EOB is an accelerator (return-window anchor, §7.2), not
//    load-bearing for boundaries. Unparseable payloads on an RTP stream fall
//    back to one-datagram-one-block.
//  - Non-RTP streams: one datagram = one block, EOB set, ARQ=0.
//
// ARQ classifier (§4.1), selected per stream by FramerConfig::classifier:
//
//  - kH264 / kH265: the NAL-type classifier (nal.h) — IDR/parameter-set NALs
//    mark the block important, stamped from the FIRST packet of the block
//    (FU fragments carry the type in every fragment). Importance is sticky
//    for the rest of the block (a STAP(SPS,PPS) opener flags the whole AU).
//  - kSize (fallback): once a block's cumulative payload crosses
//    classifier_size_threshold, this and subsequent packets are stamped
//    ARQ=1. Earlier packets of the same block stay unstamped — acceptable
//    because any surviving flagged packet reveals the block's eligibility
//    (§3.2 redundant-metadata rule).
//
// Pure tick-free logic: time is injected, emission is a callback. No sockets,
// no clocks, no allocation on the datagram path (encode into a caller-scoped
// buffer inside emit).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "wblink/nal.h"
#include "wblink/types.h"
#include "wblink/wire.h"

namespace wblink {

struct FramerConfig {
    uint16_t originator = 0;
    uint32_t session_id = 0;
    uint8_t stream_id = 0;
    uint8_t stream_type = stream_type::kUnknown;
    uint16_t destination = 0;  // §3.1 advisory; 0 = broadcast
    // §4.1 classifier for RTP streams; ignored for other profiles.
    RtpClassifier classifier = RtpClassifier::kSize;
    // kSize mode: cumulative block bytes above this => important.
    uint32_t classifier_size_threshold = 8 * 1024;
};

struct FramerStats {
    uint64_t datagrams = 0;
    uint64_t frames = 0;
    uint64_t blocks = 0;
    uint64_t oversize_ingress = 0;  // §5.1: dropped, never truncated
};

class Framer {
  public:
    // emit(frame, frame_len, hdr, now_ms): frame is valid only during the
    // call; hdr carries the stamped fields for ring bookkeeping.
    using Emit = std::function<void(const uint8_t* frame, size_t frame_len,
                                    const DataHeader& hdr, uint64_t now_ms)>;

    explicit Framer(const FramerConfig& cfg) : cfg_(cfg) {}

    // §9/§3.2: the TX operating point stamped on every packet. Static until
    // the step-8 adaptive selector drives it.
    void set_operating_point(uint8_t active_profile, uint8_t table_version) {
        active_profile_ = active_profile;
        table_version_ = table_version;
    }

    // Extra data_flags OR'd into every outgoing DATA header while set —
    // §11.6 CSA_ARMED (the craft's implicit, diversity-carried campaign ACK).
    void set_extra_flags(uint8_t f) { extra_flags_ = f; }

    // Returns false iff the datagram was dropped (oversize).
    bool on_datagram(const uint8_t* data, size_t len, uint64_t now_ms,
                     const Emit& emit);

    const FramerStats& stats() const { return stats_; }
    uint32_t next_seq() const { return next_seq_; }
    uint32_t current_block() const { return block_id_; }

    // §15.5 stats/reset: zero the cumulative counters (state untouched).
    void reset_stats() { stats_ = {}; }

  private:
    FramerConfig cfg_;
    FramerStats stats_;
    uint8_t active_profile_ = 0;
    uint8_t table_version_ = 0;
    uint8_t extra_flags_ = 0;

    uint32_t next_seq_ = 0;
    uint32_t block_id_ = 0;
    bool block_open_ = false;   // first datagram starts block 0
    uint32_t block_bytes_ = 0;
    bool block_arq_ = false;

    // RTP boundary state.
    bool have_rtp_ts_ = false;
    uint32_t last_rtp_ts_ = 0;
    bool prev_was_marker_ = false;
};

}  // namespace wblink
