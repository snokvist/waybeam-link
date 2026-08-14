// SPDX-License-Identifier: GPL-2.0-or-later
// The §15.3 stats assembly and the ARQ timing tracker that feeds it — final
// move of issue #109 Phase 2a.
//
// `StatsEmitter` itself has always lived in `io/`; what was stuck in
// `app/main.cpp` was everything that FILLS a snapshot: the per-role fill
// helpers, the 177-line `emit_stats()` that walks every subsystem, and
// `ArqTimingTracker`, whose percentiles are §15.3 fields.
//
// This one could not move before the others. `emit_stats()` reads `AirBackend`,
// `RxCore`, `TxCore` and `Loaded` — it is the join point of the whole node, so
// it was always going to be last. That is also why it is the piece that proves
// the layer: with it here, a consumer can produce a §15.3 line without
// `app/main.cpp` at all.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wblink/config.h"
#include "wblink/node/air_backend.h"
#include "wblink/node/load.h"
#include "wblink/node/rx_core.h"
#include "wblink/node/tx_core.h"
#include "wblink/table.h"
#include "wblink/stats.h"

namespace wblink {
namespace node {

class ArqTimingTracker {
  public:
    void note_eob(uint64_t at_us) { last_eob_us_ = at_us; }

    void note_nack_built(const uint8_t* frame, size_t len, uint64_t at_us) {
        const Decoded dec = decode(frame, len);
        const NackView* n = std::get_if<NackView>(&dec);
        if (n == nullptr) return;
        if (last_eob_us_ && at_us >= *last_eob_us_) {
            eob_to_build_.observe(at_us - *last_eob_us_);
        }
        for_each_seq(*n, [&](const Key& k) { built_[k] = at_us; });
        trim(built_);
    }

    void note_nack_injected(const uint8_t* frame, size_t len,
                            uint64_t at_us) {
        const Decoded dec = decode(frame, len);
        const NackView* n = std::get_if<NackView>(&dec);
        if (n == nullptr) return;
        bool sampled = false;
        for_each_seq(*n, [&](const Key& k) {
            const auto it = built_.find(k);
            if (!sampled && it != built_.end() && at_us >= it->second) {
                build_to_inject_.observe(at_us - it->second);
                sampled = true;
            }
            injected_[k] = at_us;
        });
        trim(injected_);
    }

    void note_retransmit_arrived(const DataView& v, uint64_t at_us) {
        if ((v.hdr.data_flags & data_flags::kRetransmit) == 0) return;
        const Key k = key(v.hdr.prefix.originator, v.hdr.prefix.session_id,
                          v.hdr.stream_id, v.hdr.seq);
        if (const auto it = injected_.find(k);
            it != injected_.end() && at_us >= it->second) {
            inject_to_retransmit_.observe(at_us - it->second);
            injected_.erase(it);
        }
        if (const auto it = built_.find(k);
            it != built_.end() && at_us >= it->second) {
            build_to_retransmit_.observe(at_us - it->second);
            built_.erase(it);
        }
    }

    void note_nack_received(const uint8_t* frame, size_t len,
                            uint64_t at_us) {
        const Decoded dec = decode(frame, len);
        const NackView* n = std::get_if<NackView>(&dec);
        if (n == nullptr) return;
        for_each_seq(*n, [&](const Key& k) { received_[k] = at_us; });
        trim(received_);
    }

    void note_resend_submitted(const uint8_t* frame, size_t len,
                               uint64_t at_us) {
        const Decoded dec = decode(frame, len);
        const DataView* v = std::get_if<DataView>(&dec);
        if (v == nullptr ||
            (v->hdr.data_flags & data_flags::kRetransmit) == 0) return;
        const Key k = key(v->hdr.prefix.originator,
                          v->hdr.prefix.session_id, v->hdr.stream_id,
                          v->hdr.seq);
        const auto it = received_.find(k);
        if (it != received_.end() && at_us >= it->second) {
            receive_to_resend_.observe(at_us - it->second);
            received_.erase(it);
        }
    }

    ArqTimingStats snapshot() const {
        ArqTimingStats out;
        out.eob_to_nack_build = eob_to_build_.snapshot();
        out.nack_build_to_inject = build_to_inject_.snapshot();
        out.nack_inject_to_retransmit = inject_to_retransmit_.snapshot();
        out.nack_build_to_retransmit = build_to_retransmit_.snapshot();
        out.nack_receive_to_resend = receive_to_resend_.snapshot();
        return out;
    }

    void reset() {
        eob_to_build_.reset();
        build_to_inject_.reset();
        inject_to_retransmit_.reset();
        build_to_retransmit_.reset();
        receive_to_resend_.reset();
        built_.clear();
        injected_.clear();
        received_.clear();
        last_eob_us_.reset();
    }

  private:
    class Series {
      public:
        void observe(uint64_t delta) {
            const uint32_t us = static_cast<uint32_t>(
                std::min<uint64_t>(delta, UINT32_MAX));
            ++samples_;
            max_ = std::max(max_, us);
            recent_.push_back(us);
            if (recent_.size() > 512) recent_.pop_front();
        }
        TimingMetricStats snapshot() const {
            TimingMetricStats out{samples_, 0, max_};
            if (recent_.empty()) return out;
            std::vector<uint32_t> sorted(recent_.begin(), recent_.end());
            std::sort(sorted.begin(), sorted.end());
            const size_t rank = (sorted.size() * 95 + 99) / 100;
            out.p95_us = sorted[rank - 1];
            return out;
        }
        void reset() {
            samples_ = 0;
            max_ = 0;
            recent_.clear();
        }
      private:
        uint64_t samples_ = 0;
        uint32_t max_ = 0;
        std::deque<uint32_t> recent_;
    };

    struct Key {
        uint16_t originator;
        uint32_t session;
        uint8_t stream_id;
        uint32_t seq;
        bool operator<(const Key& other) const {
            return std::tie(originator, session, stream_id, seq) <
                   std::tie(other.originator, other.session, other.stream_id,
                            other.seq);
        }
    };
    static Key key(uint16_t originator, uint32_t session, uint8_t stream_id,
                   uint32_t seq) {
        return Key{originator, session, stream_id, seq};
    }
    template <class F>
    static void for_each_seq(const NackView& n, const F& fn) {
        for (unsigned i = 0; i < static_cast<unsigned>(n.bitmap_len) * 8;
             ++i) {
            if ((n.bitmap[i / 8] & (1u << (i % 8))) != 0) {
                fn(key(n.hdr.target_originator, n.hdr.target_session,
                       n.hdr.target_stream_id, n.hdr.base_seq + i));
            }
        }
    }
    static void trim(std::map<Key, uint64_t>& m) {
        while (m.size() > 4096) m.erase(m.begin());
    }

    Series eob_to_build_;
    Series build_to_inject_;
    Series inject_to_retransmit_;
    Series build_to_retransmit_;
    Series receive_to_resend_;
    std::map<Key, uint64_t> built_;
    std::map<Key, uint64_t> injected_;
    std::map<Key, uint64_t> received_;
    std::optional<uint64_t> last_eob_us_;
};

// Loaded + load_all moved to node/load.h (#109 Phase 3 prep):
// a consumer that runs a node has to build one, and reaching it
// through the §15.3 assembly header was an accident of where it
// happened to land in Phase 2a.

// §11.7/§15.3 command-surface fields not owned by Tx/RxCore (the engines
// live in the mode loops): craft nonce, issuer campaign state, rx ARQ gate.
struct VcmdStatsFill {
    uint32_t cmd_last_nonce = 0;       // craft
    const char* vcmd_state = nullptr;  // issuer (null = not an issuer)
    uint32_t vcmd_nonce = 0;
    bool arq_rx_enabled = true;        // rx gate
    const char* mtu_mode = nullptr;    // ground local preference
    uint16_t mtu_requested = kDefaultMaxPayload;
    uint16_t mtu_effective = kDefaultMaxPayload;
    uint16_t mtu_supported = kDefaultMaxPayload;
};

// §10.7 (Pass 125) ground-uplink stats. A struct rather than eleven more
// parameters, and a pointer so every other role emits the role-neutral
// idle/zero defaults without threading anything.
struct UplinkStatsFill {
    const char* state = "idle";
    uint8_t rung = 0;
    int32_t power_qdb = 0;
    uint8_t fingerprint = 0;
    bool stale = false;
    // §3.16 (Pass 153) probe-exchange counters for the local node's run.
    uint64_t probes_sent = 0;
    uint64_t tallies_rx = 0;
    uint8_t rx_mcs = kUplinkRxMcsUnknown;
    // §10.3/§10.5/§11.7 0x0A (Pass 135). Only TxCore::fill_stats sets the
    // link.tx_power_* fields, and the ground has no TxCore — so its §15.3
    // reported tier -1 and power 0 no matter what was selected, while the
    // endpoint reported the truth. The hub menu binds to §15.3, so the one
    // row an operator reads was the one that could not move.
    int tx_power_tier = -1;
    int32_t tx_power_ceiling_qdb = 0;
    bool tx_power_tier_effective = false;
    bool tx_power_override = false;
    int32_t tx_power_qdb = 0;
};

// §15.5 Pass 113: live TX self state appended to GET /info (channel, pairing
// gate, §11.5a claim). Null on non-craft nodes.
struct InfoSelfState {
    uint16_t channel_mhz = 0;
    bool psk_announced = false;
    std::optional<uint16_t> claimed_by;
};

inline void emit_stats(StatsEmitter& emitter, const Loaded& l, uint32_t session,
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
                       StatsSnapshot* out_snap = nullptr,
                       const ArqTimingStats* arq_timing = nullptr,
                       const VcmdStatsFill* vcmd = nullptr,
                       uint16_t channel_mhz = 0,
                       const UplinkStatsFill* uplink = nullptr) {
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
    snap.link.channel_mhz = channel_mhz;  // §11 current operating channel (0 = not tracked)
    if (tx != nullptr) {
        tx->fill_stats(snap, now);
    }
    if (uplink != nullptr) {  // §10.7 Pass 125 (ground/rx node only)
        snap.link.uplink_calib_state = uplink->state;
        snap.link.uplink_calib_rung = uplink->rung;
        snap.link.uplink_calib_power_qdb = uplink->power_qdb;
        snap.link.uplink_calib_fingerprint = uplink->fingerprint;
        snap.link.uplink_calib_stale = uplink->stale;
        snap.link.calib_probes_sent = uplink->probes_sent;
        snap.link.calib_tallies_rx = uplink->tallies_rx;
        snap.link.calib_rx_mcs = uplink->rx_mcs;
        snap.link.tx_power_tier = uplink->tx_power_tier;
        snap.link.tx_power_ceiling_qdb = uplink->tx_power_ceiling_qdb;
        snap.link.tx_power_tier_effective = uplink->tx_power_tier_effective;
        snap.link.tx_power_override = uplink->tx_power_override;
        snap.link.tx_power_qdb = uplink->tx_power_qdb;
    }
    if (vcmd != nullptr) {
        snap.link.cmd_last_nonce = vcmd->cmd_last_nonce;
        if (vcmd->vcmd_state != nullptr) {
            snap.link.vcmd_state = vcmd->vcmd_state;
            snap.link.vcmd_nonce = vcmd->vcmd_nonce;
        }
        snap.link.arq_rx_enabled = vcmd->arq_rx_enabled;
        if (vcmd->mtu_mode != nullptr) {
            snap.link.mtu_mode = vcmd->mtu_mode;
            snap.link.mtu_requested = vcmd->mtu_requested;
            snap.link.mtu_effective = vcmd->mtu_effective;
            snap.link.mtu_supported = vcmd->mtu_supported;
        }
    }
    // Air adapters first so RxCore::fill_stats can merge its per-adapter
    // liveness view into them by index (radio backend; no-op on udp).
    if (air != nullptr) {
        air->fill_adapter_stats(snap, tsf_fallbacks, tx_wedged);
    }
    if (rx != nullptr) {
        rx->fill_stats(snap, now);
#if WBLINK_RADIO
        // §15.3 Pass 159 role-dependent view: a radio GROUND shows the
        // cause it computes (what it sends); the craft's accepted-verdict
        // fill lives in its own tx fill and is not touched here.
        if (air != nullptr && air->radio != nullptr) {
            snap.link.verdict = air->radio->link_verdict();
            snap.link.verdict_age_ms = 0;
        }
#endif
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
            st->fec_recovered_source_symbols =
                fr.fec_recovered_source_symbols;
            st->arq_recovered_source_symbols =
                fr.arq_recovered_source_symbols;
            st->arq_recovered_repair_symbols =
                fr.arq_recovered_repair_symbols;
            st->frames_with_arq = fr.frames_with_arq;
            st->frames_fec_only = fr.frames_fec_only;
            st->frames_fec_after_arq = fr.frames_fec_after_arq;
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
                    // §15.3 Pass 109: ingress full_drops/throttle are published
                    // only by a producer carrying the exact health marker.
                    // Egress retains its local producer counters; ring_full
                    // remains independent consumer-side evidence.
                    st.shm_health_valid = ss.health_valid;
                    st.shm_full_drops = ss.full_drops;
                    st.shm_throttle_permille = ss.throttle_permille;
                    st.shm_oversize_drops = ss.oversize_drops;
                    st.shm_bad_slots = ss.bad_slots;
                    st.shm_ring_full = ss.ring_full;
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
    if (arq_timing != nullptr) {
        snap.arq_timing = *arq_timing;
    }
    if (out_snap != nullptr) {
        *out_snap = snap;  // §15.5: GET /health reads the freshest snapshot
    }
    emitter.emit(snap);
}

// §15.5 GET /info and GET /health payloads (#109 Phase 2c). Same family as
// the §15.3 assembly above — the node's JSON output — and they read the same
// three objects (`Loaded`, `AirBackend`, `StatsSnapshot`), so they belong
// beside it rather than in a header of their own.

// Minimal JSON string escape for the few fields whose value is NOT
// house-controlled: config adapter names are checked only for
// non-emptiness/uniqueness (config.cpp), and a §11.7 MODE label is a readdir
// filename stem — either can carry a quote or backslash that would break
// every consumer of the hand-built JSON (2026-08-14 review finding). Control
// characters are dropped rather than \u-encoded: no legitimate name has any.
inline std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) >= 0x20) {
            out += c;
        }
    }
    return out;
}

// §15.5 (Pass 172) the adapters[] array, shared by /info and the C ABI
// snapshot (wblink_rx_adapters) so the two surfaces cannot drift — one
// builder is the whole mechanism that keeps every consumer shape reading
// the same answer.
inline std::string build_adapters_array(const Loaded& l,
                                        const AirBackend* air) {
    std::string s = "[";
    bool first = true;
    for (size_t i = 0; i < l.cfg.adapters.size(); ++i) {
        const AdapterCfg& a = l.cfg.adapters[i];
        if (!first) s += ',';
        first = false;
        // Live channel, not the boot config: a CSA switch, a craft-local
        // retune (§15.5 Pass 113) or a scout sweep all move adapters without
        // touching cfg. On the ground this is the ONLY channel in the object —
        // there is no top-level `channel` outside the TX self-state — so a
        // stale value here is the whole answer, not a detail.
        const uint16_t chan =
            (air != nullptr && i < air->chan_by_adapter.size())
                ? air->chan_by_adapter[i]
                : a.channel_mhz;
        s += "{\"name\":\"" + json_escape(a.name) + "\",\"role\":\"";
        s += (a.role == Role::kTx ? "tx" : "rx");
        s += "\",\"channel\":" + std::to_string(chan);
        // §15.5 (Pass 154): the per-unit EFUSE identity the §10.6 artifacts
        // key on; null where the backend reports none (D3 posture visible).
        const std::string mac =
            air != nullptr ? air->adapter_mac(i) : std::string{};
        s += ",\"mac\":";
        s += mac.empty() ? "null" : "\"" + mac + "\"";
        // §15.5 (Pass 172): the per-die capability answers, stated even with
        // no backend (a /info served before bring-up) — then they carry the
        // contract's defaults: chip "unknown", every flag false. Absence is
        // already taken (pre-172 payloads), so a reader never infers a
        // capability from a missing key.
        const AirIface::AdapterCapsView caps =
            air != nullptr ? air->adapter_caps(i)
                           : AirIface::AdapterCapsView{};
        s += ",\"chip\":\"" + caps.chip + "\"";
        s += ",\"power_actuator\":";
        s += caps.power_actuator ? "true" : "false";
        s += ",\"ldpc_rx_flag\":";
        s += caps.ldpc_rx_flag ? "true" : "false";
        s += ",\"fastretune\":";
        s += caps.fastretune ? "true" : "false";
        s += "}";
    }
    s += "]";
    return s;
}

// §15.5 GET /info — static identity. Hand-built (no json dep in app/); the
// field values are numeric or house-controlled strings (no escaping needed).
inline std::string build_info_json(const Loaded& l, uint32_t session,
                                   const char* role,
                                   const InfoSelfState* self = nullptr,
                                   const AirBackend* air = nullptr) {
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
    s += "],\"adapters\":" + build_adapters_array(l, air);
    if (self != nullptr) {  // Pass 113 TX self state
        s += ",\"channel\":" + std::to_string(self->channel_mhz);
        s += ",\"psk_announced\":";
        s += self->psk_announced ? "true" : "false";
        s += ",\"claimed\":";
        s += self->claimed_by ? "true" : "false";
        s += ",\"claimed_by\":" + std::to_string(self->claimed_by.value_or(0));
    }
    s += ",\"control\":\"" + l.cfg.control.bind + "\"}";
    return s;
}

// §15.5 GET /api/v1/tx/power. One builder for both roles: the craft and the
// ground answer the same path with the same schema, and before Pass 169 they
// answered it from two hand-rolled copies.
//
// `applied` is the ACTUATOR's account (§10.5), which is a different question
// from `override_qdb` — the latch is what was asked for, `applied_qdb` is what
// the chip took, and they diverge once a relative backend's TXAGC index rails.
// Absent when nothing has been written, so a reader can distinguish "nothing
// applied yet" from "applied 0".
inline std::string build_tx_power_json(
    const std::optional<int32_t>& override_qdb,
    const std::optional<AirIface::TxPowerApplied>& applied,
    bool radio_backend) {
    std::string s = "{\"override_active\":";
    s += override_qdb ? "true" : "false";
    if (override_qdb) {
        s += ",\"qdb\":" + std::to_string(*override_qdb);
    }
    if (applied) {
        // §10.5 (Pass 171): `actuator` is stated, never inferred. Absence of
        // the three below already means "no write yet", so a chip with no
        // lever cannot be signalled by omission alone — it needs a value.
        s += ",\"actuator\":\"";
        s += applied->actuator ? "offset" : "none";
        s += "\"";
        if (applied->actuator) {
            s += ",\"applied_qdb\":" + std::to_string(applied->qdb);
            s += ",\"saturated_low\":";
            s += applied->saturated_low ? "true" : "false";
            s += ",\"saturated_high\":";
            s += applied->saturated_high ? "true" : "false";
        }
    }
    s += ",\"backend\":\"";
    s += radio_backend ? "radio" : "udp";
    s += "\"}";
    return s;
}

// §15.5 GET /health — terse link summary from the freshest snapshot.
inline std::string build_health_json(const StatsSnapshot& snap) {
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

}  // namespace node
}  // namespace wblink
