// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/cache_assignment.h"

#include "wbtest.h"

using namespace wblink;

namespace {
CacheAssign assignment(uint32_t session, uint32_t epoch, uint16_t vehicle) {
    CacheAssign a;
    a.prefix = {9, 33, session};
    a.target_cache = 33;
    a.target_originator = vehicle;
    a.assignment_epoch = epoch;
    a.target_chan = 5805;
    a.target_bw = 0;
    a.target_net_id = 2;
    return a;
}
}  // namespace

int main() {
    CacheAssignmentGate gate(33, 9);
    CacheAssign a = assignment(100, 1, 17);
    CHECK(gate.evaluate(a) == CacheAssignmentGate::Verdict::kApply);
    CHECK(gate.evaluate(a) == CacheAssignmentGate::Verdict::kApply);
    gate.commit(a);
    CHECK(gate.evaluate(a) == CacheAssignmentGate::Verdict::kDuplicate);

    CacheAssign conflict = a;
    conflict.target_originator = 18;
    CHECK(gate.evaluate(conflict) ==
          CacheAssignmentGate::Verdict::kEpochConflict);
    conflict.assignment_epoch = 2;
    CHECK(gate.evaluate(conflict) == CacheAssignmentGate::Verdict::kApply);
    gate.commit(conflict);
    CHECK(gate.evaluate(a) == CacheAssignmentGate::Verdict::kStale);

    CacheAssign reboot = assignment(200, 1, 19);
    CHECK(gate.evaluate(reboot) == CacheAssignmentGate::Verdict::kApply);
    gate.commit(reboot);
    CHECK(gate.evaluate(conflict) == CacheAssignmentGate::Verdict::kStale);

    CacheAssign wrong = reboot;
    wrong.prefix.originator = 10;
    CHECK(gate.evaluate(wrong) ==
          CacheAssignmentGate::Verdict::kWrongController);
    wrong = reboot;
    wrong.target_cache = 34;
    CHECK(gate.evaluate(wrong) == CacheAssignmentGate::Verdict::kWrongCache);

    return wbtest_finish("cache_assignment_test");
}
