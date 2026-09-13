// SPDX-License-Identifier: GPL-2.0-or-later
// §11 CSA engine tests: follower validation (MAC / replay / issuer lock /
// allowlist / rate-limit), TSF anchoring incl. u32 wrap, the §11.5 state
// machine (jump-failed revert, hold-until-reboot, §11.5a binding release +
// in-place re-claim), the §11.6 issuer (decrementing-dt copies, Pass 69
// pre-position commit-on-armed + rendezvous beacons, ack-timeout abort,
// revert-on-no-video at the T_switch-anchored deadline), and the §11.3
// selector freeze pause.
#include <cstdio>
#include "wblink/csa.h"

#include <string_view>

#include "wblink/hmac_sha256.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

// §11.2 (Pass 204): ONE dt for every campaign. These tests used to hardcode
// 300 ms and had to be re-timed by hand when the budget moved — derive the
// instants instead, so the next change to the constant cannot silently leave
// a test asserting against a T_switch the issuer no longer uses.
namespace {
constexpr uint64_t kTs = static_cast<uint64_t>(CsaIssuer::kDtToSwitchMs) * 1000;
}  // namespace

namespace {

CsaParams policy_with_psk() {
    CsaParams p;
    p.psk = {'s', 'e', 'c', 'r', 'e', 't'};
    p.allowlist = {5745, 5805, 5825};
    // Pass 89 raised the default 150 -> 500 ms. The window-arithmetic cases
    // below were written against 150 and their timing is not what they test,
    // so pin it here; the new default is asserted on its own below.
    p.verify_timeout_ms = 150;
    return p;
}

// Re-MAC after mutating a packet's fields (the MAC covers bytes 0..27).
uint32_t mac_for(const CsaParams& pol, const CsaPacket& c) {
    uint8_t buf[32];
    CHECK_EQ_U(encode_csa(c, buf, sizeof(buf)), 32);
    return csa_mac(pol.psk.data(), pol.psk.size(), buf);
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
    // Spectator follows unauthenticated (§11.4a) — but ONLY because the role
    // says so, not because the key is empty (Pass 85).
    {
        CsaParams spec = pol;
        spec.psk.clear();
        spec.allow_unauthenticated = true;  // §15.2 node.spectator
        CsaFollower f(spec);
        CsaPacket c = make_csa(pol, 1, 5745, 150);
        c.csa_mac = 0;  // no MAC at all
        CHECK(f.on_csa(c, 1000, std::nullopt, 0, std::nullopt));
        CHECK_EQ_U(f.unauth_rejected(), 0);
    }
    // §11.4a Pass 85: a craft/ground with an EMPTY key is faulted, not
    // unauthenticated — it must fail closed. Before the fix an empty psk
    // silently accepted any forged CSA inside the allowlist, retuning a craft
    // off-channel mid-flight on a missed ANNOUNCE or a config typo.
    {
        CsaParams craft = pol;
        craft.psk.clear();  // token generation failed / ANNOUNCE not yet heard
        craft.allow_unauthenticated = false;  // craft or ground, not spectator
        CsaFollower f(craft);
        CsaPacket c = make_csa(pol, 1, 5745, 150);
        c.csa_mac = 0;
        CHECK(!f.on_csa(c, 1000, std::nullopt, 0, std::nullopt));
        CHECK_EQ_U(f.unauth_rejected(), 1);
        CHECK(std::string_view(f.state_str()) == "IDLE");
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
    {
        // §11.2 clamp (E): a garbage/reset TSF delta ≥ dt_us is discarded, so
        // the switch still fires at the full nominal dt (150 ms) instead of
        // collapsing switch_at to now and retuning early, ahead of the issuer.
        CsaFollower f(pol);
        const uint64_t tsf_now = 500'000;  // elapsed = 500 ms > dt_us = 150 ms
        CHECK(f.on_csa(make_csa(pol, 1, 5745, 150), 1'000'000, tsf_now,
                       /*rx_tsfl=*/0, std::nullopt));
        CHECK_EQ_U(f.tick(1'000'000 + 149'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        CHECK_EQ_U(f.tick(1'000'000 + 150'000).kind,
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
        // Pass 202 FINAL JUMP: the window still OPENS at landing and still
        // closes on the same schedule — what changed is what closing does. It
        // used to revert to prev_chan and drop the binding; it now commits and
        // stays, because a two-sided verified handshake strands the pair
        // whenever the two ends disagree.
        const auto rv = f.tick(330'000);  // wire t_revert_ms = 150 from landing
        CHECK_EQ_U(rv.kind, static_cast<unsigned>(CsaAction::Kind::kNone));
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
        // The binding is KEPT. Dropping it here would re-open this craft to a
        // different issuer on a channel it just followed THIS one onto.
        CHECK(f.latched_issuer().has_value());
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
        // A caller that samples its loop clock immediately before RX stamps a
        // packet must not turn the tiny timestamp regression into a u64 age.
        CHECK_EQ_U(f.tick(199'999).kind,
                   static_cast<unsigned>(CsaAction::Kind::kNone));
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
        uint16_t last_dt = CsaIssuer::kDtToSwitchMs + 1;  // §11.2 Pass 204
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
        // After T_switch — earlier video is ignored as old-channel residue.
        is.note_craft_video(kTs + 10'000, false);  // Pass 89: committed craft
        CHECK(is.active());
        CHECK_EQ_U(is.tick(kTs + 21'500).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        // Campaign closes at the deadline (T_switch + verify 150 ms) with
        // kSuccess, then goes quiet.
        const auto s = is.tick(kTs + 150'000);
        CHECK_EQ_U(s.kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSuccess));
        CHECK(!is.active());
        CHECK_EQ_U(is.tick(kTs + 150'500).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kNone));
    }
    {
        // No CSA_ARMED ⇒ abort at T_SWITCH (Pass 204), never commits. The
        // separate ack_timeout_ms timer is gone: the instant we agreed to move
        // is the last instant an ACK can mean anything, and at the generous dt
        // a 1 s ack timer aborted campaigns whose copies were still going out.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) {
            is.tick(static_cast<uint64_t>(i) * 20'000);
        }
        // Still alive well past the OLD 1000 ms ack_timeout, and still
        // RETRANSMITTING into that time rather than sitting idle — this is
        // exactly the regression that stranded the bench, so assert it
        // directly rather than only asserting the new abort instant.
        CHECK(is.active());
        CHECK_EQ_U(is.tick(1'000'001).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendCopy));
        CHECK(is.active());
        // Copies stop one ack-lead short of T_switch (Pass 90 cutoff), but the
        // campaign is still alive right up to the instant it agreed on.
        CHECK_EQ_U(is.tick(kTs - 1000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kNone));
        CHECK(is.active());
        const auto a = is.tick(kTs);
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
        // deadline anchors at max(T_switch, landing) + verify_timeout, and the
        // craft does not move before T_switch. Pass 204's generous dt makes
        // that anchoring load-bearing rather than incidental — the issuer
        // pre-positions on CSA_ARMED and then waits out the whole dt, so a
        // deadline anchored on the COMMIT instant would expire long before the
        // craft is due to arrive and report every campaign unconfirmed.
        CHECK_EQ_U(is.tick(100'500).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        CHECK_EQ_U(is.tick(kTs + 149'000).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        // Pass 202 FINAL JUMP: the deadline still fires on the same schedule,
        // but no-video no longer retreats to prev_chan — the issuer succeeds
        // on the TARGET and stays. video_seen_ is still reported so an
        // operator can tell a confirmed switch from an unconfirmed one; we
        // simply no longer act on it.
        const auto a = is.tick(kTs + 150'000);
        CHECK_EQ_U(a.kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kSuccess));
        CHECK_EQ_U(a.chan_mhz, 5745);  // target, not prev
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
        CHECK_EQ_U(is.tick(kTs).kind,   // Pass 204: abort at T_switch
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kAbort));
        // Re-key to a different craft's token, claim onto another channel: the
        // nonce is strictly greater (2) and copies MAC under the new key.
        const std::vector<uint8_t> token_b = {'B', 'B', 'B', 'B', 'B'};
        CHECK(is.set_psk(token_b));
        CHECK(is.start({9, 0, 1234}, 5825, 0, 0, 5745, 0, 4, kTs + 6'000'000));
        const auto b0 = is.tick(kTs + 6'000'000);
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
        // First post-retune tick well past T_switch + verify_timeout — the
        // window must open THERE (at landing), not have already expired.
        CHECK_EQ_U(is.tick(kTs + 150'000).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        is.note_craft_video(kTs + 160'000, false);  // Pass 89: committed craft
        CHECK_EQ_U(is.tick(kTs + 150'000 + 150'000).kind,
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
        is.tick(101'500);                 // landing: window opens
        is.note_craft_video(120'000, false);  // BEFORE T_switch: ignored
        // Deadline = max(T_switch, landing) + verify_timeout.
        // Pass 202: pre-T_switch video is still ignored as commit proof, so
        // the campaign closes unconfirmed — and now SUCCEEDS on the target
        // rather than retreating.
        const auto a = is.tick(kTs + 150'001);
        CHECK_EQ_U(a.kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kSuccess));
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
        spol.allow_unauthenticated = true;  // §15.2 node.spectator (Pass 85)
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

    // §11.5 Pass 86: t_revert_ms may SHORTEN the VERIFY window, never lengthen
    // it. Before the fix a u16 taken verbatim stranded the follower on a dead
    // channel for its full value — up to 65 s.
    {
        CsaParams pol2 = pol;
        pol2.verify_timeout_ms = 150;
        CsaFollower f(pol2);
        CsaPacket c = make_csa(pol, 1, 5745, 150);
        c.t_revert_ms = 65535;  // hostile (or simply wrong) issuer
        c.csa_mac = mac_for(pol, c);
        CHECK(f.on_csa(c, 0, std::nullopt, 0, std::nullopt));
        CHECK_EQ_U(f.tick(150'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kRetune));
        CHECK_EQ_U(f.tick(180'000).kind,  // landing: window opens
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        // Clamped to the LOCAL 150 ms — the point is that a u16 t_revert_ms
        // cannot strand the follower for 65 s. Pass 202 keeps the clamp and
        // changes only the outcome: the window closes into COMMITTED.
        CHECK_EQ_U(f.tick(329'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        CHECK_EQ_U(f.tick(330'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
    }
    // ...and the issuer CAN still shorten it.
    {
        CsaParams pol2 = pol;
        pol2.verify_timeout_ms = 150;
        CsaFollower f(pol2);
        CsaPacket c = make_csa(pol, 1, 5745, 150);
        c.t_revert_ms = 50;
        c.csa_mac = mac_for(pol, c);
        CHECK(f.on_csa(c, 0, std::nullopt, 0, std::nullopt));
        f.tick(150'000);                  // retune
        f.tick(180'000);                  // landing
        // The issuer may still SHORTEN the window (Pass 86); Pass 202 changes
        // what the window closing MEANS, not who may set it.
        CHECK_EQ_U(f.tick(230'000).kind,  // 180 + 50, the issuer's tighter budget
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
    }
    {
        // Pass 89: the §11.5 default is 500 ms, not the old median-derived
        // 150. Measured max issuer-landing delay was 143 ms — the old window
        // had 4.7% margin and stranded the fleet when it was exceeded.
        const CsaParams def;
        CHECK_EQ_U(def.verify_timeout_ms, 500u);
    }
    {
        // Pass 89 regression — the hop-4 fleet split of 2026-07-24.
        // A craft that ARRIVED on target_chan but has not COMMITTED still
        // transmits, with CSA_ARMED set. That must NOT satisfy video-verify:
        // the craft may still revert, and an issuer that confirms on it holds
        // a channel the craft has left.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
        is.note_craft_armed(100'000);
        CHECK_EQ_U(is.tick(101'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        is.tick(200'000);  // landing; deadline = max(300, 200) + 150 = 450
        // Craft is present on the target AFTER T_switch — so the pre-T_switch
        // gate is not what rejects these — but still deciding: armed bit SET.
        // Pass 92: the FIRST of these is the craft's observed landing and
        // re-anchors the deadline to that landing + 150 ms.
        is.note_craft_video(kTs + 10'000, true);
        is.note_craft_video(kTs + 100'000, true);
        // Deadline with no commit proof — CSA_ARMED never cleared. Pre-Pass-202
        // the issuer followed the craft back; under the final jump it stays on
        // the target. The distinction stays VISIBLE (video_seen_ false and
        // reported), it is simply no longer acted on.
        CHECK_EQ_U(is.tick(kTs + 160'001).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kSuccess));
        CHECK(!is.active());
    }
    {
        // Pass 89: the same campaign, but the craft commits (clears the bit)
        // inside the window → success, exactly as before the ruling.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
        is.note_craft_armed(100'000);
        CHECK_EQ_U(is.tick(101'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        is.tick(200'000);
        is.note_craft_video(kTs + 10'000, true);   // still deciding (Pass 92:
                                                   // landing re-anchors)
        is.note_craft_video(kTs + 40'000, false);  // committed — the proof
        CHECK_EQ_U(is.tick(kTs + 160'001).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSuccess));
    }
    {
        // Pass 92: the issuer's verify deadline RE-ANCHORS on the craft's
        // observed landing — the first CSA_ARMED-set frame on target_chan
        // after T_switch. Without it the issuer measures its window from
        // T_switch while the follower measures the same budget from ITS
        // landing, so the issuer stops beaconing a full craft-retune-cost
        // (bench: median 48.7, max 67.9 ms) before the craft gives up.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
        is.note_craft_armed(100'000);
        CHECK_EQ_U(is.tick(101'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        is.tick(200'000);  // landing; pre-Pass-92 deadline = 300 + 150 = 450
        // The craft lands 80 ms after T_switch and says so (armed bit still
        // set — it has arrived, not committed).
        is.note_craft_video(kTs + 80'000, true);
        // Pre-Pass-92 this reverted here, abandoning a craft whose own window
        // runs to its own landing + 150 ms. Post-Pass-92 it is still blanketing
        // window with beacons — the craft's guaranteed confirm signal.
        CHECK_EQ_U(is.tick(kTs + 150'001).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        CHECK(is.active());
        // Commit proof inside the re-anchored window.
        is.note_craft_video(kTs + 170'000, false);
        CHECK_EQ_U(is.tick(kTs + 230'001).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSuccess));
    }
    {
        // Pass 92: ONE re-anchor per campaign. A craft that keeps advertising
        // CSA_ARMED must not be able to walk the issuer's deadline forward
        // indefinitely — the window is bounded by the craft's landing, not by
        // its most recent frame.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
        is.note_craft_armed(100'000);
        is.tick(101'000);
        is.tick(200'000);
        is.note_craft_video(kTs + 80'000, true);   // landing: re-anchors once
        is.note_craft_video(kTs + 200'000, true);  // must NOT push it further
        is.note_craft_video(kTs + 220'000, true);
        // The deadline still must not be pushed out by later ARMED video.
        CHECK_EQ_U(is.tick(kTs + 230'001).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kSuccess));
        CHECK(!is.active());
    }
    {
        // Pass 92: the re-anchor inherits the §11.6 review-pass-2 gate — a
        // craft frame BEFORE T_switch is old-channel traffic or a stale ear by
        // definition, and must not move the deadline any more than it may
        // satisfy video-verify.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
        is.note_craft_armed(100'000);
        is.tick(101'000);
        is.tick(200'000);
        is.note_craft_video(250'000, true);  // before T_switch
        // Had it re-anchored, the deadline would be 250 + 150 = 400 ms; the
        // issuer must still be beaconing there, and close only at the Pass 69
        // deadline of T_switch + verify_timeout.
        CHECK_EQ_U(is.tick(400'001).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        CHECK_EQ_U(is.tick(kTs + 150'001).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kSuccess));
    }
    {
        // Pass 92: a campaign the craft never reaches leaves the deadline
        // exactly where Pass 69 put it — no ARMED frame, no re-anchor.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (int i = 0; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
        is.note_craft_armed(100'000);
        is.tick(101'000);
        is.tick(200'000);
        CHECK_EQ_U(is.tick(kTs + 149'999).kind,
                   static_cast<unsigned>(
                       CsaIssuer::IssuerAction::Kind::kSendBeacon));
        CHECK_EQ_U(is.tick(kTs + 150'001).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kSuccess));
    }
    {
        // Pass 92: the seed the ENGINE ships. Pass 89 ruled 500 ms; the value
        // that actually runs comes from §15.2 (config_test pins that it is
        // derived from this constant, not restated).
        // §11.2 (Pass 204): the dt budget is NO LONGER the retune class. The
    // class used to pick 300 vs 500 ms, and Pass 197 pinned both because
    // quick-connect was issuing the class §11.2 rejected. Pass 202 deleted the
    // deadline those budgets sized, so the split was sizing nothing; the class
    // now survives ONLY as the fast/slow retune-path hint on the wire. Pinned
    // the new way: same generous dt for both classes, class byte preserved.
    {
        CsaParams cls_pol = policy_with_psk();
        CsaIssuer c0(cls_pol);
        CHECK(c0.start({9, 0, 1234}, 5745, 0, /*retune_class=*/0, 5805, 0, 4, 0));
        const auto a0 = c0.tick(0);
        CHECK_EQ_U(a0.kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kSendCopy));
        CHECK_EQ_U(a0.pkt.dt_to_switch_ms, CsaIssuer::kDtToSwitchMs);
        CHECK_EQ_U(a0.pkt.retune_class, 0);

        CsaIssuer c1(cls_pol);
        CHECK(c1.start({9, 0, 1234}, 5745, 0, /*retune_class=*/1, 5805, 0, 4, 0));
        const auto a1 = c1.tick(0);
        CHECK_EQ_U(a1.pkt.dt_to_switch_ms, CsaIssuer::kDtToSwitchMs);
        CHECK_EQ_U(a1.pkt.retune_class, 1);
        // The class is still carried, and still means something to the RADIO —
        // the follower turns it into CsaAction::fast. Assert that link so the
        // byte cannot quietly become decorative.
        CHECK_EQ_U(a0.pkt.retune_class == 0, true);
    }

    // §11.6 (Pass 197) WHY class 1 loses campaigns that class 0 wins, in the
    // one quantity the issuer alone can express: how long it sits ALONE on the
    // target. It commits the instant the craft ACKs (Pass 69 pre-position),
    // but the craft does not leave the old channel until T_switch — so the
    // issuer hears nothing for (T_switch - commit). Class 1 doubles that
    // silence, and it is silence the §11.6 rx_liveness_ms guard counts while
    // verify_timeout_ms does not. Bench 2026-08-30, .242 vs .181, SAME-channel
    // campaigns so retune distance was not a confound: class 0 confirmed
    // 20/20, class 1 8/20 REVERTED.
    {
        CsaParams sil_pol = policy_with_psk();
        uint64_t commit_us[2] = {0, 0};
        const uint16_t dt_ms[2] = {300, 500};
        for (int klass = 0; klass < 2; ++klass) {
            CsaIssuer is(sil_pol);
            CHECK(is.start({9, 0, 1234}, 5745, 0, static_cast<uint8_t>(klass),
                           5805, 0, 4, 0));
            for (int i = 0; i < 5; ++i) is.tick(static_cast<uint64_t>(i) * 20'000);
            is.note_craft_armed(100'000);
            const auto commit = is.tick(101'000);
            CHECK_EQ_U(commit.kind,
                       static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
            commit_us[klass] = 101'000;
            // Evidence (Pass 197): the ACK landed, nothing else has yet.
            const CsaIssuer::Evidence ev = is.evidence();
            CHECK(ev.armed_seen);
            CHECK(!ev.landing_seen);
            CHECK(!ev.video_seen);
            // T_switch - commit: the pre-position silence.
            CHECK_EQ_U(static_cast<uint64_t>(dt_ms[klass]) * 1000 - commit_us[klass],
                       klass == 0 ? 199'000u : 399'000u);
        }
    }

    // §11.2 (Pass 197) A CAMPAIGN MUST ALWAYS TERMINATE. kAnnounce's only exit
    // used to be emitting all kCopies, and the only campaign timeout lives in
    // kAwaitAck — so an issuer that ran out of copy window with copies still
    // owed sat in kAnnounce forever: never committing, never aborting, and
    // refusing every later claim with "claim busy (campaign active)". Latent
    // since Pass 90 and REACHED by this Pass: class 0's 300 ms dt leaves a
    // 250 ms copy window, and an in-process hub sharing a thread with decode
    // and OSD render does not tick 5 times inside it. Device-observed on the
    // .242 ground before the fix; pinned here at tick periods either side of
    // the boundary, for BOTH classes, because a class-1 regression would be
    // just as stuck.
    {
        CsaParams term_pol = policy_with_psk();
        const uint64_t ticks[] = {1'000, 20'000, 60'000, 80'000, 100'000,
                                  250'000};
        for (uint8_t klass = 0; klass < 2; ++klass) {
            for (uint64_t tk : ticks) {
                CsaIssuer is(term_pol);
                CHECK(is.start({9, 0, 1234}, 5745, 0, klass, 5805, 0, 4, 0));
                bool ended = false;
                // No craft ever ACKs, so the only correct end is kAbort.
                // Bound derived from T_switch (Pass 204), not a literal: the
                // campaign's terminal instant IS T_switch now, so a fixed
                // 5 s bound would sit exactly ON it and a tick period that
                // does not divide it would step straight over the abort and
                // report a wedge that is not there.
                for (uint64_t t = 0; t <= kTs + 1'000'000 && !ended; t += tk) {
                    if (is.tick(t).kind ==
                        CsaIssuer::IssuerAction::Kind::kAbort) {
                        ended = true;
                    }
                }
                if (!ended) {
                    std::fprintf(stderr,
                                 "  campaign WEDGED: class %u, tick %llu us\n",
                                 unsigned(klass),
                                 static_cast<unsigned long long>(tk));
                }
                CHECK(ended);
                CHECK(!is.active());
            }
        }
    }

    CHECK_EQ_U(kCsaVerifyTimeoutMsDefault, 500u);
        CHECK_EQ_U(CsaParams{}.verify_timeout_ms, 500u);
    }
    {
        // Pass 89: campaign_active() spans ARMED and VERIFY and drops on
        // COMMITTED — it is what drives the craft's CSA_ARMED wire bit.
        CsaFollower f(pol);
        CHECK(!f.campaign_active());
        CsaPacket c = make_csa(pol, 1, 5745, 150);
        c.csa_mac = mac_for(pol, c);
        CHECK(f.on_csa(c, 0, std::nullopt, 0, std::nullopt));
        CHECK(f.campaign_active());  // ARMED
        f.tick(150'000);             // retune -> VERIFY
        CHECK(f.campaign_active());
        f.tick(180'000);  // landing
        CHECK(f.campaign_active());
        f.note_valid_rx(200'000, 9);  // heard the issuer -> COMMITTED
        CHECK(!f.campaign_active());
    }

    {
        // Pass 90: copies repeat past the initial burst until T_switch. The
        // old fixed 5-copy burst spanned 80 ms and then went silent for the
        // rest of ack_timeout, losing the campaign if the craft (RX-deaf while
        // transmitting) heard none of the five.
        // retune_class 1 (dt0 = 500 ms) is where the extension has room. At
        // class 0 the 150 ms budget minus the 50 ms ack-lead cutoff leaves
        // only the original burst — there, gap scheduling is what carries
        // delivery, not extra copies.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 1, 5805, 0, 4, 0));
        int copies = 0;
        for (uint64_t t = 0; t < 499'000; t += 1000) {
            if (is.tick(t).kind == CsaIssuer::IssuerAction::Kind::kSendCopy) {
                ++copies;
            }
        }
        CHECK(copies > 5);
        CHECK(is.active());
    }
    {
        // Pass 90 addendum: no copy inside the last kCopyCutoffUs before
        // T_switch. A craft accepting that late cannot get CSA_ARMED back to
        // the issuer before it departs — bench-observed as the only revert in
        // eight campaigns (dt 23 ms).
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        uint64_t last_copy_us = 0;
        int copies = 0;
        for (uint64_t t = 0; t < kTs; t += 1000) {
            if (is.tick(t).kind == CsaIssuer::IssuerAction::Kind::kSendCopy) {
                last_copy_us = t;
                ++copies;
            }
        }
        CHECK(last_copy_us > 0);
        // Copies stop one ack-lead (50 ms) short of T_switch.
        CHECK(last_copy_us <= kTs - 50'000);  // the Pass 90 ack lead
        // §11.2 Pass 204: the copy window is now the whole dt minus the ack
        // lead, so a craft that misses the opening burst still has seconds of
        // retransmits to catch one. THIS is what the bench needed: at the old
        // 300 ms budget a craft that accepted a late copy jumped before its
        // CSA_ARMED could reach the issuer, and the issuer aborted and
        // stranded the pair. Device-observed 2026-09-13.
        CHECK(copies > 200);
    }
    {
        // Pass 90: the ACK stops the retransmission — it is
        // retransmit-until-acked, not an unbounded burst.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (uint64_t t = 0; t < 100'000; t += 1000) is.tick(t);
        is.note_craft_armed(100'000);
        // Next tick commits rather than emitting another copy.
        CHECK_EQ_U(is.tick(101'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        int after = 0;
        for (uint64_t t = 102'000; t < 149'000; t += 1000) {
            if (is.tick(t).kind == CsaIssuer::IssuerAction::Kind::kSendCopy) {
                ++after;
            }
        }
        CHECK_EQ_U(static_cast<unsigned>(after), 0u);
    }
    {
        // Pass 90: a copy held for the quiet gap is re-stamped at release —
        // dt shrinks to match the delay, so every copy still resolves to the
        // same absolute T_switch. A pre-stamped release would put the
        // follower's switch late by the hold time.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        CsaPacket copy{};
        for (uint64_t t = 0; t < 5'000; t += 1000) {
            const auto a = is.tick(t);
            if (a.kind == CsaIssuer::IssuerAction::Kind::kSendCopy) copy = a.pkt;
        }
        const uint16_t dt_at_stamp = copy.dt_to_switch_ms;
        const uint32_t mac_at_stamp = copy.csa_mac;
        CHECK(dt_at_stamp > 0);
        // Held 30 ms for the gap, then released.
        CHECK(is.restamp_copy(copy, 30'000));
        CHECK_EQ_U(copy.dt_to_switch_ms, dt_at_stamp - 30u);
        CHECK(copy.csa_mac != mac_at_stamp);  // the MAC covers dt
        // Pass 90 addendum: inside the ack-lead cutoff (50 ms before
        // T_switch) a copy is refused — an accepting craft could not get
        // CSA_ARMED back to the issuer before departing.
        CHECK(!is.restamp_copy(copy, kTs - 40'000));
        // Past T_switch there is nothing truthful left to say — drop, never
        // send stale.
        CHECK(!is.restamp_copy(copy, kTs + 1));
    }
    {
        // Pass 90: a re-stamped copy is still MAC-valid to a follower — the
        // re-MAC must cover the NEW dt, not merely be copied forward.
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        CsaPacket copy{};
        for (uint64_t t = 0; t < 5'000; t += 1000) {
            const auto a = is.tick(t);
            if (a.kind == CsaIssuer::IssuerAction::Kind::kSendCopy) copy = a.pkt;
        }
        CHECK(is.restamp_copy(copy, 40'000));
        CsaFollower f(pol);
        CHECK(f.on_csa(copy, 0, std::nullopt, 0, std::nullopt));
        CHECK(f.campaign_active());
    }
    {
        // Pass 90 review: a copy held across a campaign boundary must NOT be
        // re-stamped. It carries the OLD target_chan and nonce; stamping the
        // NEW campaign's dt onto it and re-MACing would produce a packet that
        // validates and sends a craft that missed the old campaign to the
        // wrong channel.
        CsaParams p = pol;
        p.min_interval_ms = 0;  // operator-settable; default 5 s hides this
        CsaIssuer is(p);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        CsaPacket old_copy{};
        for (uint64_t t = 0; t < 5'000; t += 1000) {
            const auto a = is.tick(t);
            if (a.kind == CsaIssuer::IssuerAction::Kind::kSendCopy) {
                old_copy = a.pkt;
            }
        }
        CHECK_EQ_U(old_copy.target_chan, 5745);
        // Campaign 1 dies at its T_switch (Pass 204), campaign 2 goes to a
        // DIFFERENT channel.
        for (uint64_t t = 5'000; t <= kTs + 100'000; t += 10'000) is.tick(t);
        CHECK(!is.active());
        CHECK(is.start({9, 0, 1234}, 5825, 0, 0, 5805, 0, 4, kTs + 200'000));
        // The stale copy is refused outright, not silently re-aimed.
        CHECK(!is.restamp_copy(old_copy, kTs + 210'000));
        // An idle issuer refuses too.
        CsaIssuer idle(p);
        CHECK(!idle.restamp_copy(old_copy, 1000));
    }

    // --- Pass 203: every exit from on_csa is counted -----------------------
    // The device run of 2026-09-13 read csa_accepted 0 with every refusal
    // counter also 0, and that was diagnosed as "the copies never reached
    // on_csa". It was not: the §11.6 dt==0 beacon path was the one bare exit
    // in the set, so a craft hearing ONLY beacons produced a byte-identical
    // signature. Under the final jump that is precisely what a craft the
    // ground has jumped away from hears, which is why it mattered.
    {
        CsaFollower f(pol);
        // A MAC-valid beacon for a campaign this craft never accepted.
        const CsaPacket beacon = make_csa(pol, 1, 5745, /*dt_ms=*/0);
        CHECK(!f.on_csa(beacon, 1000, std::nullopt, 0, std::nullopt));
        const CsaFollower::Refusals& r = f.refusals();
        // It is counted, and counted as ITSELF — not as any refusal.
        CHECK_EQ_U(r.beacon, 1);
        CHECK_EQ_U(r.accepted, 0);
        CHECK_EQ_U(r.no_key + r.bad_mac + r.issuer_lock + r.nonce_replay +
                       r.not_allowlisted + r.rate_limited,
                   0);
        // The property the whole set exists for: nothing can enter on_csa and
        // leave without moving a counter. Drive every exit and require the
        // total to equal the number of calls.
        CsaFollower g(pol);
        uint32_t calls = 0;
        CsaPacket bad = make_csa(pol, 1, 5745, 150);
        bad.csa_mac ^= 1;
        g.on_csa(bad, 1000, std::nullopt, 0, std::nullopt); ++calls;
        g.on_csa(make_csa(pol, 1, 5900, 150), 1000, std::nullopt, 0,
                 std::nullopt); ++calls;                      // not allowlisted
        g.on_csa(make_csa(pol, 1, 5745, 0), 1000, std::nullopt, 0,
                 std::nullopt); ++calls;                      // beacon
        g.on_csa(make_csa(pol, 5, 5745, 150), 1000, std::nullopt, 0,
                 uint16_t{7}); ++calls;                       // issuer lock
        g.on_csa(make_csa(pol, 5, 5745, 150), 1000, std::nullopt, 0,
                 std::nullopt); ++calls;                      // accepted
        g.on_csa(make_csa(pol, 5, 5745, 150), 1'100'000, std::nullopt, 0,
                 std::nullopt); ++calls;                      // nonce replay
        g.on_csa(make_csa(pol, 6, 5825, 150), 1'200'000, std::nullopt, 0,
                 std::nullopt); ++calls;                      // rate limited
        const CsaFollower::Refusals& q = g.refusals();
        CHECK_EQ_U(q.no_key + q.bad_mac + q.issuer_lock + q.nonce_replay +
                       q.not_allowlisted + q.rate_limited + q.beacon +
                       q.accepted,
                   calls);
    }
    // A beacon for the craft's OWN pending campaign still confirms the switch
    // (§11.6) — the counter must not change that behaviour, only observe it.
    {
        CsaFollower f(pol);
        const CsaPacket c = make_csa(pol, 1, 5745, 150);
        CHECK(f.on_csa(c, 0, std::nullopt, 0, std::nullopt));
        f.tick(150'000);  // retune -> VERIFY
        CsaPacket beacon = c;
        beacon.csa_seq = 0;
        beacon.dt_to_switch_ms = 0;
        beacon.csa_mac = mac_for(pol, beacon);
        CHECK(!f.on_csa(beacon, 160'000, std::nullopt, 0, std::nullopt));
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
        CHECK_EQ_U(f.refusals().beacon, 1);
    }
    // --- Pass 203: the issuer's own evidence survives the campaign close ----
    // The ground has to be able to tell a followed jump from a missed one, and
    // under the final jump kSuccess is returned for BOTH. If evidence() did
    // not outlive the close, rx_node could not make that distinction at all
    // and every jump would report as confirmed.
    {
        CsaIssuer is(pol);
        CHECK(is.start({9, 0, 1234}, 5745, 0, 0, 5805, 0, 4, 0));
        for (uint64_t t = 0; t <= 200'000; t += 20'000) is.tick(t);
        is.note_craft_armed(200'000);
        CHECK_EQ_U(is.tick(220'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        is.tick(kTs);  // landing
        const auto a = is.tick(kTs + 151'000);
        CHECK_EQ_U(a.kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kSuccess));
        CHECK(!is.active());
        // Succeeded on the target, and readable as UNCONFIRMED after the fact.
        CHECK_EQ_U(a.chan_mhz, 5745);
        CHECK(is.evidence().armed_seen);
        CHECK(!is.evidence().video_seen);
    }

    return wbtest_finish("csa_test");
}
