// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: follow-me CSA engines (PROTOCOL.md §11).
//
// Two pure state machines in the µs domain (host steady-clock supplied by the
// caller, one timestamp per loop iteration):
//
//  - CsaFollower (craft + spectators): validates campaigns (§11.4 MAC /
//    anti-replay / allowlist / rate-limit), anchors T_switch on the hardware
//    TSF of the received copy (§11.2, u32 wrap-safe, host-arrival fallback),
//    and walks IDLE → ARMED → retune → VERIFY → COMMITTED. COMMITTED holds
//    until reboot (§11.5, Pass 59): the only backout is the VERIFY→prev_chan
//    jump-failed revert; the §11.5a command-source binding releases after
//    bind_release_ms of issuer silence with no channel change.
//  - CsaIssuer (ground): dt-stamped copies @ 20 ms repeated until the craft's
//    CSA_ARMED ack or T_switch (§11.2, Pass 90), each MAC'd; commits
//    its own retune immediately on the craft's CSA_ARMED flag (§11.6
//    pre-position, Pass 69), then re-injects the campaign as zero-dt
//    rendezvous beacons until it hears craft video; aborts on ack timeout,
//    reverts on no craft video by max(T_switch, landing) + verify_timeout.
//
// Neither touches a radio: they emit Actions the app maps onto RadioAir
// (FastRetune / SetMonitorChannel + ReApplyTxPower) — or onto nothing, for
// the udp-air dev backend. Selector freeze (§11.3) is exported as
// freeze_until_us() for the app to feed Selector::csa_freeze().
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "wblink/wire.h"

namespace wblink {

// §15.2 policy.csa — all times ms except the channel fields (MHz).
struct CsaParams {
    std::vector<uint8_t> psk;  // §11.4a key; empty = fault unless spectator
    // §11.4a (Pass 85): permission to follow an unauthenticated CSA is a ROLE
    // property (§15.2 node.spectator), carried explicitly. It is deliberately
    // NOT inferred from an empty psk: craft/ground with no key are faulted,
    // not unauthenticated, and must fail closed.
    bool allow_unauthenticated = false;
    uint32_t settle_ms = 3000;          // §11.3 adaptive freeze
    // §11.5 ceiling; t_revert_ms may only shorten. Pass 89: 150 -> 500. The old
    // value was a median + margin; the follower reverts on the TAIL of the
    // issuer's landing delay, measured max 143 ms over 8 hops against a 150 ms
    // window. 500 is ~3.5x that max and sits inside the 750 ms RX-liveness guard.
    uint32_t verify_timeout_ms = 500;
    uint32_t min_interval_ms = 5000;    // §11.4 rate-limit
    uint32_t ack_timeout_ms = 1000;     // §11.6 CSA_ARMED wait
    uint32_t bind_release_ms = 90000;   // §11.5a command-source binding release
    std::vector<uint16_t> allowlist;  // MHz; empty = reject all (fail-closed)
};

// What the caller must do to a radio right now. kNone otherwise.
struct CsaAction {
    enum class Kind : uint8_t { kNone, kRetune, kRevert };
    Kind kind = Kind::kNone;
    uint16_t chan_mhz = 0;
    uint8_t bw = 0;           // §11.1 encoding: 0=20 1=40 2=80
    bool fast = false;        // retune_class 0 → FastRetune path
    uint8_t power_intent = 0; // §11.1 profile power level to re-apply
};

class CsaFollower {
  public:
    explicit CsaFollower(const CsaParams& policy);

    // One received CSA copy. rx_tsfl_us = the copy's hardware receive TSF
    // (low 32 bits); tsf_now_us = a TSF read taken this loop iteration on the
    // SAME adapter (nullopt → host-arrival fallback, §11.2). latched_issuer =
    // the node's current command source (nullopt = none latched yet; a
    // MAC-valid CSA may establish the binding, §11.4). Returns true iff the
    // campaign was accepted (first copy only — later copies of the same
    // campaign fail the strictly-greater nonce test by design).
    bool on_csa(const CsaPacket& pkt, uint64_t now_us,
                std::optional<uint64_t> tsf_now_us, uint32_t rx_tsfl_us,
                std::optional<uint16_t> latched_issuer);

    // Any structurally valid waybeam-link traffic arrived (post-§3.0-filter).
    // `from_originator` is the sender in the frame's common prefix: traffic
    // from the bound issuer refreshes the §11.5a binding; any valid traffic
    // confirms a pending switch (VERIFY → COMMITTED).
    void note_valid_rx(uint64_t now_us, uint16_t from_originator);

    CsaAction tick(uint64_t now_us);

    bool armed() const { return state_ == State::kArmed; }  // §11.6 data flag
    // §11.6/§3.2 bit 4 (Pass 89): CSA_ARMED spans the WHOLE campaign — set on
    // accept, cleared only on COMMITTED. Its clearing on target_chan is the
    // craft's commit proof; clearing it at the switch (as before Pass 89) made
    // "arrived, still deciding" indistinguishable from "committed" to the issuer.
    bool campaign_active() const {
        return state_ == State::kArmed || state_ == State::kVerify;
    }
    uint64_t freeze_until_us() const { return freeze_until_us_; }  // §11.3
    // The issuer this follower latched onto (established by a MAC-valid CSA).
    std::optional<uint16_t> latched_issuer() const { return latched_; }
    // §11.4a fail-closed rejections (empty key, non-spectator).
    uint64_t unauth_rejected() const { return unauth_rejected_; }
    const char* state_str() const;

  private:
    enum class State : uint8_t {
        kIdle,       // no campaign
        kArmed,      // accepted, waiting for T_switch
        kVerify,     // retuned, waiting for valid traffic
        kCommitted,  // traffic seen on the new channel — holds until reboot
    };

    CsaParams policy_;
    State state_ = State::kIdle;
    std::optional<uint16_t> latched_;
    uint64_t unauth_rejected_ = 0;
    // §11.4 anti-replay: last accepted nonce per (originator, session).
    std::map<std::pair<uint16_t, uint32_t>, uint32_t> last_applied_;
    uint64_t last_accept_us_ = 0;  // rate-limit anchor (0 = never)

    // Accepted campaign.
    CsaPacket campaign_{};
    uint64_t switch_at_us_ = 0;
    uint64_t verify_deadline_us_ = 0;
    uint64_t freeze_until_us_ = 0;

    // §11.5a binding freshness: last time the bound issuer was heard.
    uint64_t last_bound_rx_us_ = 0;
};

class CsaIssuer {
  public:
    explicit CsaIssuer(const CsaParams& policy);

    // §15.5a claim re-key: swap the CSA PSK (a cached announced token per §11.4a,
    // or the configured secret) between campaigns. Rejected while a campaign is
    // active so an in-flight campaign keeps a single key across its copies. The
    // monotonic nonce carries across re-keys, so one long-lived issuer safely
    // commands different crafts in turn (each craft anti-replays on the issuer's
    // originator/session, §11.4). Returns false if a campaign is active.
    bool set_psk(std::vector<uint8_t> psk);

    // Begin a campaign. prev_chan/prev_bw = the CURRENT operating channel
    // (the revert target); power_intent = the current §9 profile power level.
    // false = rejected (campaign active, no PSK, rate-limit, allowlist).
    bool start(const CommonPrefix& prefix, uint16_t target_chan_mhz,
               uint8_t target_bw, uint8_t retune_class, uint16_t prev_chan_mhz,
               uint8_t prev_bw, uint8_t power_intent, uint64_t now_us);

    // Craft's CSA_ARMED data flag observed (from the latched craft).
    void note_craft_armed(uint64_t now_us);
    // Valid craft video/data seen (only meaningful in VERIFY, after commit;
    // ignored before T_switch — the craft cannot be on the target yet,
    // §11.6 review pass 2). `craft_armed` = the frame's CSA_ARMED flag: a SET
    // bit means the craft arrived but has not committed, and MUST NOT satisfy
    // video-verify (§11.6 Pass 89) — the craft transmits throughout its own
    // VERIFY window and may still revert.
    void note_craft_video(uint64_t now_us, bool craft_armed);
    // §11.2 (Pass 90): re-stamp a copy that was held for the craft's §7.2
    // quiet gap, at the instant it actually goes on air — dt_to_switch_ms
    // recomputed from the absolute T_switch and csa_mac recomputed over it.
    // Returns false once T_switch has passed: the copy is then dropped, never
    // transmitted stale (a stale dt would place the follower's switch late by
    // the hold time, since it anchors on the copy's receive TSF).
    bool restamp_copy(CsaPacket& pkt, uint64_t now_us) const;

    // The app's commit retune failed — abandon the campaign rather than
    // verify with untrusted ears (§11.6 review pass 2). The armed craft
    // reverts on its own verify timeout.
    void note_commit_failed();

    struct IssuerAction {
        enum class Kind : uint8_t {
            kNone,
            kSendCopy,    // inject pkt; MAC'd for NOW, so a copy
                          // held for the §7.2 gap must be restamp_copy()'d
            kCommit,      // retune own adapters to chan/bw
            kSendBeacon,  // §11.6 rendezvous beacon (already MAC'd, dt=0)
            kSuccess,     // campaign confirmed at the deadline (beacon tail)
            kRevert,      // retune own adapters back to prev
            kAbort,       // no CSA_ARMED — stay, campaign dead
        };
        Kind kind = Kind::kNone;
        CsaPacket pkt{};
        uint16_t chan_mhz = 0;
        uint8_t bw = 0;
        bool fast = false;
        uint8_t power_intent = 0;
    };
    IssuerAction tick(uint64_t now_us);

    bool active() const { return state_ != State::kIdle; }
    const char* state_str() const;

  private:
    enum class State : uint8_t {
        kIdle,
        kAnnounce,  // copies still going out
        kAwaitAck,  // initial burst out; copies still repeat here
                    // until CSA_ARMED / T_switch (§11.2 Pass 90)
        kVerify,    // committed, waiting for craft video
    };

    static constexpr uint8_t kCopies = 5;
    static constexpr uint32_t kCopySpacingUs = 20000;  // §11.2

    CsaParams policy_;
    State state_ = State::kIdle;
    uint32_t next_nonce_ = 1;  // strictly increasing per (originator, session)
    uint64_t last_campaign_us_ = 0;

    CommonPrefix prefix_{};
    CsaPacket tmpl_{};  // campaign fields; dt/csa_seq/mac stamped per copy
    uint8_t copies_left_ = 0;
    uint64_t next_copy_us_ = 0;
    uint64_t started_us_ = 0;
    uint64_t switch_at_us_ = 0;
    uint64_t verify_deadline_us_ = 0;
    uint64_t next_beacon_us_ = 0;  // §11.6 rendezvous beacon cadence
    // §11.2: stamp dt_to_switch_ms from the absolute T_switch and re-MAC.
    void stamp_copy(CsaPacket& pkt, uint64_t now_us) const;

    bool armed_seen_ = false;
    bool video_seen_ = false;  // latched in VERIFY; campaign closes at the
                               // deadline either way (§11.6 beacon tail)
};

}  // namespace wblink
