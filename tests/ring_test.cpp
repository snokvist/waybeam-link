// SPDX-License-Identifier: GPL-2.0-or-later
// §5.2 resend ring: age-window eviction, byte-budget eviction (oldest first),
// seq lookup, attempt accounting, and the RETRANSMIT flip on a copy leaving
// the stored frame pristine.
#include "wblink/ring.h"

#include <cstring>

#include "wbtest.h"

using namespace wblink;

namespace {

DataHeader make_hdr(uint32_t seq, uint32_t block, uint8_t flags) {
    DataHeader h;
    h.prefix = {17, 0, 1};
    h.seq = seq;
    h.block_id = block;
    h.data_flags = flags;
    return h;
}

void push_frame(ResendRing& ring, uint32_t seq, uint64_t now,
                size_t payload = 64, uint8_t flags = data_flags::kArq) {
    uint8_t frame[kDataHeaderSize + 1424];
    uint8_t payload_buf[1424] = {0x55};
    const DataHeader h = make_hdr(seq, seq / 4, flags);
    const size_t n = encode_data(h, payload_buf,
                                 static_cast<uint16_t>(payload), frame,
                                 sizeof(frame));
    ring.push(frame, n, h, now);
}

}  // namespace

int main() {
    // Lookup + miss.
    {
        ResendRing ring(RingConfig{50, 1 << 20});
        for (uint32_t s = 10; s < 20; ++s) {
            push_frame(ring, s, 100);
        }
        CHECK_EQ_U(ring.size(), 10);
        CHECK(ring.find(9) == nullptr);
        CHECK(ring.find(20) == nullptr);
        RingEntry* e = ring.find(15);
        CHECK(e != nullptr);
        if (e != nullptr) {
            CHECK_EQ_U(e->seq, 15);
            CHECK_EQ_U(e->block_id, 15 / 4);
            CHECK_EQ_U(e->attempts, 0);
            e->attempts++;
            CHECK_EQ_U(ring.find(15)->attempts, 1);
        }
    }

    // Age eviction.
    {
        ResendRing ring(RingConfig{50, 1 << 20});
        push_frame(ring, 0, 100);
        push_frame(ring, 1, 130);
        push_frame(ring, 2, 160);
        ring.evict(165);  // 0 is 65ms old -> out; 1 is 35ms -> stays
        CHECK(ring.find(0) == nullptr);
        CHECK(ring.find(1) != nullptr);
        CHECK(ring.find(2) != nullptr);
        CHECK_EQ_U(ring.oldest_seq(), 1);
    }

    // Byte-budget eviction: oldest evicted first, freshest survive.
    {
        // Each frame = 26 + 64 = 90 bytes; budget fits 3.
        ResendRing ring(RingConfig{1000, 3 * 90});
        for (uint32_t s = 0; s < 5; ++s) {
            push_frame(ring, s, 100);
        }
        CHECK_EQ_U(ring.size(), 3);
        CHECK(ring.find(0) == nullptr);
        CHECK(ring.find(1) == nullptr);
        CHECK(ring.find(2) != nullptr);
        CHECK(ring.find(4) != nullptr);
        CHECK(ring.bytes() <= 3 * 90);
    }

    // RETRANSMIT flip happens on the copy; the stored frame stays pristine.
    {
        ResendRing ring(RingConfig{50, 1 << 20});
        push_frame(ring, 7, 100);
        RingEntry* e = ring.find(7);
        CHECK(e != nullptr);
        if (e != nullptr) {
            std::vector<uint8_t> copy = e->frame;
            ResendRing::mark_retransmit(copy.data(), copy.size());
            CHECK((copy[21] & data_flags::kRetransmit) != 0);
            CHECK((e->frame[21] & data_flags::kRetransmit) == 0);
            // The copy still decodes and shows RETRANSMIT.
            const Decoded d = decode(copy.data(), copy.size());
            const DataView* v = std::get_if<DataView>(&d);
            CHECK(v != nullptr &&
                  (v->hdr.data_flags & data_flags::kRetransmit) != 0);
        }
    }

    return wbtest_finish("ring_test");
}
