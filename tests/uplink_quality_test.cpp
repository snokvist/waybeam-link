// SPDX-License-Identifier: GPL-2.0-or-later
// §3.16 UPLINK_QUALITY codec tests: exact 31-byte round trip including a
// negative RSSI sum and every last_rx_mcs shape, buffer/length refusal, the
// structural destination==target_originator invariant, and the ground accept
// gate's two clocks.
//
// Pass 131 deleted quality_mac and the whole key apparatus, so the cases that
// covered MAC coverage, wrong-key rejection and §11.4a token provenance are
// gone with it. What replaced them is the offset pin below: with no tag to
// invalidate, a silently-shifted field would no longer be caught by a failing
// MAC, so the layout has to be asserted directly.
#include <cstring>
#include <string>
#include <vector>

#include "wblink/uplink_quality.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

constexpr uint16_t kCraft = 17;
constexpr uint16_t kGround = 9;
constexpr uint32_t kCraftSession = 0xA1B2C3D4;
constexpr uint32_t kGroundSession = 4242;

UplinkQuality make_quality() {
    UplinkQuality q;
    q.prefix = {kCraft, kGround, kCraftSession};
    q.target_originator = kGround;
    q.target_session = kGroundSession;
    q.last_report_epoch = 1000;
    q.reports_received = 990;
    q.rssi_sum_dbm = 0;
    q.craft_adapter_fingerprint = 0x5A;
    q.last_rx_mcs = 0;
    return q;
}

// Case 1: exact 31-byte round trip, negative RSSI sum, last_rx_mcs 0/7/0xFF.
void test_roundtrip() {
    // §3.16 stores the two's-complement WIRE IMAGE, so a negative cumulative
    // sum must survive encode->decode bit-exact.
    const int32_t kNegSum = -1234567;
    for (uint8_t mcs : {uint8_t{0}, uint8_t{7}, kUplinkRxMcsUnknown}) {
        UplinkQuality q = make_quality();
        q.last_rx_mcs = mcs;
        q.rssi_sum_dbm = static_cast<uint32_t>(kNegSum);

        uint8_t buf[kUplinkQualitySize];
        CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)),
                   kUplinkQualitySize);
        const Decoded d = decode(buf, kUplinkQualitySize);
        const UplinkQuality* v = std::get_if<UplinkQuality>(&d);
        CHECK(v != nullptr);
        if (v == nullptr) return;
        CHECK(*v == q);
        CHECK_EQ_U(v->last_rx_mcs, mcs);
        CHECK(static_cast<int32_t>(v->rssi_sum_dbm) == kNegSum);
    }
}

// Field offsets are law (§3.16), and since Pass 131 nothing else guards them:
// with the MAC gone, a field that slid by a byte would still round-trip
// perfectly through our own encoder and decoder and only diverge against a
// peer built from the spec. Pin every offset the table names.
void test_field_offsets_pinned() {
    CHECK_EQ_U(kUplinkQualitySize, 31u);

    UplinkQuality q = make_quality();
    q.target_session = 0x11223344u;
    q.last_report_epoch = 0x55667788u;
    q.reports_received = 0x99AABBCCu;
    q.rssi_sum_dbm = 0xDDEEFF00u;
    q.craft_adapter_fingerprint = 0x5A;
    q.last_rx_mcs = 7;

    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    CHECK_EQ_U(buf[11], 0x00);  // target_originator (be16) = 9
    CHECK_EQ_U(buf[12], 0x09);
    CHECK_EQ_U(buf[13], 0x11);  // target_session
    CHECK_EQ_U(buf[16], 0x44);
    CHECK_EQ_U(buf[17], 0x55);  // last_report_epoch
    CHECK_EQ_U(buf[20], 0x88);
    CHECK_EQ_U(buf[21], 0x99);  // reports_received
    CHECK_EQ_U(buf[24], 0xCC);
    CHECK_EQ_U(buf[25], 0xDD);  // rssi_sum_dbm
    CHECK_EQ_U(buf[28], 0x00);
    CHECK_EQ_U(buf[29], 0x5A);  // craft_adapter_fingerprint
    CHECK_EQ_U(buf[30], 7);     // last_rx_mcs — the final byte since Pass 131
}

// Case 2: encode refuses a short buffer; decode rejects short and long input.
void test_buffer_and_length() {
    UplinkQuality q = make_quality();
    uint8_t buf[kUplinkQualitySize + 1];
    CHECK_EQ_U(encode_uplink_quality(q, buf, kUplinkQualitySize - 1), 0);
    CHECK_EQ_U(encode_uplink_quality(q, nullptr, sizeof(buf)), 0);
    CHECK_EQ_U(encode_uplink_quality(q, buf, kUplinkQualitySize),
               kUplinkQualitySize);

    const Decoded shorter = decode(buf, kUplinkQualitySize - 1);
    CHECK(std::get_if<DecodeError>(&shorter) != nullptr);
    // A trailing byte is an error, not padding — devourer hands us the exact
    // MPDU payload boundary. This also pins that a 35-byte Pass 125 packet no
    // longer decodes: the shape changed, and it must fail loudly.
    buf[kUplinkQualitySize] = 0;
    const Decoded longer = decode(buf, kUplinkQualitySize + 1);
    const DecodeError* e = std::get_if<DecodeError>(&longer);
    CHECK(e != nullptr);
    if (e != nullptr) CHECK(*e == DecodeError::kLengthMismatch);
}

// §3.16: target_originator MUST be non-zero and equal `destination`. Both are
// packet-internal, so both encode and decode enforce them.
void test_structural_target() {
    UplinkQuality q = make_quality();
    q.target_originator = 0;
    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), 0);

    q = make_quality();
    q.target_originator = kGround + 1;  // disagrees with prefix.destination
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), 0);

    // Forge the mismatch on the wire — decode must still refuse it.
    q = make_quality();
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    buf[11] = 0;
    buf[12] = 0;  // target_originator := 0
    const Decoded zero = decode(buf, kUplinkQualitySize);
    const DecodeError* ez = std::get_if<DecodeError>(&zero);
    CHECK(ez != nullptr);
    if (ez != nullptr) CHECK(*ez == DecodeError::kInvalidField);
}

// Case 13 (codec half): an unknown type must not be mistaken for 0xF, and a
// 0xF-typed buffer of the wrong length must not decode as some other type.
void test_type_isolation() {
    UplinkQuality q = make_quality();
    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    // ver_type low nibble is the type; 0xF is UPLINK_QUALITY.
    CHECK_EQ_U(buf[2] & 0x0F, 0x0F);
    // A 31-byte SELECTOR_STATE-typed buffer is a length error, not a quality
    // packet — the shapes are 32/34/36 and must not alias.
    buf[2] = static_cast<uint8_t>((buf[2] & 0xF0) | 0x0E);
    const Decoded d = decode(buf, kUplinkQualitySize);
    CHECK(std::get_if<UplinkQuality>(&d) == nullptr);
    CHECK(std::get_if<DecodeError>(&d) != nullptr);
}

// Case 11: only accepted, selector-fresh reports enter the craft counters.
// The caller owns the report-authority and freshness gates; what is pinned
// here is that a repeated epoch and a reporter change do the right thing.
void test_craft_counters() {
    UplinkQualityCounters c;
    CHECK_EQ_U(c.reports_received(), 0u);
    // Nothing accepted yet -> nothing honest to send.
    CHECK(!c.build(kCraft, kCraftSession, 0x5A).has_value());

    c.note_accepted(kGround, kGroundSession, 1, -40, 0);
    c.note_accepted(kGround, kGroundSession, 2, -42, 0);
    CHECK_EQ_U(c.reports_received(), 2u);
    CHECK_EQ_U(c.last_report_epoch(), 2u);
    CHECK(static_cast<int32_t>(c.rssi_sum()) == -82);
    CHECK_EQ_U(c.last_rx_mcs(), 0u);

    // A reporter tuple change restarts the domain — a new ground must never
    // read the previous one's history as its own delivery.
    c.note_accepted(kGround + 1, 7777, 500, -30, 7);
    CHECK_EQ_U(c.target_originator(), kGround + 1);
    CHECK_EQ_U(c.reports_received(), 1u);
    CHECK_EQ_U(c.last_report_epoch(), 500u);
    CHECK(static_cast<int32_t>(c.rssi_sum()) == -30);
    CHECK_EQ_U(c.last_rx_mcs(), 7u);
    // Same originator, new session (a ground reboot) is also a new domain.
    c.note_accepted(kGround + 1, 8888, 3, -31, 7);
    CHECK_EQ_U(c.reports_received(), 1u);

    // Pass 131: no key, so no key-shaped way to fail closed. The ONLY reasons
    // build() declines are "no latched reporter" and "no accepted report yet"
    // — which is what makes the Pass 127 failure unrepeatable, since the empty
    // configured secret that silenced the craft is no longer an input.
    const auto q = c.build(kCraft, kCraftSession, 0x5A);
    CHECK(q.has_value());
    if (q) {
        CHECK_EQ_U(q->prefix.originator, kCraft);
        CHECK_EQ_U(q->prefix.destination, kGround + 1);
        CHECK_EQ_U(q->target_originator, kGround + 1);
        CHECK_EQ_U(q->target_session, 8888u);
    }
}

// Cases 4-8: the ground accept gate. Wrong target, wrong session, wrong
// craft; duplicates refresh liveness only; backward counters re-baseline.
void test_ground_gate() {
    UplinkQualityGate g(kGround, kGroundSession);

    UplinkQualityCounters c;
    c.note_accepted(kGround, kGroundSession, 10, -40, 3);
    const auto mk = [&](const UplinkQualityCounters& src) {
        const auto q = src.build(kCraft, kCraftSession, 0x5A);
        CHECK(q.has_value());
        return *q;
    };

    // Baseline: first accepted packet has no delta to report.
    const UplinkQuality first = mk(c);
    QualitySample s = g.accept(first, kCraft, kCraftSession, 1000);
    CHECK(s.accepted);
    CHECK(!s.progressed);
    CHECK(g.live(1000, 2000));

    // Case 7: an exact duplicate refreshes liveness and yields no sample.
    s = g.accept(first, kCraft, kCraftSession, 2500);
    CHECK(s.accepted);
    CHECK(!s.progressed);
    CHECK(!s.resynced);  // equal counters are a duplicate, NOT backward
    CHECK(g.live(2500, 2000));
    // ...and liveness really moved: at t=4400 the age is 1900 ms from the
    // duplicate, but 3400 ms from the baseline — so this only passes if the
    // duplicate refreshed the clock.
    CHECK(g.live(4400, 2000));

    // Progress: deltas are reported, RSSI delta is signed.
    c.note_accepted(kGround, kGroundSession, 12, -50, 3);
    s = g.accept(mk(c), kCraft, kCraftSession, 3000);
    CHECK(s.accepted);
    CHECK(s.progressed);
    CHECK_EQ_U(s.reports_delta, 1u);
    CHECK_EQ_U(s.epoch_delta, 2u);
    CHECK(s.rssi_sum_delta == -50);

    // Case 8 (Pass 131 semantics): counters moving backward now means the
    // craft reset its domain, so the gate re-baselines instead of rejecting.
    // Unauthenticated there is no replay to tell it apart from, and the cost
    // of being wrong is asymmetric: a spurious re-baseline restarts one dwell
    // at the same power, whereas refusing a real reset wedged the gate
    // permanently (see the dedicated case below).
    s = g.accept(first, kCraft, kCraftSession, 3100);
    CHECK(s.accepted);
    CHECK(s.resynced);
    CHECK(!s.progressed);  // a baseline carries no delta

    // Case 6: wrong craft, and wrong craft session.
    s = g.accept(mk(c), kCraft + 1, kCraftSession, 3200);
    CHECK(!s.accepted);
    s = g.accept(mk(c), kCraft, kCraftSession + 1, 3200);
    CHECK(!s.accepted);
    // Nothing selected at all rejects everything — but is deliberately NOT
    // counted: `rejected` means "a packet failed a gate", and with no craft
    // selected there is no gate to fail. Counting it would make the stat climb
    // steadily on an idle ground and read as an attack.
    s = g.accept(mk(c), 0, kCraftSession, 3200);
    CHECK(!s.accepted);
    CHECK_EQ_U(g.rejected(), 2u);  // wrong craft, wrong craft session

    // Cases 4-5: the packet targets a different ground / a stale session.
    {
        UplinkQualityGate other(kGround + 5, kGroundSession);
        CHECK(!other.accept(mk(c), kCraft, kCraftSession, 3300).accepted);
        UplinkQualityGate stale(kGround, kGroundSession + 1);
        CHECK(!stale.accept(mk(c), kCraft, kCraftSession, 3300).accepted);
    }

    // Case 9: a craft session change starts a fresh receive domain rather
    // than reading the old craft's cumulative state as a backward jump.
    {
        UplinkQualityGate g2(kGround, kGroundSession);
        CHECK(g2.accept(mk(c), kCraft, kCraftSession, 4000).accepted);
        UplinkQualityCounters fresh;
        fresh.note_accepted(kGround, kGroundSession, 1, -35, 0);
        const auto q2 = fresh.build(kCraft, kCraftSession + 9, 0x5A);
        CHECK(q2.has_value());
        if (q2) {
            // Counters are LOWER than the previous craft's, which would be a
            // domain reset within one craft — across crafts it is simply a
            // new baseline, and must NOT be reported as a resync.
            const QualitySample d =
                g2.accept(*q2, kCraft, kCraftSession + 9, 4100);
            CHECK(d.accepted);
            CHECK(!d.progressed);
            CHECK(!d.resynced);
        }
    }
}

// Case 10: counter and RSSI delta arithmetic across the u32 wrap. A
// calibrator run is far shorter than either wrap interval, but the
// arithmetic must not care — and with the backward test being what
// distinguishes "wrapped" from "reset", getting this wrong now costs a
// spurious re-baseline rather than a rejected packet.
void test_counter_wrap() {
    UplinkQualityGate g(kGround, kGroundSession);

    UplinkQuality a = make_quality();
    a.reports_received = 0xFFFF'FFFEu;
    a.last_report_epoch = 0xFFFF'FFFDu;
    a.rssi_sum_dbm = 0xFFFF'FF00u;  // a large negative running sum
    CHECK(g.accept(a, kCraft, kCraftSession, 1000).accepted);

    UplinkQuality b = a;
    b.reports_received = 2;          // wrapped past 0
    b.last_report_epoch = 4;
    b.rssi_sum_dbm = 0xFFFF'FE9Cu;   // -100 further
    const QualitySample s = g.accept(b, kCraft, kCraftSession, 1100);
    CHECK(s.accepted);
    CHECK(s.progressed);
    CHECK(!s.resynced);                // a wrap is forward, not a reset
    CHECK_EQ_U(s.reports_delta, 4u);   // 2 - 0xFFFFFFFE, modulo 2^32
    CHECK_EQ_U(s.epoch_delta, 7u);
    CHECK(s.rssi_sum_delta == -100);
}

// §10.7 liveness: the clock that aborts a run is packet ARRIVAL, never
// counter progress. A craft that keeps talking while its uplink delivers
// nothing must stay "live" — that stall is the seek's floor evidence, and
// treating it as a timeout is what would abort every run at min_qdb.
void test_liveness_survives_stalled_counters() {
    UplinkQualityGate g(kGround, kGroundSession);
    UplinkQualityCounters c;
    c.note_accepted(kGround, kGroundSession, 1, -40, 0);
    const auto q = c.build(kCraft, kCraftSession, 0x5A);
    CHECK(q.has_value());
    if (!q) return;

    uint64_t t = 1000;
    CHECK(g.accept(*q, kCraft, kCraftSession, t).accepted);
    // Ten identical packets over 5 s: counters never move, liveness always
    // does. With the two clocks collapsed this would have timed out at 2 s.
    for (int i = 0; i < 10; ++i) {
        t += 500;
        const QualitySample s = g.accept(*q, kCraft, kCraftSession, t);
        CHECK(s.accepted);
        CHECK(!s.progressed);
        CHECK(g.live(t, 2000));
    }
    // Silence, however, does expire.
    CHECK(!g.live(t + 2001, 2000));
}

// W6 / Pass 131: the craft resets its cumulative counters whenever ITS
// accepted §3.5 reporter tuple changes — a different ground latching and this
// one re-latching does it, with the CRAFT's own originator/session unmoved.
// The domain check cannot see that, so every subsequent packet reads as a
// backward counter. Pass 126 refused an isolated one and needed three in a row
// to re-baseline; that heuristic wedged at 519 consecutive rejects when it
// guessed wrong, with liveness dead and §10.7 permanently unable to start.
// Pass 131 re-baselines on the FIRST backward packet: with no MAC there is no
// replay to distinguish a reset from.
void test_gate_resyncs_after_craft_counter_reset() {
    UplinkQualityGate g(kGround, kGroundSession);
    UplinkQualityCounters c;
    for (uint32_t e = 1; e <= 40; ++e) {
        c.note_accepted(kGround, kGroundSession, e, -40, 0);
    }
    const auto hi = c.build(kCraft, kCraftSession, 0x5A);
    CHECK(hi.has_value());
    if (!hi) return;
    uint64_t t = 1000;
    CHECK(g.accept(*hi, kCraft, kCraftSession, t).accepted);

    // The craft re-latches: same identity on the wire, counters back to 1.
    UplinkQualityCounters fresh;
    fresh.note_accepted(kGround, kGroundSession, 1, -40, 0);
    const auto lo = fresh.build(kCraft, kCraftSession, 0x5A);
    CHECK(lo.has_value());
    if (!lo) return;

    // First backward packet re-baselines, flags the discontinuity so §10.7
    // restarts any dwell spanning it, and keeps liveness alive throughout.
    t += 500;
    const QualitySample s = g.accept(*lo, kCraft, kCraftSession, t);
    CHECK(s.accepted);
    CHECK(s.resynced);
    CHECK(!s.progressed);  // a baseline carries no delta
    CHECK(g.live(t, 2000));
    CHECK_EQ_U(g.resyncs(), 1u);

    // And the new domain is usable immediately: deltas resume against the
    // fresh baseline rather than against the pre-reset 40. Under Pass 126 the
    // two packets it took to reach the strike threshold were dropped, which
    // at the 2 Hz cadence is a full second of a dwell scored as loss.
    fresh.note_accepted(kGround, kGroundSession, 3, -40, 0);
    const auto nxt = fresh.build(kCraft, kCraftSession, 0x5A);
    CHECK(nxt.has_value());
    if (!nxt) return;
    t += 500;
    const QualitySample p = g.accept(*nxt, kCraft, kCraftSession, t);
    CHECK(p.accepted);
    CHECK(p.progressed);
    CHECK(!p.resynced);
    CHECK_EQ_U(p.reports_delta, 1u);
    CHECK_EQ_U(p.epoch_delta, 2u);
    CHECK_EQ_U(g.resyncs(), 1u);  // not charged twice for one reset
}

}  // namespace

int main() {
    test_roundtrip();
    test_field_offsets_pinned();
    test_buffer_and_length();
    test_structural_target();
    test_type_isolation();
    test_craft_counters();
    test_ground_gate();
    test_counter_wrap();
    test_liveness_survives_stalled_counters();
    test_gate_resyncs_after_craft_counter_reset();
    return wbtest_finish("uplink_quality_test");
}
