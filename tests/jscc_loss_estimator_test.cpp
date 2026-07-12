// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/jscc_loss_estimator.h"

#include "wbtest.h"

using namespace wblink;

int main() {
    JsccLossEstimatorConfig cfg;
    cfg.window = 3;
    cfg.quantile_permille = 500;
    cfg.min_samples = 2;
    cfg.cold_start = 7;
    JsccLossEstimator estimator(cfg);

    CHECK_EQ_U(estimator.predict(), 7u);
    estimator.observe(1);
    CHECK_EQ_U(estimator.predict(), 7u);
    estimator.observe(5);
    CHECK_EQ_U(estimator.predict(), 1u);
    estimator.observe(9);
    estimator.observe(2);  // window is now [5, 9, 2]
    CHECK_EQ_U(estimator.predict(), 5u);
    CHECK_EQ_U(estimator.sample_count(), 3u);

    estimator.reset();
    CHECK_EQ_U(estimator.sample_count(), 0u);
    CHECK_EQ_U(estimator.predict(), 7u);

    JsccLossEstimator p95({20, 950, 20, 0});
    for (uint16_t value = 0; value < 20; ++value) p95.observe(value);
    CHECK_EQ_U(p95.predict(), 18u);  // nearest rank ceil(20 * .95)

    return wbtest_finish("jscc_loss_estimator_test");
}
