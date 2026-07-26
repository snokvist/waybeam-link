// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §3.9 Pass 106 latch-triggered decoder bootstrap schedule.
//
// A fresh latch is itself a bootstrap event. The link cannot observe decoder
// readiness — VFRM v1 carries no consumer generation (§15.4), and a consumer
// draining the egress ring with its display pipeline down is indistinguishable
// from one that is decoding — so "a local decoder is newly attached" is not a
// condition this layer can detect. A first latch is the one moment it can, and
// on a GDR craft it is the only moment at which an IRAP is guaranteed absent
// from everything that consumer will ever see.
//
// The repeat is BOUNDED because the stop condition is otherwise unobservable:
// a craft with venc.recovery_enabled false, or one whose encoder never honours
// the request, must not draw a return every second for the rest of the flight.
// The one early exit the link can observe is an IRAP reaching a frame-SHM
// egress ring (note_irap); RTP egress is not parsed, so there the attempt
// bound is the only stop.
//
// Pure and time-injected like the rest of core: the caller supplies the latched
// set and the clock and gets back the streams due an emission this tick.
#pragma once

#include <cstdint>
#include <vector>

#include "wblink/types.h"

namespace wblink {

// One latched RTP stream: its wire identity plus the local egress stream id
// that an IRAP observation is reported against.
struct LatchStream {
    StreamKey key;
    uint8_t local_stream_id = 0;
};

class LatchRecovery {
  public:
    static constexpr uint8_t kAttempts = 5;
    static constexpr uint64_t kPeriodMs = 1000;

    // Reconcile against the currently latched set and return the keys due a
    // RECOVERY_REQUEST now, counting an attempt against each. Keys absent from
    // `latched` are forgotten, so a torn-down stream that latches again re-arms
    // the full schedule rather than resuming a spent one.
    std::vector<StreamKey> due(const std::vector<LatchStream>& latched,
                               uint64_t now_ms);

    // §3.9 early exit: this local egress stream received an IRAP, so whatever
    // is behind it now has a start point. Idempotent, and safe to call for a
    // stream that was never tracked.
    void note_irap(uint8_t local_stream_id);

    // Attempts already spent on a key (0 when untracked). Diagnostics + tests.
    uint8_t attempts(const StreamKey& key) const;
    // True once the schedule for a key has stood down (IRAP seen or bound hit).
    bool settled(const StreamKey& key) const;

  private:
    struct Entry {
        StreamKey key;
        uint8_t local_stream_id = 0;
        uint8_t attempts = 0;
        uint64_t next_ms = 0;
        bool done = false;
    };

    const Entry* find(const StreamKey& key) const;

    std::vector<Entry> entries_;
};

}  // namespace wblink
