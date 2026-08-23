// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/node/features.h"

#include <string>

#include "wbtest.h"

using namespace wblink;
using namespace wblink::node;

int main() {
    Loaded l;
    l.cfg.air.kind = AirCfg::Kind::kRadio;
    l.cfg.air.ldpc = true;
    l.cfg.air.mcs_probe = true;
    l.table.probe_period = 100;
    l.cfg.venc.enabled = true;
    l.cfg.venc.recovery_enabled = true;

    StreamCfg video;
    video.stream_id = 7;
    video.stream_type = stream_type::kRtp;
    video.dir = Dir::kOut;
    video.bind.kind = BindKind::kFrameShm;
    video.conceal_enabled = true;
    video.conceal_freeze_frame = true;
    video.fec.scheme = FecScheme::kRlc256;
    video.fec.i_rate_permille = 300;
    video.fec.p_rate_permille = 200;
    video.fec.e_rate_permille.reset();
    video.fec.min_k = 3;
    video.fec.min_r = 2;
    l.cfg.streams.push_back(video);

    const std::string j = build_features_json(l, false, true);
    CHECK(j.find("\"backend\":\"radio\"") != std::string::npos);
    CHECK(j.find("\"ldpc\":true") != std::string::npos);
    CHECK(j.find("\"mcs_probe_scheduled\":true") != std::string::npos);
    CHECK(j.find("\"stream_id\":7") != std::string::npos);
    CHECK(j.find("\"direction\":\"out\"") != std::string::npos);
    CHECK(j.find("\"arq_mode\":\"receive\"") != std::string::npos);
    CHECK(j.find("\"arq_enabled\":false") != std::string::npos);
    CHECK(j.find("\"e_permille\":200") != std::string::npos);
    CHECK(j.find("\"mode\":\"slice-skip\"") != std::string::npos);
    CHECK(j.find("\"freeze_frame\":true") != std::string::npos);
    CHECK(j.find("\"recovery_enabled\":true") != std::string::npos);
    CHECK(j.find("\"fps_ladder_enabled\":true") != std::string::npos);

    Loaded empty;
    const std::string empty_j = build_features_json(empty, false, false);
    CHECK(empty_j.find("\"present\":false") != std::string::npos);
    CHECK(empty_j.find("\"spatial_recovery\":{\"mode\":\"off\","
                       "\"freeze_frame\":false}") != std::string::npos);

    return wbtest_finish("features_test");
}
