// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: spatial salvage of failed FEC blocks (PROTOCOL.md §6.3b).
//
// Sits above the FrameReassembler: learns the stream's slice geometry from
// every delivered [VencFrameMeta][Annex-B] blob, and when a block finalizes
// below k, reconstructs which HEVC slices are complete among the verified
// received source chunks, replaces the erased ones with synthesized all-skip
// slices (hevc_conceal.h), and emits a complete access unit. Every refusal
// falls back to the caller's pre-§6.3b behaviour: drop the frame.
//
// Pure logic; single-threaded use on the RX loop thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "wblink/hevc_conceal.h"

namespace wblink {

struct SpatialRepairConfig {
    bool freeze_frame = true;          // §6.3b whole-frame freeze
    uint32_t max_frame_bytes = 640 * 1024;
};

struct SpatialRepairStats {
    uint64_t frames_salvaged = 0;      // emitted with >= 1 surviving slice
    uint64_t frames_frozen = 0;        // emitted as whole-frame synthesis
    uint64_t salvage_failed = 0;       // attempted, refused -> frame dropped
    uint64_t slices_synthesized = 0;   // replacement slices written
};

class SpatialRepair {
  public:
    using Emit = std::function<bool(const uint8_t* frame, size_t len)>;

    explicit SpatialRepair(const SpatialRepairConfig& cfg) : cfg_(cfg) {}

    // Feed every blob the stream egresses (fast path, FEC path, and repaired
    // frames alike): learns SPS/PPS, slice geometry, the concealment donor
    // header, POC and PTS cadence.
    void learn(const uint8_t* blob, size_t len);

    // §6.3b: attempt to repair a failed block from its surviving source
    // chunks (chunk i = blob bytes [i*s, i*s+chunk.size())). frame_len 0 =
    // unknown. Emits the rebuilt [VencFrameMeta][Annex-B] blob and returns
    // true only when the local egress accepts it. A rejected emit discards
    // tentative donor/POC/PTS state without counting salvage_failed.
    bool repair(uint16_t k, uint16_t s, uint32_t frame_len,
                const std::map<uint16_t, std::vector<uint8_t>>& sources,
                const Emit& emit);

    const SpatialRepairStats& stats() const { return stats_; }
    bool geometry_known() const { return !geometry_.empty(); }

    // Source-tuple change (§6.1a): forget the learned stream shape.
    void reset_stream();

  private:
    struct FoundNal {
        uint32_t sc_off = 0;    // start-code offset within the blob
        uint32_t nal_off = 0;   // first NAL header byte
        uint32_t end_off = 0;   // next start code / end of frame
        bool complete = false;  // every byte to end_off verified present
        bool is_vcl = false;
        hevc::SliceInfo slice;  // valid iff is_vcl && header parsed
        bool header_ok = false;
    };

    bool fail() {
        ++stats_.salvage_failed;
        return false;
    }
    bool freeze(uint32_t total_ctbs, const Emit& emit);
    void append_conceal_meta();

    SpatialRepairConfig cfg_;
    SpatialRepairStats stats_;

    // learned stream shape
    hevc::SpsInfo sps_{};
    hevc::PpsInfo pps_{};
    bool have_sps_ = false;
    bool have_pps_ = false;
    std::vector<uint32_t> geometry_;   // expected slice_segment_address list
    // last delivered picture's address list: geometry_ adopts a new shape
    // only when two consecutive delivered pictures agree (the SSC338Q GDR
    // refresh AU is a one-picture 17-slice shape whose addresses superset
    // the steady 4-slice one — single-frame adoption would let the next
    // partial loss synthesize overlapping slices).
    std::vector<uint32_t> geometry_pending_;
    hevc::SliceInfo donor_{};          // freeze-frame donor (last P slice 0)
    bool have_donor_ = false;
    // Freeze reuses the donor's short-term RPS at POC+1, which is only valid
    // in a steady state where consecutive P pictures carry the same relative
    // reference set. Cycling GOP patterns (HM lowdelay) never satisfy it; a
    // flat 1-ref stream satisfies it everywhere except the first P after an
    // IDR (smaller DPB => legitimately different set), so this is a per-frame
    // condition — last two donors' RPS bit spans equal — not a latched
    // verdict. Compared in place against donor_ (no per-frame allocation).
    bool rps_stable_ = false;
    uint32_t last_poc_ = 0;
    bool have_poc_ = false;
    uint8_t meta_template_[8] = {0};   // last delivered VencFrameMeta
    bool have_meta_ = false;
    uint32_t last_pts_ = 0;
    uint32_t pts_delta_ = 0;

    // scratch (steady-state allocation-free once warmed)
    hevc::ConcealScratch conceal_scratch_;
    std::vector<uint8_t> present_;     // byte-presence bitmap scratch
    std::vector<uint8_t> assembly_;    // frame-bytes scratch (repair())
    std::vector<uint32_t> addr_scratch_;  // per-frame address list (learn())
    std::vector<FoundNal> found_;
    std::vector<uint8_t> rebuilt_;
    hevc::SliceInfo parsed_slice_;     // reused parse target in learn()
};

}  // namespace wblink
