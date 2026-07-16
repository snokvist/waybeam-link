// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link — one portable binary, modes tx / rx / loopback (PROTOCOL.md
// §16.1). Steps 1–10 are live: wire codec, I/O/config/stats, framer + ring,
// merged RX engine, resend scheduler, loopback bench, udp-air dev backend,
// NAL classifier, §9 selector + §10 power, the devourer radio backend
// (§3.0) with the §7.2 TSF quiet-gap pacer, and the §11 follow-me CSA
// (craft follower / ground issuer, triggered via POST /api/v1/csa), and the
// §15.5 REST control plane (stats + live knobs; stdin CSA trigger is gone).
#include <poll.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <deque>
#include <memory>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "wblink/air_udp.h"
#include "wblink/binding.h"
#include "wblink/cache_controller.h"
#include "wblink/cache_store.h"
#include "wblink/cache_udp.h"
#include "wblink/config.h"
#include "wblink/control_server.h"
#include "wblink/csa.h"
#include "wblink/endian.h"
#include "wblink/fps_ladder.h"
#include "wblink/frame_caps.h"
#include "wblink/frame_framer.h"
#include "wblink/frame_reassembler.h"
#include "wblink/frame_shm.h"
#include "wblink/frame_shm_format.h"
#include "wblink/framer.h"
#include "wblink/jscc_runtime_shadow.h"
#include "wblink/loss_model.h"
#include "wblink/power.h"
#include "wblink/power_file.h"
#include "wblink/quietgap.h"
#include "wblink/report_gate.h"
#include "wblink/reporter.h"
#include "wblink/ring.h"
#include "wblink/rx.h"
#include "wblink/scheduler.h"
#include "wblink/selector.h"
#include "wblink/stats.h"
#include "wblink/table.h"
#include "wblink/txwedge.h"
#include "wblink/venc.h"
#include "wblink/air_mon.h"
#if WBLINK_RADIO
#include "wblink/air_radio.h"
#endif

namespace {

using namespace wblink;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Bench-only, bounded packet-event trace. The wire remains the source of truth:
// this observer decodes existing frames and never feeds decisions back in.
class PacketEventTrace {
  public:
    explicit PacketEventTrace(const char* role) : role_(role) {
        const char* path = std::getenv("WBLINK_PACKET_TRACE");
        if (path == nullptr || *path == '\0') return;
        if (const char* cap = std::getenv("WBLINK_PACKET_TRACE_MAX")) {
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(cap, &end, 10);
            if (end != cap && *end == '\0' && parsed > 0) {
                cap_ = static_cast<uint64_t>(parsed);
            }
        }
        out_ = std::fopen(path, "w");
        if (out_ == nullptr) {
            std::fprintf(stderr, "packet-trace: cannot open %s\n", path);
            return;
        }
        static constexpr size_t kBufferBytes = 1024 * 1024;
        buffer_.resize(kBufferBytes);
        std::setvbuf(out_, buffer_.data(), _IOFBF, buffer_.size());
        std::fprintf(out_,
                     "{\"type\":\"schema\",\"schema\":\"waybeam-packet-events-v1\","
                     "\"role\":\"%s\",\"cap\":%llu}\n",
                     role_, static_cast<unsigned long long>(cap_));
    }

    ~PacketEventTrace() {
        if (out_ == nullptr) return;
        std::fprintf(out_,
                     "{\"type\":\"trace_end\",\"events\":%llu,"
                     "\"events_dropped\":%llu}\n",
                     static_cast<unsigned long long>(events_),
                     static_cast<unsigned long long>(dropped_));
        std::fclose(out_);
    }

    bool enabled() const { return out_ != nullptr; }
    void flush() { if (out_ != nullptr) std::fflush(out_); }

    void packet(const char* direction, const char* outcome, int adapter,
                const uint8_t* frame, size_t len) {
        if (out_ == nullptr) return;
        if (events_ >= cap_) {
            ++dropped_;
            return;
        }
        ++events_;
        const uint64_t t = now_us();
        const Decoded dec = decode(frame, len);
        if (const DataView* data = std::get_if<DataView>(&dec)) {
            const bool repair =
                (data->hdr.data_flags & data_flags::kFecRepair) != 0;
            uint16_t k = 0;
            uint16_t symbol = 0;
            uint32_t frame_len = 0;
            if (repair && data->payload_len >= kFecRepairSubheaderSize) {
                k = be16_read(data->payload + kFecOffWindowLen);
                symbol = data->payload[kFecOffRepairIdx];
                frame_len = be32_read(data->payload + kFecOffFrameLen);
            } else if (!repair && data->payload_len >= kFecSourceSubheaderSize) {
                k = be16_read(data->payload + kFecSrcOffWindowLen);
                symbol = be16_read(data->payload + kFecSrcOffSymIndex);
            }
            std::fprintf(
                out_,
                "{\"type\":\"packet\",\"t_us\":%llu,\"direction\":\"%s\","
                "\"outcome\":\"%s\",\"adapter\":%d,\"packet\":\"data\","
                "\"originator\":%u,\"session\":%u,\"stream\":%u,"
                "\"block\":%u,\"seq\":%u,\"kind\":\"%s\","
                "\"symbol\":%u,\"k\":%u,\"frame_len\":%u,"
                "\"arq\":%s,\"retransmit\":%s,\"eob\":%s,"
                "\"bytes\":%zu}\n",
                static_cast<unsigned long long>(t), direction, outcome, adapter,
                data->hdr.prefix.originator, data->hdr.prefix.session_id,
                data->hdr.stream_id, data->hdr.block_id, data->hdr.seq,
                repair ? "repair" : "source", symbol, k, frame_len,
                (data->hdr.data_flags & data_flags::kArq) ? "true" : "false",
                (data->hdr.data_flags & data_flags::kRetransmit) ? "true" : "false",
                (data->hdr.data_flags & data_flags::kEndOfBlock) ? "true" : "false",
                len);
            return;
        }
        if (const NackView* nack = std::get_if<NackView>(&dec)) {
            std::string bitmap;
            static constexpr char kHex[] = "0123456789abcdef";
            bitmap.reserve(static_cast<size_t>(nack->bitmap_len) * 2);
            for (uint8_t i = 0; i < nack->bitmap_len; ++i) {
                bitmap.push_back(kHex[nack->bitmap[i] >> 4]);
                bitmap.push_back(kHex[nack->bitmap[i] & 0x0f]);
            }
            std::fprintf(
                out_,
                "{\"type\":\"packet\",\"t_us\":%llu,\"direction\":\"%s\","
                "\"outcome\":\"%s\",\"adapter\":%d,\"packet\":\"nack\","
                "\"originator\":%u,\"session\":%u,\"stream\":%u,"
                "\"target_originator\":%u,\"target_session\":%u,"
                "\"base_seq\":%u,\"bitmap\":\"%s\",\"bytes\":%zu}\n",
                static_cast<unsigned long long>(t), direction, outcome, adapter,
                nack->hdr.prefix.originator, nack->hdr.prefix.session_id,
                nack->hdr.target_stream_id, nack->hdr.target_originator,
                nack->hdr.target_session, nack->hdr.base_seq, bitmap.c_str(), len);
            return;
        }
        std::fprintf(out_,
                     "{\"type\":\"packet\",\"t_us\":%llu,"
                     "\"direction\":\"%s\",\"outcome\":\"%s\","
                     "\"adapter\":%d,\"packet\":\"other\",\"bytes\":%zu}\n",
                     static_cast<unsigned long long>(t), direction, outcome,
                     adapter, len);
    }

  private:
    const char* role_;
    std::FILE* out_ = nullptr;
    std::vector<char> buffer_;
    uint64_t cap_ = 75000;
    uint64_t events_ = 0;
    uint64_t dropped_ = 0;
};

// Pass 17: bounded, observational catalog. It never feeds latch decisions.
class DiscoveryCatalog {
  public:
    void observe(const Decoded& dec, uint64_t now) {
        const CommonPrefix* p = nullptr;
        const DataView* data = std::get_if<DataView>(&dec);
        if (data != nullptr) {
            p = &data->hdr.prefix;
        } else if (const Heartbeat* hb = std::get_if<Heartbeat>(&dec)) {
            p = &hb->prefix;
        } else {
            return;
        }
        const uint64_t nk = (static_cast<uint64_t>(p->originator) << 32) |
                            p->session_id;
        nodes_[nk] = Node{p->originator, p->session_id, now};
        if (data != nullptr) {
            const uint64_t sk = (nk << 8) | data->hdr.stream_id;
            Stream& s = streams_[sk];
            if (s.packet_count == 0) {
                s.originator = p->originator;
                s.session = p->session_id;
                s.stream_id = data->hdr.stream_id;
                s.stream_type = data->hdr.stream_type;
                s.first_seen_ms = now;
            }
            ++s.packet_count;
            s.last_seen_ms = now;
        }
        trim();
    }

    std::string json(uint64_t now, const std::vector<StreamKey>& latched) {
        age(now);
        std::string out = "{\"nodes\":[";
        bool comma = false;
        for (const auto& [key, n] : nodes_) {
            (void)key;
            if (comma) out += ',';
            comma = true;
            out += "{\"originator\":" + std::to_string(n.originator) +
                   ",\"session\":" + std::to_string(n.session) +
                   ",\"last_seen_ms\":" + std::to_string(n.last_seen_ms) + "}";
        }
        out += "],\"streams\":[";
        comma = false;
        for (const auto& [key, s] : streams_) {
            (void)key;
            bool is_latched = false;
            for (const StreamKey& k : latched) {
                is_latched = is_latched ||
                             (k.originator == s.originator &&
                              k.session_id == s.session &&
                              k.stream_id == s.stream_id);
            }
            if (comma) out += ',';
            comma = true;
            out += "{\"originator\":" + std::to_string(s.originator) +
                   ",\"session\":" + std::to_string(s.session) +
                   ",\"stream_id\":" + std::to_string(s.stream_id) +
                   ",\"stream_type\":" + std::to_string(s.stream_type) +
                   ",\"packet_count\":" + std::to_string(s.packet_count) +
                   ",\"first_seen_ms\":" + std::to_string(s.first_seen_ms) +
                   ",\"last_seen_ms\":" + std::to_string(s.last_seen_ms) +
                   ",\"latched\":" + (is_latched ? "true" : "false") + "}";
        }
        out += "]}";
        return out;
    }

  private:
    struct Node {
        uint16_t originator = 0;
        uint32_t session = 0;
        uint64_t last_seen_ms = 0;
    };
    struct Stream {
        uint16_t originator = 0;
        uint32_t session = 0;
        uint8_t stream_id = 0;
        uint8_t stream_type = 0;
        uint64_t packet_count = 0;
        uint64_t first_seen_ms = 0;
        uint64_t last_seen_ms = 0;
    };
    void age(uint64_t now) {
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            it = now > it->second.last_seen_ms + 5000 ? nodes_.erase(it)
                                                       : std::next(it);
        }
        for (auto it = streams_.begin(); it != streams_.end();) {
            it = now > it->second.last_seen_ms + 5000 ? streams_.erase(it)
                                                       : std::next(it);
        }
    }
    void trim() {
        while (nodes_.size() > 64) nodes_.erase(nodes_.begin());
        while (streams_.size() > 64) streams_.erase(streams_.begin());
    }
    std::map<uint64_t, Node> nodes_;
    std::map<uint64_t, Stream> streams_;
};

// §15.2 policy.csa → the core engine's parameter block (string PSK to raw
// bytes, seconds to ms).
CsaParams csa_params(const Config& cfg) {
    const CsaPolicy& c = cfg.policy.csa;
    CsaParams p;
    p.psk.assign(c.psk.begin(), c.psk.end());
    p.settle_ms = static_cast<uint32_t>(c.settle_s * 1000.0);
    p.verify_timeout_ms = c.verify_timeout_ms;
    p.min_interval_ms = c.min_interval_s * 1000;
    p.ack_timeout_ms = c.ack_timeout_ms;
    p.rendezvous_timeout_ms = c.rendezvous_timeout_s * 1000;
    p.home_chan = c.home_chan;
    p.allowlist = c.channel_allowlist;
    return p;
}

// §7.2: the pacer keys off END_OF_BLOCK frames in both directions.
bool frame_is_eob(const uint8_t* f, size_t n) {
    const Decoded dec = decode(f, n);
    const DataView* v = std::get_if<DataView>(&dec);
    return v != nullptr && (v->hdr.data_flags & data_flags::kEndOfBlock) != 0;
}

// §2: random per-boot session nonce.
uint32_t session_nonce() {
    uint32_t nonce = 0;
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        const size_t got = std::fread(&nonce, 1, sizeof(nonce), f);
        std::fclose(f);
        if (got == sizeof(nonce) && nonce != 0) {
            return nonce;
        }
    }
    return static_cast<uint32_t>(now_ms()) | 1u;  // degraded fallback
}

SchedulerPolicy scheduler_policy(const Config& cfg) {
    SchedulerPolicy p;
    p.holddown_ms = cfg.policy.arq.holddown_ms;
    p.attempt_cap = cfg.policy.arq.attempt_cap;
    p.airtime_frac = cfg.policy.arq.airtime_frac;
    p.preferred_originator = cfg.node.preferred_originator;
    p.release_timeout_ms = cfg.policy.arq.release_timeout_ms;
    p.min_recoverable_ms = cfg.policy.arq.min_recoverable_ms;
    p.interval_ms = cfg.policy.arq.budget_interval_ms;
    p.max_block_pkts = cfg.policy.arq.max_block_pkts;
    p.budget_floor_bytes = cfg.policy.arq.budget_floor_bytes;
    return p;
}

RxPolicy rx_policy(const Config& cfg) {
    RxPolicy p;
    p.fwd_clamp_pkts = cfg.policy.rx.fwd_clamp_pkts;
    p.fwd_clamp_blocks = cfg.policy.arq.fwd_clamp_blocks;
    p.stall_timeout_ms = cfg.policy.rx.stall_timeout_ms;
    p.dwell_ceiling_ms = cfg.policy.rx.dwell_ceiling_ms;
    p.admit_n = cfg.policy.rx.admit_n;
    p.admit_window_ms = cfg.policy.rx.admit_window_ms;
    p.renack_attempts = cfg.policy.rx.renack_attempts;
    p.renack_backoff_ms = cfg.policy.rx.renack_backoff_ms;
    p.idle_teardown_ms = cfg.policy.rx.idle_teardown_ms;
    p.clamp_resync_ms = cfg.policy.rx.clamp_resync_ms;
    return p;
}

uint32_t s_to_ms(double s) {
    return s <= 0.0 ? 0u : static_cast<uint32_t>(s * 1000.0 + 0.5);
}

SelectorPolicy selector_policy(const Config& cfg) {
    const SelectPolicy& s = cfg.policy.select;
    SelectorPolicy p;
    p.demote_milli = s.demote_milli;
    p.rssi_floor_dbm = s.rssi_floor_dbm;
    p.rssi_fade_db_per_s = s.rssi_fade_db_per_s;
    p.rssi_fade_arm_dbm = s.rssi_fade_arm_dbm;
    p.down_cooldown_ms = s_to_ms(s.down_cooldown_s);
    p.ewma_alpha = s.ewma_alpha;
    p.rung_rssi_floor_dbm = s.rung_rssi_floor_dbm;
    p.promote_rssi_hyst_db = s.promote_rssi_hyst_db;
    p.promote_dwell_ms = s_to_ms(s.promote_dwell_s);
    p.bitrate_lead_ms = s_to_ms(s.bitrate_lead_s);
    p.mcs_up_grace_ms = s_to_ms(s.mcs_up_grace_s);
    p.mcs_settle_ms = s_to_ms(s.mcs_settle_s);
    p.reentry_backoff_ms = s_to_ms(s.reentry_backoff_s);
    p.reentry_dwell_ms = s_to_ms(s.reentry_dwell_s);
    p.flap_freeze_count = s.flap_freeze_count;
    p.flap_freeze_window_ms = s_to_ms(s.flap_freeze_window_s);
    p.flap_freeze_ms = s_to_ms(s.flap_freeze_s);
    p.min_profile = s.min_profile;
    p.max_profile = s.max_profile;
    p.report_timeout_ms = cfg.policy.report_timeout_ms;
    p.failsafe_hold_ms = s_to_ms(s.failsafe_hold_s);
    p.failsafe_step_ms = s_to_ms(s.failsafe_step_s);
    p.pressure_escape_ms = s_to_ms(s.pressure_escape_s);
    return p;
}

QuietGapPolicy quietgap_policy(const Config& cfg) {
    QuietGapPolicy p;
    p.enabled = cfg.policy.ret.quiet_gap;
    p.guard_us = cfg.policy.ret.guard_us;
    p.window_us = cfg.policy.ret.return_window_us;
    return p;
}

// ---- air backend selection (udp dev backend | devourer radio, §3.0) --------

struct AirBackend {
    std::optional<UdpAir> udp;
    std::optional<MonAir> mon;
#if WBLINK_RADIO
    std::optional<RadioAir> radio;
#endif
    uint64_t last_tx_ms = 0;

    static Result<AirBackend> create(const Config& cfg) {
        AirBackend b;
        if (cfg.air.kind == AirCfg::Kind::kUdp ||
            cfg.air.kind == AirCfg::Kind::kUdpBroadcast) {
            AirUdpCfg uc = cfg.air.udp;
            uc.rx_drop_permille = cfg.air.rx_drop_permille;  // bench synthetic loss
            uc.broadcast = cfg.air.kind == AirCfg::Kind::kUdpBroadcast;
            uc.originator = cfg.node.originator;
            auto a = UdpAir::create(uc);
            if (!a) {
                return Result<AirBackend>::fail(a.error);
            }
            b.udp.emplace(std::move(*a.value));
            return Result<AirBackend>::ok(std::move(b));
        }
        if (cfg.air.kind == AirCfg::Kind::kMonitor) {
            MonAirCfg mc;
            mc.adapters = cfg.adapters;
            mc.stamp_net_id = cfg.node.net_id.value_or(0);
            mc.filter_net_id = cfg.node.net_id;
            mc.originator = cfg.node.originator;
            mc.rx_drop_permille = cfg.air.rx_drop_permille;
            auto a = MonAir::create(mc);
            if (!a) {
                return Result<AirBackend>::fail(a.error);
            }
            b.mon.emplace(std::move(*a.value));
            return Result<AirBackend>::ok(std::move(b));
        }
        if (cfg.air.kind == AirCfg::Kind::kRadio) {
#if WBLINK_RADIO
            RadioAirCfg rc;
            rc.adapters = cfg.adapters;
            rc.stamp_net_id = cfg.node.net_id.value_or(0);
            rc.filter_net_id = cfg.node.net_id;
            rc.originator = cfg.node.originator;
            rc.rx_drop_permille = cfg.air.rx_drop_permille;
            // §3.0 Pass 12 hardware-ACK hybrid halves.
            rc.ack_responder = cfg.air.ack_responder;
            rc.unicast_returns = cfg.policy.ret.unicast;
            auto a = RadioAir::create(rc);
            if (!a) {
                return Result<AirBackend>::fail(a.error);
            }
            b.radio.emplace(std::move(*a.value));
            return Result<AirBackend>::ok(std::move(b));
#else
            return Result<AirBackend>::fail(
                "air: kind \"radio\" but this binary was built with "
                "WBLINK_RADIO=OFF");
#endif
        }
        return Result<AirBackend>::fail(
            "no air backend configured (add an \"air\" section)");
    }

    size_t inject(const uint8_t* f, size_t n) {
        size_t sent = 0;
        if (mon) {
            sent = mon->inject(f, n);
        } else {
#if WBLINK_RADIO
            if (radio) {
                sent = radio->inject(f, n);
            } else
#endif
            {
                sent = udp->inject(f, n);
            }
        }
        if (sent > 0) {
            last_tx_ms = now_ms();
        }
        return sent;
    }

    size_t inject_resend(const uint8_t* f, size_t n) {
        size_t sent = 0;
        if (mon) {
            sent = mon->inject(f, n);
        } else {
#if WBLINK_RADIO
            if (radio) {
                sent = radio->inject(f, n);
            } else
#endif
            {
                sent = udp->inject_resend(f, n);
            }
        }
        if (sent > 0) last_tx_ms = now_ms();
        return sent;
    }

    std::vector<int> wait_fds() const {
        if (mon) return {mon->wait_fd()};
#if WBLINK_RADIO
        if (radio) return {radio->wait_fd()};
#endif
        return udp->wait_fds();
    }

    // Returns (NACK/LINK_REPORT) carry their target so the radio backend
    // can address them as §3.0 unicast when return.unicast is on; the udp
    // dev backend has no L2 addressing and ignores the target.
    size_t inject_return(uint16_t target, const uint8_t* f, size_t n) {
        size_t sent = 0;
        if (mon) {
            sent = mon->inject_return(target, f, n);
        } else {
#if WBLINK_RADIO
            if (radio) {
                sent = radio->inject_return(target, f, n);
            } else
#endif
            {
                (void)target;
                sent = udp->inject(f, n);
            }
        }
        if (sent > 0) {
            last_tx_ms = now_ms();
        }
        return sent;
    }

    void heartbeat(uint16_t originator, uint32_t session, uint64_t now) {
        if (now < last_tx_ms || now - last_tx_ms < 1000) {
            return;
        }
        Heartbeat hb;
        hb.prefix.originator = originator;
        hb.prefix.session_id = session;
        uint8_t frame[kHeartbeatSize];
        if (encode_heartbeat(hb, frame, sizeof(frame)) == sizeof(frame)) {
            inject(frame, sizeof(frame));
        }
    }

    int poll_once(int timeout_ms, const UdpAir::RxCb& cb) {
        if (mon) {
            return mon->poll_once(timeout_ms, cb);
        }
#if WBLINK_RADIO
        if (radio) {
            return radio->poll_once(timeout_ms, cb);
        }
#endif
        return udp->poll_once(timeout_ms, cb);
    }

    void set_packet_trace(PacketEventTrace* trace) {
        if (!udp || trace == nullptr || !trace->enabled()) return;
        udp->set_trace([trace](const char* direction, const char* outcome,
                              int adapter, const uint8_t* frame, size_t len) {
            trace->packet(direction, outcome, adapter, frame, len);
        });
    }

    size_t rx_adapters() const {
        if (mon) {
            return mon->rx_adapters();
        }
#if WBLINK_RADIO
        if (radio) {
            return radio->rx_adapters();
        }
#endif
        return udp->rx_adapters();
    }

    // Radio-only surfaces (no-ops / nullopt on the udp dev backend).
    void set_tx_mode(uint8_t mcs, bool sgi) {
        if (mon) {
            mon->set_tx_mode(mcs, sgi);
            return;
        }
#if WBLINK_RADIO
        if (radio) {
            radio->set_tx_mode(mcs, sgi);
        }
#else
        (void)mcs;
        (void)sgi;
#endif
    }
    void set_power_qdb(size_t adapter, int32_t qdb) {
        if (mon) {
            mon->set_power_qdb(adapter, qdb);
            return;
        }
#if WBLINK_RADIO
        if (radio) {
            radio->set_power_qdb(adapter, qdb);
        }
#else
        (void)adapter;
        (void)qdb;
#endif
    }
    std::optional<uint64_t> read_tsf(uint8_t adapter) {
        if (mon) {
            return mon->read_tsf(adapter);
        }
#if WBLINK_RADIO
        if (radio) {
            return radio->read_tsf(adapter);
        }
#else
        (void)adapter;
#endif
        return std::nullopt;
    }
    // §11.6 intra-process atomic switch: every local adapter retunes at
    // T_switch (a straggler follows because a sibling heard the CSA). On the
    // udp dev backend the retune is a logged intent — the CSA state machines
    // stay exercisable end-to-end without radios.
    void retune_all(uint16_t chan_mhz, uint8_t bw, bool fast) {
        if (mon) {
            for (size_t i = 0; i < mon->rx_adapters(); ++i) {
                mon->retune(i, chan_mhz, bw, fast);
                mon->reapply_tx_power(i);
            }
            return;
        }
#if WBLINK_RADIO
        if (radio) {
            for (size_t i = 0; i < radio->rx_adapters(); ++i) {
                if (!radio->retune(i, chan_mhz, bw, fast)) {
                    std::fprintf(stderr, "csa: adapter %zu retune to %u MHz "
                                         "failed\n",
                                 i, chan_mhz);
                }
                radio->reapply_tx_power(i);  // §11.2 post-retune TXAGC
            }
            return;
        }
#endif
        std::fprintf(stderr, "csa: retune -> %u MHz bw=%u%s (udp backend, "
                             "intent only)\n",
                     chan_mhz, bw, fast ? " fast" : "");
    }
    bool is_radio() const {
        if (mon) {
            return true;
        }
#if WBLINK_RADIO
        return radio.has_value();
#else
        return false;
#endif
    }
    bool tx_pending() const { return udp && udp->tx_pending(); }
    std::optional<uint32_t> estimate_airtime_us(size_t bytes,
                                                bool include_pending) const {
        if (!udp) return std::nullopt;
        return udp->estimate_airtime_us(bytes, include_pending);
    }
    bool supports_csa() const {
        // UDP intentionally exercises CSA state without a physical retune.
        // Kernel-monitor cannot retune yet and must fail closed: pretending a
        // switch succeeded can strand the node while its interface stays put.
        return !mon.has_value();
    }
    bool set_udp_rx_drop(int permille) {
        if (!udp || permille < 0 || permille > 1000) {
            return false;
        }
        udp->set_rx_drop_permille(static_cast<uint16_t>(permille));
        return true;
    }
    // §9.10: the watchdog runs in the mode loop (it owns the clock); its
    // verdict is grafted onto the TX adapter's stats entry here.
    void fill_adapter_stats(StatsSnapshot& snap, uint64_t tsf_fallbacks,
                            bool tx_wedged) const {
        if (udp) {
            const size_t n = udp->rx_adapters();
            for (size_t i = 0; i < n; ++i) {
                AdapterStats as;
                as.name = "udp" + std::to_string(i);
                as.rx = udp->rx_frames(i);
                as.filtered = udp->rx_filtered(i);
                as.drop = udp->rx_dropped(i);
                as.kernel_drop = udp->kernel_dropped(i);
                if (i == 0) {
                    as.tx_submitted = udp->tx_submitted();
                    as.tx_failed = udp->tx_failed();
                }
                snap.adapters.push_back(std::move(as));
            }
            // A TX-only UDP node has no RX adapter entry to carry its aggregate.
            if (n == 0 && (udp->tx_submitted() != 0 || udp->tx_failed() != 0)) {
                AdapterStats as;
                as.name = "udp-tx";
                as.tx_submitted = udp->tx_submitted();
                as.tx_failed = udp->tx_failed();
                snap.adapters.push_back(std::move(as));
            }
            (void)tsf_fallbacks;
            (void)tx_wedged;
            return;
        }
        if (mon) {
            for (size_t i = 0; i < mon->rx_adapters(); ++i) {
                const auto c = mon->counters(i);
                AdapterStats as;
                as.name = c.name;
                as.rx = c.rx_frames;
                as.rssi_best = c.rssi_last;
                as.rssi_mean = c.rssi_last;
                as.tx_submitted = c.tx_submitted;
                as.tx_failed = c.tx_failed;
                as.drop = c.rx_dropped;
                as.filtered = c.rx_filtered;
                as.kernel_drop = c.kernel_dropped;
                as.tsf_fallback = (i == 0) ? tsf_fallbacks : 0;
                // No CCX tx.report on monitor injection; wedge watchdog off.
                as.tx_reports = 0;
                as.tx_report_fails = 0;
                as.tx_wedged = false;
                snap.adapters.push_back(std::move(as));
            }
            (void)tx_wedged;
            return;
        }
#if WBLINK_RADIO
        if (!radio) {
            return;
        }
        for (size_t i = 0; i < radio->rx_adapters(); ++i) {
            const auto c = radio->counters(i);
            AdapterStats as;
            as.name = c.name;
            as.rx = c.rx_frames;
            as.rssi_best = c.rssi_last;
            as.rssi_mean = c.rssi_last;
            as.tx_submitted = c.tx_submitted;
            as.tx_failed = c.tx_failed;
            as.drop = c.rx_dropped;
            // Node-wide §7.2 TSF-read fallback count, surfaced once.
            as.tsf_fallback = (i == 0) ? tsf_fallbacks : 0;
            as.tx_reports = c.tx_reports;
            as.tx_report_fails = c.tx_report_fails;
            as.tx_wedged = c.tx && tx_wedged;
            snap.adapters.push_back(std::move(as));
        }
#else
        (void)snap;
        (void)tsf_fallbacks;
        (void)tx_wedged;
#endif
    }

    // §15.3 return-block unicast counters (radio backend; no-op on udp).
    void fill_return_stats(ReturnStats& ret) const {
        if (mon) {
            mon->return_counters(ret.unicast_sent, ret.unicast_fallback);
            return;
        }
#if WBLINK_RADIO
        if (radio) {
            radio->return_counters(ret.unicast_sent, ret.unicast_fallback);
        }
#else
        (void)ret;
#endif
    }

    // TX adapter's cumulative (tx_submitted, tx_reports) for the §9.10
    // watchdog; nullopt on the udp dev backend (no CCX reports to watch).
    std::optional<std::pair<uint64_t, uint64_t>> tx_report_counters() const {
#if WBLINK_RADIO
        if (radio) {
            uint64_t s = 0;
            uint64_t r = 0;
            radio->tx_report_counters(s, r);
            return std::make_pair(s, r);
        }
#endif
        return std::nullopt;
    }
};

// ---- TX side: per in-stream framer + ring + scheduler ----------------------

struct TxCore {
    using Inject = std::function<void(const uint8_t*, size_t)>;

    // A stream carries EITHER a UDP-datagram Framer (§5.1) or a whole-frame
    // FrameFramer (§5.1a, frame-shm ingress) — never both. Exactly one of the
    // two optionals is engaged per the ingress binding kind.
    struct Stream {
        uint8_t stream_id;
        uint8_t stream_type;
        std::optional<Framer> framer;             // udp ingress
        std::optional<FrameFramer> frame_framer;  // frame-shm ingress
        std::optional<JsccRuntimeShadow> jscc_shadow;
        ResendRing ring;
        ResendScheduler sched;
        JsccShadowResult jscc_latest;
        uint64_t jscc_decision_frames = 0;
        uint64_t jscc_valid_decisions = 0;
        uint64_t jscc_fallback_decisions = 0;
        // §14.2 enforcement (Pass 38).
        bool jscc_enforce = false;
        uint64_t jscc_enforced_frames = 0;
        uint64_t jscc_discarded_frames = 0;
    };

    TxCore(const Config& cfg, uint32_t session, const ProfileTable* table,
           uint8_t table_version)
        : originator_(cfg.node.originator),
          preferred_originator_(cfg.node.preferred_originator),
          session_(session),
          table_version_(table_version),
          table_(table),
          selector_(selector_policy(cfg), table),
          venc_(cfg.venc),
          venc_knobs_(cfg.venc),
          arq_max_fps_(cfg.policy.arq.arq_max_fps),
          report_gate_(ReportGatePolicy{
              cfg.node.preferred_originator,
              cfg.policy.report_timeout_ms * 4}) {
        // §9.11 FPS ladder (Pass 39) — config validated it requires venc.
        if (cfg.venc.enabled && cfg.venc.fps_ladder.enabled) {
            const FpsLadderCfg& lc = cfg.venc.fps_ladder;
            FpsLadderPolicy fp;
            fp.min_fps = lc.min;
            fp.preferred_fps = lc.preferred;
            fp.distress_milli = lc.distress_milli;
            fp.restore_milli = lc.restore_milli;
            fp.reduce_after_ms = lc.reduce_after_ms;
            fp.reduce_dwell_ms = lc.reduce_dwell_ms;
            fp.restore_after_ms = lc.restore_after_ms;
            fp.settle_ms = lc.settle_ms;
            fp.report_timeout_ms = cfg.policy.report_timeout_ms;
            fps_ladder_.emplace(fp);
        }
        // §10: one power curve per TX adapter with an authored map. The
        // resolve happens at profile commit; the radio backend applies it
        // (apply_power hook), otherwise it stays a logged intent.
        for (size_t i = 0; i < cfg.adapters.size(); ++i) {
            const AdapterCfg& a = cfg.adapters[i];
            if (a.role != Role::kTx || a.power_map.empty()) {
                continue;
            }
            auto curve =
                load_power_curve(a.power_map, a.channel_mhz >= 4000);
            if (!curve) {
                std::fprintf(stderr, "power: %s: %s\n", a.name.c_str(),
                             curve.error.c_str());
                continue;
            }
            power_.push_back(PowerAdapter{a.name, i, *curve.value,
                                          a.max_power_qdb, std::nullopt});
        }
        for (const StreamCfg& s : cfg.streams) {
            if (s.dir != Dir::kIn) {
                continue;
            }
            RingConfig rc;
            rc.window_ms = cfg.policy.arq.ring_window_ms;
            rc.byte_budget = cfg.policy.arq.ring_byte_budget;
            Stream st{s.stream_id, s.stream_type, std::nullopt, std::nullopt,
                      std::nullopt, ResendRing(rc),
                      ResendScheduler(scheduler_policy(cfg), table), {}, 0, 0, 0};
            if (s.bind.kind == BindKind::kFrameShm) {
                // §5.1a: whole-frame ingress from a venc SHM ring. FEC policy
                // comes from the stream's fec block; MTU from the floor rung.
                FrameFramerConfig fc;
                fc.originator = cfg.node.originator;
                fc.session_id = session;
                fc.stream_id = s.stream_id;
                fc.stream_type = s.stream_type;
                fc.arq_mode = s.arq_mode;
                fc.fec.scheme = s.fec.scheme;
                fc.fec.i_rate_permille = s.fec.i_rate_permille;
                fc.fec.p_rate_permille = s.fec.p_rate_permille;
                fc.fec.min_k = s.fec.min_k;
                st.frame_framer.emplace(fc);
                st.frame_framer->set_operating_point(0, table_version,
                                                     max_payload_for(0));
                if (s.jscc_shadow) {
                    const JsccShadowCfg& jc = *s.jscc_shadow;
                    st.jscc_shadow.emplace(JsccRuntimeShadowConfig{
                        jc.fec_floor_permille, jc.fec_cap_permille,
                        jc.arq_guard_us, jc.feedback_timeout_ms,
                        jc.min_rtt_samples});
                    st.jscc_enforce = jc.enforce;  // §14.2 Pass 38
                }
            } else {
                FramerConfig fc;
                fc.originator = cfg.node.originator;
                fc.session_id = session;
                fc.stream_id = s.stream_id;
                fc.stream_type = s.stream_type;
                fc.classifier = s.classifier;
                fc.classifier_size_threshold =
                    cfg.policy.arq.classifier_size_threshold;
                st.framer.emplace(fc);
                st.framer->set_operating_point(0, table_version);
            }
            streams_.push_back(std::move(st));
        }
    }

    // §5.1a MTU budget: the active rung's max_payload (profile 0 = floor at
    // startup), or the standard-rung default when no table is loaded.
    uint16_t max_payload_for(uint8_t profile_id) const {
        if (table_ != nullptr) {
            for (const Profile& p : table_->profiles) {
                if (p.id == profile_id) {
                    return p.max_payload;
                }
            }
        }
        return kDefaultMaxPayload;
    }

    uint32_t frame_deadline_us(bool is_idr) const {
        if (table_ == nullptr) return 0;
        const uint8_t active = selector_.profile_id();
        for (const Profile& p : table_->profiles) {
            if (p.id != active) continue;
            const uint16_t ms = is_idr ? p.arq_deadline_iframe_ms
                                       : p.arq_deadline_pframe_ms;
            return static_cast<uint32_t>(ms) * 1000u;
        }
        return 0;
    }

    void on_ingress(uint8_t stream_id, const uint8_t* d, size_t n,
                    uint64_t now, const Inject& inject) {
        for (Stream& s : streams_) {
            if (s.stream_id != stream_id || !s.framer) {
                continue;
            }
            s.framer->on_datagram(
                d, n, now,
                [&](const uint8_t* frame, size_t len, const DataHeader& hdr,
                    uint64_t t) {
                    inject(frame, len);
                    s.ring.push(frame, len, hdr, t);
                    s.sched.note_live_bytes(len);
                });
            return;
        }
    }

    // §5.1a frame-shm ingress: one whole [VencFrameMeta][Annex-B] blob is
    // fragmented + FEC'd by the stream's FrameFramer. Same emit contract as
    // on_ingress (inject + ring + scheduler bookkeeping).
    void on_frame(uint8_t stream_id, const uint8_t* blob, size_t len,
                  uint64_t now, const Inject& inject) {
        for (Stream& s : streams_) {
            if (s.stream_id != stream_id || !s.frame_framer) {
                continue;
            }
            // §9.6 cadence: windowed frame count (the ring's last-gap
            // interval collapses under batch drains and cannot be used).
            if (cadence_start_ms_ == 0) {
                cadence_start_ms_ = now;
            }
            ++cadence_frames_;
            if (s.jscc_shadow && blob != nullptr && len >= kVencFrameMetaSize) {
                const uint16_t symbol = s.frame_framer->symbol_size();
                const size_t k_sz = std::max<size_t>(1, (len + symbol - 1) / symbol);
                const uint16_t k = static_cast<uint16_t>(
                    std::min<size_t>(k_sz, UINT16_MAX));
                VencFrameMeta meta;
                read_frame_meta(blob, len, &meta);
                const bool idr = (meta.flags & kFrameFlagIdr) != 0;
                const size_t source_bytes =
                    len + static_cast<size_t>(k) *
                              (kDataHeaderSize + kFecSourceSubheaderSize);
                const size_t resend_bytes =
                    kDataHeaderSize + kFecSourceSubheaderSize + symbol;
                JsccShadowFrameInput input;
                input.source_k = k;
                input.deadline_us = frame_deadline_us(idr);
                input.arq_capable =
                    (idr ||
                     s.frame_framer->arq_mode() == FrameArqMode::kAllFrames) &&
                    !arq_fps_suppressed_;  // §4.1 Pass 40 cutoff
                input.now_ms = now;
                if (estimate_airtime) {
                    input.source_tx_remaining_us =
                        estimate_airtime(source_bytes, true);
                    input.resend_airtime_us =
                        estimate_airtime(resend_bytes, false);
                }
                s.jscc_latest = s.jscc_shadow->evaluate(input);
                ++s.jscc_decision_frames;
                if (s.jscc_latest.valid) {
                    ++s.jscc_valid_decisions;
                } else {
                    ++s.jscc_fallback_decisions;
                }
                // §14.2 enforcement (Pass 38): a VALID decision actuates for
                // this one frame; any fallback keeps the fixed §14.1 path.
                if (s.jscc_enforce && s.jscc_latest.valid) {
                    if (s.jscc_latest.decision.discard) {
                        ++s.jscc_discarded_frames;  // rule 2: drop, not queue
                        return;
                    }
                    s.frame_framer->set_next_frame_override(
                        s.jscc_latest.decision.parity_symbols,
                        s.jscc_latest.decision.arq_eligible);
                    ++s.jscc_enforced_frames;
                }
            }
            s.frame_framer->on_frame(
                blob, len, now,
                [&](const uint8_t* frame, size_t flen, const DataHeader& hdr,
                    uint64_t t) {
                    inject(frame, flen);
                    s.ring.push(frame, flen, hdr, t);
                    s.sched.note_live_bytes(flen);
                });
            return;
        }
    }

    // Air packets heard back (uplink): NACKs feed the scheduler, LINK_REPORTs
    // feed the §9 selector.
    void on_air(const uint8_t* d, size_t n, uint64_t now) {
        const Decoded dec = decode(d, n);
        if (const JsccFeedback* f = std::get_if<JsccFeedback>(&dec)) {
            if (f->target_originator != originator_ ||
                f->target_session != session_ ||
                (preferred_originator_ != 0 &&
                 f->prefix.originator != preferred_originator_)) {
                return;
            }
            for (Stream& s : streams_) {
                if (s.stream_id == f->target_stream_id && s.jscc_shadow) {
                    s.jscc_shadow->observe_feedback(*f, now);
                    return;
                }
            }
            return;
        }
        if (const NackView* nack = std::get_if<NackView>(&dec)) {
            if (nack->hdr.target_originator != originator_ ||
                nack->hdr.target_session != session_) {
                return;
            }
            for (Stream& s : streams_) {
                if (s.stream_id == nack->hdr.target_stream_id) {
                    s.sched.on_nack(*nack, s.ring, now);
                    return;
                }
            }
            return;
        }
        if (const LinkReport* r = std::get_if<LinkReport>(&dec)) {
            if (r->target_originator != originator_ ||
                r->target_session != session_) {
                return;
            }
            // §3.5 acceptance filter (Pass 41): preferred/latched reporters
            // only — BEFORE the selector and the §9.11 ladder consume it.
            if (!report_gate_.accept(r->prefix.originator,
                                     r->prefix.session_id, now)) {
                return;
            }
            ++reports_received_;
            selector_.on_report(*r, now);
            if (fps_ladder_) {  // §9.11 distress/restore evidence
                fps_ladder_->note_report(r->loss_postdiv_prearq, now);
            }
            return;
        }
        if (const RecoveryRequest* r = std::get_if<RecoveryRequest>(&dec)) {
            if (r->target_originator != originator_ ||
                r->target_session != session_) {
                return;
            }
            for (const Stream& s : streams_) {
                if (s.stream_id == r->target_stream_id &&
                    s.stream_type == stream_type::kRtp) {
                    const bool ok = venc_.request_idr(now);
                    std::fprintf(stderr,
                                 "venc: decoder recovery stream=%u requester=%u %s\n",
                                 r->target_stream_id, r->prefix.originator,
                                 ok ? "accepted" : "failed");
                    return;
                }
            }
        }
    }

    void tick(uint64_t now, const Inject& inject,
              const Inject& inject_resend = {}) {
        const SelectorActions act = selector_.tick(now);
        if (act.commit) {
            // §9.5 commit: the operating point stamped on every DATA packet
            // (drives RX deadlines + supersession budgets)...
            const uint16_t mp = max_payload_for(act.commit->profile_id);
            for (Stream& s : streams_) {
                if (s.framer) {
                    s.framer->set_operating_point(act.commit->profile_id,
                                                  table_version_);
                }
                if (s.frame_framer) {
                    s.frame_framer->set_operating_point(act.commit->profile_id,
                                                        table_version_, mp);
                }
            }
            // ...the TX adapter's modulation default (§10.4, radio backend)...
            if (apply_mode) {
                apply_mode(act.commit->mcs,
                           act.commit->gi == GuardInterval::kShort);
            }
            // ...and the §10 per-adapter power resolve, applied inside the
            // same sequenced transition (real SetTxPowerOffsetQdb on the
            // radio backend; a logged intent elsewhere).
            for (PowerAdapter& pa : power_) {
                const auto qdb =
                    resolve_power_qdb(pa.curve, act.commit->mcs,
                                      act.commit->tx_power_level, pa.ceiling);
                if (qdb && (!pa.applied_qdb || *pa.applied_qdb != *qdb)) {
                    pa.applied_qdb = *qdb;
                    if (apply_power) {
                        apply_power(pa.adapter_idx, *qdb);
                    } else {
                        std::fprintf(stderr,
                                     "power: %s mcs=%u level=%u -> %d qdb\n",
                                     pa.name.c_str(), act.commit->mcs,
                                     act.commit->tx_power_level, *qdb);
                    }
                }
            }
        }
        // Push the CURRENT target every tick: write-on-change (§9.6) makes
        // this a no-op normally, and a failed push (encoder briefly down)
        // retries next tick instead of waiting for the next rung change.
        if (selector_.bitrate_kbps() > 0) {
            venc_.set_bitrate(selector_.bitrate_kbps(), now);
        }
        // §9.6 cadence estimate: frames over a ~1 s window.
        if (cadence_start_ms_ != 0 && now >= cadence_start_ms_ + 1000) {
            frame_cadence_us_ = cadence_frames_ > 0
                ? (now - cadence_start_ms_) * 1000ull / cadence_frames_
                : 0;
            cadence_frames_ = 0;
            cadence_start_ms_ = now;
        }
        // §9.11 FPS ladder: reduce on radio-loop exhaustion, restore slowly.
        if (fps_ladder_) {
            const bool at_floor = table_ != nullptr &&
                                  selector_.profile_id() ==
                                      table_->floor_profile;
            if (const auto f = fps_ladder_->tick(now, at_floor)) {
                venc_.set_fps(*f, now);
            }
        }
        // §4.1 Pass 40 high-cadence ARQ cutoff, driven by the same cadence
        // input the §9.6 caps use (ladder-commanded, else measured, else
        // hint). Sticky on the framers until the cadence drops back.
        {
            const uint32_t snapped = snap_frame_period_us(
                fps_ladder_ && fps_ladder_->current_fps() > 0
                    ? 1000000ull / fps_ladder_->current_fps()
                    : (frame_cadence_us_ != 0
                           ? frame_cadence_us_
                           : 1000000ull / venc_knobs_.fps_hint));
            arq_fps_suppressed_ = arq_max_fps_ != 0 && snapped != 0 &&
                                  snapped < 1000000u / arq_max_fps_;
            for (Stream& s : streams_) {
                if (s.frame_framer) {
                    s.frame_framer->set_arq_suppressed(arq_fps_suppressed_);
                }
            }
        }
        // §9.6 Pass 37 horizon caps: recomputed from slow inputs (rung
        // budget, ladder-snapped cadence, I deadline, live §14.1 rates);
        // write-on-change makes the steady state a no-op.
        if (venc_.frame_caps_enabled() && selector_.bitrate_kbps() > 0) {
            for (Stream& s : streams_) {
                if (!s.frame_framer) {
                    continue;  // caps apply to frame-shm ingress only (§9.6)
                }
                FrameCapInputs in;
                in.budget_kbps = selector_.bitrate_kbps();
                // §9.11: while the ladder runs, the COMMANDED fps is the
                // authoritative cadence — measurement lags a change by ~1 s.
                in.frame_period_us = snap_frame_period_us(
                    fps_ladder_ && fps_ladder_->current_fps() > 0
                        ? 1000000ull / fps_ladder_->current_fps()
                        : (frame_cadence_us_ != 0
                               ? frame_cadence_us_
                               : 1000000ull / venc_knobs_.fps_hint));
                in.iframe_deadline_ms = static_cast<uint16_t>(
                    frame_deadline_us(true) / 1000u);
                const FrameFecConfig& fec = s.frame_framer->fec();
                if (fec.scheme != FecScheme::kNone) {
                    in.i_rate_permille = fec.i_rate_permille;
                    in.p_rate_permille = fec.p_rate_permille;
                }
                in.symbol_size = static_cast<uint16_t>(
                    max_payload_for(selector_.profile_id()) -
                    kDataHeaderSize - kFecRepairSubheaderSize);
                in.ceiling_bytes = venc_knobs_.cap_ceiling_bytes;
                in.i_headroom_permille = venc_knobs_.i_headroom_permille;
                in.p_headroom_permille = venc_knobs_.p_headroom_permille;
                const FrameCaps caps = derive_frame_caps(in);
                venc_.set_max_frame_size(caps.max_i_bytes, caps.max_p_bytes,
                                         now);
                break;  // single video stream (§9.6)
            }
        }
        for (Stream& s : streams_) {
            s.ring.evict(now);
            s.sched.drain(s.ring, now, [&](const uint8_t* f, size_t l) {
                (inject_resend ? inject_resend : inject)(f, l);
            });
        }
    }

    void set_pressure(bool on, uint64_t now) {  // §9.9 gauge (step 9+ feeds it)
        selector_.set_pressure(on, now);
    }


    // §11.3: freeze the cascade + pause the watchdog across the CSA blackout.
    void csa_freeze(uint64_t until_ms) { selector_.csa_freeze(until_ms); }
    // §11.6: CSA_ARMED on every outgoing DATA frame while the campaign holds.
    void set_csa_armed(bool on) {
        const uint8_t f = on ? data_flags::kCsaArmed : 0;
        for (Stream& s : streams_) {
            if (s.framer) {
                s.framer->set_extra_flags(f);
            }
            if (s.frame_framer) {
                s.frame_framer->set_extra_flags(f);
            }
        }
    }
    uint8_t power_level() const { return selector_.tx_power_level(); }

    // §15.5 control-plane knobs -------------------------------------------
    // §9.7 profile pin: clamp the operating-point ladder to [min,max] by id.
    void set_profile_pin(uint8_t min_profile, uint8_t max_profile) {
        selector_.set_profile_pin(min_profile, max_profile);
    }
    // §14.1 live FEC-rate retune for a frame-shm stream. Returns false if the
    // stream_id is unknown or is not a frame-shm (FrameFramer) stream.
    bool set_stream_fec(uint8_t stream_id, uint16_t i_permille,
                        uint16_t p_permille, uint16_t min_k) {
        for (Stream& s : streams_) {
            if (s.stream_id != stream_id) {
                continue;
            }
            if (!s.frame_framer) {
                return false;  // udp stream: no per-stream FEC (§15.2)
            }
            s.frame_framer->set_fec_rates(i_permille, p_permille, min_k);
            return true;
        }
        return false;
    }
    void reset_stats() {
        for (Stream& s : streams_) {
            if (s.framer) {
                s.framer->reset_stats();
            }
            if (s.frame_framer) {
                s.frame_framer->reset_stats();
            }
            if (s.jscc_shadow) {
                s.jscc_shadow->reset();
                s.jscc_latest = {};
                s.jscc_decision_frames = 0;
                s.jscc_valid_decisions = 0;
                s.jscc_fallback_decisions = 0;
            }
            s.sched.reset_counters();
        }
        reports_received_ = 0;
    }

    void fill_stats(StatsSnapshot& snap, uint64_t now) const {
        for (const Stream& s : streams_) {
            StreamStats st;
            st.stream_id = s.stream_id;
            st.type = "TX";
            if (s.framer) {
                st.seq = s.framer->next_seq();
                st.delivered = s.framer->stats().frames;
                st.decode_errors = s.framer->stats().oversize_ingress;
            } else if (s.frame_framer) {
                st.seq = s.frame_framer->next_seq();
                st.delivered = s.frame_framer->stats().frames;
                st.malformed = s.frame_framer->stats().malformed_frame;
                st.source_symbols_sent =
                    s.frame_framer->stats().source_symbols;
                st.repair_symbols_sent =
                    s.frame_framer->stats().repair_symbols;
                st.fec_oversize_frames =
                    s.frame_framer->stats().fec_oversize_k;
                st.idr_frames = s.frame_framer->stats().idr_frames;
                st.arq_frames = s.frame_framer->stats().arq_frames;
                st.arq_cutoff_frames =
                    s.frame_framer->stats().arq_cutoff_frames;
            }
            if (s.jscc_shadow) {
                const JsccShadowResult& js = s.jscc_latest;
                st.jscc_decision_frames = s.jscc_decision_frames;
                st.jscc_valid_decisions = s.jscc_valid_decisions;
                st.jscc_fallback_decisions = s.jscc_fallback_decisions;
                st.jscc_decision_valid = js.valid;
                st.jscc_fallback = jscc_shadow_fallback_string(js.fallback);
                st.jscc_reason = js.valid
                    ? jscc_reason_string(js.decision.reason) : std::string();
                st.jscc_input_k = js.input.source_k;
                st.jscc_input_predicted_symbols =
                    js.input.predicted_loss_symbols;
                st.jscc_input_floor_symbols = js.input.fec_floor_symbols;
                st.jscc_input_cap_symbols = js.input.fec_cap_symbols;
                st.jscc_input_deadline_us = js.input.deadline_us;
                st.jscc_input_source_tx_us = js.input.source_tx_remaining_us;
                st.jscc_input_rtt_p95_us = js.input.rtt_p95_us;
                st.jscc_input_resend_us = js.input.resend_airtime_us;
                st.jscc_input_guard_us = js.input.arq_guard_us;
                st.jscc_output_parity_symbols = js.decision.parity_symbols;
                st.jscc_output_remaining_us =
                    js.decision.remaining_after_source_us;
                st.jscc_output_arq_eligible = js.decision.arq_eligible;
                st.jscc_output_discard = js.decision.discard;
                st.jscc_feedback_epoch = js.feedback_epoch;
                st.jscc_feedback_age_ms = js.feedback_age_ms;
                st.jscc_enforced_frames = s.jscc_enforced_frames;
                st.jscc_discarded_frames = s.jscc_discarded_frames;
            }
            st.resends_sent = s.sched.counters().resends_sent;
            st.double_send_suppressed =
                s.sched.counters().holddown_suppressed;
            st.active_profile = selector_.profile_id();
            st.table_version = table_version_;
            snap.streams.push_back(std::move(st));
        }
        snap.link.profile = selector_.profile_id();
        snap.link.mcs = selector_.mcs();
        snap.link.report_epoch = selector_.report_epoch();
        snap.link.report_age_ms =
            static_cast<uint32_t>(selector_.report_age_ms(now));
        snap.link.state = selector_.state();
        snap.link.flap_freeze = selector_.flap_frozen(now);
        // §9.6 actuator state (Pass 37).
        snap.link.venc_bitrate_kbps = venc_.commanded_bitrate_kbps();
        snap.link.venc_max_i_bytes = venc_.commanded_max_i_bytes();
        snap.link.venc_max_p_bytes = venc_.commanded_max_p_bytes();
        snap.link.venc_pushes = venc_.pushes();
        snap.link.venc_failures = venc_.failures();
        snap.link.venc_settling = venc_.settling(now);
        snap.link.venc_fps = venc_.commanded_fps();
        for (const PowerAdapter& pa : power_) {
            if (pa.applied_qdb) {
                snap.link.tx_power_qdb = *pa.applied_qdb;  // first TX adapter
                break;
            }
        }
        // §7.3 return-path visibility: the RX's epoch counter says how many
        // reports it SENT; we know how many arrived.
        snap.ret.reports_expected = selector_.report_epoch();
        snap.ret.reports_received = reports_received_;
        snap.ret.reports_rejected = report_gate_.rejected();  // §3.5 Pass 41
    }

    struct PowerAdapter {
        std::string name;
        size_t adapter_idx;  // position in cfg.adapters == radio index
        PowerCurve curve;
        std::optional<int32_t> ceiling;
        std::optional<int32_t> applied_qdb;
    };

    // Radio-backend actuation hooks (§10.4); unset = logged intent.
    std::function<void(uint8_t mcs, bool sgi)> apply_mode;
    std::function<void(size_t adapter_idx, int32_t qdb)> apply_power;
    std::function<std::optional<uint32_t>(size_t bytes, bool include_pending)>
        estimate_airtime;

    uint16_t originator_;
    uint16_t preferred_originator_ = 0;
    uint32_t session_;
    uint8_t table_version_;
    const ProfileTable* table_;
    Selector selector_;
    VencActuator venc_;
    VencCfg venc_knobs_;            // §9.6 cap knobs (Pass 37)
    std::optional<FpsLadder> fps_ladder_;  // §9.11 (Pass 39)
    uint16_t arq_max_fps_ = 100;           // §4.1 Pass 40 cutoff
    bool arq_fps_suppressed_ = false;
    ReportGate report_gate_;               // §3.5 Pass 41
    uint64_t frame_cadence_us_ = 0; // windowed ingress cadence estimate
    uint64_t cadence_start_ms_ = 0;
    uint32_t cadence_frames_ = 0;
    std::vector<PowerAdapter> power_;
    uint32_t reports_received_ = 0;
    std::vector<Stream> streams_;
};

// ---- RX side: engine + NACK encode -----------------------------------------

struct RxCore {
    // (frame, len, target_originator) — the target rides along so returns
    // can be addressed as §3.0 unicast when return.unicast is on.
    using Inject = std::function<void(const uint8_t*, size_t, uint16_t)>;

    RxCore(const Config& cfg, uint32_t session, const ProfileTable* table,
           std::optional<uint8_t> table_version)
        : originator_(cfg.node.originator),
          session_(session),
          engine_(rx_policy(cfg), wants(cfg), table, table_version),
          reporter_(ReporterPolicy{cfg.policy.report_hz > 0
                                       ? static_cast<uint32_t>(
                                             1000.0 / cfg.policy.report_hz)
                                       : 0},
                    table_version),
          feedback_period_ms_(cfg.policy.report_hz > 0
                                  ? static_cast<uint32_t>(
                                        1000.0 / cfg.policy.report_hz)
                                  : 0) {}

    static std::vector<WantSpec> wants(const Config& cfg) {
        std::vector<WantSpec> out;
        for (const StreamCfg& s : cfg.streams) {
            if (s.dir == Dir::kOut) {
                out.push_back(WantSpec{s.stream_id, s.stream_type,
                                       s.originator});
            }
        }
        return out;
    }

    void on_air(uint8_t adapter, const uint8_t* d, size_t n, uint64_t now,
                const RxEngine::Deliver& deliver, int8_t rssi = 0) {
        const Decoded dec = decode(d, n);
        if (const DataView* v = std::get_if<DataView>(&dec)) {
            engine_.on_data(adapter, *v, now, deliver, rssi);
        }
    }

    void tick(uint64_t now, const RxEngine::Deliver& deliver,
              const Inject& inject_nack) {
        engine_.tick(now, deliver);
        // §7.3: LINK_REPORTs ride the same uplink as NACKs.
        for (LinkReport r : reporter_.build(engine_, now)) {
            r.prefix.originator = originator_;
            r.prefix.destination = r.target_originator;
            r.prefix.session_id = session_;
            uint8_t frame[kLinkReportSize];
            if (encode_link_report(r, frame, sizeof(frame)) > 0) {
                inject_nack(frame, sizeof(frame), r.target_originator);
            }
        }
        for (const NackRequest& req : engine_.build_nacks(now)) {
            NackHeader hdr;
            hdr.prefix.originator = originator_;
            hdr.prefix.destination = req.target_originator;
            hdr.prefix.session_id = session_;
            hdr.target_originator = req.target_originator;
            hdr.target_session = req.target_session;
            hdr.target_stream_id = req.target_stream_id;
            hdr.base_seq = req.base_seq;
            uint8_t frame[kNackFixedSize + 255];
            const size_t n = encode_nack(
                hdr, req.bitmap.data(),
                static_cast<uint8_t>(req.bitmap.size()), frame, sizeof(frame));
            if (n > 0) {
                inject_nack(frame, n, req.target_originator);
            }
        }
    }

    void emit_jscc_feedback(
        uint64_t now,
        const std::vector<std::pair<uint8_t, JsccRepairFeedbackState>>& states,
        const Inject& inject) {
        if (feedback_period_ms_ == 0 || now < next_feedback_ms_) return;
        next_feedback_ms_ = now + feedback_period_ms_;
        for (const RxStreamInfo& info : engine_.streams()) {
            const JsccRepairFeedbackState* state = nullptr;
            for (const auto& [sid, candidate] : states) {
                if (sid == info.local_stream_id) {
                    state = &candidate;
                    break;
                }
            }
            if (state == nullptr) continue;  // not a frame-SHM egress
            JsccFeedback f;
            f.prefix = {originator_, info.key.originator, session_};
            f.target_originator = info.key.originator;
            f.target_session = info.key.session_id;
            f.target_stream_id = info.key.stream_id;
            f.feedback_epoch = ++feedback_epoch_;
            f.repair_demand_permille = state->repair_demand_permille;
            f.rtt_p95_us = info.counters.nack_rtt_p95_us;
            f.repair_samples = state->repair_samples;
            f.rtt_samples = info.counters.nack_rtt_samples;
            if (state->repair_ready) {
                f.valid_flags |= jscc_feedback_flags::kRepairReady;
            }
            if (f.rtt_samples > 0) {
                f.valid_flags |= jscc_feedback_flags::kRttReady;
            }
            f.observed_block_id = state->observed_block_id;
            uint8_t frame[kJsccFeedbackSize];
            if (encode_jscc_feedback(f, frame, sizeof(frame)) == sizeof(frame)) {
                inject(frame, sizeof(frame), f.target_originator);
            }
        }
    }

    void fill_stats(StatsSnapshot& snap) const {
        for (const RxStreamInfo& info : engine_.streams()) {
            StreamStats st;
            st.stream_id = info.local_stream_id;
            st.type = info.stream_type == stream_type::kRtp ? "RTP" : "OTHER";
            st.seq = info.counters.highest_seq;
            st.delivered = info.counters.delivered;
            st.uniq = info.counters.uniq;
            st.diversity = info.counters.diversity;
            st.loss_prediversity_milli =
                info.counters.prediv_expected == 0
                    ? 0
                    : static_cast<uint32_t>(
                          info.counters.prediv_lost * 1000 /
                          info.counters.prediv_expected);
            const uint64_t denom =
                info.counters.uniq + info.counters.lost_declared;
            st.loss_postdiv_prearq_milli =
                denom == 0 ? 0
                           : static_cast<uint32_t>(
                                 info.counters.lost_declared * 1000 / denom);
            st.recovered_arq = info.counters.recovered_arq;
            st.dropped_superseded = info.counters.dropped_superseded;
            st.dropped_deadline = info.counters.dropped_deadline;
            st.nacks_sent = info.counters.nacks_sent;
            st.nack_rtt_hist = info.counters.nack_rtt_hist;
            st.nack_rtt_max_ms = info.counters.nack_rtt_max_ms;
            st.nack_rtt_samples = info.counters.nack_rtt_samples;
            st.nack_rtt_p95_us = info.counters.nack_rtt_p95_us;
            st.arq_rec_hist = info.counters.arq_rec_hist;
            st.arq_rec_max_ms = info.counters.arq_rec_max_ms;
            st.active_profile = info.active_profile;
            snap.streams.push_back(std::move(st));
        }
        for (const auto& [id, a] : engine_.adapters()) {
            // Radio backend: the air layer already emitted this adapter's
            // counters (same index order) — graft the §6 RX-liveness stall
            // onto that entry instead of duplicating it as "vadapterN".
            if (id < snap.adapters.size()) {
                snap.adapters[id].adapter_stalled = a.stalled;
                continue;
            }
            AdapterStats as;
            as.name = "vadapter" + std::to_string(id);
            as.rx = a.rx;
            as.adapter_stalled = a.stalled;
            snap.adapters.push_back(std::move(as));
        }
    }

    // §15.5 stats/reset. The frame-shm reassemblers live in run_rx (ShmOut),
    // so the caller resets those; here we zero the RX engine's counters.
    void reset_stats() { engine_.reset_stats(); }

    std::string request_recovery(int local_stream_id, const Inject& inject) {
        std::optional<RxStreamInfo> selected;
        for (const RxStreamInfo& info : engine_.streams()) {
            if (info.stream_type != stream_type::kRtp ||
                (local_stream_id >= 0 &&
                 info.local_stream_id != static_cast<uint8_t>(local_stream_id))) {
                continue;
            }
            if (selected && local_stream_id < 0) {
                return "stream_id required when multiple RTP streams are latched";
            }
            selected = info;
        }
        if (!selected) {
            return "no matching latched RTP stream";
        }
        RecoveryRequest req;
        req.prefix = {originator_, selected->key.originator, session_};
        req.target_originator = selected->key.originator;
        req.target_session = selected->key.session_id;
        req.target_stream_id = selected->key.stream_id;
        uint8_t frame[kRecoveryRequestSize];
        if (encode_recovery_request(req, frame, sizeof(frame)) != sizeof(frame)) {
            return "failed to encode recovery request";
        }
        inject(frame, sizeof(frame), req.target_originator);
        return "";
    }

    std::vector<StreamKey> stream_keys() const {
        std::vector<StreamKey> out;
        for (const RxStreamInfo& info : engine_.streams()) {
            out.push_back(info.key);
        }
        return out;
    }

    uint16_t originator_;
    uint32_t session_;
    RxEngine engine_;
    Reporter reporter_;
    uint32_t feedback_period_ms_ = 0;
    uint64_t next_feedback_ms_ = 0;
    uint32_t feedback_epoch_ = 0;
};

// ---- shared setup -----------------------------------------------------------

struct Loaded {
    Config cfg;
    ProfileTable table;
    bool have_table = false;
    uint8_t tv = 0;
};

int load_all(const std::string& config_path, Loaded& out) {
    auto cfg = load_config(config_path);
    if (!cfg) {
        std::fprintf(stderr, "config error: %s\n", cfg.error.c_str());
        return 1;
    }
    out.cfg = std::move(*cfg.value);
    std::fputs(dump_config_summary(out.cfg).c_str(), stderr);
    if (!out.cfg.profile_table_path.empty()) {
        auto table = load_profile_table(out.cfg.profile_table_path);
        if (!table) {
            std::fprintf(stderr, "profile table error: %s\n",
                         table.error.c_str());
            return 1;
        }
        out.table = std::move(*table.value);
        out.have_table = true;
        out.tv = table_version(out.table);
        std::fprintf(stderr,
                     "profile table: %zu profiles, table_version=0x%02X\n",
                     out.table.profiles.size(), out.tv);
    }
    return 0;
}

void emit_stats(StatsEmitter& emitter, const Loaded& l, uint32_t session,
                uint64_t t0, const TxCore* tx, const RxCore* rx,
                const AirBackend* air = nullptr,
                uint64_t tsf_fallbacks = 0,
                const char* csa_state = nullptr,
                uint32_t ret_window_hits = 0,
                uint32_t ret_window_misses = 0,
                bool tx_wedged = false,
                const std::vector<std::pair<uint8_t, FrameReassemblerStats>>*
                    frame_stats = nullptr,
                const std::vector<std::pair<uint8_t, FrameShmRing::Stats>>*
                    shm_stats = nullptr,
                const CacheRepairStatsOut* cache_repair = nullptr,
                const CacheStoreStatsOut* cache_store = nullptr,
                StatsSnapshot* out_snap = nullptr) {
    const uint64_t now = now_ms();
    StatsSnapshot snap;
    snap.t_ms = now - t0;
    snap.node = l.cfg.node.originator;
    snap.session = session;
    // §7.2 observability (ground): paced vs blind coalesced return batches.
    snap.ret.return_window_hits = ret_window_hits;
    snap.ret.return_window_misses = ret_window_misses;
    // §3.0 Pass 12: unicast-return counters (radio backend only).
    if (air != nullptr) {
        air->fill_return_stats(snap.ret);
    }
    if (csa_state != nullptr) {
        snap.link.csa_state = csa_state;
    }
    if (tx != nullptr) {
        tx->fill_stats(snap, now);
    }
    // Air adapters first so RxCore::fill_stats can merge its per-adapter
    // liveness view into them by index (radio backend; no-op on udp).
    if (air != nullptr) {
        air->fill_adapter_stats(snap, tsf_fallbacks, tx_wedged);
    }
    if (rx != nullptr) {
        rx->fill_stats(snap);
    }
    // §6.3a frame-shm egress: fold each reassembler's frame-level outcomes into
    // the matching stream (by stream_id). recovered_arq / delivered / loss stay
    // the packet-layer view from RxEngine; these are the frame-layer view.
    if (frame_stats != nullptr) {
        for (const auto& [sid, fr] : *frame_stats) {
            StreamStats* st = nullptr;
            for (StreamStats& s : snap.streams) {
                if (s.stream_id == sid) {
                    st = &s;
                    break;
                }
            }
            if (st == nullptr) {  // not latched yet — surface it anyway
                snap.streams.push_back(StreamStats{});
                st = &snap.streams.back();
                st->stream_id = sid;
                st->type = "RTP";
            }
            st->recovered_fec = fr.frames_fec;
            st->frames_fast = fr.frames_fast;
            st->frames_unrecoverable = fr.frames_unrecoverable;
            st->malformed = fr.malformed;
            st->jscc_shadow_blocks = fr.jscc_shadow_blocks;
            st->jscc_predicted_loss_symbols = fr.jscc_predicted_loss_symbols;
            st->jscc_observed_loss_symbols = fr.jscc_observed_loss_symbols;
            st->jscc_underpredicted_blocks = fr.jscc_underpredicted_blocks;
            st->jscc_predicted_parity_symbols =
                fr.jscc_predicted_parity_symbols;
            st->jscc_predicted_repair_symbols =
                fr.jscc_predicted_repair_symbols;
            st->jscc_observed_repair_symbols =
                fr.jscc_observed_repair_symbols;
            st->jscc_repair_underpredicted_blocks =
                fr.jscc_repair_underpredicted_blocks;
            st->jscc_repair_demand_censored_blocks =
                fr.jscc_repair_demand_censored_blocks;
            st->jscc_repair_predicted_parity_symbols =
                fr.jscc_repair_predicted_parity_symbols;
            st->decode_errors = fr.decode_failures;
            st->dropped_superseded = fr.frames_superseded;
            st->dropped_deadline = fr.frames_deadline;
        }
    }
    if (shm_stats != nullptr) {
        for (const auto& [sid, ss] : *shm_stats) {
            for (StreamStats& st : snap.streams) {
                if (st.stream_id == sid) {
                    st.frame_count = ss.reads + ss.writes;
                    st.frame_bytes = ss.frame_bytes;
                    st.frame_size_last = ss.frame_size_last;
                    st.frame_size_min = ss.frame_size_min;
                    st.frame_size_max = ss.frame_size_max;
                    st.frame_interval_us = ss.frame_interval_us;
                    st.frame_jitter_us = ss.frame_jitter_us;
                    st.shm_full_drops = ss.full_drops;
                    st.shm_oversize_drops = ss.oversize_drops;
                    st.shm_bad_slots = ss.bad_slots;
                    break;
                }
            }
        }
    }
    // §15.3: cache blocks present only when the §14.3 role is enabled.
    if (cache_repair != nullptr) {
        snap.cache_repair = *cache_repair;
    }
    if (cache_store != nullptr) {
        snap.cache_store = *cache_store;
    }
    if (out_snap != nullptr) {
        *out_snap = snap;  // §15.5: GET /health reads the freshest snapshot
    }
    emitter.emit(snap);
}

// §15.5 GET /info — static identity. Hand-built (no json dep in app/); the
// field values are numeric or house-controlled strings (no escaping needed).
std::string build_info_json(const Loaded& l, uint32_t session,
                            const char* role) {
    std::string s = "{\"role\":\"";
    s += role;
    s += "\",\"node\":" + std::to_string(l.cfg.node.originator);
    s += ",\"session\":" + std::to_string(session);
    s += ",\"table_version\":" + std::to_string(l.tv);
    s += ",\"streams\":[";
    bool first = true;
    for (const StreamCfg& st : l.cfg.streams) {
        if (!first) s += ',';
        first = false;
        s += "{\"stream_id\":" + std::to_string(st.stream_id);
        s += ",\"dir\":\"";
        s += (st.dir == Dir::kIn ? "in" : "out");
        s += "\",\"bind\":\"";
        s += (st.bind.kind == BindKind::kFrameShm ? "frame-shm" : "udp");
        s += "\"}";
    }
    s += "],\"adapters\":[";
    first = true;
    for (const AdapterCfg& a : l.cfg.adapters) {
        if (!first) s += ',';
        first = false;
        s += "{\"name\":\"" + a.name + "\",\"role\":\"";
        s += (a.role == Role::kTx ? "tx" : "rx");
        s += "\",\"channel\":" + std::to_string(a.channel_mhz) + "}";
    }
    s += "],\"control\":\"" + l.cfg.control.bind + "\"}";
    return s;
}

// §15.5 GET /health — terse link summary from the freshest snapshot.
std::string build_health_json(const StatsSnapshot& snap) {
    int32_t rssi_best = 0;
    bool have = false;
    for (const AdapterStats& a : snap.adapters) {
        if (!have || a.rssi_best > rssi_best) {
            rssi_best = a.rssi_best;
            have = true;
        }
    }
    uint32_t loss_milli = 0;
    uint64_t delivered = 0;
    if (!snap.streams.empty()) {
        loss_milli = snap.streams.front().loss_prediversity_milli;
        delivered = snap.streams.front().delivered;
    }
    std::string s = "{\"state\":\"" + snap.link.state + "\"";
    s += ",\"profile\":" + std::to_string(snap.link.profile);
    s += ",\"mcs\":" + std::to_string(snap.link.mcs);
    s += ",\"rssi_best\":" + std::to_string(have ? rssi_best : 0);
    s += ",\"loss_milli\":" + std::to_string(loss_milli);
    s += ",\"delivered\":" + std::to_string(delivered);
    s += ",\"csa_state\":\"" + snap.link.csa_state + "\"}";
    return s;
}

// ---- modes -------------------------------------------------------------------

int run_tx(const Loaded& l) {
    auto air = AirBackend::create(l.cfg);
    if (!air) {
        std::fprintf(stderr, "air error: %s\n", air.error.c_str());
        return 1;
    }
    PacketEventTrace packet_trace("tx");
    air.value->set_packet_trace(&packet_trace);
    auto bindings = BindingSet::create(l.cfg);
    if (!bindings) {
        std::fprintf(stderr, "binding error: %s\n", bindings.error.c_str());
        return 1;
    }
    const uint32_t session = session_nonce();
    DiscoveryCatalog discovery;
    TxCore tx(l.cfg, session, l.have_table ? &l.table : nullptr, l.tv);
    tx.estimate_airtime = [&](size_t bytes, bool include_pending) {
        return air.value->estimate_airtime_us(bytes, include_pending);
    };
    if (air.value->is_radio()) {
        tx.apply_mode = [&](uint8_t mcs, bool sgi) {
            air.value->set_tx_mode(mcs, sgi);
        };
        tx.apply_power = [&](size_t idx, int32_t qdb) {
            air.value->set_power_qdb(idx, qdb);
        };
    }
    // §15.4 frame-shm ingress: one consumer ring per frame-shm in-stream. The
    // venc producer may not be up yet, so a failed attach is not fatal — it
    // retries lazily in the loop. have_udp_ins tells us whether BindingSet has
    // any pollable ingress fd of its own (frame-shm-only nodes have none).
    struct ShmIn {
        uint8_t stream_id;
        std::string name;
        std::unique_ptr<FrameShmRing> ring;  // null until attached
    };
    std::vector<ShmIn> shm_ins;
    bool have_udp_ins = false;
    for (const StreamCfg& s : l.cfg.streams) {
        if (s.dir != Dir::kIn) {
            continue;
        }
        if (s.bind.kind != BindKind::kFrameShm) {
            have_udp_ins = true;
            continue;
        }
        ShmIn si{s.stream_id, s.bind.name, nullptr};
        auto r = FrameShmRing::attach(s.bind.name);
        if (r) {
            si.ring = std::move(*r.value);
            std::fprintf(stderr, "tx: frame-shm '%s' attached\n",
                         s.bind.name.c_str());
        } else {
            std::fprintf(stderr,
                         "tx: frame-shm '%s' not up yet (%s); retrying\n",
                         s.bind.name.c_str(), r.error.c_str());
        }
        shm_ins.push_back(std::move(si));
    }
    std::vector<uint8_t> frame_buf(kFrameRingDefaultSlotSize);
    uint64_t next_shm_attach_ms = 0;
    uint64_t next_shm_identity_check_ms = 0;

    StatsEmitter emitter(true, bindings.value->stats_egress());
    const uint64_t t0 = now_ms();
    uint64_t next_stats = t0;
    const uint64_t stats_period =
        l.cfg.stats.hz > 0 ? static_cast<uint64_t>(1000.0 / l.cfg.stats.hz)
                           : 0;

    // §7.2 craft side: after an END_OF_BLOCK the pacer holds video for the
    // quiet window so the single radio can hear returns. Frames arriving
    // inside the window queue behind it (order preserved); the backlog
    // override degrades to §7.1 under load.
    QuietGap qg(quietgap_policy(l.cfg));
    std::deque<std::vector<uint8_t>> held;
    uint64_t now_us_it = now_us();
    const auto send_raw = [&](const uint8_t* f, size_t n) {
        air.value->inject(f, n);
        if (qg.enabled() && frame_is_eob(f, n)) {
            qg.note_eob_sent(now_us_it);
        }
    };
    const TxCore::Inject inject = [&](const uint8_t* f, size_t n) {
        if (!held.empty() ||
            !qg.can_send_video(now_us_it,
                               static_cast<uint32_t>(held.size()))) {
            held.emplace_back(f, f + n);
            return;
        }
        send_raw(f, n);
    };
    const TxCore::Inject inject_resend = [&](const uint8_t* f, size_t n) {
        air.value->inject_resend(f, n);
    };
    // §11 craft follower: validates campaigns, arms the CSA_ARMED flag, and
    // retunes the (single) radio at the TSF-anchored T_switch.
    CsaFollower csa(csa_params(l.cfg));
    // §9.10 TX-wedge watchdog over the TX adapter's CCX-report counters.
    TxWedge wedge(TxWedgePolicy{l.cfg.air.wedge_window_ms,
                                l.cfg.air.wedge_min_submits});
    // §15.5 REST control plane. TX node owns the profile-pin + FEC-retune
    // knobs; CSA is issuer-only (rx), so h.csa stays null → 409.
    StatsSnapshot last_snap;
    std::unique_ptr<ControlServer> control;
    if (!l.cfg.control.bind.empty()) {
        auto cs = ControlServer::create(l.cfg.control.bind);
        if (!cs) {
            std::fprintf(stderr, "control: %s\n", cs.error.c_str());
            return 1;
        }
        control = std::move(*cs.value);
        ControlHandlers h;
        h.stats_line = [&]() -> std::string {
            std::string s = emitter.last_line();
            if (!s.empty() && s.back() == '\n') s.pop_back();
            return s;
        };
        h.info_json = [&] { return build_info_json(l, session, "tx"); };
        h.health_json = [&] { return build_health_json(last_snap); };
        h.discovery_json = [&] {
            return discovery.json(now_ms(), {});
        };
        h.profile = [&](int mn, int mx) -> std::string {
            if (mn < 0 || mn > 255 || mx < 0 || mx > 255)
                return "min/max must be 0..255";
            if (mx != 255 && mn > mx) return "min > max";
            tx.set_profile_pin(static_cast<uint8_t>(mn),
                               static_cast<uint8_t>(mx));
            return "";
        };
        h.fec = [&](int sid, int ip, int pp, int mk) -> std::string {
            if (sid < 0 || sid > 255) return "bad stream_id";
            if (ip < 0 || ip > 4000 || pp < 0 || pp > 4000 || mk < 1)
                return "bad fec rates (0..4000 permille, min_k>=1)";
            return tx.set_stream_fec(static_cast<uint8_t>(sid),
                                     static_cast<uint16_t>(ip),
                                     static_cast<uint16_t>(pp),
                                     static_cast<uint16_t>(mk))
                       ? ""
                       : "no frame-shm stream with that id";
        };
        h.reset_stats = [&] {
            tx.reset_stats();
            for (ShmIn& si : shm_ins) {
                if (si.ring) si.ring->reset_stats();
            }
        };
        control->set_handlers(std::move(h));
        std::fprintf(stderr, "control: REST on %s (tx)\n",
                     l.cfg.control.bind.c_str());
    }
    std::fprintf(stderr, "tx: session=%u, running%s\n", session,
                 qg.enabled() ? " (quiet-gap pacing)" : "");
    while (g_stop == 0) {
        // One timestamp per iteration: every callback and the tick share it,
        // so the time injected into the core never steps backward between
        // calls (a fresh now_ms() inside a callback can land 1 ms AFTER the
        // tick's captured now, and u64 "now - stamp" arithmetic underflows).
        const uint64_t now = now_ms();
        now_us_it = now_us();
        if (now >= next_shm_identity_check_ms) {
            for (ShmIn& si : shm_ins) {
                if (si.ring && !si.ring->backing_object_current()) {
                    std::fprintf(stderr,
                                 "tx: frame-shm '%s' producer replaced; "
                                 "reattaching\n",
                                 si.name.c_str());
                    si.ring.reset();
                }
            }
            next_shm_identity_check_ms = now + 250;
        }
        // Flush held video the moment the gap allows it; an EOB inside the
        // flush re-arms the gap and holds the rest.
        while (!held.empty() &&
               qg.can_send_video(now_us_it,
                                 static_cast<uint32_t>(held.size()))) {
            const std::vector<uint8_t> f = std::move(held.front());
            held.pop_front();
            send_raw(f.data(), f.size());
        }
        // Lazy (re)attach of any frame-shm producer that wasn't up at startup.
        bool any_pending = false;
        for (const ShmIn& si : shm_ins) {
            if (!si.ring) {
                any_pending = true;
            }
        }
        if (any_pending && now >= next_shm_attach_ms) {
            for (ShmIn& si : shm_ins) {
                if (si.ring) {
                    continue;
                }
                if (auto r = FrameShmRing::attach(si.name)) {
                    si.ring = std::move(*r.value);
                    std::fprintf(stderr, "tx: frame-shm '%s' attached\n",
                                 si.name.c_str());
                }
            }
            next_shm_attach_ms = now + 500;
        }
        // Union the frame-shm consumer eventfds into the ingress wait. shm_fds
        // lists only attached rings, in order; drain_shm(j) maps index j back.
        std::vector<int> shm_fds;
        for (const ShmIn& si : shm_ins) {
            if (si.ring) {
                shm_fds.push_back(si.ring->event_fd());
            }
        }
        const size_t shm_fd_count = shm_fds.size();
        const std::vector<int> air_fds = air.value->wait_fds();
        shm_fds.insert(shm_fds.end(), air_fds.begin(), air_fds.end());
        const auto drain_shm = [&](size_t j) {
            if (j >= shm_fd_count) return;  // air readiness; drained below
            size_t k = 0;
            for (ShmIn& si : shm_ins) {
                if (!si.ring) {
                    continue;
                }
                if (k++ != j) {
                    continue;
                }
                si.ring->drain_event();
                for (;;) {
                    const long got =
                        si.ring->read_frame(frame_buf.data(), frame_buf.size());
                    if (got <= 0) {
                        break;
                    }
                    tx.on_frame(si.stream_id, frame_buf.data(),
                                static_cast<size_t>(got), now, inject);
                }
                return;
            }
        };
        // Held frames need µs-scale reactivity — don't sleep in poll then.
        const int in_timeout = held.empty() && !air.value->tx_pending() ? 2 : 0;
        bindings.value->poll_once(
            in_timeout,
            [&](const IngressEvent& ev) {
                tx.on_ingress(ev.stream_id, ev.data, ev.len, now, inject);
            },
            shm_fds, drain_shm);
        // Nothing to wait on yet (no UDP ingress and every frame-shm producer
        // still down): poll_once returned instantly — pace to avoid a busy spin
        // while waiting for the producer to come up.
        if (!have_udp_ins && shm_fds.empty() && in_timeout > 0) {
            ::poll(nullptr, 0, in_timeout);
        }
        const uint64_t service_now = now_ms();
        air.value->poll_once(0, [&](const AirRxMeta& meta, const uint8_t* d,
                                    size_t n) {
            const Decoded dec = decode(d, n);
            discovery.observe(dec, service_now);
            if (const CsaPacket* c = std::get_if<CsaPacket>(&dec)) {
                if (!air.value->supports_csa()) {
                    return;
                }
                // §11.2: anchor on this adapter's TSF; the follower manages
                // its own issuer latch (MAC-valid bootstrap, §11.4).
                if (csa.on_csa(*c, now_us_it,
                               air.value->read_tsf(meta.adapter_id),
                               static_cast<uint32_t>(meta.tsf_us),
                               std::nullopt)) {
                    tx.csa_freeze(now + static_cast<uint64_t>(
                                            l.cfg.policy.csa.settle_s * 1000));
                    tx.set_csa_armed(true);
                    std::fprintf(stderr,
                                 "csa: armed -> %u MHz (nonce %u, dt %u ms)\n",
                                 c->target_chan, c->csa_nonce,
                                 c->dt_to_switch_ms);
                }
                return;
            }
            if (!std::holds_alternative<DecodeError>(dec)) {
                csa.note_valid_rx(now_us_it);  // §11.5 verify/rendezvous feed
            }
            tx.on_air(d, n, service_now);
        });
        const CsaAction ca = csa.tick(now_us_it);
        if (ca.kind != CsaAction::Kind::kNone) {
            tx.set_csa_armed(false);  // switching now — the ACK window is over
            air.value->retune_all(ca.chan_mhz, ca.bw, ca.fast);
            std::fprintf(stderr, "csa: %s -> %u MHz\n", csa.state_str(),
                         ca.chan_mhz);
        }
        tx.tick(service_now, inject, inject_resend);
        air.value->heartbeat(l.cfg.node.originator, session, service_now);
        if (const auto trc = air.value->tx_report_counters()) {
            if (wedge.poll(now, trc->first, trc->second)) {
                std::fprintf(stderr, "%s", wedge.wedged()
                        ? "air: TX WEDGE — submissions advancing, zero CCX "
                          "reports over the window (§9.10)\n"
                        : "air: tx wedge cleared — CCX reports resumed\n");
            }
        }
        if (control) {
            control->service(now);
        }
        if (stats_period != 0 && now >= next_stats) {
            std::vector<std::pair<uint8_t, FrameShmRing::Stats>> shm_stats;
            for (const ShmIn& si : shm_ins) {
                if (si.ring) {
                    shm_stats.emplace_back(si.stream_id, si.ring->stats());
                }
            }
            emit_stats(emitter, l, session, t0, &tx, nullptr, &*air.value, 0,
                       csa.state_str(), 0, 0, wedge.wedged(), nullptr,
                       &shm_stats, nullptr, nullptr, &last_snap);
            if (control) {
                control->publish_stats(emitter.last_line());
            }
            next_stats = now + stats_period;
        }
    }
    return 0;
}

int run_rx(const Loaded& l) {
    auto air = AirBackend::create(l.cfg);
    if (!air) {
        std::fprintf(stderr, "air error: %s\n", air.error.c_str());
        return 1;
    }
    PacketEventTrace packet_trace("rx");
    air.value->set_packet_trace(&packet_trace);
    auto bindings = BindingSet::create(l.cfg);
    if (!bindings) {
        std::fprintf(stderr, "binding error: %s\n", bindings.error.c_str());
        return 1;
    }
    const uint32_t session = session_nonce();
    DiscoveryCatalog discovery;
    RxCore rx(l.cfg, session, l.have_table ? &l.table : nullptr,
              l.have_table ? std::optional<uint8_t>(l.tv) : std::nullopt);

    // §15.4 frame-shm egress: one producer ring + a §6.3a reassembler per
    // frame-shm out-stream. deliver_now carries the loop's per-iteration clock
    // into the deliver lambda (the reassembler needs now_ms for its deadlines).
    struct ShmOut {
        uint8_t stream_id;
        std::unique_ptr<FrameShmRing> ring;
        std::unique_ptr<FrameReassembler> reasm;
    };
    std::vector<ShmOut> shm_outs;
    for (const StreamCfg& s : l.cfg.streams) {
        if (s.dir != Dir::kOut || s.bind.kind != BindKind::kFrameShm) {
            continue;
        }
        auto r = FrameShmRing::create(s.bind.name);
        if (!r) {
            std::fprintf(stderr, "frame-shm egress '%s': %s\n",
                         s.bind.name.c_str(), r.error.c_str());
            return 1;
        }
        FrameReassemblerConfig frc;
        // Map the drop deadline from the floor rung's I-frame budget when a
        // table is loaded; otherwise keep the §6.3a default.
        if (l.have_table) {
            for (const Profile& p : l.table.profiles) {
                if (p.id == l.table.floor_profile &&
                    p.arq_deadline_iframe_ms > 0) {
                    frc.deadline_ms = p.arq_deadline_iframe_ms;
                }
            }
        }
        std::fprintf(stderr, "rx: frame-shm egress '%s' created\n",
                     s.bind.name.c_str());
        shm_outs.push_back(ShmOut{s.stream_id, std::move(*r.value),
                                  std::make_unique<FrameReassembler>(frc)});
    }
    uint64_t deliver_now = now_ms();

    // §14.3 cache roles (v1 IP transport; both optional, either or both).
    std::unique_ptr<CacheController> cache_ctl;
    std::unique_ptr<CacheUdp> cache_repair_sock;
    std::map<uint16_t, CacheEndpoint> cache_endpoints;  // originator -> ep
    FrameReassembler* cache_reasm = nullptr;
    FrameShmRing* cache_ring = nullptr;
    if (l.cfg.cache.repair.enabled) {
        const CacheRepairCfg& cr = l.cfg.cache.repair;
        for (const CacheEndpointCfg& e : cr.caches) {
            auto ep = CacheUdp::resolve(e.endpoint);
            if (!ep) {
                std::fprintf(stderr, "cache.repair '%s': %s\n",
                             e.endpoint.c_str(), ep.error.c_str());
                return 1;
            }
            cache_endpoints.emplace(e.originator, *ep.value);
        }
        auto sock = CacheUdp::open(cr.listen);
        if (!sock) {
            std::fprintf(stderr, "cache.repair listen: %s\n",
                         sock.error.c_str());
            return 1;
        }
        cache_repair_sock =
            std::make_unique<CacheUdp>(std::move(*sock.value));
        CacheControllerConfig cc;
        cc.self_originator = l.cfg.node.originator;
        cc.self_session = session;
        for (const CacheEndpointCfg& e : cr.caches) {
            cc.caches.push_back(e.originator);
        }
        cc.tail_grace_ms = cr.tail_grace_ms;
        cc.local_quiet_ms = cr.local_quiet_ms;
        cc.min_collect_ms = cr.min_collect_ms;
        cc.hard_close_ms = cr.hard_close_ms;
        cc.request_timeout_ms = cr.request_timeout_ms;
        cc.repair_fraction_permille = cr.repair_fraction_permille;
        cc.absolute_symbol_limit = cr.absolute_symbol_limit;
        cc.max_cache_attempts = cr.max_cache_attempts;
        cc.reply_limit = cr.reply_limit;
        cc.health_floor_permille = cr.health_floor_permille;
        cc.status_timeout_ms = cr.status_timeout_ms;
        cache_ctl = std::make_unique<CacheController>(cc);
        for (ShmOut& so : shm_outs) {  // config validated the stream exists
            if (so.stream_id == cr.stream_id) {
                cache_reasm = so.reasm.get();
                cache_ring = so.ring.get();
            }
        }
        std::fprintf(stderr, "rx: cache repair on stream %u (%zu caches)\n",
                     cr.stream_id, cr.caches.size());
    }
    std::unique_ptr<CacheStore> cache_store;
    std::unique_ptr<CacheUdp> cache_store_sock;
    std::vector<CacheEndpoint> cache_status_to;
    uint64_t next_cache_status_ms = 0;
    if (l.cfg.cache.store.enabled) {
        const CacheStoreCfg& cs = l.cfg.cache.store;
        auto sock = CacheUdp::open(cs.listen);
        if (!sock) {
            std::fprintf(stderr, "cache.store listen: %s\n",
                         sock.error.c_str());
            return 1;
        }
        cache_store_sock = std::make_unique<CacheUdp>(std::move(*sock.value));
        for (const std::string& t : cs.status_to) {
            auto ep = CacheUdp::resolve(t);
            if (!ep) {
                std::fprintf(stderr, "cache.store status_to '%s': %s\n",
                             t.c_str(), ep.error.c_str());
                return 1;
            }
            cache_status_to.push_back(*ep.value);
        }
        CacheStoreConfig sc;
        sc.self_originator = l.cfg.node.originator;
        sc.stream_ids = cs.stream_ids;
        sc.blocks = cs.blocks;
        sc.reply_limit = cs.reply_limit;
        sc.max_requests_per_s = cs.max_requests_per_s;
        cache_store = std::make_unique<CacheStore>(sc);
        std::fprintf(stderr, "rx: cache store on '%s' (%zu streams)\n",
                     cs.listen.c_str(), cs.stream_ids.size());
    }

    const RxEngine::Deliver deliver = [&](uint8_t sid, uint32_t block_id,
                                          uint8_t flags, const uint8_t* d,
                                          size_t n) {
        for (ShmOut& so : shm_outs) {
            if (so.stream_id == sid) {
                so.reasm->push(block_id, flags, d, n, deliver_now,
                               [&](const uint8_t* f, size_t len) {
                                   so.ring->write_frame(f, len);
                               });
                return;
            }
        }
        if (UdpEgress* out = bindings.value->egress_for(sid)) {
            out->send(d, n);
        }
    };

    // §7.2 ground side: returns (NACK/LINK_REPORT) coalesce and fire at the
    // middle of the craft's quiet gap, anchored on the EOB's receive-TSF.
    // Disabled (default) they inject immediately — §7.1 baseline.
    QuietGap qg(quietgap_policy(l.cfg));
    std::deque<std::pair<std::vector<uint8_t>, uint16_t>> ret_held;
    std::optional<uint64_t> ret_at_us;
    uint32_t ret_window_hits = 0;
    uint32_t ret_window_misses = 0;
    uint64_t tsf_fallbacks = 0;
    uint64_t now_us_it = now_us();
    const RxCore::Inject inject_nack = [&](const uint8_t* f, size_t n,
                                           uint16_t target) {
        if (!qg.enabled()) {
            air.value->inject_return(target, f, n);
            return;
        }
        ret_held.emplace_back(std::vector<uint8_t>(f, f + n), target);
    };
    StatsEmitter emitter(true, bindings.value->stats_egress());
    const uint64_t t0 = now_ms();
    uint64_t next_stats = t0;
    const uint64_t stats_period =
        l.cfg.stats.hz > 0 ? static_cast<uint64_t>(1000.0 / l.cfg.stats.hz)
                           : 0;
    // §11 ground: the issuer runs campaigns (stdin trigger "csa <mhz>
    // [class]", PSK required); the follower makes a PSK-less RX node a
    // spectator that follows others' campaigns. The issuer's own copies never
    // reach the local follower (RadioAir drops own-originator frames).
    const CsaParams cparams = csa_params(l.cfg);
    CsaIssuer issuer(cparams);
    CsaFollower follower(cparams);
    // §9.10: the ground's designated uplink TX adapter gets the same
    // CCX-liveness watchdog as the craft's radio.
    TxWedge wedge(TxWedgePolicy{l.cfg.air.wedge_window_ms,
                                l.cfg.air.wedge_min_submits});
    const uint16_t op_chan =
        l.cfg.adapters.empty() ? 0 : l.cfg.adapters[0].channel_mhz;
    // §15.5 REST control plane. RX/ground node owns the CSA trigger (replaces
    // the removed stdin trigger); profile/fec are TX-only knobs → null → 409.
    StatsSnapshot last_snap;
    std::unique_ptr<ControlServer> control;
    if (!l.cfg.control.bind.empty()) {
        auto cs = ControlServer::create(l.cfg.control.bind);
        if (!cs) {
            std::fprintf(stderr, "control: %s\n", cs.error.c_str());
            return 1;
        }
        control = std::move(*cs.value);
        ControlHandlers h;
        h.stats_line = [&]() -> std::string {
            std::string s = emitter.last_line();
            if (!s.empty() && s.back() == '\n') s.pop_back();
            return s;
        };
        h.info_json = [&] { return build_info_json(l, session, "rx"); };
        h.health_json = [&] { return build_health_json(last_snap); };
        h.discovery_json = [&] {
            return discovery.json(now_ms(), rx.stream_keys());
        };
        h.csa = [&](uint32_t mhz, uint32_t klass) -> std::string {
            if (!air.value->supports_csa()) {
                return "CSA unsupported by kernel-monitor backend";
            }
            const CommonPrefix pre{l.cfg.node.originator, 0, session};
            if (issuer.start(pre, static_cast<uint16_t>(mhz), 0,
                             static_cast<uint8_t>(klass != 0), op_chan, 0, 4,
                             now_us_it)) {
                return "";
            }
            return "rejected (active campaign, PSK, allowlist, or rate-limit)";
        };
        h.video_recover = [&](int stream_id) {
            return rx.request_recovery(stream_id, inject_nack);
        };
        if (air.value->udp) {
            h.bench_rx_drop = [&](int permille) -> std::string {
                return air.value->set_udp_rx_drop(permille)
                    ? std::string() : "permille must be 0..1000";
            };
        }
        h.reset_stats = [&] {
            rx.reset_stats();
            for (ShmOut& so : shm_outs) {
                so.reasm->reset_stats();
                so.ring->reset_stats();
            }
            if (cache_ctl) {
                cache_ctl->reset_stats();
            }
            if (cache_store) {
                cache_store->reset_stats();
            }
            ret_window_hits = 0;
            ret_window_misses = 0;
            tsf_fallbacks = 0;
        };
        control->set_handlers(std::move(h));
        std::fprintf(stderr, "control: REST on %s (rx)\n",
                     l.cfg.control.bind.c_str());
    }
    std::fprintf(stderr, "rx: session=%u, %zu adapters, running%s\n",
                 session, air.value->rx_adapters(),
                 qg.enabled() ? " (quiet-gap returns)" : "");
    while (g_stop == 0) {
        // One timestamp per iteration (see run_tx): callbacks and tick share
        // it so core-injected time never steps backward.
        const uint64_t now = now_ms();
        now_us_it = now_us();
        deliver_now = now;  // the deliver lambda's clock for reassembler pushes
        // Fire the coalesced return window. No deadline (no EOB heard yet)
        // means send immediately — never sit on a return.
        if (!ret_held.empty() &&
            (!ret_at_us || now_us_it >= *ret_at_us)) {
            for (const auto& [f, target] : ret_held) {
                air.value->inject_return(target, f.data(), f.size());
            }
            // §7.2 observability: a batch fired on a TSF-anchored window
            // deadline is a hit; one sent blind (no EOB heard) is a miss.
            if (ret_at_us) {
                ++ret_window_hits;
            } else if (qg.enabled()) {
                ++ret_window_misses;
            }
            ret_held.clear();
            ret_at_us.reset();
        }
        const int air_timeout = ret_held.empty() ? 2 : 0;
        air.value->poll_once(air_timeout, [&](const AirRxMeta& meta,
                                              const uint8_t* d, size_t n) {
            // udp-air carries no real RSSI; fall back to the loopback
            // section's synthetic value so the §9 selector can be exercised
            // over the dev backend (the radio backend supplies real RSSI).
            const int8_t rssi =
                meta.rssi != 0 ? meta.rssi : l.cfg.loopback.rssi_dbm;
            // §11 taps (cheap header decode; DATA still flows to the engine).
            const Decoded dec = decode(d, n);
            discovery.observe(dec, now);
            if (const CsaPacket* c = std::get_if<CsaPacket>(&dec)) {
                if (!air.value->supports_csa()) {
                    return;
                }
                if (follower.on_csa(*c, now_us_it,
                                    air.value->read_tsf(meta.adapter_id),
                                    static_cast<uint32_t>(meta.tsf_us),
                                    std::nullopt)) {
                    std::fprintf(stderr, "csa: following -> %u MHz\n",
                                 c->target_chan);
                }
                return;
            }
            if (const DataView* v = std::get_if<DataView>(&dec)) {
                if ((v->hdr.data_flags & data_flags::kCsaArmed) != 0) {
                    issuer.note_craft_armed(now_us_it);  // §11.6 implicit ACK
                }
                issuer.note_craft_video(now_us_it);
                if (cache_store) {  // §14.3: retain the verbatim wire packet
                    cache_store->note_data(*v, d, n);
                }
            }
            if (!std::holds_alternative<DecodeError>(dec)) {
                follower.note_valid_rx(now_us_it);
            }
            rx.on_air(meta.adapter_id, d, n, now, deliver, rssi);
            if (qg.enabled() && frame_is_eob(d, n)) {
                // Anchor on the SAME adapter's TSF (clocks never cross
                // adapters); a failed read falls back to host arrival.
                const auto tsf_now = air.value->read_tsf(meta.adapter_id);
                if (!tsf_now) {
                    ++tsf_fallbacks;
                }
                ret_at_us = qg.return_deadline(
                    now_us_it, static_cast<uint32_t>(meta.tsf_us), tsf_now);
            }
        });
        rx.tick(now, deliver, inject_nack);
        air.value->heartbeat(l.cfg.node.originator, session, now);
        // §6.3a: drop reassembler blocks past their deadline (unrecoverable),
        // so a stalled block never wedges frame-shm egress.
        for (ShmOut& so : shm_outs) {
            so.reasm->tick(now, [&](const uint8_t* f, size_t len) {
                so.ring->write_frame(f, len);
            });
        }
        // §14.3 cache store: answer requests + push periodic status.
        if (cache_store) {
            uint8_t cbuf[512];
            CacheEndpoint from;
            long rn;
            while ((rn = cache_store_sock->recv_one(cbuf, sizeof(cbuf),
                                                    &from)) > 0) {
                const Decoded cdec = decode(cbuf, static_cast<size_t>(rn));
                const CacheRequestView* rq =
                    std::get_if<CacheRequestView>(&cdec);
                if (rq == nullptr) {
                    continue;
                }
                std::vector<const std::vector<uint8_t>*> pkts;
                if (cache_store->answer(*rq, now, pkts) !=
                    CacheStore::Verdict::kAnswered) {
                    continue;
                }
                for (const std::vector<uint8_t>* p : pkts) {
                    uint8_t rbuf[kCacheReplyFixedSize + kDataHeaderSize +
                                 kMaxDataPayload];
                    const size_t sz = encode_cache_reply(
                        CommonPrefix{l.cfg.node.originator,
                                     rq->hdr.prefix.originator, session},
                        rq->hdr.request_id, p->data(),
                        static_cast<uint16_t>(p->size()), rbuf, sizeof(rbuf));
                    if (sz > 0) {
                        cache_store_sock->send_to(from, rbuf, sz);
                    }
                }
            }
            if (now >= next_cache_status_ms && !cache_status_to.empty()) {
                next_cache_status_ms =
                    now + l.cfg.cache.store.status_interval_ms;
                for (const CacheStore::StatusEntry& se :
                     cache_store->status()) {
                    CacheStatus cs;
                    cs.prefix = CommonPrefix{l.cfg.node.originator, 0,
                                             session};
                    cs.target_originator = se.key.originator;
                    cs.target_session = se.key.session_id;
                    cs.target_stream_id = se.stream_id;
                    cs.oldest_block = se.oldest_block;
                    cs.newest_block = se.newest_block;
                    cs.rx_health_permille = se.rx_health_permille;
                    cs.capability_flags = cache_capability::kIpTransport;
                    uint8_t sbuf[kCacheStatusSize];
                    if (encode_cache_status(cs, sbuf, sizeof(sbuf)) == 0) {
                        continue;
                    }
                    for (const CacheEndpoint& to : cache_status_to) {
                        if (cache_store_sock->send_to(to, sbuf,
                                                      kCacheStatusSize)) {
                            ++cache_store->stats().status_sent;
                        }
                    }
                }
            }
        }
        // §14.3 aggregator: merge replies, register status, issue requests
        // for blocks whose local collection closed below k.
        if (cache_ctl) {
            uint8_t cbuf[kCacheReplyFixedSize + kDataHeaderSize +
                         kMaxDataPayload];
            CacheEndpoint from;
            long rn;
            while ((rn = cache_repair_sock->recv_one(cbuf, sizeof(cbuf),
                                                     &from)) > 0) {
                const Decoded cdec = decode(cbuf, static_cast<size_t>(rn));
                if (const CacheStatus* st = std::get_if<CacheStatus>(&cdec)) {
                    // §13: status only from the configured cache endpoint.
                    const auto it = cache_endpoints.find(st->prefix.originator);
                    if (it != cache_endpoints.end() && it->second == from) {
                        cache_ctl->on_status(*st, now);
                    }
                    continue;
                }
                const CacheReplyView* rv = std::get_if<CacheReplyView>(&cdec);
                if (rv == nullptr) {
                    continue;
                }
                const auto it = cache_endpoints.find(rv->prefix.originator);
                if (it == cache_endpoints.end() || !(it->second == from)) {
                    continue;  // §13: replies only from configured endpoints
                }
                // §3.11: the wrapped bytes revalidate as an ordinary DATA
                // packet of the latched repair stream.
                const Decoded wdec = decode(rv->wrapped, rv->wrapped_len);
                const DataView* wv = std::get_if<DataView>(&wdec);
                if (wv == nullptr ||
                    wv->hdr.stream_id != l.cfg.cache.repair.stream_id) {
                    continue;
                }
                bool latched = false;
                for (const StreamKey& k : rx.stream_keys()) {
                    latched |= k.stream_id == wv->hdr.stream_id &&
                               k.originator == wv->hdr.prefix.originator &&
                               k.session_id == wv->hdr.prefix.session_id;
                }
                if (!latched) {
                    continue;
                }
                if (cache_ctl->on_reply(rv->prefix.originator, rv->request_id,
                                        *wv) !=
                    CacheController::ReplyVerdict::kAccept) {
                    continue;
                }
                // §14.3: cache symbols feed the reassembler directly — never
                // the §6.1/§6.2 per-adapter state.
                bool emitted = false;
                cache_reasm->push(wv->hdr.block_id, wv->hdr.data_flags,
                                  wv->payload, wv->payload_len, now,
                                  [&](const uint8_t* f, size_t len) {
                                      cache_ring->write_frame(f, len);
                                      emitted = true;
                                  });
                if (emitted) {
                    cache_ctl->note_completed(wv->hdr.block_id);
                }
            }
            for (const StreamKey& k : rx.stream_keys()) {
                if (k.stream_id != l.cfg.cache.repair.stream_id) {
                    continue;
                }
                RepairCandidate cands[16];
                const size_t cn = cache_reasm->repair_candidates(cands, 16);
                for (CacheRequestOut& r : cache_ctl->tick(now, k, cands, cn)) {
                    const auto it = cache_endpoints.find(r.cache_originator);
                    if (it != cache_endpoints.end()) {
                        cache_repair_sock->send_to(it->second, r.frame.data(),
                                                   r.frame.size());
                    }
                }
                break;
            }
        }
        std::vector<std::pair<uint8_t, JsccRepairFeedbackState>> feedback;
        feedback.reserve(shm_outs.size());
        for (const ShmOut& so : shm_outs) {
            feedback.emplace_back(so.stream_id, so.reasm->jscc_feedback());
        }
        rx.emit_jscc_feedback(now, feedback, inject_nack);
        // §11 campaign engine. The trigger is now POST /api/v1/csa (§15.5);
        // the stdin trigger was removed with the control-plane migration.
        const CsaIssuer::IssuerAction ia = issuer.tick(now_us_it);
        switch (ia.kind) {
            case CsaIssuer::IssuerAction::Kind::kSendCopy: {
                uint8_t frame[32];
                if (encode_csa(ia.pkt, frame, sizeof(frame)) == 32) {
                    air.value->inject(frame, 32);  // campaign timing: never
                }                                  // quiet-gap-held
                break;
            }
            case CsaIssuer::IssuerAction::Kind::kCommit:
            case CsaIssuer::IssuerAction::Kind::kRevert:
                air.value->retune_all(ia.chan_mhz, ia.bw, ia.fast);
                std::fprintf(stderr, "csa: %s -> %u MHz\n",
                             ia.kind == CsaIssuer::IssuerAction::Kind::kCommit
                                 ? "commit"
                                 : "revert",
                             ia.chan_mhz);
                break;
            case CsaIssuer::IssuerAction::Kind::kAbort:
                std::fprintf(stderr, "csa: aborted (no CSA_ARMED)\n");
                break;
            case CsaIssuer::IssuerAction::Kind::kNone:
                break;
        }
        // Spectator follower actions (PSK-less RX nodes; a ground issuer's
        // follower stays IDLE for its own campaigns — own frames are dropped).
        const CsaAction fa = follower.tick(now_us_it);
        if (fa.kind != CsaAction::Kind::kNone) {
            air.value->retune_all(fa.chan_mhz, fa.bw, fa.fast);
        }
        if (const auto trc = air.value->tx_report_counters()) {
            if (wedge.poll(now, trc->first, trc->second)) {
                std::fprintf(stderr, "%s", wedge.wedged()
                        ? "air: TX WEDGE — submissions advancing, zero CCX "
                          "reports over the window (§9.10)\n"
                        : "air: tx wedge cleared — CCX reports resumed\n");
            }
        }
        if (control) {
            control->service(now);
        }
        if (stats_period != 0 && now >= next_stats) {
            // §6.3a: fold the per-out-stream reassembler counters into stats.
            std::vector<std::pair<uint8_t, FrameReassemblerStats>> frame_stats;
            std::vector<std::pair<uint8_t, FrameShmRing::Stats>> shm_stats;
            frame_stats.reserve(shm_outs.size());
            shm_stats.reserve(shm_outs.size());
            for (const ShmOut& so : shm_outs) {
                frame_stats.emplace_back(so.stream_id, so.reasm->stats());
                shm_stats.emplace_back(so.stream_id, so.ring->stats());
            }
            // §15.3: cache blocks are emitted only when the role is enabled.
            CacheRepairStatsOut crs;
            if (cache_ctl) {
                const CacheRepairStats& s = cache_ctl->stats();
                crs.requests = s.requests;
                crs.replies = s.replies;
                crs.symbols_accepted = s.symbols_accepted;
                crs.symbols_rejected = s.symbols_rejected;
                crs.blocks_closed_deficit = s.blocks_closed_deficit;
                crs.blocks_repaired = s.blocks_repaired;
                crs.blocks_futile = s.blocks_futile;
                crs.requests_suppressed = s.requests_suppressed;
                crs.caches_fresh = s.caches_fresh;
            }
            CacheStoreStatsOut css;
            if (cache_store) {
                const CacheStoreStats& s = cache_store->stats();
                css.requests_received = s.requests_received;
                css.requests_answered = s.requests_answered;
                css.requests_rejected = s.requests_rejected;
                css.symbols_sent = s.symbols_sent;
                css.status_sent = s.status_sent;
                css.blocks_held = s.blocks_held;
                css.health_permille = s.health_permille;
            }
            emit_stats(emitter, l, session, t0, nullptr, &rx, &*air.value,
                       tsf_fallbacks,
                       issuer.active() ? issuer.state_str()
                                       : follower.state_str(),
                       ret_window_hits, ret_window_misses, wedge.wedged(),
                       &frame_stats, &shm_stats,
                       cache_ctl ? &crs : nullptr,
                       cache_store ? &css : nullptr, &last_snap);
            if (control) {
                control->publish_stats(emitter.last_line());
            }
            next_stats = now + stats_period;
        }
    }
    return 0;
}

int run_loopback(const Loaded& l) {
    auto bindings = BindingSet::create(l.cfg);
    if (!bindings) {
        std::fprintf(stderr, "binding error: %s\n", bindings.error.c_str());
        return 1;
    }
    const uint32_t session = session_nonce();
    TxCore tx(l.cfg, session, l.have_table ? &l.table : nullptr, l.tv);
    // The RX wants: loopback delivers every in-stream back out; if no
    // out-streams are configured, latch the in-streams' types and drop the
    // payloads (counter-only bench).
    Config rx_cfg = l.cfg;
    if (RxCore::wants(rx_cfg).empty()) {
        for (const StreamCfg& s : l.cfg.streams) {
            if (s.dir == Dir::kIn) {
                StreamCfg out = s;
                out.dir = Dir::kOut;
                out.bind = BindCfg{};
                rx_cfg.streams.push_back(out);
            }
        }
    }
    RxCore rx(rx_cfg, session ^ 0x5A5A5A5Au,
              l.have_table ? &l.table : nullptr,
              l.have_table ? std::optional<uint8_t>(l.tv) : std::nullopt);

    std::optional<GeParams> ge;
    if (l.cfg.loopback.ge) {
        ge = GeParams{(*l.cfg.loopback.ge)[0], (*l.cfg.loopback.ge)[1],
                      (*l.cfg.loopback.ge)[2], (*l.cfg.loopback.ge)[3]};
    }
    AdapterLossField field(l.cfg.loopback.adapters, l.cfg.loopback.seed,
                           l.cfg.loopback.correlation, l.cfg.loopback.uniform_p,
                           ge);
    LossRng return_rng;
    return_rng.s = l.cfg.loopback.seed ^ 0xFEEDFACEull;

    // One timestamp per loop iteration, shared by every callback and tick
    // (see run_tx): a fresh now_ms() inside inject can land 1 ms after the
    // tick's captured now and u64 "now - stamp" arithmetic underflows.
    uint64_t loop_now = now_ms();

    const RxEngine::Deliver deliver = [&](uint8_t sid, uint32_t, uint8_t,
                                          const uint8_t* d, size_t n) {
        if (UdpEgress* out = bindings.value->egress_for(sid)) {
            out->send(d, n);
        }
    };
    // Synthetic RSSI for the §9 loop: static value, with an optional
    // scripted fade window (relative to process start).
    const uint64_t rssi_t0 = now_ms();
    const auto synthetic_rssi = [&]() -> int8_t {
        if (const auto& f = l.cfg.loopback.rssi_fade) {
            const uint64_t rel = loop_now - rssi_t0;
            if (rel >= f->start_ms && rel < f->end_ms) {
                return f->dbm;
            }
        }
        return l.cfg.loopback.rssi_dbm;
    };
    // TX -> synthetic air -> RX: one loss verdict per (packet, adapter).
    const TxCore::Inject inject = [&](const uint8_t* f, size_t n) {
        field.begin_packet();
        for (uint8_t a = 0; a < field.adapters(); ++a) {
            if (!field.drop(a)) {
                rx.on_air(a, f, n, loop_now, deliver, synthetic_rssi());
            }
        }
    };
    // RX -> return direction -> TX (its own loss). No L2 addressing in
    // loopback — the unicast target is meaningless here.
    const RxCore::Inject inject_nack = [&](const uint8_t* f, size_t n,
                                           uint16_t) {
        if (return_rng.uniform() >= l.cfg.loopback.return_loss_p) {
            tx.on_air(f, n, loop_now);
        }
    };

    StatsEmitter emitter(true, bindings.value->stats_egress());
    const uint64_t t0 = now_ms();
    uint64_t next_stats = t0;
    const uint64_t stats_period =
        l.cfg.stats.hz > 0 ? static_cast<uint64_t>(1000.0 / l.cfg.stats.hz)
                           : 0;
    // §15.5 REST control plane on the bench: tx knobs (profile/fec) + reset
    // (both sides). CSA is a no-op with synthetic air → left null → 409.
    StatsSnapshot last_snap;
    std::unique_ptr<ControlServer> control;
    if (!l.cfg.control.bind.empty()) {
        auto cs = ControlServer::create(l.cfg.control.bind);
        if (!cs) {
            std::fprintf(stderr, "control: %s\n", cs.error.c_str());
            return 1;
        }
        control = std::move(*cs.value);
        ControlHandlers h;
        h.stats_line = [&]() -> std::string {
            std::string s = emitter.last_line();
            if (!s.empty() && s.back() == '\n') s.pop_back();
            return s;
        };
        h.info_json = [&] { return build_info_json(l, session, "loopback"); };
        h.health_json = [&] { return build_health_json(last_snap); };
        h.profile = [&](int mn, int mx) -> std::string {
            if (mn < 0 || mn > 255 || mx < 0 || mx > 255)
                return "min/max must be 0..255";
            if (mx != 255 && mn > mx) return "min > max";
            tx.set_profile_pin(static_cast<uint8_t>(mn),
                               static_cast<uint8_t>(mx));
            return "";
        };
        h.fec = [&](int sid, int ip, int pp, int mk) -> std::string {
            if (sid < 0 || sid > 255) return "bad stream_id";
            if (ip < 0 || ip > 4000 || pp < 0 || pp > 4000 || mk < 1)
                return "bad fec rates (0..4000 permille, min_k>=1)";
            return tx.set_stream_fec(static_cast<uint8_t>(sid),
                                     static_cast<uint16_t>(ip),
                                     static_cast<uint16_t>(pp),
                                     static_cast<uint16_t>(mk))
                       ? ""
                       : "no frame-shm stream with that id";
        };
        h.reset_stats = [&] {
            tx.reset_stats();
            rx.reset_stats();
        };
        control->set_handlers(std::move(h));
        std::fprintf(stderr, "control: REST on %s (loopback)\n",
                     l.cfg.control.bind.c_str());
    }
    std::fprintf(stderr, "loopback: %u adapters, seed=%llu, running\n",
                 l.cfg.loopback.adapters,
                 static_cast<unsigned long long>(l.cfg.loopback.seed));
    while (g_stop == 0) {
        loop_now = now_ms();
        bindings.value->poll_once(2, [&](const IngressEvent& ev) {
            tx.on_ingress(ev.stream_id, ev.data, ev.len, loop_now, inject);
        });
        tx.tick(loop_now, inject);
        rx.tick(loop_now, deliver, inject_nack);
        if (control) {
            control->service(loop_now);
        }
        if (stats_period != 0 && loop_now >= next_stats) {
            emit_stats(emitter, l, session, t0, &tx, &rx, nullptr, 0, nullptr,
                       0, 0, false, nullptr, nullptr, nullptr, nullptr,
                       &last_snap);
            if (control) {
                control->publish_stats(emitter.last_line());
            }
            next_stats = loop_now + stats_period;
        }
    }
    return 0;
}

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <tx|rx|loopback> -c <config.json> [--check]\n"
                 "  --check  validate config + bindings and exit\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage(argv[0]);
    }
    const std::string mode = argv[1];
    if (mode != "tx" && mode != "rx" && mode != "loopback") {
        return usage(argv[0]);
    }
    std::string config_path;
    bool check_only = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else {
            return usage(argv[0]);
        }
    }
    if (config_path.empty()) {
        return usage(argv[0]);
    }

    Loaded l;
    if (const int rc = load_all(config_path, l); rc != 0) {
        return rc;
    }
    if (check_only) {
        auto bindings = BindingSet::create(l.cfg);
        if (!bindings) {
            std::fprintf(stderr, "binding error: %s\n",
                         bindings.error.c_str());
            return 1;
        }
        std::fprintf(stderr, "bindings: OK\n");
        return 0;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (mode == "tx") {
        return run_tx(l);
    }
    if (mode == "rx") {
        return run_rx(l);
    }
    return run_loopback(l);
}
