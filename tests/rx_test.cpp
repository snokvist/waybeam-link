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
        return [this](uint8_t sid, uint32_t, uint8_t, const uint8_t* d,
                      size_t n) {
            delivered.emplace_back(sid, std::vector<uint8_t>(d, d + n));
        };
    }

    // Feed a DATA packet with 1-byte payload = low byte of seq.
    void feed(uint8_t adapter, uint32_t seq, uint32_t block, uint8_t flags,
              uint64_t now, uint8_t table_version = kTv,
              const RxEngine::EarlyDeliver& early_deliver = {},
              uint32_t session = kTxSession,
              uint16_t originator = kTxOrig) {
        DataHeader h;
        h.prefix = {originator, 0, session};
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
        engine.on_data(adapter, v, now, sink(), 0, early_deliver);
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

}  // namespace

int main() {
    // --- Pass 67 live vehicle selection tears down the prior latch ----------
    {
        Harness h;
        h.latch();
        CHECK(h.engine.selected_originator() == kTxOrig);
        h.engine.select_originator(18);
        CHECK_EQ_U(h.engine.streams().size(), 0);
        CHECK(h.engine.selected_originator() == 18);
        // Old vehicle is rejected; the new vehicle passes normal N=3 admission.
        h.feed(0, 3, 1, 0, 10);
        CHECK_EQ_U(h.engine.streams().size(), 0);
        h.feed(0, 0, 0, 0, 11, kTv, {}, 55, 18);
        h.feed(0, 1, 0, 0, 12, kTv, {}, 55, 18);
        h.feed(0, 2, 0, 0, 13, kTv, {}, 55, 18);
        CHECK_EQ_U(h.engine.streams().size(), 1);
        CHECK_EQ_U(h.engine.streams()[0].key.originator, 18);
    }

    // --- §15.5 Pass 206 unpin_originator detaches unconditionally ----------
    {
        Harness h;
        h.latch();
        CHECK(h.engine.selected_originator() == kTxOrig);
        CHECK_EQ_U(h.engine.streams().size(), 1);
        // Unlike select_originator(0), which is a no-op, this clears the pin.
        h.engine.unpin_originator();
        CHECK(!h.engine.selected_originator().has_value());
        CHECK_EQ_U(h.engine.streams().size(), 0);
        // Sticky first-admitted: a different craft now passes normal admission.
        h.feed(0, 0, 0, 0, 20, kTv, {}, 55, 18);
        h.feed(0, 1, 0, 0, 21, kTv, {}, 55, 18);
        CHECK_EQ_U(h.engine.streams().size(), 0);
        h.feed(0, 2, 0, 0, 22, kTv, {}, 55, 18);
        CHECK_EQ_U(h.engine.streams().size(), 1);
        CHECK_EQ_U(h.engine.streams()[0].key.originator, 18);
    }

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

    // --- same-originator reboot replaces the session after admission -------
    {
        Harness h;
        h.latch();
        constexpr uint32_t reboot_session = 0x55667788;
        std::vector<uint32_t> observed_sessions;
        const RxEngine::EarlyDeliver early =
            [&](const StreamKey& source, uint8_t, uint32_t, uint8_t,
                const uint8_t*, size_t) -> RxEngine::EarlyDeliverResult {
            observed_sessions.push_back(source.session_id);
            return {/*handled=*/true, /*block_complete=*/true};
        };
        h.feed(0, 0, 0, 0, 10, kTv, early, reboot_session);
        h.feed(0, 1, 0, 0, 11, kTv, early, reboot_session);
        CHECK_EQ_U(h.engine.streams()[0].key.session_id, kTxSession);
        h.feed(0, 2, 0, EOB, 12, kTv, early, reboot_session);
        CHECK_EQ_U(h.engine.streams().size(), 1u);
        CHECK_EQ_U(h.engine.streams()[0].key.session_id, reboot_session);
        CHECK_EQ_U(observed_sessions.size(), 1u);
        CHECK_EQ_U(observed_sessions[0], reboot_session);
    }

    // --- frame-SHM symbols bypass packet ordering after diversity dedup -----
    {
        Harness h;
        h.latch();
        std::vector<uint32_t> early_seq;
        const RxEngine::EarlyDeliver early =
            [&](const StreamKey&, uint8_t, uint32_t, uint8_t, const uint8_t* d,
                size_t) -> RxEngine::EarlyDeliverResult {
            early_seq.push_back(*d);
            return {/*handled=*/true, /*block_complete=*/true};
        };
        // seq 3 is absent, but seq 4 reaches the equation-oriented consumer
        // immediately and declares block 1 complete.
        h.feed(0, 4, 1, 0 | EOB, 10, kTv, early);
        h.feed(1, 4, 1, 0 | EOB, 11, kTv, early);  // diversity duplicate
        CHECK_EQ_U(early_seq.size(), 1);
        CHECK_EQ_U(early_seq[0], 4);
        CHECK_EQ_U(h.counters().diversity, 1);
        CHECK_EQ_U(h.counters().lost_declared, 0);
        // Ordered payload delivery did not duplicate the early symbol.
        CHECK_EQ_U(h.delivered.size(), 1);  // admission floor only
        CHECK_EQ_U(h.counters().delivered, 2);  // floor + early seq 4
        CHECK_EQ_U(h.counters().dropped_deadline, 0);
        CHECK_EQ_U(h.counters().dropped_superseded, 0);
    }

    // --- a cache/FEC completion edge retires an already-declared gap --------
    {
        Harness h;
        h.latch();
        h.feed(0, 4, 1, 0 | EOB, 10);  // seq 3 declared lost
        CHECK_EQ_U(h.counters().lost_declared, 1);
        h.engine.complete_frame(0, 1, 11, h.sink());
        CHECK_EQ_U(h.counters().dropped_deadline, 0);
        CHECK_EQ_U(h.counters().dropped_superseded, 0);
        // The satisfied hole no longer holds the generic cursor.
        CHECK_EQ_U(h.delivered.size(), 2);  // floor 2, then held seq 4
    }

    // --- §6.2-1: all live adapters advanced => lost immediately -------------
    {
        Harness h;
        h.latch();
        // Two adapters both deliver past seq 3 (never seen).
        h.feed(0, 4, 1, 0, 10);
        h.feed(1, 5, 1, EOB, 11);
        h.feed(0, 5, 1, EOB, 12);
        h.feed(1, 4, 1, 0, 13);
        // seq 3 is now behind both adapters' last -> declared lost.
        CHECK_EQ_U(h.counters().lost_declared, 1);
        // Delivery is blocked at the gap (in-deadline, FEC-pending).
        CHECK_EQ_U(h.delivered.size(), 1);  // just the floor (2)

        // A late original fills the gap; delivery resumes in order. With no
        // ARQ there is no NACK and no recovery counter.
        h.feed(0, 3, 1, 0, 20);
        CHECK_EQ_U(h.delivered.size(), 4);  // 2,3,4,5
        CHECK_EQ_U(h.delivered[1].second[0], 3);
    }

    // --- §6.2-1 must NOT fire while one live adapter lags --------------------
    {
        Harness h;
        h.latch();
        h.feed(1, 2, 0, 0, 5);   // adapter 1 latches onto the stream, last=2
        h.feed(0, 4, 1, 0, 10);  // adapter 0 ahead; gap at 3
        // adapter 1 (live, last=2) has not advanced past 3 -> not lost yet.
        CHECK_EQ_U(h.counters().lost_declared, 0);
        // adapter 1 catches up past the gap -> now lost.
        h.feed(1, 5, 1, 0, 12);
        CHECK_EQ_U(h.counters().lost_declared, 1);
    }

    // --- §6.2-2 supersession: newer block => older gaps dropped -------------
    {
        Harness h;
        h.latch();
        h.feed(0, 3, 1, 0, 10);          // block 1 starts
        h.feed(0, 5, 2, EOB, 12);        // block 2 seen; gap at 4 (block<=2)
        // seq 4's nearest-above is seq 5 (block 2 == max_block): NOT
        // superseded (could belong to the live block).
        h.feed(0, 6, 3, EOB, 14);        // block 3; now nearest-above(4)=5,
                                           // block 2 < max_block 3 => superseded
        h.engine.tick(15, h.sink());
        // Superseded gap: dropped, cursor advanced.
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
        h.feed(0, 4, 1, 0, 100);  // gap at 3
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
        Harness h(p);
        h.latch();
        h.feed(0, 4, 1, 0, 100);
        h.feed(1, 5, 1, EOB, 101);  // gap 3 declared via SC1
        CHECK_EQ_U(h.counters().lost_declared, 1);
        // Past block-1 deadline (first_seen 100 + 50): dropped, cursor moves.
        h.engine.tick(151, h.sink());
        CHECK_EQ_U(h.counters().dropped_deadline, 1);
        CHECK_EQ_U(h.delivered.size(), 3);  // 2,4,5
    }

    // --- §3.4 best-effort fallback on table_version mismatch -----------------
    {
        Harness h;
        h.latch();
        h.feed(0, 3, 1, 0, 10, /*table_version=*/0x99);  // mismatch
        CHECK(h.engine.streams()[0].best_effort);
        // Gap at 4 (mismatched stream): declared but never requested.
        h.feed(0, 5, 1, 0, 11, 0x99);
        h.feed(1, 5, 1, 0, 12, 0x99);
        h.engine.tick(50, h.sink());
        // Lost gap skipped, later packets still delivered by diversity.
        CHECK_EQ_U(h.delivered.size(), 3);  // 2,3,5
        CHECK_EQ_U(h.counters().table_mismatch, 3);
    }

    // --- §6.5 stall watchdog: stalled adapter leaves the SC1 fast path -------
    {
        RxPolicy p;
        p.stall_timeout_ms = 200;
        p.dwell_ceiling_ms = 100000;  // keep SC3 out of the way
        // Pass 205: one unified deadline is min(iframe, pframe), so hold both
        // far out of the way or the gap would be deadline-dropped before the
        // stall is observed.
        p.default_deadline_iframe_ms = 60000;
        p.default_deadline_pframe_ms = 60000;
        Harness h(p);
        h.latch();
        h.feed(1, 2, 0, 0, 5);       // adapter 1 heard the stream, last=2
        h.feed(0, 4, 1, 0, 10);    // gap at 3; adapter 1 lags -> no SC1
        h.engine.tick(50, h.sink());
        CHECK_EQ_U(h.counters().lost_declared, 0);
        // Adapter 1 goes silent while 0 keeps delivering -> stalled at +200ms.
        h.feed(0, 5, 1, 0, 300);
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

    // --- §3.7 pre-diversity estimator: per-adapter loss + reorder fill -------
    {
        Harness h;
        h.latch();
        h.feed(1, 2, 0, 0, 3);  // adapter 1 establishes its own anchor
        h.feed(0, 3, 0, 0, 4);
        h.feed(1, 3, 0, 0, 4);
        h.feed(1, 4, 0, 0, 5);  // adapter 0 misses seq 4
        h.feed(0, 5, 0, 0, 6);
        h.feed(1, 5, 0, 0, 6);
        CHECK_EQ_U(h.counters().prediv_expected, 8);
        CHECK_EQ_U(h.counters().prediv_lost, 1);
        h.feed(0, 4, 0, 0, 7);  // bounded reorder fills it exactly once
        h.feed(0, 4, 0, 0, 8);  // duplicate does not over-credit
        CHECK_EQ_U(h.counters().prediv_lost, 0);
        h.feed(0, 6, 0, data_flags::kRetransmit, 9);
        CHECK_EQ_U(h.counters().prediv_expected, 8);  // 0 excluded
        h.engine.reset_stats();
        CHECK_EQ_U(h.counters().prediv_expected, 0);
        CHECK_EQ_U(h.counters().prediv_lost, 0);
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

    // §2.1/§6.6 Pass 81: a u32 seq wrap must not enumerate the sequence
    // space. Latching at the top of the range walks the cursor across
    // 0xFFFFFFFF; before the fix the next packet spun ~4.29e9 iterations
    // inserting a Gap node each time (hang + OOM off four packets).
    {
        Harness h;
        h.feed(0, 0xFFFFFFFD, 0, 0, 1000);  // admission
        h.feed(0, 0xFFFFFFFE, 0, 0, 1001);  // admission
        h.feed(0, 0xFFFFFFFF, 0, 0, 1002);  // startup floor + delivered
        CHECK_EQ_U(h.counters().delivered, 1);
        // Cursor has now wrapped to 0. This packet must return promptly and
        // be treated as a §2.1 desync re-floor, not a 4-billion-gap walk.
        h.feed(0, 0, 1, 0, 1003);
        CHECK_EQ_U(h.counters().resyncs, 1);
        // ...and the stream keeps working under the new floor.
        h.feed(0, 1, 1, 0, 1004);
        h.feed(0, 2, 1, 0, 1005);
        CHECK(h.counters().delivered >= 2);
    }

    // §3.4 Pass 87: best-effort must keep ratcheting max_block. Before the
    // fix max_block froze at the latch value, so fwd_clamp_blocks blocks
    // later every packet clamp-rejected and the 500 ms resync flushed the
    // stream — forever. Drive many blocks with a mismatched table_version.
    {
        RxPolicy p;
        p.clamp_resync_ms = 500;
        Harness h(p);
        h.latch(1000);
        uint64_t t = 1010;
        for (uint32_t blk = 1; blk <= 40; ++blk, t += 33) {
            h.feed(0, 2 + blk, blk, 0, t, 0x99);  // 0x99 != kTv -> best-effort
        }
        CHECK(h.counters().table_mismatch > 0);  // fallback did engage
        // The whole point of §3.4: degrade gracefully. No clamp storm, no
        // state flush, and every packet delivered in order.
        CHECK_EQ_U(h.counters().clamp_rejected, 0);
        CHECK_EQ_U(h.counters().resyncs, 0);
        CHECK_EQ_U(h.counters().delivered, 41);  // floor + 40 blocks
    }

    return wbtest_finish("rx_test");
}
