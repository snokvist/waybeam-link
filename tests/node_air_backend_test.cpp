// SPDX-License-Identifier: GPL-2.0-or-later
// AirBackend and the pieces that moved with it (#109 Phase 2a).
//
// One #include, link wblink::node, no app/main.cpp. AirBackend is the first
// target of the phase that was NOT clean — it carries PacketEventTrace — so
// the pin that matters most here is the layering itself: if a future change
// makes this header need an app-layer type, this file stops compiling.
#include "wblink/node/air_backend.h"

#include "wblink/node/aim.h"
#include "wblink/node/clock.h"
#include "wbtest.h"

namespace {

using wblink::node::AimHist;
using wblink::node::now_ms;
using wblink::node::now_us;
using wblink::node::packet_type_name;

// §3.1: the type nibble names every frame kind on the wire, and an unmodelled
// one from a mixed-version peer must land on "other" rather than indexing off
// the end of the table. The nibble is 4 bits, so all 16 slots are reachable
// from the air and every one of them has to be a valid string.
void test_packet_type_name_covers_every_nibble() {
    for (int n = 0; n < 16; ++n) {
        uint8_t frame[3] = {0, 0, static_cast<uint8_t>(n)};
        const char* name = packet_type_name(frame, sizeof(frame));
        CHECK(name != nullptr);
        CHECK(name[0] != '\0');
    }
    uint8_t data[3] = {0, 0, 0x01};
    CHECK(std::string(packet_type_name(data, 3)) == "data");
    // The high nibble is not part of the type and must not change the answer.
    uint8_t high[3] = {0, 0, 0xF1};
    CHECK(std::string(packet_type_name(high, 3)) == "data");
}

// A runt shorter than the §3.1 prefix has no type byte to read. It must answer
// "other", not read frame[2] — this is the one path in the tracer that a
// malformed frame off the air reaches directly.
void test_packet_type_name_refuses_a_runt() {
    uint8_t two[2] = {0, 0};
    CHECK(std::string(packet_type_name(two, 2)) == "other");
    CHECK(std::string(packet_type_name(two, 0)) == "other");
}

// §7.2 aim histogram: bucket edges are 50/100/200/500/1k/2k/5k us, and the
// boundary rule is `us >= edge` moves up. Off-by-one here would silently
// mis-report the tail, which is the entire quantity the histogram exists for
// ("distributions, not means: the tail is the contract").
void test_aim_hist_bucket_edges() {
    AimHist h;
    h.add(49);      // b[0]
    h.add(50);      // b[1] — the edge belongs to the HIGHER bucket
    h.add(4999);    // b[6]
    h.add(5000);    // b[7], the >=5k tail
    CHECK_EQ_U(h.n, 4);
    CHECK_EQ_U(h.b[0], 1);
    CHECK_EQ_U(h.b[1], 1);
    CHECK_EQ_U(h.b[6], 1);
    CHECK_EQ_U(h.b[7], 1);
    CHECK_EQ_U(h.max_us, 5000);
    // dump() on an empty histogram prints nothing rather than dividing by n.
    AimHist empty;
    empty.dump("empty");   // must not crash; n == 0 short-circuits
    CHECK_EQ_U(empty.n, 0);
}

// The node clock is monotonic and the two units agree. `core/` takes time as
// an injected argument on purpose; this is where the injected number comes
// from, so "it never goes backwards" is worth one assertion.
void test_clock_is_monotonic_and_consistent() {
    const uint64_t a_ms = now_ms();
    const uint64_t a_us = now_us();
    const uint64_t b_us = now_us();
    const uint64_t b_ms = now_ms();
    CHECK(b_us >= a_us);
    CHECK(b_ms >= a_ms);
    // Same clock, two scales: the us reading must not be behind the ms one.
    CHECK(a_us / 1000 + 1 >= a_ms);
}

}  // namespace

int main() {
    test_packet_type_name_covers_every_nibble();
    test_packet_type_name_refuses_a_runt();
    test_aim_hist_bucket_edges();
    test_clock_is_monotonic_and_consistent();
    return wbtest_finish("node_air_backend_test");
}
