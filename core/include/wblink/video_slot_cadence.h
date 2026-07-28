// SPDX-License-Identifier: GPL-2.0-or-later
// Cadence gate for low-rate control/observability traffic that is allowed to
// ride only inside an already-active live-video TX slot.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace wblink {

class VideoSlotCadence {
  public:
    explicit VideoSlotCadence(uint64_t period_ms) : period_ms_(period_ms) {}

    // A due item remains pending while video is idle. Merely checking the
    // cadence never consumes it; only a successful in-slot send advances it.
    bool due(bool live_video_slot, uint64_t now_ms) const {
        return live_video_slot && now_ms >= next_due_ms_;
    }

    // Air backends return a successful submission/target count (normally 1),
    // not the encoded packet length. A zero result leaves the item pending.
    bool note_submitted(size_t submission_count, uint64_t now_ms) {
        if (submission_count == 0) {
            return false;
        }
        const uint64_t room = std::numeric_limits<uint64_t>::max() - now_ms;
        next_due_ms_ =
            period_ms_ > room ? std::numeric_limits<uint64_t>::max()
                              : now_ms + period_ms_;
        return true;
    }

    uint64_t next_due_ms() const { return next_due_ms_; }

  private:
    uint64_t period_ms_;
    uint64_t next_due_ms_ = 0;
};

}  // namespace wblink
