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
        out += ",\"frames_fast\":";
        append_u64(out, s.frames_fast);
        out += ",\"frames_unrecoverable\":";
        append_u64(out, s.frames_unrecoverable);
        out += ",\"malformed\":";
        append_u64(out, s.malformed);
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
        out += ",\"arq_rec_hist\":";
        append_hist(out, s.arq_rec_hist);
        out += ",\"arq_rec_max_ms\":";
        append_u64(out, s.arq_rec_max_ms);
        out += ",\"resends_sent\":";
        append_u64(out, s.resends_sent);
        out += ",\"double_send_suppressed\":";
        append_u64(out, s.double_send_suppressed);
        out += ",\"decode_errors\":";
        append_u64(out, s.decode_errors);
        out += ",\"active_profile\":";
        append_u64(out, s.active_profile);
        out += ",\"table_version\":";
        append_u64(out, s.table_version);
        out += '}';
    }
    out += ']';

    out += ",\"return\":{\"reports_expected\":";
    append_u64(out, snap.ret.reports_expected);
    out += ",\"reports_received\":";
    append_u64(out, snap.ret.reports_received);
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
