// SPDX-License-Identifier: GPL-2.0-or-later
// §15.1 UDP binding layer: loopback send -> poll_once -> recv through a
// BindingSet built from a real config (ephemeral ports so the test is
// parallel-safe), plus host:port parsing edges.
#include "wblink/binding.h"
#include "wblink/air_udp.h"
#include "wblink/wire.h"

#include <cstring>
#include <string>

#include "wbtest.h"

using namespace wblink;

int main() {
    const UdpAir::RxCb discard = [](const AirRxMeta&, const uint8_t*, size_t) {};
    // host:port parsing.
    {
        AirUdpCfg cfg;
        cfg.rx = {"127.0.0.1:0"};
        auto air = UdpAir::create(cfg);
        CHECK(bool(air));
        if (air) {
            unsigned accepted_events = 0;
            air.value->set_trace(
                [&](const char* direction, const char* outcome, int adapter,
                    const uint8_t*, size_t len) {
                    CHECK(std::strcmp(direction, "rx") == 0);
                    CHECK(std::strcmp(outcome, "accepted") == 0);
                    CHECK_EQ_U(adapter, 0);
                    CHECK_EQ_U(len, 3u);
                    ++accepted_events;
                });
            AirUdpCfg sender_cfg;
            sender_cfg.tx = {"127.0.0.1:" +
                             std::to_string(air.value->adapter_port(0))};
            auto sender = UdpAir::create(sender_cfg);
            CHECK(bool(sender));
            const uint8_t msg[] = {1, 2, 3};
            CHECK_EQ_U(sender.value->inject(msg, sizeof(msg)), 1u);
            CHECK_EQ_U(sender.value->tx_submitted(), 1u);
            CHECK_EQ_U(sender.value->tx_failed(), 0u);
            CHECK_EQ_U(air.value->poll_once(100, discard), 1u);
            CHECK_EQ_U(air.value->rx_frames(0), 1u);
            CHECK_EQ_U(air.value->rx_dropped(0), 0u);
            CHECK_EQ_U(accepted_events, 1u);
        }
    }

    // One loopback broadcast reaches every shared-port listener; each listener
    // rejects its own originator exactly as the RF backends do.
    {
        AirUdpCfg craft_cfg;
        craft_cfg.broadcast = true;
        craft_cfg.originator = 17;
        craft_cfg.rx = {"0.0.0.0:0"};
        auto craft = UdpAir::create(craft_cfg);
        CHECK(bool(craft));
        if (craft) {
            const uint16_t port = craft.value->adapter_port(0);
            AirUdpCfg ground_cfg;
            ground_cfg.broadcast = true;
            ground_cfg.originator = 9;
            ground_cfg.tx = {"127.255.255.255:" + std::to_string(port)};
            ground_cfg.rx = {"0.0.0.0:" + std::to_string(port),
                             "0.0.0.0:" + std::to_string(port)};
            auto ground = UdpAir::create(ground_cfg);
            CHECK(bool(ground));

            AirUdpCfg sender_cfg;
            sender_cfg.broadcast = true;
            sender_cfg.tx = {"127.255.255.255:" + std::to_string(port)};
            auto sender = UdpAir::create(sender_cfg);
            CHECK(bool(sender));
            if (ground && sender) {
                uint8_t frame[kCommonPrefixSize]{};
                const Heartbeat hb{{17, 0, 1234}};
                CHECK_EQ_U(encode_heartbeat(hb, frame, sizeof(frame)),
                           sizeof(frame));
                CHECK_EQ_U(sender.value->inject(frame, sizeof(frame)), 1u);
                CHECK_EQ_U(craft.value->poll_once(100, discard), 0u);
                CHECK_EQ_U(ground.value->poll_once(100, discard), 2u);
                CHECK_EQ_U(craft.value->rx_filtered(0), 0u);
                CHECK_EQ_U(craft.value->kernel_dropped(0), 0u);
                CHECK_EQ_U(ground.value->rx_frames(0), 1u);
                CHECK_EQ_U(ground.value->rx_frames(1), 1u);

                uint8_t own_frame[kCommonPrefixSize]{};
                const Heartbeat own_hb{{9, 0, 5678}};
                CHECK_EQ_U(encode_heartbeat(own_hb, own_frame,
                                            sizeof(own_frame)),
                           sizeof(own_frame));
                CHECK_EQ_U(ground.value->inject(own_frame,
                                                sizeof(own_frame)),
                           1u);
                CHECK_EQ_U(craft.value->poll_once(100, discard), 1u);
                CHECK_EQ_U(ground.value->poll_once(100, discard), 0u);
                CHECK_EQ_U(ground.value->rx_filtered(0), 1u);
                CHECK_EQ_U(ground.value->rx_filtered(1), 1u);

                uint8_t recovery_frame[kRecoveryRequestSize]{};
                RecoveryRequest recovery;
                recovery.prefix = {9, 17, 9012};
                recovery.target_originator = 17;
                recovery.target_session = 1234;
                recovery.target_stream_id = 0;
                CHECK_EQ_U(encode_recovery_request(
                               recovery, recovery_frame,
                               sizeof(recovery_frame)),
                           sizeof(recovery_frame));
                CHECK_EQ_U(ground.value->inject(recovery_frame,
                                                sizeof(recovery_frame)),
                           1u);
                unsigned recovery_received = 0;
                const UdpAir::RxCb receive_recovery =
                    [&](const AirRxMeta&, const uint8_t* data, size_t len) {
                        const Decoded decoded = decode(data, len);
                        const auto* request =
                            std::get_if<RecoveryRequest>(&decoded);
                        CHECK(request != nullptr);
                        if (request != nullptr) {
                            CHECK_EQ_U(request->target_originator, 17u);
                            CHECK_EQ_U(request->target_session, 1234u);
                            ++recovery_received;
                        }
                    };
                CHECK_EQ_U(craft.value->poll_once(100, receive_recovery), 1u);
                CHECK_EQ_U(recovery_received, 1u);
                CHECK_EQ_U(ground.value->poll_once(100, discard), 0u);
                CHECK_EQ_U(ground.value->rx_filtered(0), 2u);
                CHECK_EQ_U(ground.value->rx_filtered(1), 2u);

                const uint8_t junk[] = {1, 2, 3};
                CHECK_EQ_U(sender.value->inject(junk, sizeof(junk)), 1u);
                CHECK_EQ_U(craft.value->poll_once(100, discard), 0u);
                CHECK_EQ_U(ground.value->poll_once(100, discard), 0u);
                CHECK_EQ_U(craft.value->rx_filtered(0), 1u);
                CHECK_EQ_U(ground.value->rx_filtered(0), 3u);
                CHECK_EQ_U(ground.value->rx_filtered(1), 3u);
            }
        }
    }

    // Real node shape: both peers inject and sniff the same paced channel.
    // Self copies are rejected before the queue; foreign traffic is delivered.
    {
        uint16_t port = 0;
        {
            auto reservation = UdpIngress::open("0.0.0.0:0");
            CHECK(bool(reservation));
            if (reservation) port = reservation.value->bound_port();
        }
        CHECK(port != 0);
        AirUdpCfg craft_cfg;
        craft_cfg.broadcast = true;
        craft_cfg.originator = 17;
        craft_cfg.pace_mbps = 10;
        craft_cfg.tx = {"127.255.255.255:" + std::to_string(port)};
        craft_cfg.rx = {"0.0.0.0:" + std::to_string(port)};
        AirUdpCfg ground_cfg = craft_cfg;
        ground_cfg.originator = 9;
        auto craft = UdpAir::create(craft_cfg);
        auto ground = UdpAir::create(ground_cfg);
        CHECK(bool(craft));
        CHECK(bool(ground));
        if (craft && ground) {
            uint8_t craft_frame[kCommonPrefixSize]{};
            uint8_t ground_frame[kCommonPrefixSize]{};
            CHECK_EQ_U(encode_heartbeat(Heartbeat{{17, 0, 111}}, craft_frame,
                                        sizeof(craft_frame)),
                       sizeof(craft_frame));
            CHECK_EQ_U(encode_heartbeat(Heartbeat{{9, 17, 222}}, ground_frame,
                                        sizeof(ground_frame)),
                       sizeof(ground_frame));
            CHECK_EQ_U(craft.value->inject(craft_frame, sizeof(craft_frame)), 1u);
            CHECK_EQ_U(ground.value->inject(ground_frame, sizeof(ground_frame)),
                       1u);
            int craft_got = 0;
            int ground_got = 0;
            for (int tries = 0; tries < 100 &&
                                (craft_got == 0 || ground_got == 0);
                 ++tries) {
                craft_got += craft.value->poll_once(2, discard);
                ground_got += ground.value->poll_once(2, discard);
            }
            CHECK_EQ_U(craft_got, 1u);
            CHECK_EQ_U(ground_got, 1u);
            CHECK_EQ_U(craft.value->rx_frames(0), 1u);
            CHECK_EQ_U(ground.value->rx_frames(0), 1u);
            CHECK_EQ_U(craft.value->rx_filtered(0), 1u);
            CHECK_EQ_U(ground.value->rx_filtered(0), 1u);
            CHECK_EQ_U(craft.value->kernel_dropped(0), 0u);
            CHECK_EQ_U(ground.value->kernel_dropped(0), 0u);
            CHECK_EQ_U(craft.value->tx_submitted(), 1u);
            CHECK_EQ_U(ground.value->tx_submitted(), 1u);
        }
    }

    // A paced RX-only instance must fail injection without retaining a queue.
    {
        AirUdpCfg cfg;
        cfg.pace_mbps = 10;
        cfg.rx = {"127.0.0.1:0"};
        auto air = UdpAir::create(cfg);
        CHECK(bool(air));
        if (air) {
            const uint8_t msg = 1;
            CHECK_EQ_U(air.value->inject(&msg, 1), 0u);
            CHECK(!air.value->tx_pending());
            CHECK_EQ_U(air.value->tx_failed(), 1u);
        }
    }

    {
        AirUdpCfg cfg;
        cfg.rx = {"127.0.0.1:0"};
        cfg.rx_drop_permille = 1000;
        auto air = UdpAir::create(cfg);
        CHECK(bool(air));
        if (air) {
            unsigned drop_events = 0;
            air.value->set_trace(
                [&](const char* direction, const char* outcome, int adapter,
                    const uint8_t*, size_t len) {
                    CHECK(std::strcmp(direction, "rx") == 0);
                    CHECK(std::strcmp(outcome, "synthetic_drop") == 0);
                    CHECK_EQ_U(adapter, 0);
                    CHECK_EQ_U(len, 1u);
                    ++drop_events;
                });
            AirUdpCfg sender_cfg;
            sender_cfg.tx = {"127.0.0.1:" +
                             std::to_string(air.value->adapter_port(0))};
            auto sender = UdpAir::create(sender_cfg);
            CHECK(bool(sender));
            const uint8_t msg = 7;
            CHECK_EQ_U(sender.value->inject(&msg, 1), 1u);
            CHECK_EQ_U(air.value->poll_once(100, discard), 0u);
            CHECK_EQ_U(air.value->rx_frames(0), 0u);
            CHECK_EQ_U(air.value->rx_dropped(0), 1u);
            CHECK_EQ_U(drop_events, 1u);
        }
    }

    {
        auto ok = split_host_port("127.0.0.1:5600");
        CHECK(bool(ok));
        if (ok) {
            CHECK(ok.value->first == "127.0.0.1");
            CHECK_EQ_U(ok.value->second, 5600);
        }
        CHECK(!split_host_port("127.0.0.1"));
        CHECK(!split_host_port("127.0.0.1:"));
        CHECK(!split_host_port(":5600"));
        CHECK(!split_host_port("127.0.0.1:99999"));
        CHECK(!split_host_port("127.0.0.1:56x0"));
    }

    // Raw ingress/egress pair on an ephemeral port.
    {
        auto in = UdpIngress::open("127.0.0.1:0");
        CHECK(bool(in));
        auto out = UdpEgress::open("127.0.0.1:" +
                                   std::to_string(in.value->bound_port()));
        CHECK(bool(out));

        const uint8_t msg[] = {0x57, 0x42, 0x01, 0x02, 0x03};
        CHECK(out.value->send(msg, sizeof(msg)));

        uint8_t buf[64];
        long n = 0;
        for (int tries = 0; tries < 100 && n <= 0; ++tries) {
            n = in.value->recv_one(buf, sizeof(buf));
        }
        CHECK_EQ_U(static_cast<unsigned long long>(n), sizeof(msg));
        CHECK(std::memcmp(buf, msg, sizeof(msg)) == 0);
    }

    // Full BindingSet from a config: one in-stream, one out-stream, stats.
    {
        auto cfg = load_config_json(R"({
          "node": {"originator": 1, "role": "rx"},
          "streams": [
            {"stream_id": 0, "stream_type": "RTP", "dir": "in",
             "bind": {"kind": "udp", "listen": "127.0.0.1:0"}},
            {"stream_id": 1, "stream_type": "RTP", "dir": "out",
             "bind": {"kind": "udp", "send": "127.0.0.1:65000"}}
          ],
          "stats": {"hz": 1,
                    "bind": {"kind": "udp", "send": "127.0.0.1:65001"}}
        })");
        CHECK(bool(cfg));
        auto set = BindingSet::create(*cfg.value);
        CHECK(bool(set));
        if (set) {
            BindingSet& b = *set.value;
            CHECK(b.egress_for(1) != nullptr);
            CHECK(b.egress_for(0) == nullptr);
            CHECK(b.egress_for(9) == nullptr);
            CHECK(b.stats_egress() != nullptr);

            const uint16_t port = b.ingress_port(0);
            CHECK(port != 0);
            CHECK_EQ_U(b.ingress_port(1), 0);

            auto tx = UdpEgress::open("127.0.0.1:" + std::to_string(port));
            CHECK(bool(tx));
            const uint8_t d1[] = {1, 2, 3};
            const uint8_t d2[] = {4, 5, 6, 7};
            CHECK(tx.value->send(d1, sizeof(d1)));
            CHECK(tx.value->send(d2, sizeof(d2)));

            int got = 0;
            size_t total = 0;
            uint8_t last_stream = 0xFF;
            for (int tries = 0; tries < 100 && got < 2; ++tries) {
                got += b.poll_once(50, [&](const IngressEvent& ev) {
                    last_stream = ev.stream_id;
                    total += ev.len;
                });
            }
            CHECK_EQ_U(got, 2);
            CHECK_EQ_U(total, sizeof(d1) + sizeof(d2));
            CHECK_EQ_U(last_stream, 0);

            // Nothing pending: poll_once times out with 0.
            CHECK_EQ_U(b.poll_once(1, [](const IngressEvent&) {}), 0);
        }
    }

    // Unbindable port must fail at create, not at first traffic.
    {
        auto cfg = load_config_json(R"({
          "node": {"originator": 1, "role": "rx"},
          "streams": [
            {"stream_id": 0, "stream_type": "RTP", "dir": "in",
             "bind": {"kind": "udp", "listen": "8.8.8.8:5600"}}]
        })");
        CHECK(bool(cfg));
        auto set = BindingSet::create(*cfg.value);
        CHECK(!set);
    }

    return wbtest_finish("udp_test");
}
