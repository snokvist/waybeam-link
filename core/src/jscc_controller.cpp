// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/jscc_controller.h"

#include <algorithm>
#include <cstdint>

#include "wblink/types.h"

namespace wblink {

JsccInnerDecision jscc_inner_decide(const JsccInnerInput& in) {
    JsccInnerDecision out;
    const uint64_t source_done = static_cast<uint64_t>(in.elapsed_us) +
                                 in.source_tx_remaining_us;
    if (source_done > in.deadline_us) {
        out.discard = true;
        out.reason = JsccReason::kDeadlineUnreachable;
        return out;
    }
    out.remaining_after_source_us =
        static_cast<uint32_t>(in.deadline_us - source_done);

    const uint16_t desired =
        std::max(in.predicted_loss_symbols, in.fec_floor_symbols);
    const uint16_t gf_cap =
        in.source_k <= kFecMaxSymbols
            ? static_cast<uint16_t>(kFecMaxSymbols - in.source_k)
            : 0;
    const uint16_t cap = std::min(in.fec_cap_symbols, gf_cap);
    out.parity_symbols = std::min(desired, cap);
    out.fec_capacity_limited = desired > cap;

    if (out.fec_capacity_limited) {
        out.reason = JsccReason::kFecCapacityLimited;
    } else if (out.parity_symbols > 0) {
        out.reason = JsccReason::kFecOnly;
    } else {
        out.reason = JsccReason::kUnprotected;
    }
    return out;
}

const char* jscc_reason_string(JsccReason reason) {
    switch (reason) {
        case JsccReason::kDeadlineUnreachable: return "deadline_unreachable";
        case JsccReason::kFecCapacityLimited: return "fec_capacity_limited";
        case JsccReason::kFecOnly: return "fec_only";
        case JsccReason::kUnprotected: return "unprotected";
    }
    return "unprotected";
}

}  // namespace wblink
