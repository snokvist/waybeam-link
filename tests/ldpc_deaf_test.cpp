// SPDX-License-Identifier: GPL-2.0-or-later
// §15.3 (Pass 157) coding-deafness predicate: which adapter's silence about
// received coding is UNEXPLAINED. The whole point is that the blind die's own
// counter cannot answer, so the test is about the ORACLE — a reporting die
// having actually counted LDPC — not about the blind one.
#include "wblink/node/air_backend.h"

#include <vector>

#include "wbtest.h"

using wblink::AdapterStats;
using wblink::node::ldpc_deaf_adapter;

namespace {

AdapterStats ad(const char* name, bool flag_ok, uint64_t rx_ldpc) {
    AdapterStats a;
    a.name = name;
    a.ldpc_flag_ok = flag_ok;
    a.rx_ldpc = rx_ldpc;
    return a;
}

}  // namespace

int main() {
    {
        // Nothing to say on an all-BCC link: the reporting dies see no LDPC,
        // so the blind die's 0 is unremarkable. This is the fleet's normal
        // state and must stay silent, or the line is noise.
        const std::vector<AdapterStats> v = {ad("8733bu", false, 0),
                                             ad("8812au", true, 0),
                                             ad("8812cu", true, 0)};
        CHECK(ldpc_deaf_adapter(v) == nullptr);
    }
    {
        // The case this exists for: a reporting die counted LDPC, so LDPC is
        // demonstrably on air, and the blind die's 0 is now unexplained.
        const std::vector<AdapterStats> v = {ad("8733bu", false, 0),
                                             ad("8812au", true, 41),
                                             ad("8812cu", true, 0)};
        const AdapterStats* d = ldpc_deaf_adapter(v);
        CHECK(d != nullptr);
        if (d) CHECK(d->name == "8733bu");
    }
    {
        // No blind adapter: every die reports, so nobody's silence needs
        // explaining however much LDPC is flying.
        const std::vector<AdapterStats> v = {ad("8812au", true, 900),
                                             ad("8812cu", true, 900)};
        CHECK(ldpc_deaf_adapter(v) == nullptr);
    }
    {
        // A blind die alone is NOT a finding. With no reporting die there is
        // no oracle, so there is no evidence LDPC is on air at all — the
        // rx_ldpc it carries is 0 by construction and proves nothing. Guards
        // the direction that would fire on every single-8733BU ground.
        const std::vector<AdapterStats> v = {ad("8733bu", false, 0)};
        CHECK(ldpc_deaf_adapter(v) == nullptr);
    }
    {
        // A blind die's OWN nonzero rx_ldpc cannot be the oracle for itself.
        // Contrived (the flag being false is what makes the counter dead),
        // but it pins that the oracle must be a reporting die.
        const std::vector<AdapterStats> v = {ad("blind", false, 77)};
        CHECK(ldpc_deaf_adapter(v) == nullptr);
    }
    {
        // Empty set: no crash, no finding.
        const std::vector<AdapterStats> v;
        CHECK(ldpc_deaf_adapter(v) == nullptr);
    }

    return wbtest_finish("ldpc_deaf");
}
