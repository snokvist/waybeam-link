// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/stats.h"

#include <cinttypes>
#include <cstdio>

namespace wblink {

namespace {

void append_escaped(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x",
                                  static_cast<unsigned>(c) & 0xFF);
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void append_u64(std::string& out, uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%" PRIu64, v);
    out += buf;
}

void append_i32(std::string& out, int32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%" PRId32, v);
    out += buf;
}

void append_bool(std::string& out, bool v) { out += v ? "true" : "false"; }

void append_hist(std::string& out, const std::array<uint64_t, 8>& h) {
    out += '[';
    for (size_t i = 0; i < h.size(); ++i) {
        if (i != 0) out += ',';
        append_u64(out, h[i]);
    }
    out += ']';
}

void append_timing(std::string& out, const TimingMetricStats& t) {
    out += "{\"samples\":";
    append_u64(out, t.samples);
    out += ",\"p95_us\":";
    append_u64(out, t.p95_us);
    out += ",\"max_us\":";
    append_u64(out, t.max_us);
    out += '}';
}

}  // namespace

void format_stats_line(const StatsSnapshot& snap, std::string& out) {
    out += "{\"t_ms\":";
    append_u64(out, snap.t_ms);
    out += ",\"node\":";
    append_u64(out, snap.node);
    out += ",\"session\":";
    append_u64(out, snap.session);

    out += ",\"adapters\":[";
    bool first = true;
    for (const AdapterStats& a : snap.adapters) {
        if (!first) out += ',';
        first = false;
        out += "{\"name\":";
        append_escaped(out, a.name);
        out += ",\"rx\":";
        append_u64(out, a.rx);
        out += ",\"dup\":";
        append_u64(out, a.dup);
        out += ",\"rssi_best\":";
        append_i32(out, a.rssi_best);
        out += ",\"rssi_mean\":";
        append_i32(out, a.rssi_mean);
        out += ",\"snr\":";
        append_i32(out, a.snr);
        out += ",\"noise\":";
        append_i32(out, a.noise);
        out += ",\"tx_submitted\":";
        append_u64(out, a.tx_submitted);
        out += ",\"tx_failed\":";
        append_u64(out, a.tx_failed);
        out += ",\"tx_timeout\":";
        append_u64(out, a.tx_timeout);
        out += ",\"drop\":";
        append_u64(out, a.drop);
        out += ",\"filtered\":";
        append_u64(out, a.filtered);
        out += ",\"kernel_drop\":";
        append_u64(out, a.kernel_drop);
        out += ",\"bpf_filtered\":";
        append_u64(out, a.bpf_filtered);
        out += ",\"tsf_fallback\":";
        append_u64(out, a.tsf_fallback);
        out += ",\"tx_reports\":";
        append_u64(out, a.tx_reports);
        out += ",\"tx_report_fails\":";
        append_u64(out, a.tx_report_fails);
        out += ",\"adapter_stalled\":";
        append_bool(out, a.adapter_stalled);
        out += ",\"tx_wedged\":";
        append_bool(out, a.tx_wedged);
        out += '}';
    }
    out += ']';

    out += ",\"streams\":[";
    first = true;
    for (const StreamStats& s : snap.streams) {
        if (!first) out += ',';
        first = false;
        out += "{\"stream_id\":";
        append_u64(out, s.stream_id);
        out += ",\"type\":";
        append_escaped(out, s.type);
        out += ",\"seq\":";
        append_u64(out, s.seq);
        out += ",\"delivered\":";
        append_u64(out, s.delivered);
        out += ",\"uniq\":";
        append_u64(out, s.uniq);
        out += ",\"diversity\":";
        append_u64(out, s.diversity);
        out += ",\"loss_prediversity_milli\":";
        append_u64(out, s.loss_prediversity_milli);
        out += ",\"loss_postdiv_prearq_milli\":";
        append_u64(out, s.loss_postdiv_prearq_milli);
        out += ",\"recovered_arq\":";
        append_u64(out, s.recovered_arq);
        out += ",\"recovered_fec\":";
        append_u64(out, s.recovered_fec);
        out += ",\"fec_recovered_source_symbols\":";
        append_u64(out, s.fec_recovered_source_symbols);
        out += ",\"arq_recovered_source_symbols\":";
        append_u64(out, s.arq_recovered_source_symbols);
        out += ",\"arq_recovered_repair_symbols\":";
        append_u64(out, s.arq_recovered_repair_symbols);
        out += ",\"frames_with_arq\":";
        append_u64(out, s.frames_with_arq);
        out += ",\"frames_fec_only\":";
        append_u64(out, s.frames_fec_only);
        out += ",\"frames_fec_after_arq\":";
        append_u64(out, s.frames_fec_after_arq);
        out += ",\"frame_count\":";
        append_u64(out, s.frame_count);
        out += ",\"frame_bytes\":";
        append_u64(out, s.frame_bytes);
        out += ",\"frame_size_last\":";
        append_u64(out, s.frame_size_last);
        out += ",\"frame_size_min\":";
        append_u64(out, s.frame_size_min);
        out += ",\"frame_size_max\":";
        append_u64(out, s.frame_size_max);
        out += ",\"frame_interval_us\":";
        append_u64(out, s.frame_interval_us);
        out += ",\"frame_jitter_us\":";
        append_u64(out, s.frame_jitter_us);
        out += ",\"frames_fast\":";
        append_u64(out, s.frames_fast);
        out += ",\"frames_unrecoverable\":";
        append_u64(out, s.frames_unrecoverable);
        out += ",\"malformed\":";
        append_u64(out, s.malformed);
        out += ",\"jscc_shadow_blocks\":";
        append_u64(out, s.jscc_shadow_blocks);
        out += ",\"jscc_predicted_loss_symbols\":";
        append_u64(out, s.jscc_predicted_loss_symbols);
        out += ",\"jscc_observed_loss_symbols\":";
        append_u64(out, s.jscc_observed_loss_symbols);
        out += ",\"jscc_underpredicted_blocks\":";
        append_u64(out, s.jscc_underpredicted_blocks);
        out += ",\"jscc_predicted_parity_symbols\":";
        append_u64(out, s.jscc_predicted_parity_symbols);
        out += ",\"jscc_predicted_repair_symbols\":";
        append_u64(out, s.jscc_predicted_repair_symbols);
        out += ",\"jscc_observed_repair_symbols\":";
        append_u64(out, s.jscc_observed_repair_symbols);
        out += ",\"jscc_repair_underpredicted_blocks\":";
        append_u64(out, s.jscc_repair_underpredicted_blocks);
        out += ",\"jscc_repair_demand_censored_blocks\":";
        append_u64(out, s.jscc_repair_demand_censored_blocks);
        out += ",\"jscc_repair_predicted_parity_symbols\":";
        append_u64(out, s.jscc_repair_predicted_parity_symbols);
        out += ",\"jscc_decision_frames\":";
        append_u64(out, s.jscc_decision_frames);
        out += ",\"jscc_valid_decisions\":";
        append_u64(out, s.jscc_valid_decisions);
        out += ",\"jscc_fallback_decisions\":";
        append_u64(out, s.jscc_fallback_decisions);
        out += ",\"jscc_decision_valid\":";
        append_bool(out, s.jscc_decision_valid);
        out += ",\"jscc_fallback\":";
        append_escaped(out, s.jscc_fallback);
        out += ",\"jscc_reason\":";
        append_escaped(out, s.jscc_reason);
        out += ",\"jscc_input_k\":";
        append_u64(out, s.jscc_input_k);
        out += ",\"jscc_input_predicted_symbols\":";
        append_u64(out, s.jscc_input_predicted_symbols);
        out += ",\"jscc_input_floor_symbols\":";
        append_u64(out, s.jscc_input_floor_symbols);
        out += ",\"jscc_input_cap_symbols\":";
        append_u64(out, s.jscc_input_cap_symbols);
        out += ",\"jscc_input_deadline_us\":";
        append_u64(out, s.jscc_input_deadline_us);
        out += ",\"jscc_input_source_tx_us\":";
        append_u64(out, s.jscc_input_source_tx_us);
        out += ",\"jscc_input_rtt_p95_us\":";
        append_u64(out, s.jscc_input_rtt_p95_us);
        out += ",\"jscc_input_resend_us\":";
        append_u64(out, s.jscc_input_resend_us);
        out += ",\"jscc_input_guard_us\":";
        append_u64(out, s.jscc_input_guard_us);
        out += ",\"jscc_output_parity_symbols\":";
        append_u64(out, s.jscc_output_parity_symbols);
        out += ",\"jscc_output_remaining_us\":";
        append_u64(out, s.jscc_output_remaining_us);
        out += ",\"jscc_output_arq_eligible\":";
        append_bool(out, s.jscc_output_arq_eligible);
        out += ",\"jscc_output_discard\":";
        append_bool(out, s.jscc_output_discard);
        out += ",\"jscc_feedback_epoch\":";
        append_u64(out, s.jscc_feedback_epoch);
        out += ",\"jscc_feedback_age_ms\":";
        append_u64(out, s.jscc_feedback_age_ms);
        out += ",\"jscc_enforced_frames\":";
        append_u64(out, s.jscc_enforced_frames);
        out += ",\"jscc_discarded_frames\":";
        append_u64(out, s.jscc_discarded_frames);
        out += ",\"shm_full_drops\":";
        append_u64(out, s.shm_full_drops);
        out += ",\"shm_oversize_drops\":";
        append_u64(out, s.shm_oversize_drops);
        out += ",\"shm_bad_slots\":";
        append_u64(out, s.shm_bad_slots);
        out += ",\"dropped_superseded\":";
        append_u64(out, s.dropped_superseded);
        out += ",\"dropped_deadline\":";
        append_u64(out, s.dropped_deadline);
        out += ",\"nacks_sent\":";
        append_u64(out, s.nacks_sent);
        out += ",\"nack_rtt_hist\":";
        append_hist(out, s.nack_rtt_hist);
        out += ",\"nack_rtt_max_ms\":";
        append_u64(out, s.nack_rtt_max_ms);
        out += ",\"nack_rtt_samples\":";
        append_u64(out, s.nack_rtt_samples);
        out += ",\"nack_rtt_p95_us\":";
        append_u64(out, s.nack_rtt_p95_us);
        out += ",\"arq_rec_hist\":";
        append_hist(out, s.arq_rec_hist);
        out += ",\"arq_rec_max_ms\":";
        append_u64(out, s.arq_rec_max_ms);
        out += ",\"resends_sent\":";
        append_u64(out, s.resends_sent);
        out += ",\"double_send_suppressed\":";
        append_u64(out, s.double_send_suppressed);
        out += ",\"source_symbols_sent\":";
        append_u64(out, s.source_symbols_sent);
        out += ",\"repair_symbols_sent\":";
        append_u64(out, s.repair_symbols_sent);
        out += ",\"fec_oversize_frames\":";
        append_u64(out, s.fec_oversize_frames);
        out += ",\"idr_frames\":";
        append_u64(out, s.idr_frames);
        out += ",\"arq_frames\":";
        append_u64(out, s.arq_frames);
        out += ",\"arq_cutoff_frames\":";
        append_u64(out, s.arq_cutoff_frames);
        out += ",\"decode_errors\":";
        append_u64(out, s.decode_errors);
        out += ",\"active_profile\":";
        append_u64(out, s.active_profile);
        out += ",\"table_version\":";
        append_u64(out, s.table_version);
        out += '}';
    }
    out += ']';

    // §15.3: cache blocks appear only when the §14.3 role is enabled.
    if (snap.cache_repair) {
        const CacheRepairStatsOut& c = *snap.cache_repair;
        out += ",\"cache_repair\":{\"requests\":";
        append_u64(out, c.requests);
        out += ",\"replies\":";
        append_u64(out, c.replies);
        out += ",\"symbols_accepted\":";
        append_u64(out, c.symbols_accepted);
        out += ",\"symbols_rejected\":";
        append_u64(out, c.symbols_rejected);
        out += ",\"blocks_closed_deficit\":";
        append_u64(out, c.blocks_closed_deficit);
        out += ",\"blocks_repaired\":";
        append_u64(out, c.blocks_repaired);
        out += ",\"blocks_futile\":";
        append_u64(out, c.blocks_futile);
        out += ",\"requests_suppressed\":";
        append_u64(out, c.requests_suppressed);
        out += ",\"caches_fresh\":";
        append_u64(out, c.caches_fresh);
        out += ",\"nack_graces_armed\":";
        append_u64(out, c.nack_graces_armed);
        out += ",\"blocks_repaired_before_nack\":";
        append_u64(out, c.blocks_repaired_before_nack);
        out += ",\"request_to_first_reply\":";
        append_timing(out, c.request_to_first_reply);
        out += ",\"request_to_completion\":";
        append_timing(out, c.request_to_completion);
        out += '}';
    }
    if (snap.cache_store) {
        const CacheStoreStatsOut& c = *snap.cache_store;
        out += ",\"cache_store\":{\"requests_received\":";
        append_u64(out, c.requests_received);
        out += ",\"requests_answered\":";
        append_u64(out, c.requests_answered);
        out += ",\"requests_rejected\":";
        append_u64(out, c.requests_rejected);
        out += ",\"symbols_sent\":";
        append_u64(out, c.symbols_sent);
        out += ",\"status_sent\":";
        append_u64(out, c.status_sent);
        out += ",\"blocks_held\":";
        append_u64(out, c.blocks_held);
        out += ",\"health_permille\":";
        append_u64(out, c.health_permille);
        out += '}';
    }

    out += ",\"arq_timing\":{\"eob_to_nack_build\":";
    append_timing(out, snap.arq_timing.eob_to_nack_build);
    out += ",\"nack_build_to_inject\":";
    append_timing(out, snap.arq_timing.nack_build_to_inject);
    out += ",\"nack_inject_to_retransmit\":";
    append_timing(out, snap.arq_timing.nack_inject_to_retransmit);
    out += ",\"nack_build_to_retransmit\":";
    append_timing(out, snap.arq_timing.nack_build_to_retransmit);
    out += ",\"nack_receive_to_resend\":";
    append_timing(out, snap.arq_timing.nack_receive_to_resend);
    out += '}';

    out += ",\"return\":{\"reports_expected\":";
    append_u64(out, snap.ret.reports_expected);
    out += ",\"reports_received\":";
    append_u64(out, snap.ret.reports_received);
    out += ",\"reports_rejected\":";
    append_u64(out, snap.ret.reports_rejected);
    out += ",\"return_window_hits\":";
    append_u64(out, snap.ret.return_window_hits);
    out += ",\"return_window_misses\":";
    append_u64(out, snap.ret.return_window_misses);
    out += ",\"unicast_sent\":";
    append_u64(out, snap.ret.unicast_sent);
    out += ",\"unicast_fallback\":";
    append_u64(out, snap.ret.unicast_fallback);
    out += '}';

    out += ",\"link\":{\"target_originator\":";
    append_u64(out, snap.link.target_originator);
    out += ",\"target_session\":";
    append_u64(out, snap.link.target_session);
    out += ",\"profile\":";
    append_u64(out, snap.link.profile);
    out += ",\"mcs\":";
    append_u64(out, snap.link.mcs);
    out += ",\"tx_power_qdb\":";
    append_i32(out, snap.link.tx_power_qdb);
    out += ",\"report_epoch\":";
    append_u64(out, snap.link.report_epoch);
    out += ",\"report_age_ms\":";
    append_u64(out, snap.link.report_age_ms);
    out += ",\"state\":";
    append_escaped(out, snap.link.state);
    out += ",\"flap_freeze\":";
    append_bool(out, snap.link.flap_freeze);
    out += ",\"csa_state\":";
    append_escaped(out, snap.link.csa_state);
    out += ",\"venc_bitrate_kbps\":";
    append_u64(out, snap.link.venc_bitrate_kbps);
    out += ",\"venc_max_i_bytes\":";
    append_u64(out, snap.link.venc_max_i_bytes);
    out += ",\"venc_max_p_bytes\":";
    append_u64(out, snap.link.venc_max_p_bytes);
    out += ",\"venc_pushes\":";
    append_u64(out, snap.link.venc_pushes);
    out += ",\"venc_failures\":";
    append_u64(out, snap.link.venc_failures);
    out += ",\"venc_settling\":";
    append_bool(out, snap.link.venc_settling);
    out += ",\"venc_fps\":";
    append_u64(out, snap.link.venc_fps);
    out += "}}\n";
}

void StatsEmitter::emit(const StatsSnapshot& snap) {
    line_.clear();
    format_stats_line(snap, line_);
    if (to_stdout_) {
        std::fwrite(line_.data(), 1, line_.size(), stdout);
        std::fflush(stdout);
    }
    if (udp_ != nullptr) {
        udp_->send(reinterpret_cast<const uint8_t*>(line_.data()), line_.size());
    }
}

}  // namespace wblink
