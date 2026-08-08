// SPDX-License-Identifier: GPL-2.0-or-later
// §9.4 Pass 163 — sequence-derived rate probe: candidate derivation + the
// RX evidence window (three normative guards; see mcs_probe.h).
#include "wblink/mcs_probe.h"

#include <cstring>

namespace wblink {

std::optional<uint8_t> probe_up_candidate_mcs(const ProfileTable& table,
                                              uint8_t active_profile) {
    const Profile* cur = nullptr;
    const Profile* next = nullptr;
    for (const Profile& p : table.profiles) {
        if (p.id == active_profile) {
            cur = &p;
        } else if (p.id > active_profile &&
                   (next == nullptr || p.id < next->id)) {
            next = &p;
        }
    }
    if (cur == nullptr || next == nullptr || next->mcs == cur->mcs) {
        return std::nullopt;
    }
    return next->mcs;
}

bool McsProbeWindow::seen(uint32_t seq) const {
    const uint32_t bit = seq % kSeenBits;
    return (seen_[bit / 64] >> (bit % 64)) & 1u;
}

void McsProbeWindow::mark_seen(uint32_t seq) {
    const uint32_t bit = seq % kSeenBits;
    seen_[bit / 64] |= uint64_t{1} << (bit % 64);
}

void McsProbeWindow::reset() {
    have_context_ = false;
    candidate_.reset();
    confirmed_ = false;
    successes_ = 0;
    failures_ = 0;
    have_head_ = false;
}

void McsProbeWindow::reset_context(uint8_t active_profile, uint64_t now_ms) {
    reset();
    profile_ = active_profile;
    const Profile* cur = nullptr;
    for (const Profile& p : table_->profiles) {
        if (p.id == active_profile) cur = &p;
    }
    if (cur == nullptr) {
        return;  // unknown profile id — stay contextless, score nothing
    }
    have_context_ = true;
    selected_mcs_ = cur->mcs;
    candidate_ = probe_up_candidate_mcs(*table_, active_profile);
    window_start_ms_ = now_ms;
}

void McsProbeWindow::advance_gap_walk(uint64_t now_ms) {
    (void)now_ms;
    // Bitmap knowledge only spans kSeenBits behind head; anything older is
    // unknown, not lost — skip, never attribute.
    if (head_seq_ - walked_seq_ > kSeenBits) {
        walked_seq_ = head_seq_ - kSeenBits;
    }
    while (head_seq_ - walked_seq_ > p_.gap_horizon) {
        const uint32_t s = ++walked_seq_;
        // Guard 2: a missing probe-slot seq charges the candidate only while
        // non-probe frames confirm the TX is flying the commanded rate.
        if (candidate_ && confirmed_ && !seen(s) &&
            probe_slot_hit(s, table_->probe_period, table_->probe_slot)) {
            ++failures_;
        }
    }
}

void McsProbeWindow::on_data(uint32_t seq, uint8_t active_profile,
                             uint8_t table_version, uint8_t rx_mcs,
                             uint64_t now_ms) {
    if (table_ == nullptr || table_->probe_period == 0) {
        return;
    }
    // §3.4/§3.6: a mismatched table_version means an unknown schedule —
    // refuse to score rather than mis-score.
    if (tv_.has_value() && table_version != *tv_) {
        return;
    }
    if (!have_context_ || active_profile != profile_) {
        reset_context(active_profile, now_ms);
        if (!have_context_) return;
    }
    // Rolling freshness: evidence spans at most max_age_ms (§17 seed).
    if (now_ms - window_start_ms_ > p_.max_age_ms) {
        successes_ = 0;
        failures_ = 0;
        window_start_ms_ = now_ms;
    }
    bool first_arrival;
    if (!have_head_) {
        have_head_ = true;
        head_seq_ = seq;
        walked_seq_ = seq;  // pre-context history is not ours to attribute
        std::memset(seen_, 0, sizeof(seen_));
        mark_seen(seq);
        first_arrival = true;
    } else if (seq > head_seq_) {
        // Recycle bitmap bits for the seqs entering the window.
        const uint32_t delta = seq - head_seq_;
        if (delta >= kSeenBits) {
            std::memset(seen_, 0, sizeof(seen_));
        } else {
            for (uint32_t s = head_seq_ + 1; s != seq + 1; ++s) {
                const uint32_t bit = s % kSeenBits;
                seen_[bit / 64] &= ~(uint64_t{1} << (bit % 64));
            }
        }
        head_seq_ = seq;
        mark_seen(seq);
        first_arrival = true;
        advance_gap_walk(now_ms);
    } else {
        // Reordered/diversity copy. Outside the bitmap span (or already
        // settled by the gap walk) it can no longer be classified.
        if (head_seq_ - seq >= kSeenBits || seq <= walked_seq_) {
            return;
        }
        first_arrival = !seen(seq);
        mark_seen(seq);
    }

    if (probe_slot_hit(seq, table_->probe_period, table_->probe_slot)) {
        // Guard 1: successes are rate-verified — a probe-slot frame that
        // demonstrably flew elsewhere (resend, suppressed probe, command
        // lag) is ignored, never mis-credited.
        if (first_arrival && candidate_ && rx_mcs == *candidate_) {
            ++successes_;
        }
        return;
    }
    // Guard 2 confirmation: non-probe frames tell us whether the TX is
    // flying the commanded rate; unknown PHY rate confirms nothing.
    if (rx_mcs == selected_mcs_) {
        confirmed_ = true;
    } else if (rx_mcs != kUplinkRxMcsUnknown) {
        confirmed_ = false;
    }
}

void McsProbeWindow::on_crc_frames(uint8_t rx_mcs, uint32_t count,
                                   uint64_t now_ms) {
    if (table_ == nullptr || table_->probe_period == 0 || !have_context_ ||
        !candidate_ || !confirmed_ || count == 0) {
        return;
    }
    if (now_ms - window_start_ms_ > p_.max_age_ms) {
        successes_ = 0;
        failures_ = 0;
        window_start_ms_ = now_ms;
    }
    // Guard 3: the descriptor rate is pre-FCS — a corrupt frame at the
    // candidate rate is a rate-verified failure, pushed seq-free (it never
    // advances the gap walk; the body is untrusted).
    if (rx_mcs == *candidate_) {
        failures_ += count;
    }
}

std::optional<uint16_t> McsProbeWindow::probe_per(uint64_t now_ms) const {
    if (!have_context_ || !candidate_) {
        return std::nullopt;
    }
    if (now_ms - window_start_ms_ > p_.max_age_ms) {
        return std::nullopt;  // stale evidence gates nothing
    }
    const uint32_t total = successes_ + failures_;
    if (total < p_.min_samples) {
        return std::nullopt;
    }
    const uint32_t per =
        static_cast<uint32_t>(uint64_t{failures_} * 1000 / total);
    return static_cast<uint16_t>(per > 1000 ? 1000 : per);
}

}  // namespace wblink
