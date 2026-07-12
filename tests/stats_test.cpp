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
    st.frames_fast = 89571;
    st.frames_unrecoverable = 0;
    st.malformed = 0;
    st.dropped_superseded = 110;
    st.dropped_deadline = 8;
    st.nacks_sent = 18;
    st.nack_rtt_hist = {0, 2, 7, 6, 2, 1, 0, 0};
    st.nack_rtt_max_ms = 34;
    st.arq_rec_hist = {0, 1, 6, 6, 3, 1, 1, 0};
    st.arq_rec_max_ms = 61;
    st.resends_sent = 230;
    st.double_send_suppressed = 5;
    st.decode_errors = 0;
    st.active_profile = 4;
    st.table_version = 178;
    s.streams.push_back(st);
    s.ret = ReturnStats{10, 9, 7, 2};
    s.link.target_originator = 9;
    s.link.target_session = 183726;
    s.link.profile = 4;
    s.link.mcs = 4;
    s.link.tx_power_qdb = 1800;
    s.link.report_epoch = 1822;
    s.link.report_age_ms = 40;
    s.link.state = "HOLD";
    s.link.flap_freeze = false;
    s.link.csa_state = "IDLE";
    return s;
}

// The §15.3 sample, one line, fixed field order.
const char* kGolden =
    "{\"t_ms\":172834,\"node\":17,\"session\":2748291,"
    "\"adapters\":[{\"name\":\"wlan0\",\"rx\":10234,\"dup\":812,"
    "\"rssi_best\":-58,\"rssi_mean\":-63,\"snr\":22,\"noise\":-85,"
    "\"tx_submitted\":540,\"tx_failed\":2,\"tx_timeout\":0,"
    "\"drop\":3,\"tsf_fallback\":1,\"tx_reports\":40,\"tx_report_fails\":2,"
    "\"adapter_stalled\":false,\"tx_wedged\":false}],"
    "\"streams\":[{\"stream_id\":0,\"type\":\"RTP\",\"seq\":90233,"
    "\"delivered\":89901,\"uniq\":90100,\"diversity\":178342,"
    "\"loss_prediversity_milli\":41,"
    "\"loss_postdiv_prearq_milli\":6,\"recovered_arq\":220,"
    "\"recovered_fec\":0,\"frames_fast\":89571,\"frames_unrecoverable\":0,"
    "\"malformed\":0,\"dropped_superseded\":110,\"dropped_deadline\":8,"
    "\"nacks_sent\":18,"
    "\"nack_rtt_hist\":[0,2,7,6,2,1,0,0],\"nack_rtt_max_ms\":34,"
    "\"arq_rec_hist\":[0,1,6,6,3,1,1,0],\"arq_rec_max_ms\":61,"
    "\"resends_sent\":230,\"double_send_suppressed\":5,"
    "\"decode_errors\":0,\"active_profile\":4,\"table_version\":178}],"
    "\"return\":{\"reports_expected\":10,\"reports_received\":9,"
    "\"return_window_hits\":7,\"return_window_misses\":2,"
    "\"unicast_sent\":0,\"unicast_fallback\":0},"
    "\"link\":{\"target_originator\":9,\"target_session\":183726,"
    "\"profile\":4,\"mcs\":4,\"tx_power_qdb\":1800,\"report_epoch\":1822,"
    "\"report_age_ms\":40,\"state\":\"HOLD\",\"flap_freeze\":false,"
    "\"csa_state\":\"IDLE\"}}\n";

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

        uint8_t buf[2048];
        long n = 0;
        for (int tries = 0; tries < 100 && n <= 0; ++tries) {
            n = in.value->recv_one(buf, sizeof(buf));
        }
        CHECK_EQ_U(static_cast<unsigned long long>(n), std::strlen(kGolden));
        CHECK(std::memcmp(buf, kGolden, static_cast<size_t>(n)) == 0);
    }

    return wbtest_finish("stats_test");
}
