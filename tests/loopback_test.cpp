// SPDX-License-Identifier: GPL-2.0-or-later
// §16.2 bench assertions, deterministic (fake time, seeded loss). Pass 205
// removed ARQ, so this exercises the surviving path: Framer -> AdapterLossField
// -> RxEngine -> delivery, with diversity as the primary redundancy.
//   (a) independent per-adapter loss => diversity delivers with ~no gaps;
//   (b) correlated burst => losses are declared and the stream survives;
//   (c) rising loss => declared loss reacts monotonically.
#include <cstdio>
#include <optional>
#include <vector>

#include "wblink/endian.h"
#include "wblink/framer.h"
#include "wblink/loss_model.h"
#include "wblink/rtp.h"
#include "wblink/rx.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

struct Bench {
    Framer framer;
    RxEngine rx;
    AdapterLossField field;
    uint64_t now = 1000;
    uint64_t delivered = 0;
    uint64_t delivered_gaps = 0;  // discontinuities in the delivered stream
    std::optional<uint32_t> last_marker;

    Bench(uint8_t adapters, uint64_t seed, double correlation, double uniform_p,
          std::optional<GeParams> ge)
        : framer(FramerConfig{.originator = 17,
                              .session_id = 0xC0FFEE,
                              .stream_id = 0,
                              .stream_type = stream_type::kRtp,
                              .destination = 0}),
          rx(make_rx_policy(), {WantSpec{0, stream_type::kRtp, 17}}, nullptr,
             std::nullopt),
          field(adapters, seed, correlation, uniform_p, ge) {
        framer.set_operating_point(0, 0);
    }

    static RxPolicy make_rx_policy() {
        RxPolicy p;
        p.dwell_ceiling_ms = 15;
        p.default_deadline_iframe_ms = 120;
        p.idle_teardown_ms = 60000;
        p.clamp_resync_ms = 100;
        return p;
    }

    RxEngine::Deliver deliver() {
        return [this](uint8_t, uint32_t, uint8_t, const uint8_t* d, size_t n) {
            ++delivered;
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

    // Feed one RTP frame of `pkts` packets (marker on the last).
    void send_frame(uint32_t frame_no, unsigned pkts, size_t payload_bytes) {
        for (unsigned i = 0; i < pkts; ++i) {
            std::vector<uint8_t> d(kRtpFixedHeaderSize + payload_bytes, 0);
            d[0] = 0x80;
            d[1] = static_cast<uint8_t>(i + 1 == pkts ? 0x80 | 96 : 96);
            be16_write(d.data() + 2, static_cast<uint16_t>(frame_no));
            be32_write(d.data() + 4, frame_no * 3000);
            be32_write(d.data() + 8, 0xCAFEBABE);
            be32_write(d.data() + kRtpFixedHeaderSize, marker_counter_++);
            framer.on_datagram(
                d.data(), d.size(), now,
                [&](const uint8_t* frame, size_t len, const DataHeader&,
                    uint64_t) { air_to_rx(frame, len); });
        }
    }

    void tick_ms(uint64_t ms) {
        for (uint64_t i = 0; i < ms; ++i) {
            ++now;
            rx.tick(now, deliver());
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
                     "lost=%llu sup=%llu ddl=%llu unrec=%llu clamp=%llu\n",
                     label, (unsigned long long)c.uniq,
                     (unsigned long long)c.diversity,
                     (unsigned long long)delivered,
                     (unsigned long long)delivered_gaps,
                     (unsigned long long)c.lost_declared,
                     (unsigned long long)c.dropped_superseded,
                     (unsigned long long)c.dropped_deadline,
                     (unsigned long long)c.dropped_unrecoverable,
                     (unsigned long long)c.clamp_rejected);
    }

    uint32_t marker_counter_ = 0;
};

void run_stream(Bench& b, uint32_t frames, unsigned pkts,
                size_t payload = 600) {
    for (uint32_t f = 0; f < frames; ++f) {
        b.send_frame(f, pkts, payload);
        b.tick_ms(8);
    }
    b.tick_ms(200);
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
        CHECK(c.uniq > 1700);
        CHECK(c.lost_declared <= 5);
        CHECK(b.delivered_gaps <= 5);
        CHECK(c.diversity > 2000);  // duplicates prove multi-adapter merge
    }

    // (b) correlated burst: diversity cannot carry it, losses are declared,
    //     the stream keeps running and recovers.
    {
        Bench b(/*adapters=*/2, /*seed=*/7, /*corr=*/1.0,
                /*uniform_p=*/0.0, GeParams{0.02, 0.25, 0.0, 0.9});
        run_stream(b, 400, 4);
        b.print("(b) ge-burst");
        const RxStreamCounters c = b.rx_counters();
        CHECK(c.lost_declared > 20);  // the bursts really bit
        CHECK(c.dropped_unrecoverable + c.dropped_superseded +
                  c.dropped_deadline >
              0);
        // The stream survived: it kept delivering past the burst.
        CHECK(b.delivered > 300);
    }

    // (c) increasing loss: declared loss reacts monotonically.
    {
        Bench b(/*adapters=*/1, /*seed=*/29, /*corr=*/0.0,
                /*uniform_p=*/0.01, std::nullopt);
        uint64_t prev_lost = 0;
        uint64_t previous_delta = 0;
        const double levels[] = {0.01, 0.05, 0.15, 0.30};
        for (size_t i = 0; i < 4; ++i) {
            b.field = AdapterLossField(1, 100 + i, 0.0, levels[i], std::nullopt);
            run_stream(b, 400, 4);
            const uint64_t lost = b.rx_counters().lost_declared;
            const uint64_t delta = lost - prev_lost;
            CHECK(delta > previous_delta);
            previous_delta = delta;
            prev_lost = lost;
        }
        b.print("(c) incremental");
    }

    return wbtest_finish("loopback_test");
}
