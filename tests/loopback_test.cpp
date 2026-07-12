// SPDX-License-Identifier: GPL-2.0-or-later
// §16.2 bench assertions, fully deterministic (fake time, seeded loss):
//   (a) independent per-adapter loss  => ~zero delivered gaps from diversity
//       alone, few/no NACKs;
//   (b) Gilbert-Elliott burst on ARQ blocks => recovered-by-NACK > 0 and
//       delivered loss ~ 0;
//   (c) loss on non-ARQ blocks => zero NACKs for those seqs;
//   (d) correlated all-adapter fade => ARQ bounded by its caps (no
//       self-congestion collapse), stream recovers after the fade.
//
// The harness wires the real production pieces — Framer -> AdapterLossField
// -> RxEngine -> (NACK) -> ResendScheduler -> loss -> RxEngine — exactly as
// app loopback mode does, but with injected time.
#include <cstring>
#include <optional>
#include <variant>
#include <vector>

#include "wblink/endian.h"
#include "wblink/framer.h"
#include "wblink/loss_model.h"
#include "wblink/ring.h"
#include "wblink/rtp.h"
#include "wblink/rx.h"
#include "wblink/scheduler.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

struct Bench {
    Framer framer;
    ResendRing ring;
    ResendScheduler sched;
    RxEngine rx;
    AdapterLossField field;
    LossRng return_rng;
    double return_loss_p;

    uint64_t now = 1000;
    uint64_t delivered = 0;
    uint64_t delivered_gaps = 0;  // discontinuities in the delivered stream
    std::optional<uint32_t> last_marker;  // per-datagram marker payload

    Bench(uint8_t adapters, uint64_t seed, double correlation,
          double uniform_p, std::optional<GeParams> ge,
          double return_loss = 0.0, uint32_t arq_threshold = 300)
        : framer(FramerConfig{.originator = 17,
                              .session_id = 0xC0FFEE,
                              .stream_id = 0,
                              .stream_type = stream_type::kRtp,
                              .destination = 0,
                              .classifier = RtpClassifier::kSize,
                              .classifier_size_threshold = arq_threshold}),
          ring(RingConfig{200, 1 << 20}),
          sched(make_sched_policy(), nullptr),
          rx(make_rx_policy(), {WantSpec{0, stream_type::kRtp, 17}}, nullptr,
             std::nullopt),
          field(adapters, seed, correlation, uniform_p, ge),
          return_loss_p(return_loss) {
        return_rng.s = seed ^ 0xFEEDFACEull;
        framer.set_operating_point(0, 0);
    }

    static SchedulerPolicy make_sched_policy() {
        SchedulerPolicy p;
        p.holddown_ms = 10;
        p.attempt_cap = 3;
        p.preferred_originator = 9;
        p.budget_floor_bytes = 64 * 1024;  // ample for the bench
        return p;
    }
    static RxPolicy make_rx_policy() {
        RxPolicy p;
        p.dwell_ceiling_ms = 15;
        p.renack_attempts = 5;
        p.renack_backoff_ms = 10;
        p.default_deadline_iframe_ms = 120;
        p.default_deadline_pframe_ms = 60;
        p.idle_teardown_ms = 60000;
        p.clamp_resync_ms = 100;  // fast outage recovery for the bench
        return p;
    }

    RxEngine::Deliver deliver() {
        return [this](uint8_t, uint32_t, uint8_t, const uint8_t* d, size_t n) {
            ++delivered;
            // Delivered datagram = RTP header + payload; the marker counter
            // sits right after the 12-byte fixed header.
            if (n >= kRtpFixedHeaderSize + 4) {
                const uint32_t v = be32_read(d + kRtpFixedHeaderSize);
                if (last_marker && v != *last_marker + 1) {
                    ++delivered_gaps;
                }
                last_marker = v;
            }
        };
    }

    void air_to_rx(const uint8_t* f, size_t n) {
        // on_data copies the payload synchronously, so the view's lifetime
        // is just this call.
        const Decoded dec = decode(f, n);
        const DataView* v = std::get_if<DataView>(&dec);
        if (v == nullptr) {
            return;
        }
        field.begin_packet();
        for (uint8_t a = 0; a < field.adapters(); ++a) {
            if (!field.drop(a)) {
                rx.on_data(a, *v, now, deliver());
            }
        }
    }

    // Feed one RTP datagram (frame of `pkts` packets => marker on the last).
    void send_frame(uint32_t frame_no, unsigned pkts, size_t payload_bytes) {
        for (unsigned i = 0; i < pkts; ++i) {
            std::vector<uint8_t> d(kRtpFixedHeaderSize + payload_bytes, 0);
            d[0] = 0x80;
            d[1] = static_cast<uint8_t>(i + 1 == pkts ? 0x80 | 96 : 96);
            be16_write(d.data() + 2, static_cast<uint16_t>(frame_no));
            be32_write(d.data() + 4, frame_no * 3000);  // timestamp
            be32_write(d.data() + 8, 0xCAFEBABE);
            // Payload marker: global packet counter.
            be32_write(d.data() + kRtpFixedHeaderSize, marker_counter_++);
            framer.on_datagram(
                d.data(), d.size(), now,
                [&](const uint8_t* frame, size_t len, const DataHeader& hdr,
                    uint64_t t) {
                    ring.push(frame, len, hdr, t);
                    sched.note_live_bytes(len);
                    air_to_rx(frame, len);
                });
        }
    }

    void tick_ms(uint64_t ms) {
        for (uint64_t i = 0; i < ms; ++i) {
            ++now;
            rx.tick(now, deliver());
            // NACK return path (with its own loss), then resend service.
            for (const NackRequest& req : rx.build_nacks(now)) {
                if (return_rng.uniform() < return_loss_p) {
                    continue;
                }
                NackHeader hdr;
                hdr.prefix = {9, 17, 0xBEEF};
                hdr.target_originator = req.target_originator;
                hdr.target_session = req.target_session;
                hdr.target_stream_id = req.target_stream_id;
                hdr.base_seq = req.base_seq;
                uint8_t frame[kNackFixedSize + 255];
                const size_t n = encode_nack(
                    hdr, req.bitmap.data(),
                    static_cast<uint8_t>(req.bitmap.size()), frame,
                    sizeof(frame));
                const Decoded dec = decode(frame, n);
                if (const NackView* nv = std::get_if<NackView>(&dec)) {
                    sched.on_nack(*nv, ring, now);
                }
            }
            ring.evict(now);
            sched.drain(ring, now, [&](const uint8_t* f, size_t l) {
                air_to_rx(f, l);
            });
        }
    }

    RxStreamCounters rx_counters() {
        auto ss = rx.streams();
        return ss.empty() ? RxStreamCounters{} : ss[0].counters;
    }

    void print(const char* label) {
        const RxStreamCounters c = rx_counters();
        std::fprintf(stderr,
                     "%s: uniq=%llu div=%llu delivered=%llu gaps=%llu "
                     "lost=%llu rec_arq=%llu sup=%llu ddl=%llu unrec=%llu "
                     "nacks=%llu resends=%llu clamp=%llu\n",
                     label, (unsigned long long)c.uniq,
                     (unsigned long long)c.diversity,
                     (unsigned long long)delivered,
                     (unsigned long long)delivered_gaps,
                     (unsigned long long)c.lost_declared,
                     (unsigned long long)c.recovered_arq,
                     (unsigned long long)c.dropped_superseded,
                     (unsigned long long)c.dropped_deadline,
                     (unsigned long long)c.dropped_unrecoverable,
                     (unsigned long long)c.nacks_sent,
                     (unsigned long long)sched.counters().resends_sent,
                     (unsigned long long)c.clamp_rejected);
    }

    uint32_t marker_counter_ = 0;
};

// Run: `frames` video frames, `pkts` packets each, 33 ms apart w/ ticking.
void run_stream(Bench& b, uint32_t frames, unsigned pkts,
                size_t payload = 600) {
    for (uint32_t f = 0; f < frames; ++f) {
        b.send_frame(f, pkts, payload);
        b.tick_ms(8);
    }
    b.tick_ms(200);  // drain the tail
}

}  // namespace

int main() {
    // (a) independent per-adapter loss: diversity alone carries it.
    {
        Bench b(/*adapters=*/3, /*seed=*/42, /*corr=*/0.0,
                /*uniform_p=*/0.10, std::nullopt);
        run_stream(b, 300, 6);
        b.print("(a) independent");
        const RxStreamCounters c = b.rx_counters();
        // P(all 3 drop) = 0.1% — diversity delivers essentially everything.
        CHECK(c.uniq > 1700);
        CHECK(c.lost_declared <= 5);
        CHECK(c.nacks_sent <= 5);
        CHECK(b.delivered_gaps <= 5);
        CHECK(c.diversity > 2000);  // duplicates prove multi-adapter merge
    }

    // (b) correlated GE burst on ARQ blocks: NACK/resend recovers them.
    {
        // Big frames (>300 B cumulative => ARQ per the size stub); bursty
        // correlated loss so diversity alone cannot carry it.
        Bench b(/*adapters=*/2, /*seed=*/7, /*corr=*/1.0,
                /*uniform_p=*/0.0,
                GeParams{0.02, 0.25, 0.0, 0.9},
                /*return_loss=*/0.0, /*arq_threshold=*/300);
        run_stream(b, 400, 4);
        b.print("(b) ge-burst");
        const RxStreamCounters c = b.rx_counters();
        CHECK(c.lost_declared > 20);       // the bursts really bit
        CHECK(c.recovered_arq > 10);       // and ARQ pulled frames back
        // Delivered loss after recovery stays small relative to declared.
        CHECK(c.recovered_arq * 2 > c.lost_declared);
    }

    // (c) non-ARQ loss => zero NACKs (importance gate, both sides).
    {
        // Tiny frames stay under the ARQ threshold: nothing is eligible.
        Bench b(/*adapters=*/1, /*seed=*/3, /*corr=*/0.0,
                /*uniform_p=*/0.15, std::nullopt,
                /*return_loss=*/0.0, /*arq_threshold=*/100000);
        run_stream(b, 200, 3, /*payload=*/64);
        b.print("(c) non-arq");
        const RxStreamCounters c = b.rx_counters();
        CHECK(c.lost_declared > 10);  // losses happened
        CHECK_EQ_U(c.nacks_sent, 0);  // but nothing was NACKed
        CHECK_EQ_U(c.recovered_arq, 0);
        CHECK_EQ_U(b.sched.counters().resends_sent, 0);
    }

    // (d) deep correlated fade: ARQ stays bounded, stream recovers after.
    {
        Bench b(/*adapters=*/2, /*seed=*/11, /*corr=*/1.0,
                /*uniform_p=*/0.0, std::nullopt,
                /*return_loss=*/0.0, /*arq_threshold=*/300);
        // Clean phase.
        run_stream(b, 100, 4);
        const uint64_t delivered_clean = b.delivered;
        // Fade: 95% correlated loss for a stretch (swap the loss field).
        b.field = AdapterLossField(2, 99, 1.0, 0.95, std::nullopt);
        run_stream(b, 150, 4);
        // ARQ bounded: resends cannot exceed attempt_cap x declared losses.
        const RxStreamCounters c1 = b.rx_counters();
        CHECK(b.sched.counters().resends_sent <= 3 * c1.lost_declared + 10);
        // Recovery phase: clean air again.
        b.field = AdapterLossField(2, 123, 0.0, 0.0, std::nullopt);
        const uint64_t delivered_before = b.delivered;
        run_stream(b, 100, 4);
        const uint64_t delivered_after = b.delivered - delivered_before;
        b.print("(d) fade-recover");
        CHECK(delivered_clean > 350);  // sanity on the clean phase
        // The fade outran the §6.6 clamp; the sustained-clamp resync
        // re-floors the stream and recovery completes within one
        // clamp_resync_ms window (100 ms ≈ 50 packets at this rate).
        CHECK(b.rx_counters().resyncs >= 1);
        CHECK(delivered_after >= 300);
    }

    return wbtest_finish("loopback_test");
}
