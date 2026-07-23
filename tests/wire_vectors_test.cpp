// SPDX-License-Identifier: GPL-2.0-or-later
// Golden byte vectors, hand-authored from the PROTOCOL.md §3/§11.1 offset
// tables. These lock the wire format independent of the codec's own
// encode/decode symmetry: if the codec and these arrays ever disagree, the
// codec is wrong, not the arrays.
#include <cstring>
#include <variant>
#include <vector>

#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

template <typename T>
const T* expect(const Decoded& d) {
    ++wbtest::checks;
    const T* p = std::get_if<T>(&d);
    if (p == nullptr) {
        std::fprintf(stderr, "decode did not yield the expected type\n");
        ++wbtest::failures;
    }
    return p;
}

void check_bytes(const uint8_t* got, const uint8_t* want, size_t n) {
    ++wbtest::checks;
    if (std::memcmp(got, want, n) != 0) {
        ++wbtest::failures;
        for (size_t i = 0; i < n; ++i) {
            if (got[i] != want[i]) {
                std::fprintf(stderr,
                             "byte mismatch at offset %zu: got %02X want %02X\n",
                             i, got[i], want[i]);
            }
        }
    }
}

}  // namespace

int main() {
    uint8_t buf[2048];

    // ---- CACHE_ASSIGN (§3.13): fixed 23 B -------------------------------
    {
        CacheAssign a;
        a.prefix = {0x0009, 0x0021, 0xAABBCCDD};
        a.target_cache = 0x0021;
        a.target_originator = 0x0012;
        a.assignment_epoch = 7;
        a.target_chan = 5805;
        a.target_bw = 0;
        a.target_net_id = 3;
        const uint8_t want[] = {
            0x57, 0x42, 0x0C,        // magic, version, CACHE_ASSIGN
            0x00, 0x09,              // owning receiver
            0x00, 0x21,              // destination cache
            0xAA, 0xBB, 0xCC, 0xDD,  // receiver session
            0x00, 0x21,              // target_cache
            0x00, 0x12,              // target vehicle originator
            0x00, 0x00, 0x00, 0x07,  // assignment_epoch
            0x16, 0xAD,              // target channel 5805
            0x00,                    // target_bw 20 MHz
            0x03,                    // target net_id
        };
        CHECK_EQ_U(sizeof(want), kCacheAssignSize);
        CHECK_EQ_U(encode_cache_assign(a, buf, sizeof(buf)), sizeof(want));
        check_bytes(buf, want, sizeof(want));
        const Decoded d = decode(want, sizeof(want));
        if (const CacheAssign* v = expect<CacheAssign>(d)) CHECK(*v == a);
    }

    // ---- VEHICLE_CMD (§3.14): fixed 23 B --------------------------------
    {
        VehicleCmd c;
        c.prefix = {0x0009, 0x0011, 0xAABBCCDD};
        c.cmd_nonce = 0x01020304;
        c.cmd_seq = 3;
        c.cmd_flags = 0;
        c.cmd_id = vcmd_id::kArq;
        c.cmd_arg = 1;
        c.cmd_mac = 0xDEADBEEF;
        const uint8_t want[] = {
            0x57, 0x42, 0x0D,        // magic, version, VEHICLE_CMD
            0x00, 0x09,              // issuing ground
            0x00, 0x11,              // destination craft
            0xAA, 0xBB, 0xCC, 0xDD,  // issuer session
            0x01, 0x02, 0x03, 0x04,  // cmd_nonce
            0x03,                    // cmd_seq (copy 3)
            0x00,                    // cmd_flags (command, not echo)
            0x01,                    // cmd_id ARQ
            0x01,                    // cmd_arg on
            0xDE, 0xAD, 0xBE, 0xEF,  // cmd_mac
        };
        CHECK_EQ_U(sizeof(want), kVehicleCmdSize);
        CHECK_EQ_U(encode_vehicle_cmd(c, buf, sizeof(buf)), sizeof(want));
        check_bytes(buf, want, sizeof(want));
        const Decoded d = decode(want, sizeof(want));
        if (const VehicleCmd* v = expect<VehicleCmd>(d)) CHECK(*v == c);
    }

    // ---- DATA (§3.2): 26 B header + 4 B payload ---------------------------
    {
        DataHeader h;
        h.prefix = {0x0011, 0x0000, 0x01020304};
        h.stream_id = 0x07;
        h.stream_type = stream_type::kRtp;
        h.seq = 5;
        h.block_id = 2;
        h.data_flags = data_flags::kEndOfBlock | data_flags::kArq |
                       data_flags::kCsaArmed;  // 0x13
        h.active_profile = 4;
        h.table_version = 0xB2;
        const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

        const uint8_t want[] = {
            0x57, 0x42,              // magic "WB"
            0x01,                    // ver 0 | type DATA
            0x00, 0x11,              // originator 17
            0x00, 0x00,              // destination broadcast
            0x01, 0x02, 0x03, 0x04,  // session_id
            0x07,                    // stream_id
            0x01,                    // stream_type RTP
            0x00, 0x00, 0x00, 0x05,  // seq
            0x00, 0x00, 0x00, 0x02,  // block_id
            0x13,                    // EOB|ARQ|CSA_ARMED
            0x04,                    // active_profile
            0xB2,                    // table_version
            0x00, 0x04,              // payload_len
            0xDE, 0xAD, 0xBE, 0xEF,  // payload
        };
        CHECK_EQ_U(sizeof(want), 30);
        const size_t n = encode_data(h, payload, 4, buf, sizeof(buf));
        CHECK_EQ_U(n, sizeof(want));
        check_bytes(buf, want, sizeof(want));

        const Decoded d = decode(want, sizeof(want));
        if (const DataView* v = expect<DataView>(d)) {
            CHECK(v->hdr == h);
            CHECK_EQ_U(v->payload_len, 4);
            check_bytes(v->payload, payload, 4);
        }
    }

    // ---- NACK (§3.3): 23 B fixed + 2 B bitmap -----------------------------
    {
        NackHeader h;
        h.prefix = {0x0009, 0x0011, 0xAABBCCDD};
        h.target_originator = 0x0011;
        h.target_session = 0x01020304;
        h.target_stream_id = 0x00;
        h.base_seq = 1000;
        const uint8_t bitmap[] = {0x80, 0x01};

        const uint8_t want[] = {
            0x57, 0x42,              // magic
            0x02,                    // ver 0 | type NACK
            0x00, 0x09,              // sender = asking RX node
            0x00, 0x11,              // destination
            0xAA, 0xBB, 0xCC, 0xDD,  // sender session
            0x00, 0x11,              // target_originator
            0x01, 0x02, 0x03, 0x04,  // target_session
            0x00,                    // target_stream_id
            0x00, 0x00, 0x03, 0xE8,  // base_seq 1000
            0x02,                    // bitmap_len
            0x80, 0x01,              // bitmap
        };
        CHECK_EQ_U(sizeof(want), 25);
        const size_t n = encode_nack(h, bitmap, 2, buf, sizeof(buf));
        CHECK_EQ_U(n, sizeof(want));
        check_bytes(buf, want, sizeof(want));

        const Decoded d = decode(want, sizeof(want));
        if (const NackView* v = expect<NackView>(d)) {
            CHECK(v->hdr == h);
            CHECK_EQ_U(v->bitmap_len, 2);
            check_bytes(v->bitmap, bitmap, 2);
        }
    }

    // ---- LINK_REPORT (§3.5): fixed 39 B -----------------------------------
    {
        LinkReport r;
        r.prefix = {0x0009, 0x0000, 0xAABBCCDD};
        r.target_originator = 0x0011;
        r.target_session = 0x01020304;
        r.target_stream_id = kNodeScopeStreamId;
        r.report_epoch = 1822;
        r.table_version = 0xB2;
        r.rssi_best = -58;
        r.rssi_mean = -63;
        r.loss_postdiv_prearq = 6;
        r.uniq = 90000;
        r.diversity = 812;
        r.adapters = 3;
        r.probe_per = kNoProbe;
        r.recommended_prof = 4;

        const uint8_t want[] = {
            0x57, 0x42,              // magic
            0x03,                    // ver 0 | type LINK_REPORT
            0x00, 0x09,              // sender
            0x00, 0x00,              // destination
            0xAA, 0xBB, 0xCC, 0xDD,  // sender session
            0x00, 0x11,              // target_originator
            0x01, 0x02, 0x03, 0x04,  // target_session
            0xFF,                    // target_stream_id node-scope
            0x00, 0x00, 0x07, 0x1E,  // report_epoch 1822
            0xB2,                    // table_version
            0xC6,                    // rssi_best -58
            0xC1,                    // rssi_mean -63
            0x00, 0x06,              // loss_postdiv_prearq 6
            0x00, 0x01, 0x5F, 0x90,  // uniq 90000
            0x00, 0x00, 0x03, 0x2C,  // diversity 812
            0x03,                    // adapters
            0xFF, 0xFF,              // probe_per = no probe
            0x04,                    // recommended_prof
        };
        CHECK_EQ_U(sizeof(want), kLinkReportSize);
        const size_t n = encode_link_report(r, buf, sizeof(buf));
        CHECK_EQ_U(n, sizeof(want));
        check_bytes(buf, want, sizeof(want));

        const Decoded d = decode(want, sizeof(want));
        if (const LinkReport* v = expect<LinkReport>(d)) {
            CHECK(*v == r);
        }
    }

    // ---- HEARTBEAT (§3.8): prefix only, exactly 11 B ----------------------
    {
        Heartbeat hb;
        hb.prefix = {0x0011, 0x0000, 0x01020304};

        const uint8_t want[] = {
            0x57, 0x42,              // magic
            0x04,                    // ver 0 | type HEARTBEAT
            0x00, 0x11,              // originator
            0x00, 0x00,              // destination broadcast
            0x01, 0x02, 0x03, 0x04,  // session_id
        };
        CHECK_EQ_U(sizeof(want), kHeartbeatSize);
        const size_t n = encode_heartbeat(hb, buf, sizeof(buf));
        CHECK_EQ_U(n, sizeof(want));
        check_bytes(buf, want, sizeof(want));

        const Decoded d = decode(want, sizeof(want));
        if (const Heartbeat* v = expect<Heartbeat>(d)) {
            CHECK(*v == hb);
        }
    }

    // ---- CSA (§11.1): fixed 32 B ------------------------------------------
    {
        CsaPacket c;
        c.prefix = {0x0009, 0x0000, 0xAABBCCDD};
        c.csa_nonce = 7;
        c.csa_seq = 5;
        c.target_chan = 5805;  // 0x16AD
        c.target_bw = 0;
        c.retune_class = 0;
        c.dt_to_switch_ms = 150;
        c.t_revert_ms = 1000;
        c.prev_chan = 5745;  // 0x1671
        c.prev_bw = 0;
        c.power_intent = 2;
        c.csa_mac = 0xDEADBEEF;

        const uint8_t want[] = {
            0x57, 0x42,              // magic
            0x05,                    // ver 0 | type CSA
            0x00, 0x09,              // sender = issuing ground node
            0x00, 0x00,              // destination
            0xAA, 0xBB, 0xCC, 0xDD,  // sender session
            0x00, 0x00, 0x00, 0x07,  // csa_nonce
            0x05,                    // csa_seq
            0x16, 0xAD,              // target_chan 5805
            0x00,                    // target_bw 20 MHz
            0x00,                    // retune_class fast intra-band
            0x00, 0x96,              // dt_to_switch_ms 150
            0x03, 0xE8,              // t_revert_ms 1000
            0x16, 0x71,              // prev_chan 5745
            0x00,                    // prev_bw
            0x02,                    // power_intent
            0xDE, 0xAD, 0xBE, 0xEF,  // csa_mac (opaque here)
        };
        CHECK_EQ_U(sizeof(want), kCsaSize);
        const size_t n = encode_csa(c, buf, sizeof(buf));
        CHECK_EQ_U(n, sizeof(want));
        check_bytes(buf, want, sizeof(want));

        const Decoded d = decode(want, sizeof(want));
        if (const CsaPacket* v = expect<CsaPacket>(d)) {
            CHECK(*v == c);
        }
    }

    // ---- RECOVERY_REQUEST (§3.9): fixed 18 B ------------------------------
    {
        RecoveryRequest r;
        r.prefix = {0x0009, 0x0011, 0xAABBCCDD};
        r.target_originator = 0x0011;
        r.target_session = 0x01020304;
        r.target_stream_id = 0x07;
        const uint8_t want[] = {
            0x57, 0x42, 0x06,        // magic, version, type
            0x00, 0x09,              // requester
            0x00, 0x11,              // destination
            0xAA, 0xBB, 0xCC, 0xDD,  // requester session
            0x00, 0x11,              // target originator
            0x01, 0x02, 0x03, 0x04,  // target session
            0x07,                    // target stream
        };
        CHECK_EQ_U(sizeof(want), kRecoveryRequestSize);
        CHECK_EQ_U(encode_recovery_request(r, buf, sizeof(buf)), sizeof(want));
        check_bytes(buf, want, sizeof(want));
        const Decoded d = decode(want, sizeof(want));
        if (const RecoveryRequest* v = expect<RecoveryRequest>(d)) {
            CHECK(*v == r);
        }
        CHECK(std::holds_alternative<DecodeError>(
            decode(want, sizeof(want) - 1)));
    }

    // ---- JSCC_FEEDBACK (§3.10): fixed 37 B -------------------------------
    {
        JsccFeedback f;
        f.prefix = {0x0009, 0x0011, 0xAABBCCDD};
        f.target_originator = 0x0011;
        f.target_session = 0x01020304;
        f.target_stream_id = 0x07;
        f.feedback_epoch = 9;
        f.repair_demand_permille = 125;
        f.rtt_p95_us = 2000;
        f.repair_samples = 30;
        f.rtt_samples = 24;
        f.valid_flags = jscc_feedback_flags::kKnownMask;
        f.observed_block_id = 4400;
        const uint8_t want[] = {
            0x57, 0x42, 0x07,        // magic, version, type
            0x00, 0x09,              // reporting RX
            0x00, 0x11,              // destination TX
            0xAA, 0xBB, 0xCC, 0xDD,  // reporter session
            0x00, 0x11,              // target originator
            0x01, 0x02, 0x03, 0x04,  // target session
            0x07,                    // target stream
            0x00, 0x00, 0x00, 0x09,  // feedback epoch
            0x00, 0x7D,              // repair demand 125 permille
            0x00, 0x00, 0x07, 0xD0,  // RTT P95 2000 us
            0x00, 0x1E,              // 30 repair samples
            0x00, 0x18,              // 24 RTT samples
            0x03,                    // repair + RTT ready
            0x00, 0x00, 0x11, 0x30,  // observed block 4400
        };
        CHECK_EQ_U(sizeof(want), kJsccFeedbackSize);
        CHECK_EQ_U(encode_jscc_feedback(f, buf, sizeof(buf)), sizeof(want));
        check_bytes(buf, want, sizeof(want));
        const Decoded d = decode(want, sizeof(want));
        if (const JsccFeedback* v = expect<JsccFeedback>(d)) {
            CHECK(*v == f);
        }
        CHECK(std::holds_alternative<DecodeError>(
            decode(want, sizeof(want) - 1)));
        uint8_t bad_flags[kJsccFeedbackSize];
        std::memcpy(bad_flags, want, sizeof(want));
        bad_flags[32] = 0x80;
        CHECK(std::get<DecodeError>(decode(bad_flags, sizeof(bad_flags))) ==
              DecodeError::kInvalidField);
    }

    // ---- error paths -------------------------------------------------------
    {
        // Wrong magic.
        const uint8_t bad_magic[] = {0x57, 0x43, 0x04, 0, 0, 0, 0, 0, 0, 0, 0};
        const Decoded d1 = decode(bad_magic, sizeof(bad_magic));
        CHECK(std::get_if<DecodeError>(&d1) != nullptr &&
              std::get<DecodeError>(d1) == DecodeError::kBadMagic);

        // Bad version nibble.
        const uint8_t bad_ver[] = {0x57, 0x42, 0x14, 0, 0, 0, 0, 0, 0, 0, 0};
        const Decoded d2 = decode(bad_ver, sizeof(bad_ver));
        CHECK(std::get_if<DecodeError>(&d2) != nullptr &&
              std::get<DecodeError>(d2) == DecodeError::kBadVersion);

        // Unknown type nibble.
        const uint8_t bad_type[] = {0x57, 0x42, 0x0F, 0, 0, 0, 0, 0, 0, 0, 0};
        const Decoded d3 = decode(bad_type, sizeof(bad_type));
        CHECK(std::get_if<DecodeError>(&d3) != nullptr &&
              std::get<DecodeError>(d3) == DecodeError::kUnknownType);

        // HEARTBEAT with a trailing byte: exactly 11 or error (§3.8).
        const uint8_t hb12[] = {0x57, 0x42, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const Decoded d4 = decode(hb12, sizeof(hb12));
        CHECK(std::get_if<DecodeError>(&d4) != nullptr &&
              std::get<DecodeError>(d4) == DecodeError::kLengthMismatch);

        // Encode into an undersized buffer must fail, not truncate.
        Heartbeat hb;
        CHECK_EQ_U(encode_heartbeat(hb, buf, kHeartbeatSize - 1), 0);
    }

    return wbtest_finish("wire_vectors_test");
}
