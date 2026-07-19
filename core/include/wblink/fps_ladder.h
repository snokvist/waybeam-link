// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §9.11 FPS ladder (Pass 39, corrected Pass 53). Preserve
// useful frame-aligned FEC block size: reduce fps on sustained small P frames,
// restore only when the next rung's predicted size clears hysteresis. Pure,
// time-injected.
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
    uint16_t preferred_fps = 100;  // v1 recovery target AND ceiling (§9.11)
    uint32_t min_p_frame_bytes = 10000;
    uint32_t restore_hysteresis_bytes = 1000;
    uint32_t sample_timeout_ms = 500;
    uint32_t reduce_after_ms = 3000;
    uint32_t reduce_dwell_ms = 4000;
    uint32_t restore_after_ms = 8000;
    uint32_t settle_ms = 1500;
};

class FpsLadder {
  public:
    explicit FpsLadder(const FpsLadderPolicy& p) : p_(p) {}

    // Feed each non-IDR Annex-B payload size at frame-SHM ingress. The fixed
    // EWMA smooths encoder noise; the multi-second evidence timers own the
    // actual anti-flap policy.
    void note_p_frame(uint32_t bytes, uint64_t now_ms) {
        if (bytes == 0) return;
        ewma_bytes_ = have_sample_ ? 0.7 * ewma_bytes_ + 0.3 * bytes
                                   : static_cast<double>(bytes);
        have_sample_ = true;
        last_sample_ms_ = now_ms;
    }

    // actuator_settling means bitrate/caps are still taking effect; those
    // changes get first claim on observed frame-size evidence. Returns an fps
    // to command exactly once per change (first call commands preferred).
    std::optional<uint16_t> tick(uint64_t now_ms,
                                 bool actuator_settling = false) {
        if (!started_) {
            started_ = true;
            current_ = p_.preferred_fps;
            settle_until_ms_ = now_ms + p_.settle_ms;
            last_change_ms_ = now_ms;
            state_ = "SETTLE";
            return current_;
        }
        if (actuator_settling) {
            clear_evidence();
            state_ = "ACTUATOR_SETTLE";
            return std::nullopt;
        }
        if (now_ms < settle_until_ms_) {
            state_ = "SETTLE";
            return std::nullopt;
        }
        if (!have_sample_ ||
            now_ms - last_sample_ms_ > p_.sample_timeout_ms) {
            clear_evidence();
            state_ = "STALE";
            return std::nullopt;
        }

        if (ewma_bytes_ < p_.min_p_frame_bytes) {
            restore_since_ms_ = 0;
            const uint16_t down = step(-1);
            if (down == current_) {
                reduce_since_ms_ = 0;
                state_ = "FLOOR";
                return std::nullopt;
            }
            if (reduce_since_ms_ == 0) {
                reduce_since_ms_ = now_ms;
            }
            state_ = "REDUCE_WAIT";
            if (now_ms - reduce_since_ms_ >= p_.reduce_after_ms &&
                now_ms - last_change_ms_ >= p_.reduce_dwell_ms) {
                return commit(down, now_ms);
            }
            return std::nullopt;
        }

        reduce_since_ms_ = 0;
        const uint16_t up = step(+1);
        if (up == current_) {
            restore_since_ms_ = 0;
            state_ = "HOLD";
            return std::nullopt;
        }
        const double predicted_up =
            ewma_bytes_ * static_cast<double>(current_) / up;
        const uint64_t restore_floor =
            static_cast<uint64_t>(p_.min_p_frame_bytes) +
            p_.restore_hysteresis_bytes;
        if (predicted_up >= static_cast<double>(restore_floor)) {
            if (restore_since_ms_ == 0) {
                restore_since_ms_ = now_ms;
            }
            state_ = "RESTORE_WAIT";
            if (now_ms - restore_since_ms_ >= p_.restore_after_ms) {
                return commit(up, now_ms);
            }
            return std::nullopt;
        }
        restore_since_ms_ = 0;
        state_ = "HOLD";
        return std::nullopt;
    }

    uint16_t current_fps() const { return current_; }
    uint32_t observed_p_frame_bytes() const {
        return have_sample_ ? static_cast<uint32_t>(ewma_bytes_ + 0.5) : 0;
    }
    uint32_t target_p_frame_bytes() const { return p_.min_p_frame_bytes; }
    const char* state() const { return state_; }

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
        clear_evidence();
        have_sample_ = false;  // pre-change frame sizes are no longer causal
        ewma_bytes_ = 0.0;
        last_sample_ms_ = 0;
        state_ = "SETTLE";
        return current_;
    }
    void clear_evidence() {
        reduce_since_ms_ = 0;
        restore_since_ms_ = 0;
    }

    FpsLadderPolicy p_;
    bool started_ = false;
    uint16_t current_ = 0;
    double ewma_bytes_ = 0.0;
    bool have_sample_ = false;
    uint64_t last_sample_ms_ = 0;
    uint64_t last_change_ms_ = 0;
    uint64_t settle_until_ms_ = 0;
    uint64_t reduce_since_ms_ = 0;
    uint64_t restore_since_ms_ = 0;
    const char* state_ = "INIT";
};

}  // namespace wblink
