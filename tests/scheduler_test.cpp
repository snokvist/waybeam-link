// SPDX-License-Identifier: GPL-2.0-or-later
// §5.3/§12/§13 resend scheduler: global per-seq hold-down (1000 NACKs => one
// resend), importance/deadline/attempt gates, bitmap sanity clamp, budget
// partition with a fenced preferred share, preferred preemption +
// contested-only release, freshness ordering.
#include "wblink/scheduler.h"

#include <cstring>
#include <variant>
#include <vector>

#include "wbtest.h"

using namespace wblink;

namespace {

constexpr uint16_t kPilot = 9;
constexpr uint16_t kSpectator = 40;
constexpr uint16_t kSpectator2 = 41;

void push_frames(ResendRing& ring, uint32_t first_seq, uint32_t count,
                 uint64_t now, uint8_t flags = data_flags::kArq,
                 uint16_t payload = 64) {
    uint8_t frame[kDataHeaderSize + 1424];
    uint8_t payload_buf[1424] = {0};
    for (uint32_t s = first_seq; s < first_seq + count; ++s) {
        DataHeader h;
        h.prefix = {17, 0, 1};
        h.seq = s;
        h.block_id = s / 8;
        h.data_flags = flags;
        const size_t n =
            encode_data(h, payload_buf, payload, frame, sizeof(frame));
        ring.push(frame, n, h, now);
    }
}

NackView make_nack(uint16_t requester, uint32_t base,
                   std::vector<uint8_t>& bitmap_store,
                   std::initializer_list<uint32_t> offsets) {
    bitmap_store.assign(32, 0);
    uint32_t max_bit = 0;
    for (const uint32_t o : offsets) {
        bitmap_store[o / 8] =
            static_cast<uint8_t>(bitmap_store[o / 8] | (1u << (o % 8)));
        max_bit = o > max_bit ? o : max_bit;
    }
    NackView v;
    v.hdr.prefix = {requester, 0, 0xAABBCCDD};
    v.hdr.target_originator = 17;
    v.hdr.target_session = 1;
    v.hdr.target_stream_id = 0;
    v.hdr.base_seq = base;
    v.bitmap = bitmap_store.data();
    v.bitmap_len = static_cast<uint8_t>(max_bit / 8 + 1);
    return v;
}

struct Emitted {
    std::vector<std::vector<uint8_t>> frames;
    ResendScheduler::EmitResend cb() {
        return [this](const uint8_t* f, size_t n) {
            frames.emplace_back(f, f + n);
        };
    }
    uint32_t seq_of(size_t i) const {
        const Decoded d = decode(frames[i].data(), frames[i].size());
        const DataView* v = std::get_if<DataView>(&d);
        return v ? v->hdr.seq : 0xFFFFFFFF;
    }
};

SchedulerPolicy base_policy() {
    SchedulerPolicy p;
    p.holddown_ms = 20;
    p.attempt_cap = 3;
    p.preferred_originator = kPilot;
    p.release_timeout_ms = 500;
    p.interval_ms = 100;
    p.budget_floor_bytes = 1 << 20;  // budget out of the way by default
    return p;
}

}  // namespace

int main() {
    // --- basic resend: NACKed ARQ seq is re-emitted with RETRANSMIT ---------
    {
        ResendRing ring(RingConfig{10000, 1 << 20});
        push_frames(ring, 100, 10, 1000);
        ResendScheduler sched(base_policy(), nullptr);
        std::vector<uint8_t> bm;
        sched.on_nack(make_nack(kSpectator, 103, bm, {0, 2}), ring, 1001);
        Emitted out;
        sched.drain(ring, 1002, out.cb());
        CHECK_EQ_U(out.frames.size(), 2);
        CHECK_EQ_U(out.seq_of(0), 103);
        CHECK_EQ_U(out.seq_of(1), 105);
        const Decoded d = decode(out.frames[0].data(), out.frames[0].size());
        const DataView* v = std::get_if<DataView>(&d);
        CHECK(v != nullptr &&
              (v->hdr.data_flags & data_flags::kRetransmit) != 0);
        // Ring attempts were counted.
        CHECK_EQ_U(ring.find(103)->attempts, 1);
    }

    // --- global per-seq hold-down: a NACK storm yields ONE resend -----------
    {
        ResendRing ring(RingConfig{10000, 1 << 20});
        push_frames(ring, 100, 4, 1000);
        SchedulerPolicy p = base_policy();
        p.attempt_cap = 100;
        ResendScheduler sched(p, nullptr);
        Emitted out;
        std::vector<uint8_t> bm;
        for (int i = 0; i < 1000; ++i) {
            const uint16_t requester = static_cast<uint16_t>(100 + (i % 50));
            sched.on_nack(make_nack(requester, 101, bm, {0}), ring, 1001);
            sched.drain(ring, 1002, out.cb());
        }
        CHECK_EQ_U(out.frames.size(), 1);  // §13: amplification factor 1
        CHECK(sched.counters().holddown_suppressed > 0);
        // After the hold-down window a re-request may fire again.
        sched.on_nack(make_nack(kSpectator, 101, bm, {0}), ring, 1030);
        sched.drain(ring, 1030, out.cb());
        CHECK_EQ_U(out.frames.size(), 2);
    }

    // --- importance gate: non-ARQ seqs never resent --------------------------
    {
        ResendRing ring(RingConfig{10000, 1 << 20});
        push_frames(ring, 100, 4, 1000, /*flags=*/0);  // no ARQ bit
        ResendScheduler sched(base_policy(), nullptr);
        std::vector<uint8_t> bm;
        sched.on_nack(make_nack(kSpectator, 100, bm, {0, 1}), ring, 1001);
        Emitted out;
        sched.drain(ring, 1002, out.cb());
        CHECK_EQ_U(out.frames.size(), 0);
        CHECK_EQ_U(sched.counters().dropped_not_arq, 2);
    }

    // --- §13 bitmap sanity clamp ---------------------------------------------
    {
        ResendRing ring(RingConfig{10000, 1 << 20});
        push_frames(ring, 100, 10, 1000);
        SchedulerPolicy p = base_policy();
        p.max_block_pkts = 8;
        ResendScheduler sched(p, nullptr);
        std::vector<uint8_t> bm;
        // popcount 9 > 8: rejected outright.
        sched.on_nack(
            make_nack(kSpectator, 100, bm, {0, 1, 2, 3, 4, 5, 6, 7, 8}),
            ring, 1001);
        CHECK_EQ_U(sched.counters().nacks_rejected_sanity, 1);
        // base_seq far outside the ring window: rejected.
        sched.on_nack(make_nack(kSpectator, 500000, bm, {0}), ring, 1001);
        CHECK_EQ_U(sched.counters().nacks_rejected_sanity, 2);
        Emitted out;
        sched.drain(ring, 1002, out.cb());
        CHECK_EQ_U(out.frames.size(), 0);
    }

    // --- attempt cap ----------------------------------------------------------
    {
        ResendRing ring(RingConfig{10000, 1 << 20});
        push_frames(ring, 100, 2, 1000);
        SchedulerPolicy p = base_policy();
        p.attempt_cap = 2;
        p.holddown_ms = 1;  // let repeats through quickly
        ResendScheduler sched(p, nullptr);
        Emitted out;
        std::vector<uint8_t> bm;
        for (uint64_t t = 1001; t < 1050; t += 5) {
            sched.on_nack(make_nack(kSpectator, 100, bm, {0}), ring, t);
            sched.drain(ring, t + 1, out.cb());
        }
        CHECK_EQ_U(out.frames.size(), 2);  // capped
        CHECK(sched.counters().dropped_attempts > 0);
    }

    // --- deadline gate: too-old entries never resent --------------------------
    {
        ResendRing ring(RingConfig{10000, 1 << 20});
        push_frames(ring, 100, 2, 1000);  // ARQ class fallback budget = 50ms
        ResendScheduler sched(base_policy(), nullptr);
        std::vector<uint8_t> bm;
        sched.on_nack(make_nack(kSpectator, 100, bm, {0}), ring, 1060);
        Emitted out;
        sched.drain(ring, 1060, out.cb());  // 60ms > 50ms budget
        CHECK_EQ_U(out.frames.size(), 0);
        CHECK_EQ_U(sched.counters().dropped_deadline, 1);
    }

    // --- freshness ordering: nearest deadline first ---------------------------
    {
        ResendRing ring(RingConfig{10000, 1 << 20});
        push_frames(ring, 100, 1, 1000);  // older -> earlier deadline
        push_frames(ring, 101, 1, 1030);  // newer -> later deadline
        ResendScheduler sched(base_policy(), nullptr);
        std::vector<uint8_t> bm;
        sched.on_nack(make_nack(kSpectator, 100, bm, {0, 1}), ring, 1040);
        Emitted out;
        sched.drain(ring, 1041, out.cb());
        CHECK_EQ_U(out.frames.size(), 2);
        CHECK_EQ_U(out.seq_of(0), 100);  // most urgent first
        CHECK_EQ_U(out.seq_of(1), 101);
    }

    // --- budget partition: fenced shares, holder served first -----------------
    {
        ResendRing ring(RingConfig{10000, 1 << 20});
        // 90-byte frames (26 + 64).
        push_frames(ring, 100, 20, 1000);
        SchedulerPolicy p = base_policy();
        p.budget_floor_bytes = 4 * 90;  // room for 4 frames total
        ResendScheduler sched(p, nullptr);
        std::vector<uint8_t> bm1, bm2;
        // Pilot asks for 4 seqs, spectator for 4 different ones.
        sched.on_nack(make_nack(kPilot, 100, bm1, {0, 1, 2, 3}), ring, 1001);
        sched.on_nack(make_nack(kSpectator, 110, bm2, {0, 1, 2, 3}), ring,
                      1001);
        Emitted out;
        sched.drain(ring, 1002, out.cb());
        // Two active requesters -> 2 frames each; pilot (lock holder via
        // preemption) drains first.
        CHECK_EQ_U(out.frames.size(), 4);
        CHECK_EQ_U(out.seq_of(0), 100);
        CHECK_EQ_U(out.seq_of(1), 101);
        CHECK_EQ_U(out.seq_of(2), 110);
        CHECK_EQ_U(out.seq_of(3), 111);
        CHECK(sched.counters().budget_deferred > 0);
        CHECK_EQ_U(sched.counters().lock_holder, kPilot);
    }

    // --- §12 lock: first latcher, contested-only release, preemption ---------
    {
        ResendRing ring(RingConfig{100000, 1 << 20});
        push_frames(ring, 100, 50, 1000);
        SchedulerPolicy p = base_policy();
        p.attempt_cap = 100;
        p.holddown_ms = 1;
        ResendScheduler sched(p, nullptr);
        std::vector<uint8_t> bm;
        // Spectator latches first.
        sched.on_nack(make_nack(kSpectator, 100, bm, {0}), ring, 1001);
        CHECK_EQ_U(sched.counters().lock_holder, kSpectator);
        // A second non-preferred cannot steal while the holder is active.
        sched.on_nack(make_nack(kSpectator2, 101, bm, {0}), ring, 1002);
        CHECK_EQ_U(sched.counters().lock_holder, kSpectator);
        // Holder goes silent past release_timeout AND someone else asks.
        sched.on_nack(make_nack(kSpectator2, 102, bm, {0}), ring, 1600);
        CHECK_EQ_U(sched.counters().lock_holder, kSpectator2);
        // The pilot preempts immediately, mid-activity, unconditionally.
        sched.on_nack(make_nack(kSpectator2, 103, bm, {0}), ring, 1601);
        sched.on_nack(make_nack(kPilot, 104, bm, {0}), ring, 1602);
        CHECK_EQ_U(sched.counters().lock_holder, kPilot);
        // And a spectator NACK right after does NOT take it back.
        sched.on_nack(make_nack(kSpectator, 105, bm, {0}), ring, 1603);
        CHECK_EQ_U(sched.counters().lock_holder, kPilot);
    }

    // --- §12 Pass 116: the claim moves the lock, softly --------------------
    {
        ResendRing ring(RingConfig{100000, 1 << 20});
        push_frames(ring, 100, 50, 1000);
        SchedulerPolicy p = base_policy();
        p.preferred_originator = 0;  // unpinned craft (the shipping default)
        p.attempt_cap = 100;
        p.holddown_ms = 1;
        ResendScheduler sched(p, nullptr);
        std::vector<uint8_t> bm;
        // A bench station latches first and keeps NACKing, so contested
        // release never fires — the stickiness this pass exists to break.
        sched.on_nack(make_nack(kSpectator, 100, bm, {0}), ring, 1001);
        sched.on_nack(make_nack(kSpectator, 101, bm, {0}), ring, 1200);
        sched.on_nack(make_nack(kSpectator2, 102, bm, {0}), ring, 1300);
        CHECK_EQ_U(sched.counters().lock_holder, kSpectator);
        // The claim takes it immediately, mid-activity.
        sched.force_lock(kSpectator2);
        CHECK_EQ_U(sched.counters().lock_holder, kSpectator2);
        // SOFT: last_nack_ms_ was untouched, so the displaced node — still
        // actively asking — reclaims via the normal contested rule once the
        // new holder has been quiet for release_timeout_ms. This is the
        // deliberate difference from the §3.5 latch, which pins.
        sched.on_nack(make_nack(kSpectator, 103, bm, {0}), ring, 1900);
        CHECK_EQ_U(sched.counters().lock_holder, kSpectator);
        // release_lock parks it: the next NACKer takes it with no wait.
        sched.release_lock();
        CHECK_EQ_U(sched.counters().lock_holder, 0);
        sched.on_nack(make_nack(kSpectator2, 104, bm, {0}), ring, 1901);
        CHECK_EQ_U(sched.counters().lock_holder, kSpectator2);
        // originator 0 is the parked sentinel — never a valid holder.
        sched.force_lock(0);
        CHECK_EQ_U(sched.counters().lock_holder, kSpectator2);
    }

    // Pinned craft: config outranks the claim, exactly as in §3.5.
    {
        ResendRing ring(RingConfig{100000, 1 << 20});
        push_frames(ring, 100, 50, 1000);
        SchedulerPolicy p = base_policy();  // preferred_originator = kPilot
        p.attempt_cap = 100;
        p.holddown_ms = 1;
        ResendScheduler sched(p, nullptr);
        std::vector<uint8_t> bm;
        sched.on_nack(make_nack(kPilot, 100, bm, {0}), ring, 1001);
        CHECK_EQ_U(sched.counters().lock_holder, kPilot);
        sched.force_lock(kSpectator);  // refused
        CHECK_EQ_U(sched.counters().lock_holder, kPilot);
        sched.release_lock();          // refused
        CHECK_EQ_U(sched.counters().lock_holder, kPilot);
    }

    return wbtest_finish("scheduler_test");
}
