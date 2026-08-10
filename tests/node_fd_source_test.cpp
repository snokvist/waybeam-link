// SPDX-License-Identifier: GPL-2.0-or-later
// The Android device-source seam: Config::adapter_fds -> AirBackend::create ->
// RadioAirCfg::adapter_fds, and the C ABI call that sets it.
//
// Why the seam needs its own test: RadioAir::create's own validation is
// already covered by radio_fd_source_test, but NOTHING previously connected a
// config-driven node to it. AirBackend::create built RadioAirCfg field by
// field and simply never mentioned adapter_fds, so an unrooted Android caller
// — which cannot enumerate usbfs and has no device source but a UsbManager fd
// — could not reach the feature at all through wblink_rx_run.
//
// SAFETY, inherited verbatim from radio_fd_source_test: **never assert on a
// case that enumerates.** An enumerating create() opens whatever Realtek
// dongle the host has and, at do_reset's default of true, resets and brings it
// up — on a bench rig with a usbfs udev rule that is live flight hardware.
// This host has an 8812AU on it right now. Every case below either stops in
// validation or wraps an fd on /dev/null, so the failure is a deterministic
// libusb_wrap_sys_device error and nothing here can touch a USB device.
#include <fcntl.h>
#include <unistd.h>

#include <string>

#include "wblink/config.h"
#include "wblink/node/air_backend.h"
#include "wblink/node/rx_node_c.h"
#include "wbtest.h"

using namespace wblink;

namespace {

Config fd_config(int fd_a, int fd_b) {
    Config cfg;
    for (const char* name : {"a", "b"}) {
        AdapterCfg a;
        a.name = name;
        a.role = Role::kRx;
        a.channel_mhz = 5805;
        cfg.adapters.push_back(a);
    }
    cfg.air.kind = AirCfg::Kind::kRadio;
    cfg.node.spectator = true;  // allow_rx_only: no TX stanza needed
    cfg.adapter_fds = {fd_a, fd_b};
    return cfg;
}

bool create_fails_containing(const Config& cfg, const char* needle) {
    auto r = node::AirBackend::create(cfg);
    if (r) return false;  // a pass here would have opened USB
    return r.error.find(needle) != std::string::npos;
}

// --- the seam itself ------------------------------------------------------
void test_config_fds_reach_the_radio() {
    const int null_fd = ::open("/dev/null", O_RDONLY);
    CHECK(null_fd >= 0);

    // An fd in slot 0 must be WRAPPED, not enumerated. libusb_wrap_sys_device
    // on /dev/null fails deterministically, and that error string is the
    // proof the fd travelled Config -> RadioAirCfg: without the propagation
    // this stanza would enumerate instead and never mention wrapping.
    {
        Config cfg = fd_config(null_fd, -1);
        CHECK(create_fails_containing(cfg, "libusb_wrap_sys_device"));
    }

    // Parallelism is enforced downstream, and reaching that error is itself
    // proof the vector was copied rather than dropped: an empty adapter_fds
    // is legal and would have enumerated instead.
    {
        Config cfg = fd_config(null_fd, -1);
        cfg.adapter_fds = {null_fd};  // 1 fd, 2 adapters
        CHECK(create_fails_containing(cfg, "adapter_fds must be empty"));
    }

    // NEGATIVE CONTROL for the two cases above. Same config, fds cleared —
    // and it must NOT produce either message. Without this, a create() that
    // failed for some unrelated reason would satisfy the greps above and the
    // test would pass while proving nothing. Asserting only on the ABSENCE
    // of the fd-path errors keeps this safe: an empty adapter_fds enumerates,
    // so whatever this returns is not something to assert a value on.
    {
        Config cfg = fd_config(-1, -1);
        cfg.adapter_fds.clear();
        auto r = node::AirBackend::create(cfg);
        if (!r) {
            CHECK(r.error.find("libusb_wrap_sys_device") == std::string::npos);
            CHECK(r.error.find("adapter_fds must be empty") ==
                  std::string::npos);
        }
    }

    ::close(null_fd);
}

// --- B4: supplying an fd must force the bring-up reset off ----------------
// RadioAir::create REFUSES do_reset together with a wrapped fd rather than
// silently downgrading it. So if AirBackend::create had left do_reset at its
// default true, the case below would fail with that refusal instead of
// reaching the wrap. Getting "libusb_wrap_sys_device" is what proves the
// override happened — and it is the only externally visible evidence of it.
void test_fd_forces_reset_off() {
    const int null_fd = ::open("/dev/null", O_RDONLY);
    CHECK(null_fd >= 0);
    Config cfg = fd_config(null_fd, -1);
    CHECK(create_fails_containing(cfg, "libusb_wrap_sys_device"));
    CHECK(!create_fails_containing(cfg, "reset"));
    ::close(null_fd);
}

// --- the C ABI setter -----------------------------------------------------
void test_c_abi_set_adapter_fds() {
    const int fds[2] = {7, -1};

    CHECK(wblink_rx_set_adapter_fds(nullptr, fds, 2) == 2);

    wblink_rx* rx = wblink_rx_create();
    CHECK(rx != nullptr);
    CHECK(wblink_rx_set_adapter_fds(rx, nullptr, 1) == 2);  // NULL with n>0
    CHECK(wblink_rx_set_adapter_fds(rx, nullptr, 0) == 0);  // NULL with n==0
    CHECK(wblink_rx_set_adapter_fds(rx, fds, 2) == 0);
    CHECK(wblink_rx_set_adapter_fds(rx, fds, 0) == 0);  // clears
    CHECK(wblink_rx_set_adapter_fds(rx, fds, 2) == 0);

    // Ordering guard. Stop first so the run returns 0 WITHOUT loading a config
    // or opening a radio — the header's documented pre-stop path — which is
    // what makes this assertion safe to make with real fds in the handle.
    wblink_rx_request_stop(rx);
    CHECK(wblink_rx_run(rx, "/nonexistent/config.json", nullptr, nullptr) == 0);

    // The handle is now used; setting fds after the config was consumed is
    // refused rather than silently ignored.
    CHECK(wblink_rx_set_adapter_fds(rx, fds, 2) == 3);

    wblink_rx_destroy(rx);
}

}  // namespace

int main() {
    test_config_fds_reach_the_radio();
    test_fd_forces_reset_off();
    test_c_abi_set_adapter_fds();
    // The return is load-bearing: a discarded wbtest_finish is a test that
    // cannot fail.
    return wbtest_finish("node_fd_source_test");
}
