// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link — one portable binary, modes tx / rx / loopback (PROTOCOL.md
// §16.1). Steps 1–9 are live: wire codec, I/O/config/stats, framer + ring,
// merged RX engine, resend scheduler, loopback bench, udp-air dev backend,
// NAL classifier, §9 selector + §10 power, and the devourer radio backend
// (§3.0) with the §7.2 TSF quiet-gap pacer.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <deque>
#include <string>
#include <vector>

#include "wblink/air_udp.h"
#include "wblink/binding.h"
#include "wblink/config.h"
#include "wblink/framer.h"
#include "wblink/loss_model.h"
#include "wblink/power.h"
#include "wblink/power_file.h"
#include "wblink/quietgap.h"
#include "wblink/reporter.h"
#include "wblink/ring.h"
#include "wblink/rx.h"
#include "wblink/scheduler.h"
#include "wblink/selector.h"
#include "wblink/stats.h"
#include "wblink/table.h"
#include "wblink/venc.h"
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
#if WBLINK_RADIO
    std::optional<RadioAir> radio;
#endif

    static Result<AirBackend> create(const Config& cfg) {
        AirBackend b;
        if (cfg.air.kind == AirCfg::Kind::kUdp) {
            auto a = UdpAir::create(cfg.air.udp);
            if (!a) {
                return Result<AirBackend>::fail(a.error);
            }
            b.udp.emplace(std::move(*a.value));
            return Result<AirBackend>::ok(std::move(b));
        }
        if (cfg.air.kind == AirCfg::Kind::kRadio) {
#if WBLINK_RADIO
            RadioAirCfg rc;
            rc.adapters = cfg.adapters;
            rc.stamp_net_id = cfg.node.net_id.value_or(0);
            rc.filter_net_id = cfg.node.net_id;
            rc.originator = cfg.node.originator;
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
#if WBLINK_RADIO
        if (radio) {
            return radio->inject(f, n);
        }
#endif
        return udp->inject(f, n);
    }

    int poll_once(int timeout_ms, const UdpAir::RxCb& cb) {
#if WBLINK_RADIO
        if (radio) {
            return radio->poll_once(timeout_ms, cb);
        }
#endif
        return udp->poll_once(timeout_ms, cb);
    }

    size_t rx_adapters() const {
#if WBLINK_RADIO
        if (radio) {
            return radio->rx_adapters();
        }
#endif
        return udp->rx_adapters();
    }

    // Radio-only surfaces (no-ops / nullopt on the udp dev backend).
    void set_tx_mode(uint8_t mcs, bool sgi) {
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
#if WBLINK_RADIO
        if (radio) {
            return radio->read_tsf(adapter);
        }
#else
        (void)adapter;
#endif
        return std::nullopt;
    }
    bool is_radio() const {
#if WBLINK_RADIO
        return radio.has_value();
#else
        return false;
#endif
    }
    void fill_adapter_stats(StatsSnapshot& snap,
                            uint64_t tsf_fallbacks) const {
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
            snap.adapters.push_back(std::move(as));
        }
#else
        (void)snap;
        (void)tsf_fallbacks;
#endif
    }
};

// ---- TX side: per in-stream framer + ring + scheduler ----------------------

struct TxCore {
    using Inject = std::function<void(const uint8_t*, size_t)>;

    struct Stream {
        uint8_t stream_id;
        Framer framer;
        ResendRing ring;
        ResendScheduler sched;
    };

    TxCore(const Config& cfg, uint32_t session, const ProfileTable* table,
           uint8_t table_version)
        : originator_(cfg.node.originator),
          session_(session),
          table_version_(table_version),
          selector_(selector_policy(cfg), table),
          venc_(cfg.venc) {
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
            FramerConfig fc;
            fc.originator = cfg.node.originator;
            fc.session_id = session;
            fc.stream_id = s.stream_id;
            fc.stream_type = s.stream_type;
            fc.classifier = s.classifier;
            fc.classifier_size_threshold =
                cfg.policy.arq.classifier_size_threshold;
            RingConfig rc;
            rc.window_ms = cfg.policy.arq.ring_window_ms;
            rc.byte_budget = cfg.policy.arq.ring_byte_budget;
            streams_.emplace_back(Stream{
                s.stream_id, Framer(fc), ResendRing(rc),
                ResendScheduler(scheduler_policy(cfg), table)});
            streams_.back().framer.set_operating_point(0, table_version);
        }
    }

    void on_ingress(uint8_t stream_id, const uint8_t* d, size_t n,
                    uint64_t now, const Inject& inject) {
        for (Stream& s : streams_) {
            if (s.stream_id != stream_id) {
                continue;
            }
            s.framer.on_datagram(
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

    // Air packets heard back (uplink): NACKs feed the scheduler, LINK_REPORTs
    // feed the §9 selector.
    void on_air(const uint8_t* d, size_t n, uint64_t now) {
        const Decoded dec = decode(d, n);
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
            ++reports_received_;
            selector_.on_report(*r, now);
        }
    }

    void tick(uint64_t now, const Inject& inject) {
        const SelectorActions act = selector_.tick(now);
        if (act.commit) {
            // §9.5 commit: the operating point stamped on every DATA packet
            // (drives RX deadlines + supersession budgets)...
            for (Stream& s : streams_) {
                s.framer.set_operating_point(act.commit->profile_id,
                                             table_version_);
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
        for (Stream& s : streams_) {
            s.ring.evict(now);
            s.sched.drain(s.ring, now,
                          [&](const uint8_t* f, size_t l) { inject(f, l); });
        }
    }

    void set_pressure(bool on, uint64_t now) {  // §9.9 gauge (step 9+ feeds it)
        selector_.set_pressure(on, now);
    }

    void fill_stats(StatsSnapshot& snap, uint64_t now) const {
        for (const Stream& s : streams_) {
            StreamStats st;
            st.stream_id = s.stream_id;
            st.type = "TX";
            st.seq = s.framer.next_seq();
            st.delivered = s.framer.stats().frames;
            st.resends_sent = s.sched.counters().resends_sent;
            st.double_send_suppressed =
                s.sched.counters().holddown_suppressed;
            st.decode_errors = s.framer.stats().oversize_ingress;
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

    uint16_t originator_;
    uint32_t session_;
    uint8_t table_version_;
    Selector selector_;
    VencActuator venc_;
    std::vector<PowerAdapter> power_;
    uint32_t reports_received_ = 0;
    std::vector<Stream> streams_;
};

// ---- RX side: engine + NACK encode -----------------------------------------

struct RxCore {
    using Inject = std::function<void(const uint8_t*, size_t)>;

    RxCore(const Config& cfg, uint32_t session, const ProfileTable* table,
           std::optional<uint8_t> table_version)
        : originator_(cfg.node.originator),
          session_(session),
          engine_(rx_policy(cfg), wants(cfg), table, table_version),
          reporter_(ReporterPolicy{cfg.policy.report_hz > 0
                                       ? static_cast<uint32_t>(
                                             1000.0 / cfg.policy.report_hz)
                                       : 0},
                    table_version) {}

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
                inject_nack(frame, sizeof(frame));
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
                inject_nack(frame, n);
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
            st.active_profile = info.active_profile;
            snap.streams.push_back(std::move(st));
        }
        for (const auto& [id, a] : engine_.adapters()) {
            AdapterStats as;
            as.name = "vadapter" + std::to_string(id);
            as.rx = a.rx;
            as.adapter_stalled = a.stalled;
            snap.adapters.push_back(std::move(as));
        }
    }

    uint16_t originator_;
    uint32_t session_;
    RxEngine engine_;
    Reporter reporter_;
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
                uint64_t tsf_fallbacks = 0) {
    const uint64_t now = now_ms();
    StatsSnapshot snap;
    snap.t_ms = now - t0;
    snap.node = l.cfg.node.originator;
    snap.session = session;
    if (tx != nullptr) {
        tx->fill_stats(snap, now);
    }
    if (rx != nullptr) {
        rx->fill_stats(snap);
    }
    if (air != nullptr) {
        air->fill_adapter_stats(snap, tsf_fallbacks);
    }
    emitter.emit(snap);
}

// ---- modes -------------------------------------------------------------------

int run_tx(const Loaded& l) {
    auto air = AirBackend::create(l.cfg);
    if (!air) {
        std::fprintf(stderr, "air error: %s\n", air.error.c_str());
        return 1;
    }
    auto bindings = BindingSet::create(l.cfg);
    if (!bindings) {
        std::fprintf(stderr, "binding error: %s\n", bindings.error.c_str());
        return 1;
    }
    const uint32_t session = session_nonce();
    TxCore tx(l.cfg, session, l.have_table ? &l.table : nullptr, l.tv);
    if (air.value->is_radio()) {
        tx.apply_mode = [&](uint8_t mcs, bool sgi) {
            air.value->set_tx_mode(mcs, sgi);
        };
        tx.apply_power = [&](size_t idx, int32_t qdb) {
            air.value->set_power_qdb(idx, qdb);
        };
    }
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
    std::fprintf(stderr, "tx: session=%u, running%s\n", session,
                 qg.enabled() ? " (quiet-gap pacing)" : "");
    while (g_stop == 0) {
        // One timestamp per iteration: every callback and the tick share it,
        // so the time injected into the core never steps backward between
        // calls (a fresh now_ms() inside a callback can land 1 ms AFTER the
        // tick's captured now, and u64 "now - stamp" arithmetic underflows).
        const uint64_t now = now_ms();
        now_us_it = now_us();
        // Flush held video the moment the gap allows it; an EOB inside the
        // flush re-arms the gap and holds the rest.
        while (!held.empty() &&
               qg.can_send_video(now_us_it,
                                 static_cast<uint32_t>(held.size()))) {
            const std::vector<uint8_t> f = std::move(held.front());
            held.pop_front();
            send_raw(f.data(), f.size());
        }
        // Held frames need µs-scale reactivity — don't sleep in poll then.
        const int in_timeout = held.empty() ? 2 : 0;
        bindings.value->poll_once(in_timeout, [&](const IngressEvent& ev) {
            tx.on_ingress(ev.stream_id, ev.data, ev.len, now, inject);
        });
        air.value->poll_once(0, [&](const AirRxMeta&, const uint8_t* d,
                                    size_t n) { tx.on_air(d, n, now); });
        tx.tick(now, inject);
        if (stats_period != 0 && now >= next_stats) {
            emit_stats(emitter, l, session, t0, &tx, nullptr, &*air.value);
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
    auto bindings = BindingSet::create(l.cfg);
    if (!bindings) {
        std::fprintf(stderr, "binding error: %s\n", bindings.error.c_str());
        return 1;
    }
    const uint32_t session = session_nonce();
    RxCore rx(l.cfg, session, l.have_table ? &l.table : nullptr,
              l.have_table ? std::optional<uint8_t>(l.tv) : std::nullopt);
    const RxEngine::Deliver deliver = [&](uint8_t sid, const uint8_t* d,
                                          size_t n) {
        if (UdpEgress* out = bindings.value->egress_for(sid)) {
            out->send(d, n);
        }
    };

    // §7.2 ground side: returns (NACK/LINK_REPORT) coalesce and fire at the
    // middle of the craft's quiet gap, anchored on the EOB's receive-TSF.
    // Disabled (default) they inject immediately — §7.1 baseline.
    QuietGap qg(quietgap_policy(l.cfg));
    std::deque<std::vector<uint8_t>> ret_held;
    std::optional<uint64_t> ret_at_us;
    uint64_t tsf_fallbacks = 0;
    uint64_t now_us_it = now_us();
    const RxCore::Inject inject_nack = [&](const uint8_t* f, size_t n) {
        if (!qg.enabled()) {
            air.value->inject(f, n);
            return;
        }
        ret_held.emplace_back(f, f + n);
    };
    StatsEmitter emitter(true, bindings.value->stats_egress());
    const uint64_t t0 = now_ms();
    uint64_t next_stats = t0;
    const uint64_t stats_period =
        l.cfg.stats.hz > 0 ? static_cast<uint64_t>(1000.0 / l.cfg.stats.hz)
                           : 0;
    std::fprintf(stderr, "rx: session=%u, %zu adapters, running%s\n",
                 session, air.value->rx_adapters(),
                 qg.enabled() ? " (quiet-gap returns)" : "");
    while (g_stop == 0) {
        // One timestamp per iteration (see run_tx): callbacks and tick share
        // it so core-injected time never steps backward.
        const uint64_t now = now_ms();
        now_us_it = now_us();
        // Fire the coalesced return window. No deadline (no EOB heard yet)
        // means send immediately — never sit on a return.
        if (!ret_held.empty() &&
            (!ret_at_us || now_us_it >= *ret_at_us)) {
            for (const auto& f : ret_held) {
                air.value->inject(f.data(), f.size());
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
        if (stats_period != 0 && now >= next_stats) {
            emit_stats(emitter, l, session, t0, nullptr, &rx, &*air.value,
                       tsf_fallbacks);
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

    const RxEngine::Deliver deliver = [&](uint8_t sid, const uint8_t* d,
                                          size_t n) {
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
    // RX -> return direction -> TX (its own loss).
    const RxCore::Inject inject_nack = [&](const uint8_t* f, size_t n) {
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
        if (stats_period != 0 && loop_now >= next_stats) {
            emit_stats(emitter, l, session, t0, &tx, &rx);
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
