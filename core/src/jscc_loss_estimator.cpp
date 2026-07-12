// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/jscc_loss_estimator.h"

#include <algorithm>

namespace wblink {

uint16_t JsccLossEstimator::predict() const {
    if (samples_.size() < cfg_.min_samples || samples_.empty()) {
        return cfg_.cold_start;
    }
    const size_t rank = std::max<size_t>(
        1, (samples_.size() * cfg_.quantile_permille + 999) / 1000);
    size_t cumulative = 0;
    for (size_t loss = 0; loss < histogram_.size(); ++loss) {
        cumulative += histogram_[loss];
        if (cumulative >= rank) {
            return static_cast<uint16_t>(loss);
        }
    }
    return 256;
}

void JsccLossEstimator::observe(uint16_t lost_symbols) {
    const uint16_t sample = std::min<uint16_t>(lost_symbols, 256);
    samples_.push_back(sample);
    ++histogram_[sample];
    if (samples_.size() > cfg_.window) {
        const uint16_t expired = samples_.front();
        samples_.pop_front();
        --histogram_[expired];
    }
}

void JsccLossEstimator::reset() {
    samples_.clear();
    histogram_.fill(0);
}

}  // namespace wblink
