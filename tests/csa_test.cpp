// SPDX-License-Identifier: GPL-2.0-or-later
// §11 CSA engine tests: follower validation (MAC / replay / issuer lock /
// allowlist / rate-limit), TSF anchoring incl. u32 wrap, the §11.5 state
// machine (jump-failed revert, hold-until-reboot, §11.5a binding release +
// in-place re-claim), the §11.6 issuer (decrementing-dt copies, Pass 69
// pre-position commit-on-armed + rendezvous beacons, ack-timeout abort,
// revert-on-no-video at the T_switch-anchored deadline), and the §11.3
// selector freeze pause.
#include "wblink/csa.h"

#include <string_view>

#include "wblink/hmac_sha256.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

CsaParams policy_with_psk() {
    CsaParams p;
    p.psk = {'s', 'e', 'c', 'r', 'e', 't'};
    p.allowlist = {5745, 5805, 5825};
    return p;
}

CsaPacket make_csa(const CsaParams& pol, uint32_t nonce, uint16_t chan,
                   uint16_t dt_ms) {
    CsaPacket c;
    c.prefix = {9, 0, 1234};  // ground originator 9
    c.csa_nonce = nonce;
    c.csa_seq = 5;
    c.target_chan = chan;
    c.target_bw = 0;
    c.retune_class = 0;
    c.dt_to_switch_ms = dt_ms;
    c.t_revert_ms = 150;
    c.prev_chan = 5805;
    c.prev_bw = 0;
    c.power_intent = 4;
    uint8_t buf[32];
    CHECK_EQ_U(encode_csa(c, buf, sizeof(buf)), 32);
    c.csa_mac = csa_mac(pol.psk.data(), pol.psk.size(), buf);
    return c;
}

}  // namespace

int main() {
    const CsaParams pol = policy_with_psk();

    // --- follower validation ------------------------------------------------
    {
        CsaFollower f(pol);
        CsaPacket c = make_csa(pol, 1, 5745, 150);
        // Bad MAC rejected.
        CsaPacket bad = c;
        bad.csa_mac ^= 1;
        CHECK(!f.on_csa(bad, 1000, std::nullopt, 0, std::nullopt));
        // Channel outside the allowlist rejected (even MAC-valid).
        CsaPacket off = make_csa(pol, 1, 5900, 150);
        CHECK(!f.on_csa(off, 1000, std::nullopt, 0, std::nullopt));
        // Valid accepted; establishes the issuer latch (§11.4 bootstrap).
        CHECK(f.on_csa(c, 1000, std::nullopt, 0, std::nullopt));
        CHECK(f.armed());
        CHECK(f.latched_issuer().has_value());
        CHECK_EQ_U(*f.latched_issuer(), 9);
        // Second copy of the SAME campaign (same nonce): dropped, still armed.
        CHECK(!f.on_csa(c, 21000, std::nullopt, 0, std::nullopt));
        // Replay of an older nonce after a fresh campaign is dead too.
        CHECK(!f.on_csa(c, 90'000'000, std::nullopt, 0, std::nullopt));
    }
    // Issuer lock: latched command source wins over a MAC-valid stranger.
    {
        CsaFollower f(pol);
        CsaPacket c = make_csa(pol, 1, 5745, 150);
        CHECK(!f.on_csa(c, 1000, std::nullopt, 0, /*latched=*/uint16_t{7}));
        CHECK(f.on_csa(c, 1000, std::nullopt, 0, uint16_t{9}));
    }
    // Rate-limit: a second campaign inside min_interval is rejected.
    {
        CsaFollower f(pol);
        CHECK(f.on_csa(make_csa(pol, 1, 5745, 150), 1000, std::nullopt, 0,
                       std::nullopt));
        CHECK(!f.on_csa(make_csa(pol, 2, 5825, 150), 2'000'000, std::nullopt,
                        0, std::nullopt));
        CHECK(f.on_csa(make_csa(pol, 2, 5825, 150), 6'000'000, std::nullopt,
                       0, std::nullopt));
    }
    // Spectator (empty PSK) follows unauthenticated (§11.4).
    {
        CsaParams spec = pol;
        spec.psk.clear();
        CsaFollower f(spec);
        CsaPacket c = make_csa(pol, 1, 5745, 150);
        c.csa_mac = 0;  // no MAC at all
        CHECK(f.on_csa(c, 1000, std::nullopt, 0, std::nullopt));
    }

    // --- §11.2 TSF anchor ---------------------------------------------------
    {
        // Copy received 40 ms ago per TSF ⇒ switch fires 110 ms out, not 150.
        CsaFollower f(pol);
        CHECK(f.on_csa(make_csa(pol, 1, 5745, 150), 1'000'000,
                       uint64_t{500'000}, /*rx_tsfl=*/460'000, std::nullopt));
        CHECK_EQ_U(f.tick(1'000'000 + 109'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        const auto a = f.tick(1'000'000 + 110'000);
        CHECK_EQ_U(a.kind, static_cast<unsigned>(CsaAction::Kind::kRetune));
        CHECK_EQ_U(a.chan_mhz, 5745);
        CHECK(a.fast);
    }
    {
        // u32 TSF wrap: tsf_now wrapped past 0, rx_tsfl just below the wrap.
        CsaFollower f(pol);
        const uint32_t rx_tsfl = 0xFFFFFF00u;
        const uint64_t tsf_now = 0x1'0000'0120ull;  // elapsed = 0x220 = 544 µs
        CHECK(f.on_csa(make_csa(pol, 1, 5745, 150), 1'000'000, tsf_now,
                       rx_tsfl, std::nullopt));
        const uint64_t at = 1'000'000 + 150'000 - 544;
        CHECK_EQ_U(f.tick(at - 1).kind,
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        CHECK_EQ_U(f.tick(at).kind,
                   static_cast<unsigned>(CsaAction::Kind::kRetune));
    }

    // --- §11.5 follower walk ------------------------------------------------
    {
        // Happy path: retune → traffic → COMMITTED; freeze covers §11.3.
        CsaFollower f(pol);
        CHECK(f.on_csa(make_csa(pol, 1, 5745, 150), 0, std::nullopt, 0,
                       std::nullopt));
        CHECK_EQ_U(f.freeze_until_us(), 3'000'000);  // settle_ms default
        CHECK_EQ_U(f.tick(150'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kRetune));
        f.note_valid_rx(200'000, 9);  // ground (bound issuer) heard
        CHECK(!f.armed());
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
        // §11.5 hold-until-reboot: COMMITTED never auto-reverts, however long.
        CHECK_EQ_U(f.tick(200'000 + 3'600'000'000ull).kind,
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
    }
    {
        // §11.5 jump-failed backout: no traffic ⇒ revert to prev_chan and
        // return to IDLE (no mid-flight rendezvous); the claim is dropped.
        CsaFollower f(pol);
        CHECK(f.on_csa(make_csa(pol, 1, 5825, 150), 0, std::nullopt, 0,
                       std::nullopt));
        CHECK_EQ_U(f.tick(150'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kRetune));
        // Pass 69 H1b: the window opens at LANDING (first post-retune tick),
        // not at the tick that ordered the retune. Landing at 180 ms →
        // revert at 180 + t_revert(150) ms, not 300 ms.
        CHECK_EQ_U(f.tick(180'000).kind,  // landing: window opens
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        CHECK_EQ_U(f.tick(329'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        const auto rv = f.tick(330'000);  // wire t_revert_ms = 150 from landing
        CHECK_EQ_U(rv.kind, static_cast<unsigned>(CsaAction::Kind::kRevert));
        CHECK_EQ_U(rv.chan_mhz, 5805);  // prev_chan
        CHECK(std::string_view(f.state_str()) == "IDLE");
        CHECK(!f.latched_issuer().has_value());
    }
    {
        // §11.5a binding release: after bind_release_ms (default 90 s) of
        // silence from the bound issuer the binding drops with NO channel
        // change (stays COMMITTED), re-opening the craft for in-place re-claim.
        CsaFollower f(pol);
        CHECK(f.on_csa(make_csa(pol, 1, 5745, 150), 0, std::nullopt, 0,
                       std::nullopt));
        CHECK_EQ_U(f.tick(150'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kRetune));
        f.note_valid_rx(200'000, 9);
        CHECK(f.latched_issuer().has_value());
        const uint64_t rel = 200'000 + 90'000'000ull;  // last_bound + 90 s
        CHECK_EQ_U(f.tick(rel).kind,  // exactly at the boundary: still bound
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        CHECK(f.latched_issuer().has_value());
        CHECK_EQ_U(f.tick(rel + 2).kind,  // past it: released, no channel change
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        CHECK(!f.latched_issuer().has_value());
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
        // A fresh issuer (different originator) can now re-claim in place.
        CsaPacket c2;
        c2.prefix = {11, 0, 999};
        c2.csa_nonce = 1;
        c2.csa_seq = 5;
        c2.target_chan = 5825;
        c2.target_bw = 0;
        c2.retune_class = 0;
        c2.dt_to_switch_ms = 150;
        c2.t_revert_ms = 150;
        c2.prev_chan = 5745;
        c2.prev_bw = 0;
        c2.power_intent = 4;
        uint8_t b2[32];
        CHECK_EQ_U(encode_csa(c2, b2, sizeof(b2)), 32);
        c2.csa_mac = csa_mac(pol.psk.data(), pol.psk.size(), b2);
        CHECK(f.on_csa(c2, rel + 3, std::nullopt, 0, std::nullopt));
        CHECK_EQ_U(*f.latched_issuer(), 11);
    }

    // --- §11.6 issuer -------------------------------------------------------
    {
        CsaIssuer is(pol);
        const CommonPrefix pre{9, 0, 1234};
        CHECK(!is.start(pre, 5900, 0, 0, 5805, 0, 4, 0));  // allowlist
        CHECK(is.start(pre, 5745, 0, 0, 5805, 0, 4, 0));
        CHECK(!is.start(pre, 5745, 0, 0, 5805, 0, 4, 0));  // already active
        // 5 copies, csa_seq 5..1, dt decrementing toward one T_switch.
        uint16_t last_dt = 200;
        for (int i = 0; i < 5; ++i) {
            const auto a = is.tick(static_cast<uint64_t>(i) * 20'000);
            CHECK_EQ_U(a.kind,
                       static_cast<unsigned>(
                           CsaIssuer::IssuerAction::Kind::kSendCopy));
            CHECK_EQ_U(a.pkt.csa_seq, 5 - i);
            CHECK(a.pkt.dt_to_switch_ms <= last_dt);
            CHECK(a.pkt.dt_to_switch_ms >= 1);
            last_dt = a.pkt.dt_to_switch_ms;
            // Every copy is MAC-valid under the PSK.
            uint8_t buf[32];
            CHECK_EQ_U(encode_csa(a.pkt, buf, sizeof(buf)), 32);
            CHECK_EQ_U(csa_mac(pol.psk.data(), pol.psk.size(), buf),
                       a.pkt.csa_mac);
        }
        // Craft armed → commit IMMEDIATELY (§11.6 pre-position, Pass 69) —
        // not at T_switch.
        is.note_craft_armed(100'000);
        const auto c = is.tick(101'000);
        CHECK_EQ_U(c.kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        CHECK_EQ_U(c.chan_mhz, 5745);
        // Landed → rendezvous beacons at copy spacing: csa_seq 0, dt 0,
        // MAC-valid under the campaign key (§11.6, Pass 69).
        const auto b0 = is.tick(101'500);
        CHECK_EQ_U(b0.kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        CHECK_EQ_U(b0.pkt.csa_seq, 0);
        CHECK_EQ_U(b0.pkt.dt_to_switch_ms, 0);
        CHECK_EQ_U(b0.pkt.csa_nonce, 1);
        uint8_t bbuf[32];
        CHECK_EQ_U(encode_csa(b0.pkt, bbuf, sizeof(bbuf)), 32);
        CHECK_EQ_U(csa_mac(pol.psk.data(), pol.psk.size(), bbuf),
                   b0.pkt.csa_mac);
        // Spaced, not spammed: nothing for the next copy interval.
        CHECK_EQ_U(is.tick(111'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kNone));
        CHECK_EQ_U(is.tick(121'500).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        // Craft video arrives → success LATCHED, but the beacon tail keeps
        // blanketing the craft's verify window (§11.6 beacon tail).
        is.note_craft_video(200'000);
        CHECK(is.active());
        CHECK_EQ_U(is.tick(221'500).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        // Campaign closes at the deadline (T_switch 150 ms + verify 150 ms)
        // with kSuccess, then goes quiet.
        const auto s = is.tick(300'000);
        CHECK_EQ_U(s.kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSuccess));
        CHECK(!is.active());
        CHECK_EQ_U(is.tick(300'500).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kNone));
    }
    {
        // No CSA_ARMED ⇒ abort at ack_timeout, never commits.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) {
            is.tick(static_cast<uint64_t>(i) * 20'000);
        }
        CHECK_EQ_U(is.tick(999'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kNone));
        const auto a = is.tick(1'000'000);
        CHECK_EQ_U(a.kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kAbort));
        CHECK(!is.active());
    }
    {
        // Committed but no craft video ⇒ revert to prev (§11.6 backstop).
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) {
            is.tick(static_cast<uint64_t>(i) * 20'000);
        }
        is.note_craft_armed(100'000);
        CHECK_EQ_U(is.tick(100'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        // Landing (first kVerify tick) right after the commit: the §11.6
        // deadline anchors at max(T_switch, landing) + verify_timeout =
        // 150 + 150 ms — the craft does not move before T_switch.
        CHECK_EQ_U(is.tick(100'500).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        CHECK_EQ_U(is.tick(150'000 + 149'000).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        const auto a = is.tick(150'000 + 150'000);
        CHECK_EQ_U(a.kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kRevert));
        CHECK_EQ_U(a.chan_mhz, 5805);
        CHECK(!is.active());
        // Nonce advanced: the next campaign is strictly greater.
        CHECK(is.start({9, 0, 1234}, 5825, 0, 0, 5805, 0, 4, 10'000'000));
        const auto b = is.tick(10'000'000);
        CHECK_EQ_U(b.pkt.csa_nonce, 2);
    }
    {
        // §15.5a claim re-key: an announced-mode issuer (no configured secret)
        // cannot issue until keyed with a craft's token; re-keying is idle-only
        // and the monotonic nonce carries across keys so one issuer commands
        // different crafts in turn, each copy MAC-valid under the current key.
        CsaParams announced = pol;
        announced.psk.clear();  // announced mode: no configured secret
        CsaIssuer is(announced);
        CHECK(!is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));  // no key
        const std::vector<uint8_t> token_a = {'A', 'A', 'A', 'A'};
        CHECK(is.set_psk(token_a));
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        CHECK(!is.set_psk({'Z'}));  // never swap the key mid-campaign
        const auto a0 = is.tick(0);
        CHECK_EQ_U(a0.pkt.csa_nonce, 1);
        uint8_t buf_a[32];
        CHECK_EQ_U(encode_csa(a0.pkt, buf_a, sizeof(buf_a)), 32);
        CHECK_EQ_U(csa_mac(token_a.data(), token_a.size(), buf_a),
                   a0.pkt.csa_mac);
        // Drain the copies and abort (no CSA_ARMED) back to idle.
        for (int i = 1; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
        CHECK_EQ_U(is.tick(1'000'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kAbort));
        // Re-key to a different craft's token, claim onto another channel: the
        // nonce is strictly greater (2) and copies MAC under the new key.
        const std::vector<uint8_t> token_b = {'B', 'B', 'B', 'B', 'B'};
        CHECK(is.set_psk(token_b));
        CHECK(is.start({9, 0, 1234}, 5825, 0, 0, 5745, 0, 4, 6'000'000));
        const auto b0 = is.tick(6'000'000);
        CHECK_EQ_U(b0.pkt.csa_nonce, 2);
        uint8_t buf_b[32];
        CHECK_EQ_U(encode_csa(b0.pkt, buf_b, sizeof(buf_b)), 32);
        CHECK_EQ_U(csa_mac(token_b.data(), token_b.size(), buf_b),
                   b0.pkt.csa_mac);
    }

    {
        // H1 (review pass 2): a slow blocking retune delays the first
        // kVerify tick past T_switch + verify_timeout. "Landing" is that
        // first tick, so the window opens THERE — no instant revert.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
        is.note_craft_armed(100'000);
        CHECK_EQ_U(is.tick(101'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        // First post-retune tick at 450 ms — already past the old (broken)
        // anchor of 300 ms. Must open the window and beacon, not revert.
        CHECK_EQ_U(is.tick(450'000).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        is.note_craft_video(460'000);
        CHECK_EQ_U(is.tick(450'000 + 150'000).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSuccess));
        CHECK(!is.active());
    }
    {
        // H2 (review pass 2): craft "video" before T_switch is a stale ear
        // or bleed — it must NOT latch success; the campaign reverts.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
        is.note_craft_armed(100'000);
        CHECK_EQ_U(is.tick(101'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        is.tick(101'500);                 // landing: window opens (300 ms)
        is.note_craft_video(120'000);     // BEFORE T_switch (150 ms): ignored
        const auto a = is.tick(300'000);
        CHECK_EQ_U(a.kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kRevert));
        CHECK(!is.active());
    }
    {
        // Review pass 2: a failed commit retune abandons the campaign — the
        // issuer must not verify with untrusted ears.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
        is.note_craft_armed(100'000);
        CHECK_EQ_U(is.tick(101'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        is.note_commit_failed();
        CHECK(!is.active());
        CHECK_EQ_U(is.tick(102'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kNone));
    }

    // --- §11.6 rendezvous beacon at the follower (Pass 69) ------------------
    {
        // A beacon = the campaign packet with csa_seq 0 and dt 0, re-MAC'd.
        const auto make_beacon = [&](uint32_t nonce, uint16_t chan) {
            CsaPacket b = make_csa(pol, nonce, chan, 1);
            b.csa_seq = 0;
            b.dt_to_switch_ms = 0;
            uint8_t buf[32];
            CHECK_EQ_U(encode_csa(b, buf, sizeof(buf)), 32);
            b.csa_mac = csa_mac(pol.psk.data(), pol.psk.size(), buf);
            return b;
        };
        // In VERIFY, a matching beacon confirms → COMMITTED.
        CsaFollower f(pol);
        CHECK(f.on_csa(make_csa(pol, 1, 5745, 150), 0, std::nullopt, 0,
                       std::nullopt));
        CHECK_EQ_U(f.tick(150'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kRetune));
        // Wrong nonce: stays VERIFY.
        CHECK(!f.on_csa(make_beacon(7, 5745), 160'000, std::nullopt, 0,
                        std::nullopt));
        CHECK(std::string_view(f.state_str()) == "VERIFY");
        // Bad MAC: stays VERIFY.
        CsaPacket forged = make_beacon(1, 5745);
        forged.csa_mac ^= 1;
        CHECK(!f.on_csa(forged, 165'000, std::nullopt, 0, std::nullopt));
        CHECK(std::string_view(f.state_str()) == "VERIFY");
        // Matching beacon: confirms (on_csa still returns false — it is not
        // a campaign acceptance and must not log/arm).
        CHECK(!f.on_csa(make_beacon(1, 5745), 170'000, std::nullopt, 0,
                        std::nullopt));
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
        // No revert at the old verify deadline — the switch is confirmed.
        CHECK_EQ_U(f.tick(300'001).kind,
                   static_cast<unsigned>(CsaAction::Kind::kNone));
    }
    {
        // A beacon never arms from IDLE (dt = 0 fails §11.4 accept) and does
        // not latch.
        CsaFollower f(pol);
        CHECK(!f.on_csa(make_csa(pol, 1, 5745, 0), 1000, std::nullopt, 0,
                        std::nullopt));
        CHECK(std::string_view(f.state_str()) == "IDLE");
        CHECK(!f.latched_issuer().has_value());
    }
    {
        // In ARMED a beacon is a no-op: the follower still switches at
        // T_switch, and in COMMITTED a beacon does NOT refresh the §11.5a
        // binding (a recorded beacon must not hold the binding alive).
        const auto make_beacon = [&](uint32_t nonce, uint16_t chan) {
            CsaPacket b = make_csa(pol, nonce, chan, 1);
            b.csa_seq = 0;
            b.dt_to_switch_ms = 0;
            uint8_t buf[32];
            CHECK_EQ_U(encode_csa(b, buf, sizeof(buf)), 32);
            b.csa_mac = csa_mac(pol.psk.data(), pol.psk.size(), buf);
            return b;
        };
        CsaFollower f(pol);
        CHECK(f.on_csa(make_csa(pol, 1, 5745, 150), 0, std::nullopt, 0,
                       std::nullopt));
        CHECK(!f.on_csa(make_beacon(1, 5745), 1000, std::nullopt, 0,
                        std::nullopt));
        CHECK(f.armed());  // still ARMED, still switches on time
        CHECK_EQ_U(f.tick(150'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kRetune));
        f.note_valid_rx(200'000, 9);  // committed; binding fresh at 200 ms
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
        // Beacon mid-hold: must NOT refresh the binding...
        CHECK(!f.on_csa(make_beacon(1, 5745), 60'000'000, std::nullopt, 0,
                        std::nullopt));
        // ...so release still fires at 200 ms + bind_release_ms.
        const uint64_t rel = 200'000 + 90'000'000ull;
        f.tick(rel + 2);
        CHECK(!f.latched_issuer().has_value());
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
    }

    {
        // A beacon must not touch the §11.4 rate-limit anchor: a follower
        // that confirmed via beacon still accepts the next campaign as soon
        // as min_interval has elapsed from the ACCEPT, not from the beacon.
        const auto make_beacon = [&](uint32_t nonce, uint16_t chan) {
            CsaPacket b = make_csa(pol, nonce, chan, 1);
            b.csa_seq = 0;
            b.dt_to_switch_ms = 0;
            uint8_t buf[32];
            CHECK_EQ_U(encode_csa(b, buf, sizeof(buf)), 32);
            b.csa_mac = csa_mac(pol.psk.data(), pol.psk.size(), buf);
            return b;
        };
        CsaFollower f(pol);
        CHECK(f.on_csa(make_csa(pol, 1, 5745, 150), 1000, std::nullopt, 0,
                       std::nullopt));
        f.tick(151'000);  // T_switch → VERIFY
        CHECK(!f.on_csa(make_beacon(1, 5745), 4'900'000, std::nullopt, 0,
                        std::nullopt));
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
        // 5.2 s after the accept (but only 0.3 s after the beacon): accepted.
        CHECK(f.on_csa(make_csa(pol, 2, 5825, 150), 5'200'000, std::nullopt,
                       0, std::nullopt));
    }
    {
        // Spectator (empty PSK, §11.4): beacon confirm follows the same
        // unauthenticated-follow posture — self-harm only.
        CsaParams spol;
        spol.allowlist = {5745, 5805, 5825};
        CsaFollower f(spol);
        CsaPacket c = make_csa(pol, 1, 5745, 150);  // MAC irrelevant
        CHECK(f.on_csa(c, 0, std::nullopt, 0, std::nullopt));
        f.tick(150'000);  // VERIFY
        CsaPacket b = c;
        b.csa_seq = 0;
        b.dt_to_switch_ms = 0;
        CHECK(!f.on_csa(b, 160'000, std::nullopt, 0, std::nullopt));
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
    }

    return wbtest_finish("csa_test");
}
