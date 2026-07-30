// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: remote vehicle command engines (PROTOCOL.md §11.7).
//
// Two pure state machines in the µs domain, shaped like the §11 CSA engines
// (csa.h): time is injected, nothing touches a radio.
//
//  - VcmdCraft: validates commands (§11.4-style MAC / per-(originator,session)
//    nonce monotonicity / §11.5a bound-issuer-only / rate-limit), applies them
//    through a caller-supplied hook (false = REJECTED echo — unknown cmd_id,
//    bad arg, unconfigured actuator), and emits the echo-ACK burst. A
//    duplicate nonce re-echoes the remembered outcome without re-applying
//    (§11.7 idempotent retry); anything from a non-bound sender is a silent
//    drop (no probe oracle).
//  - VcmdIssuer (ground): sends `copies` MAC'd copies of one nonce, awaits a
//    MAC-valid matching echo, retries the SAME nonce up to retry_cap
//    campaigns, then reports timeout. Terminal state is polled via §15.5
//    GET /api/v1/vehicle/command.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "wblink/wire.h"

namespace wblink {

// §15.2 policy.cmd — all §17 RE-DERIVE seeds.
struct VcmdParams {
    std::vector<uint8_t> psk;  // §11.4a provenance — same key as CSA
    uint8_t copies = 3;
    uint32_t copy_interval_ms = 20;
    uint8_t echo_copies = 2;
    uint32_t ack_timeout_ms = 1000;
    uint8_t retry_cap = 3;          // campaigns total on one nonce
    uint32_t min_interval_ms = 250; // craft accept / issuer start spacing
};

class VcmdCraft {
  public:
    // apply(cmd_id, cmd_arg) actuates an accepted command; false = the
    // command is understood-but-unapplicable (or unknown) → REJECTED echo.
    using Apply = std::function<bool(uint8_t cmd_id, uint8_t cmd_arg)>;

    VcmdCraft(const VcmdParams& policy, const CommonPrefix& self);

    // One received VEHICLE_CMD (echoes are ignored here — a craft never acts
    // on kAck). bound_issuer = the §11.5a latched command source (nullopt =
    // unclaimed → silent drop). Returns true iff a NEW command was accepted
    // and applied (the echo burst queues in both the applied and REJECTED
    // cases; duplicates re-queue the remembered outcome).
    bool on_cmd(const VehicleCmd& pkt, uint64_t now_us,
                std::optional<uint16_t> bound_issuer, const Apply& apply);

    // Next echo copy to inject (already MAC'd), spaced copy_interval_ms.
    std::optional<VehicleCmd> tick(uint64_t now_us);

    // §11.4a runtime pairing re-key (Pass 113): new pairing epoch — clears
    // the anti-replay map and any pending echo burst.
    void set_psk(std::vector<uint8_t> psk) {
        policy_.psk = std::move(psk);
        last_applied_.clear();
        echoes_left_ = 0;
    }

    uint32_t last_nonce() const { return last_.nonce; }

  private:
    struct Outcome {
        uint32_t nonce = 0;
        uint8_t cmd_id = 0;
        uint8_t cmd_arg = 0;
        bool rejected = false;
    };

    void queue_echo(const Outcome& o, uint16_t to, uint64_t now_us);

    VcmdParams policy_;
    CommonPrefix self_;
    // §11.7 anti-replay: last consumed nonce per (originator, session).
    std::map<std::pair<uint16_t, uint32_t>, Outcome> last_applied_;
    Outcome last_{};               // most recent outcome (stats)
    uint64_t last_accept_us_ = 0;  // rate-limit anchor (0 = never)

    // Pending echo burst.
    VehicleCmd echo_{};
    uint8_t echoes_left_ = 0;
    uint64_t next_echo_us_ = 0;
    uint32_t last_echo_nonce_ = 0;
    uint64_t last_echo_start_us_ = 0;  // burst holddown (min_interval_ms)
    bool echoed_once_ = false;
};

class VcmdIssuer {
  public:
    explicit VcmdIssuer(const VcmdParams& policy);

    // §15.5a re-key alongside the CSA issuer (same provenance rules).
    // Rejected while a campaign is in flight.
    bool set_psk(std::vector<uint8_t> psk);

    // §3.14: the nonce domain starts at a random 32-bit value per issuer
    // session (cross-session echo replay defence). The core stays RNG-free —
    // the app seeds this once at boot from its session entropy.
    void seed_nonce(uint32_t n) { next_nonce_ = n; }

    // Begin a campaign toward the bound craft. false = rejected (campaign
    // active, no PSK, bad arg, rate-limit).
    bool start(const CommonPrefix& prefix, uint16_t target_originator,
               uint8_t cmd_id, uint8_t cmd_arg, uint64_t now_us);

    // A received VEHICLE_CMD with kAck from the air: MAC-verified and matched
    // against the in-flight campaign (sender, nonce, cmd, arg).
    void on_echo(const VehicleCmd& pkt, uint64_t now_us);

    struct Action {
        enum class Kind : uint8_t { kNone, kSendCopy };
        Kind kind = Kind::kNone;
        VehicleCmd pkt{};
    };
    Action tick(uint64_t now_us);

    bool active() const { return state_ == State::kSending ||
                                 state_ == State::kAwaitEcho; }
    // §15.5 GET /api/v1/vehicle/command: idle|pending|acked|rejected|timeout.
    const char* state_str() const;
    uint32_t nonce() const { return tmpl_.cmd_nonce; }
    uint8_t cmd_id() const { return tmpl_.cmd_id; }
    uint8_t cmd_arg() const { return tmpl_.cmd_arg; }

  private:
    enum class State : uint8_t {
        kIdle,       // never started
        kSending,    // copies going out
        kAwaitEcho,  // copies done, waiting for the echo / retry timer
        kAcked,      // terminal (until the next start)
        kRejected,   // terminal: craft echoed REJECTED
        kTimeout,    // terminal: retry_cap campaigns, no echo
    };

    VcmdParams policy_;
    State state_ = State::kIdle;
    uint32_t next_nonce_ = 1;  // strictly increasing per (originator, session)
    uint64_t last_start_us_ = 0;

    VehicleCmd tmpl_{};          // campaign fields; seq/mac stamped per copy
    uint16_t target_ = 0;        // bound craft originator (echo match)
    uint8_t copies_left_ = 0;
    uint8_t campaigns_left_ = 0;
    uint64_t next_copy_us_ = 0;
    uint64_t echo_deadline_us_ = 0;
};

}  // namespace wblink
