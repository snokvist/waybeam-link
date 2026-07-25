// SPDX-License-Identifier: GPL-2.0-or-later
// §11.7 VEHICLE_CMD engine tests: craft acceptance (MAC / bound-issuer-only,
// no bootstrap / nonce monotonicity / rate-limit), stored-tuple duplicate
// re-echo with per-nonce burst holddown, REJECTED consumption (unknown id,
// bad arg, unconfigured actuator), per-(originator,session) domains; issuer
// campaign (copies, echo acceptance guards incl. MAC and forged-echo
// rejection, same-nonce retry, retry_cap timeout, start pacing, terminal
// states), and the end-to-end issuer↔craft loop.
#include "wblink/vehicle_cmd.h"

#include <string>

#include "wblink/hmac_sha256.h"
#include "wblink/wire.h"
#include "wbtest.h"

using namespace wblink;

namespace {

constexpr uint16_t kGround = 9;
constexpr uint16_t kCraftOrig = 17;
constexpr uint32_t kGroundSession = 1234;

VcmdParams policy_with_psk() {
    VcmdParams p;
    p.psk = {'s', 'e', 'c', 'r', 'e', 't'};
    return p;
}

uint32_t mac_for(const VehicleCmd& c, const VcmdParams& pol) {
    uint8_t buf[kVehicleCmdSize];
    CHECK_EQ_U(encode_vehicle_cmd(c, buf, sizeof(buf)), kVehicleCmdSize);
    return vcmd_mac(pol.psk.data(), pol.psk.size(), buf);
}

VehicleCmd make_cmd(const VcmdParams& pol, uint32_t nonce, uint8_t id,
                    uint8_t arg, uint32_t session = kGroundSession) {
    VehicleCmd c;
    c.prefix = {kGround, kCraftOrig, session};
    c.cmd_nonce = nonce;
    c.cmd_seq = 3;
    c.cmd_id = id;
    c.cmd_arg = arg;
    c.cmd_mac = mac_for(c, pol);
    return c;
}

// Drain the craft's echo burst at now_us, advancing only through the copy
// spacing (two consecutive empty ticks end the drain, so the caller's clock
// stays well inside the min_interval holddown window).
std::vector<VehicleCmd> drain_echoes(VcmdCraft& craft, uint64_t& now_us,
                                     const VcmdParams& pol) {
    std::vector<VehicleCmd> out;
    int misses = 0;
    while (misses < 2) {
        if (const auto e = craft.tick(now_us)) {
            out.push_back(*e);
            misses = 0;
        } else {
            ++misses;
        }
        now_us += pol.copy_interval_ms * 1000;
    }
    return out;
}

}  // namespace

int main() {
    const VcmdParams pol = policy_with_psk();
    const CommonPrefix craft_self{kCraftOrig, 0, 777};
    const std::optional<uint16_t> bound{kGround};

    // --- craft acceptance + echo -------------------------------------------
    {
        VcmdCraft craft(pol, craft_self);
        uint64_t now = 1'000'000;
        int applied_id = -1, applied_arg = -1;
        const VcmdCraft::Apply apply = [&](uint8_t id, uint8_t arg) {
            applied_id = id;
            applied_arg = arg;
            return true;
        };
        VehicleCmd c = make_cmd(pol, 10, vcmd_id::kArq, 0);

        // Bad MAC → silent drop, nothing applied, no echo.
        VehicleCmd bad = c;
        bad.cmd_mac ^= 1;
        CHECK(!craft.on_cmd(bad, now, bound, apply));
        CHECK_EQ_U(craft.last_nonce(), 0u);
        CHECK(!craft.tick(now).has_value());

        // Unbound craft / non-bound sender → silent drop (no bootstrap).
        CHECK(!craft.on_cmd(c, now, std::nullopt, apply));
        CHECK(!craft.on_cmd(c, now, std::optional<uint16_t>{12}, apply));
        CHECK(!craft.tick(now).has_value());

        // An echo-flagged packet is never acted on.
        VehicleCmd as_echo = c;
        as_echo.cmd_flags = vcmd_flags::kAck;
        as_echo.cmd_mac = mac_for(as_echo, pol);
        CHECK(!craft.on_cmd(as_echo, now, bound, apply));

        // MAC-valid, bound → applied; echo burst carries ACK + the fields,
        // re-MAC'd under the craft prefix, addressed to the issuer.
        CHECK(craft.on_cmd(c, now, bound, apply));
        CHECK_EQ_U(static_cast<unsigned>(applied_id), vcmd_id::kArq);
        CHECK_EQ_U(static_cast<unsigned>(applied_arg), 0u);
        CHECK_EQ_U(craft.last_nonce(), 10u);
        auto echoes = drain_echoes(craft, now, pol);
        CHECK_EQ_U(echoes.size(), pol.echo_copies);
        for (const VehicleCmd& e : echoes) {
            CHECK(e.cmd_flags == vcmd_flags::kAck);
            CHECK(e.prefix.originator == kCraftOrig);
            CHECK(e.prefix.destination == kGround);
            CHECK(e.cmd_nonce == 10 && e.cmd_id == vcmd_id::kArq &&
                  e.cmd_arg == 0);
            VehicleCmd unmacd = e;
            unmacd.cmd_mac = 0;
            CHECK_EQ_U(e.cmd_mac, mac_for(unmacd, pol));
        }

        // Craft-side rate limit: a NEW nonce within min_interval of the last
        // accept is dropped and NOT consumed — the same nonce lands later.
        VehicleCmd next = make_cmd(pol, 11, vcmd_id::kSelector, 1);
        CHECK(!craft.on_cmd(next, now, bound, apply));
        now += static_cast<uint64_t>(pol.min_interval_ms + 1) * 1000;
        CHECK(craft.on_cmd(next, now, bound, apply));
        CHECK_EQ_U(static_cast<unsigned>(applied_id), vcmd_id::kSelector);
        echoes = drain_echoes(craft, now, pol);
        CHECK_EQ_U(echoes.size(), pol.echo_copies);

        // Duplicate nonce, same fields: re-echo WITHOUT re-apply — but only
        // after the per-nonce burst holddown (min_interval) has passed.
        applied_id = -1;
        CHECK(!craft.on_cmd(next, now, bound, apply));
        CHECK(applied_id == -1);
        CHECK(!craft.tick(now).has_value());  // within holddown: no burst
        now += static_cast<uint64_t>(pol.min_interval_ms + 1) * 1000;
        CHECK(!craft.on_cmd(next, now, bound, apply));
        CHECK(applied_id == -1);
        echoes = drain_echoes(craft, now, pol);
        CHECK_EQ_U(echoes.size(), pol.echo_copies);
        CHECK(echoes[0].cmd_nonce == 11 &&
              echoes[0].cmd_id == vcmd_id::kSelector);

        // Duplicate nonce, DIFFERENT fields → silent drop (a retry cannot
        // mint an ACK for a command never applied).
        VehicleCmd twisted = make_cmd(pol, 11, vcmd_id::kSelector, 0);
        now += static_cast<uint64_t>(pol.min_interval_ms + 1) * 1000;
        CHECK(!craft.on_cmd(twisted, now, bound, apply));
        CHECK(!craft.tick(now).has_value());

        // Older nonce → silent drop.
        CHECK(!craft.on_cmd(c, now, bound, apply));
        CHECK(!craft.tick(now).has_value());

        // A fresh issuer session opens a fresh nonce domain.
        VehicleCmd resess = make_cmd(pol, 1, vcmd_id::kArq, 1, 5678);
        now += static_cast<uint64_t>(pol.min_interval_ms + 1) * 1000;
        CHECK(craft.on_cmd(resess, now, bound, apply));
    }

    // --- craft REJECTED paths ----------------------------------------------
    {
        VcmdCraft craft(pol, craft_self);
        uint64_t now = 1'000'000;
        const VcmdCraft::Apply reject_all = [](uint8_t, uint8_t) {
            return false;
        };
        // Unapplicable command: consumed (on_cmd false), echoed REJECTED|ACK.
        VehicleCmd c = make_cmd(pol, 5, vcmd_id::kFpsLadder, 1);
        CHECK(!craft.on_cmd(c, now, bound, reject_all));
        CHECK_EQ_U(craft.last_nonce(), 5u);
        auto echoes = drain_echoes(craft, now, pol);
        CHECK_EQ_U(echoes.size(), pol.echo_copies);
        CHECK(echoes[0].cmd_flags ==
              (vcmd_flags::kAck | vcmd_flags::kRejected));
        // The duplicate re-echo remembers the REJECTED disposition.
        now += static_cast<uint64_t>(pol.min_interval_ms + 1) * 1000;
        CHECK(!craft.on_cmd(c, now, bound, reject_all));
        echoes = drain_echoes(craft, now, pol);
        CHECK_EQ_U(echoes.size(), pol.echo_copies);
        CHECK(echoes[0].cmd_flags ==
              (vcmd_flags::kAck | vcmd_flags::kRejected));
        // The consumed nonce blocks re-application under a working apply.
        now += static_cast<uint64_t>(pol.min_interval_ms + 1) * 1000;
        int applied = 0;
        CHECK(!craft.on_cmd(c, now, bound, [&](uint8_t, uint8_t) {
            ++applied;
            return true;
        }));
        CHECK_EQ_U(static_cast<unsigned>(applied), 0u);
    }

    // --- craft MODE (Pass 105): a wide arg reaches Apply untouched ----------
    {
        // The core does not clamp MODE; the app-level Apply maps the index and
        // REJECTs an over-range one. Verify the >4 arg survives on_cmd, and a
        // rejecting Apply produces a REJECTED echo carrying that same wide arg.
        VcmdCraft craft(pol, craft_self);
        uint64_t now = 1'000'000;
        uint8_t seen_id = 0, seen_arg = 0;
        const VcmdCraft::Apply capture = [&](uint8_t id, uint8_t arg) {
            seen_id = id;
            seen_arg = arg;
            return arg < 10;  // stand-in for "index within catalog"
        };
        VehicleCmd in_range = make_cmd(pol, 7, vcmd_id::kMode, 9);
        CHECK(craft.on_cmd(in_range, now, bound, capture));  // applied
        CHECK_EQ_U(static_cast<unsigned>(seen_id), vcmd_id::kMode);
        CHECK_EQ_U(static_cast<unsigned>(seen_arg), 9u);
        drain_echoes(craft, now, pol);
        now += static_cast<uint64_t>(pol.min_interval_ms + 1) * 1000;
        VehicleCmd over = make_cmd(pol, 8, vcmd_id::kMode, 200);
        CHECK(!craft.on_cmd(over, now, bound, capture));  // out of range
        CHECK_EQ_U(static_cast<unsigned>(seen_arg), 200u);
        auto echoes = drain_echoes(craft, now, pol);
        CHECK_EQ_U(echoes.size(), pol.echo_copies);
        CHECK(echoes[0].cmd_flags ==
              (vcmd_flags::kAck | vcmd_flags::kRejected));
        CHECK_EQ_U(static_cast<unsigned>(echoes[0].cmd_arg), 200u);
    }

    // --- issuer campaign ----------------------------------------------------
    {
        VcmdIssuer issuer(pol);
        issuer.seed_nonce(4000);
        uint64_t now = 1'000'000;
        CHECK(std::string(issuer.state_str()) == "idle");
        // No PSK → refused.
        VcmdIssuer nokey{VcmdParams{}};
        CHECK(!nokey.start({kGround, 0, kGroundSession}, kCraftOrig,
                           vcmd_id::kArq, 0, now));
        // Bad arg / zero target → refused.
        CHECK(!issuer.start({kGround, 0, kGroundSession}, 0, vcmd_id::kArq, 0,
                            now));
        CHECK(!issuer.start({kGround, 0, kGroundSession}, kCraftOrig,
                            vcmd_id::kArq, kVcmdMaxArg + 1, now));
        // §11.7 Pass 105: a wide arg the ≤5 bound refuses is accepted for MODE
        // and still refused for any other command (the cmd_id-dependent gate).
        {
            VcmdIssuer modei(pol);
            CHECK(!modei.start({kGround, 0, kGroundSession}, kCraftOrig,
                               vcmd_id::kFpsSelect, 9, now));
            CHECK(modei.start({kGround, 0, kGroundSession}, kCraftOrig,
                              vcmd_id::kMode, 9, now));
        }

        CHECK(issuer.start({kGround, 0, kGroundSession}, kCraftOrig,
                           vcmd_id::kArq, 0, now));
        CHECK_EQ_U(issuer.nonce(), 4000u);
        CHECK(std::string(issuer.state_str()) == "pending");
        CHECK(!issuer.start({kGround, 0, kGroundSession}, kCraftOrig,
                            vcmd_id::kArq, 1, now));  // active → refused
        // Copies come out spaced copy_interval_ms, cmd_seq N..1, MAC'd.
        std::vector<VehicleCmd> copies;
        for (int i = 0; i < 32 && copies.size() < pol.copies; ++i) {
            const auto a = issuer.tick(now);
            if (a.kind == VcmdIssuer::Action::Kind::kSendCopy) {
                copies.push_back(a.pkt);
            }
            now += pol.copy_interval_ms * 1000;
        }
        CHECK_EQ_U(copies.size(), pol.copies);
        CHECK_EQ_U(static_cast<unsigned>(copies.front().cmd_seq), pol.copies);
        CHECK_EQ_U(static_cast<unsigned>(copies.back().cmd_seq), 1u);
        for (const VehicleCmd& k : copies) {
            VehicleCmd unmacd = k;
            unmacd.cmd_mac = 0;
            CHECK_EQ_U(k.cmd_mac, mac_for(unmacd, pol));
            CHECK(k.prefix.destination == kCraftOrig);
        }

        // Echo acceptance guards: ACK flag, sender, nonce, cmd, arg, MAC.
        VehicleCmd echo;
        echo.prefix = {kCraftOrig, kGround, 777};
        echo.cmd_nonce = 4000;
        echo.cmd_flags = vcmd_flags::kAck;
        echo.cmd_id = vcmd_id::kArq;
        echo.cmd_arg = 0;
        echo.cmd_mac = mac_for(echo, pol);
        VehicleCmd wrong = echo;
        wrong.cmd_nonce = 3999;
        wrong.cmd_mac = mac_for(wrong, pol);
        issuer.on_echo(wrong, now);
        CHECK(std::string(issuer.state_str()) == "pending");
        wrong = echo;
        wrong.prefix.originator = 18;  // not the bound craft
        wrong.cmd_mac = mac_for(wrong, pol);
        issuer.on_echo(wrong, now);
        CHECK(std::string(issuer.state_str()) == "pending");
        wrong = echo;
        wrong.cmd_mac ^= 1;  // forged echo must not fake "acked" (§13)
        issuer.on_echo(wrong, now);
        CHECK(std::string(issuer.state_str()) == "pending");
        issuer.on_echo(echo, now);
        CHECK(std::string(issuer.state_str()) == "acked");
        // Start pacing: a new campaign within min_interval+copy_interval of
        // the last start is refused, then accepted.
        CHECK(!issuer.start({kGround, 0, kGroundSession}, kCraftOrig,
                            vcmd_id::kSelector, 1, now));
        now += static_cast<uint64_t>(pol.min_interval_ms +
                                     pol.copy_interval_ms + 1) * 1000;
        CHECK(issuer.start({kGround, 0, kGroundSession}, kCraftOrig,
                           vcmd_id::kSelector, 1, now));
        CHECK_EQ_U(issuer.nonce(), 4001u);
        // REJECTED echo → terminal rejected.
        for (int i = 0; i < 32; ++i) {
            issuer.tick(now);
            now += pol.copy_interval_ms * 1000;
        }
        VehicleCmd rej;
        rej.prefix = {kCraftOrig, kGround, 777};
        rej.cmd_nonce = 4001;
        rej.cmd_flags = vcmd_flags::kAck | vcmd_flags::kRejected;
        rej.cmd_id = vcmd_id::kSelector;
        rej.cmd_arg = 1;
        rej.cmd_mac = mac_for(rej, pol);
        issuer.on_echo(rej, now);
        CHECK(std::string(issuer.state_str()) == "rejected");
    }

    // --- issuer retry + timeout --------------------------------------------
    {
        VcmdIssuer issuer(pol);
        issuer.seed_nonce(1);
        uint64_t now = 1'000'000;
        CHECK(issuer.start({kGround, 0, kGroundSession}, kCraftOrig,
                           vcmd_id::kArq, 1, now));
        // retry_cap campaigns of `copies` copies each, all the SAME nonce,
        // then terminal timeout.
        size_t sent = 0;
        for (int i = 0; i < 4000 && std::string(issuer.state_str()) ==
                                        "pending"; ++i) {
            const auto a = issuer.tick(now);
            if (a.kind == VcmdIssuer::Action::Kind::kSendCopy) {
                CHECK_EQ_U(a.pkt.cmd_nonce, 1u);
                ++sent;
            }
            now += 5'000;  // 5 ms
        }
        CHECK_EQ_U(sent, pol.copies * pol.retry_cap);
        CHECK(std::string(issuer.state_str()) == "timeout");
    }

    // --- end-to-end: issuer ↔ craft ----------------------------------------
    {
        VcmdIssuer issuer(pol);
        issuer.seed_nonce(0xFFFFFFF0u);  // random seed near wrap still works
        VcmdCraft craft(pol, craft_self);
        uint64_t now = 1'000'000;
        int applied_arg = -1;
        const VcmdCraft::Apply apply = [&](uint8_t, uint8_t arg) {
            applied_arg = arg;
            return true;
        };
        CHECK(issuer.start({kGround, 0, kGroundSession}, kCraftOrig,
                           vcmd_id::kArq, 0, now));
        for (int i = 0; i < 64 && std::string(issuer.state_str()) ==
                                      "pending"; ++i) {
            const auto a = issuer.tick(now);
            if (a.kind == VcmdIssuer::Action::Kind::kSendCopy) {
                craft.on_cmd(a.pkt, now, bound, apply);
            }
            if (const auto e = craft.tick(now)) {
                issuer.on_echo(*e, now);
            }
            now += pol.copy_interval_ms * 1000;
        }
        CHECK_EQ_U(static_cast<unsigned>(applied_arg), 0u);
        CHECK(std::string(issuer.state_str()) == "acked");
        CHECK_EQ_U(craft.last_nonce(), 0xFFFFFFF0u);
    }

    return wbtest_finish("vehicle_cmd_test");
}
