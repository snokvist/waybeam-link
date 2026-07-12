// SPDX-License-Identifier: GPL-2.0-or-later
// Pure per-frame JSCC protection decision (PROTOCOL.md §14.2).
#pragma once

#include <cstdint>

namespace wblink {

enum class JsccReason : uint8_t {
    kDeadlineUnreachable,
    kFecCapacityLimited,
    kFecAndArq,
    kFecOnly,
    kArqOnly,
    kUnprotected,
};

struct JsccInnerInput {
    uint16_t source_k = 0;
    uint16_t predicted_loss_symbols = 0;
    uint16_t fec_floor_symbols = 0;
    uint16_t fec_cap_symbols = 0;
    uint32_t deadline_us = 0;
    uint32_t elapsed_us = 0;
    uint32_t source_tx_remaining_us = 0;
    uint32_t rtt_p95_us = 0;
    uint32_t resend_airtime_us = 0;
    uint32_t arq_guard_us = 0;
    bool arq_capable = false;
};

struct JsccInnerDecision {
    uint16_t parity_symbols = 0;
    uint32_t remaining_after_source_us = 0;
    bool arq_eligible = false;
    bool discard = false;
    bool fec_capacity_limited = false;
    JsccReason reason = JsccReason::kUnprotected;
};

JsccInnerDecision jscc_inner_decide(const JsccInnerInput& input);
const char* jscc_reason_string(JsccReason reason);

}  // namespace wblink
