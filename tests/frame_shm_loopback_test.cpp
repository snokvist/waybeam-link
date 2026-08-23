// SPDX-License-Identifier: GPL-2.0-or-later
// End-to-end frame-shm path in one process, WITHOUT radios (§5.1a → §3 wire →
// §6.3a → §15.4): a producer writes whole [VencFrameMeta][Annex-B] frames into
// an ingress SHM ring; the app's TX glue reads them, runs FrameFramer to emit
// DATA packets; those packets are decode()d (block_id/flags/payload extracted
// exactly as the RX engine hands them to Deliver), passed through a synthetic
// FEC-bounded loss pattern into FrameReassembler, whose reassembled blobs are
// written to an egress SHM ring and read back byte-exact by a consumer.
//
// Proves: clean frames take the fast path, FEC-lossy frames recover byte-exact,
// and dropping beyond the repair budget yields no frame (never a corrupt one).
#include "wblink/frame_shm.h"

#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <variant>
#include <vector>

#include "wblink/frame_framer.h"
#include "wblink/frame_reassembler.h"
#include "wblink/frame_shm_format.h"
#include "wblink/rx.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

const char* kInRing = "/wblink-frame-shm-loopback-in";
const char* kOutRing = "/wblink-frame-shm-loopback-out";

// One [VencFrameMeta][Annex-B] blob with a deterministic body (§15.4 layout).
std::vector<uint8_t> make_frame(size_t body, bool idr, uint8_t seed) {
    std::vector<uint8_t> b(kVencFrameMetaSize + body, 0);
    b[4] = kFrameCodecH265;
    b[5] = idr ? kFrameFlagIdr : 0;
    for (size_t i = 0; i < body; ++i) {
        b[kVencFrameMetaSize + i] =
            static_cast<uint8_t>((i * 131u + seed * 7u) & 0xFF);
    }
    return b;
}

// A decoded DATA symbol, as the RX engine would hand it to Deliver.
struct Pkt {
    DataHeader hdr;
    uint32_t block_id;
    uint8_t flags;
    std::vector<uint8_t> payload;
};

// Push one blob through the ingress ring → FrameFramer, decode() each emitted
// DATA packet back into (block_id, flags, payload).
std::vector<Pkt> frame_to_packets(FrameShmRing& prod, FrameShmRing& cons,
                                  FrameFramer& ff,
                                  const std::vector<uint8_t>& blob, uint64_t t,
                                  std::vector<uint8_t>& rbuf) {
    CHECK(prod.write_frame(blob.data(), blob.size()));
    const long got = cons.read_frame(rbuf.data(), rbuf.size());
    CHECK_EQ_U(static_cast<unsigned long long>(got), blob.size());
    std::vector<Pkt> pkts;
    ff.on_frame(rbuf.data(), got > 0 ? static_cast<size_t>(got) : 0, t,
                [&](const uint8_t* f, size_t n, const DataHeader&, uint64_t) {
                    const Decoded dec = decode(f, n);
                    const DataView* v = std::get_if<DataView>(&dec);
                    CHECK(v != nullptr);
                    if (v == nullptr) {
                        return;
                    }
                    Pkt p;
                    p.hdr = v->hdr;
                    p.block_id = v->hdr.block_id;
                    p.flags = v->hdr.data_flags;
                    p.payload.assign(v->payload, v->payload + v->payload_len);
                    pkts.push_back(std::move(p));
                });
    return pkts;
}

size_t count_repairs(const std::vector<Pkt>& pkts) {
    size_t n = 0;
    for (const Pkt& p : pkts) {
        n += (p.flags & data_flags::kFecRepair) != 0;
    }
    return n;
}

// Feed packets to the reassembler, dropping the first `drop_sources` SOURCE
// symbols (repairs always kept). Reassembled blobs land on the egress ring.
void deliver_lossy(FrameReassembler& ra, FrameShmRing& out_prod,
                   const std::vector<Pkt>& pkts, size_t drop_sources,
                   uint64_t t) {
    size_t dropped = 0;
    const auto emit = [&](const uint8_t* f, size_t n) {
        return out_prod.write_frame(f, n);
    };
    for (const Pkt& p : pkts) {
        const bool is_rep = (p.flags & data_flags::kFecRepair) != 0;
        if (!is_rep && dropped < drop_sources) {
            ++dropped;
            continue;
        }
        ra.push(p.block_id, p.flags, p.payload.data(), p.payload.size(), t,
                emit);
    }
}

}  // namespace

int main() {
    for (const char* n : {kInRing, kOutRing}) {
        ::shm_unlink(n);
    }

    auto in_prod = FrameShmRing::create(kInRing);
    auto in_cons = FrameShmRing::attach(kInRing);
    auto out_prod = FrameShmRing::create(kOutRing);
    auto out_cons = FrameShmRing::attach(kOutRing);
    CHECK(bool(in_prod));
    CHECK(bool(in_cons));
    CHECK(bool(out_prod));
    CHECK(bool(out_cons));
    if (!in_prod || !in_cons || !out_prod || !out_cons) {
        return wbtest_finish("frame_shm_loopback_test");
    }
    FrameShmRing& ip = **in_prod.value;
    FrameShmRing& ic = **in_cons.value;
    FrameShmRing& op = **out_prod.value;
    FrameShmRing& oc = **out_cons.value;

    std::vector<uint8_t> rbuf(kFrameRingDefaultSlotSize);
    std::vector<uint8_t> obuf(kFrameRingDefaultSlotSize);

    // --- ordered stream: IDR clean, P lossy, P clean, IDR lossy -------------
    {
        FrameFramerConfig fc;
        fc.stream_type = stream_type::kRtp;
        fc.fec.scheme = FecScheme::kRlc256;
        fc.fec.i_rate_permille = 300;
        fc.fec.p_rate_permille = 150;
        fc.fec.min_k = 3;
        FrameFramer ff(fc);
        ff.set_operating_point(0, 0, kDefaultMaxPayload);

        FrameReassemblerConfig rc;
        rc.deadline_ms = 50;
        FrameReassembler ra(rc);

        struct Case {
            size_t body;
            bool idr;
            bool lossy;
        };
        const Case cases[] = {
            {9000, true, false}, {9000, false, true},
            {9000, false, false}, {9000, true, true}};
        size_t expect_fast = 0;
        size_t expect_fec = 0;
        uint64_t t = 1000;
        for (size_t i = 0; i < 4; ++i) {
            const Case& cs = cases[i];
            auto blob = make_frame(cs.body, cs.idr, static_cast<uint8_t>(i + 1));
            auto pkts = frame_to_packets(ip, ic, ff, blob, t, rbuf);
            const size_t r = count_repairs(pkts);
            CHECK(r >= 1);  // FEC actually engaged (k > min_k)
            // Drop exactly r sources on the lossy frames — tight MDS recovery.
            const size_t drop = cs.lossy ? r : 0;
            deliver_lossy(ra, op, pkts, drop, t);
            if (cs.lossy) {
                ++expect_fec;
            } else {
                ++expect_fast;
            }
            // The egress ring must now hold exactly this frame, byte-exact.
            const long got = oc.read_frame(obuf.data(), obuf.size());
            CHECK_EQ_U(static_cast<unsigned long long>(got), blob.size());
            CHECK(got > 0 &&
                  std::memcmp(obuf.data(), blob.data(), blob.size()) == 0);
            CHECK_EQ_U(oc.read_frame(obuf.data(), obuf.size()), 0);  // just one
            ++t;
        }
        CHECK_EQ_U(ra.stats().frames_delivered, 4u);
        CHECK_EQ_U(ra.stats().frames_fast, expect_fast);
        CHECK_EQ_U(ra.stats().frames_fec, expect_fec);
        CHECK(expect_fec == 2 && expect_fast == 2);
    }

    // --- merged RX: repair-tail FEC retires the packet gap before ARQ ------
    {
        FrameFramerConfig fc;
        fc.originator = 17;
        fc.session_id = 0x01020304;
        fc.stream_type = stream_type::kRtp;
        fc.fec.scheme = FecScheme::kRlc256;
        fc.fec.i_rate_permille = 500;
        fc.fec.p_rate_permille = 500;
        fc.fec.min_k = 3;
        FrameFramer ff(fc);
        ff.set_operating_point(0, 0, kDefaultMaxPayload);

        FrameReassemblerConfig rc;
        rc.deadline_ms = 50;
        FrameReassembler ra(rc);

        RxPolicy policy;
        policy.admit_n = 1;
        RxEngine rx(policy,
                    {WantSpec{0, stream_type::kRtp, fc.originator}}, nullptr,
                    std::nullopt);

        auto blob = make_frame(9000, /*idr=*/true, 77);
        auto pkts = frame_to_packets(ip, ic, ff, blob, 1500, rbuf);
        CHECK(count_repairs(pkts) >= 1);

        size_t sources = 0;
        size_t ordered_deliveries = 0;
        const RxEngine::Deliver ordered =
            [&](uint8_t, uint32_t, uint8_t, const uint8_t*, size_t) {
                ++ordered_deliveries;
            };
        const RxEngine::EarlyDeliver early =
            [&](const StreamKey&, uint8_t, uint32_t block_id, uint8_t flags,
                const uint8_t* data, size_t len) {
                const bool complete = ra.push(
                    block_id, flags, data, len, 1500,
                    [&](const uint8_t* f, size_t n) {
                        return op.write_frame(f, n);
                    });
                return RxEngine::EarlyDeliverResult{true, complete};
            };

        uint64_t now = 1500;
        for (const Pkt& p : pkts) {
            const bool repair =
                (p.flags & data_flags::kFecRepair) != 0;
            if (!repair && sources++ == 1) {
                continue;  // one real packet-sequence gap inside this block
            }
            DataView view;
            view.hdr = p.hdr;
            view.payload = p.payload.data();
            view.payload_len = static_cast<uint16_t>(p.payload.size());
            rx.on_data(0, view, now, ordered, 0, early);
            rx.on_data(1, view, now, ordered, 0, early);  // diversity copy
            ++now;
        }

        const long got = oc.read_frame(obuf.data(), obuf.size());
        CHECK_EQ_U(static_cast<unsigned long long>(got), blob.size());
        CHECK(got > 0 &&
              std::memcmp(obuf.data(), blob.data(), blob.size()) == 0);
        CHECK_EQ_U(ordered_deliveries, 0u);  // no second delivery on seq drain
        CHECK_EQ_U(rx.build_nacks(now).size(), 0u);
        const auto streams = rx.streams();
        CHECK_EQ_U(streams.size(), 1u);
        CHECK(streams.size() == 1 &&
              streams[0].counters.lost_declared >= 1u);
        CHECK(streams.size() == 1 && streams[0].counters.diversity >= 1u);
        CHECK_EQ_U(ra.stats().frames_fec, 1u);
    }

    // --- beyond the repair budget: drop r+1 sources → nothing emitted -------
    {
        FrameFramerConfig fc;
        fc.stream_type = stream_type::kRtp;
        fc.fec.scheme = FecScheme::kRlc256;
        fc.fec.i_rate_permille = 200;
        fc.fec.p_rate_permille = 100;
        fc.fec.min_k = 3;
        FrameFramer ff(fc);
        ff.set_operating_point(0, 0, kDefaultMaxPayload);

        FrameReassemblerConfig rc;
        rc.deadline_ms = 50;
        FrameReassembler ra(rc);

        auto blob = make_frame(9000, /*idr=*/true, 42);
        auto pkts = frame_to_packets(ip, ic, ff, blob, 2000, rbuf);
        const size_t r = count_repairs(pkts);
        CHECK(r >= 1);
        deliver_lossy(ra, op, pkts, r + 1, 2000);  // one past recoverable
        CHECK_EQ_U(oc.read_frame(obuf.data(), obuf.size()), 0);  // no frame out
        // Deadline expiry finalizes the block as unrecoverable — still no blob.
        ra.tick(2000 + rc.deadline_ms + 1,
                [&](const uint8_t* f, size_t n) {
                    return op.write_frame(f, n);
                });
        CHECK_EQ_U(oc.read_frame(obuf.data(), obuf.size()), 0);
        CHECK_EQ_U(ra.stats().frames_delivered, 0u);
        CHECK(ra.stats().frames_deadline >= 1u);
    }

    // --- a full egress ring rejects without claiming delivery ------------
    {
        const auto filler = make_frame(64, /*idr=*/false, 90);
        for (uint32_t i = 0; i < kFrameRingDefaultSlots; ++i) {
            CHECK(op.write_frame(filler.data(), filler.size()));
        }
        const uint64_t drops_before = op.stats().full_drops;

        FrameFramerConfig fc;
        fc.stream_type = stream_type::kRtp;
        fc.fec.scheme = FecScheme::kNone;
        FrameFramer ff(fc);
        ff.set_operating_point(0, 0, kDefaultMaxPayload);
        FrameReassemblerConfig rc;
        FrameReassembler ra(rc);
        const auto rejected = make_frame(3000, /*idr=*/true, 91);
        const auto pkts = frame_to_packets(ip, ic, ff, rejected, 3000, rbuf);
        bool completed = false;
        for (const Pkt& p : pkts) {
            completed |= ra.push(
                p.block_id, p.flags, p.payload.data(), p.payload.size(), 3000,
                [&](const uint8_t* f, size_t n) {
                    return op.write_frame(f, n);
                });
        }
        CHECK(completed);
        CHECK_EQ_U(ra.stats().frames_delivered, 0u);
        CHECK_EQ_U(ra.stats().frames_fast, 0u);
        CHECK_EQ_U(ra.stats().frames_egress_rejected, 1u);
        CHECK_EQ_U(op.stats().full_drops, drops_before + 1);

        // Only the fillers committed. The refused blob never became a slot.
        for (uint32_t i = 0; i < kFrameRingDefaultSlots; ++i) {
            const long got = oc.read_frame(obuf.data(), obuf.size());
            CHECK_EQ_U(static_cast<unsigned long long>(got), filler.size());
            CHECK(got > 0 &&
                  std::memcmp(obuf.data(), filler.data(), filler.size()) == 0);
        }
        CHECK_EQ_U(oc.read_frame(obuf.data(), obuf.size()), 0);
    }

    // Rings' destructors join reader threads + unlink; reaching finish without
    // hanging (under the dev preset's ASan) proves clean teardown.
    return wbtest_finish("frame_shm_loopback_test");
}
