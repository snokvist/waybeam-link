// SPDX-License-Identifier: GPL-2.0-or-later
// §5.1 framer: RTP block boundaries (marker + timestamp-change fallback),
// EOB placement, per-datagram non-RTP blocks, seq/block monotonicity,
// oversize drop, operating-point stamping. Pass 205 retired ARQ; the RTP
// NAL/size classifier went with it (O2).
#include "wblink/framer.h"

#include <cstring>
#include <vector>

#include "wblink/endian.h"
#include "wblink/rtp.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

struct Sent {
    DataHeader hdr;
    std::vector<uint8_t> frame;
};

std::vector<uint8_t> rtp_datagram(uint32_t timestamp, bool marker,
                                  size_t payload_bytes) {
    std::vector<uint8_t> d(kRtpFixedHeaderSize + payload_bytes, 0xAB);
    d[0] = 0x80;  // version 2
    d[1] = static_cast<uint8_t>(marker ? 0x80 | 96 : 96);
    be16_write(d.data() + 2, 0x1234);
    be32_write(d.data() + 4, timestamp);
    be32_write(d.data() + 8, 0xCAFEBABE);
    return d;
}

struct Harness {
    Framer framer;
    std::vector<Sent> sent;
    explicit Harness(uint8_t stream_type_v)
        : framer(FramerConfig{.originator = 17,
                              .session_id = 0x01020304,
                              .stream_id = 0,
                              .stream_type = stream_type_v,
                              .destination = 0}) {
        framer.set_operating_point(4, 0x2B);
    }
    bool feed(const std::vector<uint8_t>& d, uint64_t now = 0) {
        return framer.on_datagram(
            d.data(), d.size(), now,
            [&](const uint8_t* f, size_t n, const DataHeader& h, uint64_t) {
                sent.push_back(Sent{h, std::vector<uint8_t>(f, f + n)});
            });
    }
};

}  // namespace

int main() {
    // --- RTP: marker delimits blocks ----------------------------------------
    {
        Harness h(stream_type::kRtp);
        // Frame A: 3 packets, marker on the last. Frame B: 1 packet w/ marker.
        h.feed(rtp_datagram(1000, false, 100));
        h.feed(rtp_datagram(1000, false, 100));
        h.feed(rtp_datagram(1000, true, 100));
        h.feed(rtp_datagram(2000, true, 100));
        CHECK_EQ_U(h.sent.size(), 4);
        // seq strictly monotonic from 0.
        for (size_t i = 0; i < h.sent.size(); ++i) {
            CHECK_EQ_U(h.sent[i].hdr.seq, i);
        }
        // Frame A = block 0, frame B = block 1.
        CHECK_EQ_U(h.sent[0].hdr.block_id, 0);
        CHECK_EQ_U(h.sent[1].hdr.block_id, 0);
        CHECK_EQ_U(h.sent[2].hdr.block_id, 0);
        CHECK_EQ_U(h.sent[3].hdr.block_id, 1);
        // EOB only on marker packets.
        CHECK((h.sent[0].hdr.data_flags & data_flags::kEndOfBlock) == 0);
        CHECK((h.sent[1].hdr.data_flags & data_flags::kEndOfBlock) == 0);
        CHECK((h.sent[2].hdr.data_flags & data_flags::kEndOfBlock) != 0);
        CHECK((h.sent[3].hdr.data_flags & data_flags::kEndOfBlock) != 0);
        // Operating point stamped on every packet (§3.2 redundancy rule).
        for (const Sent& s : h.sent) {
            CHECK_EQ_U(s.hdr.active_profile, 4);
            CHECK_EQ_U(s.hdr.table_version, 0x2B);
            CHECK_EQ_U(s.hdr.stream_type, stream_type::kRtp);
        }
        // Emitted frames decode back to the same header.
        const Decoded d = decode(h.sent[2].frame.data(), h.sent[2].frame.size());
        const DataView* v = std::get_if<DataView>(&d);
        CHECK(v != nullptr && v->hdr == h.sent[2].hdr);
    }

    // --- RTP: timestamp change without marker also starts a new block -------
    {
        Harness h(stream_type::kRtp);
        h.feed(rtp_datagram(1000, false, 100));
        h.feed(rtp_datagram(1000, false, 100));
        h.feed(rtp_datagram(2000, false, 100));  // ts moved, no marker seen
        CHECK_EQ_U(h.sent[0].hdr.block_id, 0);
        CHECK_EQ_U(h.sent[1].hdr.block_id, 0);
        CHECK_EQ_U(h.sent[2].hdr.block_id, 1);
        // The orphaned block 0 never got EOB — accepted (supersession keys
        // off block_id, §6.2-2).
        CHECK((h.sent[1].hdr.data_flags & data_flags::kEndOfBlock) == 0);
    }

    // --- RTP: unparseable payload closes defensively ------------------------
    {
        Harness h(stream_type::kRtp);
        h.feed(rtp_datagram(1000, false, 100));
        std::vector<uint8_t> junk(40, 0x00);  // version 0 -> parse fails
        h.feed(junk);
        h.feed(rtp_datagram(1000, false, 100));
        CHECK_EQ_U(h.sent[1].hdr.block_id, 0);  // junk rides the open block
        CHECK((h.sent[1].hdr.data_flags & data_flags::kEndOfBlock) != 0);
        CHECK_EQ_U(h.sent[2].hdr.block_id, 1);  // and forces a boundary after
    }

    // --- non-RTP: one datagram = one block, EOB -----------------------------
    {
        Harness h(stream_type::kTelemetry);
        h.feed(std::vector<uint8_t>(64, 0x11));
        h.feed(std::vector<uint8_t>(64, 0x22));
        CHECK_EQ_U(h.sent.size(), 2);
        CHECK_EQ_U(h.sent[0].hdr.block_id, 0);
        CHECK_EQ_U(h.sent[1].hdr.block_id, 1);
        for (const Sent& s : h.sent) {
            CHECK((s.hdr.data_flags & data_flags::kEndOfBlock) != 0);
            CHECK((s.hdr.data_flags & 0x22) == 0);  // no ARQ bits, ever
        }
    }

    // --- oversize ingress: dropped with a stat, never truncated -------------
    {
        Harness h(stream_type::kRtp);
        std::vector<uint8_t> big(kMaxDataPayload + 1, 0xEE);
        CHECK(!h.feed(big));
        CHECK_EQ_U(h.sent.size(), 0);
        CHECK_EQ_U(h.framer.stats().oversize_ingress, 1);
        // Exactly at the budget is fine.
        std::vector<uint8_t> fit(kMaxDataPayload, 0xEE);
        CHECK(h.feed(fit));
        CHECK_EQ_U(h.sent.size(), 1);
    }

    return wbtest_finish("framer_test");
}
