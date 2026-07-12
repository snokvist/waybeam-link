// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/jscc_controller.h"

#include <cstring>
#include <limits>

#include "wbtest.h"

using namespace wblink;

namespace {
JsccInnerInput base() {
    JsccInnerInput in;
    in.source_k = 100;
    in.predicted_loss_symbols = 8;
    in.fec_floor_symbols = 4;
    in.fec_cap_symbols = 32;
    in.deadline_us = 16000;
    in.elapsed_us = 2000;
    in.source_tx_remaining_us = 6000;
    in.rtt_p95_us = 4000;
    in.resend_airtime_us = 1000;
    in.arq_guard_us = 1000;
    in.arq_capable = true;
    return in;
}
}  // namespace

int main() {
    {
        const auto out = jscc_inner_decide(base());
        CHECK_EQ_U(out.parity_symbols, 8u);
        CHECK_EQ_U(out.remaining_after_source_us, 8000u);
        CHECK(out.arq_eligible);
        CHECK(!out.discard);
        CHECK(out.reason == JsccReason::kFecAndArq);
        CHECK(std::strcmp(jscc_reason_string(out.reason), "fec_and_arq") == 0);
    }
    {
        auto in = base();
        in.source_tx_remaining_us = 14001;
        const auto out = jscc_inner_decide(in);
        CHECK(out.discard);
        CHECK_EQ_U(out.parity_symbols, 0u);
        CHECK(out.reason == JsccReason::kDeadlineUnreachable);
    }
    {
        auto in = base();
        in.elapsed_us = 10000;
        in.source_tx_remaining_us = 6000;
        const auto out = jscc_inner_decide(in);
        CHECK(!out.discard);
        CHECK_EQ_U(out.remaining_after_source_us, 0u);
        CHECK(!out.arq_eligible);
        CHECK(out.reason == JsccReason::kFecOnly);
    }
    {
        auto in = base();
        in.predicted_loss_symbols = 2;
        in.fec_floor_symbols = 6;
        in.rtt_p95_us = 6000;
        in.resend_airtime_us = 1000;
        in.arq_guard_us = 1000;
        const auto out = jscc_inner_decide(in);
        CHECK_EQ_U(out.parity_symbols, 6u);
        CHECK(out.arq_eligible);
    }
    {
        auto in = base();
        in.source_k = 250;
        in.predicted_loss_symbols = 20;
        const auto out = jscc_inner_decide(in);
        CHECK_EQ_U(out.parity_symbols, 6u);
        CHECK(out.fec_capacity_limited);
        CHECK(!out.discard);
        CHECK(out.reason == JsccReason::kFecCapacityLimited);
    }
    {
        auto in = base();
        in.source_k = 300;
        const auto out = jscc_inner_decide(in);
        CHECK_EQ_U(out.parity_symbols, 0u);
        CHECK(out.fec_capacity_limited);
    }
    {
        auto in = base();
        in.predicted_loss_symbols = 0;
        in.fec_floor_symbols = 0;
        CHECK(jscc_inner_decide(in).reason == JsccReason::kArqOnly);
    }
    {
        auto in = base();
        in.predicted_loss_symbols = 0;
        in.fec_floor_symbols = 0;
        in.arq_capable = false;
        CHECK(jscc_inner_decide(in).reason == JsccReason::kUnprotected);
    }
    {
        auto in = base();
        in.elapsed_us = std::numeric_limits<uint32_t>::max();
        in.source_tx_remaining_us = std::numeric_limits<uint32_t>::max();
        in.deadline_us = std::numeric_limits<uint32_t>::max();
        CHECK(jscc_inner_decide(in).discard);
    }
    return wbtest_finish("jscc_controller_test");
}
