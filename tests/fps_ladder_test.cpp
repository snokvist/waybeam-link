// SPDX-License-Identifier: GPL-2.0-or-later
// §9.11 FPS ladder (Pass 39, corrected Pass 53): preserve useful P-frame
// FEC block size, with stale/actuator holds and hysteretic slow restore.
#include "wblink/fps_ladder.h"

#include <string>

#include "wbtest.h"

using namespace wblink;

namespace {
FpsLadderPolicy fast() {
    FpsLadderPolicy p;
    p.min_fps = 60;
    p.preferred_fps = 100;
    p.min_p_frame_bytes = 10000;
    p.restore_hysteresis_bytes = 1000;
    p.sample_timeout_ms = 500;
    p.reduce_after_ms = 300;
    p.reduce_dwell_ms = 400;
    p.restore_after_ms = 800;
    p.settle_ms = 100;
    return p;
}

void sample(FpsLadder& ladder, uint32_t bytes, uint64_t now_ms,
            bool actuator_settling = false) {
    ladder.note_p_frame(bytes, now_ms);
    ladder.tick(now_ms, actuator_settling);
}

void drive_small_to_floor(FpsLadder& ladder) {
    for (uint64_t t = 1100; t <= 2400; t += 100) {
        sample(ladder, 8000, t);
    }
}
}  // namespace

int main() {
    CHECK(fps_ladder_member(100));
    CHECK(!fps_ladder_member(50));

    // First tick commands the preferred 100-fps low-latency mode.
    {
        FpsLadder ladder(fast());
        const auto fps = ladder.tick(1000);
        CHECK(fps && *fps == 100);
        CHECK_EQ_U(ladder.current_fps(), 100);
        CHECK_EQ_U(ladder.target_p_frame_bytes(), 10000);
        CHECK(std::string(ladder.state()) == "SETTLE");
    }

    // Sustained undersized P frames step down adjacent rungs to the floor.
    {
        FpsLadder ladder(fast());
        ladder.tick(1000);
        drive_small_to_floor(ladder);
        CHECK_EQ_U(ladder.current_fps(), 60);
        CHECK_EQ_U(ladder.observed_p_frame_bytes(), 8000);
        sample(ladder, 8000, 2500);
        CHECK(std::string(ladder.state()) == "FLOOR");
        CHECK(!ladder.tick(2600).has_value());
    }

    // Missing or stale frame samples hold rather than changing cadence.
    {
        FpsLadder ladder(fast());
        ladder.tick(1000);
        CHECK(!ladder.tick(1200).has_value());
        CHECK(std::string(ladder.state()) == "STALE");
        sample(ladder, 8000, 1300);
        CHECK_EQ_U(ladder.observed_p_frame_bytes(), 8000);
        CHECK(!ladder.tick(1900).has_value());
        CHECK(std::string(ladder.state()) == "STALE");
        CHECK_EQ_U(ladder.current_fps(), 100);
    }

    // Large frames restore slowly only when the predicted next-rung size
    // remains above the target plus hysteresis.
    {
        FpsLadder ladder(fast());
        ladder.tick(1000);
        drive_small_to_floor(ladder);
        for (uint64_t t = 2500; t <= 6500; t += 100) {
            sample(ladder, 16000, t);
        }
        CHECK_EQ_U(ladder.current_fps(), 100);
        sample(ladder, 16000, 6600);
        CHECK(std::string(ladder.state()) == "HOLD");
        CHECK(!ladder.tick(6700).has_value());
    }

    // At 90 fps, 11.5 KB predicts only 10.35 KB at 100 fps: below the
    // 11 KB restoration threshold, so the ladder remains at 90.
    {
        FpsLadder ladder(fast());
        ladder.tick(1000);
        for (uint64_t t = 1100; t <= 1400; t += 100) {
            sample(ladder, 8000, t);
        }
        CHECK_EQ_U(ladder.current_fps(), 90);
        for (uint64_t t = 1500; t <= 3000; t += 100) {
            sample(ladder, 11500, t);
        }
        CHECK_EQ_U(ladder.current_fps(), 90);
        CHECK(std::string(ladder.state()) == "HOLD");
    }

    // Encoder bitrate/cap settling clears accumulated evidence.
    {
        FpsLadder ladder(fast());
        ladder.tick(1000);
        sample(ladder, 8000, 1100);
        sample(ladder, 8000, 1300, true);
        CHECK(std::string(ladder.state()) == "ACTUATOR_SETTLE");
        for (uint64_t t = 1400; t < 1700; t += 100) {
            sample(ladder, 8000, t);
            CHECK_EQ_U(ladder.current_fps(), 100);
        }
        sample(ladder, 8000, 1700);
        CHECK_EQ_U(ladder.current_fps(), 90);
    }

    // §11.7 FPS_SELECT sync (Pass 71): a disabled ladder adopts the external
    // rung and resumes from it, then steps back inside [min, preferred].
    {
        FpsLadder ladder(fast());
        ladder.tick(1000);
        CHECK_EQ_U(ladder.current_fps(), 100);
        ladder.note_external_fps(60);
        CHECK_EQ_U(ladder.current_fps(), 60);
        ladder.resume(2000);
        CHECK(std::string(ladder.state()) == "SETTLE");
        CHECK_EQ_U(ladder.current_fps(), 60);
        // A selection above preferred is pulled back on the next restore
        // step (step() clamps to [min_fps, preferred_fps]).
        ladder.note_external_fps(120);
        CHECK_EQ_U(ladder.current_fps(), 120);
    }

    return wbtest_finish("fps_ladder_test");
}
