// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/jscc_runtime_shadow.h"

#include <cstring>

#include "wbtest.h"

using namespace wblink;

int main() {
    JsccRuntimeShadow shadow({20, 400, 500, 500, 20});
    JsccShadowFrameInput frame;
    frame.source_k = 40;
    frame.deadline_us = 16667;
    frame.source_tx_remaining_us = 5000;
    frame.resend_airtime_us = 200;
    frame.arq_capable = true;
    frame.now_ms = 1000;

    auto out = shadow.evaluate(frame);
    CHECK(!out.valid);
    CHECK(out.fallback == JsccShadowFallback::kFeedbackMissing);

    JsccFeedback fb;
    fb.feedback_epoch = 7;
    fb.repair_demand_permille = 125;
    fb.rtt_p95_us = 2000;
    fb.repair_samples = 30;
    fb.rtt_samples = 19;
    fb.valid_flags = jscc_feedback_flags::kKnownMask;
    CHECK(shadow.observe_feedback(fb, 900));
    out = shadow.evaluate(frame);
    CHECK(!out.valid);
    CHECK(out.fallback == JsccShadowFallback::kRttNotReady);

    fb.feedback_epoch = 8;
    fb.rtt_samples = 20;
    CHECK(shadow.observe_feedback(fb, 950));
    out = shadow.evaluate(frame);
    CHECK(out.valid);
    CHECK(out.fallback == JsccShadowFallback::kNone);
    CHECK_EQ_U(out.input.predicted_loss_symbols, 5);
    CHECK_EQ_U(out.input.fec_floor_symbols, 1);
    CHECK_EQ_U(out.input.fec_cap_symbols, 16);
    CHECK_EQ_U(out.decision.parity_symbols, 5);
    CHECK(out.decision.arq_eligible);
    CHECK(std::strcmp(jscc_reason_string(out.decision.reason),
                      "fec_and_arq") == 0);

    // Replayed feedback cannot replace the cache; freshness is bounded.
    CHECK(!shadow.observe_feedback(fb, 1001));
    frame.now_ms = 1450;
    out = shadow.evaluate(frame);
    CHECK(out.valid);  // equality with the 500 ms timeout is fresh
    frame.now_ms = 1451;
    out = shadow.evaluate(frame);
    CHECK(!out.valid);
    CHECK(out.fallback == JsccShadowFallback::kFeedbackStale);

    shadow.reset();
    CHECK(!shadow.evaluate(frame).valid);
    return wbtest_finish("jscc_runtime_shadow_test");
}
