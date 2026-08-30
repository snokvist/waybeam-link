// SPDX-License-Identifier: GPL-2.0-or-later
// Sanitized §15.5 effective-feature summary for local management clients.
#pragma once

#include <string>

#include "wblink/node/load.h"

namespace wblink::node {

inline const char* feature_air_backend(AirCfg::Kind kind) {
    switch (kind) {
        case AirCfg::Kind::kUdp: return "udp";
        case AirCfg::Kind::kUdpBroadcast: return "udp-broadcast";
        case AirCfg::Kind::kRadio: return "radio";
        case AirCfg::Kind::kNone: return "none";
    }
    return "none";
}

inline const char* feature_fec_scheme(FecScheme scheme) {
    switch (scheme) {
        case FecScheme::kRlc256: return "rlc256";
        case FecScheme::kRlc256Iframe: return "rlc256_iframe";
        case FecScheme::kTetrysReactive: return "tetrys_reactive";
        case FecScheme::kNone: return "none";
    }
    return "none";
}

// `arq_enabled` is the §11.7 operator latch (PROTOCOL.md §15.5). `arq_effective`
// is whether ARQ can actually run: the §3.4 best-effort fallback switches NACK
// generation off inside the RX engine, which the latch cannot see, so for two
// hours on 2026-08-30 this endpoint reported ARQ enabled on a stream that had
// not sent a NACK since it latched. On a TX node the two are equal — a sender
// has no receive engine to be downgraded.
inline std::string build_features_json(const Loaded& l, bool arq_enabled,
                                       bool fps_ladder_enabled,
                                       bool arq_effective) {
    const StreamCfg* video = nullptr;
    for (const StreamCfg& stream : l.cfg.streams) {
        if (stream.stream_type == stream_type::kRtp) {
            video = &stream;
            break;
        }
    }

    std::string out = "{\"air\":{\"backend\":\"";
    out += feature_air_backend(l.cfg.air.kind);
    out += "\",\"ldpc\":";
    out += l.cfg.air.ldpc ? "true" : "false";
    out += ",\"stbc\":";
    out += l.cfg.air.stbc ? "true" : "false";
    out += ",\"mcs_probe_configured\":";
    out += l.cfg.air.mcs_probe ? "true" : "false";
    out += ",\"mcs_probe_scheduled\":";
    out += l.cfg.air.mcs_probe && l.table.probe_period != 0 ? "true" : "false";
    out += "},\"video\":{";

    if (video == nullptr) {
        out += "\"present\":false,\"stream_id\":0,\"direction\":\"none\",";
        out += "\"binding\":\"none\",\"arq_mode\":\"none\",";
        out += "\"arq_enabled\":";
        out += arq_enabled ? "true" : "false";
        out += ",\"arq_effective\":";
        out += arq_effective ? "true" : "false";
        out += ",\"fec\":{\"scheme\":\"none\",\"i_permille\":0,";
        out += "\"p_permille\":0,\"e_permille\":0,\"min_k\":0,\"min_r\":0},";
        out += "\"spatial_recovery\":{\"mode\":\"off\",\"freeze_frame\":false},";
        out += "\"jscc\":{\"configured\":false,\"enforce\":false}";
    } else {
        out += "\"present\":true,\"stream_id\":" +
               std::to_string(video->stream_id);
        out += ",\"direction\":\"";
        out += video->dir == Dir::kIn ? "in" : "out";
        out += "\",\"binding\":\"";
        out += video->bind.kind == BindKind::kFrameShm ? "frame-shm" : "udp";
        out += "\",\"arq_mode\":\"";
        out += video->dir == Dir::kIn
                   ? (video->arq_mode == FrameArqMode::kAllFrames
                          ? "all-frames" : "idr-only")
                   : "receive";
        out += "\",\"arq_enabled\":";
        out += arq_enabled ? "true" : "false";
        out += ",\"arq_effective\":";
        out += arq_effective ? "true" : "false";
        out += ",\"fec\":{\"scheme\":\"";
        out += feature_fec_scheme(video->fec.scheme);
        out += "\",\"i_permille\":" +
               std::to_string(video->fec.i_rate_permille);
        out += ",\"p_permille\":" +
               std::to_string(video->fec.p_rate_permille);
        out += ",\"e_permille\":" +
               std::to_string(video->fec.e_rate_permille.value_or(
                   video->fec.p_rate_permille));
        out += ",\"min_k\":" + std::to_string(video->fec.min_k);
        out += ",\"min_r\":" + std::to_string(video->fec.min_r) + "},";
        out += "\"spatial_recovery\":{\"mode\":\"";
        out += video->conceal_enabled ? "slice-skip" : "off";
        out += "\",\"freeze_frame\":";
        out += video->conceal_enabled && video->conceal_freeze_frame
                   ? "true" : "false";
        out += "},\"jscc\":{\"configured\":";
        out += video->jscc_shadow ? "true" : "false";
        out += ",\"enforce\":";
        out += video->jscc_shadow && video->jscc_shadow->enforce
                   ? "true" : "false";
        out += "}";
    }

    out += "},\"venc\":{\"enabled\":";
    out += l.cfg.venc.enabled ? "true" : "false";
    out += ",\"recovery_enabled\":";
    out += l.cfg.venc.recovery_enabled ? "true" : "false";
    out += ",\"fps_ladder_boot\":";
    out += l.cfg.venc.fps_ladder.enabled ? "true" : "false";
    out += ",\"fps_ladder_enabled\":";
    out += fps_ladder_enabled ? "true" : "false";

    // §11.1 channel policy. This is the only place the allowlist is readable
    // over REST: a rejected §15.5a claim returns one opaque string covering
    // allowlist, active-campaign and rate-limit alike, so an operator aiming
    // the radio had no way to see which channels the node would even accept.
    // It belongs here rather than in §15.3 because it is loaded configuration,
    // not a running measurement, and /features is defined as exactly that. The
    // PSK stays out — the key VALUE is the secret, the channel policy is not.
    out += "},\"csa\":{\"home_chan\":" +
           std::to_string(l.cfg.policy.csa.home_chan);
    out += ",\"channel_allowlist\":[";
    bool first_chan = true;
    for (uint16_t mhz : l.cfg.policy.csa.channel_allowlist) {
        if (!first_chan) out += ',';
        first_chan = false;
        out += std::to_string(mhz);
    }
    out += "]}}";
    return out;
}

}  // namespace wblink::node
