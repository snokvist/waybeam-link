// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/jscc_runtime_shadow.h"

#include <algorithm>
#include <cstdint>

namespace wblink {

namespace {
bool forward_u32(uint32_t next, uint32_t previous) {
    return static_cast<int32_t>(next - previous) > 0;
}

uint16_t rate_symbols(uint16_t rate, uint16_t k) {
    return static_cast<uint16_t>(std::min<uint32_t>(
        UINT16_MAX, (static_cast<uint32_t>(rate) * k + 999u) / 1000u));
}
}  // namespace

bool JsccRuntimeShadow::observe_feedback(const JsccFeedback& feedback,
                                         uint64_t now_ms) {
    const bool same_reporter_session =
        feedback_ &&
        feedback.prefix.originator == feedback_->prefix.originator &&
        feedback.prefix.session_id == feedback_->prefix.session_id;
    if (same_reporter_session &&
        !forward_u32(feedback.feedback_epoch, feedback_->feedback_epoch)) {
        return false;
    }
    feedback_ = feedback;
    feedback_at_ms_ = now_ms;
    return true;
}

JsccShadowResult JsccRuntimeShadow::evaluate(
    const JsccShadowFrameInput& frame) const {
    JsccShadowResult out;
    if (!feedback_) return out;
    out.feedback_epoch = feedback_->feedback_epoch;
    out.feedback_age_ms = static_cast<uint32_t>(std::min<uint64_t>(
        UINT32_MAX, frame.now_ms >= feedback_at_ms_
                        ? frame.now_ms - feedback_at_ms_
                        : 0));
    if (frame.now_ms < feedback_at_ms_ ||
        out.feedback_age_ms > cfg_.feedback_timeout_ms) {
        out.fallback = JsccShadowFallback::kFeedbackStale;
        return out;
    }
    if ((feedback_->valid_flags & jscc_feedback_flags::kRepairReady) == 0) {
        out.fallback = JsccShadowFallback::kRepairNotReady;
        return out;
    }
    if ((feedback_->valid_flags & jscc_feedback_flags::kRttReady) == 0 ||
        feedback_->rtt_samples < cfg_.min_rtt_samples) {
        out.fallback = JsccShadowFallback::kRttNotReady;
        return out;
    }
    if (!frame.source_tx_remaining_us || !frame.resend_airtime_us) {
        out.fallback = JsccShadowFallback::kAirtimeUnavailable;
        return out;
    }
    if (frame.deadline_us == 0) {
        out.fallback = JsccShadowFallback::kDeadlineUnavailable;
        return out;
    }

    out.input.source_k = frame.source_k;
    out.input.predicted_loss_symbols =
        rate_symbols(feedback_->repair_demand_permille, frame.source_k);
    out.input.fec_floor_symbols =
        rate_symbols(cfg_.fec_floor_permille, frame.source_k);
    out.input.fec_cap_symbols =
        rate_symbols(cfg_.fec_cap_permille, frame.source_k);
    out.input.deadline_us = frame.deadline_us;
    out.input.source_tx_remaining_us = *frame.source_tx_remaining_us;
    out.input.rtt_p95_us = feedback_->rtt_p95_us;
    out.input.resend_airtime_us = *frame.resend_airtime_us;
    out.input.arq_guard_us = cfg_.arq_guard_us;
    out.input.arq_capable = frame.arq_capable;
    out.decision = jscc_inner_decide(out.input);
    out.valid = true;
    out.fallback = JsccShadowFallback::kNone;
    return out;
}

void JsccRuntimeShadow::reset() {
    feedback_.reset();
    feedback_at_ms_ = 0;
}

const char* jscc_shadow_fallback_string(JsccShadowFallback fallback) {
    switch (fallback) {
        case JsccShadowFallback::kNone: return "none";
        case JsccShadowFallback::kFeedbackMissing: return "feedback_missing";
        case JsccShadowFallback::kFeedbackStale: return "feedback_stale";
        case JsccShadowFallback::kRepairNotReady: return "repair_not_ready";
        case JsccShadowFallback::kRttNotReady: return "rtt_not_ready";
        case JsccShadowFallback::kAirtimeUnavailable: return "airtime_unavailable";
        case JsccShadowFallback::kDeadlineUnavailable: return "deadline_unavailable";
    }
    return "feedback_missing";
}

}  // namespace wblink
