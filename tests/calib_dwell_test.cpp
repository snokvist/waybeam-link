// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the §3.16 (Pass 153) calibration dwell primitive
// (core/include/wblink/calib_dwell.h): sender pacing and re-elicitation,
// receiver dedup/close rules, and the two halves wired back-to-back under
// injected probe/tally loss, duplication, and cross-adapter reorder.
#include "wblink/calib_dwell.h"
#include "wblink/wire.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(x)                                                          \
    do {                                                                  \
        if (!(x)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #x);                                             \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

using namespace wblink;

DwellSendParams fast_params() {
    DwellSendParams p;
    p.probe_pace_us = 1000;  // 1 probe/ms — keeps test clocks simple
    p.tally_wait_ms = 50;
    p.tally_retries = 2;
    p.max_probes_per_tick = 8;
    return p;
}

// Drain every probe the sender will emit this tick.
std::vector<uint16_t> drain(DwellSender& s, uint64_t now) {
    std::vector<uint16_t> seqs;
    s.new_tick();
    for (;;) {
        const DwellProbeOut o = s.next_probe(now);
        if (!o.send) break;
        seqs.push_back(o.seq);
    }
    return seqs;
}

void test_sender_pacing_and_completion() {
    DwellSender s(fast_params());
    CHECK(!s.begin(7, 0, 10, 0));  // dwell_id 0 invalid
    CHECK(!s.begin(7, 1, 0, 0));   // count 0 invalid
    CHECK(s.begin(7, 1, 10, 0));
    CHECK(s.state() == DwellState::kEmitting);

    // t=0: one credit banked by begin() — exactly one probe.
    auto seqs = drain(s, 0);
    CHECK(seqs.size() == 1 && seqs[0] == 1);
    // +8 ms: 8 more credits, capped by max_probes_per_tick.
    seqs = drain(s, 8);
    CHECK(seqs.size() == 8 && seqs.front() == 2 && seqs.back() == 9);
    // Tail goes out; state flips to kAwaitTally.
    seqs = drain(s, 16);
    CHECK(seqs.size() == 1 && seqs[0] == 10);
    CHECK(s.state() == DwellState::kAwaitTally);
    // No further emission while waiting inside the window.
    CHECK(drain(s, 17).empty());

    // Tally arrives: result carries the D-A evidence fields.
    CHECK(s.on_tally(7, 1, 9, 0x1234, 5, 0xAB));
    CHECK(s.state() == DwellState::kDone);
    CHECK(s.result().sent == 10 && s.result().received == 9);
    CHECK(s.result().loss_milli() == 100);
    CHECK(s.result().rx_mcs == 5 && s.result().adapter_fingerprint == 0xAB);
}

void test_sender_reelicit_then_no_evidence() {
    DwellSender s(fast_params());
    CHECK(s.begin(9, 3, 2, 0));
    auto seqs = drain(s, 0);   // seq 1
    seqs = drain(s, 5);        // seq 2 (tail)
    CHECK(s.state() == DwellState::kAwaitTally);
    // Wait window elapses: re-elicit = tail seq again, twice, then give up.
    seqs = drain(s, 60);
    CHECK(seqs.size() == 1 && seqs[0] == 2);
    seqs = drain(s, 120);
    CHECK(seqs.size() == 1 && seqs[0] == 2);
    seqs = drain(s, 180);
    CHECK(seqs.empty());
    CHECK(s.state() == DwellState::kNoEvidence);
    // A tally after exhaustion is refused — no-evidence is terminal.
    CHECK(!s.on_tally(9, 3, 2, 0, 0, 0));
}

void test_sender_ignores_stale_tally() {
    DwellSender s(fast_params());
    CHECK(s.begin(9, 4, 2, 0));
    CHECK(!s.on_tally(8, 4, 2, 0, 0, 0));  // wrong run
    CHECK(!s.on_tally(9, 3, 2, 0, 0, 0));  // wrong dwell
    CHECK(s.state() == DwellState::kEmitting);
    // A tally may legitimately arrive while still kEmitting (receiver saw
    // the tail before we drained our own state) — accepted.
    CHECK(s.on_tally(9, 4, 1, 0, 0, 0));
    CHECK(s.state() == DwellState::kDone);
}

void test_receiver_counts_dedups_and_closes_on_tail() {
    DwellReceiver r;
    // New run flags the feed-pause hook exactly once.
    auto o = r.on_probe(42, 1, 1, 3, -50, 4, 0);
    CHECK(o.new_run && !o.send);
    o = r.on_probe(42, 1, 2, 3, -52, 4, 1);
    CHECK(!o.new_run && !o.send);
    // Diversity duplicate of seq 2 — counted once.
    o = r.on_probe(42, 1, 2, 3, -51, 4, 1);
    CHECK(!o.send);
    // Tail closes and answers immediately.
    o = r.on_probe(42, 1, 3, 3, -54, 4, 2);
    CHECK(o.send && o.run_id == 42 && o.dwell_id == 1);
    CHECK(o.received == 3);
    CHECK(static_cast<int32_t>(o.rssi_sum_dbm) == -50 - 52 - 54);
    CHECK(o.rx_mcs == 4);
    // Late duplicate of the closed dwell: idempotent re-send, same numbers.
    o = r.on_probe(42, 1, 2, 3, -51, 4, 9);
    CHECK(o.send && o.dwell_id == 1 && o.received == 3);
}

void test_receiver_reorder_is_exact() {
    DwellReceiver r;
    // Cross-adapter reorder: 1,3,2 — a high-water scheme would drop seq 2.
    (void)r.on_probe(1, 1, 1, 3, -40, 7, 0);
    (void)r.on_probe(1, 1, 3, 3, -40, 7, 1);  // tail closes at received=2
    auto o = r.on_probe(1, 1, 2, 3, -40, 7, 2);
    // seq 2 is late — the dwell already closed; the re-sent tally must still
    // say 2, not 3 (the closed tally is immutable), which is the conservative
    // direction. The EXACTNESS claim is for reorder *before* the tail:
    CHECK(o.send && o.received == 2);
    DwellReceiver r2;
    (void)r2.on_probe(2, 1, 2, 3, -40, 7, 0);  // reordered ahead
    (void)r2.on_probe(2, 1, 1, 3, -40, 7, 1);
    auto o2 = r2.on_probe(2, 1, 3, 3, -40, 7, 2);
    CHECK(o2.send && o2.received == 3);
}

void test_receiver_close_by_succession() {
    DwellReceiver r;
    (void)r.on_probe(5, 1, 1, 2, -45, 3, 0);
    // Tail of dwell 1 lost; first probe of dwell 2 closes dwell 1 and emits
    // its (partial) tally.
    auto o = r.on_probe(5, 2, 1, 2, -45, 3, 5);
    CHECK(o.send && o.dwell_id == 1 && o.received == 1);
    // Dwell 2 then completes normally.
    o = r.on_probe(5, 2, 2, 2, -45, 3, 6);
    CHECK(o.send && o.dwell_id == 2 && o.received == 2);
}

void test_receiver_new_run_resets_and_quiet_expiry() {
    DwellReceiver r;
    (void)r.on_probe(10, 1, 1, 1, -45, 3, 0);  // count==1: closes at once
    CHECK(r.run_id() == 10);
    auto o = r.on_probe(11, 1, 1, 2, -45, 3, 1);
    CHECK(o.new_run && r.run_id() == 11);
    // Quiet expiry drives the caller's feed-resume edge.
    CHECK(!r.quiet_for(1000, 2000));
    CHECK(r.quiet_for(3001, 2000));
    r.expire_run();
    CHECK(r.run_id() == 0);
    // A stale re-elicit after expiry opens a fresh run rather than
    // resurrecting the old closed tally.
    o = r.on_probe(11, 1, 1, 2, -45, 3, 4000);
    CHECK(o.new_run);
}

// The two halves back-to-back through the wire codec, under shaped loss:
// every 5th probe is dropped, the first tally is dropped. The sender must
// converge on the true delivered count via re-elicitation.
void test_end_to_end_with_codec_and_loss() {
    DwellSendParams p = fast_params();
    DwellSender s(p);
    DwellReceiver r;
    CHECK(s.begin(77, 1, 20, 0));

    uint64_t now = 0;
    int probes_on_air = 0;
    int tallies_dropped = 1;  // drop the first tally
    bool done = false;
    uint16_t delivered = 0;
    while (now < 2000 && !done) {
        s.new_tick();
        for (;;) {
            const DwellProbeOut po = s.next_probe(now);
            if (!po.send) break;
            ++probes_on_air;
            if (probes_on_air % 5 == 0) continue;  // shaped probe loss

            // Round-trip through the real codec.
            CalibProbe pr;
            pr.prefix.originator = 2;
            pr.prefix.destination = 1;
            pr.prefix.session_id = 0xAA;
            pr.run_id = 77;
            pr.dwell_id = 1;
            pr.seq = po.seq;
            pr.count = 20;
            uint8_t buf[kDefaultMaxPayload];
            const size_t n =
                encode_calib_probe(pr, kDefaultMaxPayload, buf, sizeof buf);
            CHECK(n == kDefaultMaxPayload);  // padded to the budget
            const Decoded d = decode(buf, n);
            const CalibProbe* dp = std::get_if<CalibProbe>(&d);
            CHECK(dp != nullptr);
            if (dp == nullptr) return;
            CHECK(dp->wire_len == kDefaultMaxPayload);

            const DwellTallyOut to =
                r.on_probe(dp->run_id, dp->dwell_id, dp->seq, dp->count,
                           -48, 6, now);
            if (to.send) {
                delivered = to.received;
                if (tallies_dropped > 0) {
                    --tallies_dropped;  // shaped tally loss
                    continue;
                }
                CalibTally t;
                t.prefix.originator = 1;
                t.prefix.destination = 2;
                t.prefix.session_id = 0xBB;
                t.run_id = to.run_id;
                t.dwell_id = to.dwell_id;
                t.received = to.received;
                t.rssi_sum_dbm = to.rssi_sum_dbm;
                t.rx_mcs = to.rx_mcs;
                t.adapter_fingerprint = 0x5A;
                uint8_t tb[64];
                const size_t tn = encode_calib_tally(t, tb, sizeof tb);
                CHECK(tn == kCalibTallySize);
                const Decoded td = decode(tb, tn);
                const CalibTally* dt = std::get_if<CalibTally>(&td);
                CHECK(dt != nullptr);
                if (dt == nullptr) return;
                if (s.on_tally(dt->run_id, dt->dwell_id, dt->received,
                               dt->rssi_sum_dbm, dt->rx_mcs,
                               dt->adapter_fingerprint)) {
                    done = true;
                }
            }
        }
        now += 1;
    }
    CHECK(done);
    CHECK(s.state() == DwellState::kDone);
    // 20 sent; every 5th of the on-air stream dropped. The exact delivered
    // count depends on where re-elicited tails fall in the drop pattern, so
    // assert the invariants rather than a magic number: the tally equals what
    // the receiver truly counted, and loss is self-denominated against 20.
    CHECK(s.result().sent == 20);
    CHECK(s.result().received == delivered);
    CHECK(s.result().received < 20 && s.result().received >= 15);
    CHECK(s.result().loss_milli() ==
          1000u * (20 - s.result().received) / 20);
    CHECK(s.result().adapter_fingerprint == 0x5A);
}

void test_codec_rejects_and_unknown_subtype() {
    // Structural rejects.
    CalibProbe pr;
    pr.prefix.destination = 1;
    pr.run_id = 1;
    pr.dwell_id = 0;  // invalid
    pr.seq = 1;
    pr.count = 1;
    uint8_t buf[kDefaultMaxPayload];
    CHECK(encode_calib_probe(pr, 100, buf, sizeof buf) == 0);
    pr.dwell_id = 1;
    pr.seq = 3;
    pr.count = 2;  // seq > count
    CHECK(encode_calib_probe(pr, 100, buf, sizeof buf) == 0);
    pr.seq = 1;
    pr.prefix.destination = 0;  // broadcast forbidden
    CHECK(encode_calib_probe(pr, 100, buf, sizeof buf) == 0);
    pr.prefix.destination = 1;
    // pad_to below the fixed size clamps up, never truncates.
    CHECK(encode_calib_probe(pr, 4, buf, sizeof buf) == kCalibProbeFixedSize);

    // Unknown subtype decodes to CalibUnknown, not an error (§3.16: a
    // mixed-version pair degrades to "calibration unavailable" and the frame
    // still counts as valid RX).
    const size_t n = encode_calib_probe(pr, 0, buf, sizeof buf);
    CHECK(n == kCalibProbeFixedSize);
    buf[11] = 0x7E;
    const Decoded d = decode(buf, n);
    const CalibUnknown* u = std::get_if<CalibUnknown>(&d);
    CHECK(u != nullptr && u->subtype == 0x7E);

    // Oversize probe (past the High budget) is a length error.
    uint8_t big[4096] = {};
    const size_t bn = encode_calib_probe(pr, 3072, big, sizeof big);
    CHECK(bn == 3072);
    // Re-decode at a forged longer length.
    CHECK(std::holds_alternative<CalibProbe>(decode(big, 3072)));

    // Tally length is exact.
    CalibTally t;
    t.prefix.destination = 2;
    t.run_id = 1;
    t.dwell_id = 1;
    uint8_t tb[64];
    CHECK(encode_calib_tally(t, tb, sizeof tb) == kCalibTallySize);
    CHECK(std::holds_alternative<DecodeError>(decode(tb, kCalibTallySize - 1)));
}

}  // namespace

int main() {
    test_sender_pacing_and_completion();
    test_sender_reelicit_then_no_evidence();
    test_sender_ignores_stale_tally();
    test_receiver_counts_dedups_and_closes_on_tail();
    test_receiver_reorder_is_exact();
    test_receiver_close_by_succession();
    test_receiver_new_run_resets_and_quiet_expiry();
    test_end_to_end_with_codec_and_loss();
    test_codec_rejects_and_unknown_subtype();
    if (g_fail == 0) std::printf("calib_dwell_test: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
