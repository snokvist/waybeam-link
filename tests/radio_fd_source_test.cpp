// SPDX-License-Identifier: GPL-2.0-or-later
// B1/B4 (Phase 1b) wrapped-fd device source: the pre-USB validation of
// RadioAir::create. Everything here returns before any libusb work, so the
// test is hermetic — same discipline as radio_rx_only_test. Actually opening
// a wrapped fd needs a dongle and a caller that can produce one (unrooted
// Android's UsbManager), and stays a bench check.
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

// A one-TX config with the fd path armed the way a consumer would arm it.
RadioAirCfg fd_base() {
    RadioAirCfg cfg;
    cfg.adapters = {adapter("uplink", Role::kTx), adapter("ear", Role::kRx)};
    cfg.adapter_fds = {7, 8};
    cfg.do_reset = false;  // mandatory on the fd path (B4)
    return cfg;
}

}  // namespace

int main() {
    // --- defaults are exactly today's behaviour --------------------------
    // No fds, reset on, devourer's own lock dir. A JSON-driven daemon never
    // sets any of the three, so it must not be able to notice they exist.
    {
        RadioAirCfg cfg;
        CHECK(cfg.adapter_fds.empty());
        CHECK(cfg.do_reset);
        CHECK(cfg.lock_dir.empty());
    }

    // --- adapter_fds must be empty or exactly parallel to adapters -------
    // A short vector would silently enumerate the tail, which is the kind of
    // "loads clean, flies wrong" the config surface already suffers from.
    {
        RadioAirCfg cfg;
        cfg.adapters = {adapter("uplink", Role::kTx), adapter("ear", Role::kRx)};
        cfg.do_reset = false;
        cfg.adapter_fds = {7};
        CHECK(fails_containing(cfg, "adapter_fds must be empty"));
        cfg.adapter_fds = {7, 8, 9};
        CHECK(fails_containing(cfg, "adapter_fds must be empty"));
    }

    // --- -1 is "enumerate this stanza", and an all--1 vector is not fds ---
    // It must NOT trip the do_reset refusal: nothing here is fd-supplied, so
    // the run is the shipped enumeration path and reaches libusb.
    {
        RadioAirCfg cfg;
        cfg.adapters = {adapter("uplink", Role::kTx)};
        cfg.adapter_fds = {-1};
        CHECK(fails_containing(cfg, "no matching Realtek device"));
    }

    // --- an fd-supplied stanza cannot also carry a bus pin ---------------
    {
        RadioAirCfg cfg = fd_base();
        cfg.adapters[1].bus = "1-1.2";
        CHECK(fails_containing(cfg, "cannot be honoured"));
        // A mac pin is fine on the same stanza: identity comes off the die,
        // so the §15.2 re-bind still works for a wrapped fd.
        cfg.adapters[1].bus.clear();
        cfg.adapters[1].mac = "aa:bb:cc:dd:ee:ff";
        CHECK(fails_containing(cfg, "libusb_wrap_sys_device"));
    }

    // --- the same fd cannot back two adapters ----------------------------
    {
        RadioAirCfg cfg = fd_base();
        cfg.adapter_fds = {7, 7};
        CHECK(fails_containing(cfg, "supplied for two adapters"));
    }

    // --- B4: reset and wrapped fd are mutually exclusive, and refused ----
    // The reset re-enumerates and orphans the caller's fd. Refusing beats
    // overriding: a caller that asked for a reset gets told, not downgraded.
    {
        RadioAirCfg cfg = fd_base();
        cfg.do_reset = true;  // the default — so this is the easy mistake
        CHECK(fails_containing(cfg, "do_reset must be false"));
        // Mixed sources are allowed, but one fd is enough to bind the rule.
        cfg.adapter_fds = {-1, 8};
        CHECK(fails_containing(cfg, "do_reset must be false"));
    }

    // --- role rules are unchanged by the device source -------------------
    // The fd path must serve a role:"tx" adapter (operator ruling
    // 2026-08-08: Android is a full TX/RX node), so §3.11's counting has to
    // apply to it exactly as to an enumerated one.
    {
        RadioAirCfg cfg = fd_base();
        cfg.adapters = {adapter("a", Role::kTx), adapter("b", Role::kTx)};
        CHECK(fails_containing(cfg, "exactly one"));

        cfg = fd_base();
        cfg.adapters = {adapter("ear0", Role::kRx), adapter("ear1", Role::kRx)};
        CHECK(fails_containing(cfg, "exactly one"));
        cfg.allow_rx_only = true;  // spectator over fds is the other archetype
        CHECK(fails_containing(cfg, "libusb_wrap_sys_device"));

        // ...and a TX-die knob still fails closed with no TX die under it.
        cfg.ldpc = true;
        CHECK(fails_containing(cfg, "air.ldpc"));
    }

    // --- validation order: the fd rules precede any libusb work ----------
    // A bad fd vector on a config that would also fail role counting must
    // report the role error, since that check runs first and is cheaper to
    // act on. Guards against a future edit hoisting the fd block too early.
    {
        RadioAirCfg cfg;
        cfg.adapters = {adapter("a", Role::kTx), adapter("b", Role::kTx)};
        cfg.adapter_fds = {7};
        CHECK(fails_containing(cfg, "exactly one"));
    }

    return wbtest_finish("radio_fd_source_test");
}
