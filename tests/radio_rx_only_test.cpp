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

    // --- §15.2 (Pass 195) the auto form's PRE-USB refusals -----------------
    // Auto assigns roles only after bring-up, so the role-count checks above
    // cannot run at entry. What still can, and must, is the rx-only leg: with
    // allow_rx_only the election will produce no tx adapter however the
    // ranking comes out, so every TX-die knob is refused here — before a
    // single device is opened, which is what keeps this file hermetic.
    {
        RadioAirCfg base;
        base.auto_cfg.enabled = true;
        base.auto_cfg.channel_mhz = 5805;
        base.allow_rx_only = true;

        RadioAirCfg cfg = base;
        cfg.ack_responder = true;
        CHECK(fails_containing(cfg, "air.ack_responder"));
        CHECK(fails_containing(cfg, "uplink-free archetype"));

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
        cfg.mcs_probe = true;
        CHECK(fails_containing(cfg, "air.mcs_probe"));

        // A bad channel is caught before any libusb work too — the auto form
        // has no per-stanza channel for the bring-up loop to validate later.
        cfg = base;
        cfg.auto_cfg.channel_mhz = 1234;
        CHECK(fails_containing(cfg, "bad channel"));

        // The two §15.2 forms are exclusive: a caller that filled in both has
        // a bug, and silently preferring one of them would hide it.
        cfg = base;
        cfg.adapters = {adapter("ear0", Role::kRx)};
        CHECK(fails_containing(cfg, "forms are exclusive"));
    }

    // An EMPTY adapters array is still its own error — auto is what licenses
    // an empty one, and nothing else does.
    {
        RadioAirCfg cfg;
        cfg.allow_rx_only = true;
        CHECK(fails_containing(cfg, "no adapters"));
    }

    // §15.2 (Pass 195): auto with an fd device set. The fd list IS the device
    // set under auto — no enumeration, no parallel-array rule — so a bogus fd
    // exercises the skip path to exhaustion and must report the AUTO error,
    // not the array form's "adapter_fds must be empty ... or exactly as long
    // as adapters". Hermetic: fd -2 is rejected before any libusb call.
    {
        RadioAirCfg cfg;
        cfg.auto_cfg.enabled = true;
        cfg.auto_cfg.channel_mhz = 5805;
        cfg.allow_rx_only = true;
        cfg.adapter_fds = {-2};
        CHECK(fails_containing(cfg, "only -1 means"));
        CHECK(!fails_containing(cfg, "adapter_fds must be empty"));
    }

    return wbtest_finish("radio_rx_only_test");
}
