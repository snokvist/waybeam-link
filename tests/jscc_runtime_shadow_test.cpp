// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/jscc_runtime_shadow.h"

#include <cstring>

#include "wbtest.h"

using namespace wblink;

int main() {
    JsccRuntimeShadow shadow({20, 400, 500});
    JsccShadowFrameInput frame;
    frame.source_k = 40;
    frame.deadline_us = 16667;
    frame.source_tx_remaining_us = 5000;
    frame.now_ms = 1000;

    auto out = shadow.evaluate(frame);
    CHECK(!out.valid);
    CHECK(out.fallback == JsccShadowFallback::kFeedbackMissing);

    JsccFeedback fb;
    fb.prefix = {9, 17, 1001};
    fb.feedback_epoch = 7;
    fb.repair_demand_permille = 125;
    fb.repair_samples = 30;
    fb.valid_flags = jscc_feedback_flags::kRepairReady;
    CHECK(shadow.observe_feedback(fb, 900));
    // §3.10/§14.2 (Pass 205 O6): the NACK-RTT readiness gate is gone, so a
    // repair-ready feedback with no RTT sample now evaluates.
    out = shadow.evaluate(frame);
    CHECK(out.valid);
    CHECK(out.fallback == JsccShadowFallback::kNone);
    CHECK_EQ_U(out.input.predicted_loss_symbols, 5);
    CHECK_EQ_U(out.input.fec_floor_symbols, 1);
    CHECK_EQ_U(out.input.fec_cap_symbols, 16);
    CHECK_EQ_U(out.decision.parity_symbols, 5);
    CHECK(std::strcmp(jscc_reason_string(out.decision.reason),
                      "fec_only") == 0);

    // Re-anchor the freshness clock to 950 so the boundary below is exact.
    fb.feedback_epoch = 8;
    CHECK(shadow.observe_feedback(fb, 950));

    // Replayed feedback cannot replace the cache; freshness is bounded.
    CHECK(!shadow.observe_feedback(fb, 1001));
    frame.now_ms = 1450;
    out = shadow.evaluate(frame);
    CHECK(out.valid);  // equality with the 500 ms timeout is fresh
    frame.now_ms = 1451;
    out = shadow.evaluate(frame);
    CHECK(!out.valid);
    CHECK(out.fallback == JsccShadowFallback::kFeedbackStale);

    // Epoch monotonicity is scoped to the reporter session. A receiver reboot
    // starts again at a low epoch and must immediately replace stale cache.
    fb.prefix.session_id = 2002;
    fb.feedback_epoch = 1;
    CHECK(shadow.observe_feedback(fb, 1500));
    frame.now_ms = 1500;
    out = shadow.evaluate(frame);
    CHECK(out.valid);
    CHECK_EQ_U(out.feedback_epoch, 1);
    CHECK(!shadow.observe_feedback(fb, 1501));  // replay in new session

    shadow.reset();
    CHECK(!shadow.evaluate(frame).valid);
    return wbtest_finish("jscc_runtime_shadow_test");
}
