// SPDX-License-Identifier: GPL-2.0-or-later
// §7.3 metric reporter: cadence, monotonic epoch, WINDOWED loss ‰ from
// counter deltas, RSSI best/mean from live adapters, one report per latched
// tuple, and the RSSI plumbing through RxEngine::on_data.
#include "wblink/reporter.h"

#include "wblink/rx.h"
#include "wbtest.h"

using namespace wblink;

namespace {

constexpr uint16_t kTxOrig = 17;
constexpr uint32_t kTxSession = 0x01020304;

struct Harness {
    RxEngine engine;
    Reporter reporter;

    Harness()
        : engine(RxPolicy{}, {WantSpec{0, stream_type::kRtp, kTxOrig}},
                 nullptr, std::nullopt),
          reporter(ReporterPolicy{100}, std::nullopt) {}

    void feed(uint8_t adapter, uint32_t seq, uint64_t now, int8_t rssi) {
        DataHeader h;
        h.prefix = {kTxOrig, 0, kTxSession};
        h.stream_id = 0;
        h.stream_type = stream_type::kRtp;
        h.seq = seq;
        h.block_id = seq;
        h.data_flags = data_flags::kEndOfBlock;
        const uint8_t payload = static_cast<uint8_t>(seq & 0xFF);
        DataView v;
        v.hdr = h;
        v.payload = &payload;
        v.payload_len = 1;
        engine.on_data(adapter, v, now,
                       [](uint8_t, uint32_t, uint8_t, const uint8_t*, size_t) {},
                       rssi);
    }
};

}  // namespace

int main() {
    Harness h;

    // Latch (admission eats the first 2) with distinct per-adapter RSSI.
    h.feed(0, 0, 0, -50);
    h.feed(0, 1, 1, -50);
    h.feed(0, 2, 2, -50);
    h.feed(1, 2, 2, -70);  // diversity copy on a weaker adapter

    // First build emits immediately (next_ms starts at 0), then respects the
    // 100 ms cadence.
    auto r1 = h.reporter.build(h.engine, 10);
    CHECK_EQ_U(r1.size(), 1);
    CHECK(h.reporter.build(h.engine, 50).empty());   // inside the window
    CHECK(!h.reporter.build(h.engine, 120).empty()); // next window

    // Target identity + epoch monotonic.
    CHECK_EQ_U(r1[0].target_originator, kTxOrig);
    CHECK_EQ_U(r1[0].target_session, kTxSession);
    CHECK_EQ_U(r1[0].target_stream_id, 0);
    // §3.5/§10.7: build() no longer spends an epoch. The number belongs to an
    // EMITTED report, so it is stamped into the frame at the radio call and
    // committed only on a successful submit — a report dropped between build
    // and injection (no uplink adapter, TX queue full) burns none, and the
    // next one reuses it. Otherwise §10.7's denominator counts emissions that
    // never happened and the seek reads phantom loss.
    CHECK_EQ_U(r1[0].report_epoch, 0);
    CHECK_EQ_U(h.reporter.next_epoch(), 1);
    CHECK_EQ_U(h.reporter.epoch(), 0);
    h.reporter.commit_epoch();
    CHECK_EQ_U(h.reporter.epoch(), 1);
    CHECK_EQ_U(h.reporter.next_epoch(), 2);
    // A failed submit commits nothing: next_epoch stands still.
    CHECK_EQ_U(h.reporter.next_epoch(), 2);
    CHECK_EQ_U(r1[0].probe_per, kNoProbe);

    // RSSI: best = strongest recent packet, mean = average of adapter EWMAs.
    CHECK(r1[0].rssi_best == -50);
    CHECK(r1[0].rssi_mean == -60);  // (-50 + -70) / 2
    CHECK_EQ_U(r1[0].adapters, 2);

    // Windowed loss: a clean window reports 0 even after historic loss.
    // Create a gap (seq 4 missing; both adapters advance past it => lost).
    h.feed(0, 3, 200, -50);
    h.feed(1, 3, 200, -70);
    h.feed(0, 5, 210, -50);
    h.feed(1, 5, 210, -70);
    h.engine.tick(230, [](uint8_t, uint32_t, uint8_t, const uint8_t*, size_t) {});
    auto r2 = h.reporter.build(h.engine, 240);
    CHECK_EQ_U(r2.size(), 1);
    CHECK(r2[0].loss_postdiv_prearq > 0);  // the window saw the loss
    CHECK_EQ_U(r2[0].uniq, 3);  // two delivered + one declared lost
    // Next window is clean again: the loss must NOT stick (windowed, not
    // lifetime).
    h.feed(0, 6, 300, -50);
    h.feed(0, 7, 310, -50);
    auto r3 = h.reporter.build(h.engine, 400);
    CHECK_EQ_U(r3.size(), 1);
    CHECK_EQ_U(r3[0].loss_postdiv_prearq, 0);
    CHECK_EQ_U(r3[0].report_epoch, 0);  // stamped by the injector, not here

    // The stamp is a plain in-place field write on an encoded frame, at the
    // same offset encode_link_report uses — that is what lets the epoch be
    // decided after the frame has been sitting in the §7.2 hold queue.
    {
        LinkReport probe = r3[0];
        probe.prefix = {9, kTxOrig, 4242};
        uint8_t frame[kLinkReportSize];
        CHECK_EQ_U(encode_link_report(probe, frame, sizeof(frame)),
                   kLinkReportSize);
        CHECK(link_report_stamp_epoch(frame, sizeof(frame), 77));
        const Decoded d = decode(frame, kLinkReportSize);
        const LinkReport* got = std::get_if<LinkReport>(&d);
        CHECK(got != nullptr);
        if (got != nullptr) {
            CHECK_EQ_U(got->report_epoch, 77);
            CHECK_EQ_U(got->uniq, probe.uniq);  // nothing else moved
            CHECK_EQ_U(got->target_stream_id, probe.target_stream_id);
        }
        CHECK(!link_report_stamp_epoch(frame, kLinkReportSize - 1, 78));
        CHECK(!link_report_stamp_epoch(nullptr, kLinkReportSize, 78));
    }

    // Pass 110: uniq is the same interval denominator as the loss fraction;
    // diversity remains the cumulative decorrelation gauge.
    CHECK_EQ_U(r3[0].uniq, 2);
    CHECK(r3[0].diversity >= 2);

    // §7.3 Pass 79: reports are emitted for RTP streams only — a latched
    // AUDIO stream must not produce one (its per-stream loss fraction would
    // otherwise steer the §9 selector).
    {
        RxEngine e2(RxPolicy{},
                    {WantSpec{0, stream_type::kRtp, kTxOrig},
                     WantSpec{1, stream_type::kAudio, kTxOrig}},
                    nullptr, std::nullopt);
        Reporter rep2(ReporterPolicy{100}, std::nullopt);
        auto feed2 = [&](uint8_t sid, uint8_t stype, uint32_t seq) {
            DataHeader hh;
            hh.prefix = {kTxOrig, 0, kTxSession};
            hh.stream_id = sid;
            hh.stream_type = stype;
            hh.seq = seq;
            hh.block_id = seq;
            hh.data_flags = data_flags::kEndOfBlock;
            const uint8_t payload = 0;
            DataView v;
            v.hdr = hh;
            v.payload = &payload;
            v.payload_len = 1;
            e2.on_data(0, v, 0,
                       [](uint8_t, uint32_t, uint8_t, const uint8_t*, size_t) {},
                       -50);
        };
        for (uint32_t q = 0; q < 3; ++q) {
            feed2(0, stream_type::kRtp, q);
            feed2(1, stream_type::kAudio, q);
        }
        auto ra = rep2.build(e2, 10);
        CHECK_EQ_U(ra.size(), 1);
        CHECK_EQ_U(ra[0].target_stream_id, 0);
    }

    return wbtest_finish("reporter_test");
}
