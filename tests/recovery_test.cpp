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

    // --- mid-stream re-arm on a repaired frame (§3.9) ------------------------
    // A latch is not the only bootstrap-relevant moment, only the one this
    // layer could originally detect. A §6.3b-repaired frame is decodable but
    // is NOT what was sent, so the reference chain is damaged from there.
    {
        LatchRecovery lr;
        uint64_t now = 0;
        // Latch, ask once, get answered: the schedule stands down.
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 1);
        lr.note_irap(0);
        CHECK(lr.settled(kA));
        now += 5000;
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 0);

        // Damage re-arms it, and because the previous round WAS answered the
        // re-arm is immediate -- the mechanism demonstrably works here.
        lr.note_damage(0, now);
        auto due = lr.due(one(kA, 0), now);
        CHECK_EQ_U(due.size(), 1);
        CHECK(due[0] == kA);
        CHECK_EQ_U(lr.attempts(kA), 1);
    }

    // A schedule already in flight is left alone: resetting its attempt count
    // on every damaged frame of a bad patch would defeat the bound outright.
    {
        LatchRecovery lr;
        uint64_t now = 0;
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 1);   // attempt 1
        now += LatchRecovery::kPeriodMs;
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 1);   // attempt 2
        CHECK_EQ_U(lr.attempts(kA), 2);
        lr.note_damage(0, now);                          // must NOT reset
        CHECK_EQ_U(lr.attempts(kA), 2);
        // The bound still arrives on schedule.
        for (int i = 2; i < LatchRecovery::kAttempts; ++i) {
            now += LatchRecovery::kPeriodMs;
            lr.due(one(kA, 0), now);
        }
        CHECK(lr.settled(kA));
        CHECK_EQ_U(lr.attempts(kA), LatchRecovery::kAttempts);
    }

    // A round that stood down WITHOUT ever being answered is backed off. This
    // is the case §3.9's bound exists for -- a craft with venc.recovery_enabled
    // false, or an encoder that ignores the call -- and re-arming on every
    // damaged frame would draw a return every second for the rest of the
    // flight, which is precisely what the bound forbids.
    {
        LatchRecovery lr;
        uint64_t now = 0;
        for (int i = 0; i < LatchRecovery::kAttempts; ++i) {
            lr.due(one(kA, 0), now);
            now += LatchRecovery::kPeriodMs;
        }
        CHECK(lr.settled(kA));                       // never answered
        CHECK_EQ_U(lr.attempts(kA), LatchRecovery::kAttempts);

        lr.note_damage(0, now);                      // inside the backoff
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 0);
        CHECK_EQ_U(lr.attempts(kA), LatchRecovery::kAttempts);

        now += LatchRecovery::kRearmBackoffMs;       // backoff elapsed
        lr.note_damage(0, now);
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 1);
        CHECK_EQ_U(lr.attempts(kA), 1);
    }

    // Each re-armed round must re-prove that it can be answered: an answered
    // round does not buy immediate re-arms forever.
    {
        LatchRecovery lr;
        uint64_t now = 0;
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 1);
        lr.note_irap(0);                              // answered
        lr.note_damage(0, now);                       // immediate re-arm
        CHECK_EQ_U(lr.attempts(kA), 0);
        // Burn this round WITHOUT an IRAP.
        for (int i = 0; i < LatchRecovery::kAttempts; ++i) {
            lr.due(one(kA, 0), now);
            now += LatchRecovery::kPeriodMs;
        }
        CHECK(lr.settled(kA));
        // The earlier IRAP must not still be granting immediate re-arms.
        lr.note_damage(0, now);
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 0);
    }

    // Damage on a stream this schedule never tracked is inert, and the local
    // id is the addressing key (it is what note_irap already uses).
    {
        LatchRecovery lr;
        uint64_t now = 0;
        lr.note_damage(9, now);                       // untracked: no crash
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 1);
        lr.note_irap(0);
        now += 5000;
        lr.note_damage(9, now);                       // wrong local id
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 0);
        lr.note_damage(0, now);                       // right one
        CHECK_EQ_U(lr.due(one(kA, 0), now).size(), 1);
    }

    return wbtest_finish("recovery_test");
}
