// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/jscc_loss_estimator.h"

#include <algorithm>

namespace wblink {

uint32_t JsccLossEstimator::predict() const {
    if (samples_.size() < cfg_.min_samples || samples_.empty()) {
        return cfg_.cold_start;
    }
    const size_t rank = std::max<size_t>(
        1, (samples_.size() * cfg_.quantile_permille + 999) / 1000);
    std::deque<uint32_t> ordered = samples_;
    std::sort(ordered.begin(), ordered.end());
    return ordered[rank - 1];
}

void JsccLossEstimator::observe(uint32_t sample) {
    samples_.push_back(sample);
    if (samples_.size() > cfg_.window) {
        samples_.pop_front();
    }
}

void JsccLossEstimator::reset() {
    samples_.clear();
}

}  // namespace wblink
