// SPDX-License-Identifier: GPL-2.0-or-later
// §15.3 (Pass 198) trailing-window loss. The point of this class is that it
// FORGETS, so every case here is about what it stops reporting — a cumulative
// ratio would pass a test that only checked "does it compute a percentage".
#include "wblink/node/loss_window.h"

#include "wbtest.h"

using wblink::node::LossWindow;

namespace {

// 1000 packet opportunities per tick, `lost` of them missed.
struct Feed {
    uint64_t t = 0, exp = 0, lost = 0, uniq = 0, decl = 0;
    LossWindow::Out step(LossWindow& w, uint64_t dt, uint64_t per_tick,
                         uint64_t lost_this_tick) {
        t += dt;
        exp += per_tick;
        lost += lost_this_tick;
        uniq += per_tick - lost_this_tick;
        decl += lost_this_tick;
        return w.update(t, exp, lost, uniq, decl);
    }
};

// THE REGRESSION. A stream that ran badly and then recovered must read clean
// within one window — this is the operator-visible bug the Pass exists for
// (a scout sweep parks one ear off-channel, and the lifetime mean carried
// that 100%-loss stretch for the rest of the session).
void test_it_forgets() {
    LossWindow w(500);
    Feed f;
    for (int i = 0; i < 20; ++i) f.step(w, 100, 1000, 1000);  // 2 s at 100%
    LossWindow::Out o = f.step(w, 100, 1000, 1000);
    CHECK_EQ_U(o.pre_milli, 1000u);

    // Perfect from here. After one full window nothing bad is left inside it.
    for (int i = 0; i < 5; ++i) o = f.step(w, 100, 1000, 0);
    CHECK_EQ_U(o.pre_milli, 0u);
    CHECK_EQ_U(o.post_milli, 0u);

    // And the cumulative ratio it replaces would still read ~476 permille here
    // (10000 lost of 21000 expected) — assert the two genuinely disagree, or
    // this test would pass against the very average it exists to replace.
    CHECK(f.lost * 1000 / f.exp > 400);
}

// Reacts to a step INTO loss just as fast.
void test_it_reacts() {
    LossWindow w(500);
    Feed f;
    for (int i = 0; i < 30; ++i) f.step(w, 100, 1000, 0);  // 3 s clean
    LossWindow::Out o = f.step(w, 100, 1000, 0);
    CHECK_EQ_U(o.pre_milli, 0u);
    for (int i = 0; i < 5; ++i) o = f.step(w, 100, 1000, 500);  // 50% for 500 ms
    CHECK_EQ_U(o.pre_milli, 500u);
    // The lifetime mean would read ~71 permille at this point.
    CHECK(f.lost * 1000 / f.exp < 100);
}

// A silent link must NOT read 0% loss. Nothing is expected, so there is no
// evidence — hold the last verdict rather than inventing a clean one.
void test_silence_holds_rather_than_clearing() {
    LossWindow w(500);
    Feed f;
    for (int i = 0; i < 10; ++i) f.step(w, 100, 1000, 300);
    LossWindow::Out o = f.step(w, 100, 1000, 300);
    CHECK_EQ_U(o.pre_milli, 300u);
    // Counters frozen: same cumulative values, time advancing.
    for (int i = 0; i < 20; ++i) o = w.update(f.t += 100, f.exp, f.lost, f.uniq, f.decl);
    CHECK_EQ_U(o.pre_milli, 300u);
    CHECK_EQ_U(o.post_milli, 300u);
}

// pre and post are independent: diversity can hide per-ear loss completely,
// which is exactly the case that made the AIR bar useless (a weak second ear
// pinned it while the picture was clean).
void test_pre_and_post_are_independent() {
    LossWindow w(500);
    uint64_t t = 0, exp = 0, lost = 0, uniq = 0, decl = 0;
    LossWindow::Out o;
    for (int i = 0; i < 10; ++i) {
        t += 100;
        exp += 2000;   // two ears, 1000 opportunities each
        lost += 700;   // the weak ear misses most of its share
        uniq += 1000;  // ...but every packet arrives on at least one ear
        decl += 0;
        o = w.update(t, exp, lost, uniq, decl);
    }
    CHECK_EQ_U(o.pre_milli, 350u);
    CHECK_EQ_U(o.post_milli, 0u);
}

// A re-latch resets the engine's counters. A backward step must re-anchor, not
// wrap the unsigned subtraction into a vast bogus delta.
void test_counter_reset_reanchors() {
    LossWindow w(500);
    Feed f;
    for (int i = 0; i < 10; ++i) f.step(w, 100, 1000, 100);
    LossWindow::Out o = w.update(f.t + 100, 0, 0, 0, 0);  // stream re-latched
    CHECK_EQ_U(o.pre_milli, 0u);
    CHECK_EQ_U(o.post_milli, 0u);
    Feed f2;
    f2.t = f.t + 100;
    for (int i = 0; i < 6; ++i) o = f2.step(w, 100, 1000, 250);
    CHECK_EQ_U(o.pre_milli, 250u);
}

// The window is a TIME span, not a sample count: a caller that ticks slowly
// must still measure over at most window_ms, and one that ticks fast must not
// grow the ring without bound.
void test_window_is_time_not_ticks() {
    {
        LossWindow w(500);
        Feed f;
        f.step(w, 100, 1000, 1000);            // one bad tick
        LossWindow::Out o = f.step(w, 5000, 1000, 0);  // ...then a long stall
        // The bad sample aged out; the only anchor left is the pre-stall one,
        // so this reports the clean tick, not the 50% a two-sample mean gives.
        CHECK_EQ_U(o.pre_milli, 0u);
    }
    {
        LossWindow w(500, /*max_samples=*/8);
        Feed f;
        LossWindow::Out o;
        for (int i = 0; i < 200; ++i) o = f.step(w, 1, 100, 10);  // 1 ms ticks
        CHECK_EQ_U(o.pre_milli, 100u);
    }
}

}  // namespace

int main() {
    test_it_forgets();
    test_it_reacts();
    test_silence_holds_rather_than_clearing();
    test_pre_and_post_are_independent();
    test_counter_reset_reanchors();
    test_window_is_time_not_ticks();
    return wbtest_finish("loss_window_test");
}
