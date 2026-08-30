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
    // set under auto, so EVERY entry must be a real descriptor. The -1
    // "enumerate this stanza" sentinel is an array-form concept and must be
    // REFUSED here rather than honoured: honouring it manufactures an
    // enumerating stanza, which on unrooted Android — the only reason the fd
    // source exists — finds nothing and silently shrinks the device set, and
    // on a rooted host can claim the same physical dongle another entry
    // supplied by fd (a wrapped unit never enters used_paths, and the dev_key
    // guard is disabled at bus 0, which is what the wrap path reports).
    //
    // Hermetic: all of these are refused before any libusb call.
    {
        RadioAirCfg base;
        base.auto_cfg.enabled = true;
        base.auto_cfg.channel_mhz = 5805;
        base.allow_rx_only = true;

        RadioAirCfg cfg = base;
        cfg.adapter_fds = {-1};
        CHECK(fails_containing(cfg, "the fd list is the device set"));
        // NOT the array form's parallelism message — under auto there is no
        // stanza array for the fds to be parallel to.
        CHECK(!fails_containing(cfg, "adapter_fds must be empty"));

        cfg = base;
        cfg.adapter_fds = {3, -1};  // one good, one sentinel
        CHECK(fails_containing(cfg, "adapter_fds[1]"));

        cfg = base;
        cfg.adapter_fds = {-2};
        CHECK(fails_containing(cfg, "the fd list is the device set"));
    }

    // --- §3.0 (Pass 198) SA-latch freshness ---------------------------------
    // inject_return's entire out-of-range decision is this predicate: fresh
    // keeps unicast + hardware retries, stale falls back to one broadcast
    // copy. The cases below are the ones a hardware run cannot stage.
    {
        // Plain aging. The boundary is exclusive-at-stale: a latch exactly
        // `stale_ms` old is already stale, so the window means "unheard for
        // this long", not "this long plus a tick".
        CHECK(RadioAir::sa_fresh(1000, 500, 1000));   // 500 ms old
        CHECK(RadioAir::sa_fresh(1499, 500, 1000));   // 999 ms old
        CHECK(!RadioAir::sa_fresh(1500, 500, 1000));  // exactly 1000 ms
        CHECK(!RadioAir::sa_fresh(9000, 500, 1000));  // long gone

        // Just-heard is always fresh, at every window.
        CHECK(RadioAir::sa_fresh(500, 500, 1000));
        CHECK(RadioAir::sa_fresh(500, 500, 1));

        // stale_ms == 0 disables the age-out entirely (the Pass 12
        // never-expiring latch, kept for the A/B). Nothing is ever stale,
        // however old — this is the footgun §15.2 names, and it has to keep
        // working exactly as Pass 12 did or the A/B measures two changes.
        CHECK(RadioAir::sa_fresh(0xffffffffull, 0, 0));

        // THE CLOCK RACE. last_ms is stamped by the RX thread and read by
        // the main one, so the two steady_ms() reads can land out of order.
        // Unguarded, now - last underflows to ~1.8e19 ms and every target
        // reads permanently stale — the hybrid would disarm itself for the
        // rest of the session on a one-millisecond interleave. A latch from
        // the future is not-yet-aged, which is the only safe reading.
        CHECK(RadioAir::sa_fresh(500, 501, 1000));
        CHECK(RadioAir::sa_fresh(0, 1, 1));
        CHECK(RadioAir::sa_fresh(0, 0xffffffffull, 1000));

        // A 1 ms window still ages: the guard above must not swallow real
        // staleness at small windows.
        CHECK(!RadioAir::sa_fresh(502, 500, 1));
    }

    return wbtest_finish("radio_rx_only_test");
}
