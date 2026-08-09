// SPDX-License-Identifier: GPL-2.0-or-later
// The air backend and the bench packet trace — third move of the node/ layer
// (issue #109 Phase 2a).
//
// `AirBackend` is the first target of the phase that was NOT clean: it carries
// `PacketEventTrace` with it, so the two move together rather than leaving a
// dangling reference across the layer boundary. Both are otherwise verbatim
// from `app/main.cpp`'s anonymous namespace.
//
// What this owns: which §3.0 backend a node runs (the udp dev backend or the
// devourer radio), the typed non-owning views the §15.3 stats fills read, and
// the two bench knobs that observe the path (`WBLINK_PKT_TRACE`,
// `WBLINK_MCS_TRACE` — both Tier-2, findings.md, no spec surface).
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wblink/node/aim.h"
#include "wblink/node/clock.h"
#include "wblink/air_iface.h"
#include "wblink/air_udp.h"
#include "wblink/config.h"
#include "wblink/log.h"
#include "wblink/stats.h"
#include "wblink/wire.h"
#if WBLINK_RADIO
#include "wblink/air_radio.h"
#endif

namespace wblink {
namespace node {

// Bench-only, bounded packet-event trace. The wire remains the source of truth:
// this observer decodes existing frames and never feeds decisions back in.
// §3.1 type nibble -> name, for the bench packet trace. Reads the wire byte
// directly: this is the path for packets `decode` returned as neither DATA
// nor NACK, including ones a mixed-version peer sent that we do not model.
inline const char* packet_type_name(const uint8_t* frame, size_t len) {
    if (len < 3) return "other";
    static const char* const kNames[16] = {
        "other",       "data",         "nack",        "link_report",
        "heartbeat",   "csa",          "recovery",    "jscc_feedback",
        "cache_status", "cache_request", "cache_reply", "announce",
        "cache_assign", "vehicle_cmd", "selector_state", "extended"};
    return kNames[frame[2] & 0x0F];
}

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
        // Everything else was collapsed into "other", which made the §3.15 /
        // §3.16 guard-cost boundary unverifiable from a trace: the whole
        // property is about WHICH packet rode WHICH slot. Name the type.
        std::fprintf(out_,
                     "{\"type\":\"packet\",\"t_us\":%llu,"
                     "\"direction\":\"%s\",\"outcome\":\"%s\","
                     "\"adapter\":%d,\"packet\":\"%s\",\"bytes\":%zu}\n",
                     static_cast<unsigned long long>(t), direction, outcome,
                     adapter, packet_type_name(frame, len), len);
    }

  private:
    const char* role_;
    std::FILE* out_ = nullptr;
    std::vector<char> buffer_;
    uint64_t cap_ = 75000;
    uint64_t events_ = 0;
    uint64_t dropped_ = 0;
};

// #101 stage-0 verifier (Tier-2 bench knob, findings.md 2026-08-08):
// WBLINK_MCS_TRACE=1 dumps one stderr line per received DATA frame —
// (wire seq, PHY rx_mcs, adapter, rssi) — so an offline correlator can
// check the commanded per-packet rate flew frame-for-frame against the
// TX side's WBLINK_MCS_CYCLE schedule (mcs = seq % 8). rx-role only.
inline bool mcs_trace_enabled() {
    static const bool on = std::getenv("WBLINK_MCS_TRACE") != nullptr;
    return on;
}

struct AirBackend {
    // One owner, held by the contract. Typed views alongside it are NON-owning
    // and exist only for the §15.3 stats fills, which read each backend's own
    // counters struct (their fields genuinely differ — kernel_dropped /
    // bpf_filtered against evm / cfo / snr — so unifying them is a schema
    // question, not a dispatch one) and for the udp-only bench hooks.
    //
    // Owning through a unique_ptr rather than a std::optional member is what
    // lets a test install a FakeAir and exercise the loops below. It also
    // removes a hazard the optionals had: AirBackend is returned by value from
    // create(), so anything caching a pointer INTO an optional member would
    // dangle after the move, silently, since the object still exists at its
    // new address.
    std::unique_ptr<AirIface> air;
    UdpAir* udp = nullptr;
#if WBLINK_RADIO
    RadioAir* radio = nullptr;
#endif
    uint64_t last_tx_ms = 0;
    uint64_t last_announce_ms = 0;  // §3.12 ANNOUNCE cadence (own timer)

    // The ONE place that decides which backend is engaged. Every method below
    // goes through it, so the precedence lives here instead of being retyped
    // per call — that repetition is how backend-specific methods came to
    // differ from their neighbours without anyone noticing.
    //
    // Resolved per call rather than cached: these are std::optional members and
    // AirBackend is returned by value from create(), so a stored AirIface*
    // would dangle after the move — silently, since the object still exists at
    // its new address.
    AirIface* iface() { return air.get(); }
    const AirIface* iface() const {
        return const_cast<AirBackend*>(this)->iface();
    }
    // Live per-adapter channel, indexed like cfg.adapters. §15.5 /api/v1/info
    // reported the CONFIG channel before this, so every adapter still read its
    // boot channel after a CSA or a scout sweep. Per-adapter rather than one
    // backend-wide value because retune_one() moves a single ear (the scout
    // sweeps with the others left in place), so they genuinely diverge.
    //
    // Confidence differs by backend, deliberately not papered over:
    //   radio   — commanded: RadioAir::retune returns true unconditionally
    //             (devourer FastRetune/SetMonitorChannel are void), so this
    //             records intent, not confirmation.
    //   udp dev — logged intent, same as the retune itself.
    // RadioAir applies channel_mhz at create, so the seed is the live value.
    std::vector<uint16_t> chan_by_adapter;

    // §10.6 (Pass 154) per-unit EFUSE identity, empty where the backend has
    // none (udp, an unprogrammed unit). Contract passthrough.
    std::string adapter_mac(size_t i) const { return iface()->adapter_mac(i); }

    uint16_t mtu_supported() const { return iface()->mtu_supported(); }

    static Result<AirBackend> create(const Config& cfg) {
        AirBackend b;
        b.chan_by_adapter.reserve(cfg.adapters.size());
        for (const AdapterCfg& a : cfg.adapters) {
            b.chan_by_adapter.push_back(a.channel_mhz);
        }
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
            auto owned = std::make_unique<UdpAir>(std::move(*a.value));
            b.udp = owned.get();
            b.air = std::move(owned);
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
            // §3.0 Pass 12 hardware-ACK hybrid halves + the Pass 156
            // retry half of the same decision.
            rc.ack_responder = cfg.air.ack_responder;
            rc.unicast_returns = cfg.policy.ret.unicast;
            rc.tx_retry_limit = cfg.air.tx_retry_limit;
            rc.ldpc = cfg.air.ldpc;  // §3.0 Pass 157 node coding
            rc.stbc = cfg.air.stbc;
            rc.mcs_probe = cfg.air.mcs_probe;  // §9.4 Pass 163
            rc.disable_cca = cfg.air.disable_cca;
            // §14.2 (Pass 143): the authored calibration. Zero keeps the
            // estimate unavailable.
            rc.airtime_efficiency_permille =
                cfg.air.airtime_efficiency_permille;
            // §3.11 (Pass 162): the uplink-free archetypes — dedicated cache
            // with no media streams, §2 passive spectator (Pass 74).
            rc.allow_rx_only =
                (cfg.cache.store.enabled && cfg.streams.empty()) ||
                cfg.node.spectator;
            auto a = RadioAir::create(rc);
            if (!a) {
                return Result<AirBackend>::fail(a.error);
            }
            auto owned = std::make_unique<RadioAir>(std::move(*a.value));
            b.radio = owned.get();
            b.air = std::move(owned);
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
        const size_t sent = iface()->inject(f, n);
        if (sent > 0) {
            last_tx_ms = now_ms();
        }
        return sent;
    }

    size_t inject_resend(const uint8_t* f, size_t n) {
        const size_t sent = iface()->inject_resend(f, n);
        if (sent > 0) last_tx_ms = now_ms();
        return sent;
    }

    std::vector<int> wait_fds() const {
        return iface()->wait_fds();
    }

    // Returns (NACK/LINK_REPORT) carry their target so the radio backend
    // can address them as §3.0 unicast when return.unicast is on; the udp
    // dev backend has no L2 addressing and ignores the target.
    size_t inject_return(uint16_t target, const uint8_t* f, size_t n,
                         bool urgent = false) {
        const size_t sent = iface()->inject_return(target, f, n, urgent);
        if (sent > 0) {
            last_tx_ms = now_ms();
        }
        return sent;
    }

    void heartbeat(uint16_t originator, uint32_t session, uint64_t now) {
        // An RX-only node has no uplink to beat on, and this guard was the
        // one backend divergence that was NOT documented anywhere — it read as
        // a monitor detail rather than a rule. It is a rule: a node with no TX
        // adapter does not emit §3.11 heartbeats, whichever backend it runs.
        if (!iface()->has_tx()) return;
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

    // §3.12 pairing beacon. Unlike heartbeat this is NOT suppressed by active
    // DATA — it is the craft's continuous claim/token advertisement, emitted at
    // ~2 Hz in both claimed and unclaimed states so a rebooted ground can
    // re-learn the token and re-claim in place (§11.5a). psk_present selects
    // announced (token in psk) vs secret (all-zero) mode; claimed/claimed_by
    // come from the follower's §11.5a binding.
    void announce(uint16_t originator, uint32_t session, uint64_t now,
                  const uint8_t* token, bool psk_present, bool claimed,
                  uint16_t claimed_by) {
        static constexpr uint64_t kAnnounceIntervalMs = 500;  // §3.12 1–2 Hz
        if (now < last_announce_ms ||
            now - last_announce_ms < kAnnounceIntervalMs) {
            return;
        }
        last_announce_ms = now;
        Announce a;
        a.prefix.originator = originator;
        a.prefix.session_id = session;  // destination = 0 (§3.12)
        a.flags = 0;
        if (claimed) {
            a.flags |= announce_flags::kClaimed;
            a.claimed_by = claimed_by;
        }
        if (psk_present) {
            a.flags |= announce_flags::kPskPresent;
            std::memcpy(a.psk, token, kAnnouncePskSize);
        }  // else psk stays all-zero (secret mode)
        uint8_t frame[kAnnounceSize];
        if (encode_announce(a, frame, sizeof(frame)) == sizeof(frame)) {
            inject(frame, sizeof(frame));
        }
    }

    int poll_once(int timeout_ms, const AirIface::RxCb& cb) {
        return iface()->poll_once(timeout_ms, cb);
    }

    void set_packet_trace(PacketEventTrace* trace) {
        if (!udp || trace == nullptr || !trace->enabled()) return;
        udp->set_trace([trace](const char* direction, const char* outcome,
                              int adapter, const uint8_t* frame, size_t len) {
            trace->packet(direction, outcome, adapter, frame, len);
        });
    }

    size_t rx_adapters() const {
        return iface()->rx_adapters();
    }

    // Radio surfaces. Each backend states its own answer (§10.5's bool, the
    // §10.2-vs-driver-default split behind set_power_auto, nullopt TSF where
    // there is no hardware clock) — see io/include/wblink/air_iface.h.
    void set_tx_mode(uint8_t mcs, bool sgi) { iface()->set_tx_mode(mcs, sgi); }
    // §9.4 Pass 163 (radio-real; no-op elsewhere by the iface contract).
    void set_mcs_probe(uint16_t period, uint16_t slot, uint8_t mcs) {
        iface()->set_mcs_probe(period, slot, mcs);
    }
    bool set_power_qdb(size_t adapter, int32_t qdb) {
        return iface()->set_power_qdb(adapter, qdb);
    }
    // §10.5 (Pass 150) relative contract, resolved natively per backend.
    bool set_power_offset_qdb(size_t adapter, int32_t qdb) {
        return iface()->set_power_offset_qdb(adapter, qdb);
    }
    void set_power_auto(size_t adapter) {
        (void)iface()->set_power_auto(adapter);
    }
    std::optional<uint64_t> read_tsf(uint8_t adapter) {
        if (!aim_log_enabled()) {
            return iface()->read_tsf(adapter);
        }
        // Issue #99: the §7.2 term with no number — time the control
        // transfer (bench knob; the steady clock is never on a wire).
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = iface()->read_tsf(adapter);
        g_aim_read_tsf.add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0)
                .count()));
        return r;
    }
    // §11.6 intra-process atomic switch: every local adapter retunes at
    // T_switch (a straggler follows because a sibling heard the CSA). On the
    // udp dev backend the retune is a logged intent — the CSA state machines
    // stay exercisable end-to-end without radios.
    // §10.7 (Pass 125): latch the rx node's uplink operating point and commit
    // it through the same seam the tx node's selector uses. A tx node never
    // latches — its selector re-commits the rate on every profile change, so
    // it self-heals; an rx node has no selector, which is why the rate has to
    // be re-asserted after every retune. Unlatched, both calls are no-ops.
    std::optional<std::pair<uint8_t, bool>> uplink_rate;
    void latch_uplink_rate(uint8_t mcs, bool sgi) {
        uplink_rate = std::pair<uint8_t, bool>{mcs, sgi};
        set_tx_mode(mcs, sgi);
    }
    // Scope guard, not a call before each `return`: retune_all/retune_one have
    // several exits and a missed one would leave the uplink on whatever rate
    // the retune left behind — silently, and only on some paths.
    struct ReassertRate {
        AirBackend& a;
        explicit ReassertRate(AirBackend& b) : a(b) {}
        ~ReassertRate() {
            if (a.uplink_rate) {
                a.set_tx_mode(a.uplink_rate->first, a.uplink_rate->second);
            }
        }
    };

    // §11.1 bandwidth arrives as either an MHz width or an already-encoded
    // class, depending on the caller. One place resolves it now.
    static uint8_t width_mhz(uint8_t bw) {
        return bw <= 2 ? (bw == 2 ? 80 : bw == 1 ? 40 : 20) : bw;
    }

    bool retune_all(uint16_t chan_mhz, uint8_t bw, bool fast) {
        const ReassertRate guard(*this);
        AirIface* a = iface();
        if (!a->is_rf()) {
            std::fprintf(stderr, "csa: retune -> %u MHz bw=%u%s (udp backend, "
                                 "intent only)\n",
                         chan_mhz, bw, fast ? " fast" : "");
        }
        const uint8_t width = width_mhz(bw);
        bool ok = true;
        for (size_t i = 0; i < a->rx_adapters(); ++i) {
            if (a->retune(i, chan_mhz, width, fast)) {
                note_chan(i, chan_mhz);
                a->reapply_tx_power(i);  // §11.2 post-retune TXAGC
            } else {
                ok = false;
                std::fprintf(stderr,
                             "csa: adapter %zu retune to %u MHz failed\n", i,
                             chan_mhz);
            }
        }
        // Pass 69 §11.6 verify hygiene: pre-retune backlog is old-channel
        // residue — it must never satisfy a video-verify.
        a->flush_rx();
        return ok;
    }
    // Bounds-checked because the retune loops iterate the BACKEND's adapter
    // count. That equals cfg.adapters.size() today (both backends build 1:1
    // from cfg, all-or-fail), so the drop is unreachable — but any future
    // adapter pre-filtering would silently reintroduce the stale-channel bug
    // this exists to fix, so the check stays and this note with it.
    void note_chan(size_t adapter, uint16_t chan_mhz) {
        if (adapter < chan_by_adapter.size()) {
            chan_by_adapter[adapter] = chan_mhz;
        }
    }
    // §11.6 Pass 80: total RX frames across adapters (liveness baseline) and
    // the one-shot backend re-init recovery.
    uint64_t rx_frames_total() const {
        const AirIface* a = iface();
        uint64_t total = 0;
        for (size_t i = 0; i < a->rx_adapters(); ++i) {
            total += a->rx_frames(i);
        }
        return total;
    }
    // §11.6 Pass 80. A backend without a re-init path answers false for every
    // adapter (see AirIface), so this returns false there without a special
    // case — which is what "recovery unavailable" used to mean.
    bool recover_all(uint16_t chan_mhz, uint8_t bw) {
        AirIface* a = iface();
        const uint8_t width = width_mhz(bw);
        bool ok = true;
        bool any = false;
        for (size_t i = 0; i < a->rx_adapters(); ++i) {
            const bool up = a->recover(i, chan_mhz, width);
            if (up) {
                note_chan(i, chan_mhz);
                any = true;
            }
            ok = up && ok;
        }
        // Only flush when something actually came back. The old monitor path
        // flushed unconditionally; discarding the backlog after a recovery
        // that recovered nothing throws away frames for no benefit, and on a
        // backend with no re-init path it would be a pure loss.
        if (any) a->flush_rx();
        return ok;
    }
    // "Real RF", not "is devourer" — the udp dev transport answers false.
    // Since Pass 164 devourer is the only backend that answers true, so the
    // distinction is now vestigial; name kept for its call sites.
    bool is_radio() const { return iface()->is_rf(); }
    bool tx_pending() const { return udp && udp->tx_pending(); }
    std::optional<uint32_t> estimate_airtime_us(size_t bytes,
                                                bool include_pending,
                                                uint16_t packet_budget) const {
        return iface()->estimate_airtime_us(bytes, include_pending,
                                           packet_budget);
    }
    bool supports_csa() const {
        // UDP exercises CSA state without a physical retune; the radio
        // backend retunes via devourer. Both real.
        return true;
    }
    // §15.5a scout: widen/narrow the RX net_id filter at runtime and retune a
    // single adapter (the scout adapter). Real on the radio backend since Pass
    // 142; UdpAir has no §3.0 identity to retarget.
    void set_filter_net_id(std::optional<uint8_t> net_id) {
        iface()->set_filter_net_id(net_id);
    }
    void set_stamp_net_id(uint8_t net_id) {
        iface()->set_stamp_net_id(net_id);
    }
    // §15.5a scout (Pass 64): index of the uplink adapter the sweep roams. Both
    // RF backends resolve it from their adapter set (radio since Pass 142); udp
    // answers 0, where the retune is a logged-intent no-op anyway.
    size_t tx_index() const {
        return iface()->tx_index();
    }
    bool retune_one(size_t adapter, uint16_t chan_mhz, uint8_t bw, bool fast) {
        const ReassertRate guard(*this);
        const bool tuned =
            iface()->retune(adapter, chan_mhz, width_mhz(bw), fast);
        if (tuned) note_chan(adapter, chan_mhz);
        return tuned;
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
#if WBLINK_RADIO
        if (!radio) {
            return;
        }
        for (size_t i = 0; i < radio->rx_adapters(); ++i) {
            const auto c = radio->counters(i);
            AdapterStats as;
            as.name = c.name;
            as.rx = c.rx_frames;
            // §15.3 Pass 158: the quality window (drained here — this loop
            // is the single reader). Empty window keeps the pre-158
            // last-frame RSSI so a quiet adapter still shows its last level.
            const auto qw = radio->rx_quality_window(i);
            if (qw.frames > 0) {
                as.rssi_best = qw.rssi_peak_dbm;
                as.rssi_mean = qw.rssi_mean_dbm;
                as.snr = qw.snr_db;
                if (qw.noise_valid) as.noise = qw.noise_dbm;
                if (qw.evm_valid) {
                    as.evm = qw.evm_db;
                    as.evm_valid = true;
                }
            } else {
                as.rssi_best = c.rssi_last;
                as.rssi_mean = c.rssi_last;
            }
            as.tx_submitted = c.tx_submitted;
            as.tx_failed = c.tx_failed;
            as.drop = c.rx_dropped;
            as.filtered = c.rx_filtered;  // Pass 114 audit: was computed+dropped
            // Node-wide §7.2 TSF-read fallback count, surfaced once.
            as.tsf_fallback = (i == 0) ? tsf_fallbacks : 0;
            as.tx_reports = c.tx_reports;
            as.tx_report_fails = c.tx_report_fails;
            for (size_t m = 0; m < kRxMcsBuckets; ++m) {
                as.rx_mcs[m] = c.rx_mcs[m];  // §15.3 Pass 118
            }
            as.rx_mcs_unknown = c.rx_mcs_unknown;
            as.rx_ldpc = c.rx_ldpc;  // §15.3 Pass 157
            as.rx_stbc = c.rx_stbc;
            as.ldpc_flag_ok = c.ldpc_flag_ok;
            as.tx_wedged = c.tx && tx_wedged;
            as.rx_dead = c.rx_dead;  // §15.3 Pass 101 (RadioAir only)
            snap.adapters.push_back(std::move(as));
        }
#else
        (void)snap;
        (void)tsf_fallbacks;
        (void)tx_wedged;
#endif
    }

    // §9.4 Pass 163 guard 3: cumulative CRC/ICV-errored frames per
    // descriptor MCS, summed across adapters. false = the backend has no
    // such surface (udp).
    bool crc_mcs_totals(uint64_t (&out)[kRxMcsBuckets]) const {
#if WBLINK_RADIO
        if (radio) {
            for (size_t m = 0; m < kRxMcsBuckets; ++m) out[m] = 0;
            for (size_t i = 0; i < radio->rx_adapters(); ++i) {
                const auto c = radio->counters(i);
                for (size_t m = 0; m < kRxMcsBuckets; ++m) {
                    out[m] += c.rx_crc_mcs[m];
                }
            }
            return true;
        }
#endif
        (void)out;
        return false;
    }

    // §15.3 return-block unicast counters (radio backend; no-op on udp).
    void fill_return_stats(ReturnStats& ret) const {
#if WBLINK_RADIO
        if (radio) {
            radio->return_counters(ret.unicast_sent, ret.unicast_fallback);
        }
#else
        (void)ret;
#endif
    }

    // TX adapter's cumulative (submitted, completion-progress) counters for
    // §9.10: CCX reports on devourer. nullopt on UDP, which has no
    // independent completion surface.
    std::optional<std::pair<uint64_t, uint64_t>> tx_progress_counters() const {
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

}  // namespace node
}  // namespace wblink
