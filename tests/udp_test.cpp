// SPDX-License-Identifier: GPL-2.0-or-later
// §15.1 UDP binding layer: loopback send -> poll_once -> recv through a
// BindingSet built from a real config (ephemeral ports so the test is
// parallel-safe), plus host:port parsing edges.
#include "wblink/binding.h"
#include "wblink/air_udp.h"

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
        }
    }

    {
        AirUdpCfg cfg;
        cfg.rx = {"127.0.0.1:0"};
        cfg.rx_drop_permille = 1000;
        auto air = UdpAir::create(cfg);
        CHECK(bool(air));
        if (air) {
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
