// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: CSA engines (PROTOCOL.md §11) — see csa.h.
#include "wblink/csa.h"

#include <algorithm>

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
    // §11.4a (Pass 85): an absent key is a FAULT, not a mode. For craft/ground
    // both sources are exhaustive (secret configured, or announced token
    // self-generated at boot), so an empty key means a missed ANNOUNCE, a
    // config typo or failed token generation — fail closed. Only a §15.2
    // spectator may follow unauthenticated, and that permission is carried
    // explicitly by role: never inferred from an empty key.
    if (policy_.psk.empty()) {
        if (!policy_.allow_unauthenticated) {
            ++unauth_rejected_;
            return false;
        }
    } else {
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
    // §11.6 rendezvous beacon (Pass 69): dt == 0 never arms. A MAC-valid
    // copy of the currently armed campaign confirms a pending VERIFY exactly
    // like valid traffic; in any other state it is a silent drop with no
    // side effects (no §11.5a binding refresh — a recorded beacon must not
    // hold the binding alive).
    if (pkt.dt_to_switch_ms == 0) {
        if (state_ == State::kVerify &&
            pkt.prefix.originator == campaign_.prefix.originator &&
            pkt.prefix.session_id == campaign_.prefix.session_id &&
            pkt.csa_nonce == campaign_.csa_nonce) {
            state_ = State::kCommitted;
            last_bound_rx_us_ = now_us;
        }
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

    // Accept. §11.2 TSF anchor: elapsed since the copy left the air, from the
    // SAME adapter's TSF; no TSF read → host-arrival approximation.
    last_applied_[key] = pkt.csa_nonce;
    last_accept_us_ = now_us;
    latched_ = pkt.prefix.originator;
    last_bound_rx_us_ = now_us;  // §11.5a: start the binding-freshness clock
    campaign_ = pkt;
    const uint64_t dt_us = static_cast<uint64_t>(pkt.dt_to_switch_ms) * 1000;
    uint64_t elapsed = 0;
    if (tsf_now_us) {
        elapsed = static_cast<uint32_t>(static_cast<uint32_t>(*tsf_now_us) -
                                        rx_tsfl_us);
        // A fresh copy's air+host transit is sub-ms and can never legitimately
        // exceed the copy's own remaining window (dt_us). A larger value is a
        // reset/garbage TSF delta — discard it, else it collapses switch_at_us_
        // to now_us and retunes up to dt early, ahead of the issuer (§11.2).
        if (elapsed > dt_us) {
            elapsed = 0;
        }
    }
    switch_at_us_ = now_us + (dt_us > elapsed ? dt_us - elapsed : 0);
    freeze_until_us_ =
        now_us + static_cast<uint64_t>(policy_.settle_ms) * 1000;  // §11.3
    state_ = State::kArmed;
    return true;
}

void CsaFollower::note_valid_rx(uint64_t now_us, uint16_t from_originator) {
    if (state_ == State::kVerify) {
        state_ = State::kCommitted;  // §11.5 valid traffic confirms the switch
    }
    // §11.5a: any packet the craft accepts from the bound issuer refreshes the
    // command-source binding (CSA / NACK / LINK_REPORT / HEARTBEAT alike).
    if (latched_ && from_originator == *latched_) {
        last_bound_rx_us_ = now_us;
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
                // §11.5 (Pass 69 H1b): the verify window opens at LANDING —
                // the first tick after the blocking retune — computed lazily
                // in kVerify (0 = not yet landed), so the retune itself
                // cannot burn the window from the inside.
                verify_deadline_us_ = 0;
                state_ = State::kVerify;
            }
            break;
        case State::kVerify:
            if (verify_deadline_us_ == 0) {
                // §11.5 (Pass 86): the issuer may SHORTEN the window, never
                // lengthen it. t_revert_ms is u16 — taken unclamped, one frame
                // strands a follower 65 s on a channel it cannot hear. How
                // long a node is willing to be deaf is the node's decision.
                const uint32_t vt =
                    std::min(campaign_.t_revert_ms != 0
                                 ? campaign_.t_revert_ms
                                 : policy_.verify_timeout_ms,
                             policy_.verify_timeout_ms);
                verify_deadline_us_ = now_us + static_cast<uint64_t>(vt) * 1000;
            }
            if (now_us >= verify_deadline_us_) {
                // §11.5 jump-failed backout: the retune landed on a dead
                // channel — revert to prev_chan, drop the incomplete claim,
                // return to IDLE. No mid-flight rendezvous (Pass 59).
                a.kind = CsaAction::Kind::kRevert;
                a.chan_mhz = campaign_.prev_chan;
                a.bw = campaign_.prev_bw;
                a.fast = campaign_.retune_class == 0;
                a.power_intent = campaign_.power_intent;
                latched_ = std::nullopt;
                state_ = State::kIdle;
            }
            break;
        case State::kCommitted:
            // §11.5a: hold the channel until reboot. Release only the binding
            // after bind_release_ms of silence from the bound issuer — no
            // channel change; the craft re-opens for in-place re-claim.
            if (latched_ &&
                now_us - last_bound_rx_us_ >
                    static_cast<uint64_t>(policy_.bind_release_ms) * 1000) {
                latched_ = std::nullopt;
            }
            break;
        case State::kIdle:
            break;
    }
    return a;
}

void CsaFollower::set_psk(std::vector<uint8_t> psk) {
    policy_.psk = std::move(psk);
    latched_ = std::nullopt;
    last_applied_.clear();
    campaign_ = CsaPacket{};
    state_ = State::kIdle;
}

void CsaFollower::sync_channel(uint16_t chan_mhz) {
    campaign_ = CsaPacket{};
    campaign_.prev_chan = chan_mhz;
    campaign_.target_chan = chan_mhz;
    state_ = State::kIdle;
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
    }
    return "?";
}

// ---- issuer -----------------------------------------------------------------

CsaIssuer::CsaIssuer(const CsaParams& policy) : policy_(policy) {}

bool CsaIssuer::set_psk(std::vector<uint8_t> psk) {
    if (state_ != State::kIdle) {
        return false;  // never swap the key mid-campaign (§15.5a re-key)
    }
    policy_.psk = std::move(psk);
    return true;
}

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
    // §11.2 (Pass 91): class 0 is 300 ms, not 150. The budget must hold both
    // the copy window and the 50 ms ack-lead cutoff; at 150 ms those conflict
    // and the window collapses to roughly the pre-Pass-90 burst.
    const uint32_t dt0_ms = retune_class == 0 ? 300 : 500;
    started_us_ = now_us;
    switch_at_us_ = now_us + static_cast<uint64_t>(dt0_ms) * 1000;
    copies_left_ = kCopies;
    next_copy_us_ = now_us;
    armed_seen_ = false;
    video_seen_ = false;
    landing_seen_ = false;  // §11.6 Pass 92: one re-anchor per campaign
    state_ = State::kAnnounce;
    return true;
}

void CsaIssuer::note_craft_armed(uint64_t) {
    if (state_ == State::kAnnounce || state_ == State::kAwaitAck) {
        armed_seen_ = true;
    }
}

void CsaIssuer::stamp_copy(CsaPacket& pkt, uint64_t now_us) const {
    // §11.2: dt is relative to THIS copy's transmission, derived from the
    // campaign's absolute T_switch, so every copy — first or last — resolves
    // to the same instant. Clamped to >= 1: dt == 0 marks a §11.6 beacon.
    const uint64_t left_us =
        switch_at_us_ > now_us ? switch_at_us_ - now_us : 1000;
    pkt.dt_to_switch_ms =
        static_cast<uint16_t>(left_us / 1000 == 0 ? 1 : left_us / 1000);
    if (const auto m = mac_of(pkt, policy_.psk)) {
        pkt.csa_mac = *m;  // the MAC covers dt — restamp implies re-MAC
    }
}

bool CsaIssuer::restamp_copy(CsaPacket& pkt, uint64_t now_us) const {
    // §11.2 (Pass 90): a copy held for the craft's §7.2 quiet gap must be
    // re-stamped at the instant it goes on air. The follower anchors T_switch
    // on the copy's RECEIVE TSF, so a pre-stamped copy released after a hold
    // of dt-hold places its switch that much late — desynchronising the exact
    // instant the campaign exists to agree on. Past T_switch there is nothing
    // truthful left to say: drop it rather than send it stale.
    // Same cutoff as emission (§11.2 Pass 90 addendum): the gap hold can push
    // a copy that was legal when produced past the ack-lead deadline, and a
    // copy is only worth sending if an accepting craft can still get
    // CSA_ARMED back before it departs.
    if (switch_at_us_ <= now_us + kCopyCutoffUs) {
        return false;
    }
    // The held copy must still belong to the campaign whose T_switch we are
    // about to stamp onto it. A copy held across a campaign boundary would
    // otherwise be re-stamped with the NEW campaign's dt while carrying the
    // OLD target_chan and nonce — and re-MAC'd, so it would validate. A craft
    // that missed the old campaign would then follow it to the wrong channel.
    // Unreachable at the default 5 s min_interval, but min_interval_s is
    // operator-settable and this is the injection path.
    if (state_ == State::kIdle || pkt.csa_nonce != tmpl_.csa_nonce) {
        return false;
    }
    stamp_copy(pkt, now_us);
    return true;
}

void CsaIssuer::note_craft_video(uint64_t now_us, bool craft_armed) {
    // §11.6 (Pass 89): a CSA_ARMED-set frame on target_chan proves the craft
    // ARRIVED, not that it STAYED — the craft transmits throughout its own
    // §11.5 VERIFY window, before deciding, and may still revert. Latching on
    // it lets the issuer confirm a campaign the craft abandons, holding a
    // channel the craft has left (observed 2026-07-24: issuer confirmed 5745
    // while the craft reverted to 5805). The CLEARED bit is the commit proof.
    if (craft_armed) {
        // §11.6 (Pass 92): but that same frame IS the craft's landing, and the
        // follower's §11.5 window runs from ITS landing while this one runs
        // from T_switch — so without re-anchoring the issuer stops beaconing a
        // full craft-retune-cost (measured median 48.7 / max 67.9 ms over 27
        // hops) before the craft gives up. Re-anchor once: a later ARMED frame
        // must not extend the window again.
        if (state_ == State::kVerify && now_us >= switch_at_us_ &&
            !landing_seen_) {
            landing_seen_ = true;
            verify_deadline_us_ =
                now_us + static_cast<uint64_t>(policy_.verify_timeout_ms) * 1000;
        }
        return;
    }
    // §11.6 review pass 2: nothing before T_switch can be legitimate craft
    // video on the target — the craft does not move until then; an earlier
    // frame is a stale ear (failed per-adapter retune) or RF bleed and must
    // not satisfy the backstop.
    if (state_ == State::kVerify && now_us >= switch_at_us_) {
        // §11.6 beacon tail: success is latched, not acted on — the beacons
        // keep blanketing the craft's verify window and the campaign closes
        // at the deadline (kSuccess), never early.
        video_seen_ = true;
    }
}

void CsaIssuer::note_commit_failed() {
    // §11.6 review pass 2: a failed commit retune means the issuer cannot
    // trust the position of its ears — abandon the campaign rather than
    // verify with them. The armed craft reverts on its own verify timeout.
    if (state_ == State::kVerify) {
        state_ = State::kIdle;
    }
}

CsaIssuer::IssuerAction CsaIssuer::tick(uint64_t now_us) {
    IssuerAction a;
    switch (state_) {
        case State::kAnnounce:
            // §11.2 (Pass 90 addendum): never emit a copy so close to T_switch
            // that an accepting craft cannot get CSA_ARMED back to the issuer
            // before it leaves the old channel.
            if (now_us >= next_copy_us_ &&
                switch_at_us_ > now_us + kCopyCutoffUs) {
                a.kind = IssuerAction::Kind::kSendCopy;
                a.pkt = tmpl_;
                a.pkt.csa_seq = copies_left_;
                stamp_copy(a.pkt, now_us);
                next_copy_us_ = now_us + kCopySpacingUs;
                if (--copies_left_ == 0) {
                    state_ = State::kAwaitAck;
                }
            }
            break;
        case State::kAwaitAck:
            // §11.2 (Pass 90): keep re-sending copies until the craft ACKs or
            // T_switch arrives. The old fixed 5-copy burst spanned 80 ms and
            // then went silent for the rest of ack_timeout; a craft that heard
            // none of the five lost the campaign with ~1 s of airtime unspent.
            // The ACK below is the stop condition, so this is
            // retransmit-until-acked. Checked before the ACK/timeout arms so a
            // due copy is not dropped on the tick that also commits — commit
            // wins next tick, one copy later, which is harmless.
            if (!armed_seen_ && switch_at_us_ > now_us + kCopyCutoffUs &&
                now_us >= next_copy_us_) {
                a.kind = IssuerAction::Kind::kSendCopy;
                a.pkt = tmpl_;
                a.pkt.csa_seq = 0;  // §11.1: repeats past the initial burst
                stamp_copy(a.pkt, now_us);
                next_copy_us_ = now_us + kCopySpacingUs;
                break;
            }
            // §11.6 pre-position (Pass 69): commit immediately on CSA_ARMED —
            // the copies are out, the craft has ACKed, there is no further
            // business on the old channel. Waiting for T_switch made both
            // retunes simultaneous and the issuer unhearable inside the
            // craft's verify window. No flag by ack_timeout → abort and stay
            // (the craft that DID arm reverts on its own verify timeout and
            // reconverges on prev_chan).
            if (armed_seen_) {
                a.kind = IssuerAction::Kind::kCommit;
                a.chan_mhz = tmpl_.target_chan;
                a.bw = tmpl_.target_bw;
                a.fast = tmpl_.retune_class == 0;
                a.power_intent = tmpl_.power_intent;
                // §11.6: the deadline anchors at max(T_switch, landing), and
                // "landing" is the first tick IN kVerify — the engine cannot
                // observe time during the app's blocking retunes, so it is
                // computed lazily there (0 = not yet landed), never here.
                verify_deadline_us_ = 0;
                state_ = State::kVerify;
            } else if (now_us - started_us_ >=
                       static_cast<uint64_t>(policy_.ack_timeout_ms) * 1000) {
                a.kind = IssuerAction::Kind::kAbort;
                state_ = State::kIdle;
            }
            break;
        case State::kVerify:
            if (verify_deadline_us_ == 0) {
                // §11.6 landing: the first tick after the app's blocking
                // commit retunes — the deadline and beacon cadence open here.
                verify_deadline_us_ =
                    (now_us > switch_at_us_ ? now_us : switch_at_us_) +
                    static_cast<uint64_t>(policy_.verify_timeout_ms) * 1000;
                next_beacon_us_ = now_us;
            }
            if (now_us >= verify_deadline_us_) {
                if (video_seen_) {
                    // §11.6 beacon tail complete — campaign succeeded.
                    a.kind = IssuerAction::Kind::kSuccess;
                    state_ = State::kIdle;
                    break;
                }
                // Issuer revert-on-no-video (§11.6 backstop, also covers a
                // forged CSA_ARMED making us commit to a ghost).
                a.kind = IssuerAction::Kind::kRevert;
                a.chan_mhz = tmpl_.prev_chan;
                a.bw = tmpl_.prev_bw;
                a.fast = tmpl_.retune_class == 0;
                a.power_intent = tmpl_.power_intent;
                state_ = State::kIdle;
            } else if (now_us >= next_beacon_us_) {
                // §11.6 rendezvous beacon (Pass 69): re-inject the accepted
                // campaign with csa_seq = 0 and dt = 0 (never arms, §11.4) at
                // copy spacing until the craft's video confirms the switch —
                // the guaranteed issuer-present signal inside the craft's
                // verify window.
                a.kind = IssuerAction::Kind::kSendBeacon;
                a.pkt = tmpl_;
                a.pkt.csa_seq = 0;
                a.pkt.dt_to_switch_ms = 0;
                if (const auto m = mac_of(a.pkt, policy_.psk)) {
                    a.pkt.csa_mac = *m;
                }
                next_beacon_us_ = now_us + kCopySpacingUs;
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
