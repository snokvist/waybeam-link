// SPDX-License-Identifier: GPL-2.0-or-later
// The RX node: §3 receive engine, §3.5 reporting, §3.9 recovery and the
// §15.3 fill, wired together and driven by injected callbacks.
//
// FIRST PIECE OF THE `node/` LAYER (issue #109 Phase 2a). Everything here was
// in `app/main.cpp`'s anonymous namespace, which gave it internal linkage and
// made it reachable from exactly one place: `tests/app_test.cpp`, by
// `#include`-ing the whole 8.8k-line translation unit with `main()` suppressed.
// That is why two of the defects fixed in waybeam-link Passes 165-167 could
// only be proven on hardware. RxCore was the plan's designated first move
// because it is already clean: it references no other app-layer structure
// (no AirBackend, no ScoutEngine, no UplinkPower, no TxCore), only `core/`
// plus `Config` and `StatsSnapshot` from `io/`.
//
// Nothing below is rewritten. The struct is byte-identical to the one that
// left `main.cpp`, and `rx_policy()` moved with it because RxCore's
// constructor was its only caller. The layering rule this establishes:
// `node/` may use `core/` and `io/`; neither may use `node/`.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wblink/config.h"
#include "wblink/frame_reassembler.h"
#include "wblink/mcs_probe.h"
#include "wblink/recovery.h"
#include "wblink/reporter.h"
#include "wblink/rx.h"
#include "wblink/selector.h"
#include "wblink/selector_state.h"
#include "wblink/stats.h"
#include "wblink/wire.h"

namespace wblink {
namespace node {

// §15.2 -> §3 policy translation. Lived beside its siblings (arq_policy,
// selector_policy, quietgap_policy) in main.cpp; those stay there until the
// TX half of Phase 2a moves, because they have callers this layer does not.
inline RxPolicy rx_policy(const Config& cfg) {
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

struct RxCore {
    // (frame, len, target_originator) — the target rides along so returns
    // can be addressed as §3.0 unicast when return.unicast is on.
    using Inject = std::function<void(const uint8_t*, size_t, uint16_t)>;

    RxCore(const Config& cfg, uint32_t session, const ProfileTable* table,
           std::optional<uint8_t> table_version)
        : originator_(cfg.node.originator),
          session_(session),
          table_(table),
          local_table_version_(table_version),
          engine_(rx_policy(cfg), wants(cfg), table, table_version),
          reporter_(ReporterPolicy{cfg.policy.report_hz > 0
                                       ? static_cast<uint32_t>(
                                             1000.0 / cfg.policy.report_hz)
                                       : 0},
                    table_version),
          probe_window_(table, table_version, McsProbeWindow::Params{}),
          feedback_period_ms_(cfg.policy.report_hz > 0
                                  ? static_cast<uint32_t>(
                                        1000.0 / cfg.policy.report_hz)
                                  : 0),
          // §3.9 Pass 106. A spectator (§2 Pass 74) has no uplink, so it never
          // emits regardless of the knob — gated here rather than relying on
          // the return inject to no-op, so it also stays quiet in the log.
          recovery_on_latch_(cfg.node.recovery_on_latch &&
                             !cfg.node.spectator) {}

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
                const RxEngine::Deliver& deliver, int8_t rssi = 0,
                const RxEngine::EarlyDeliver& early_deliver = {},
                uint8_t rx_mcs = kUplinkRxMcsUnknown) {
        const Decoded dec = decode(d, n);
        if (const DataView* v = std::get_if<DataView>(&dec)) {
            // §9.4 Pass 163: the probe window tracks the video stream only,
            // scoped to one sender tuple — a different tuple is another
            // operating context and resets the evidence. Review fix: gate on
            // the ACCEPTED originator (0 = none yet), or an interleaving
            // stranger stream (Pass 144: they do reach this callback)
            // thrash-resets the window and silently suppresses the veto.
            if (v->hdr.stream_type == stream_type::kRtp &&
                probe_originator_ != 0 &&
                v->hdr.prefix.originator == probe_originator_) {
                const StreamKey key{v->hdr.prefix.originator,
                                    v->hdr.prefix.session_id,
                                    v->hdr.stream_id};
                if (!probe_key_ || !(*probe_key_ == key)) {
                    probe_window_.reset();
                    probe_key_ = key;
                }
                probe_window_.on_data(v->hdr.seq, v->hdr.active_profile,
                                      v->hdr.table_version, rx_mcs, now);
            }
            engine_.on_data(adapter, *v, now, deliver, rssi, early_deliver);
            return;
        }
        if (const SelectorState* s = std::get_if<SelectorState>(&dec)) {
            bool any_rtp = false;
            for (const RxStreamInfo& info : engine_.streams()) {
                if (info.stream_type != stream_type::kRtp) {
                    continue;
                }
                any_rtp = true;
                if (selector_state_admissible(
                        *s, local_table_version_, info.key.originator,
                        info.key.session_id)) {
                    // §3.15 (Pass 153): latch the accepted tuple — acceptance
                    // survives the §2 idle teardown a §3.16 pause causes.
                    word_source_ = {info.key.originator, info.key.session_id};
                    remote_selector_state_ = *s;
                    remote_selector_state_ms_ = now;
                    return;
                }
            }
            // The latch stands in ONLY while no live RTP stream exists (the
            // teardown case it exists for). With a live stream present, a
            // non-matching word is a stale session — a rebooted craft
            // re-latches through its new stream, never through the old latch.
            if (!any_rtp && word_source_ &&
                selector_state_admissible(*s, local_table_version_,
                                          word_source_->first,
                                          word_source_->second)) {
                remote_selector_state_ = *s;
                remote_selector_state_ms_ = now;
            }
        }
    }

    void complete_frame(uint8_t stream_id, uint32_t block_id, uint64_t now,
                        const RxEngine::Deliver& deliver) {
        engine_.complete_frame(stream_id, block_id, now, deliver);
    }

    bool defer_first_nack(uint8_t stream_id, uint32_t block_id,
                          uint64_t not_before_ms) {
        return engine_.defer_first_nack(stream_id, block_id, not_before_ms);
    }

    bool block_had_nack(uint8_t stream_id, uint32_t block_id) const {
        return engine_.block_had_nack(stream_id, block_id);
    }

    // §10.7: the ground's own emitted-report counter. report_epoch advances
    // once per emitted report (§3.5), so this IS the emission count — the
    // calibrator needs it only for the counter-blackout case, where the
    // craft's anchors cannot advance.
    uint32_t report_epoch() const { return reporter_.epoch(); }
    // §10.7 (Pass 132) probe burst. `report_epoch()` above is what makes this
    // work: it counts reports the radio actually TOOK, so the calibrator can
    // size a burst and then divide by the ground's own exact count instead of
    // reconstructing it from the craft's anchors.
    // The injector's half of the §3.5 emission contract: the number to stamp
    // into the frame at the radio call, and the commit that spends it.
    uint32_t next_report_epoch() const { return reporter_.next_epoch(); }
    void commit_report_epoch() { reporter_.commit_epoch(); }

    // §10.7 interlock: is the craft running its own §10.6 downlink
    // calibration? The §3.15 word already mirrors it here, so the two
    // directions can refuse to overlap without any new wire. They must not:
    // §10.7 drives ground power to min_qdb, starving the report stream that
    // every §10.6 dwell and its abort clock depend on.
    bool craft_calibrating() const { return craft_calib_state() == 1; }

    // The mirrored §3.15 calibration word's state nibble, or -1 when the
    // craft is not airing one (older build, or no selector state yet). The
    // §10.7 sequencer needs the full value, not just "running": it has to
    // tell a finished downlink phase from a failed one.
    int craft_calib_state() const {
        if (!remote_selector_state_ ||
            (remote_selector_state_->state_flags &
             selector_state_flags::kCalibPresent) == 0) {
            return -1;
        }
        return static_cast<int>(remote_selector_state_->calib_word & 0x03u);
    }

    // §3.15a report_latch_holder, or -1 when the craft is not reporting it
    // (bit3 clear = legacy build, or no selector state yet). Pass 131: this is
    // §10.7's authority signal. Pass 125 inferred the latch from a valid MAC
    // on §3.16; with the MAC gone the latch is READ from the packet that
    // already carries it, craft-owned, rather than deduced. Note -1 (not
    // reported) is deliberately distinct from 0 (reported, nobody holds it) —
    // §3.15a is explicit that a receiver MUST NOT render the first as the
    // second, and a start prerequisite that conflated them would tell an
    // operator they had lost a latch that was never being published.
    int craft_report_latch_holder() const {
        if (!remote_selector_state_ ||
            (remote_selector_state_->state_flags &
             selector_state_flags::kHolderPresent) == 0) {
            return -1;
        }
        return static_cast<int>(remote_selector_state_->report_latch_holder);
    }

    // §9.4 Pass 163 guard 3: CRC-errored descriptor rates from the radio
    // backend, delta-fed by the loop (the bodies were dropped pre-parse).
    void on_crc_frames(uint8_t rx_mcs, uint32_t count, uint64_t now) {
        probe_window_.on_crc_frames(rx_mcs, count, now);
    }

    // §9.4 Pass 163: the loop's active selection scopes the probe feed.
    void set_probe_originator(uint16_t originator) {
        probe_originator_ = originator;
    }

    void tick(uint64_t now, const RxEngine::Deliver& deliver,
              const Inject& inject_report, const Inject& inject_nack,
              bool emit_nacks = true, uint8_t link_verdict_now = 0,
              const Inject* inject_verdict = nullptr) {
        engine_.tick(now, deliver);
        // §3.5 Pass 163: refresh the up-candidate evidence for the video
        // stream before building; unfilled/stale fails closed to kNoProbe.
        if (probe_key_) {
            reporter_.set_probe_per(*probe_key_, probe_window_.probe_per(now));
        }
        // §7.3: LINK_REPORTs ride the same uplink as NACKs. The epoch is
        // stamped by the injector at the radio call, not here — see
        // Reporter::next_epoch().
        for (LinkReport r : reporter_.build(engine_, now)) {
            r.prefix.originator = originator_;
            r.prefix.destination = r.target_originator;
            r.prefix.session_id = session_;
            uint8_t frame[kLinkReportSize];
            if (encode_link_report(r, frame, sizeof(frame)) > 0) {
                inject_report(frame, sizeof(frame), r.target_originator);
            }
            // §3.16 (Pass 159): the verdict travels with report authority —
            // ≤1 Hz, only while reports flow, only when the sensor has a
            // cause (Unknown = no radio backend / nothing heard). Its OWN
            // inject path: the report injector stamps+commits a §3.5 epoch
            // per frame, which on a 23 B verdict would burn a phantom epoch
            // into the §10.7 seek.
            if (inject_verdict != nullptr &&
                link_verdict_now != link_verdict::kUnknown &&
                now - verdict_emit_ms_ >= 1000) {
                LinkVerdictPkt v;
                v.prefix.originator = originator_;
                v.prefix.destination = r.target_originator;
                v.prefix.session_id = session_;
                v.target_originator = r.target_originator;
                v.target_session = r.target_session;
                // Reporter::build leaves r.report_epoch 0 — the real epoch
                // is stamped into the FRAME at injection. The verdict wants
                // the sender's current counter (monotone, ties it to the
                // report stream) WITHOUT committing one: a burned epoch is
                // phantom loss in the §10.7 seek denominator.
                v.report_epoch = next_report_epoch();
                v.verdict = link_verdict_now;
                uint8_t vf[kLinkVerdictSize];
                if (encode_link_verdict(v, vf, sizeof(vf)) > 0) {
                    (*inject_verdict)(vf, sizeof(vf), r.target_originator);
                    verdict_emit_ms_ = now;
                }
            }
        }
        if (!emit_nacks) {
            return;
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

    void fill_stats(StatsSnapshot& snap, uint64_t now) const {
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
        bool selector_source_current = false;
        if (remote_selector_state_) {
            bool any_rtp = false;
            for (const RxStreamInfo& info : engine_.streams()) {
                if (info.stream_type != stream_type::kRtp) {
                    continue;
                }
                any_rtp = true;
                if (selector_state_admissible(
                        *remote_selector_state_, local_table_version_,
                        info.key.originator, info.key.session_id)) {
                    selector_source_current = true;
                    break;
                }
            }
            // §3.15 (Pass 153): the acceptance latch keeps the mirrored word
            // current across the §2 teardown a calibration pause causes —
            // same no-live-stream gate as the admission path.
            if (!selector_source_current && !any_rtp && word_source_ &&
                selector_state_admissible(*remote_selector_state_,
                                          local_table_version_,
                                          word_source_->first,
                                          word_source_->second)) {
                selector_source_current = true;
            }
        }
        if (selector_source_current &&
            selector_state_fresh(now, remote_selector_state_ms_)) {
            const SelectorState& s = *remote_selector_state_;
            const uint64_t age = now - remote_selector_state_ms_;
            snap.link.profile = s.active_profile;
            if (table_ != nullptr) {
                for (const Profile& p : table_->profiles) {
                    if (p.id == s.active_profile) {
                        snap.link.mcs = p.mcs;
                        break;
                    }
                }
            }
            snap.link.transition_reason = selector_reason_name(
                static_cast<SelectorReason>(s.transition_reason));
            snap.link.loss_window_milli = s.loss_window_milli;
            snap.link.loss_ewma_milli = s.loss_ewma_milli;
            snap.link.loss_uniq = s.loss_uniq;
            snap.link.loss_score = s.loss_score;
            snap.link.safe_floor_profile = s.safe_floor_profile;
            // §3.15a: bit3 clear means the craft did not report it (legacy
            // build) — distinct from "nobody holds the latch", which is bit3
            // set with holder 0.
            if ((s.state_flags & selector_state_flags::kHolderPresent) != 0) {
                snap.link.report_latch_holder = s.report_latch_holder;
                snap.link.report_latch_known = true;
            }
            snap.link.selector_state_valid = true;
            snap.link.selector_state_age_ms = static_cast<uint32_t>(age);
            snap.link.lockout_active =
                (s.state_flags & selector_state_flags::kActive) != 0;
            snap.link.lockout_latched =
                (s.state_flags & selector_state_flags::kLatched) != 0;
            snap.link.lockout_conflict =
                (s.state_flags & selector_state_flags::kConflict) != 0;
            snap.link.lockout_profile = s.lockout_profile;
            snap.link.lockout_ceiling_profile = s.ceiling_profile;
            snap.link.lockout_remaining_ms =
                s.remaining_ms > age
                    ? static_cast<uint32_t>(s.remaining_ms - age)
                    : 0;
            snap.link.lockout_strikes = s.lockout_strikes;
            snap.link.lockout_active_mask = s.lockout_active_mask;
            snap.link.lockout_latched_mask = s.lockout_latched_mask;
        }
        // §15.3 (Pass 121 addendum 6): ground mirrors the received §3.15
        // calibration word — the WebUI's only progress view when the craft
        // has no IP path. Deliberately OUTSIDE the freshness gate: the
        // word is sticky across RF gaps (calibration's own wall probes
        // black out the link; a freshness-gated mirror flickers "idle"
        // mid-run). calib_stale is craft-local and stays false here.
        if (remote_selector_state_ &&
            (remote_selector_state_->state_flags &
             selector_state_flags::kCalibPresent) != 0) {
            static const char* const kCalibNames[4] = {"idle", "running",
                                                       "done", "failed"};
            const SelectorState& cs = *remote_selector_state_;
            snap.link.calib_state = kCalibNames[cs.calib_word & 0x03u];
            snap.link.calib_rung = (cs.calib_word >> 2) & 0x07u;
            snap.link.calib_fingerprint = cs.calib_fingerprint;
        }
    }

    // §15.5 stats/reset. The frame-shm reassemblers live in run_rx (ShmOut),
    // so the caller resets those; here we zero the RX engine's counters.
    // §3.5: the reporter's loss window deltas are taken against the engine's
    // counters, so resetting one without the other underflows the next
    // LINK_REPORT to 0 permille — failing OPTIMISTIC, which §3.5 forbids.
    void reset_stats() {
        engine_.reset_stats();
        reporter_.reset_link();
    }

    void select_originator(uint16_t originator) {
        engine_.select_originator(originator);
        reporter_.reset_link();
        next_feedback_ms_ = 0;
        remote_selector_state_.reset();
        remote_selector_state_ms_ = 0;
        word_source_.reset();  // §3.15: the latch never crosses crafts
    }

    std::optional<uint16_t> selected_originator() const {
        return engine_.selected_originator();
    }

    // §15.5 Pass 108: the originator this node is actually LATCHED to, as
    // opposed to selected_originator()'s configured output-want pin (which is
    // nullopt on the common ground that names no preferred_originator — the
    // reason latch adoption cannot read it). nullopt when nothing is latched,
    // or when latched streams disagree: an ambiguous binding is no binding.
    std::optional<uint16_t> latched_originator() const {
        std::optional<uint16_t> found;
        for (const RxStreamInfo& info : engine_.streams()) {
            if (found && *found != info.key.originator) return std::nullopt;
            found = info.key.originator;
        }
        return found;
    }

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

    // §3.9 Pass 106: fresh latch is itself a decoder-bootstrap event. The link
    // cannot see decoder readiness (VFRM v1 carries no consumer generation,
    // §15.4) and a GDR craft emits no IRAP unasked, so the first latch is both
    // the only moment we can detect and the only one at which an IRAP is
    // guaranteed absent from everything the consumer will ever see. Bounded at
    // kLatchRecoveryAttempts: the stop condition is unobservable on RTP egress,
    // and a craft with venc.recovery_enabled false would otherwise draw a
    // return every second for the whole flight.
    void emit_latch_recovery(uint64_t now, const Inject& inject) {
        if (!recovery_on_latch_) return;
        // Called once per rx-loop iteration: reuse the scratch buffer so the
        // steady state costs a scan and no allocation. due() likewise returns
        // an empty vector (no allocation) on the overwhelmingly common tick
        // where nothing is due.
        latch_scratch_.clear();
        for (const RxStreamInfo& info : engine_.streams()) {
            if (info.stream_type != stream_type::kRtp) continue;
            latch_scratch_.push_back(LatchStream{info.key, info.local_stream_id});
        }
        for (const StreamKey& key : latch_recovery_.due(latch_scratch_, now)) {
            RecoveryRequest req;
            req.prefix = {originator_, key.originator, session_};
            req.target_originator = key.originator;
            req.target_session = key.session_id;
            req.target_stream_id = key.stream_id;
            uint8_t frame[kRecoveryRequestSize];
            if (encode_recovery_request(req, frame, sizeof(frame)) !=
                sizeof(frame)) {
                continue;
            }
            inject(frame, sizeof(frame), req.target_originator);
            std::fprintf(stderr,
                         "rx: latch recovery stream=%u origin=%u attempt %u/%u\n",
                         key.stream_id, key.originator,
                         unsigned(latch_recovery_.attempts(key)),
                         unsigned(LatchRecovery::kAttempts));
        }
    }

    // §3.9 Pass 106 early exit: an IRAP-flagged frame reaching a frame-SHM
    // egress ring is the link's own observable proxy for "the consumer now has
    // something to start from". RTP egress has no equivalent — there the
    // attempt bound is the only stop.
    void note_egress_irap(uint8_t local_stream_id) {
        latch_recovery_.note_irap(local_stream_id);
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
    const ProfileTable* table_;
    std::optional<uint8_t> local_table_version_;
    RxEngine engine_;
    Reporter reporter_;
    // §9.4 Pass 163: RX probe evidence for the accepted sender's video
    // stream (always-on — rate-verification keeps it inert when nothing
    // probes or the backend has no PHY rate).
    McsProbeWindow probe_window_;
    std::optional<StreamKey> probe_key_;
    uint16_t probe_originator_ = 0;  // accepted sender; 0 = feed disabled
    uint64_t verdict_emit_ms_ = 0;  // §3.16 Pass 159 ≤1 Hz emit guard
    uint32_t feedback_period_ms_ = 0;
    uint64_t next_feedback_ms_ = 0;
    uint32_t feedback_epoch_ = 0;
    bool recovery_on_latch_ = false;
    LatchRecovery latch_recovery_;
    std::vector<LatchStream> latch_scratch_;  // reused; see emit_latch_recovery
    std::optional<SelectorState> remote_selector_state_;
    uint64_t remote_selector_state_ms_ = 0;
    // §3.15 (Pass 153) acceptance latch: the (originator, session) tuple of
    // the last summary accepted through a live-consumed RTP stream.
    std::optional<std::pair<uint16_t, uint32_t>> word_source_;
};

}  // namespace node
}  // namespace wblink
