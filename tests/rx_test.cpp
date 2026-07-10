// SPDX-License-Identifier: GPL-2.0-or-later
// §6 merged RX state machine, fake time throughout: admission+latch, dedup +
// diversity accounting, in-order delivery, each §6.2 short-circuit fired in
// isolation, §6.6 clamp, supersession vs NACK eligibility, deadline drops,
// stall watchdog, re-NACK backoff + quiesce, best-effort fallback (§3.4),
// idle teardown.
#include "wblink/rx.h"

#include <cstring>
#include <string>
#include <vector>

#include "wbtest.h"

using namespace wblink;

namespace {

constexpr uint16_t kTxOrig = 17;
constexpr uint32_t kTxSession = 0x01020304;
constexpr uint8_t kTv = 0x2B;  // "matching" table_version

struct Harness {
    RxEngine engine;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> delivered;

    explicit Harness(const RxPolicy& p = RxPolicy{},
                     std::optional<uint8_t> local_tv = kTv)
        : engine(p, {WantSpec{0, stream_type::kRtp, kTxOrig}}, nullptr,
                 local_tv) {}

    RxEngine::Deliver sink() {
        return [this](uint8_t sid, const uint8_t* d, size_t n) {
            delivered.emplace_back(sid, std::vector<uint8_t>(d, d + n));
        };
    }

    // Feed a DATA packet with 1-byte payload = low byte of seq.
    void feed(uint8_t adapter, uint32_t seq, uint32_t block, uint8_t flags,
              uint64_t now, uint8_t table_version = kTv) {
        DataHeader h;
        h.prefix = {kTxOrig, 0, kTxSession};
        h.stream_id = 0;
        h.stream_type = stream_type::kRtp;
        h.seq = seq;
        h.block_id = block;
        h.data_flags = flags;
        h.active_profile = 0;
        h.table_version = table_version;
        const uint8_t payload = static_cast<uint8_t>(seq & 0xFF);
        DataView v;
        v.hdr = h;
        v.payload = &payload;
        v.payload_len = 1;
        engine.on_data(adapter, v, now, sink());
    }

    // Latch quickly: 3 packets within the admission window. Note §2
    // semantics: the first two packets are consumed by admission (dropped,
    // no back-fill); the startup floor is seq 2 and it is the first
    // delivery. After latch(): delivered == [2], uniq == 1.
    void latch(uint64_t t0 = 0) {
        feed(0, 0, 0, 0, t0);
        feed(0, 1, 0, 0, t0 + 1);
        feed(0, 2, 0, 0, t0 + 2);
    }

    const RxStreamCounters& counters() {
        static RxStreamCounters empty;
        auto ss = engine.streams();
        return ss.empty() ? empty : (cached_ = ss[0].counters, cached_);
    }
    RxStreamCounters cached_;
};

constexpr uint8_t EOB = data_flags::kEndOfBlock;
constexpr uint8_t ARQ = data_flags::kArq;

}  // namespace

int main() {
    // --- admission control + latch + startup floor --------------------------
    {
        Harness h;
        h.feed(0, 100, 10, 0, 0);  // one-shot packet: never latches
        CHECK_EQ_U(h.engine.streams().size(), 0);
        h.feed(0, 101, 10, 0, 1);
        CHECK_EQ_U(h.engine.streams().size(), 0);
        h.feed(0, 102, 10, 0, 2);  // 3rd within window -> latch, floor=102
        CHECK_EQ_U(h.engine.streams().size(), 1);
        // Delivery starts at the floor; nothing back-filled.
        CHECK_EQ_U(h.delivered.size(), 1);
        CHECK_EQ_U(h.delivered[0].second[0], 102 & 0xFF);

        // A stale window restarts the count.
        Harness h2;
        h2.feed(0, 0, 0, 0, 0);
        h2.feed(0, 1, 0, 0, 500);
        h2.feed(0, 2, 0, 0, 2000);  // window expired between 2nd and 3rd
        CHECK_EQ_U(h2.engine.streams().size(), 0);
    }

    // --- non-matching originator never latches ------------------------------
    {
        Harness h;
        DataHeader hdr;
        hdr.prefix = {99, 0, 7};  // wrong originator
        hdr.stream_type = stream_type::kRtp;
        hdr.table_version = kTv;
        const uint8_t b = 0;
        DataView v{hdr, &b, 1};
        for (int i = 0; i < 5; ++i) {
            v.hdr.seq = static_cast<uint32_t>(i);
            h.engine.on_data(0, v, static_cast<uint64_t>(i), h.sink());
        }
        CHECK_EQ_U(h.engine.streams().size(), 0);
    }

    // --- dedup across adapters = diversity, in-order delivery ---------------
    {
        Harness h;
        h.latch();
        h.feed(0, 3, 1, 0, 10);
        h.feed(1, 3, 1, 0, 11);  // duplicate copy from adapter 1
        h.feed(1, 4, 1, 0, 12);
        h.feed(0, 4, 1, 0, 13);  // duplicate copy from adapter 0
        CHECK_EQ_U(h.delivered.size(), 3);  // 2,3,4 in order (floor = 2)
        CHECK_EQ_U(h.counters().diversity, 2);
        CHECK_EQ_U(h.counters().uniq, 3);
        for (size_t i = 0; i < h.delivered.size(); ++i) {
            CHECK_EQ_U(h.delivered[i].second[0], i + 2);
        }
    }

    // --- §6.2-1: all live adapters advanced => lost immediately -------------
    {
        Harness h;
        h.latch();
        // Two adapters both deliver past seq 3 (never seen, ARQ block).
        h.feed(0, 4, 1, ARQ, 10);
        h.feed(1, 5, 1, ARQ | EOB, 11);
        h.feed(0, 5, 1, ARQ | EOB, 12);
        h.feed(1, 4, 1, ARQ, 13);
        // seq 3 is now behind both adapters' last (4,5... wait adapter0 last=5,
        // adapter1 last=5) -> declared lost, NACK-eligible immediately.
        auto nacks = h.engine.build_nacks(14);
        CHECK_EQ_U(nacks.size(), 1);
        if (!nacks.empty()) {
            CHECK_EQ_U(nacks[0].base_seq, 3);
            CHECK_EQ_U(nacks[0].target_originator, kTxOrig);
            CHECK_EQ_U(nacks[0].target_session, kTxSession);
            CHECK_EQ_U(nacks[0].bitmap.size(), 1);
            CHECK_EQ_U(nacks[0].bitmap[0], 0x01);
        }
        CHECK_EQ_U(h.counters().lost_declared, 1);
        // Delivery is blocked at the gap (in-deadline, recoverable).
        CHECK_EQ_U(h.delivered.size(), 1);  // just the floor (2)

        // The RETRANSMIT arrives: recovered, delivery resumes in order.
        h.feed(0, 3, 1, ARQ | data_flags::kRetransmit, 20);
        CHECK_EQ_U(h.counters().recovered_arq, 1);
        CHECK_EQ_U(h.delivered.size(), 4);  // 2,3,4,5
        CHECK_EQ_U(h.delivered[1].second[0], 3);
        // Quiesce: no further NACKs for it.
        CHECK_EQ_U(h.engine.build_nacks(21).size(), 0);
    }

    // --- §6.2-1 must NOT fire while one live adapter lags --------------------
    {
        Harness h;
        h.latch();
        h.feed(1, 2, 0, 0, 5);   // adapter 1 latches onto the stream, last=2
        h.feed(0, 4, 1, ARQ, 10);  // adapter 0 ahead; gap at 3
        // adapter 1 (live, last=2) has not advanced past 3 -> not lost yet.
        CHECK_EQ_U(h.engine.build_nacks(11).size(), 0);
        CHECK_EQ_U(h.counters().lost_declared, 0);
        // adapter 1 catches up past the gap -> now lost.
        h.feed(1, 5, 1, ARQ, 12);
        CHECK_EQ_U(h.counters().lost_declared, 1);
    }

    // --- §6.2-2 supersession: newer block => older gaps dropped, no NACK ----
    {
        Harness h;
        h.latch();
        h.feed(0, 3, 1, ARQ, 10);          // block 1 starts
        h.feed(0, 5, 2, ARQ | EOB, 12);    // block 2 seen; gap at 4 (block<=2)
        // seq 4's nearest-above is seq 5 (block 2 == max_block): NOT
        // superseded (could belong to the live block).
        h.feed(0, 6, 3, ARQ | EOB, 14);    // block 3; now nearest-above(4)=5,
                                           // block 2 < max_block 3 => superseded
        h.engine.tick(15, h.sink());
        // Superseded gap: dropped, cursor advanced, never NACKed.
        CHECK_EQ_U(h.engine.build_nacks(16).size(), 0);
        CHECK_EQ_U(h.counters().dropped_superseded, 1);
        // Held packets after the hole were delivered (never withheld).
        CHECK_EQ_U(h.delivered.size(), 4);  // 2,3,5,6
        CHECK(h.delivered.size() == 4 && h.delivered[2].second[0] == 5);
    }

    // --- §6.6 clamp: forged far-future seq/block rejected --------------------
    {
        Harness h;
        h.latch();
        h.feed(0, 1000000, 1, 0, 10);      // far-future seq
        h.feed(0, 4, 4000, 0, 11);         // far-future block
        CHECK_EQ_U(h.counters().clamp_rejected, 2);
        CHECK_EQ_U(h.counters().uniq, 1);  // only the floor packet counted
        // The forged block did NOT poison supersession: normal traffic flows.
        h.feed(0, 3, 1, EOB, 12);
        CHECK_EQ_U(h.delivered.size(), 2);  // 2,3
    }

    // --- §6.2-3 dwell ceiling: single adapter, gap declared by timer --------
    {
        RxPolicy p;
        p.dwell_ceiling_ms = 20;
        Harness h(p);
        h.latch();
        // Introduce a second live adapter lagging at seq 2 BEFORE the gap
        // appears, so SC1 stays blocked and only the dwell timer can fire.
        h.feed(1, 2, 0, 0, 5);
        h.feed(0, 4, 1, ARQ, 100);  // gap at 3
        CHECK_EQ_U(h.counters().lost_declared, 0);
        h.engine.tick(105, h.sink());
        CHECK_EQ_U(h.counters().lost_declared, 0);  // dwell not reached
        h.engine.tick(125, h.sink());               // 25ms > 20ms ceiling
        CHECK_EQ_U(h.counters().lost_declared, 1);
    }

    // --- deadline: gap past its block deadline is dropped --------------------
    {
        RxPolicy p;
        p.default_deadline_iframe_ms = 50;
        p.renack_attempts = 100;  // don't run out before the deadline
        Harness h(p);
        h.latch();
        h.feed(0, 4, 1, ARQ, 100);
        h.feed(1, 5, 1, ARQ | EOB, 101);  // gap 3 declared via SC1
        CHECK_EQ_U(h.counters().lost_declared, 1);
        CHECK(h.engine.build_nacks(102).size() == 1);
        // Past block-1 deadline (first_seen 100 + 50): dropped, cursor moves.
        h.engine.tick(151, h.sink());
        CHECK_EQ_U(h.counters().dropped_deadline, 1);
        CHECK_EQ_U(h.delivered.size(), 3);  // 2,4,5
        CHECK_EQ_U(h.engine.build_nacks(152).size(), 0);
    }

    // --- re-NACK backoff + attempt cap ---------------------------------------
    {
        RxPolicy p;
        p.renack_attempts = 3;
        p.renack_backoff_ms = 15;
        p.default_deadline_iframe_ms = 10000;  // deadline out of the way
        Harness h(p);
        h.latch();
        h.feed(0, 4, 1, ARQ, 10);
        h.feed(1, 5, 1, ARQ, 11);  // gap 3 declared, first NACK due now
        CHECK_EQ_U(h.engine.build_nacks(12).size(), 1);   // attempt 1
        CHECK_EQ_U(h.engine.build_nacks(13).size(), 0);   // backoff holds
        CHECK_EQ_U(h.engine.build_nacks(27).size(), 1);   // attempt 2 (+15)
        CHECK_EQ_U(h.engine.build_nacks(90).size(), 1);   // attempt 3
        CHECK_EQ_U(h.engine.build_nacks(500).size(), 0);  // cap reached
        CHECK_EQ_U(h.counters().nacks_sent, 3);
    }

    // --- coalescing: several gaps -> one bitmap -------------------------------
    {
        Harness h;
        h.latch();
        h.feed(0, 3, 1, ARQ, 10);
        h.feed(0, 7, 1, ARQ, 11);
        h.feed(1, 7, 1, ARQ, 12);  // gaps 4,5,6 all behind both adapters
        auto nacks = h.engine.build_nacks(13);
        CHECK_EQ_U(nacks.size(), 1);
        if (!nacks.empty()) {
            CHECK_EQ_U(nacks[0].base_seq, 4);
            CHECK_EQ_U(nacks[0].bitmap.size(), 1);
            CHECK_EQ_U(nacks[0].bitmap[0], 0x07);  // bits 0,1,2 = seqs 4,5,6
        }
    }

    // --- §3.4 best-effort fallback on table_version mismatch -----------------
    {
        Harness h;
        h.latch();
        h.feed(0, 3, 1, ARQ, 10, /*table_version=*/0x99);  // mismatch
        CHECK(h.engine.streams()[0].best_effort);
        // Gap at 4 (mismatched stream): declared but never NACKed.
        h.feed(0, 5, 1, ARQ, 11, 0x99);
        h.feed(1, 5, 1, ARQ, 12, 0x99);
        h.engine.tick(50, h.sink());
        CHECK_EQ_U(h.engine.build_nacks(51).size(), 0);
        // Lost gap skipped, later packets still delivered by diversity.
        CHECK_EQ_U(h.delivered.size(), 3);  // 2,3,5
        CHECK_EQ_U(h.counters().table_mismatch, 3);
    }

    // --- §6.5 stall watchdog: stalled adapter leaves the SC1 fast path -------
    {
        RxPolicy p;
        p.stall_timeout_ms = 200;
        p.dwell_ceiling_ms = 100000;  // keep SC3 out of the way
        Harness h(p);
        h.latch();
        h.feed(1, 2, 0, 0, 5);       // adapter 1 heard the stream, last=2
        h.feed(0, 4, 1, ARQ, 10);    // gap at 3; adapter 1 lags -> no SC1
        h.engine.tick(50, h.sink());
        CHECK_EQ_U(h.counters().lost_declared, 0);
        // Adapter 1 goes silent while 0 keeps delivering -> stalled at +200ms.
        h.feed(0, 5, 1, ARQ, 300);
        CHECK_EQ_U(h.counters().lost_declared, 1);  // SC1 now fires without it
        CHECK_EQ_U(h.engine.live_adapter_count(), 1);
    }

    // --- §6.6 sustained-clamp resync: outage recovery, forgery still hard ----
    {
        RxPolicy p;
        p.clamp_resync_ms = 500;
        Harness h(p);
        h.latch();
        // A single forged far-future packet: rejected, no state change.
        h.feed(0, 900000, 90000, 0, 10);
        CHECK_EQ_U(h.counters().clamp_rejected, 1);
        CHECK_EQ_U(h.counters().resyncs, 0);
        // Legit traffic keeps flowing -> the storm window resets.
        h.feed(0, 3, 0, 0, 20);
        // A real outage: the TX ran far ahead; everything now clamps...
        h.feed(0, 5000, 500, 0, 1000);
        h.feed(0, 5001, 500, 0, 1200);
        h.feed(0, 5002, 500, 0, 1400);
        CHECK_EQ_U(h.counters().resyncs, 0);  // still inside the window
        // ...until the storm has lasted clamp_resync_ms: re-floor.
        h.feed(0, 5003, 500, 0, 1600);
        CHECK_EQ_U(h.counters().resyncs, 1);
        CHECK_EQ_U(h.delivered.back().second[0], static_cast<uint8_t>(5003));
        // Stream is live again under the new floor.
        h.feed(0, 5004, 500, 0, 1601);
        CHECK_EQ_U(h.delivered.back().second[0], static_cast<uint8_t>(5004));
    }

    // --- §2 idle teardown ------------------------------------------------------
    {
        RxPolicy p;
        p.idle_teardown_ms = 5000;
        Harness h(p);
        h.latch();
        CHECK_EQ_U(h.engine.streams().size(), 1);
        h.engine.tick(6000, h.sink());
        CHECK_EQ_U(h.engine.streams().size(), 0);
    }

    // --- backward-step guards: a tick 1 ms behind a packet stamp must not ----
    // underflow into an instant teardown (regression: the loopback event loop
    // stamped on_data from a fresh clock inside the inject callback, then
    // ticked with an older captured now — streams were flushed mid-flight).
    {
        RxPolicy p;
        p.idle_teardown_ms = 5000;
        Harness h(p);
        h.latch(1000);  // last activity at t=1002
        CHECK_EQ_U(h.engine.streams().size(), 1);
        h.engine.tick(1001, h.sink());  // 1 ms behind the last packet
        CHECK_EQ_U(h.engine.streams().size(), 1);
        h.engine.tick(1002, h.sink());  // equal is not "idle" either
        CHECK_EQ_U(h.engine.streams().size(), 1);
    }

    // ...and a clamp-rejected packet 1 ms behind the storm-window start must
    // not underflow into an instant §6.6 resync (that would hand a forger
    // the one-packet video flush the clamp exists to prevent).
    {
        RxPolicy p;
        p.clamp_resync_ms = 500;
        Harness h(p);
        h.latch(1000);
        h.feed(0, 900000, 90000, 0, 2000);  // forged: opens the storm window
        CHECK_EQ_U(h.counters().clamp_rejected, 1);
        h.feed(0, 900001, 90000, 0, 1999);  // 1 ms behind the window start
        CHECK_EQ_U(h.counters().clamp_rejected, 2);
        CHECK_EQ_U(h.counters().resyncs, 0);  // guarded: zero elapsed, no flush
        // The window still works normally once real time has passed.
        h.feed(0, 900002, 90000, 0, 2600);
        CHECK_EQ_U(h.counters().resyncs, 1);
    }

    return wbtest_finish("rx_test");
}
