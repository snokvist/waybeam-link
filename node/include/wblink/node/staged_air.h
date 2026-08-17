// SPDX-License-Identifier: GPL-2.0-or-later
// StagedAir — the one place the batched-TX ordering rule lives (Pass 184).
//
// §15.2 `air.usb_tx_agg` lets the backend carry several frames in one USB
// bulk-OUT transfer. Only frames the framer emitted back to back are ever
// batched, so nothing is deferred — but a frame that is STAGED is not yet on
// the air, and anything sent by another route while a batch is pending would
// overtake it. The wire order would then depend on whether batching happened
// to be on, which is precisely what must not be true.
//
// The first cut of this change enforced that with ten hand-placed
// `flush_staged()` calls in run_tx. That is a discipline, not an invariant:
// adversarial review deleted all ten and every test still passed, because
// nothing could see the rule. Here the rule is structural instead — an
// unbatched send cannot be written without its flush, because `send_now`
// and `resend` ARE the flush.
//
// Templated on the air type so a unit test can instantiate it over a fake
// `AirIface` and exercise this exact code. That is the point: the body below
// is production, not a model of production.
#ifndef WBLINK_NODE_STAGED_AIR_H
#define WBLINK_NODE_STAGED_AIR_H

#include <cstddef>
#include <cstdint>

namespace wblink {
namespace node {

template <class Air>
class StagedAir {
  public:
    explicit StagedAir(Air& air) : air_(air) {}

    // Batched path: a frame the producer emitted as part of a fan-out. May
    // sit staged until the batch fills or the caller flushes.
    size_t stage(const uint8_t* f, size_t n) { return air_.inject_staged(f, n); }

    // Unbatched paths. Each flushes FIRST — that is what makes it impossible
    // to add one of these without preserving order.
    size_t send_now(const uint8_t* f, size_t n) {
        flush();
        return air_.inject(f, n);
    }
    size_t resend(const uint8_t* f, size_t n) {
        flush();
        return air_.inject_resend(f, n);
    }

    // Explicit boundary flush. Still hand-placed, because "the fan-out ended"
    // is a fact about the tick's structure that no wrapper can infer — but it
    // is now the ONLY hand-placed rule, and its failure mode is a frame
    // arriving one boundary late rather than out of order.
    size_t flush() { return air_.flush_staged(); }

  private:
    Air& air_;
};

}  // namespace node
}  // namespace wblink

#endif  // WBLINK_NODE_STAGED_AIR_H
