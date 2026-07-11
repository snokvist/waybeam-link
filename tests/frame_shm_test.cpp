// SPDX-License-Identifier: GPL-2.0-or-later
// §15.4 venc_frame_ring SHM transport: single-process create + attach of the
// same ring. Round-trip (incl. >64 KB frames), full-ring drop, oversize drop,
// attach validation, eventfd readiness, and clean teardown (join + unlink
// without hanging — proven by returning cleanly under the dev preset's ASan).
#include "wblink/frame_shm.h"

#include <poll.h>
#include <sys/mman.h>

#include <cstring>
#include <string>
#include <vector>

#include "wbtest.h"

using namespace wblink;

namespace {

// Fixed suffix so a stale /dev/shm object from a crashed run can't collide.
const char* kRoundTrip = "/wblink-frame-shm-test-rt";
const char* kFull = "/wblink-frame-shm-test-full";
const char* kOversize = "/wblink-frame-shm-test-oversize";
const char* kEvent = "/wblink-frame-shm-test-event";
const char* kMissing = "/wblink-frame-shm-test-missing";

std::vector<uint8_t> pattern(size_t len, uint8_t seed) {
    std::vector<uint8_t> v(len);
    for (size_t i = 0; i < len; ++i) {
        v[i] = static_cast<uint8_t>((i * 31u + seed) & 0xFFu);
    }
    return v;
}

}  // namespace

int main() {
    // Defensive: clear any stale objects up front (ENOENT is fine).
    for (const char* n : {kRoundTrip, kFull, kOversize, kEvent, kMissing}) {
        ::shm_unlink(n);
    }

    // --- round-trip: varied sizes incl. > 64 KB, byte-exact, in order ------
    {
        const uint32_t slot_size = 128 * 1024;  // holds a > 64 KB frame
        auto prod = FrameShmRing::create(kRoundTrip, 8, slot_size);
        CHECK(bool(prod));
        auto cons = FrameShmRing::attach(kRoundTrip);
        CHECK(bool(cons));
        if (prod && cons) {
            FrameShmRing& p = **prod.value;
            FrameShmRing& c = **cons.value;
            CHECK(!p.is_consumer());
            CHECK(c.is_consumer());
            CHECK_EQ_U(p.event_fd() < 0, 1);   // producer has no eventfd
            CHECK(c.event_fd() >= 0);

            const size_t sizes[] = {1, 10, 1000, 65600, slot_size, 200, 4096};
            const size_t n = sizeof(sizes) / sizeof(sizes[0]);
            std::vector<std::vector<uint8_t>> frames;
            for (size_t i = 0; i < n; ++i) {
                frames.push_back(pattern(sizes[i], static_cast<uint8_t>(i + 1)));
                CHECK(p.write_frame(frames[i].data(), frames[i].size()));
            }
            CHECK_EQ_U(p.stats().writes, n);

            std::vector<uint8_t> buf(slot_size);
            for (size_t i = 0; i < n; ++i) {
                const long got = c.read_frame(buf.data(), buf.size());
                CHECK_EQ_U(static_cast<unsigned long long>(got), sizes[i]);
                CHECK(got >= 0 &&
                      std::memcmp(buf.data(), frames[i].data(), sizes[i]) == 0);
            }
            CHECK_EQ_U(c.read_frame(buf.data(), buf.size()), 0);  // now empty
            CHECK_EQ_U(c.stats().reads, n);
        }
    }

    // --- full-ring drop: fill slots+1 without reading ----------------------
    {
        const uint32_t slots = 4;
        const uint32_t slot_size = 4096;
        auto prod = FrameShmRing::create(kFull, slots, slot_size);
        CHECK(bool(prod));
        auto cons = FrameShmRing::attach(kFull);
        CHECK(bool(cons));
        if (prod && cons) {
            FrameShmRing& p = **prod.value;
            FrameShmRing& c = **cons.value;
            auto f = pattern(128, 7);
            for (uint32_t i = 0; i < slots; ++i) {
                CHECK(p.write_frame(f.data(), f.size()));
            }
            // One past capacity: dropped, never blocks.
            CHECK(!p.write_frame(f.data(), f.size()));
            CHECK_EQ_U(p.stats().full_drops, 1);
            CHECK_EQ_U(p.stats().writes, slots);

            // Drain exactly the frames that fit, in order.
            std::vector<uint8_t> buf(slot_size);
            for (uint32_t i = 0; i < slots; ++i) {
                const long got = c.read_frame(buf.data(), buf.size());
                CHECK_EQ_U(static_cast<unsigned long long>(got), f.size());
                CHECK(got >= 0 && std::memcmp(buf.data(), f.data(), f.size()) == 0);
            }
            CHECK_EQ_U(c.read_frame(buf.data(), buf.size()), 0);
        }
    }

    // --- oversize: len > slot_data_size is rejected ------------------------
    {
        const uint32_t slot_size = 4096;
        auto prod = FrameShmRing::create(kOversize, 4, slot_size);
        CHECK(bool(prod));
        if (prod) {
            FrameShmRing& p = **prod.value;
            auto big = pattern(slot_size + 1, 3);
            CHECK(!p.write_frame(big.data(), big.size()));
            CHECK_EQ_U(p.stats().oversize_drops, 1);
            CHECK_EQ_U(p.stats().writes, 0);
            // Exactly slot_data_size is accepted (boundary).
            auto exact = pattern(slot_size, 4);
            CHECK(p.write_frame(exact.data(), exact.size()));
        }
    }

    // --- attach validation: missing object fails ---------------------------
    {
        auto miss = FrameShmRing::attach(kMissing);
        CHECK(!miss);
    }

    // --- eventfd readiness: write -> poll readable -> drain -> read ---------
    {
        const uint32_t slot_size = 4096;
        auto prod = FrameShmRing::create(kEvent, 4, slot_size);
        CHECK(bool(prod));
        auto cons = FrameShmRing::attach(kEvent);
        CHECK(bool(cons));
        if (prod && cons) {
            FrameShmRing& p = **prod.value;
            FrameShmRing& c = **cons.value;
            c.drain_event();  // clear any startup edge

            auto f = pattern(777, 9);
            CHECK(p.write_frame(f.data(), f.size()));

            pollfd pfd{c.event_fd(), POLLIN, 0};
            bool readable = false;
            for (int tries = 0; tries < 20 && !readable; ++tries) {
                const int rc = ::poll(&pfd, 1, 200);  // 200 ms slices, same-proc
                readable = (rc > 0) && (pfd.revents & POLLIN) != 0;
            }
            CHECK(readable);
            c.drain_event();

            std::vector<uint8_t> buf(slot_size);
            const long got = c.read_frame(buf.data(), buf.size());
            CHECK_EQ_U(static_cast<unsigned long long>(got), f.size());
            CHECK(got >= 0 && std::memcmp(buf.data(), f.data(), f.size()) == 0);
        }
    }
    // Rings out of scope here: destructors join the reader thread + unlink.
    // Reaching wbtest_finish without hanging proves clean teardown.

    return wbtest_finish("frame_shm_test");
}
