// SPDX-License-Identifier: GPL-2.0-or-later
// §3.11 (Pass 162) RX-only devourer bring-up: the pre-USB validation of
// RadioAir::create — role counting and the fail-closed TX-knob refusals.
// Everything here returns before any libusb work, so the test is hermetic;
// a successful RX-only bring-up needs hardware and stays a bench check.
#include <string>

#include "wblink/air_radio.h"
#include "wblink/config.h"
#include "wbtest.h"

using namespace wblink;

namespace {

AdapterCfg adapter(const char* name, Role role) {
    AdapterCfg a;
    a.name = name;
    a.role = role;
    a.channel_mhz = 5805;
    return a;
}

bool fails_containing(const RadioAirCfg& cfg, const char* needle) {
    auto r = RadioAir::create(cfg);
    if (r) return false;  // must fail — a pass here would open USB
    return r.error.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    // --- no adapters at all is its own error, rx-only or not ---------------
    {
        RadioAirCfg cfg;
        CHECK(fails_containing(cfg, "no adapters"));
        cfg.allow_rx_only = true;
        CHECK(fails_containing(cfg, "no adapters"));
    }

    // --- more than one role:"tx" is a config error on every shape (§6.4) ---
    {
        RadioAirCfg cfg;
        cfg.adapters = {adapter("a", Role::kTx), adapter("b", Role::kTx)};
        CHECK(fails_containing(cfg, "exactly one"));
        cfg.allow_rx_only = true;  // the flag never licenses a second uplink
        CHECK(fails_containing(cfg, "exactly one"));
    }

    // --- zero tx refuses unless the archetype vouches (allow_rx_only) ------
    {
        RadioAirCfg cfg;
        cfg.adapters = {adapter("ear0", Role::kRx), adapter("ear1", Role::kRx)};
        CHECK(fails_containing(cfg, "exactly one"));
    }

    // --- fail closed: each TX-die knob refuses an RX-only bring-up ---------
    {
        RadioAirCfg base;
        base.adapters = {adapter("ear0", Role::kRx)};
        base.allow_rx_only = true;

        RadioAirCfg cfg = base;
        cfg.ack_responder = true;
        CHECK(fails_containing(cfg, "air.ack_responder"));

        cfg = base;
        cfg.unicast_returns = true;
        CHECK(fails_containing(cfg, "policy.return.unicast"));

        cfg = base;
        cfg.ldpc = true;
        CHECK(fails_containing(cfg, "air.ldpc"));

        cfg = base;
        cfg.stbc = true;
        CHECK(fails_containing(cfg, "air.stbc"));

        cfg = base;
        cfg.mcs_probe = true;  // §9.4 Pass 163: probing is a TX-die property
        CHECK(fails_containing(cfg, "air.mcs_probe"));
    }

    return wbtest_finish("radio_rx_only_test");
}
