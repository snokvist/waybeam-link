// SPDX-License-Identifier: GPL-2.0-or-later
//
// Hardware trial harness for the two RadioAir device sources (issue #140).
// Brings adapters up, prints per-unit identity and RX counters, tears down.
//
// **It never transmits.** Adapters are created RX-only (`allow_rx_only`), so
// no code path here can inject: bring-up is InitWrite + SetMonitorChannel +
// StartRxLoop. That is what makes it safe to run unattended, and it is also
// exactly the surface the Phase 1a/1b changes touched.
//
// Why this exists: Phase 1a (#138), 1b (#139) and 1a′ (#141) are all
// compile-, unit- and byte-comparison-verified. Several of their behaviours
// can only be observed against a radio — notably the bus:devaddr duplicate
// guard, which refuses two stanzas resolving to one unit and had never
// executed against a real device.
//
// The fd mode is the point. `libusb_wrap_sys_device` takes ANY usbfs fd on
// Linux, so `--fd <bus>/<dev>` exercises the whole Android-shaped path — wrap,
// do_reset=false, lock_dir, the claim, InitWrite, the EFUSE walk — with no
// phone involved. That makes B11's long-unproven leg (wrapped fd +
// do_reset=false + InitWrite + EFUSE) runnable on the bench.
//
//   hwtrial_bringup --auto                    # enumerate every Realtek unit
//   hwtrial_bringup --bus 5-2 --bus 8-4       # explicit bus paths
//   hwtrial_bringup --fd 5/2 --fd 8/4         # wrapped fds (needs usbfs rw)
//   hwtrial_bringup --fd 5/2 --bus 5-2        # duplicate-unit guard: must FAIL
//
// Exit 0 = every adapter came up and reported an identity. Anything else is a
// failure with a reason on stderr.

#include <dirent.h>
#include <fcntl.h>
#include <libusb.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "wblink/air_radio.h"

namespace {

constexpr uint16_t kRealtekVid = 0x0bda;

struct Unit {
    std::string bus_path;  // "5-2" — usb_path_of() form
    uint8_t busnum = 0;
    uint8_t devnum = 0;
};

// Enumerate Realtek units the same way RadioAir does, so --auto and --bus
// agree on what a path means.
std::vector<Unit> enumerate() {
    std::vector<Unit> out;
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        return out;
    }
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0 ||
            dd.idVendor != kRealtekVid) {
            continue;
        }
        uint8_t ports[7];
        const int np = libusb_get_port_numbers(list[i], ports, sizeof ports);
        Unit u;
        u.busnum = libusb_get_bus_number(list[i]);
        u.devnum = libusb_get_device_address(list[i]);
        u.bus_path = std::to_string(u.busnum);
        u.bus_path += '-';
        for (int k = 0; k < np; ++k) {
            if (k) u.bus_path += '.';
            u.bus_path += std::to_string(ports[k]);
        }
        std::fprintf(stderr, "unit %s (%04x:%04x, bus %u dev %u)\n",
                     u.bus_path.c_str(), dd.idVendor, dd.idProduct, u.busnum,
                     u.devnum);
        out.push_back(u);
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

// usbfs node for bus/dev. The fd is handed to libusb_wrap_sys_device and
// stays OWNED BY US — libusb marks a wrapped handle fd_keep, so RadioAir's
// teardown closes its side and leaves this descriptor open.
int open_usbfs(unsigned bus, unsigned dev) {
    char path[64];
    std::snprintf(path, sizeof path, "/dev/bus/usb/%03u/%03u", bus, dev);
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::fprintf(stderr,
                     "open %s: %s (usbfs needs rw — run as root or add a udev "
                     "rule)\n",
                     path, std::strerror(errno));
    }
    return fd;
}

void usage() {
    std::fprintf(stderr,
                 "usage: hwtrial_bringup [--auto] [--bus <path>]... "
                 "[--fd <bus>/<dev>]... [--chan-mhz N] [--seconds N] "
                 "[--lock-dir DIR]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> buses;
    std::vector<std::pair<unsigned, unsigned>> fds;
    uint16_t chan_mhz = 5805;
    int seconds = 3;
    std::string lock_dir;
    bool automatic = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (a == "--auto") {
            automatic = true;
        } else if (a == "--bus" && next) {
            buses.emplace_back(next);
            ++i;
        } else if (a == "--fd" && next) {
            unsigned b = 0, d = 0;
            if (std::sscanf(next, "%u/%u", &b, &d) != 2) {
                std::fprintf(stderr, "--fd wants <bus>/<dev>, got %s\n", next);
                return 2;
            }
            fds.emplace_back(b, d);
            ++i;
        } else if (a == "--chan-mhz" && next) {
            chan_mhz = static_cast<uint16_t>(std::atoi(next));
            ++i;
        } else if (a == "--seconds" && next) {
            seconds = std::atoi(next);
            ++i;
        } else if (a == "--lock-dir" && next) {
            lock_dir = next;
            ++i;
        } else {
            usage();
            return 2;
        }
    }

    const std::vector<Unit> units = enumerate();
    if (automatic) {
        if (units.empty()) {
            std::fprintf(stderr, "no Realtek units present\n");
            return 1;
        }
        for (const Unit& u : units) {
            buses.push_back(u.bus_path);
        }
    }
    if (buses.empty() && fds.empty()) {
        usage();
        return 2;
    }

    wblink::RadioAirCfg cfg;
    // RX-only on purpose: nothing here can transmit. It also means every
    // TX-die knob stays at its fail-closed default.
    cfg.allow_rx_only = true;
    cfg.lock_dir = lock_dir;

    std::vector<int> owned_fds;
    for (const std::string& b : buses) {
        wblink::AdapterCfg a;
        a.name = "bus-" + b;
        a.role = wblink::Role::kRx;
        a.channel_mhz = chan_mhz;
        a.bus = b;
        cfg.adapters.push_back(a);
        cfg.adapter_fds.push_back(-1);
    }
    for (const auto& [b, d] : fds) {
        const int fd = open_usbfs(b, d);
        if (fd < 0) {
            for (int f : owned_fds) ::close(f);
            return 1;
        }
        owned_fds.push_back(fd);
        wblink::AdapterCfg a;
        a.name = "fd-" + std::to_string(b) + "/" + std::to_string(d);
        a.role = wblink::Role::kRx;
        a.channel_mhz = chan_mhz;
        // No bus pin: create() refuses one on an fd-supplied stanza.
        cfg.adapters.push_back(a);
        cfg.adapter_fds.push_back(fd);
    }
    // B4: mandatory whenever any fd is supplied — the reset would
    // re-enumerate and orphan the descriptors opened above.
    cfg.do_reset = fds.empty();

    std::fprintf(stderr,
                 "bring-up: %zu adapter(s), %zu wrapped fd(s), ch %u MHz, "
                 "do_reset=%s, lock_dir=%s\n",
                 cfg.adapters.size(), fds.size(), chan_mhz,
                 cfg.do_reset ? "true" : "false",
                 lock_dir.empty() ? "(devourer default)" : lock_dir.c_str());

    int rc = 0;
    {
        auto air = wblink::RadioAir::create(cfg);
        if (!air) {
            std::fprintf(stderr, "FAIL create: %s\n", air.error.c_str());
            for (int f : owned_fds) ::close(f);
            return 1;
        }
        wblink::RadioAir& a = *air.value;
        for (size_t i = 0; i < a.rx_adapters(); ++i) {
            const std::string mac = a.adapter_mac(i);
            std::fprintf(stderr, "  adapter %zu: mac %s%s\n", i,
                         mac.empty() ? "NONE" : mac.c_str(),
                         mac.empty() ? "  <-- no identity (D3 fail-closed)"
                                     : "");
            if (mac.empty()) {
                rc = 1;  // §10.6: a unit with no identity cannot be trusted
            }
        }
        // Dwell so the RX threads actually run, then report. Frames are not
        // required (an idle channel is legitimate); a live counter proves the
        // loop is alive when there IS traffic.
        for (int s = 0; s < seconds; ++s) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        for (size_t i = 0; i < a.rx_adapters(); ++i) {
            const auto& c = a.counters(i);
            std::fprintf(stderr,
                         "  adapter %zu: rx_frames=%llu filtered=%llu "
                         "dropped=%llu\n",
                         i, static_cast<unsigned long long>(c.rx_frames),
                         static_cast<unsigned long long>(c.rx_filtered),
                         static_cast<unsigned long long>(c.rx_dropped));
        }
        std::fprintf(stderr, "teardown...\n");
    }
    // After teardown, so it also proves libusb left our descriptors alone
    // (fd_keep): a closed fd would make this fail.
    for (int f : owned_fds) {
        if (::fcntl(f, F_GETFD) < 0) {
            std::fprintf(stderr,
                         "FAIL: teardown closed our fd %d — ownership "
                         "contract broken\n",
                         f);
            rc = 1;
        }
        ::close(f);
    }
    std::fprintf(stderr, "%s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}
