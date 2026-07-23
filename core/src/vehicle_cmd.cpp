// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: remote vehicle command engines (PROTOCOL.md §11.7) —
// see vehicle_cmd.h.
#include "wblink/vehicle_cmd.h"

#include "wblink/hmac_sha256.h"

namespace wblink {

namespace {

// §3.14: MAC over bytes 0..18 of the encoded packet. Re-encode rather than
// trust the caller to keep the raw frame around (the csa.cpp pattern).
std::optional<uint32_t> mac_of(const VehicleCmd& pkt,
                               const std::vector<uint8_t>& psk) {
    uint8_t buf[kVehicleCmdSize];
    if (encode_vehicle_cmd(pkt, buf, sizeof(buf)) != sizeof(buf)) {
        return std::nullopt;
    }
    return vcmd_mac(psk.data(), psk.size(), buf);
}

}  // namespace

// ---- craft ------------------------------------------------------------------

VcmdCraft::VcmdCraft(const VcmdParams& policy, const CommonPrefix& self)
    : policy_(policy), self_(self) {}

bool VcmdCraft::on_cmd(const VehicleCmd& pkt, uint64_t now_us,
                       std::optional<uint16_t> bound_issuer,
                       const Apply& apply) {
    if ((pkt.cmd_flags & vcmd_flags::kAck) != 0) {
        return false;  // an echo — craft never acts on one
    }
    // §11.7 guard order mirrors §11.4: one HMAC at most for a forged packet,
    // and every pre-MAC check is cheap.
    if (policy_.psk.empty()) {
        return false;  // no key → fail closed (announced token not learned)
    }
    const auto want = mac_of(pkt, policy_.psk);
    if (!want || *want != pkt.cmd_mac) {
        return false;
    }
    // §11.7 bound-issuer-only, no bootstrap: an unclaimed craft or a non-bound
    // sender is a silent drop (no probe oracle).
    if (!bound_issuer || *bound_issuer != pkt.prefix.originator) {
        return false;
    }
    const auto key =
        std::make_pair(pkt.prefix.originator, pkt.prefix.session_id);
    const auto it = last_applied_.find(key);
    if (it != last_applied_.end() && pkt.cmd_nonce <= it->second.nonce) {
        // §11.7 idempotent retry: the exact command re-echoes its remembered
        // outcome (a retried campaign means the ground lost the echo). A
        // same-nonce packet with DIFFERENT fields, or an older nonce, drops.
        if (pkt.cmd_nonce == it->second.nonce &&
            pkt.cmd_id == it->second.cmd_id &&
            pkt.cmd_arg == it->second.cmd_arg) {
            queue_echo(it->second, pkt.prefix.originator, now_us);
        }
        return false;
    }
    if (last_accept_us_ != 0 &&
        now_us - last_accept_us_ <
            static_cast<uint64_t>(policy_.min_interval_ms) * 1000) {
        return false;
    }

    // Accept: the nonce is consumed in the applied AND rejected cases — the
    // ground learns "understood, won't do" instead of retrying into silence.
    Outcome o{pkt.cmd_nonce, pkt.cmd_id, pkt.cmd_arg, false};
    o.rejected = !apply || !apply(pkt.cmd_id, pkt.cmd_arg);
    last_applied_[key] = o;
    last_ = o;
    last_accept_us_ = now_us;
    queue_echo(o, pkt.prefix.originator, now_us);
    return !o.rejected;
}

void VcmdCraft::queue_echo(const Outcome& o, uint16_t to, uint64_t now_us) {
    // One burst per nonce per min_interval: campaign copies arrive
    // copy_interval_ms apart and would otherwise re-trigger a burst each.
    if (echoed_once_ && last_echo_nonce_ == o.nonce &&
        (echoes_left_ > 0 ||
         now_us - last_echo_start_us_ <
             static_cast<uint64_t>(policy_.min_interval_ms) * 1000)) {
        return;
    }
    echo_ = VehicleCmd{};
    echo_.prefix = self_;
    echo_.prefix.destination = to;
    echo_.cmd_nonce = o.nonce;
    echo_.cmd_flags = static_cast<uint8_t>(
        vcmd_flags::kAck | (o.rejected ? vcmd_flags::kRejected : 0));
    echo_.cmd_id = o.cmd_id;
    echo_.cmd_arg = o.cmd_arg;
    echoes_left_ = policy_.echo_copies;
    next_echo_us_ = now_us;
    last_echo_nonce_ = o.nonce;
    last_echo_start_us_ = now_us;
    echoed_once_ = true;
}

std::optional<VehicleCmd> VcmdCraft::tick(uint64_t now_us) {
    if (echoes_left_ == 0 || now_us < next_echo_us_) {
        return std::nullopt;
    }
    VehicleCmd out = echo_;
    out.cmd_seq = echoes_left_;
    if (const auto m = mac_of(out, policy_.psk)) {
        out.cmd_mac = *m;
    } else {
        echoes_left_ = 0;
        return std::nullopt;
    }
    --echoes_left_;
    next_echo_us_ =
        now_us + static_cast<uint64_t>(policy_.copy_interval_ms) * 1000;
    return out;
}

// ---- issuer -----------------------------------------------------------------

VcmdIssuer::VcmdIssuer(const VcmdParams& policy) : policy_(policy) {}

bool VcmdIssuer::set_psk(std::vector<uint8_t> psk) {
    if (active()) {
        return false;  // never swap the key mid-campaign
    }
    policy_.psk = std::move(psk);
    return true;
}

bool VcmdIssuer::start(const CommonPrefix& prefix, uint16_t target_originator,
                       uint8_t cmd_id, uint8_t cmd_arg, uint64_t now_us) {
    if (active() || policy_.psk.empty() || target_originator == 0 ||
        cmd_arg > kVcmdMaxArg) {
        return false;
    }
    // §11.7: pace starts by min_interval + copy_interval so a fresh
    // campaign's first copy always clears the craft-side accept limit.
    if (last_start_us_ != 0 &&
        now_us - last_start_us_ <
            static_cast<uint64_t>(policy_.min_interval_ms +
                                  policy_.copy_interval_ms) *
                1000) {
        return false;
    }
    last_start_us_ = now_us;
    tmpl_ = VehicleCmd{};
    tmpl_.prefix = prefix;
    tmpl_.prefix.destination = target_originator;
    tmpl_.cmd_nonce = next_nonce_++;
    tmpl_.cmd_id = cmd_id;
    tmpl_.cmd_arg = cmd_arg;
    target_ = target_originator;
    copies_left_ = policy_.copies;
    campaigns_left_ = policy_.retry_cap;
    next_copy_us_ = now_us;
    state_ = State::kSending;
    return true;
}

void VcmdIssuer::on_echo(const VehicleCmd& pkt, uint64_t now_us) {
    (void)now_us;
    if (state_ != State::kSending && state_ != State::kAwaitEcho) {
        return;  // stale/duplicate echo after a terminal state
    }
    if ((pkt.cmd_flags & vcmd_flags::kAck) == 0 ||
        pkt.prefix.originator != target_ ||
        pkt.cmd_nonce != tmpl_.cmd_nonce || pkt.cmd_id != tmpl_.cmd_id ||
        pkt.cmd_arg != tmpl_.cmd_arg) {
        return;
    }
    // A forged un-MAC'd echo must not fake an applied command (§13 row).
    const auto want = mac_of(pkt, policy_.psk);
    if (!want || *want != pkt.cmd_mac) {
        return;
    }
    state_ = (pkt.cmd_flags & vcmd_flags::kRejected) != 0 ? State::kRejected
                                                          : State::kAcked;
}

VcmdIssuer::Action VcmdIssuer::tick(uint64_t now_us) {
    Action a;
    switch (state_) {
        case State::kSending:
            if (now_us >= next_copy_us_) {
                a.kind = Action::Kind::kSendCopy;
                a.pkt = tmpl_;
                a.pkt.cmd_seq = copies_left_;
                if (const auto m = mac_of(a.pkt, policy_.psk)) {
                    a.pkt.cmd_mac = *m;
                }
                next_copy_us_ =
                    now_us +
                    static_cast<uint64_t>(policy_.copy_interval_ms) * 1000;
                if (--copies_left_ == 0) {
                    // The ack window opens as the last copy leaves.
                    echo_deadline_us_ =
                        now_us +
                        static_cast<uint64_t>(policy_.ack_timeout_ms) * 1000;
                    state_ = State::kAwaitEcho;
                }
            }
            break;
        case State::kAwaitEcho:
            if (now_us >= echo_deadline_us_) {
                if (--campaigns_left_ == 0) {
                    state_ = State::kTimeout;
                } else {
                    // §11.7: re-send the SAME nonce — duplicates re-echo.
                    copies_left_ = policy_.copies;
                    next_copy_us_ = now_us;
                    state_ = State::kSending;
                }
            }
            break;
        case State::kIdle:
        case State::kAcked:
        case State::kRejected:
        case State::kTimeout:
            break;
    }
    return a;
}

const char* VcmdIssuer::state_str() const {
    switch (state_) {
        case State::kIdle:
            return "idle";
        case State::kSending:
        case State::kAwaitEcho:
            return "pending";
        case State::kAcked:
            return "acked";
        case State::kRejected:
            return "rejected";
        case State::kTimeout:
            return "timeout";
    }
    return "?";
}

}  // namespace wblink
