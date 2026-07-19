// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>

#include "wblink/jscc_controller.h"
#include "wblink/wire.h"

namespace wblink {

struct JsccRuntimeShadowConfig {
    uint16_t fec_floor_permille = 0;
    uint16_t fec_cap_permille = 0;
    uint32_t arq_guard_us = 0;
    uint32_t feedback_timeout_ms = 0;
    uint16_t min_rtt_samples = 0;
};

enum class JsccShadowFallback : uint8_t {
    kNone,
    kFeedbackMissing,
    kFeedbackStale,
    kRepairNotReady,
    kRttNotReady,
    kAirtimeUnavailable,
    kDeadlineUnavailable,
};

struct JsccShadowFrameInput {
    uint16_t source_k = 0;
    uint32_t deadline_us = 0;
    std::optional<uint32_t> source_tx_remaining_us;
    std::optional<uint32_t> resend_airtime_us;
    bool arq_capable = false;
    uint64_t now_ms = 0;
};

struct JsccShadowResult {
    bool valid = false;
    JsccShadowFallback fallback = JsccShadowFallback::kFeedbackMissing;
    uint32_t feedback_epoch = 0;
    uint32_t feedback_age_ms = 0;
    JsccInnerInput input;
    JsccInnerDecision decision;
};

class JsccRuntimeShadow {
  public:
    explicit JsccRuntimeShadow(const JsccRuntimeShadowConfig& cfg) : cfg_(cfg) {}

    // Caller validates reporter/target identity. Epochs are monotonic within
    // one reporter session; a receiver reboot starts a new epoch domain.
    bool observe_feedback(const JsccFeedback& feedback, uint64_t now_ms);
    JsccShadowResult evaluate(const JsccShadowFrameInput& frame) const;
    void reset();

  private:
    JsccRuntimeShadowConfig cfg_;
    std::optional<JsccFeedback> feedback_;
    uint64_t feedback_at_ms_ = 0;
};

const char* jscc_shadow_fallback_string(JsccShadowFallback fallback);

}  // namespace wblink
