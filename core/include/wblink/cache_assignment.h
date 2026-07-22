// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §3.13 receiver-owned cache assignment replay gate.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include "wblink/wire.h"

namespace wblink {

class CacheAssignmentGate {
  public:
    enum class Verdict : uint8_t {
        kApply,
        kDuplicate,
        kWrongController,
        kWrongCache,
        kStale,
        kEpochConflict,
    };

    CacheAssignmentGate(uint16_t self_originator,
                        uint16_t controller_originator)
        : self_(self_originator), controller_(controller_originator) {}

    // Pure decision. Call commit() only after the radio retune succeeds so a
    // failed apply remains retryable with the same epoch.
    Verdict evaluate(const CacheAssign& a) const;
    void commit(const CacheAssign& a);

    const std::optional<CacheAssign>& current() const { return current_; }

  private:
    bool retired(uint32_t session) const;

    uint16_t self_ = 0;
    uint16_t controller_ = 0;
    std::optional<CacheAssign> current_;
    std::deque<uint32_t> retired_sessions_;
};

}  // namespace wblink
