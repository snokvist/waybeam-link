// SPDX-License-Identifier: GPL-2.0-or-later
// The TX node: framers, the send ring, the §9 selector, §10 power ownership,
// §10.6 calibration and the §11.7 command surface — fourth and largest move of
// the node/ layer (issue #109 Phase 2a).
//
// 1812 lines, and the piece the plan flagged as genuinely entangled with
// `run_rx`. It turned out to have NO app-layer code dependency: the two
// `UplinkPower` mentions in it are comments about the ground half, not calls.
// What it does need are three §15.2 -> core policy adapters, which move with
// it because `selector_policy()`'s only caller was TxCore and the other two
// are read by both halves.
//
// `resolve_power_qdb()` and `load_power_curve()` are NOT here: they already
// live in `io/` (`wblink/power_file.h`), which is the layering working —
// a node-layer object reaching down to io/ is expected; the reverse is not.
//
// Layering rule (CLAUDE.md): node/ may use core/ and io/; neither may use
// node/.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wblink/calibrate.h"
#include "wblink/fps_ladder.h"
#include "wblink/frame_framer.h"
#include "wblink/jscc_runtime_shadow.h"
#include "wblink/log.h"
#include "wblink/mcs_probe.h"
#include "wblink/frame_caps.h"
#include "wblink/frame_shm.h"
#include "wblink/framer.h"
#include "wblink/scheduler.h"
#include "wblink/config.h"
#include "wblink/node/aim.h"
#include "wblink/node/clock.h"
#include "wblink/power_file.h"
#include "wblink/report_gate.h"
#include "wblink/selector.h"
#include "wblink/stats.h"
#include "wblink/venc.h"
#include "wblink/wire.h"

namespace wblink {
namespace node {

inline uint8_t bw_code(uint8_t width_mhz) {
    return width_mhz >= 80 ? 2 : width_mhz >= 40 ? 1 : 0;
}

inline SchedulerPolicy scheduler_policy(const Config& cfg) {
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

inline uint32_t s_to_ms(double s) {
    return s <= 0.0 ? 0u : static_cast<uint32_t>(s * 1000.0 + 0.5);
}

inline SelectorPolicy selector_policy(const Config& cfg) {
    const SelectPolicy& s = cfg.policy.select;
    SelectorPolicy p;
    p.demote_milli = s.demote_milli;
    p.emergency_loss_milli = s.emergency_loss_milli;
    p.loss_min_uniq = s.loss_min_uniq;
    p.loss_persist_score = s.loss_persist_score;
    p.rung_lockout_ms = s_to_ms(s.rung_lockout_s);
    p.rung_lockout_latch_count = s.rung_lockout_latch_count;
    p.rssi_floor_dbm = s.rssi_floor_dbm;
    p.rssi_fade_db_per_s = s.rssi_fade_db_per_s;
    p.rssi_fade_arm_dbm = s.rssi_fade_arm_dbm;
    p.down_cooldown_ms = s_to_ms(s.down_cooldown_s);
    p.ewma_alpha = s.ewma_alpha;
    p.rung_rssi_floor_dbm = s.rung_rssi_floor_dbm;
    p.promote_rssi_hyst_db = s.promote_rssi_hyst_db;
    p.promote_dwell_ms = s_to_ms(s.promote_dwell_s);
    p.verdict_ttl_ms = s_to_ms(s.verdict_ttl_s);  // §9.4 Pass 160
    p.probe_veto_permille = s.probe_veto_permille;  // §9.4 Pass 163
    p.probe_veto_ttl_ms = s_to_ms(s.probe_veto_ttl_s);
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
    p.max_bitrate_kbps = cfg.venc.max_bitrate_kbps;  // §9.6 Pass 75 ceiling
    p.report_timeout_ms = cfg.policy.report_timeout_ms;
    p.failsafe_hold_ms = s_to_ms(s.failsafe_hold_s);
    p.failsafe_step_ms = s_to_ms(s.failsafe_step_s);
    p.pressure_escape_ms = s_to_ms(s.pressure_escape_s);
    return p;
}

// §15.2 policy.calibration -> the core engine seeds. Shared: §10.6's craft
// calibrator and §10.7's ground uplink calibrator read the same block, and
// only the gating differs (§10.7 uses epoch counts, not the ms dwells).
inline CalibrateParams calib_params_from(const CalibrationPolicy& c) {
    CalibrateParams p;
    p.loss_ok_milli = static_cast<uint16_t>(c.loss_ok_milli);
    p.loss_bad_milli = static_cast<uint16_t>(c.loss_bad_milli);
    p.seek_step_qdb = c.seek_step_qdb;
    p.rssi_guard_dbm = c.rssi_guard_dbm;
    p.min_qdb = c.min_qdb;
    p.max_qdb = c.max_qdb;
    p.settle_ms = static_cast<uint32_t>(c.settle_ms);
    p.dwell_probe_frames = static_cast<uint16_t>(c.dwell_probe_frames);
    p.dwell_verify_frames = static_cast<uint16_t>(c.dwell_verify_frames);
    p.dwell.probe_pace_us = static_cast<uint32_t>(c.probe_pace_us);
    p.dwell.tally_wait_ms = static_cast<uint32_t>(c.tally_wait_ms);
    p.dwell.tally_retries = static_cast<uint32_t>(c.tally_retries);
    p.hard_cap_ms = static_cast<uint32_t>(c.hard_cap_ms);
    return p;
}

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
        // §14.2 enforcement (Pass 38). NOTE: every counter below must also
        // be zeroed in reset_stats() — two of them were not, and a §15.5
        // reset left them at lifetime totals against restarted denominators.
        bool jscc_enforce = false;
        uint64_t jscc_enforced_frames = 0;
        uint64_t jscc_discarded_frames = 0;
        // §14.2 Pass 149: valid decisions skipped because the frame is
        // non-referenced (§14.1a) and exempt from enforcement entirely.
        uint64_t jscc_exempt_frames = 0;
    };

    TxCore(const Config& cfg, uint32_t session, const ProfileTable* table,
           uint8_t table_version,
           uint16_t mtu_supported = kDefaultMaxPayload)
        : originator_(cfg.node.originator),
          session_(session),
          table_version_(table_version),
          table_(table),
          selector_(selector_policy(cfg), table),
          venc_(cfg.venc),
          venc_knobs_(cfg.venc),
          arq_max_fps_(cfg.policy.arq.arq_max_fps),
          mtu_supported_(mtu_supported),
          boot_min_profile_(cfg.policy.select.min_profile),
          boot_max_profile_(cfg.policy.select.max_profile),
          feedback_gate_(ReportGatePolicy{
              cfg.node.preferred_originator,
              cfg.policy.report_timeout_ms * 4}),
          report_gate_(ReportGatePolicy{
              cfg.node.preferred_originator,
              cfg.policy.report_timeout_ms * 4}) {
        // §9.11 FPS ladder (Pass 39; instantiate-vs-run split Pass 99). The
        // object is instantiated on every venc craft — its existence commands
        // nothing. `fps_ladder.enabled` sets only the BOOT run-state
        // (cmd_fps_enabled_), so FPS_LADDER on/off (§11.7 0x03 / the craft-local
        // POST /api/v1/link/fps) toggles the loop both ways at runtime with no
        // link restart — which is what makes the variable-fps mode switchable.
        if (cfg.venc.enabled) {
            const FpsLadderCfg& lc = cfg.venc.fps_ladder;
            FpsLadderPolicy fp;
            fp.min_fps = lc.min;
            fp.preferred_fps = lc.preferred;
            fp.min_p_frame_bytes = lc.min_p_frame_bytes;
            fp.restore_hysteresis_bytes = lc.restore_hysteresis_bytes;
            fp.sample_timeout_ms = lc.sample_timeout_ms;
            fp.reduce_after_ms = lc.reduce_after_ms;
            fp.reduce_dwell_ms = lc.reduce_dwell_ms;
            fp.restore_after_ms = lc.restore_after_ms;
            fp.settle_ms = lc.settle_ms;
            fps_ladder_.emplace(fp);
            cmd_fps_enabled_ = lc.enabled;  // static mode boots the loop off
        }
        // §10: one power curve per TX adapter with an authored map. The
        // resolve happens at profile commit and actuates through the
        // apply_power hook (§10.5); on the udp dev backend it stays a logged
        // intent.
        for (size_t i = 0; i < cfg.adapters.size(); ++i) {
            const AdapterCfg& a = cfg.adapters[i];
            if (a.role != Role::kTx) {
                continue;
            }
            // §10.5 override targets: EVERY tx adapter, curve or not.
            power_targets_.push_back(PowerTarget{
                a.name, i, a.max_power_qdb, a.power_presets_qdb,
                a.power_offset_presets_qdb, a.power_offset_qdb,
                a.power_offset_max_qdb});
            if (a.power_map.empty()) {
                continue;
            }
            auto curve =
                load_power_curve(a.power_map, a.channel_mhz >= 4000);
            if (!curve) {
                wb_logf("power: %s: %s\n", a.name.c_str(),
                        curve.error.c_str());
                continue;
            }
            power_.push_back(PowerAdapter{
                a.name, i, *curve.value, a.max_power_qdb, a.power_presets_qdb,
                a.power_offset_presets_qdb, a.power_offset_max_qdb,
                std::nullopt});
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
                fc.fec.e_rate_permille = s.fec.e_rate_permille;  // §14.1a
                fc.fec.min_k = s.fec.min_k;
                fc.fec.min_r = s.fec.min_r;
                st.frame_framer.emplace(fc);
                st.frame_framer->set_operating_point(0, table_version,
                                                     profile_max_payload_for(0));
                st.frame_framer->set_negotiated_packet_budget(
                    negotiated_packet_budget_);
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
        for (const AdapterCfg& a : cfg.adapters) {
            if (a.role == Role::kTx) {
                (void)selector_.on_rf_environment(a.channel_mhz,
                                                  bw_code(a.bw), 0);
                break;
            }
        }
    }

    // §5.1a/§9.3a packet budget: profile policy ceiling intersected with the
    // currently accepted claimed-ground ceiling.
    uint16_t profile_max_payload_for(uint8_t profile_id) const {
        if (table_ != nullptr) {
            for (const Profile& p : table_->profiles) {
                if (p.id == profile_id) {
                    return p.max_payload;
                }
            }
        }
        return kDefaultMaxPayload;
    }
    uint16_t max_payload_for(uint8_t profile_id) const {
        return std::min(profile_max_payload_for(profile_id),
                        negotiated_packet_budget_);
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
            VencFrameMeta meta;
            const bool have_meta = read_frame_meta(blob, len, &meta);
            const bool idr = have_meta && (meta.flags & kFrameFlagIdr) != 0;
            // §14.1a non-referenced class (IDR wins, matching FrameFramer).
            const bool enhance =
                have_meta && !idr && (meta.flags & kFrameFlagEnhance) != 0;
            if (fps_ladder_ && have_meta && !idr) {
                fps_ladder_->note_p_frame(
                    static_cast<uint32_t>(std::min<size_t>(
                        len - kVencFrameMetaSize, UINT32_MAX)),
                    now);
            }
            if (s.jscc_shadow && have_meta) {
                const uint16_t symbol = s.frame_framer->symbol_size();
                const size_t k_sz = 1u + (len - 1u) / symbol;
                const uint16_t k = static_cast<uint16_t>(
                    std::min<size_t>(k_sz, UINT16_MAX));
                const size_t source_bytes =
                    len + static_cast<size_t>(k) *
                              (kDataHeaderSize + kFecSourceSubheaderSize);
                const size_t resend_bytes =
                    kDataHeaderSize + kFecSourceSubheaderSize + symbol;
                const uint16_t source_packet_budget = static_cast<uint16_t>(
                    symbol + kDataHeaderSize + kFecSourceSubheaderSize);
                JsccShadowFrameInput input;
                input.source_k = k;
                input.deadline_us = frame_deadline_us(idr);
                // §14.1a: a non-referenced frame is never ARQ-eligible, so the
                // shadow must not model an ARQ that cannot occur.
                input.arq_capable =
                    (idr || (s.frame_framer->arq_mode() ==
                                 FrameArqMode::kAllFrames &&
                             !enhance)) &&
                    !arq_fps_suppressed_;  // §4.1 Pass 40 cutoff
                input.now_ms = now;
                if (estimate_airtime) {
                    input.source_tx_remaining_us =
                        estimate_airtime(source_bytes, true,
                                         source_packet_budget);
                    input.resend_airtime_us =
                        estimate_airtime(resend_bytes, false,
                                         source_packet_budget);
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
                    if (enhance) {
                        // §14.2 (Pass 149, operator ruling): non-referenced
                        // frames are exempt from enforcement ENTIRELY — no
                        // parity replacement (rule 1), no deadline discard
                        // (rule 2), and rule 3 is moot since §14.1a makes the
                        // class ARQ-ineligible. The shadow still evaluated it
                        // above, so telemetry stays comparable.
                        ++s.jscc_exempt_frames;
                    } else if (s.jscc_latest.decision.discard) {
                        ++s.jscc_discarded_frames;  // rule 2: drop, not queue
                        return;
                    } else {
                        s.frame_framer->set_next_frame_override(
                            s.jscc_latest.decision.parity_symbols,
                            s.jscc_latest.decision.arq_eligible);
                        ++s.jscc_enforced_frames;
                    }
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
    // rx_rssi/rx_mcs are the AirRxMeta of the frame carrying this packet —
    // §3.16 needs both at the accepted-LINK_REPORT point. Defaulted so the
    // loopback path, which has no PHY, stays a one-line call.
    bool on_air(const uint8_t* d, size_t n, uint64_t now, int8_t rx_rssi = 0,
                uint8_t rx_mcs = kUplinkRxMcsUnknown) {
        const Decoded dec = decode(d, n);
        if (const JsccFeedback* f = std::get_if<JsccFeedback>(&dec)) {
            if (f->target_originator != originator_ ||
                f->target_session != session_ ||
                !feedback_gate_.accept(f->prefix.originator,
                                       f->prefix.session_id, now)) {
                return false;
            }
            for (Stream& s : streams_) {
                if (s.stream_id == f->target_stream_id && s.jscc_shadow) {
                    s.jscc_shadow->observe_feedback(*f, now);
                    return false;
                }
            }
            return false;
        }
        if (const NackView* nack = std::get_if<NackView>(&dec)) {
            if (nack->hdr.target_originator != originator_ ||
                nack->hdr.target_session != session_) {
                return false;
            }
            if (!cmd_arq_enabled_) {
                return false;  // §11.7 ARQ off: serve no NACKs
            }
            for (Stream& s : streams_) {
                if (s.stream_id == nack->hdr.target_stream_id) {
                    s.sched.on_nack(*nack, s.ring, now);
                    return true;
                }
            }
            return false;
        }
        if (const LinkReport* r = std::get_if<LinkReport>(&dec)) {
            if (r->target_originator != originator_ ||
                r->target_session != session_) {
                return false;
            }
            // §7.3 Pass 79: selection feedback keys on RTP streams only.
            // Defensive against a pre-79 ground still reporting non-video
            // streams (mixed-version fleet).
            bool video_stream = false;
            for (const Stream& s : streams_) {
                if (s.stream_id == r->target_stream_id &&
                    s.stream_type == stream_type::kRtp) {
                    video_stream = true;
                    break;
                }
            }
            if (!video_stream) {
                return false;
            }
            // §3.5 acceptance filter (Pass 41): preferred/latched reporters
            // only — BEFORE the selector and the §9.11 ladder consume it.
            if (!report_gate_.accept(r->prefix.originator,
                                     r->prefix.session_id, now)) {
                return false;
            }
            // Pass 78: count selector-fresh epochs only — redundant copies
            // (return.report_redundancy) and replays must not inflate the
            // §15.3 heard-ratio.
            if (selector_.on_report(*r, now)) {
                ++reports_received_;
            }
            return false;
        }
        if (const LinkVerdictPkt* v = std::get_if<LinkVerdictPkt>(&dec)) {
            // §3.16 (Pass 159) acceptance = the §3.5 filter: addressed to
            // us, target tuple is us, sender is the report-latched tuple,
            // epoch monotone (a reordered verdict must not regress the
            // craft's view). Anything else drops without counting.
            if (v->prefix.destination != originator_ ||
                v->target_originator != originator_ ||
                v->target_session != session_) {
                return false;
            }
            if (report_gate_.latched_originator() != v->prefix.originator) {
                return false;
            }
            const uint32_t ls = report_gate_.latched_session();
            if (ls != 0 && ls != v->prefix.session_id) return false;
            // The monotone gate is SCOPED to the sender identity — a ground
            // reboot restarts its epoch counter near 1, and an unscoped
            // high-water mark would drop the rebooted ground's verdicts for
            // thousands of epochs (mirrors on_report's per-identity epoch
            // domain, selector.cpp).
            const std::pair<uint16_t, uint32_t> vid{v->prefix.originator,
                                                    v->prefix.session_id};
            if (!verdict_epoch_src_ || *verdict_epoch_src_ != vid) {
                verdict_epoch_src_ = vid;
                verdict_epoch_seen_ = 0;
            }
            if (verdict_epoch_seen_ != 0 &&
                v->report_epoch < verdict_epoch_seen_) {
                return false;
            }
            verdict_epoch_seen_ = v->report_epoch;
            selector_.on_verdict(v->verdict, now);
            verdict_rx_ms_ = now;
            return false;
        }
        if (const CalibProbe* pr = std::get_if<CalibProbe>(&dec)) {
            // §3.16 acceptance: addressed to us, from the report-latched
            // ground tuple — the only peer whose probes may pause the feed
            // (D-C ruling) or draw tallies.
            if (pr->prefix.destination != originator_) return false;
            if (report_gate_.latched_originator() != pr->prefix.originator) {
                return false;
            }
            const uint32_t ls = report_gate_.latched_session();
            if (ls != 0 && ls != pr->prefix.session_id) return false;
            const DwellTallyOut t = calib_rx_.on_probe(
                pr->run_id, pr->dwell_id, pr->seq, pr->count, rx_rssi, rx_mcs,
                now);
            if (t.new_run) {
                wb_logf("calibrate: uplink run %u from ground %u — "
                        "video feed PAUSED (input-starve, §3.16)\n",
                        t.run_id, pr->prefix.originator);
            }
            if (t.send && send_calib) {
                CalibTally out;
                out.prefix.originator = originator_;
                out.prefix.destination = pr->prefix.originator;
                out.prefix.session_id = session_;
                out.run_id = t.run_id;
                out.dwell_id = t.dwell_id;
                out.received = t.received;
                out.rssi_sum_dbm = t.rssi_sum_dbm;
                out.rx_mcs = t.rx_mcs;
                out.adapter_fingerprint = calib_ident_fingerprint_;
                uint8_t tb[kCalibTallySize];
                const size_t tn = encode_calib_tally(out, tb, sizeof tb);
                if (tn != 0) {
                    send_calib(tb, tn);
                    ++calib_tallies_tx_;
                }
            }
            return false;
        }
        if (const CalibTally* t = std::get_if<CalibTally>(&dec)) {
            // §3.16: our own §10.6 run's evidence, from the accepted reporter.
            if (t->prefix.destination != originator_) return false;
            if (report_gate_.latched_originator() != t->prefix.originator) {
                return false;
            }
            const uint32_t ls = report_gate_.latched_session();
            if (ls != 0 && ls != t->prefix.session_id) return false;
            if (calibrator_) {
                calibrator_->on_tally(t->run_id, t->dwell_id, t->received,
                                      t->rssi_sum_dbm, t->rx_mcs,
                                      t->adapter_fingerprint);
                ++calib_tallies_rx_;
                calib_rx_mcs_ = t->rx_mcs;
            }
            return false;
        }
        if (std::holds_alternative<ExtUnknown>(dec)) {
            return false;  // §3.16: newer peer — feature unavailable
        }
        if (const RecoveryRequest* r = std::get_if<RecoveryRequest>(&dec)) {
            if (r->target_originator != originator_ ||
                r->target_session != session_) {
                return false;
            }
            for (const Stream& s : streams_) {
                if (s.stream_id == r->target_stream_id &&
                    s.stream_type == stream_type::kRtp) {
                    const bool queued = venc_.request_idr(now);
                    wb_logf("venc: decoder recovery stream=%u requester=%u %s\n",
                            r->target_stream_id, r->prefix.originator,
                            queued ? "requested" : "suppressed");
                    return false;
                }
            }
        }
        return false;
    }

    void drain_resends(uint64_t now, const Inject& inject_resend) {
        for (Stream& s : streams_) {
            s.ring.evict(now);
            s.sched.drain(s.ring, now, [&](const uint8_t* f, size_t l) {
                inject_resend(f, l);
            });
        }
    }

    void tick(uint64_t now, const Inject& inject,
              const Inject& inject_resend = {}) {
        const SelectorActions act = selector_.tick(now);
        if (act.commit) {
            // §9.5 commit: the operating point stamped on every DATA packet
            // (drives RX deadlines + supersession budgets)...
            const uint16_t mp = profile_max_payload_for(act.commit->profile_id);
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
            // §9.4 Pass 163/186: re-derive the up-candidate for the new
            // operating point through the live table and the live §9.7 pin.
            refresh_probe(act.commit->profile_id);
            // ...and the §10 per-adapter power resolve, applied inside the
            // same sequenced transition through the apply_power hook (both RF
            // backends, §10.5; a logged intent on the udp dev backend). While
            // the §10.5 override-latch is set, the resolve YIELDS — the
            // latched value already sits on the hardware.
            last_commit_mcs_ = act.commit->mcs;
            last_commit_level_ = act.commit->tx_power_level;
            if (!power_override_ && !calibrating()) {
                resolve_and_apply_power(act.commit->mcs,
                                        act.commit->tx_power_level);
            }
        }
        // B1: advance the non-blocking venc HTTP state machine once per loop
        // iteration (never blocks). The setters below only record the desired
        // value; this drives the actual connect/send/recv.
        venc_.poll(now);
        // §10.6: drive the calibration engine at loop cadence.
        calibrate_service(now);
        calibrate_probe_service(now);
        // §3.16 (Pass 153): resume a probe-paused feed once the ground's
        // uplink run has gone quiet — the bounded, self-clearing D-C edge.
        if (calib_rx_.run_id() != 0 &&
            calib_rx_.quiet_for(now, feed_quiet_ms_)) {
            calib_rx_.expire_run();
            wb_logf("calibrate: uplink probes quiet — video feed "
                    "RESUMED\n");
        }
        // Push the CURRENT target every tick: write-on-change (§9.6) makes
        // this a no-op normally, and a failed push (encoder briefly down)
        // retries next tick instead of waiting for the next rung change.
        if (selector_.bitrate_kbps() > 0) {
            venc_.set_bitrate(selector_.bitrate_kbps());
        }
        // §9.6 cadence estimate: frames over a ~1 s window. Frozen while
        // the feed is paused (Pass 153): a starved input is not evidence
        // about the encoder's cadence, and the §9.6/§9.11 ladder inputs must
        // hold at their pre-pause values.
        if (feed_paused()) {
            cadence_frames_ = 0;
            cadence_start_ms_ = now;
        } else if (cadence_start_ms_ != 0 && now >= cadence_start_ms_ + 1000) {
            frame_cadence_us_ = cadence_frames_ > 0
                ? (now - cadence_start_ms_) * 1000ull / cadence_frames_
                : 0;
            cadence_frames_ = 0;
            cadence_start_ms_ = now;
        }
        // §9.11 FPS ladder: preserve measured P-frame FEC block size. Bitrate
        // and cap transitions get first claim on the resulting frame evidence.
        // §11.7 FPS_LADDER off: the loop stops issuing commands and the fps
        // holds where it is (current_fps stays the cadence input below).
        if (fps_ladder_ && cmd_fps_enabled_) {
            fps_ladder_->tick(now, venc_.settling(now));
            // Re-offer the current target every tick. VencActuator dedupes a
            // successfully applied value and retries after transient HTTP or
            // shared-holdoff failures, so controller state cannot outrun venc.
            venc_.set_fps(fps_ladder_->current_fps());
        } else if (cmd_fps_select_hz_ != 0) {
            // §11.7 FPS_SELECT (Pass 71): same re-offer discipline while no
            // ladder runs, so a transient HTTP failure cannot lose a one-shot
            // command. Cleared when FPS_LADDER on takes back ownership.
            venc_.set_fps(cmd_fps_select_hz_);
        }
        // §4.1 Pass 40 high-cadence ARQ cutoff, driven by the same cadence
        // input the §9.6 caps use (ladder-commanded, else measured, else
        // hint). Sticky on the framers until the cadence drops back.
        {
            const uint32_t snapped = snap_frame_period_us(cadence_period_us());
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
                in.frame_period_us = snap_frame_period_us(cadence_period_us());
                in.iframe_deadline_ms = static_cast<uint16_t>(
                    frame_deadline_us(true) / 1000u);
                const FrameFecConfig& fec = s.frame_framer->fec();
                if (fec.scheme != FecScheme::kNone) {
                    in.i_rate_permille = fec.i_rate_permille;
                    // §9.6 caps stay on the P rate even when §14.1a lowers the
                    // non-referenced class: max_p_bytes is then conservative
                    // (net of more parity than is actually emitted), so freed
                    // airtime is NOT handed back to the encoder. Deliberate —
                    // e_rate has no validated non-default setting (§14.1a), and
                    // a blended rate would have to track measured density.
                    in.p_rate_permille = fec.p_rate_permille;
                }
                in.symbol_size = static_cast<uint16_t>(
                    max_payload_for(selector_.profile_id()) -
                    kDataHeaderSize - kFecRepairSubheaderSize);
                in.ceiling_bytes = venc_knobs_.cap_ceiling_bytes;
                in.i_headroom_permille = venc_knobs_.i_headroom_permille;
                in.p_headroom_permille = venc_knobs_.p_headroom_permille;
                const FrameCaps caps = derive_frame_caps(in);
                venc_.set_max_frame_size(caps.max_i_bytes, caps.max_p_bytes);
                break;  // single video stream (§9.6)
            }
        }
        drain_resends(now, inject_resend ? inject_resend : inject);
    }

    void set_pressure(bool on, uint64_t now) {  // §9.9 gauge (step 9+ feeds it)
        selector_.set_pressure(on, now);
    }


    // §11.3: freeze the cascade + pause the watchdog across the CSA blackout.
    void csa_freeze(uint64_t until_ms) { selector_.csa_freeze(until_ms); }

    // §3.16 (Pass 153): receiver half of the dwell primitive — counts the
    // ground's uplink probes and answers tallies; its run lifecycle also
    // drives the D-C feed pause.
    DwellReceiver calib_rx_;
    Inject send_calib;  // wired to the air inject in run_tx
    uint8_t calib_ident_fingerprint_ = 0;
    uint32_t feed_quiet_ms_ = 2000;  // §15.2 feed_quiet_ms
    // §15.3 probe-exchange counters (role-neutral).
    uint64_t calib_probes_tx_ = 0;
    uint64_t calib_tallies_rx_ = 0;
    uint64_t calib_tallies_tx_ = 0;
    uint8_t calib_rx_mcs_ = kUplinkRxMcsUnknown;

    // §3.5 Pass 115: report authority is ONE authority across both return
    // gates. These wrap the pair deliberately — moving report_gate_ alone
    // would leave §3.10 JSCC feedback flowing from the displaced ground,
    // which presents as a working fix. Keep the two calls together.
    void report_authority_set(uint16_t originator, uint64_t now_ms) {
        report_gate_.force_latch(originator, now_ms);
        feedback_gate_.force_latch(originator, now_ms);
        // §12 Pass 116: repairs follow the claim too, per-stream — the
        // scheduler owns one lock each. Soft: an actively-NACKing node
        // reclaims via contested release, unlike the report latch.
        for (Stream& s : streams_) {
            s.sched.force_lock(originator);
        }
    }
    void report_authority_clear() {
        report_gate_.clear_latch();
        feedback_gate_.clear_latch();
        for (Stream& s : streams_) {
            s.sched.release_lock();
        }
    }
    bool report_authority_overridable() const {
        return report_gate_.overridable();
    }
    void on_rf_environment(uint16_t channel_mhz, uint8_t bw,
                           uint64_t now_ms) {
        (void)selector_.on_rf_environment(channel_mhz, bw, now_ms);
    }

    SelectorState selector_state(uint64_t now_ms) const {
        const SelectorLockout lock = selector_.lockout(now_ms);
        SelectorState s;
        s.prefix = {originator_, 0, session_};
        s.table_version = table_version_;
        s.active_profile = selector_.profile_id();
        s.safe_floor_profile = selector_.safe_floor_profile();
        s.ceiling_profile = lock.ceiling_profile;
        s.lockout_profile = lock.profile;
        if (lock.active) {
            s.state_flags |= selector_state_flags::kActive;
        }
        if (lock.latched) {
            s.state_flags |= selector_state_flags::kLatched;
        }
        if (lock.conflict) {
            s.state_flags |= selector_state_flags::kConflict;
        }
        s.lockout_strikes = lock.active ? lock.strikes : 0;
        s.remaining_ms = static_cast<uint16_t>(
            std::min<uint32_t>(lock.remaining_ms, 0xFFFFu));
        s.transition_reason = static_cast<uint8_t>(selector_.reason());
        s.loss_window_milli = selector_.loss_window_milli();
        s.lockout_active_mask = lock.active_mask;
        s.lockout_latched_mask = lock.latched_mask;
        s.loss_ewma_milli = selector_.loss_ewma_milli();
        s.loss_uniq = selector_.loss_uniq();
        s.loss_score = selector_.loss_score();
        // §3.15a Pass 117: name the §3.5 latch holder on air. This build
        // always carries it, so bit3 is always set and the packet is 34 bytes.
        s.state_flags |= selector_state_flags::kHolderPresent;
        s.report_latch_holder = report_gate_.latched_originator();
        if (calibrator_) {  // §10.6 Pass 120: bit4 word (implies bit3)
            s.state_flags |= selector_state_flags::kCalibPresent;
            s.calib_word = calibrator_->word();
            s.calib_fingerprint = calib_fingerprint_;
        }
        return s;
    }
    // §3.16 (Pass 153): the CRC-8 of this craft's TX-adapter canonical
    // calibration identity — stamped into every TALLY we return so the
    // ground's artifact can identity-gate its evidence source.
    void set_calib_identity(uint8_t fingerprint) {
        calib_ident_fingerprint_ = fingerprint;
    }

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
        apply_profile_pin(min_profile, max_profile);
    }
    // §15.5 Pass 103: forget the venc actuator's write-on-change cache so the
    // next tick re-asserts bitrate/caps/fps — called after an out-of-loop venc
    // restart (the §16 mode applier) which the actuator cannot otherwise see.
    void reassert_venc() { venc_.invalidate(); }
    // §14.1 live FEC-rate retune for a frame-shm stream. Returns false if the
    // stream_id is unknown or is not a frame-shm (FrameFramer) stream.
    bool set_stream_fec(uint8_t stream_id, uint16_t i_permille,
                        uint16_t p_permille, uint16_t min_k, uint16_t min_r,
                        std::optional<uint16_t> e_permille) {
        for (Stream& s : streams_) {
            if (s.stream_id != stream_id) {
                continue;
            }
            if (!s.frame_framer) {
                return false;  // udp stream: no per-stream FEC (§15.2)
            }
            s.frame_framer->set_fec_rates(i_permille, p_permille, min_k, min_r,
                                          e_permille);
            return true;
        }
        return false;
    }
    // §9.6/§9.11 cadence input: ladder-commanded, else §11.7 FPS_SELECT
    // (Pass 71 — authoritative immediately, same rule as a ladder command),
    // else measured, else hint.
    uint64_t cadence_period_us() const {
        if (fps_ladder_ && fps_ladder_->current_fps() > 0) {
            return 1000000ull / fps_ladder_->current_fps();
        }
        if (cmd_fps_select_hz_ != 0) {
            return 1000000ull / cmd_fps_select_hz_;
        }
        return frame_cadence_us_ != 0 ? frame_cadence_us_
                                      : 1000000ull / venc_knobs_.fps_hint;
    }
    // §11.7 remote command actuation (VcmdCraft::Apply). false = REJECTED —
    // unknown cmd_id, out-of-range arg, or unconfigured actuator.
    bool apply_command(uint8_t cmd_id, uint8_t arg, uint64_t now) {
        switch (cmd_id) {
            case vcmd_id::kArq:
                if (arg > 1) return false;
                cmd_arq_enabled_ = arg != 0;
                for (Stream& s : streams_) {
                    if (s.framer) {
                        s.framer->set_arq_enabled(cmd_arq_enabled_);
                    }
                    if (s.frame_framer) {
                        s.frame_framer->set_arq_enabled(cmd_arq_enabled_);
                    }
                }
                return true;  // all-off boot config ⇒ acked no-op (§11.7)
            case vcmd_id::kSelector:
                if (arg > 1) return false;
                cmd_selector_frozen_ = arg != 0;
                // §11.7: the pin lands at the next evaluate() — during a
                // §11.3 freeze the rung cannot move, so sampling now equals
                // sampling at freeze-lift.
                if (cmd_selector_frozen_) {
                    const uint8_t p = selector_.profile_id();
                    apply_profile_pin(p, p);
                } else {
                    apply_profile_pin(boot_min_profile_,
                                                      boot_max_profile_);
                }
                return true;
            case vcmd_id::kFpsLadder:
                if (arg > 1) return false;
                if (!fps_ladder_) {
                    return false;  // §11.7: the ladder did not run at boot
                }
                if (arg != 0 && !cmd_fps_enabled_) {
                    fps_ladder_->resume(now);  // §9.11 settle semantics
                    // §11.7 (Pass 71): the ladder takes back video0.fps; a
                    // later ladder-off holds fps where the ladder left it.
                    cmd_fps_select_hz_ = 0;
                }
                cmd_fps_enabled_ = arg != 0;
                return true;
            case vcmd_id::kFpsSelect: {
                // §11.7 v2 (Pass 71): preset-indexed; REJECTED while the
                // §9.11 ladder owns video0.fps (issue FPS_LADDER off first).
                if (!venc_.enabled()) return false;
                if (arg >= venc_knobs_.preset_fps.size()) return false;
                if (cmd_fps_ladder()) return false;
                const uint16_t fps = venc_knobs_.preset_fps[arg];
                venc_.set_fps(fps);
                if (fps_ladder_) {
                    // A disabled ladder resumes from the selected rung.
                    fps_ladder_->note_external_fps(fps);
                }
                cmd_fps_select_hz_ = fps;
                cmd_fps_select_ = static_cast<uint8_t>(arg + 1);
                return true;
            }
            case vcmd_id::kResolution:
            case vcmd_id::kFraming:
                // §11.7 v2 staged (Pass 71): specified, but the venc-side
                // knobs do not exist yet — unconfigured-actuator REJECTED.
                return false;
            case vcmd_id::kCalibrate:
                // §10.6 (Pass 120): start needs an actuator and a latched
                // reporter — the loop is blind without §3.5 reports.
                if (arg > 1) return false;
                if (!calibrator_) return false;
                if (arg == 1) {
                    // §10.6 (Pass 171): a wired hook is not a working lever.
                    // Refuse before the space checks below, because on a chip
                    // with no actuator the offset window is perfectly valid
                    // and every rung in it commands the same power.
                    if (!power_actuator_) return false;
                    // §10.6 (Pass 151): whichever actuator this backend's
                    // space uses must exist. On udp both are absent and the
                    // run stays a logged intent, so it is refused as before.
                    if (backend_relative_) {
                        // The Pass 150 blanket refusal is lifted: the sweep now
                        // runs in offset space, bounded by the §10.5 band. A
                        // config that leaves no band (bound at or under the
                        // safe offset) still has nothing to sweep.
                        if (!apply_power_offset) return false;
                        if (!offset_window()) return false;
                    } else if (!apply_power) {
                        return false;
                    }
                    if (report_gate_.latched_originator() == 0) return false;
                    // Pass 153: no report-health precondition — probe/tally
                    // delivery is its own health check, and a run that cannot
                    // exchange evidence fails loudly at its first dwell
                    // (evidence_lost) instead of being refused by a proxy.
                    return calibrator_->start(now);
                }
                return calibrator_->abort(now);
            case vcmd_id::kTxPower:
                // §11.7 0x0A (Pass 135): preset index into the adapter's
                // §10.3 list. Same unconfigured-actuator pattern as the
                // 0x04-0x06 preset commands — an index with no configured
                // entry is consumed and REJECTED, never silently clamped.
                return set_power_tier(arg);
            case vcmd_id::kMtuTier: {
                if (!mtu_tier::valid(arg)) return false;
                const uint16_t requested = mtu_tier::budget(arg);
                if (requested > mtu_supported_) return false;  // no clamp
                negotiated_packet_budget_ = requested;
                for (Stream& s : streams_) {
                    if (s.frame_framer) {
                        s.frame_framer->set_negotiated_packet_budget(requested);
                    }
                }
                wb_logf("mtu: accepted tier=%u budget=%u (supported=%u)\n",
                        arg, requested, mtu_supported_);
                return true;
            }
            default:
                return false;
        }
    }
    void reset_negotiated_mtu() {
        (void)apply_command(vcmd_id::kMtuTier, mtu_tier::kDefault, 0);
    }
    uint16_t mtu_effective() const {
        // max_payload_for() is already the profile/negotiated intersection.
        return max_payload_for(selector_.profile_id());
    }
    uint16_t mtu_requested() const { return negotiated_packet_budget_; }
    uint16_t mtu_supported() const { return mtu_supported_; }
    // §10.3 (Pass 134): the adapter's sanity ceiling narrows the sweep, and
    // the §9.3 table's per-rung tx_power_level tapers it. Without this the
    // one operation that deliberately walks a rung into overload was the one
    // operation the ceiling did not cover.
    // Split from init_calibration (Pass 154): the §3.16 tally-receiver path
    // and its feed-quiet expiry run on every craft, including a D3 unit
    // whose calibrator is refused — the policy seeds must not be gated on
    // the calibrator existing, or feed_quiet_ms is silently dead exactly on
    // the node whose identity is missing.
    void seed_calib_policy(const CalibrationPolicy& c) {
        calib_policy_ = c;
        feed_quiet_ms_ = static_cast<uint32_t>(c.feed_quiet_ms);
    }

    void init_calibration(const CalibrationPolicy& c,
                          std::optional<int32_t> max_power_qdb) {
        seed_calib_policy(c);
        CalibrateParams p = calib_params_from(c);
        if (table_ != nullptr) {
            for (const Profile& pr : table_->profiles) {
                if (pr.mcs < p.levels.size()) {
                    p.levels[pr.mcs] = pr.tx_power_level;
                }
            }
        }
        if (const auto w = offset_window()) {
            // §10.6 (Pass 151): offset space. min_qdb/max_qdb and the §10.3
            // absolute ceiling do not apply here at all — the §10.5 band IS
            // the window, so the sweep cannot place hotter than
            // power_offset_max_qdb nor colder than the safe offset it began
            // at, and it climbs from the safe end as every live-link sweep
            // must. `levels` still encodes the §10.2 curve; it just stops
            // narrowing the ceiling (see taper_rung_ceiling).
            p.min_qdb = w->first;
            p.max_qdb = w->second;
            p.seek_step_qdb = c.offset_seek_step_qdb;
            p.taper_rung_ceiling = false;
        } else if (max_power_qdb) {
            p.max_qdb = std::min(p.max_qdb, *max_power_qdb);
        }
        calibrator_.emplace(p);
    }

    // §10.6 (Pass 151): the relative-backend sweep window, or nullopt on an
    // absolute backend. One sweep drives every tx adapter, so the band must be
    // safe for all of them: the top is the LOWEST §10.5 bound, and the bottom
    // is the COLDEST safe offset — starting at the warmest would command an
    // adapter above its own boot offset, which is the state Pass 150 exists to
    // forbid.
    std::optional<std::pair<int32_t, int32_t>> offset_window() const {
        if (!backend_relative_ || power_targets_.empty()) return std::nullopt;
        // The top is the CONFIG bound, deliberately not `ceiling` — a §11.7
        // 0x0A tier does not narrow this window (Pass 167, operator ruling
        // 2026-08-09). Pass 166 briefly made it do so, for symmetry with the
        // absolute backend; the bench measured what that costs. See the
        // §10.7 amendment: calibration must always be able to reach the
        // configured bound, because that is how a unit's real maximum is
        // found, and a session-volatile menu choice must not be able to hide
        // it. The tier still bounds FLIGHT power — that is `ceiling`, applied
        // in resolve_and_apply_power and hw_qdb, not here.
        int32_t lo = power_targets_.front().offset_qdb;
        int32_t hi = power_targets_.front().offset_max_qdb;
        for (const PowerTarget& t : power_targets_) {
            lo = std::min(lo, t.offset_qdb);
            hi = std::min(hi, t.offset_max_qdb);
        }
        // A config whose bound sits at or under its own safe offset leaves no
        // band to sweep. Refusing here keeps §11.7 CALIBRATE honest (REJECTED)
        // rather than running a one-point sweep and calling it a curve.
        if (hi <= lo) return std::nullopt;
        return std::make_pair(lo, hi);
    }
    // §10.6 (Pass 134) accepted-report cadence over the last closed window.
    // A whole window rather than an instantaneous gap, because the §7.2
    // return path is bursty by construction — one report per video EOB gap,
    // so gaps are normal and only the aggregate rate is meaningful.

    bool cmd_arq_enabled() const { return cmd_arq_enabled_; }
    bool cmd_selector_frozen() const { return cmd_selector_frozen_; }
    bool cmd_fps_ladder() const {
        return fps_ladder_.has_value() && cmd_fps_enabled_;
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
                // Pass 149: the enforcement counters were omitted here, so a
                // §15.5 reset left them at lifetime totals while the decision
                // denominators restarted — enforced/decisions could read >1
                // and every windowed measurement was silently wrong.
                s.jscc_enforced_frames = 0;
                s.jscc_discarded_frames = 0;
                s.jscc_exempt_frames = 0;
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
                st.mtu_fec_guard_frames =
                    s.frame_framer->stats().mtu_fec_guard_frames;
                st.idr_frames = s.frame_framer->stats().idr_frames;
                st.arq_frames = s.frame_framer->stats().arq_frames;
                st.arq_cutoff_frames =
                    s.frame_framer->stats().arq_cutoff_frames;
                st.fec_enhance_frames =
                    s.frame_framer->stats().fec_enhance_frames;  // §14.1a
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
                st.jscc_exempt_frames = s.jscc_exempt_frames;
            }
            st.resends_sent = s.sched.counters().resends_sent;
            st.arq_lock_holder = s.sched.counters().lock_holder;
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
        snap.link.transition_reason =
            selector_reason_name(selector_.reason());
        snap.link.loss_window_milli = selector_.loss_window_milli();
        snap.link.loss_ewma_milli = selector_.loss_ewma_milli();
        snap.link.loss_uniq = selector_.loss_uniq();
        snap.link.loss_score = selector_.loss_score();
        snap.link.safe_floor_profile = selector_.safe_floor_profile();
        snap.link.selector_state_valid = true;  // local selector authority
        const SelectorLockout lock = selector_.lockout(now);
        snap.link.lockout_active = lock.active;
        snap.link.lockout_latched = lock.latched;
        snap.link.lockout_profile = lock.profile;
        snap.link.lockout_ceiling_profile = lock.ceiling_profile;
        snap.link.lockout_remaining_ms = lock.remaining_ms;
        snap.link.lockout_strikes = lock.strikes;
        snap.link.lockout_active_mask = lock.active_mask;
        snap.link.lockout_latched_mask = lock.latched_mask;
        snap.link.lockout_conflict = lock.conflict;
        snap.link.flap_freeze = selector_.flap_frozen(now);
        // §9.6 actuator state (Pass 37).
        snap.link.venc_bitrate_kbps = venc_.commanded_bitrate_kbps();
        snap.link.venc_max_i_bytes = venc_.commanded_max_i_bytes();
        snap.link.venc_max_p_bytes = venc_.commanded_max_p_bytes();
        snap.link.venc_pushes = venc_.pushes();
        snap.link.venc_failures = venc_.failures();
        snap.link.venc_settling = venc_.settling(now);
        snap.link.venc_fps = venc_.commanded_fps();
        snap.link.cmd_arq = cmd_arq_enabled_;
        snap.link.cmd_selector_frozen = cmd_selector_frozen_;
        snap.link.cmd_fps_ladder = cmd_fps_ladder();
        snap.link.cmd_fps_select = cmd_fps_select_;
        snap.link.mtu_mode = "remote";
        snap.link.mtu_requested = negotiated_packet_budget_;
        snap.link.mtu_effective = mtu_effective();
        snap.link.mtu_supported = mtu_supported_;
        if (calibrator_) {  // §10.6 Pass 120
            switch (calibrator_->state()) {
                case CalibState::kIdle: snap.link.calib_state = "idle"; break;
                case CalibState::kRunning:
                    snap.link.calib_state = "running"; break;
                case CalibState::kDone: snap.link.calib_state = "done"; break;
                case CalibState::kFailed:
                    snap.link.calib_state = "failed"; break;
            }
            snap.link.calib_rung = calibrator_->rung();
            snap.link.calib_fingerprint = calib_fingerprint_;
            snap.link.calib_stale = calib_stale_;
        }
        // §3.16 (Pass 153) probe-exchange counters + the input-starve state.
        snap.link.calib_probes_sent = calib_probes_tx_;
        snap.link.calib_tallies_rx = calib_tallies_rx_;
        snap.link.calib_rx_mcs = calib_rx_mcs_;
        snap.link.feed_paused = feed_paused();
        // cmd_resolution_select / cmd_framing_select stay 0 until the venc
        // knobs exist (§11.7 staged, Pass 71).
        if (fps_ladder_) {
            snap.link.venc_p_frame_bytes =
                fps_ladder_->observed_p_frame_bytes();
            snap.link.venc_p_frame_target_bytes =
                fps_ladder_->target_p_frame_bytes();
            snap.link.venc_fps_ladder_state = fps_ladder_->state();
        }
        // §10.5: while the override-latch is set it IS the hardware value.
        snap.link.tx_power_tier = power_tier_;
        snap.link.tx_power_tier_effective = power_tier_effective();
        if (auto c = power_tier_ceiling()) snap.link.tx_power_ceiling_qdb = *c;
        snap.link.tx_power_override = power_override_.has_value();
        if (power_override_) {
            snap.link.tx_power_qdb = *power_override_;
        } else {
            for (const PowerAdapter& pa : power_) {
                if (pa.applied_qdb) {
                    snap.link.tx_power_qdb = *pa.applied_qdb;  // first TX adapter
                    break;
                }
            }
        }
        // §7.3 return-path visibility: the RX's epoch counter says how many
        // reports it SENT; we know how many arrived.
        snap.ret.reports_expected = selector_.report_epoch();
        snap.ret.reports_received = reports_received_;
        snap.ret.reports_rejected = report_gate_.rejected();  // §3.5 Pass 41
        snap.ret.feedback_rejected = feedback_gate_.rejected();   // Pass 115
        snap.ret.report_latch_holder = report_gate_.latched_originator();
        snap.link.report_latch_holder = report_gate_.latched_originator();
        snap.link.report_latch_known = true;   // local gate; always known here
        // §15.3 Pass 159/160: last ACCEPTED §3.16 verdict + the climb-gate
        // suppression count. verdict 0 with age 0 = none this session.
        snap.link.verdict = selector_.verdict();
        snap.link.verdict_age_ms =
            verdict_rx_ms_ == 0
                ? 0
                : static_cast<uint32_t>(std::min<uint64_t>(
                      now - verdict_rx_ms_, 0xFFFFFFFFull));
        snap.link.promote_blocked_saturated =
            selector_.promote_blocked_saturated();
        snap.link.promote_blocked_probe =
            selector_.promote_blocked_probe();  // §15.3 Pass 163
        // §15.3 Pass 186 craft view of the §9.4 probe: the evidence the veto
        // is reading (kNoProbe = none received), how old it is against
        // probe_veto_ttl_ms, and the rate we are actually flying on probe
        // slots. The three ground-only tallies stay 0 here.
        snap.link.probe_per = selector_.probe_per();
        snap.link.probe_per_age_ms =
            selector_.probe_per_ms() == 0
                ? 0
                : static_cast<uint32_t>(std::min<uint64_t>(
                      now - selector_.probe_per_ms(), 0xFFFFFFFFull));
        snap.link.probe_candidate_mcs = probe_candidate_mcs_;
    }

    struct PowerAdapter {
        std::string name;
        size_t adapter_idx;  // position in cfg.adapters == radio index
        PowerCurve curve;
        // The ceiling IN THIS NODE'S SPACE — absolute at construction,
        // re-seeded from power_offset_max_qdb by set_backend_relative()
        // (Pass 166). It clamps the curve resolve, and on a relative node it
        // used to hold an absolute 108 against offsets, i.e. no clamp at all.
        std::optional<int32_t> ceiling;
        std::vector<int32_t> presets_qdb;  // §11.7 0x0A selectable ceilings
        // Parsed alongside; set_backend_relative() promotes it into
        // presets_qdb on a relative node so no downstream site branches.
        std::vector<int32_t> offset_presets_qdb;
        int32_t offset_max_qdb = 0;
        std::optional<int32_t> applied_qdb;
    };
    // §10.5 override targets: every role:"tx" adapter, curve or not.
    struct PowerTarget {
        std::string name;
        size_t adapter_idx;
        // The ceiling in this node's space: §10.3 absolute at construction,
        // re-seeded from offset_max_qdb by set_backend_relative() (Pass 166).
        // It is what §15.3 tx_power_ceiling_qdb and §15.5 report.
        std::optional<int32_t> ceiling;
        std::vector<int32_t> presets_qdb;  // §11.7 0x0A selectable ceilings
        // Parsed alongside; promoted into presets_qdb on a relative node.
        std::vector<int32_t> offset_presets_qdb;
        // §10.5 (Pass 150) relative contract: the safe boot offset and the
        // bound the runtime latch may not exceed.
        int32_t offset_qdb = -24;
        int32_t offset_max_qdb = 0;
    };

    // §10.4 curve resolve for the committed operating point, through the
    // change-detection cache (apply only when the resolved value moves).
    void resolve_and_apply_power(uint8_t mcs, uint8_t level) {
        // §10.5 (Pass 150) / §10.6 (Pass 151): a curve's numbers only mean
        // something in a known space. An offset-space curve is one THIS
        // backend's own calibration authored — the Pass 146 fingerprint is
        // backend-scoped, so a foreign one cannot load — and it actuates. An
        // explicit config `power_map` is not backend-scoped and carries no
        // space at all, so on a relative backend it stays refused: a 108 qdb
        // entry there is +27 dB, not 27 dBm, and one profile commit would
        // silently undo the boot offset.
        if (!power_.empty() && !curve_actuates()) {
            if (!warned_curve_refused_) {
                warned_curve_refused_ = true;
                wb_logf("power: §10.2 curve REFUSED on a relative backend "
                        "(§10.5 Pass 150) — config power_map holds "
                        "absolute qdb; running on the §10.5 boot offset "
                        "instead. Run §10.6 calibration to author an "
                        "offset-space curve.\n");
            }
            return;
        }
        for (PowerAdapter& pa : power_) {
            const auto qdb =
                resolve_power_qdb(pa.curve, mcs, level, pa.ceiling);
            if (qdb && (!pa.applied_qdb || *pa.applied_qdb != *qdb)) {
                // §10.5: cache only what the backend accepted — a failed
                // write retries at the next commit/re-assert.
                bool ok = true;
                if (backend_relative_ && apply_power_offset) {
                    ok = apply_power_offset(pa.adapter_idx, *qdb);
                } else if (!backend_relative_ && apply_power) {
                    ok = apply_power(pa.adapter_idx, *qdb);
                } else {
                    wb_logf("power: %s mcs=%u level=%u -> %d qdb\n",
                            pa.name.c_str(), mcs, level, *qdb);
                }
                if (ok) pa.applied_qdb = *qdb;
            }
        }
    }

    // §11.7 0x0A / §15.5 (Pass 135): select the §10.3 ceiling from the
    // adapter's preset list. This moves the BASELINE the Pass 134 per-rung
    // mask is derived from, not the power itself — so the calibrated per-rung
    // shape survives, which is exactly what the rung-agnostic §10.5 latch
    // cannot do. Returns false = REJECTED (no list, or index past its end).
    bool set_power_tier(uint8_t tier) {
        // A running §10.6 sweep OWNS the power actuator, and the re-init
        // below would replace the live Calibrator wholesale — losing the run
        // and, with it, the restore that hands the actuator back. The craft
        // would be left at whatever power the abandoned sweep last commanded.
        // §10.5 latching takes the same position (the run yields first).
        if (calibrating()) return false;
        // §10.3/§11.7 0x0A: Pass 151 REJECTED outright on a relative backend,
        // because `power_presets_qdb` are ABSOLUTE qdb and installing a
        // 60..108 preset as the resolve clamp removes the one guard keeping a
        // resolved curve inside the §10.5 window. Pass 166 re-bases instead:
        // set_backend_relative() has already promoted
        // `power_offset_presets_qdb` into `presets_qdb` on this node, so the
        // list below is in the same space as the resolve it clamps and there
        // is nothing left to refuse. A relative node that configured no
        // offset list has an empty list and falls out of the
        // `configured` check.
        //
        // ALL-OR-NOTHING. Validate every configured list before touching any
        // ceiling: applying the tier to one tx adapter, skipping another whose
        // list is shorter, and still reporting success would leave two
        // adapters on different ceilings with nothing saying so — and
        // power_tier_ceiling() reports only the FIRST, which is the number
        // that then seeds the calibrator and §15.3.
        bool configured = false;
        for (const PowerTarget& t : power_targets_) {
            if (t.presets_qdb.empty()) continue;
            configured = true;
            if (tier >= t.presets_qdb.size()) return false;
        }
        if (!configured) return false;
        for (PowerAdapter& pa : power_) {
            if (tier >= pa.presets_qdb.size()) continue;
            pa.ceiling = pa.presets_qdb[tier];
            pa.applied_qdb.reset();  // force a real re-apply at the new bound
        }
        for (PowerTarget& t : power_targets_) {
            if (tier >= t.presets_qdb.size()) continue;
            t.ceiling = t.presets_qdb[tier];
        }
        power_tier_ = tier;
        // The sweep bound moves with it, so a calibration started after a tier
        // change measures inside the new envelope rather than the boot one.
        if (calibrator_ && power_tier_ceiling()) {
            init_calibration(calib_policy_, power_tier_ceiling());
        }
        // Re-resolve once at the committed operating point. On a node with no
        // curve this resolves to nullopt and moves nothing — §10.3 (Pass 134):
        // the ceiling binds only where a number of ours reaches the actuator.
        if (power_override_) {
            // §10.5 says the ceiling is the only clamp on a latch, so a
            // lowered tier must re-assert it through the NEW clamp. Skipping
            // this left the hardware on the old, higher clamped value until
            // some unrelated event re-applied it — a tier that visibly did
            // nothing, which is the worst outcome for a safety control.
            set_power_override(*power_override_);
        } else if (last_commit_mcs_) {
            resolve_and_apply_power(*last_commit_mcs_, last_commit_level_);
        }
        return true;
    }
    int power_tier() const { return power_tier_; }
    // §15.5 reports ONE ceiling and ONE preset list, so both must come from
    // the same adapter — the tiered one. Reading the ceiling from the first
    // target with any ceiling and the list from the first with a non-empty
    // one let a second tx adapter contribute half of a pair that is then
    // presented as describing a single node.
    const PowerTarget* tiered_target() const {
        for (const PowerTarget& t : power_targets_) {
            if (!t.presets_qdb.empty()) return &t;
        }
        return nullptr;
    }
    std::optional<int32_t> power_tier_ceiling() const {
        if (const PowerTarget* t = tiered_target()) return t->ceiling;
        // No preset list anywhere: no tier is selectable, so there is no
        // pairing to keep consistent — still report the §10.3 boot ceiling,
        // which is what §15.3 carried before tiers existed.
        for (const PowerTarget& t : power_targets_) {
            if (t.ceiling) return t.ceiling;
        }
        return std::nullopt;
    }
    const std::vector<int32_t>& power_presets() const {
        static const std::vector<int32_t> kNone;
        const PowerTarget* t = tiered_target();
        return t != nullptr ? t->presets_qdb : kNone;
    }
    // §15.5 `effective`: false when no curve and no artifact are loaded, where
    // a tier is recorded but reaches no actuator (§10.3 Pass 134 ruling).
    // A §10.5 latch counts: the ceiling is the ONE clamp on an override, so
    // while one is held the tier plainly reaches hardware even with no curve
    // — device-shown, tier 0 (ceiling 60) clamping a 120 qdb latch to 15 dBm
    // while `effective` claimed the tier bound nothing.
    // §10.5 (Pass 150): a held latch no longer passes through the §10.3
    // ceiling — set_power_override does not read t.ceiling at all — so a tier
    // cannot claim to be effective on the strength of one.
    bool power_tier_effective() const {
        // A curve that resolve_and_apply_power() refuses reaches no hardware,
        // so reporting the tier effective would be the same lie Pass 136
        // removed for the latch. The `&& !backend_relative_` term is gone
        // with Pass 166: a relative tier now installs an offset-space ceiling
        // that clamps an offset-space curve, so it is effective on exactly
        // the same condition as an absolute one — curve_actuates() already
        // encodes the extra requirement that a relative curve be
        // calibration-authored.
        return curve_actuates();
    }

    // The one predicate for "resolve_and_apply_power() will reach hardware".
    // Pass 150 spelled this test out at three call sites and they drifted
    // apart within a single pass; there is one copy now.
    bool curve_actuates() const {
        return has_power_curve() &&
               (!backend_relative_ || curve_is_offset_space_);
    }

    // §10.5 (Pass 150) override-latch: latch a RELATIVE offset on every tx
    // adapter; the §10.4 commit resolve yields until cleared. Bounded by
    // power_offset_max_qdb and rejected past it — the §10.3 ceiling no longer
    // clamps it, because on an offset backend it clamped the offset.
    // §10.5 (Pass 150): the latch is a RELATIVE offset, bounded by each
    // adapter's power_offset_max_qdb and REJECTED past it — never silently
    // clamped. The old min(qdb, max_power_qdb) clamp is gone: on an offset
    // backend it clamped the offset, so a documented safety ceiling became a
    // boost permit. All-or-nothing, matching set_power_tier: validate every
    // target before touching any, so a partial apply cannot leave two tx
    // adapters on different offsets with nothing saying so.
    bool set_power_override(int32_t qdb) {
        for (const PowerTarget& t : power_targets_) {
            if (qdb > t.offset_max_qdb) return false;
        }
        // A node with no tx adapter has no bound to check against; latching
        // an unbounded value there would report authority nobody holds.
        if (power_targets_.empty()) return false;
        bool all_ok = true;
        for (const PowerTarget& t : power_targets_) {
            // §10.3/§11.7 0x0A: a held tier clamps the latch (Pass 167
            // review). Pass 150 removed the old `min(qdb, max_power_qdb)`
            // because on an offset backend it clamped an offset by an
            // absolute and so was a no-op dressed as a safety ceiling — but
            // it removed the clamp entirely rather than fixing its space, and
            // since Pass 166 `t.ceiling` IS in the actuator's space. Without
            // this the craft latch outranked the tier while the ground's
            // UplinkPower::hw_qdb() clamped correctly, so one spec sentence
            // described two opposite behaviours. §10.5 still REPORTS the
            // request, not the clamped value, and a request above
            // power_offset_max_qdb is still rejected above rather than
            // clamped here.
            const int32_t hw = t.ceiling ? std::min(qdb, *t.ceiling) : qdb;
            if (apply_power_offset) {
                all_ok = apply_power_offset(t.adapter_idx, hw) && all_ok;
            } else {
                wb_logf("power: %s offset -> %+d qdb\n",
                        t.name.c_str(), static_cast<int>(hw));
            }
        }
        // §15.3 must not report a latch the radio refused (air_iface.h: a
        // false return means the caller must not cache it as applied).
        if (!all_ok) return false;
        power_override_ = qdb;
        return true;
    }

    // §10.5 (Pass 150): the forced safe boot point. Applied once at startup on
    // every role:"tx" adapter so no node ever transmits at the uncharacterised
    // efuse default (offset 0), which Pass 150 measured as a compressing
    // operating point on the fleet's 8822EU.
    void apply_boot_power_offsets() {
        for (const PowerTarget& t : power_targets_) {
            const bool ok =
                apply_power_offset && apply_power_offset(t.adapter_idx,
                                                         t.offset_qdb);
            // Say what happened. A safety control that logs an intent it did
            // not achieve is worse than one that logs nothing.
            wb_logf("power: %s §10.5 boot offset %+d qdb (bound %+d) -> "
                    "%s\n",
                    t.name.c_str(), static_cast<int>(t.offset_qdb),
                    static_cast<int>(t.offset_max_qdb),
                    ok ? "applied" : "NOT APPLIED");
        }
    }

    // §10.5 auto: clear the latch with one forced immediate restore —
    // apply_power_auto (backend default) first, then re-resolve the curve at
    // the last committed operating point so a loaded power_map re-asserts
    // without waiting for the next profile change.
    void clear_power_override() {
        power_override_.reset();
        // §10.5 (Pass 150): auto resolves to the adapter's configured safe
        // offset, NOT the backend default. apply_power_auto was offset 0 on
        // devourer — the uncharacterised point, and the worst setting on a
        // compressing unit. §11.6 recovery ends here (Pass 48), so this is
        // also what an unattended recovery lands on.
        for (const PowerTarget& t : power_targets_) {
            if (apply_power_offset) {
                apply_power_offset(t.adapter_idx, t.offset_qdb);
            }
        }
        for (PowerAdapter& pa : power_) {
            pa.applied_qdb.reset();  // force re-apply even at the same value
        }
        if (last_commit_mcs_) {
            resolve_and_apply_power(*last_commit_mcs_, last_commit_level_);
        }
    }

    // §10.5 re-assert after any event that may have reset hardware power (a
    // §11 retune's TXAGC reset, a §11.6 monitor recovery's `txpower auto`).
    void reassert_power() {
        if (power_override_) {
            set_power_override(*power_override_);
            return;
        }
        bool any_curve = false;
        for (PowerAdapter& pa : power_) {
            if (pa.applied_qdb && apply_power) {
                any_curve = true;
                if (!apply_power(pa.adapter_idx, *pa.applied_qdb)) {
                    pa.applied_qdb.reset();  // §10.5: retry at next commit
                }
            }
        }
        // §10.5 (Pass 150): with no latch and no curve this used to restore
        // NOTHING, so a §11.6 recovery — which restores the backend default —
        // permanently lost the boot offset on the default config. The boot
        // offset is the floor of this precedence, not an absent case.
        if (!any_curve) {
            apply_boot_power_offsets();
        }
    }

    std::optional<int32_t> power_override() const { return power_override_; }
    bool has_power_targets() const { return !power_targets_.empty(); }

    // Actuation hooks (§10.4/§10.5); unset = logged intent. apply_mode is
    // radio-only by design (Pass 13: monitor carries MCS per-packet).
    std::function<void(uint8_t mcs, bool sgi)> apply_mode;
    // §9.4 Pass 163: (period, slot, candidate_mcs) probe schedule refresh;
    // installed only when air.mcs_probe is on (radio backend). (0,0,0)
    // disarms.
    std::function<void(uint16_t, uint16_t, uint8_t)> apply_probe;

    // §9.7 pin + §9.4 probe move together (Pass 186). The probe candidate is
    // clamped to max_profile, so a pin change that is never followed by a
    // commit — a §11.7 SELECTOR freeze is exactly that, it pins the CURRENT
    // rung so nothing can commit afterwards — would otherwise strand the
    // previous arming on the radio. Every pin write goes through here.
    void apply_profile_pin(uint8_t min_profile, uint8_t max_profile) {
        selector_.set_profile_pin(min_profile, max_profile);
        refresh_probe(selector_.profile_id());
    }

    // §9.4: (re)derive the up-candidate for `profile_id` through the live
    // table and the live §9.7 ceiling, and push it at the radio. Disarms
    // (period 0) at the top rung, at same-MCS adjacency, and above the pin —
    // all three are probe_up_candidate_mcs returning nullopt.
    void refresh_probe(uint8_t profile_id) {
        // §9.4 fail-closed: the hook exists only where the operator armed
        // air.mcs_probe on a stage-0-proven unit. Not armed => not probing,
        // and §15.3 must keep saying so.
        if (!apply_probe) return;
        std::optional<uint8_t> cand;
        if (table_ != nullptr && table_->probe_period != 0) {
            cand = probe_up_candidate_mcs(*table_, profile_id,
                                          selector_.max_profile());
        }
        probe_candidate_mcs_ = cand ? *cand : kProbeMcsNone;  // §15.3
        if (cand) {
            apply_probe(table_->probe_period, table_->probe_slot, *cand);
        } else {
            apply_probe(0, 0, 0);
        }
    }
    std::function<bool(size_t adapter_idx, int32_t qdb)> apply_power;
    std::function<void(size_t adapter_idx)> apply_power_auto;
    // §10.5 (Pass 150) relative actuator — bound for EVERY backend, unlike
    // apply_power which is radio-only.
    std::function<bool(size_t adapter_idx, int32_t qdb)> apply_power_offset;
    // §10.5/§10.6 (Pass 171): does this node's role:"tx" adapter have a power
    // lever AT ALL. Distinct from the hooks above, which say whether a lever
    // is WIRED here — both can be bound while the chip behind them ignores
    // every write (devourer answers an unsupported knob with 0). Set once at
    // startup beside set_backend_relative(). A §10.6 run on such a chip reads
    // flat at every rung and would persist that as an artifact which the
    // resolve, a pairing pass and the next boot all believe.
    void set_power_actuator(bool ok) { power_actuator_ = ok; }
    // §10.5 (Pass 150): true when the backend's power lever is RELATIVE
    // (devourer). The §10.2 absolute curve and the §10.6 absolute sweep are
    // then not merely inaccurate but dangerous — their 60..108 qdb values
    // become +15..+27 dB of boost — so both are refused until re-based.
    // §10.5. Called once at startup, before any tier can be selected, which
    // is what makes the re-seed below safe: it would clobber an applied tier
    // if it ever ran twice.
    void set_backend_relative(bool on) {
        backend_relative_ = on;
        if (!on) return;
        // §10.3/§11.7 0x0A (Pass 166): on a relative node the tier lives in
        // OFFSET space. Select the governing list and re-seed the boot
        // ceiling from the §10.5 bound here, once, so that set_power_tier(),
        // the curve resolve clamp, §15.3 tx_power_ceiling_qdb and §15.5 all
        // read one space with no branch of their own — the four-copy
        // precedence bug UplinkPower was written to end.
        //
        // The old absolute seeding is the craft-side twin of Pass 165's named
        // residual: resolve_power_qdb() clamps to `ceiling`, and a 108 there
        // is a no-op against an offset-space calibrated curve, so the clamp
        // bound nothing on the one backend that still exists.
        for (PowerAdapter& pa : power_) {
            pa.presets_qdb = std::move(pa.offset_presets_qdb);
            pa.offset_presets_qdb.clear();
            pa.ceiling = pa.offset_max_qdb;
        }
        for (PowerTarget& t : power_targets_) {
            t.presets_qdb = std::move(t.offset_presets_qdb);
            t.offset_presets_qdb.clear();
            t.ceiling = t.offset_max_qdb;
        }
    }
    bool backend_relative() const { return backend_relative_; }
    std::function<std::optional<uint32_t>(size_t bytes, bool include_pending,
                                          uint16_t packet_budget)>
        estimate_airtime;

    uint16_t originator_;
    uint32_t session_;
    uint8_t table_version_;
    const ProfileTable* table_;
    Selector selector_;
    VencActuator venc_;
    VencCfg venc_knobs_;            // §9.6 cap knobs (Pass 37)
    std::optional<FpsLadder> fps_ladder_;  // §9.11 (Pass 39)
    uint16_t arq_max_fps_ = 100;           // §4.1 Pass 40 cutoff
    bool arq_fps_suppressed_ = false;
    uint16_t mtu_supported_ = kDefaultMaxPayload;  // §9.3a local adapter min
    uint16_t negotiated_packet_budget_ = kDefaultMaxPayload;
    // §11.7 remote command state (craft-session-volatile).
    bool cmd_arq_enabled_ = true;
    bool cmd_selector_frozen_ = false;
    // §11.7 v2 FPS_SELECT (Pass 71): stats index (1-based, 0 = none this
    // session) and the re-offer target (cleared when the ladder resumes).
    uint8_t cmd_fps_select_ = 0;
    uint16_t cmd_fps_select_hz_ = 0;
    bool cmd_fps_enabled_ = true;
    uint8_t boot_min_profile_ = 0;   // §9.7 boot envelope for SELECTOR run
    uint8_t boot_max_profile_ = 255;
    // §15.3 Pass 186: the MCS this node is currently flying on §9.4 probe
    // slots. kProbeMcsNone until refresh_probe() arms one, and back to it on
    // every disarm — so it reads "not probing" for a node with air.mcs_probe
    // off, at the top rung, at same-MCS adjacency, or above its §9.7 pin.
    uint8_t probe_candidate_mcs_ = kProbeMcsNone;
    ReportGate feedback_gate_;             // §3.10 Pass 55
    ReportGate report_gate_;               // §3.5 Pass 41
    uint32_t verdict_epoch_seen_ = 0;      // §3.16 Pass 159 monotone gate
    std::optional<std::pair<uint16_t, uint32_t>>
        verdict_epoch_src_;                // ...scoped to (orig, session)
    uint64_t verdict_rx_ms_ = 0;           // last accepted verdict (§15.3)
    uint64_t frame_cadence_us_ = 0; // windowed ingress cadence estimate
    uint64_t cadence_start_ms_ = 0;
    uint32_t cadence_frames_ = 0;
    std::vector<PowerAdapter> power_;
    std::vector<PowerTarget> power_targets_;      // §10.5 all tx adapters
    int power_tier_ = -1;                         // §11.7 0x0A, -1 = unset
    CalibrationPolicy calib_policy_;              // for a tier re-init
    std::optional<int32_t> power_override_;       // §10.5 latch (volatile)
    bool backend_relative_ = false;  // §10.5 Pass 150
    bool power_actuator_ = true;     // §10.5 Pass 171
    // §10.6 (Pass 151): set when the loaded curve was authored by THIS
    // backend's calibration, so its numbers are in this backend's space. A
    // config power_map never sets it — it is not backend-scoped.
    bool curve_is_offset_space_ = false;
    bool warned_curve_refused_ = false;
    std::optional<uint8_t> last_commit_mcs_;      // §10.5 clear-restore point
    uint8_t last_commit_level_ = 4;
    uint32_t reports_received_ = 0;
    // §10.6 (Pass 134) report-health window. NOT reset by reset_stats(): it
    // gates whether a calibration may start, and an operator zeroing the
    // stats display must not open that gate.
    std::vector<Stream> streams_;

  public:
    // §10.6 (Pass 120) craft-resident calibration. The engine is core/; the
    // app maps its actions onto the §9.7 pin and §10.5 power seams and
    // persists the artifact through on_calib_artifact.
    std::optional<Calibrator> calibrator_;
    std::function<void(const CalibArtifact&)> on_calib_artifact;
    uint8_t calib_fingerprint_ = 0;   // CRC-8 of persisted artifact (0=none)
    bool calib_stale_ = false;        // persisted artifact pairing mismatch

    void calibrate_service(uint64_t now) {
        if (!calibrator_) return;
        const CalibActions a = calibrator_->tick(now);
        // Artifact BEFORE restore: persisting installs the fresh curve, so
        // the restore resolve below re-places the committed rung from it.
        if (a.artifact_ready && on_calib_artifact) {
            on_calib_artifact(calibrator_->artifact());
        }
        if (a.restore) {
            // R4 order: power first (a probe may sit on a rung's ceiling),
            // then the selector window, then the §10.4 resolve.
            for (PowerAdapter& pa : power_) {
                pa.applied_qdb.reset();  // force a real re-apply
            }
            if (cmd_selector_frozen_) {
                const uint8_t pf = selector_.profile_id();
                apply_profile_pin(pf, pf);
            } else {
                apply_profile_pin(boot_min_profile_,
                                                  boot_max_profile_);
            }
            if (power_override_) {
                // §10.5: the latch owns power — re-assert it (probes wrote
                // past it, so "skip the resolve" alone strands the last
                // probe value on the hardware).
                set_power_override(*power_override_);
            } else if (curve_actuates()) {
                if (last_commit_mcs_) {
                    resolve_and_apply_power(*last_commit_mcs_,
                                            last_commit_level_);
                }
            } else if (apply_power_offset) {
                // No curve and no override: no in-process authority knows the
                // pre-run power, but the BACKEND does — this is exactly the
                // §10.5 `{"auto": true}` condition ("release power authority
                // with no curve loaded"), whose defined answer is the
                // backend default. Before Pass 134 this leaf
                // was near-unreachable, because a run that got far enough to
                // move power almost always ended by installing a curve. The
                // no_wall_found refusal makes a first-ever run END in failure
                // routinely, and leaving the last probe value latched strands
                // the actuator on exactly the runs the refusal exists to
                // catch. Device-confirmed: a bench-range run left the craft
                // at 15.00 dBm (rung 7's mask ceiling) indefinitely.
                // §10.5 (Pass 150): "backend auto" on a relative backend is
                // offset 0 — the uncharacterised efuse default, measured to be
                // a compressing operating point. Land on the configured safe
                // offset instead, matching clear_power_override().
                for (const PowerTarget& t : power_targets_) {
                    apply_power_offset(t.adapter_idx, t.offset_qdb);
                }
                wb_logf("calibrate: restore -> safe offset "
                        "(no curve, no override)\n");
            } else {
                // udp/dev backend: no actuator at all, so there is nothing to
                // hand back. Say so rather than implying a restore happened.
                wb_logf("calibrate: restore has no power authority and "
                        "no auto actuator — TX power left at the last "
                        "probe value\n");
            }
            wb_logf("calibrate: %s%s%s\n",
                    calibrator_->state() == CalibState::kDone
                        ? "done"
                        : "failed",
                    calibrator_->fail_reason() ? " reason=" : "",
                    calibrator_->fail_reason()
                        ? calibrator_->fail_reason()
                        : "");
        }
        if (a.set_qdb) {
            // §10.6: every tx adapter, curve or not (power_targets_) — the
            // run exists precisely because no curve may be loaded yet.
            for (const PowerTarget& t : power_targets_) {
                if (backend_relative_) {
                    // §10.6 (Pass 151): the sweep is in offset space, so it
                    // drives the SAME actuator §10.5 boots this node with,
                    // bounded by the same key. Belt and braces on the clamp:
                    // offset_window() already caps the seek, but a probe is
                    // the one thing in the process that deliberately walks
                    // toward a wall.
                    const int32_t v = std::min(*a.set_qdb, t.offset_max_qdb);
                    if (apply_power_offset) {
                        (void)apply_power_offset(t.adapter_idx, v);
                    }
                    continue;
                }
                const int32_t v = t.ceiling
                                      ? std::min(*a.set_qdb, *t.ceiling)
                                      : *a.set_qdb;
                if (apply_power) (void)apply_power(t.adapter_idx, v);
            }
        }
        if (a.pin_rung) {
            apply_profile_pin(*a.pin_rung, *a.pin_rung);
        }
    }
    // §3.16 (Pass 153): the video input is starved while EITHER direction
    // runs — our own §10.6 run, or a ground-driven §10.7 run observed as an
    // active probe run at the receiver half.
    bool feed_paused() const {
        return calibrating() || calib_rx_.run_id() != 0;
    }

    // §3.16 (Pass 153): drain the §10.6 run's probe emissions. Probes go
    // through send_calib (plain air inject) — they are their own traffic
    // class, never ride the §7.2 held queue, and with the feed paused there
    // is no video to pace against anyway.
    void calibrate_probe_service(uint64_t now) {
        if (!calibrator_ || !send_calib) return;
        calibrator_->new_tick();
        const uint16_t dest = report_gate_.latched_originator();
        if (dest == 0) return;
        for (;;) {
            const DwellProbeOut po = calibrator_->next_probe(now);
            if (!po.send) break;
            CalibProbe pr;
            pr.prefix.originator = originator_;
            pr.prefix.destination = dest;
            pr.prefix.session_id = session_;
            pr.run_id = calibrator_->probe_run_id();
            pr.dwell_id = calibrator_->probe_dwell_id();
            pr.seq = po.seq;
            pr.count = calibrator_->probe_dwell_count();
            uint8_t buf[mtu_tier::kHighBudget];
            const size_t n = encode_calib_probe(
                pr, mtu_effective(), buf, sizeof buf);
            if (n != 0) {
                send_calib(buf, n);
                ++calib_probes_tx_;
            }
        }
    }

    bool has_power_curve() const { return !power_.empty(); }
    // §10.6: install/replace the tx adapters' §10.2 curve (calibration
    // artifact or boot auto-load) so the commit resolve uses it.
    void install_curve(const PowerCurve& c) {
        power_.clear();
        for (const PowerTarget& t : power_targets_) {
            power_.push_back(
                PowerAdapter{t.name, t.adapter_idx, c,
                             // `t.ceiling` in BOTH spaces (Pass 167 review).
                             // The relative arm used to seed offset_max_qdb,
                             // a Pass 151 relic from when a tier was refused
                             // here and `ceiling` held an incomparable
                             // absolute 108. Since Pass 166 `ceiling` IS the
                             // offset ceiling and a tier lowers it, so
                             // re-seeding the bound discarded the tier —
                             // install_curve runs on every completed §10.6
                             // calibration, and nothing forbids calibrating
                             // with a tier held, so a run silently RAISED
                             // power back to the §10.5 bound while §15.3 and
                             // §15.5 kept reporting the tier held. That is
                             // the "a tier can only ever LOWER power" ruling
                             // inverted, which is why it is a one-line arm.
                             t.ceiling,
                             // Already space-selected on the target by
                             // set_backend_relative() (Pass 166), so the
                             // offset list is spent and passed empty.
                             t.presets_qdb, std::vector<int32_t>{},
                             t.offset_max_qdb, std::nullopt});
        }
        // An installed curve came from THIS backend's artifact (the Pass 146
        // fingerprint is backend-scoped), so on a relative backend it holds
        // offsets and the §10.4 resolve may actuate it.
        curve_is_offset_space_ = backend_relative_;
    }
    bool calibrating() const {
        return calibrator_ &&
               calibrator_->state() == CalibState::kRunning;
    }
};

}  // namespace node
}  // namespace wblink
