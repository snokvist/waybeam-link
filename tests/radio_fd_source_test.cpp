// SPDX-License-Identifier: GPL-2.0-or-later
// B1/B4 (Phase 1b) wrapped-fd device source: the validation of
// RadioAir::create.
//
// MOST cases here return before any libusb work, like radio_rx_only_test.
// Three do not: proving that a config PASSES validation means letting it
// reach the device open. Those use an fd on /dev/null, so the failure is a
// deterministic libusb_wrap_sys_device error and nothing in the test can
// touch a USB device. **Never assert on a case that enumerates** — an
// enumerating create() opens whatever Realtek dongle the host has and, with
// do_reset at its default true, resets and brings it up. On a bench rig with
// a usbfs udev rule that is live flight hardware. The `-1 = enumerate`
// semantics are therefore covered only where a second, fd-supplied stanza
// makes the outcome observable before the open.
#include <fcntl.h>
#include <unistd.h>

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
// The fds are placeholders: every case built on this stops in validation.
RadioAirCfg fd_base() {
    RadioAirCfg cfg;
    cfg.adapters = {adapter("uplink", Role::kTx), adapter("ear", Role::kRx)};
    cfg.adapter_fds = {7, 8};
    cfg.do_reset = false;  // mandatory on the fd path (B4)
    return cfg;
}

}  // namespace

int main() {
    // A real, open, definitely-not-USB fd for the cases that reach the open.
    const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    CHECK(null_fd >= 0);

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

    // --- -1 is "enumerate this stanza"; every other negative is refused ---
    // Observable here because stanza 1 IS fd-supplied, so validation reaches
    // a verdict before anything opens.
    {
        RadioAirCfg cfg = fd_base();
        cfg.adapter_fds = {-1, 8};  // legal mix: enumerate one, wrap one
        cfg.do_reset = true;
        CHECK(fails_containing(cfg, "do_reset must be false"));

        cfg = fd_base();
        cfg.adapter_fds = {-9, 8};
        CHECK(fails_containing(cfg, "only -1 means"));
    }

    // --- an fd-supplied stanza cannot also carry a bus pin ---------------
    {
        RadioAirCfg cfg = fd_base();
        cfg.adapters[1].bus = "1-1.2";
        CHECK(fails_containing(cfg, "cannot be honoured"));
    }

    // --- ...but a mac pin on the same stanza is fine ----------------------
    // Identity is read off the die, so §15.2 re-binds a wrapped unit too.
    // The pinned stanza is index 0 so that it is the one that reaches the
    // open — otherwise this passes on stanza 0's failure and proves nothing.
    {
        RadioAirCfg cfg = fd_base();
        cfg.adapters[0].mac = "aa:bb:cc:dd:ee:ff";
        cfg.adapter_fds = {null_fd, -1};
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

        // A spectator over wrapped fds is the other supported archetype, so
        // it must clear validation and get as far as the open.
        cfg.allow_rx_only = true;
        cfg.adapter_fds = {null_fd, -1};
        CHECK(fails_containing(cfg, "libusb_wrap_sys_device"));

        // ...and a TX-die knob still fails closed with no TX die under it.
        cfg.ldpc = true;
        CHECK(fails_containing(cfg, "air.ldpc"));
    }

    // --- validation order: role counting precedes the fd rules -----------
    // A config that breaks both must report the role error, since that check
    // runs first. Guards against a future edit hoisting the fd block above it.
    {
        RadioAirCfg cfg;
        cfg.adapters = {adapter("a", Role::kTx), adapter("b", Role::kTx)};
        cfg.adapter_fds = {7};
        CHECK(fails_containing(cfg, "exactly one"));
    }

    ::close(null_fd);
    return wbtest_finish("radio_fd_source_test");
}
