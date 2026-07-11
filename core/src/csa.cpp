// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: CSA engines (PROTOCOL.md §11) — see csa.h.
#include "wblink/csa.h"

#include "wblink/hmac_sha256.h"

namespace wblink {

namespace {

// §11.4: MAC over bytes 0..27 of the encoded packet. encode_csa is
// deterministic, so verification re-encodes rather than trusting the caller
// to keep the raw frame around. Returns nullopt on an encode failure (never
// happens for a decoded packet; fail-closed anyway).
std::optional<uint32_t> mac_of(const CsaPacket& pkt,
                               const std::vector<uint8_t>& psk) {
    uint8_t buf[32];
    if (encode_csa(pkt, buf, sizeof(buf)) != sizeof(buf)) {
        return std::nullopt;
    }
    return csa_mac(psk.data(), psk.size(), buf);
}

bool allowed(const std::vector<uint16_t>& allowlist, uint16_t chan) {
    for (const uint16_t c : allowlist) {
        if (c == chan) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---- follower ---------------------------------------------------------------

CsaFollower::CsaFollower(const CsaParams& policy) : policy_(policy) {}

bool CsaFollower::on_csa(const CsaPacket& pkt, uint64_t now_us,
                         std::optional<uint64_t> tsf_now_us,
                         uint32_t rx_tsfl_us,
                         std::optional<uint16_t> latched_issuer) {
    // §11.4 validation order, cheapest structural checks last so a forged
    // packet costs one HMAC at most.
    if (!policy_.psk.empty()) {
        const auto want = mac_of(pkt, policy_.psk);
        if (!want || *want != pkt.csa_mac) {
            return false;
        }
    }
    // Issuer lock: the currently-latched command source. A MAC-valid CSA may
    // establish the binding (§11.4 bootstrap); an unauthenticated spectator
    // latches onto whoever it heard first.
    const std::optional<uint16_t> lock = latched_issuer ? latched_issuer
                                                        : latched_;
    if (lock && *lock != pkt.prefix.originator) {
        return false;
    }
    const auto key = std::make_pair(pkt.prefix.originator,
                                    pkt.prefix.session_id);
    const auto it = last_applied_.find(key);
    if (it != last_applied_.end() && pkt.csa_nonce <= it->second) {
        return false;  // replay, or another copy of an accepted campaign
    }
    if (!allowed(policy_.allowlist, pkt.target_chan)) {
        return false;
    }
    if (last_accept_us_ != 0 &&
        now_us - last_accept_us_ <
            static_cast<uint64_t>(policy_.min_interval_ms) * 1000) {
        return false;
    }
    if (pkt.dt_to_switch_ms == 0) {
        return false;
    }

    // Accept. §11.2 TSF anchor: elapsed since the copy left the air, from the
    // SAME adapter's TSF; no TSF read → host-arrival approximation.
    last_applied_[key] = pkt.csa_nonce;
    last_accept_us_ = now_us;
    latched_ = pkt.prefix.originator;
    campaign_ = pkt;
    const uint64_t dt_us = static_cast<uint64_t>(pkt.dt_to_switch_ms) * 1000;
    uint64_t elapsed = 0;
    if (tsf_now_us) {
        elapsed = static_cast<uint32_t>(static_cast<uint32_t>(*tsf_now_us) -
                                        rx_tsfl_us);
    }
    switch_at_us_ = now_us + (dt_us > elapsed ? dt_us - elapsed : 0);
    freeze_until_us_ =
        now_us + static_cast<uint64_t>(policy_.settle_ms) * 1000;  // §11.3
    state_ = State::kArmed;
    return true;
}

void CsaFollower::note_valid_rx(uint64_t now_us) {
    have_traffic_ = true;
    last_rx_us_ = now_us;
    switch (state_) {
        case State::kVerify:
            state_ = State::kCommitted;
            break;
        case State::kCommitted:
        case State::kReverted:
        case State::kHome:
            state_ = State::kIdle;  // link re-established, campaign closed
            break;
        default:
            break;
    }
}

CsaAction CsaFollower::tick(uint64_t now_us) {
    CsaAction a;
    switch (state_) {
        case State::kArmed:
            if (now_us >= switch_at_us_) {
                a.kind = CsaAction::Kind::kRetune;
                a.chan_mhz = campaign_.target_chan;
                a.bw = campaign_.target_bw;
                a.fast = campaign_.retune_class == 0;
                a.power_intent = campaign_.power_intent;
                const uint32_t vt = campaign_.t_revert_ms != 0
                                        ? campaign_.t_revert_ms
                                        : policy_.verify_timeout_ms;
                verify_deadline_us_ = now_us + static_cast<uint64_t>(vt) * 1000;
                state_ = State::kVerify;
            }
            break;
        case State::kVerify:
            if (now_us >= verify_deadline_us_) {
                a.kind = CsaAction::Kind::kRevert;
                a.chan_mhz = campaign_.prev_chan;
                a.bw = campaign_.prev_bw;
                a.fast = campaign_.retune_class == 0;
                a.power_intent = campaign_.power_intent;
                rendezvous_deadline_us_ =
                    now_us +
                    static_cast<uint64_t>(policy_.rendezvous_timeout_ms) *
                        1000;
                state_ = State::kReverted;
            }
            break;
        case State::kReverted:
            if (now_us >= rendezvous_deadline_us_ && policy_.home_chan != 0) {
                a.kind = CsaAction::Kind::kHome;
                a.chan_mhz = policy_.home_chan;
                a.bw = 0;  // home rendezvous is always 20 MHz (§1)
                a.power_intent = campaign_.power_intent;
                state_ = State::kHome;
            }
            break;
        case State::kIdle:
            // §11.5 long path: a node that never saw the CSA loses the link
            // and falls back to the config rendezvous channel.
            if (have_traffic_ && policy_.home_chan != 0 &&
                now_us - last_rx_us_ >
                    static_cast<uint64_t>(policy_.rendezvous_timeout_ms) *
                        1000) {
                a.kind = CsaAction::Kind::kHome;
                a.chan_mhz = policy_.home_chan;
                a.bw = 0;
                state_ = State::kHome;
            }
            break;
        case State::kCommitted:
        case State::kHome:
            break;
    }
    return a;
}

const char* CsaFollower::state_str() const {
    switch (state_) {
        case State::kIdle:
            return "IDLE";
        case State::kArmed:
            return "ARMED";
        case State::kVerify:
            return "VERIFY";
        case State::kCommitted:
            return "COMMITTED";
        case State::kReverted:
            return "REVERTED";
        case State::kHome:
            return "HOME";
    }
    return "?";
}

// ---- issuer -----------------------------------------------------------------

CsaIssuer::CsaIssuer(const CsaParams& policy) : policy_(policy) {}

bool CsaIssuer::start(const CommonPrefix& prefix, uint16_t target_chan_mhz,
                      uint8_t target_bw, uint8_t retune_class,
                      uint16_t prev_chan_mhz, uint8_t prev_bw,
                      uint8_t power_intent, uint64_t now_us) {
    if (state_ != State::kIdle || policy_.psk.empty()) {
        return false;  // issuing without a PSK is a no-op fleet-wide
    }
    if (!allowed(policy_.allowlist, target_chan_mhz)) {
        return false;
    }
    if (last_campaign_us_ != 0 &&
        now_us - last_campaign_us_ <
            static_cast<uint64_t>(policy_.min_interval_ms) * 1000) {
        return false;
    }
    last_campaign_us_ = now_us;
    prefix_ = prefix;
    tmpl_ = CsaPacket{};
    tmpl_.prefix = prefix;
    tmpl_.csa_nonce = next_nonce_++;
    tmpl_.target_chan = target_chan_mhz;
    tmpl_.target_bw = target_bw;
    tmpl_.retune_class = retune_class;
    tmpl_.t_revert_ms = static_cast<uint16_t>(policy_.verify_timeout_ms);
    tmpl_.prev_chan = prev_chan_mhz;
    tmpl_.prev_bw = prev_bw;
    tmpl_.power_intent = power_intent;
    // §11.2 dt budget: campaign span + max retune for the class + margin.
    const uint32_t dt0_ms = retune_class == 0 ? 150 : 500;
    started_us_ = now_us;
    switch_at_us_ = now_us + static_cast<uint64_t>(dt0_ms) * 1000;
    copies_left_ = kCopies;
    next_copy_us_ = now_us;
    armed_seen_ = false;
    state_ = State::kAnnounce;
    return true;
}

void CsaIssuer::note_craft_armed(uint64_t) {
    if (state_ == State::kAnnounce || state_ == State::kAwaitAck) {
        armed_seen_ = true;
    }
}

void CsaIssuer::note_craft_video(uint64_t) {
    if (state_ == State::kVerify) {
        state_ = State::kIdle;  // campaign succeeded
    }
}

CsaIssuer::IssuerAction CsaIssuer::tick(uint64_t now_us) {
    IssuerAction a;
    switch (state_) {
        case State::kAnnounce:
            if (now_us >= next_copy_us_) {
                a.kind = IssuerAction::Kind::kSendCopy;
                a.pkt = tmpl_;
                a.pkt.csa_seq = copies_left_;
                const uint64_t left_us =
                    switch_at_us_ > now_us ? switch_at_us_ - now_us : 1000;
                a.pkt.dt_to_switch_ms = static_cast<uint16_t>(
                    left_us / 1000 == 0 ? 1 : left_us / 1000);
                if (const auto m = mac_of(a.pkt, policy_.psk)) {
                    a.pkt.csa_mac = *m;
                }
                next_copy_us_ = now_us + kCopySpacingUs;
                if (--copies_left_ == 0) {
                    state_ = State::kAwaitAck;
                }
            }
            break;
        case State::kAwaitAck:
            // §11.6: commit only after CSA_ARMED, at T_switch. No flag by
            // ack_timeout → abort and stay (the craft that DID arm reverts on
            // its own verify timeout and reconverges on prev_chan).
            if (armed_seen_ && now_us >= switch_at_us_) {
                a.kind = IssuerAction::Kind::kCommit;
                a.chan_mhz = tmpl_.target_chan;
                a.bw = tmpl_.target_bw;
                a.fast = tmpl_.retune_class == 0;
                a.power_intent = tmpl_.power_intent;
                verify_deadline_us_ =
                    now_us +
                    static_cast<uint64_t>(policy_.verify_timeout_ms) * 1000;
                state_ = State::kVerify;
            } else if (!armed_seen_ &&
                       now_us - started_us_ >=
                           static_cast<uint64_t>(policy_.ack_timeout_ms) *
                               1000) {
                a.kind = IssuerAction::Kind::kAbort;
                state_ = State::kIdle;
            }
            break;
        case State::kVerify:
            if (now_us >= verify_deadline_us_) {
                // Issuer revert-on-no-video (§11.6 backstop, also covers a
                // forged CSA_ARMED making us commit to a ghost).
                a.kind = IssuerAction::Kind::kRevert;
                a.chan_mhz = tmpl_.prev_chan;
                a.bw = tmpl_.prev_bw;
                a.fast = tmpl_.retune_class == 0;
                a.power_intent = tmpl_.power_intent;
                state_ = State::kIdle;
            }
            break;
        case State::kIdle:
            break;
    }
    return a;
}

const char* CsaIssuer::state_str() const {
    switch (state_) {
        case State::kIdle:
            return "IDLE";
        case State::kAnnounce:
            return "ANNOUNCE";
        case State::kAwaitAck:
            return "AWAIT_ACK";
        case State::kVerify:
            return "VERIFY";
    }
    return "?";
}

}  // namespace wblink
