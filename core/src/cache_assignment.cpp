// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/cache_assignment.h"

#include <algorithm>

namespace wblink {

bool CacheAssignmentGate::retired(uint32_t session) const {
    return std::find(retired_sessions_.begin(), retired_sessions_.end(),
                     session) != retired_sessions_.end();
}

CacheAssignmentGate::Verdict CacheAssignmentGate::evaluate(
    const CacheAssign& a) const {
    if (a.prefix.originator != controller_) return Verdict::kWrongController;
    if (a.prefix.destination != self_ || a.target_cache != self_) {
        return Verdict::kWrongCache;
    }
    if (!current_) return Verdict::kApply;
    if (a.prefix.session_id != current_->prefix.session_id) {
        return retired(a.prefix.session_id) ? Verdict::kStale
                                            : Verdict::kApply;
    }
    if (a.assignment_epoch < current_->assignment_epoch) {
        return Verdict::kStale;
    }
    if (a.assignment_epoch > current_->assignment_epoch) {
        return Verdict::kApply;
    }
    return a == *current_ ? Verdict::kDuplicate : Verdict::kEpochConflict;
}

void CacheAssignmentGate::commit(const CacheAssign& a) {
    if (current_ && current_->prefix.session_id != a.prefix.session_id) {
        retired_sessions_.push_back(current_->prefix.session_id);
        while (retired_sessions_.size() > 8) retired_sessions_.pop_front();
    }
    current_ = a;
}

}  // namespace wblink
