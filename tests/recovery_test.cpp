// SPDX-License-Identifier: GPL-2.0-or-later
// §3.9 Pass 106 latch-triggered decoder bootstrap schedule. A newly latched RTP
// stream is due a RECOVERY_REQUEST immediately, then at 1 Hz up to the bound;
// an IRAP on the matching egress ring stands the schedule down early; and a
// stream that unlatches and latches again re-arms rather than resuming a spent
// schedule.
#include "wblink/recovery.h"

#include <vector>

#include "wbtest.h"

using namespace wblink;

namespace {

StreamKey key(uint16_t origin, uint32_t session, uint8_t sid) {
    StreamKey k;
    k.originator = origin;
    k.session_id = session;
    k.stream_id = sid;
    return k;
}

const StreamKey kA = key(17, 1000, 0);
const StreamKey kB = key(21, 2000, 0);

std::vector<LatchStream> one(const StreamKey& k, uint8_t local) {
    return {LatchStream{k, local}};
}

}  // namespace

int main() {
    // --- fresh latch is due immediately, then paced at kPeriodMs ------------
    {
        LatchRecovery lr;
        auto due = lr.due(one(kA, 0), 5000);
        CHECK_EQ_U(due.size(), 1);
        CHECK(due[0] == kA);
        CHECK_EQ_U(lr.attempts(kA), 1);

        // Same tick and anything inside the period: not due again.
        CHECK_EQ_U(lr.due(one(kA, 0), 5000).size(), 0);
        CHECK_EQ_U(lr.due(one(kA, 0), 5999).size(), 0);
        CHECK_EQ_U(lr.attempts(kA), 1);

        // The period boundary is inclusive.
        CHECK_EQ_U(lr.due(one(kA, 0), 6000).size(), 1);
        CHECK_EQ_U(lr.attempts(kA), 2);
    }

    // --- the bound holds: exactly kAttempts emissions, then silence ---------
    {
        LatchRecovery lr;
        uint64_t now = 0;
        unsigned emitted = 0;
        for (int i = 0; i < 40; ++i) {
            emitted += static_cast<unsigned>(lr.due(one(kA, 0), now).size());
            now += LatchRecovery::kPeriodMs;
        }
        CHECK_EQ_U(emitted, LatchRecovery::kAttempts);
        CHECK_EQ_U(lr.attempts(kA), LatchRecovery::kAttempts);
        CHECK(lr.settled(kA));
    }

    // --- an IRAP on the egress ring stands the schedule down early ----------
    {
        LatchRecovery lr;
        CHECK_EQ_U(lr.due(one(kA, 3), 0).size(), 1);
        CHECK(!lr.settled(kA));
        lr.note_irap(3);
        CHECK(lr.settled(kA));
        uint64_t now = LatchRecovery::kPeriodMs;
        for (int i = 0; i < 10; ++i) {
            CHECK_EQ_U(lr.due(one(kA, 3), now).size(), 0);
            now += LatchRecovery::kPeriodMs;
        }
        CHECK_EQ_U(lr.attempts(kA), 1);
    }

    // --- an IRAP on a DIFFERENT local stream does not settle this one -------
    {
        LatchRecovery lr;
        CHECK_EQ_U(lr.due(one(kA, 3), 0).size(), 1);
        lr.note_irap(7);
        CHECK(!lr.settled(kA));
        CHECK_EQ_U(lr.due(one(kA, 3), LatchRecovery::kPeriodMs).size(), 1);
        CHECK_EQ_U(lr.attempts(kA), 2);
    }

    // --- unlatch forgets the key; a re-latch re-arms the full schedule ------
    {
        LatchRecovery lr;
        uint64_t now = 0;
        for (int i = 0; i < LatchRecovery::kAttempts; ++i) {
            CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 1);
            now += LatchRecovery::kPeriodMs;
        }
        CHECK(lr.settled(kA));

        lr.due({}, now);  // stream torn down
        CHECK_EQ_U(lr.attempts(kA), 0);
        CHECK(!lr.settled(kA));  // untracked, not settled

        now += LatchRecovery::kPeriodMs;
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 1);
        CHECK_EQ_U(lr.attempts(kA), 1);
    }

    // --- a settled stream does not suppress a second, independent stream ----
    {
        LatchRecovery lr;
        std::vector<LatchStream> both{LatchStream{kA, 0}, LatchStream{kB, 1}};
        auto due = lr.due(both, 0);
        CHECK_EQ_U(due.size(), 2);
        lr.note_irap(0);  // only stream A's egress got an IRAP
        CHECK(lr.settled(kA));
        CHECK(!lr.settled(kB));
        due = lr.due(both, LatchRecovery::kPeriodMs);
        CHECK_EQ_U(due.size(), 1);
        CHECK(due[0] == kB);
    }

    // --- same stream_id from a different session is a different key ---------
    // A craft reboot re-sessions the stream: that is a fresh bootstrap, not a
    // continuation of the old schedule.
    {
        LatchRecovery lr;
        uint64_t now = 0;
        for (int i = 0; i < LatchRecovery::kAttempts; ++i) {
            lr.due(one(kA, 0), now);
            now += LatchRecovery::kPeriodMs;
        }
        CHECK(lr.settled(kA));
        const StreamKey reborn = key(17, 1001, 0);
        auto due = lr.due(one(reborn, 0), now);
        CHECK_EQ_U(due.size(), 1);
        CHECK(due[0] == reborn);
        CHECK_EQ_U(lr.attempts(kA), 0);  // old session forgotten
    }

    return wbtest_finish("recovery_test");
}
