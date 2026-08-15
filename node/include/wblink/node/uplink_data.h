// SPDX-License-Identifier: GPL-2.0-or-later
// §7.5 (Pass 183) uplink data plane — both halves' policy state at namespace
// scope so the admission matrix and hold/budget policy are reachable from
// unit tests (the app_test seam rule: interior lambda state is untestable).
//
// Ground half (UplinkDataStream): per-stream framing + CONTROL latest-state /
// TELEMETRY FIFO hold + pps budget. The caller supplies the §7.2 pacing
// decision (gated) and the send path; time is injected.
// Craft half (UplinkAcceptor): configured-stream match, §11.5a bound-issuer
// gate, strictly-monotonic seq cursor per sender session, then delivery.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "wblink/config.h"
#include "wblink/framer.h"
#include "wblink/stats.h"
#include "wblink/types.h"
#include "wblink/wire.h"

namespace wblink {

struct UplinkDataStream {
    uint8_t stream_id = 0;
    uint8_t stype = 0;
    Framer framer;
    std::deque<std::vector<uint8_t>> held;
    uint64_t pps_win_start_us = 0;
    uint32_t pps_in_win = 0;
    UplinkDataStreamStats st;

    UplinkDataStream(uint8_t sid, uint8_t stream_type_in,
                     const FramerConfig& fc)
        : stream_id(sid), stype(stream_type_in), framer(fc) {
        st.stream_id = sid;
        st.type =
            stype == stream_type::kControl ? "CONTROL" : "TELEMETRY";
        st.tx = true;
    }

    // Frames one ingress datagram. gated=false (no scheduled §7.2 window)
    // sends immediately; gated=true holds under the §7.5 policy. Returns
    // true iff a frame was newly held — the caller arms the blind fallback.
    template <typename SendNow>
    bool on_datagram(const uint8_t* d, size_t n, uint64_t now_ms_in,
                     uint64_t now_us_in, bool gated, const UplinkPolicy& pol,
                     const SendNow& send_now) {
        ++st.submitted;
        if (now_us_in - pps_win_start_us >= 1000000) {
            pps_win_start_us = now_us_in;
            pps_in_win = 0;
        }
        if (++pps_in_win > pol.pps_budget) {
            ++st.dropped_budget;
            return false;
        }
        bool held_one = false;
        framer.on_datagram(
            d, n, now_ms_in,
            [&](const uint8_t* f, size_t fn, const DataHeader&, uint64_t) {
                if (!gated) {
                    send_now(f, fn);
                    ++st.sent;
                    return;
                }
                if (stype == stream_type::kControl) {
                    // depth 1: latest state wins
                    if (!held.empty()) {
                        st.dropped_stale += held.size();
                        held.clear();
                    }
                } else if (held.size() >= pol.telemetry_hold) {
                    ++st.dropped_stale;  // drop-oldest
                    held.pop_front();
                }
                held.emplace_back(f, f + fn);
                held_one = true;
            });
        return held_one;
    }

    template <typename SendNow>
    void flush(const SendNow& send_now) {
        for (const auto& f : held) {
            send_now(f.data(), f.size());
            ++st.sent;
        }
        held.clear();
    }
};

struct UplinkAcceptor {
    uint8_t stream_id = 0;
    uint8_t stype = 0;
    // §7.5 strictly-monotonic accept cursor, reset per sender session.
    uint16_t src_originator = 0;
    uint32_t src_session = 0;
    bool have_seq = false;
    uint32_t last_seq = 0;
    UplinkDataStreamStats st;

    UplinkAcceptor(uint8_t sid, uint8_t stream_type_in)
        : stream_id(sid), stype(stream_type_in) {
        st.stream_id = sid;
        st.type =
            stype == stream_type::kControl ? "CONTROL" : "TELEMETRY";
        st.tx = false;
    }

    // Returns true iff dv belongs to this stream (stream_id match) —
    // accepted or counted-rejected either way. false = not this stream's
    // frame (on a shared channel that is another node's stream, not a
    // rejection — no counter).
    template <typename Deliver>
    bool on_data(const DataView& dv, std::optional<uint16_t> bound,
                 const Deliver& deliver) {
        if (dv.hdr.stream_id != stream_id) {
            return false;
        }
        if (dv.hdr.stream_type != stype) {
            ++st.rej_stream;
            return true;
        }
        if (!bound || *bound != dv.hdr.prefix.originator) {
            ++st.rej_unbound;
            return true;
        }
        if (have_seq && src_originator == dv.hdr.prefix.originator &&
            src_session == dv.hdr.prefix.session_id &&
            dv.hdr.seq <= last_seq) {
            ++st.dup;
            return true;
        }
        src_originator = dv.hdr.prefix.originator;
        src_session = dv.hdr.prefix.session_id;
        have_seq = true;
        last_seq = dv.hdr.seq;
        deliver(dv.payload, dv.payload_len);
        ++st.accepted;
        return true;
    }
};

}  // namespace wblink
