// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §9.4 Pass 163 sequence-derived rate probe — the shared
// schedule derivation (both ends compute the probe set from the wire seq
// alone) and the RX-side evidence window. Pure logic, injected time; the
// §3.6-hashed schedule (probe_period/probe_slot) rides ProfileTable.
#pragma once

#include <cstdint>
#include <optional>

#include "wblink/table.h"
#include "wblink/types.h"

namespace wblink {

// True when this first-send video DATA seq is a probe slot. period 0 = off.
inline bool probe_slot_hit(uint32_t seq, uint16_t period, uint16_t slot) {
    return period != 0 && seq % period == slot;
}

// §9.4 up-candidate: the mcs of the profile with the smallest id greater
// than active_profile's, resolved through the live table (never
// rung_index == mcs). nullopt when active_profile is unknown, at the top
// rung, or when the adjacent profile shares the current MCS — a probe there
// would be rate-unverifiable at the receiver.
std::optional<uint8_t> probe_up_candidate_mcs(const ProfileTable& table,
                                              uint8_t active_profile);

// RX-side probe evidence window (§9.4 Pass 163). One instance per node,
// scoped to the video stream of the accepted sender. All three normative
// guards live here:
//   1. successes are rate-verified (rx_mcs must match the candidate);
//   2. gap losses are epoch-gated (attributed only while non-probe frames
//      confirm the TX is flying the commanded rate);
//   3. CRC-errored frames attribute rate-free and seq-free (descriptor rate
//      is pre-FCS) — they never advance the gap walk.
// Any operating-context change (active_profile, table_version, sender
// identity — the caller resets on identity change) resets the window.
class McsProbeWindow {
  public:
    struct Params {
        uint32_t min_samples = 32;   // §17 seed: opinion floor
        uint32_t max_age_ms = 8000;  // §17 seed: evidence freshness
        uint32_t gap_horizon = 128;  // §17 seed: seqs behind head before a
                                     // missing probe slot becomes a loss
    };

    McsProbeWindow(const ProfileTable* table,
                   std::optional<uint8_t> local_table_version,
                   const Params& params)
        : table_(table), tv_(local_table_version), p_(params) {}

    // One received video DATA frame (any adapter, pre-dedup — the window
    // dedups by seq). rx_mcs = AirRxMeta PHY rate, kUplinkRxMcsUnknown when
    // the backend has none (the window then never confirms and stays inert).
    void on_data(uint32_t seq, uint8_t active_profile, uint8_t table_version,
                 uint8_t rx_mcs, uint64_t now_ms);

    // count CRC-errored frames whose descriptor rate read rx_mcs (guard 3).
    void on_crc_frames(uint8_t rx_mcs, uint32_t count, uint64_t now_ms);

    // §3.5 probe_per: up-candidate failure rate in ‰ once min_samples
    // accumulated within max_age_ms; nullopt otherwise (reports kNoProbe).
    std::optional<uint16_t> probe_per(uint64_t now_ms) const;

    // Caller-driven reset (sender identity change).
    void reset();

    // Introspection (tests/stats).
    uint32_t successes() const { return successes_; }
    uint32_t failures() const { return failures_; }
    bool confirmed() const { return confirmed_; }
    std::optional<uint8_t> candidate_mcs() const { return candidate_; }

  private:
    void reset_context(uint8_t active_profile, uint64_t now_ms);
    void advance_gap_walk(uint64_t now_ms);
    bool seen(uint32_t seq) const;
    void mark_seen(uint32_t seq);

    const ProfileTable* table_;
    std::optional<uint8_t> tv_;
    Params p_;

    bool have_context_ = false;
    uint8_t profile_ = 0;                  // sender's active_profile
    uint8_t selected_mcs_ = 0;             // that profile's mcs
    std::optional<uint8_t> candidate_;     // up-candidate mcs (nullopt = idle)
    bool confirmed_ = false;               // guard 2: TX flying commanded rate
    uint64_t window_start_ms_ = 0;
    uint32_t successes_ = 0;
    uint32_t failures_ = 0;

    // Seq dedup/gap state: bitmap over the last kSeenBits seqs behind head.
    static constexpr uint32_t kSeenBits = 1024;
    bool have_head_ = false;
    uint32_t head_seq_ = 0;    // highest seq observed in this context
    uint32_t walked_seq_ = 0;  // gap walk: probe slots at/below are settled
    uint64_t seen_[kSeenBits / 64] = {};
};

}  // namespace wblink
