// SPDX-License-Identifier: GPL-2.0-or-later
// §11 CSA engine tests: follower validation (MAC / replay / issuer lock /
// allowlist / rate-limit), TSF anchoring incl. u32 wrap, the §11.5 state
// machine (verify-revert, rendezvous home, link-loss long path), the §11.6
// issuer (decrementing-dt copies, commit-after-armed, ack-timeout abort,
// revert-on-no-video), and the §11.3 selector freeze pause.
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
    p.home_chan = 5745;
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
        f.note_valid_rx(200'000);
        CHECK(!f.armed());
        CHECK(std::string_view(f.state_str()) == "COMMITTED");
    }
    {
        // No traffic ⇒ revert at t_revert_ms, then rendezvous home.
        CsaFollower f(pol);
        CHECK(f.on_csa(make_csa(pol, 1, 5825, 150), 0, std::nullopt, 0,
                       std::nullopt));
        CHECK_EQ_U(f.tick(150'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kRetune));
        const auto rv = f.tick(150'000 + 150'000);  // wire t_revert_ms = 150
        CHECK_EQ_U(rv.kind, static_cast<unsigned>(CsaAction::Kind::kRevert));
        CHECK_EQ_U(rv.chan_mhz, 5805);  // prev_chan
        const auto hm = f.tick(300'000 + 5'000'000);  // rendezvous_timeout
        CHECK_EQ_U(hm.kind, static_cast<unsigned>(CsaAction::Kind::kHome));
        CHECK_EQ_U(hm.chan_mhz, 5745);
        // Traffic on home closes the campaign.
        f.note_valid_rx(6'000'000);
        CHECK(std::string_view(f.state_str()) == "IDLE");
    }
    {
        // Long path: node that never saw a CSA loses the link > rendezvous
        // timeout ⇒ home. Requires prior traffic (no false trigger at boot).
        CsaFollower f(pol);
        CHECK_EQ_U(f.tick(10'000'000).kind,
                   static_cast<unsigned>(CsaAction::Kind::kNone));
        f.note_valid_rx(10'000'000);
        const auto a = f.tick(10'000'000 + 5'000'001);
        CHECK_EQ_U(a.kind, static_cast<unsigned>(CsaAction::Kind::kHome));
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
        // Craft armed → commit at T_switch (150 ms for class 0).
        is.note_craft_armed(100'000);
        CHECK_EQ_U(is.tick(149'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kNone));
        const auto c = is.tick(150'000);
        CHECK_EQ_U(c.kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
        CHECK_EQ_U(c.chan_mhz, 5745);
        // Craft video arrives → campaign closed.
        is.note_craft_video(200'000);
        CHECK(!is.active());
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
        CHECK_EQ_U(is.tick(150'000).kind,
                   static_cast<unsigned>(CsaIssuer::IssuerAction::Kind::kCommit));
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

    return wbtest_finish("csa_test");
}
