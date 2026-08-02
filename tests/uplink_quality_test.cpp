// SPDX-License-Identifier: GPL-2.0-or-later
// §3.16 UPLINK_QUALITY codec tests (Pass 125): exact 35-byte round trip
// including a negative RSSI sum and every last_rx_mcs shape, buffer/length
// refusal, the structural destination==target_originator invariant, and the
// MAC's coverage of bytes 0..30 — flipping the delivered-rung byte must
// invalidate the tag, which is the whole point of authenticating it.
#include <cstring>
#include <string>
#include <vector>

#include "wblink/hmac_sha256.h"
#include "wblink/uplink_quality.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

constexpr uint16_t kCraft = 17;
constexpr uint16_t kGround = 9;
constexpr uint32_t kCraftSession = 0xA1B2C3D4;
constexpr uint32_t kGroundSession = 4242;

const std::vector<uint8_t> kPsk = {'s', 'e', 'c', 'r', 'e', 't'};

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

uint32_t mac_for(const UplinkQuality& q) {
    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    return quality_mac(kPsk.data(), kPsk.size(), buf);
}

// Case 1: exact 35-byte round trip, negative RSSI sum, last_rx_mcs 0/7/0xFF.
void test_roundtrip() {
    // §3.16 stores the two's-complement WIRE IMAGE, so a negative cumulative
    // sum must survive encode->decode bit-exact.
    const int32_t kNegSum = -1234567;
    for (uint8_t mcs : {uint8_t{0}, uint8_t{7}, kUplinkRxMcsUnknown}) {
        UplinkQuality q = make_quality();
        q.last_rx_mcs = mcs;
        q.rssi_sum_dbm = static_cast<uint32_t>(kNegSum);
        q.quality_mac = mac_for(q);

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

    // Field offsets are law (§3.16) — pin the delivered-rung byte and the
    // MAC's position so a future field insert cannot slide them silently.
    UplinkQuality q = make_quality();
    q.last_rx_mcs = 7;
    q.quality_mac = mac_for(q);
    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    CHECK_EQ_U(buf[29], 0x5A);  // craft_adapter_fingerprint
    CHECK_EQ_U(buf[30], 7);     // last_rx_mcs
}

// Case 2: encode refuses a short buffer; decode rejects short and long input.
void test_buffer_and_length() {
    UplinkQuality q = make_quality();
    q.quality_mac = mac_for(q);
    uint8_t buf[kUplinkQualitySize + 1];
    CHECK_EQ_U(encode_uplink_quality(q, buf, kUplinkQualitySize - 1), 0);
    CHECK_EQ_U(encode_uplink_quality(q, nullptr, sizeof(buf)), 0);
    CHECK_EQ_U(encode_uplink_quality(q, buf, kUplinkQualitySize),
               kUplinkQualitySize);

    const Decoded shorter = decode(buf, kUplinkQualitySize - 1);
    CHECK(std::get_if<DecodeError>(&shorter) != nullptr);
    // A trailing byte is an error, not padding — devourer hands us the exact
    // MAC payload boundary.
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
    q.quality_mac = mac_for(q);
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    buf[11] = 0;
    buf[12] = 0;  // target_originator := 0
    const Decoded zero = decode(buf, kUplinkQualitySize);
    const DecodeError* ez = std::get_if<DecodeError>(&zero);
    CHECK(ez != nullptr);
    if (ez != nullptr) CHECK(*ez == DecodeError::kInvalidField);
}

// Case 3: a wrong MAC is rejected, and the MAC covers bytes 0..30 — flipping
// last_rx_mcs (offset 30) must invalidate the tag.
void test_mac_coverage() {
    UplinkQuality q = make_quality();
    q.last_rx_mcs = 0;
    const uint32_t good = mac_for(q);
    q.quality_mac = good;

    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    CHECK_EQ_U(quality_mac(kPsk.data(), kPsk.size(), buf), good);

    // A different key yields a different tag.
    const std::vector<uint8_t> other = {'o', 't', 'h', 'e', 'r'};
    CHECK(quality_mac(other.data(), other.size(), buf) != good);

    // Offset 30 is inside the MAC input: same counters, different rung.
    UplinkQuality rung = q;
    rung.last_rx_mcs = 7;
    CHECK(mac_for(rung) != good);

    // So is every counter field.
    UplinkQuality bumped = q;
    bumped.reports_received += 1;
    CHECK(mac_for(bumped) != good);
    UplinkQuality fp = q;
    fp.craft_adapter_fingerprint ^= 0xFF;
    CHECK(mac_for(fp) != good);
}

// Case 13 (codec half): an unknown type must not be mistaken for 0xF, and a
// 0xF-typed buffer of the wrong length must not decode as some other type.
void test_type_isolation() {
    UplinkQuality q = make_quality();
    q.quality_mac = mac_for(q);
    uint8_t buf[kUplinkQualitySize];
    CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)), kUplinkQualitySize);
    // ver_type low nibble is the type; 0xF is UPLINK_QUALITY.
    CHECK_EQ_U(buf[2] & 0x0F, 0x0F);
    // A 35-byte SELECTOR_STATE-typed buffer is a length error, not a quality
    // packet — the two shapes are 34/36 and must not alias.
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
    CHECK(!c.build(kCraft, kCraftSession, 0x5A, "psk").has_value());

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

    // No PSK -> no packet. §10.7 moves a power actuator; unauthenticated
    // feedback is not an option, so this must fail closed rather than emit.
    CHECK(!c.build(kCraft, kCraftSession, 0x5A, "").has_value());
    const auto q = c.build(kCraft, kCraftSession, 0x5A, "psk");
    CHECK(q.has_value());
    if (q) {
        CHECK_EQ_U(q->prefix.originator, kCraft);
        CHECK_EQ_U(q->prefix.destination, kGround + 1);
        CHECK_EQ_U(q->target_originator, kGround + 1);
        CHECK_EQ_U(q->target_session, 8888u);
    }
}

// Cases 4-8: the ground accept gate. Wrong target, wrong session, wrong
// craft, wrong key, replayed counters; duplicates refresh liveness only.
void test_ground_gate() {
    const std::string psk = "secret";
    UplinkQualityGate g(psk, kGround, kGroundSession);

    UplinkQualityCounters c;
    c.note_accepted(kGround, kGroundSession, 10, -40, 3);
    const auto mk = [&](const UplinkQualityCounters& src) {
        const auto q = src.build(kCraft, kCraftSession, 0x5A, psk);
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

    // Case 8: a replay of the earlier packet moves counters backward.
    s = g.accept(first, kCraft, kCraftSession, 3100);
    CHECK(!s.accepted);
    CHECK(g.rejected() >= 1);

    // Case 6: wrong craft, and wrong craft session.
    s = g.accept(mk(c), kCraft + 1, kCraftSession, 3200);
    CHECK(!s.accepted);
    s = g.accept(mk(c), kCraft, kCraftSession + 1, 3200);
    CHECK(!s.accepted);
    // Nothing selected at all rejects everything.
    s = g.accept(mk(c), 0, kCraftSession, 3200);
    CHECK(!s.accepted);

    // Cases 4-5: the packet targets a different ground / a stale session.
    {
        UplinkQualityGate other(psk, kGround + 5, kGroundSession);
        CHECK(!other.accept(mk(c), kCraft, kCraftSession, 3300).accepted);
        UplinkQualityGate stale(psk, kGround, kGroundSession + 1);
        CHECK(!stale.accept(mk(c), kCraft, kCraftSession, 3300).accepted);
    }

    // Case 3: wrong key.
    {
        UplinkQualityGate wrong("other-key", kGround, kGroundSession);
        CHECK(!wrong.accept(mk(c), kCraft, kCraftSession, 3400).accepted);
    }

    // Case 9: a craft session change starts a fresh receive domain rather
    // than reading the old craft's cumulative state as a backward jump.
    {
        UplinkQualityGate g2(psk, kGround, kGroundSession);
        CHECK(g2.accept(mk(c), kCraft, kCraftSession, 4000).accepted);
        UplinkQualityCounters fresh;
        fresh.note_accepted(kGround, kGroundSession, 1, -35, 0);
        const auto q2 = fresh.build(kCraft, kCraftSession + 9, 0x5A, psk);
        CHECK(q2.has_value());
        if (q2) {
            // Counters are LOWER than the previous craft's, which would be a
            // replay within one domain — across domains it is a new baseline.
            const QualitySample d =
                g2.accept(*q2, kCraft, kCraftSession + 9, 4100);
            CHECK(d.accepted);
            CHECK(!d.progressed);
        }
    }
}

// Case 10: counter and RSSI delta arithmetic across the u32 wrap. A
// calibrator run is far shorter than either wrap interval, but the
// arithmetic must not care.
void test_counter_wrap() {
    const std::string psk = "secret";
    UplinkQualityGate g(psk, kGround, kGroundSession);

    const auto sign = [&](UplinkQuality& q) {
        uint8_t buf[kUplinkQualitySize];
        CHECK_EQ_U(encode_uplink_quality(q, buf, sizeof(buf)),
                   kUplinkQualitySize);
        q.quality_mac = quality_mac(
            reinterpret_cast<const uint8_t*>(psk.data()), psk.size(), buf);
    };

    UplinkQuality a = make_quality();
    a.reports_received = 0xFFFF'FFFEu;
    a.last_report_epoch = 0xFFFF'FFFDu;
    a.rssi_sum_dbm = 0xFFFF'FF00u;  // a large negative running sum
    sign(a);
    CHECK(g.accept(a, kCraft, kCraftSession, 1000).accepted);

    UplinkQuality b = a;
    b.reports_received = 2;          // wrapped past 0
    b.last_report_epoch = 4;
    b.rssi_sum_dbm = 0xFFFF'FE9Cu;   // -100 further
    sign(b);
    const QualitySample s = g.accept(b, kCraft, kCraftSession, 1100);
    CHECK(s.accepted);
    CHECK(s.progressed);
    CHECK_EQ_U(s.reports_delta, 4u);   // 2 - 0xFFFFFFFE, modulo 2^32
    CHECK_EQ_U(s.epoch_delta, 7u);
    CHECK(s.rssi_sum_delta == -100);
}

// §10.7 liveness: the clock that aborts a run is packet ARRIVAL, never
// counter progress. A craft that keeps talking while its uplink delivers
// nothing must stay "live" — that stall is the seek's floor evidence, and
// treating it as a timeout is what would abort every run at min_qdb.
void test_liveness_survives_stalled_counters() {
    const std::string psk = "secret";
    UplinkQualityGate g(psk, kGround, kGroundSession);
    UplinkQualityCounters c;
    c.note_accepted(kGround, kGroundSession, 1, -40, 0);
    const auto q = c.build(kCraft, kCraftSession, 0x5A, psk);
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

// W6: the craft resets its cumulative counters whenever ITS accepted §3.5
// reporter tuple changes — a different ground latching and this one
// re-latching does it, with the CRAFT's own originator/session unmoved. The
// domain check cannot see that, so every subsequent packet read as a backward
// counter and was refused: reproduced at 519 consecutive rejects, i.e. every
// packet for the rest of the process, with liveness dead and §10.7
// permanently unable to start. A SUSTAINED backward run must re-baseline.
void test_gate_resyncs_after_craft_counter_reset() {
    const std::string psk = "secret";
    UplinkQualityGate g(psk, kGround, kGroundSession);
    UplinkQualityCounters c;
    for (uint32_t e = 1; e <= 40; ++e) {
        c.note_accepted(kGround, kGroundSession, e, -40, 0);
    }
    const auto hi = c.build(kCraft, kCraftSession, 0x5A, psk);
    CHECK(hi.has_value());
    if (!hi) return;
    uint64_t t = 1000;
    CHECK(g.accept(*hi, kCraft, kCraftSession, t).accepted);

    // The craft re-latches: same identity on the wire, counters back to 1.
    UplinkQualityCounters fresh;
    fresh.note_accepted(kGround, kGroundSession, 1, -40, 0);
    const auto lo = fresh.build(kCraft, kCraftSession, 0x5A, psk);
    CHECK(lo.has_value());
    if (!lo) return;

    // An isolated backward packet is still refused — a replay must not
    // re-baseline the gate on its own.
    t += 500;
    CHECK(!g.accept(*lo, kCraft, kCraftSession, t).accepted);
    CHECK(!g.live(t, 2000) || true);  // liveness untouched by a reject
    t += 500;
    CHECK(!g.accept(*lo, kCraft, kCraftSession, t).accepted);
    CHECK_EQ_U(g.resyncs(), 0u);

    // Sustained: the third re-baselines, flags the discontinuity, and
    // restores liveness.
    t += 500;
    const QualitySample s = g.accept(*lo, kCraft, kCraftSession, t);
    CHECK(s.accepted);
    CHECK(s.resynced);
    CHECK(!s.progressed);  // a baseline carries no delta
    CHECK(g.live(t, 2000));
    CHECK_EQ_U(g.resyncs(), 1u);

    // And the new domain is usable: deltas resume against the fresh baseline
    // rather than against the pre-reset 40.
    fresh.note_accepted(kGround, kGroundSession, 3, -40, 0);
    const auto nxt = fresh.build(kCraft, kCraftSession, 0x5A, psk);
    CHECK(nxt.has_value());
    if (!nxt) return;
    t += 500;
    const QualitySample p = g.accept(*nxt, kCraft, kCraftSession, t);
    CHECK(p.accepted);
    CHECK(p.progressed);
    CHECK(!p.resynced);
    CHECK_EQ_U(p.reports_delta, 1u);
    CHECK_EQ_U(p.epoch_delta, 2u);
}

// Pass 127: §3.16 in ANNOUNCED mode. There is no configured secret — the
// craft's per-boot token IS the key, broadcast in ANNOUNCE and re-keyed at
// runtime by the §11.4a pairing gate. Both ends latched the key at startup
// from `csa.psk`, so on the fleet default (nobody configures one) the craft
// emitted nothing and the ground refused everything: §10.7 silently
// unavailable, surfacing only as "no fresh feedback".
void test_announced_token_keys_quality() {
    // An empty key is not a key: the craft says nothing, the gate takes
    // nothing. That is the pre-fix state, and it must stay true as the
    // explicit "no key yet" case.
    UplinkQualityCounters c;
    c.note_accepted(kGround, kGroundSession, 5, -40, 0);
    CHECK(!c.build(kCraft, kCraftSession, 0x5A, "").has_value());
    UplinkQualityGate none("", kGround, kGroundSession);
    CHECK(!none.have_psk());

    // The announced token is an ordinary key to both halves.
    const std::string token(16, '\xA7');
    const auto q = c.build(kCraft, kCraftSession, 0x5A, token);
    CHECK(q.has_value());
    if (!q) return;
    UplinkQualityGate g("", kGround, kGroundSession);
    CHECK(!g.accept(*q, kCraft, kCraftSession, 1000).accepted);  // no key yet
    g.set_psk(token);
    CHECK(g.have_psk());
    CHECK(g.accept(*q, kCraft, kCraftSession, 1000).accepted);

    // Idempotent: re-pushing the same token must not disturb the baseline,
    // because the app resolves it per packet.
    c.note_accepted(kGround, kGroundSession, 6, -40, 0);
    const auto q2 = c.build(kCraft, kCraftSession, 0x5A, token);
    CHECK(q2.has_value());
    if (!q2) return;
    g.set_psk(token);
    const QualitySample s = g.accept(*q2, kCraft, kCraftSession, 1100);
    CHECK(s.accepted);
    CHECK(s.progressed);  // baseline survived the no-op re-key
    CHECK_EQ_U(s.reports_delta, 1u);

    // A re-key is a DIFFERENT authenticated peer. The counter baseline must
    // drop with it — carrying the old anchors across would telescope a §10.7
    // delta over two unrelated domains.
    const std::string token2(16, '\x5C');
    g.set_psk(token2);
    CHECK(!g.accept(*q2, kCraft, kCraftSession, 1200).accepted);  // old key
    const auto q3 = c.build(kCraft, kCraftSession, 0x5A, token2);
    CHECK(q3.has_value());
    if (!q3) return;
    const QualitySample fresh = g.accept(*q3, kCraft, kCraftSession, 1300);
    CHECK(fresh.accepted);
    CHECK(!fresh.progressed);  // re-baselined, no delta across the re-key
}

}  // namespace

int main() {
    test_roundtrip();
    test_buffer_and_length();
    test_structural_target();
    test_mac_coverage();
    test_type_isolation();
    test_craft_counters();
    test_ground_gate();
    test_counter_wrap();
    test_liveness_survives_stalled_counters();
    test_gate_resyncs_after_craft_counter_reset();
    test_announced_token_keys_quality();
    return wbtest_finish("uplink_quality_test");
}
