// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §9.6 horizon frame caps (Pass 37). Pure all-integer
// derivation of the venc maxIBytes/maxPBytes ceilings from slow inputs —
// the active rung's derived bitrate budget, the ladder-snapped frame
// cadence, the I-class recoverable deadline, and the stream's §14.1 parity
// rates. A per-frame budget channel is ruled out of scope; these caps
// change only when an input changes.
#pragma once

#include <cstdint>

namespace wblink {

// venc's own clamp floor for nonzero caps — never command below it.
inline constexpr uint32_t kVencCapFloorBytes = 4096;

struct FrameCapInputs {
    uint32_t budget_kbps = 0;         // §9.5 rung bitrate target (net)
    uint32_t frame_period_us = 0;     // ladder-snapped cadence (§9.6)
    uint16_t iframe_deadline_ms = 0;  // active rung §9.3 I-class budget
    uint16_t i_rate_permille = 0;     // §14.1 stream parity rates
    uint16_t p_rate_permille = 0;
    uint16_t symbol_size = 0;         // s at the rung (§5.1a); 0 = no bound
    uint32_t ceiling_bytes = 196608;  // venc.cap_ceiling_bytes
    uint16_t i_headroom_permille = 1000;
    uint16_t p_headroom_permille = 1000;
};

struct FrameCaps {
    uint32_t max_i_bytes = 0;  // 0 = insufficient inputs, do not push
    uint32_t max_p_bytes = 0;
    friend bool operator==(const FrameCaps&, const FrameCaps&) = default;
};

// §9.6: snap a measured frame interval to the nearest ladder fps so cadence
// jitter cannot churn the caps. 0 in => 0 out (unmeasured).
inline uint32_t snap_frame_period_us(uint64_t measured_us) {
    if (measured_us == 0) {
        return 0;
    }
    static constexpr uint32_t kLadderFps[] = {30, 45, 60, 75, 90, 100, 120, 144};
    uint32_t best = 0;
    uint64_t best_diff = UINT64_MAX;
    for (const uint32_t fps : kLadderFps) {
        const uint32_t period = 1000000u / fps;
        const uint64_t diff = period > measured_us ? period - measured_us
                                                   : measured_us - period;
        if (diff < best_diff) {
            best_diff = diff;
            best = period;
        }
    }
    return best;
}

// budget_kbps over window_us, net of parity rate and headroom, in bytes.
// budget_kbps == bits/ms; bits = budget_kbps * window_us / 1000.
inline uint32_t cap_window_bytes(uint32_t budget_kbps, uint64_t window_us,
                                 uint16_t rate_permille,
                                 uint16_t headroom_permille) {
    uint64_t bits = static_cast<uint64_t>(budget_kbps) * window_us / 1000u;
    uint64_t bytes = bits / 8u;
    bytes = bytes * 1000u / (1000u + rate_permille);
    bytes = bytes * headroom_permille / 1000u;
    return static_cast<uint32_t>(bytes > UINT32_MAX ? UINT32_MAX : bytes);
}

// §14.1 GF(256) eligibility bound at symbol size s for a parity rate:
// k + ceil(k*rate/1000) <= 256  =>  k_max = floor(256000/(1000+rate)).
inline uint32_t fec_eligibility_ceiling(uint16_t symbol_size,
                                        uint16_t rate_permille) {
    if (symbol_size == 0) {
        return UINT32_MAX;
    }
    const uint32_t k_max = 256000u / (1000u + rate_permille);
    return k_max * symbol_size;
}

inline FrameCaps derive_frame_caps(const FrameCapInputs& in) {
    FrameCaps out;
    if (in.budget_kbps == 0 || in.frame_period_us == 0 ||
        in.iframe_deadline_ms == 0) {
        return out;  // insufficient inputs — leave venc unconstrained
    }
    // maxP: one frame period of rung budget, net of P parity (§9.6).
    uint64_t max_p = cap_window_bytes(in.budget_kbps, in.frame_period_us,
                                      in.p_rate_permille,
                                      in.p_headroom_permille);
    // maxI: the I-class recoverable deadline (§4.1/§8), net of I parity.
    uint64_t max_i = cap_window_bytes(
        in.budget_kbps, static_cast<uint64_t>(in.iframe_deadline_ms) * 1000u,
        in.i_rate_permille, in.i_headroom_permille);
    const uint64_t p_ceiling =
        in.ceiling_bytes < fec_eligibility_ceiling(in.symbol_size,
                                                   in.p_rate_permille)
            ? in.ceiling_bytes
            : fec_eligibility_ceiling(in.symbol_size, in.p_rate_permille);
    const uint64_t i_ceiling =
        in.ceiling_bytes < fec_eligibility_ceiling(in.symbol_size,
                                                   in.i_rate_permille)
            ? in.ceiling_bytes
            : fec_eligibility_ceiling(in.symbol_size, in.i_rate_permille);
    if (max_p > p_ceiling) max_p = p_ceiling;
    if (max_i > i_ceiling) max_i = i_ceiling;
    // Encoder sanity I >= P, but FEC eligibility wins: never raise maxI
    // past its own §14.1 ceiling to chase maxP.
    if (max_i < max_p) {
        max_i = max_p < i_ceiling ? max_p : i_ceiling;
    }
    if (max_p < kVencCapFloorBytes) max_p = kVencCapFloorBytes;
    if (max_i < kVencCapFloorBytes) max_i = kVencCapFloorBytes;
    out.max_i_bytes = static_cast<uint32_t>(max_i);
    out.max_p_bytes = static_cast<uint32_t>(max_p);
    return out;
}

}  // namespace wblink
