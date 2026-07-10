// SPDX-License-Identifier: GPL-2.0-or-later
// §6.6 plausible-forward clamp. seq/block_id never wrap within a flight (§2),
// so the clamp is plain forward distance — these tests document that there is
// deliberately no modular arithmetic.
#include "wblink/clamp.h"

#include "wbtest.h"

using namespace wblink;

int main() {
    constexpr uint32_t kK = 4096;

    // Equal and backward are always plausible (dedup's problem, not ours).
    CHECK(plausible_forward(100, 100, kK));
    CHECK(plausible_forward(100, 0, kK));
    CHECK(plausible_forward(0xFFFFFFFF, 0, kK));

    // Forward within K.
    CHECK(plausible_forward(100, 101, kK));
    CHECK(plausible_forward(100, 100 + kK, kK));

    // Forward beyond K — the forged far-future flush (§13) dies here.
    CHECK(!plausible_forward(100, 100 + kK + 1, kK));
    CHECK(!plausible_forward(0, 0xFFFFFFFF, kK));
    CHECK(!plausible_forward(0, 1000000, kK));

    // Near the top of the u32 range: still plain integer comparison.
    CHECK(plausible_forward(0xFFFFFFF0, 0xFFFFFFFF, kK));
    CHECK(!plausible_forward(0xFFFFF000, 0xFFFFFFFF, 15));

    // K = 0: only equal-or-backward passes.
    CHECK(plausible_forward(5, 5, 0));
    CHECK(!plausible_forward(5, 6, 0));

    // compile-time usable.
    static_assert(plausible_forward(0, 1, 1));
    static_assert(!plausible_forward(0, 2, 1));

    return wbtest_finish("clamp_test");
}
