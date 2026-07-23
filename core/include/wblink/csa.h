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
//  - CsaIssuer (ground): N=5 decrementing-dt copies @ 20 ms, MAC'd; commits
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
    std::vector<uint8_t> psk;  // empty = spectator (unauthenticated follow)
    uint32_t settle_ms = 3000;          // §11.3 adaptive freeze
    uint32_t verify_timeout_ms = 150;   // §11.5 default; wire t_revert_ms wins
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
    uint64_t freeze_until_us() const { return freeze_until_us_; }  // §11.3
    // The issuer this follower latched onto (established by a MAC-valid CSA).
    std::optional<uint16_t> latched_issuer() const { return latched_; }
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
    // Valid craft video/data seen (only meaningful in VERIFY, after commit).
    void note_craft_video(uint64_t now_us);

    struct IssuerAction {
        enum class Kind : uint8_t {
            kNone,
            kSendCopy,    // inject pkt (already MAC'd)
            kCommit,      // retune own adapters to chan/bw
            kSendBeacon,  // §11.6 rendezvous beacon (already MAC'd, dt=0)
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
        kAwaitAck,  // copies done, waiting for CSA_ARMED / T_switch
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
    bool armed_seen_ = false;
};

}  // namespace wblink
