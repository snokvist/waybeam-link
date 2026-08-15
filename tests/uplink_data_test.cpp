// SPDX-License-Identifier: GPL-2.0-or-later
// §7.5 uplink data plane (Pass 183): ground hold/budget policy and the craft
// admission matrix — bound-issuer gate, strictly-monotonic seq cursor with
// per-session reset, stream match, and the wire round-trip between the two
// halves. Every rejection path asserts BOTH the counter and the absence of
// delivery, so removing a guard fails the suite rather than passing it.
#include "wblink/node/uplink_data.h"

#include <cstring>
#include <string>
#include <vector>

#include "wblink/types.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

std::vector<uint8_t> payload_bytes(uint8_t fill, size_t n = 26) {
    return std::vector<uint8_t>(n, fill);
}

DataView make_dv(uint8_t stream_id, uint8_t stype, uint16_t originator,
                 uint32_t session, uint32_t seq, const uint8_t* p, size_t n) {
    DataView dv;
    dv.hdr.prefix.originator = originator;
    dv.hdr.prefix.session_id = session;
    dv.hdr.stream_id = stream_id;
    dv.hdr.stream_type = stype;
    dv.hdr.seq = seq;
    dv.payload = p;
    dv.payload_len = static_cast<uint16_t>(n);
    return dv;
}

struct DeliverLog {
    std::vector<std::vector<uint8_t>> out;
    auto sink() {
        return [this](const uint8_t* p, size_t n) {
            out.emplace_back(p, p + n);
        };
    }
};

void acceptor_matrix() {
    UplinkAcceptor ua(2, stream_type::kControl);
    DeliverLog log;
    const auto pay = payload_bytes(0x41);
    const std::optional<uint16_t> bound9 = 9;

    // Unmatched stream_id: not ours, no counter.
    CHECK(!ua.on_data(make_dv(3, stream_type::kControl, 9, 100, 1,
                              pay.data(), pay.size()),
                      bound9, log.sink()));
    CHECK(ua.st.rej_stream == 0 && ua.st.rej_unbound == 0 && log.out.empty());

    // stream_id match, type mismatch: consumed + rej_stream, no delivery.
    CHECK(ua.on_data(make_dv(2, stream_type::kTelemetry, 9, 100, 1,
                             pay.data(), pay.size()),
                     bound9, log.sink()));
    CHECK(ua.st.rej_stream == 1 && log.out.empty());

    // Unbound craft: rej_unbound, no delivery.
    CHECK(ua.on_data(make_dv(2, stream_type::kControl, 9, 100, 1, pay.data(),
                             pay.size()),
                     std::nullopt, log.sink()));
    CHECK(ua.st.rej_unbound == 1 && log.out.empty());

    // Wrong issuer (bound to 9, sender 12): rej_unbound, no delivery.
    CHECK(ua.on_data(make_dv(2, stream_type::kControl, 12, 100, 1, pay.data(),
                             pay.size()),
                     bound9, log.sink()));
    CHECK(ua.st.rej_unbound == 2 && log.out.empty());

    // Accept from the bound issuer; payload delivered verbatim.
    CHECK(ua.on_data(make_dv(2, stream_type::kControl, 9, 100, 5, pay.data(),
                             pay.size()),
                     bound9, log.sink()));
    CHECK(ua.st.accepted == 1 && log.out.size() == 1);
    CHECK(log.out[0] == pay);

    // Replay (same seq) and older seq: dup, no delivery.
    CHECK(ua.on_data(make_dv(2, stream_type::kControl, 9, 100, 5, pay.data(),
                             pay.size()),
                     bound9, log.sink()));
    CHECK(ua.on_data(make_dv(2, stream_type::kControl, 9, 100, 3, pay.data(),
                             pay.size()),
                     bound9, log.sink()));
    CHECK(ua.st.dup == 2 && log.out.size() == 1);

    // A seq gap is loss, not a fault: accepted.
    CHECK(ua.on_data(make_dv(2, stream_type::kControl, 9, 100, 50, pay.data(),
                             pay.size()),
                     bound9, log.sink()));
    CHECK(ua.st.accepted == 2);

    // New sender session opens a fresh cursor: a LOWER seq is accepted.
    CHECK(ua.on_data(make_dv(2, stream_type::kControl, 9, 101, 1, pay.data(),
                             pay.size()),
                     bound9, log.sink()));
    CHECK(ua.st.accepted == 3 && log.out.size() == 3);

    // The cursor never substitutes for the binding: a fresh seq from a
    // non-bound issuer is still rejected after prior acceptances.
    CHECK(ua.on_data(make_dv(2, stream_type::kControl, 12, 200, 99,
                             pay.data(), pay.size()),
                     bound9, log.sink()));
    CHECK(ua.st.rej_unbound == 3 && log.out.size() == 3);
}

FramerConfig ground_cfg(uint8_t stream_id, uint8_t stype) {
    FramerConfig fc;
    fc.originator = 9;
    fc.session_id = 0xAABBCCDD;
    fc.stream_id = stream_id;
    fc.stream_type = stype;
    return fc;
}

void ground_ungated_sends_now() {
    UplinkDataStream us(2, stream_type::kControl,
                        ground_cfg(2, stream_type::kControl));
    UplinkPolicy pol;
    DeliverLog wire;
    const auto d = payload_bytes(0x51);
    CHECK(!us.on_datagram(d.data(), d.size(), 1000, 1000000, /*gated=*/false,
                          pol, wire.sink()));
    CHECK(us.st.submitted == 1 && us.st.sent == 1 && us.held.empty());
    CHECK(wire.out.size() == 1);
}

void ground_control_latest_state() {
    UplinkDataStream us(2, stream_type::kControl,
                        ground_cfg(2, stream_type::kControl));
    UplinkPolicy pol;
    DeliverLog wire;
    const auto d1 = payload_bytes(0x61);
    const auto d2 = payload_bytes(0x62);
    CHECK(us.on_datagram(d1.data(), d1.size(), 1000, 1000000, true, pol,
                         wire.sink()));
    CHECK(us.on_datagram(d2.data(), d2.size(), 1001, 1001000, true, pol,
                         wire.sink()));
    // depth 1: only the newest survives, the replaced one is counted.
    CHECK(us.held.size() == 1 && us.st.dropped_stale == 1);
    // The held frame decodes to the SECOND payload.
    const Decoded dec = decode(us.held.front().data(), us.held.front().size());
    const DataView* dv = std::get_if<DataView>(&dec);
    CHECK(dv != nullptr);
    if (dv != nullptr) {
        CHECK(std::vector<uint8_t>(dv->payload, dv->payload + dv->payload_len)
              == d2);
    }
    us.flush(wire.sink());
    CHECK(us.held.empty() && us.st.sent == 1 && wire.out.size() == 1);
}

void ground_telemetry_fifo_cap() {
    UplinkDataStream us(3, stream_type::kTelemetry,
                        ground_cfg(3, stream_type::kTelemetry));
    UplinkPolicy pol;
    pol.telemetry_hold = 2;
    DeliverLog wire;
    const auto d1 = payload_bytes(0x71);
    const auto d2 = payload_bytes(0x72);
    const auto d3 = payload_bytes(0x73);
    us.on_datagram(d1.data(), d1.size(), 1000, 1000000, true, pol, wire.sink());
    us.on_datagram(d2.data(), d2.size(), 1001, 1001000, true, pol, wire.sink());
    us.on_datagram(d3.data(), d3.size(), 1002, 1002000, true, pol, wire.sink());
    // FIFO drop-oldest: d1 gone, d2+d3 held in order.
    CHECK(us.held.size() == 2 && us.st.dropped_stale == 1);
    const Decoded front = decode(us.held.front().data(),
                                 us.held.front().size());
    const DataView* fv = std::get_if<DataView>(&front);
    CHECK(fv != nullptr);
    if (fv != nullptr) {
        CHECK(std::vector<uint8_t>(fv->payload, fv->payload + fv->payload_len)
              == d2);
    }
    us.flush(wire.sink());
    CHECK(us.st.sent == 2 && wire.out.size() == 2);
}

void ground_pps_budget() {
    UplinkDataStream us(2, stream_type::kControl,
                        ground_cfg(2, stream_type::kControl));
    UplinkPolicy pol;
    pol.pps_budget = 2;
    DeliverLog wire;
    const auto d = payload_bytes(0x55);
    const uint64_t t0 = 5000000;
    us.on_datagram(d.data(), d.size(), 1000, t0, false, pol, wire.sink());
    us.on_datagram(d.data(), d.size(), 1000, t0 + 1000, false, pol,
                   wire.sink());
    // Third within the same second: dropped at ingress, never framed.
    us.on_datagram(d.data(), d.size(), 1000, t0 + 2000, false, pol,
                   wire.sink());
    CHECK(us.st.submitted == 3 && us.st.sent == 2 &&
          us.st.dropped_budget == 1);
    CHECK(wire.out.size() == 2);
    // A new one-second window admits again.
    us.on_datagram(d.data(), d.size(), 2001, t0 + 1100000, false, pol,
                   wire.sink());
    CHECK(us.st.sent == 3 && us.st.dropped_budget == 1);
}

void ground_oversize_budget() {
    UplinkDataStream us(3, stream_type::kTelemetry,
                        ground_cfg(3, stream_type::kTelemetry));
    UplinkPolicy pol;
    DeliverLog wire;
    // Exactly at the §7.5 budget: framed and sent.
    const auto ok = payload_bytes(0x11, kUplinkMaxDatagram);
    CHECK(!us.on_datagram(ok.data(), ok.size(), 1000, 1000000, false, pol,
                          wire.sink()));
    CHECK(us.st.sent == 1 && us.st.dropped_oversize == 0);
    // One byte over: dropped whole with its own counter, never framed.
    const auto big = payload_bytes(0x12, kUplinkMaxDatagram + 1);
    CHECK(!us.on_datagram(big.data(), big.size(), 1001, 1001000, false, pol,
                          wire.sink()));
    CHECK(us.st.dropped_oversize == 1 && us.st.sent == 1);
    CHECK(wire.out.size() == 1);
    // §15.3 accounting closes: submitted = sent + dropped_*.
    CHECK(us.st.submitted == us.st.sent + us.st.dropped_stale +
                                 us.st.dropped_budget +
                                 us.st.dropped_oversize);
}

void shape_rule() {
    StreamCfg s;
    s.stream_id = 2;
    s.stream_type = stream_type::kControl;
    s.dir = Dir::kIn;
    s.bind.kind = BindKind::kUdp;
    // rx-node uplink ingress: CONTROL/TELEMETRY over udp pass...
    CHECK(uplink_shape_error(s, /*tx_role=*/false) == nullptr);
    s.stream_type = stream_type::kTelemetry;
    CHECK(uplink_shape_error(s, false) == nullptr);
    // ...RTP / AUDIO / frame-shm are refused...
    s.stream_type = stream_type::kRtp;
    CHECK(uplink_shape_error(s, false) != nullptr);
    s.stream_type = stream_type::kAudio;
    CHECK(uplink_shape_error(s, false) != nullptr);
    s.stream_type = stream_type::kControl;
    s.bind.kind = BindKind::kFrameShm;
    CHECK(uplink_shape_error(s, false) != nullptr);
    // ...and the same stream is NOT an uplink stream on the other role, so
    // a tx node's RTP dir:"in" (video ingest) stays legal.
    s.stream_type = stream_type::kRtp;
    s.bind.kind = BindKind::kFrameShm;
    CHECK(uplink_shape_error(s, /*tx_role=*/true) == nullptr);
    // tx-node dir:"out" mirror.
    s.dir = Dir::kOut;
    s.bind.kind = BindKind::kUdp;
    s.stream_type = stream_type::kControl;
    CHECK(uplink_shape_error(s, true) == nullptr);
    s.stream_type = stream_type::kRtp;
    CHECK(uplink_shape_error(s, true) != nullptr);
    // rx-node dir:"out" RTP (video egress) stays legal.
    CHECK(uplink_shape_error(s, false) == nullptr);
}

void wire_round_trip_ground_to_craft() {
    UplinkDataStream us(2, stream_type::kControl,
                        ground_cfg(2, stream_type::kControl));
    UplinkPolicy pol;
    DeliverLog wire;
    const auto rc = payload_bytes(0xC8);  // CRSF-sized uplink datagram
    us.on_datagram(rc.data(), rc.size(), 1000, 1000000, false, pol,
                   wire.sink());
    CHECK(wire.out.size() == 1);

    const Decoded dec = decode(wire.out[0].data(), wire.out[0].size());
    const DataView* dv = std::get_if<DataView>(&dec);
    CHECK(dv != nullptr);
    if (dv == nullptr) {
        return;
    }
    // §7.5 stamps: one-datagram-one-block (EOB, no ARQ), profile fields 0.
    CHECK(dv->hdr.stream_type == stream_type::kControl);
    CHECK((dv->hdr.data_flags & data_flags::kEndOfBlock) != 0);
    CHECK((dv->hdr.data_flags & data_flags::kArq) == 0);
    CHECK(dv->hdr.active_profile == 0 && dv->hdr.table_version == 0);
    CHECK(dv->hdr.prefix.originator == 9);

    UplinkAcceptor ua(2, stream_type::kControl);
    DeliverLog craft;
    CHECK(ua.on_data(*dv, std::optional<uint16_t>(9), craft.sink()));
    CHECK(ua.st.accepted == 1 && craft.out.size() == 1);
    CHECK(craft.out[0] == rc);
}

}  // namespace

int main() {
    acceptor_matrix();
    ground_ungated_sends_now();
    ground_control_latest_state();
    ground_telemetry_fifo_cap();
    ground_pps_budget();
    ground_oversize_budget();
    shape_rule();
    wire_round_trip_ground_to_craft();
    return wbtest_finish("uplink_data_test");
}
