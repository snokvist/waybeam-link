// SPDX-License-Identifier: GPL-2.0-or-later
// §15.3 stats: the NDJSON line for the spec's own sample values must match a
// golden string byte-for-byte (fixed field order is part of the contract),
// escaping is correct, and the UDP sink receives the same line.
#include "wblink/stats.h"

#include <cstring>
#include <string>

#include "wbtest.h"

using namespace wblink;

namespace {

StatsSnapshot sample_snapshot() {
    StatsSnapshot s;
    s.t_ms = 172834;
    s.node = 17;
    s.session = 2748291;
    AdapterStats a;
    a.name = "wlan0";
    a.rx = 10234;
    a.dup = 812;
    a.rssi_best = -58;
    a.rssi_mean = -63;
    a.snr = 22;
    a.noise = -85;
    a.tx_submitted = 540;
    a.tx_failed = 2;
    a.tx_timeout = 0;
    a.drop = 3;
    a.tsf_fallback = 1;
    a.tx_reports = 40;
    a.tx_report_fails = 2;
    // §15.3 Pass 118: distinct per-bucket values so the golden pins ordering,
    // not just the field's presence. Sums to rx (10234) with the unknowns.
    a.rx_mcs[0] = 1;
    a.rx_mcs[1] = 2;
    a.rx_mcs[2] = 3;
    a.rx_mcs[3] = 4;
    a.rx_mcs[4] = 118;
    a.rx_mcs[5] = 10099;
    a.rx_mcs[6] = 5;
    a.rx_mcs[7] = 0;
    a.rx_mcs_unknown = 2;
    a.adapter_stalled = false;
    s.adapters.push_back(a);
    StreamStats st;
    st.stream_id = 0;
    st.type = "RTP";
    st.seq = 90233;
    st.delivered = 89901;
    st.uniq = 90100;
    st.diversity = 178342;
    st.loss_prediversity_milli = 41;
    st.loss_postdiv_prearq_milli = 6;
    st.recovered_arq = 220;
    st.recovered_fec = 0;
    st.fec_recovered_source_symbols = 173;
    st.arq_recovered_source_symbols = 201;
    st.arq_recovered_repair_symbols = 19;
    st.frames_with_arq = 187;
    st.frames_fec_only = 91;
    st.frames_fec_after_arq = 34;
    st.frame_count = 89571;
    st.frame_bytes = 5872391040;
    st.frame_size_last = 65432;
    st.frame_size_min = 8120;
    st.frame_size_max = 241810;
    st.frame_interval_us = 11106;
    st.frame_jitter_us = 184;
    st.frames_fast = 89571;
    st.frames_unrecoverable = 0;
    st.malformed = 0;
    st.jscc_shadow_blocks = 89681;
    st.jscc_predicted_loss_symbols = 3;
    st.jscc_observed_loss_symbols = 1;
    st.jscc_underpredicted_blocks = 72;
    st.jscc_predicted_parity_symbols = 271044;
    st.jscc_predicted_repair_symbols = 4;
    st.jscc_observed_repair_symbols = 3;
    st.jscc_repair_underpredicted_blocks = 18;
    st.jscc_repair_demand_censored_blocks = 2;
    st.jscc_repair_predicted_parity_symbols = 358121;
    st.jscc_decision_frames = 89571;
    st.jscc_valid_decisions = 89200;
    st.jscc_fallback_decisions = 371;
    st.jscc_decision_valid = true;
    st.jscc_fallback = "none";
    st.jscc_reason = "fec_and_arq";
    st.jscc_input_k = 38;
    st.jscc_input_predicted_symbols = 5;
    st.jscc_input_floor_symbols = 1;
    st.jscc_input_cap_symbols = 16;
    st.jscc_input_deadline_us = 16667;
    st.jscc_input_source_tx_us = 5210;
    st.jscc_input_rtt_p95_us = 2000;
    st.jscc_input_resend_us = 116;
    st.jscc_input_guard_us = 500;
    st.jscc_output_parity_symbols = 5;
    st.jscc_output_remaining_us = 11457;
    st.jscc_output_arq_eligible = true;
    st.jscc_output_discard = false;
    st.jscc_feedback_epoch = 1821;
    st.jscc_feedback_age_ms = 42;
    st.dropped_superseded = 110;
    st.dropped_deadline = 8;
    st.nacks_sent = 18;
    st.nack_rtt_hist = {0, 2, 7, 6, 2, 1, 0, 0};
    st.nack_rtt_max_ms = 34;
    st.nack_rtt_samples = 24;
    st.nack_rtt_p95_us = 2000;
    st.arq_rec_hist = {0, 1, 6, 6, 3, 1, 1, 0};
    st.arq_rec_max_ms = 61;
    st.resends_sent = 230;
    st.arq_lock_holder = 9;
    st.double_send_suppressed = 5;
    st.source_symbols_sent = 4120300;
    st.repair_symbols_sent = 358944;
    st.fec_oversize_frames = 0;
    st.idr_frames = 17;
    st.arq_frames = 68342;
    st.decode_errors = 0;
    st.active_profile = 4;
    st.table_version = 178;
    s.streams.push_back(st);
    // expected, received, reports_rejected, feedback_rejected,
    // report_latch_holder, window hits, window misses (§3.5 Pass 115 added
    // the middle two — this is positional, so it must move with the struct).
    s.ret = ReturnStats{10, 9, 0, 0, 9, 7, 2};
    s.link.target_originator = 9;
    s.link.target_session = 183726;
    s.link.profile = 4;
    s.link.mcs = 4;
    s.link.tx_power_qdb = 1800;
    s.link.tx_power_override = true;  // §10.5 Pass 114
    s.link.report_epoch = 1822;
    s.link.report_age_ms = 40;
    s.link.state = "HOLD";
    s.link.transition_reason = "LOSS_PERSISTENT";
    s.link.loss_window_milli = 48;
    s.link.loss_ewma_milli = 36;
    s.link.loss_uniq = 100;
    s.link.loss_score = 5;
    s.link.safe_floor_profile = 1;
    s.link.report_latch_holder = 9;   // §3.15a Pass 117
    s.link.report_latch_known = true;
    s.link.selector_state_valid = true;
    s.link.selector_state_age_ms = 75;
    s.link.lockout_active = true;
    s.link.lockout_profile = 5;
    s.link.lockout_ceiling_profile = 4;
    s.link.lockout_remaining_ms = 29925;
    s.link.lockout_strikes = 2;
    s.link.lockout_active_mask = 32;
    s.link.flap_freeze = false;
    s.link.csa_state = "IDLE";
    s.link.channel_mhz = 5805;
    s.link.venc_bitrate_kbps = 14000;
    s.link.venc_max_i_bytes = 70000;
    s.link.venc_max_p_bytes = 19444;
    s.link.venc_pushes = 6;
    s.link.venc_failures = 0;
    s.link.venc_settling = false;
    s.link.venc_fps = 90;
    s.link.venc_p_frame_bytes = 12345;
    s.link.venc_p_frame_target_bytes = 10000;
    s.link.venc_fps_ladder_state = "HOLD";
    // §11.7 command surface (§15.3): craft applied state, issuer campaign,
    // rx NACK-emission gate.
    s.link.cmd_arq = true;
    s.link.cmd_selector_frozen = false;
    s.link.cmd_fps_ladder = true;
    s.link.cmd_last_nonce = 3054418130;
    s.link.cmd_fps_select = 2;
    s.link.cmd_resolution_select = 0;
    s.link.cmd_framing_select = 0;
    s.link.vcmd_state = "acked";
    s.link.vcmd_nonce = 3054418130;
    s.link.arq_rx_enabled = false;
    // §10.6 Pass 120: distinct values so the golden pins the calib mirror.
    s.link.calib_state = "running";
    s.link.calib_rung = 5;
    s.link.calib_fingerprint = 0xBF;
    s.link.calib_stale = true;
    return s;
}

// The §15.3 sample, one line, fixed field order.
const char* kGolden =
    "{\"t_ms\":172834,\"node\":17,\"session\":2748291,"
    "\"adapters\":[{\"name\":\"wlan0\",\"rx\":10234,\"dup\":812,"
    "\"rssi_best\":-58,\"rssi_mean\":-63,\"snr\":22,\"noise\":-85,"
    "\"tx_submitted\":540,\"tx_failed\":2,\"tx_timeout\":0,"
    "\"drop\":3,\"filtered\":0,\"kernel_drop\":0,\"bpf_filtered\":0,\"tsf_fallback\":1,"
    "\"tx_reports\":40,"
    "\"tx_report_fails\":2,"
    "\"rx_mcs\":[1,2,3,4,118,10099,5,0],\"rx_mcs_unknown\":2,"
    "\"adapter_stalled\":false,\"rx_dead\":false,\"tx_wedged\":false}],"
    "\"streams\":[{\"stream_id\":0,\"type\":\"RTP\",\"seq\":90233,"
    "\"delivered\":89901,\"uniq\":90100,\"diversity\":178342,"
    "\"loss_prediversity_milli\":41,"
    "\"loss_postdiv_prearq_milli\":6,\"recovered_arq\":220,"
    "\"recovered_fec\":0,\"fec_recovered_source_symbols\":173,"
    "\"arq_recovered_source_symbols\":201,"
    "\"arq_recovered_repair_symbols\":19,\"frames_with_arq\":187,"
    "\"frames_fec_only\":91,\"frames_fec_after_arq\":34,"
    "\"frame_count\":89571,"
    "\"frame_bytes\":5872391040,\"frame_size_last\":65432,"
    "\"frame_size_min\":8120,\"frame_size_max\":241810,"
    "\"frame_interval_us\":11106,\"frame_jitter_us\":184,"
    "\"frames_fast\":89571,\"frames_unrecoverable\":0,"
    "\"malformed\":0,\"jscc_shadow_blocks\":89681,"
    "\"jscc_predicted_loss_symbols\":3,"
    "\"jscc_observed_loss_symbols\":1,"
    "\"jscc_underpredicted_blocks\":72,"
    "\"jscc_predicted_parity_symbols\":271044,"
    "\"jscc_predicted_repair_symbols\":4,"
    "\"jscc_observed_repair_symbols\":3,"
    "\"jscc_repair_underpredicted_blocks\":18,"
    "\"jscc_repair_demand_censored_blocks\":2,"
    "\"jscc_repair_predicted_parity_symbols\":358121,"
    "\"jscc_decision_frames\":89571,\"jscc_valid_decisions\":89200,"
    "\"jscc_fallback_decisions\":371,\"jscc_decision_valid\":true,"
    "\"jscc_fallback\":\"none\",\"jscc_reason\":\"fec_and_arq\","
    "\"jscc_input_k\":38,\"jscc_input_predicted_symbols\":5,"
    "\"jscc_input_floor_symbols\":1,\"jscc_input_cap_symbols\":16,"
    "\"jscc_input_deadline_us\":16667,\"jscc_input_source_tx_us\":5210,"
    "\"jscc_input_rtt_p95_us\":2000,\"jscc_input_resend_us\":116,"
    "\"jscc_input_guard_us\":500,\"jscc_output_parity_symbols\":5,"
    "\"jscc_output_remaining_us\":11457,"
    "\"jscc_output_arq_eligible\":true,\"jscc_output_discard\":false,"
    "\"jscc_feedback_epoch\":1821,\"jscc_feedback_age_ms\":42,"
    "\"jscc_enforced_frames\":0,\"jscc_discarded_frames\":0,"
    "\"shm_health_valid\":false,\"shm_full_drops\":0,"
    "\"shm_throttle_permille\":0,\"shm_oversize_drops\":0,"
    "\"shm_bad_slots\":0,\"shm_ring_full\":0,"
    "\"dropped_superseded\":110,"
    "\"dropped_deadline\":8,"
    "\"nacks_sent\":18,"
    "\"nack_rtt_hist\":[0,2,7,6,2,1,0,0],\"nack_rtt_max_ms\":34,"
    "\"nack_rtt_samples\":24,\"nack_rtt_p95_us\":2000,"
    "\"arq_rec_hist\":[0,1,6,6,3,1,1,0],\"arq_rec_max_ms\":61,"
    "\"resends_sent\":230,\"arq_lock_holder\":9,"
    "\"double_send_suppressed\":5,"
    "\"source_symbols_sent\":4120300,\"repair_symbols_sent\":358944,"
    "\"fec_oversize_frames\":0,\"idr_frames\":17,\"arq_frames\":68342,"
    "\"arq_cutoff_frames\":0,"
    "\"decode_errors\":0,\"active_profile\":4,\"table_version\":178}],"
    "\"arq_timing\":{"
    "\"eob_to_nack_build\":{\"samples\":0,\"p95_us\":0,\"max_us\":0},"
    "\"nack_build_to_inject\":{\"samples\":0,\"p95_us\":0,\"max_us\":0},"
    "\"nack_inject_to_retransmit\":{\"samples\":0,\"p95_us\":0,\"max_us\":0},"
    "\"nack_build_to_retransmit\":{\"samples\":0,\"p95_us\":0,\"max_us\":0},"
    "\"nack_receive_to_resend\":{\"samples\":0,\"p95_us\":0,\"max_us\":0}},"
    "\"return\":{\"reports_expected\":10,\"reports_received\":9,"
    "\"reports_rejected\":0,\"feedback_rejected\":0,"
    "\"report_latch_holder\":9,"
    "\"return_window_hits\":7,\"return_window_misses\":2,"
    "\"unicast_sent\":0,\"unicast_fallback\":0},"
    "\"link\":{\"target_originator\":9,\"target_session\":183726,"
    "\"profile\":4,\"mcs\":4,\"tx_power_qdb\":1800,\"tx_power_override\":true,"
    "\"report_epoch\":1822,"
    "\"report_age_ms\":40,\"state\":\"HOLD\","
    "\"transition_reason\":\"LOSS_PERSISTENT\","
    "\"loss_window_milli\":48,\"loss_ewma_milli\":36,\"loss_uniq\":100,"
    "\"loss_score\":5,\"safe_floor_profile\":1,"
    "\"report_latch_holder\":9,\"report_latch_known\":true,"
    "\"selector_state_valid\":true,\"selector_state_age_ms\":75,"
    "\"lockout_active\":true,\"lockout_latched\":false,"
    "\"lockout_profile\":5,\"lockout_ceiling_profile\":4,"
    "\"lockout_remaining_ms\":29925,\"lockout_strikes\":2,"
    "\"lockout_active_mask\":32,\"lockout_latched_mask\":0,"
    "\"lockout_conflict\":false,\"flap_freeze\":false,"
    "\"csa_state\":\"IDLE\",\"channel\":5805,\"venc_bitrate_kbps\":14000,"
    "\"venc_max_i_bytes\":70000,\"venc_max_p_bytes\":19444,"
    "\"venc_pushes\":6,\"venc_failures\":0,\"venc_settling\":false,"
    "\"venc_fps\":90,\"venc_p_frame_bytes\":12345,"
    "\"venc_p_frame_target_bytes\":10000,"
    "\"venc_fps_ladder_state\":\"HOLD\","
    "\"cmd_arq\":true,\"cmd_selector_frozen\":false,"
    "\"cmd_fps_ladder\":true,\"cmd_last_nonce\":3054418130,"
    "\"cmd_fps_select\":2,\"cmd_resolution_select\":0,"
    "\"cmd_framing_select\":0,"
    "\"vcmd_state\":\"acked\",\"vcmd_nonce\":3054418130,"
    "\"arq_rx_enabled\":false,"
    "\"calib_state\":\"running\",\"calib_rung\":5,"
    "\"calib_fingerprint\":191,\"calib_stale\":true}}\n";

}  // namespace

int main() {
    // Golden line, byte-for-byte.
    {
        std::string out;
        format_stats_line(sample_snapshot(), out);
        ++wbtest::checks;
        if (out != kGolden) {
            std::fprintf(stderr, "golden mismatch:\n got: %s want: %s",
                         out.c_str(), kGolden);
            ++wbtest::failures;
        }
    }

    // Escaping: quotes/backslash/control chars in a name stay valid JSON.
    {
        StatsSnapshot s;
        AdapterStats a;
        a.name = "we\"ird\\na\nme";
        s.adapters.push_back(a);
        std::string out;
        format_stats_line(s, out);
        CHECK(out.find("we\\\"ird\\\\na\\u000ame") != std::string::npos);
    }

    // Empty snapshot is still a complete, valid schema.
    {
        StatsSnapshot s;
        std::string out;
        format_stats_line(s, out);
        CHECK(out.find("\"adapters\":[]") != std::string::npos);
        CHECK(out.find("\"streams\":[]") != std::string::npos);
        CHECK(out.find("\"arq_timing\":{") != std::string::npos);
        CHECK(out.find("\"return\":{") != std::string::npos);
        CHECK(out.find("\"link\":{") != std::string::npos);
        CHECK(out.back() == '\n');
    }

    // The UDP sink receives the exact same line.
    {
        auto in = UdpIngress::open("127.0.0.1:0");
        CHECK(bool(in));
        auto out = UdpEgress::open("127.0.0.1:" +
                                   std::to_string(in.value->bound_port()));
        CHECK(bool(out));
        StatsEmitter emitter(/*to_stdout=*/false, &*out.value);
        emitter.emit(sample_snapshot());

        uint8_t buf[8192];
        long n = 0;
        for (int tries = 0; tries < 100 && n <= 0; ++tries) {
            n = in.value->recv_one(buf, sizeof(buf));
        }
        const size_t golden_len = std::strlen(kGolden);
        CHECK_EQ_U(static_cast<unsigned long long>(n), golden_len);
        CHECK(n >= 0 && static_cast<size_t>(n) == golden_len &&
              std::memcmp(buf, kGolden, golden_len) == 0);
    }

    // §15.3 cache blocks: absent by default, exact shape when enabled,
    // placed between the streams array and the return block.
    {
        StatsSnapshot s;
        std::string out;
        format_stats_line(s, out);
        CHECK(out.find("cache_repair") == std::string::npos);
        CHECK(out.find("cache_store") == std::string::npos);

        CacheRepairStatsOut cr;
        cr.requests = 12;
        cr.replies = 11;
        cr.symbols_accepted = 18;
        cr.blocks_closed_deficit = 9;
        cr.blocks_repaired = 7;
        cr.blocks_futile = 1;
        cr.requests_suppressed = 2;
        cr.caches_fresh = 2;
        cr.nack_graces_armed = 8;
        cr.blocks_repaired_before_nack = 6;
        cr.request_to_first_reply = {10, 1200, 2400};
        cr.request_to_completion = {7, 2300, 4100};
        s.cache_repair = cr;
        CacheStoreStatsOut cs;
        cs.requests_received = 12;
        cs.requests_answered = 11;
        cs.requests_rejected = 1;
        cs.symbols_sent = 18;
        cs.status_sent = 240;
        cs.blocks_held = 96;
        cs.health_permille = 971;
        s.cache_store = cs;
        out.clear();
        format_stats_line(s, out);
        const char* want_repair =
            "\"cache_repair\":{\"requests\":12,\"replies\":11,"
            "\"symbols_accepted\":18,\"symbols_rejected\":0,"
            "\"blocks_closed_deficit\":9,\"blocks_repaired\":7,"
            "\"blocks_futile\":1,\"requests_suppressed\":2,"
            "\"caches_fresh\":2,\"nack_graces_armed\":8,"
            "\"blocks_repaired_before_nack\":6,"
            "\"request_to_first_reply\":{\"samples\":10,"
            "\"p95_us\":1200,\"max_us\":2400},"
            "\"request_to_completion\":{\"samples\":7,"
            "\"p95_us\":2300,\"max_us\":4100}}";
        const char* want_store =
            "\"cache_store\":{\"requests_received\":12,"
            "\"requests_answered\":11,\"requests_rejected\":1,"
            "\"symbols_sent\":18,\"status_sent\":240,\"blocks_held\":96,"
            "\"health_permille\":971}";
        CHECK(out.find(want_repair) != std::string::npos);
        CHECK(out.find(want_store) != std::string::npos);
        CHECK(out.find("\"streams\"") < out.find("\"cache_repair\""));
        CHECK(out.find("\"cache_repair\"") < out.find("\"cache_store\""));
        CHECK(out.find("\"cache_store\"") < out.find("\"return\""));
    }

    return wbtest_finish("stats_test");
}
