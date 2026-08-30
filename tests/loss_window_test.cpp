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

// A SILENT ear must not win the best-ear minimum. It holds its last reading,
// and if that was 0 it would win forever while the one live ear degrades —
// a loss bar reporting a perfect link on a failing one. This is the reason
// Out carries pre_valid at all.
void test_a_silent_ear_cannot_win_the_minimum() {
    LossWindow a(500), b(500);
    uint64_t t = 0, aexp = 0, alost = 0, bexp = 0;
    for (int i = 0; i < 10; ++i) {  // both ears perfect
        t += 100; aexp += 1000; bexp += 1000;
        a.update(t, aexp, alost, 0, 0);
        b.update(t, bexp, 0, 0, 0);
    }
    LossWindow::Out ao{}, bo{};
    for (int i = 0; i < 20; ++i) {  // B goes silent, A degrades to 30%
        t += 100; aexp += 1000; alost += 300;
        ao = a.update(t, aexp, alost, 0, 0);
        bo = b.update(t, bexp, 0, 0, 0);  // B: no new opportunities
    }
    CHECK_EQ_U(ao.pre_milli, 300u);
    CHECK(ao.pre_valid);
    // B still REPORTS 0 — holding is correct for a single window — but it must
    // declare itself stale so a caller comparing ears can exclude it.
    CHECK_EQ_U(bo.pre_milli, 0u);
    CHECK(!bo.pre_valid);

    // The rule rx_core applies: valid ears only.
    uint32_t best = 0; bool have = false;
    for (const LossWindow::Out& o : {ao, bo}) {
        if (!o.pre_valid) continue;
        if (!have || o.pre_milli < best) { best = o.pre_milli; have = true; }
    }
    CHECK(have);
    CHECK_EQ_U(best, 300u);  // NOT 0
}

// pre_valid must also be false while a live stream is merely quiet, so the
// stream-level hold and the ear-level exclusion agree about what "no evidence"
// means.
void test_valid_flags_track_evidence() {
    LossWindow w(500);
    LossWindow::Out o = w.update(0, 0, 0, 0, 0);
    CHECK(!o.pre_valid);   // first sample: no delta yet
    CHECK(!o.post_valid);
    o = w.update(100, 1000, 100, 900, 100);
    CHECK(o.pre_valid);
    CHECK(o.post_valid);
    // Frozen counters are not enough on their own: while the window still
    // spans the earlier real traffic there IS evidence, and pre_valid must
    // stay true. It goes false only once every sample inside the window is
    // frozen — which is the condition the ear-exclusion actually depends on.
    o = w.update(200, 1000, 100, 900, 100);
    CHECK(o.pre_valid);
    for (uint64_t t = 300; t <= 1200; t += 100) {
        o = w.update(t, 1000, 100, 900, 100);
    }
    CHECK(!o.pre_valid);
    CHECK(!o.post_valid);
    CHECK_EQ_U(o.pre_milli, 100u);  // ...but the held value survives
}

}  // namespace

int main() {
    test_it_forgets();
    test_it_reacts();
    test_silence_holds_rather_than_clearing();
    test_pre_and_post_are_independent();
    test_counter_reset_reanchors();
    test_window_is_time_not_ticks();
    test_a_silent_ear_cannot_win_the_minimum();
    test_valid_flags_track_evidence();
    return wbtest_finish("loss_window_test");
}
