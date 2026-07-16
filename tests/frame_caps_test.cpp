// SPDX-License-Identifier: GPL-2.0-or-later
// §9.6 horizon frame caps (Pass 37): ladder snap, cap formulas, ceilings,
// floors, and the insufficient-inputs guard.
#include "wblink/frame_caps.h"

#include "wbtest.h"

using namespace wblink;

int main() {
    // --- ladder snap ---------------------------------------------------------
    CHECK_EQ_U(snap_frame_period_us(0), 0);
    CHECK_EQ_U(snap_frame_period_us(11100), 1000000 / 90);   // ~90 fps
    CHECK_EQ_U(snap_frame_period_us(11111), 1000000 / 90);
    CHECK_EQ_U(snap_frame_period_us(16800), 1000000 / 60);   // ~60 fps
    CHECK_EQ_U(snap_frame_period_us(33000), 1000000 / 30);
    CHECK_EQ_U(snap_frame_period_us(6900), 1000000 / 144);   // clamps at 144
    CHECK_EQ_U(snap_frame_period_us(1000000), 1000000 / 30); // clamps at 30

    // --- doc worked example: 20 Mbps @ 90 fps => 27.8 KB average -------------
    // With zero parity and 1000 permille headroom, maxP is exactly one frame
    // period of budget: 20000 kbps * 11111 us / 1000 / 8 = 27777 B.
    {
        FrameCapInputs in;
        in.budget_kbps = 20000;
        in.frame_period_us = snap_frame_period_us(11100);
        in.iframe_deadline_ms = 40;
        in.symbol_size = 1387;
        auto caps = derive_frame_caps(in);
        CHECK_EQ_U(caps.max_p_bytes, 27777);
        // maxI = 20000 kbps * 40 ms / 8 = 100000 B.
        CHECK_EQ_U(caps.max_i_bytes, 100000);
    }

    // --- parity + headroom deductions ---------------------------------------
    {
        FrameCapInputs in;
        in.budget_kbps = 20000;
        in.frame_period_us = 1000000 / 90;
        in.iframe_deadline_ms = 40;
        in.i_rate_permille = 250;   // §14.1 seeds
        in.p_rate_permille = 100;
        in.i_headroom_permille = 900;
        in.p_headroom_permille = 900;
        in.symbol_size = 1387;
        auto caps = derive_frame_caps(in);
        // P: 27777 * 1000/1100 * 900/1000 = 22725 (all-integer truncation)
        CHECK_EQ_U(caps.max_p_bytes, 22725);
        // I: 100000 * 1000/1250 * 900/1000 = 72000
        CHECK_EQ_U(caps.max_i_bytes, 72000);
    }

    // --- ceilings: configured absolute + §14.1 GF(256) eligibility ----------
    {
        // k_max at 250 permille = 256000/1250 = 204 symbols * 1387 = 282948.
        CHECK_EQ_U(fec_eligibility_ceiling(1387, 250), 204 * 1387);
        CHECK_EQ_U(fec_eligibility_ceiling(0, 250), UINT32_MAX);
        FrameCapInputs in;
        in.budget_kbps = 30000;
        in.frame_period_us = 1000000 / 30;
        in.iframe_deadline_ms = 100;   // raw I budget 375000 B
        in.symbol_size = 1387;
        in.ceiling_bytes = 196608;
        auto caps = derive_frame_caps(in);
        CHECK_EQ_U(caps.max_i_bytes, 196608);  // configured ceiling binds
        // P raw = 30000 * 33333 / 1000 / 8 = 124998 — under both ceilings.
        CHECK_EQ_U(caps.max_p_bytes, 124998);
    }
    {
        // Tight FEC ceiling binds below the configured one: s=100, rate 250
        // => 204*100 = 20400 for I frames.
        FrameCapInputs in;
        in.budget_kbps = 30000;
        in.frame_period_us = 1000000 / 30;
        in.iframe_deadline_ms = 100;
        in.i_rate_permille = 250;
        in.symbol_size = 100;
        auto caps = derive_frame_caps(in);
        CHECK_EQ_U(caps.max_i_bytes, 204 * 100);
        // P ceiling at rate 0: 256*100 = 25600.
        CHECK_EQ_U(caps.max_p_bytes, 25600);
        // FEC eligibility wins over I >= P when the I ceiling is tighter.
        CHECK(caps.max_i_bytes < caps.max_p_bytes);
    }

    // --- floors + insufficient inputs ----------------------------------------
    {
        FrameCapInputs in;
        in.budget_kbps = 1000;             // floor rung
        in.frame_period_us = 1000000 / 144;
        in.iframe_deadline_ms = 20;
        auto caps = derive_frame_caps(in);
        // Raw P = 1000 * 6944 / 1000 / 8 = 868 -> venc floor 4096.
        CHECK_EQ_U(caps.max_p_bytes, kVencCapFloorBytes);
        CHECK_EQ_U(caps.max_i_bytes, kVencCapFloorBytes);
    }
    {
        FrameCapInputs in;  // any zero input => {0,0}: do not push
        in.budget_kbps = 0;
        in.frame_period_us = 11111;
        in.iframe_deadline_ms = 40;
        CHECK(derive_frame_caps(in) == FrameCaps{});
        in.budget_kbps = 20000;
        in.frame_period_us = 0;
        CHECK(derive_frame_caps(in) == FrameCaps{});
        in.frame_period_us = 11111;
        in.iframe_deadline_ms = 0;
        CHECK(derive_frame_caps(in) == FrameCaps{});
    }

    return wbtest_finish("frame_caps_test");
}
