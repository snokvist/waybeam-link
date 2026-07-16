// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §9.11 FPS ladder (Pass 39). The last-resort actuator,
// outside the §9.1 cascade: reduce fps only on measurable radio-loop
// exhaustion (floor rung + sustained loss), restore much more slowly on
// positive evidence, hold on stale feedback. Pure, time-injected.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace wblink {

// The §9.6 ladder — the only commandable fps values.
inline constexpr std::array<uint16_t, 8> kFpsLadder = {30, 45,  60,  75,
                                                       90, 100, 120, 144};

inline bool fps_ladder_member(uint16_t fps) {
    for (const uint16_t f : kFpsLadder) {
        if (f == fps) return true;
    }
    return false;
}

struct FpsLadderPolicy {
    uint16_t min_fps = 60;
    uint16_t preferred_fps = 90;  // v1 recovery target AND ceiling (§9.11)
    uint16_t distress_milli = 20;  // seed = §9.1 demote_milli
    uint16_t restore_milli = 5;
    uint32_t reduce_after_ms = 3000;
    uint32_t reduce_dwell_ms = 4000;
    uint32_t restore_after_ms = 8000;
    uint32_t settle_ms = 1500;
    uint32_t report_timeout_ms = 500;  // = policy.report_timeout_ms
};

class FpsLadder {
  public:
    explicit FpsLadder(const FpsLadderPolicy& p) : p_(p) {}

    // Feed each accepted LINK_REPORT's post-diversity loss (§3.7).
    void note_report(uint16_t loss_milli, uint64_t now_ms) {
        ewma_ = have_report_ ? 0.7 * ewma_ + 0.3 * loss_milli
                             : static_cast<double>(loss_milli);
        have_report_ = true;
        last_report_ms_ = now_ms;
    }

    // at_floor = the selector holds the table's floor rung. Returns an fps
    // to command exactly once per change (first call commands preferred).
    std::optional<uint16_t> tick(uint64_t now_ms, bool at_floor) {
        if (!started_) {
            started_ = true;
            current_ = p_.preferred_fps;
            settle_until_ms_ = now_ms + p_.settle_ms;
            last_change_ms_ = now_ms;
            return current_;
        }
        if (now_ms < settle_until_ms_) {
            return std::nullopt;
        }
        // §9.11: stale feedback holds — §9.8 owns degradation on silence.
        if (!have_report_ || now_ms - last_report_ms_ > p_.report_timeout_ms) {
            distress_since_ms_ = 0;
            healthy_since_ms_ = 0;
            return std::nullopt;
        }
        const bool distress = at_floor && ewma_ >= p_.distress_milli;
        const bool healthy = !at_floor && ewma_ <= p_.restore_milli;
        if (distress) {
            healthy_since_ms_ = 0;
            if (distress_since_ms_ == 0) {
                distress_since_ms_ = now_ms;
            }
            if (now_ms - distress_since_ms_ >= p_.reduce_after_ms &&
                now_ms - last_change_ms_ >= p_.reduce_dwell_ms) {
                if (const uint16_t down = step(-1); down != current_) {
                    return commit(down, now_ms);
                }
            }
            return std::nullopt;
        }
        distress_since_ms_ = 0;
        if (healthy) {
            if (healthy_since_ms_ == 0) {
                healthy_since_ms_ = now_ms;
            }
            if (now_ms - healthy_since_ms_ >= p_.restore_after_ms) {
                if (const uint16_t up = step(+1); up != current_) {
                    return commit(up, now_ms);
                }
            }
            return std::nullopt;
        }
        healthy_since_ms_ = 0;
        return std::nullopt;
    }

    uint16_t current_fps() const { return current_; }

  private:
    uint16_t step(int dir) const {
        // Neighboring ladder member inside [min_fps, preferred_fps].
        for (size_t i = 0; i < kFpsLadder.size(); ++i) {
            if (kFpsLadder[i] != current_) continue;
            const size_t j = static_cast<size_t>(static_cast<int>(i) + dir);
            if (j >= kFpsLadder.size()) return current_;
            const uint16_t f = kFpsLadder[j];
            return (f >= p_.min_fps && f <= p_.preferred_fps) ? f : current_;
        }
        return current_;
    }
    uint16_t commit(uint16_t fps, uint64_t now_ms) {
        current_ = fps;
        last_change_ms_ = now_ms;
        settle_until_ms_ = now_ms + p_.settle_ms;
        distress_since_ms_ = 0;
        healthy_since_ms_ = 0;
        return current_;
    }

    FpsLadderPolicy p_;
    bool started_ = false;
    uint16_t current_ = 0;
    double ewma_ = 0.0;
    bool have_report_ = false;
    uint64_t last_report_ms_ = 0;
    uint64_t last_change_ms_ = 0;
    uint64_t settle_until_ms_ = 0;
    uint64_t distress_since_ms_ = 0;
    uint64_t healthy_since_ms_ = 0;
};

}  // namespace wblink
