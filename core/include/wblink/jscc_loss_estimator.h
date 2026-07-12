// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace wblink {

struct JsccLossEstimatorConfig {
    size_t window = 120;
    uint16_t quantile_permille = 950;
    size_t min_samples = 20;
    uint16_t cold_start = 0;
};

class JsccLossEstimator {
  public:
    explicit JsccLossEstimator(const JsccLossEstimatorConfig& cfg = {})
        : cfg_(cfg) {}

    uint16_t predict() const;
    void observe(uint16_t lost_symbols);
    void reset();
    size_t sample_count() const { return samples_.size(); }

  private:
    JsccLossEstimatorConfig cfg_;
    std::deque<uint16_t> samples_;
    std::array<uint16_t, 257> histogram_{};
};

}  // namespace wblink
