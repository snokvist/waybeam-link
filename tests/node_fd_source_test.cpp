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

// Every create() in this file must fail BEFORE the device-open loop. The
// comment above says so; this function is what makes it checkable.
//
// air_radio.cpp:695 is the enumerate branch — reached whenever a stanza has
// no fd — and it runs libusb_init, libusb_get_device_list, libusb_open on the
// first Realtek VID it finds, and then claim_interface_then_reset, which
// detaches the kernel driver and calls libusb_reset_device. On the bench rig
// CLAUDE.md documents (sudo, kernel drivers unloaded) that is live flight
// hardware being reset by a unit test.
//
// So: a create() that SUCCEEDS opened a device, and an error from the
// enumerate path means we got there. Both are failures of this file's
// contract, not just of the case under test — and the enumerate check must
// come first, because an EACCES on an unprivileged host makes an enumerating
// case look like a clean refusal. That accident is exactly what hid this once.
// Takes cfg BY VALUE: AirBackend::create writes the resolved adapter array
// back into it (§15.2 Pass 195), and a helper that quietly mutated the
// caller's fixture would make the case order matter.
bool create_fails_containing(Config cfg, const char* needle) {
    auto r = node::AirBackend::create(cfg);
    CHECK(!r);  // a pass here means a USB device was opened and brought up
    if (r) return false;
    // Enumerate-path markers (air_radio.cpp:695 onwards). Any of these means
    // the case escaped validation.
    for (const char* escaped : {"libusb_init failed", "no matching Realtek",
                                "libusb_get_device_list"}) {
        if (r.error.find(escaped) != std::string::npos) {
            std::fprintf(stderr,
                         "FATAL: case reached the ENUMERATE path (%s). This "
                         "test must never open a USB device.\n",
                         r.error.c_str());
            CHECK(false);
            return false;
        }
    }
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

    // NEGATIVE CONTROL for the two cases above: with adapter_fds cleared,
    // neither fd-path message may appear. Without it, a create() failing for
    // some unrelated reason would satisfy the greps above and prove nothing.
    //
    // IT MUST ALSO NOT ENUMERATE, and that is the whole difficulty. An empty
    // adapter_fds is precisely the shipped enumerate-by-bus-path config, so
    // the obvious way to write this control walks straight into
    // air_radio.cpp:695 and opens the host's dongle. The first version of
    // this test did exactly that and looked safe only because an
    // unprivileged libusb_open returns EACCES — safety by accident, and void
    // under the sudo bench procedure CLAUDE.md documents.
    //
    // So drop `spectator`: with no role:"tx" adapter and no allow_rx_only,
    // air_radio.cpp:493 refuses BEFORE any libusb call. Same two assertions,
    // no device anywhere near it.
    {
        Config cfg = fd_config(-1, -1);
        cfg.adapter_fds.clear();
        cfg.node.spectator = false;
        CHECK(create_fails_containing(cfg, "exactly one adapter"));
        auto r = node::AirBackend::create(cfg);
        CHECK(!r);
        CHECK(r.error.find("libusb_wrap_sys_device") == std::string::npos);
        CHECK(r.error.find("adapter_fds must be empty") == std::string::npos);
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

    // Pass 177: before any claimed run the handle is CREATED, and `exit_rc`
    // is untouched — primed with a sentinel so a spurious write is visible.
    {
        int rc = 55;
        CHECK(wblink_rx_state(nullptr, &rc) == -1);
        CHECK(wblink_rx_state(rx, &rc) == WBLINK_NODE_CREATED);
        CHECK(rc == 55);
    }

    // Ordering guard. Stop first so the run returns 0 WITHOUT loading a config
    // or opening a radio — the header's documented pre-stop path — which is
    // what makes this assertion safe to make with real fds in the handle.
    wblink_rx_request_stop(rx);
    CHECK(wblink_rx_run(rx, "/nonexistent/config.json", nullptr, nullptr) == 0);

    // Pass 177: that pre-stopped run still counts as a run — EXITED with rc
    // 0. A reader latching on "state == RUNNING" therefore stops trusting a
    // node that returned in microseconds, which is the whole point.
    {
        int rc = 55;
        CHECK(wblink_rx_state(rx, &rc) == WBLINK_NODE_EXITED);
        CHECK(rc == 0);
    }

    // The handle is now used; setting fds after the config was consumed is
    // refused rather than silently ignored.
    CHECK(wblink_rx_set_adapter_fds(rx, fds, 2) == 3);

    // Pass 176/178: a run that never reached a backend published no stats,
    // no health and — the one that matters — no control endpoint. An
    // embedder reading 3 knows there is no control plane, rather than being
    // handed an address that answers nothing.
    {
        size_t required = 77;
        CHECK(wblink_rx_stats(rx, nullptr, 0, &required) == 3);
        CHECK(wblink_rx_health(rx, nullptr, 0, &required) == 3);
        CHECK(wblink_rx_control_endpoint(rx, nullptr, 0, &required) == 3);
        CHECK(wblink_rx_control_endpoint(nullptr, nullptr, 0, &required) == 2);
    }

    // A reuse refusal (3) must not overwrite the real run's record.
    CHECK(wblink_rx_run(rx, "/nonexistent/config.json", nullptr, nullptr) == 3);
    {
        int rc = 55;
        CHECK(wblink_rx_state(rx, &rc) == WBLINK_NODE_EXITED);
        CHECK(rc == 0);
    }

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
