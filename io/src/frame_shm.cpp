// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/frame_shm.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace wblink {

namespace {

using R = Result<std::unique_ptr<FrameShmRing>>;

constexpr uint32_t kReaderTimeoutNs = 100 * 1000 * 1000;  // 100 ms observe-stop tick
constexpr mode_t kEgressMode = 0666;

std::string normalize_name(const std::string& name) {
    if (!name.empty() && name.front() == '/') {
        return name;
    }
    return "/" + name;
}

bool is_pow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

size_t align8(size_t v) { return (v + 7) & ~static_cast<size_t>(7); }

uint64_t monotonic_us() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000u +
           static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

// ---- native-endian header field access (offsets from frame_shm_format.h) ----
// Config words (line 0) are plain reads/writes; index words (lines 1/2) go
// through __atomic_* with the memory order the SPSC protocol requires.

uint32_t load_u32(const uint8_t* base, size_t off) {
    uint32_t v;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

void store_u32(uint8_t* base, size_t off, uint32_t v) {
    std::memcpy(base + off, &v, sizeof(v));
}

uint64_t atomic_load_u64(const uint8_t* base, size_t off, int order) {
    return __atomic_load_n(reinterpret_cast<const uint64_t*>(base + off), order);
}

void atomic_store_u64(uint8_t* base, size_t off, uint64_t v, int order) {
    __atomic_store_n(reinterpret_cast<uint64_t*>(base + off), v, order);
}

uint32_t atomic_load_u32(const uint8_t* base, size_t off, int order) {
    return __atomic_load_n(reinterpret_cast<const uint32_t*>(base + off), order);
}

uint16_t atomic_load_u16(const uint8_t* base, size_t off, int order) {
    return __atomic_load_n(reinterpret_cast<const uint16_t*>(base + off), order);
}

void atomic_store_u32(uint8_t* base, size_t off, uint32_t v, int order) {
    __atomic_store_n(reinterpret_cast<uint32_t*>(base + off), v, order);
}

// Shared (cross-process) futex — NOT the _PRIVATE variant: producer and
// consumer live in different processes / mappings of the same shm object, so
// the kernel must key the wait on the physical page, not the mm.
long futex_wait(uint32_t* addr, uint32_t expected, uint32_t timeout_ns) {
    timespec ts{};
    ts.tv_sec = 0;
    ts.tv_nsec = static_cast<long>(timeout_ns);
    return syscall(SYS_futex, reinterpret_cast<int*>(addr), FUTEX_WAIT,
                   static_cast<int>(expected), &ts, nullptr, 0);
}

void futex_wake(uint32_t* addr, int count) {
    syscall(SYS_futex, reinterpret_cast<int*>(addr), FUTEX_WAKE, count, nullptr,
            nullptr, 0);
}

}  // namespace

// ---- create (producer) ----------------------------------------------------

R FrameShmRing::create(const std::string& name, uint32_t slots,
                       uint32_t slot_size) {
    if (!is_pow2(slots)) {
        return R::fail("frame_shm create: slots must be a power of two");
    }
    if (slot_size == 0) {
        return R::fail("frame_shm create: slot_size must be > 0");
    }
    const std::string shm_name = normalize_name(name);
    const size_t stride = align8(kFrameSlotLenPrefix + slot_size);
    const size_t total = kFrameRingHeaderSize + static_cast<size_t>(slots) * stride;
    if (total > 0xFFFFFFFFu) {
        return R::fail("frame_shm create: total_size overflows u32 header field");
    }

    // Clear any stale object so the O_EXCL open below is authoritative.
    ::shm_unlink(shm_name.c_str());
    const int fd =
        ::shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, kEgressMode);
    if (fd < 0) {
        return R::fail("frame_shm create: shm_open('" + shm_name + "'): " +
                       std::strerror(errno));
    }
    // The monitor-radio process commonly runs as root while its local viewer
    // does not. The consumer owns read_idx/consumer_waiting, so the SPSC ABI
    // requires a writable mapping. Apply the public mode after creation so a
    // restrictive service umask cannot narrow it (§15.4).
    if (::fchmod(fd, kEgressMode) != 0) {
        const std::string e = std::strerror(errno);
        ::close(fd);
        ::shm_unlink(shm_name.c_str());
        return R::fail("frame_shm create: fchmod: " + e);
    }
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
        const std::string e = std::strerror(errno);
        ::close(fd);
        ::shm_unlink(shm_name.c_str());
        return R::fail("frame_shm create: ftruncate: " + e);
    }
    struct stat backing{};
    const bool have_backing = ::fstat(fd, &backing) == 0;
    void* p = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);  // the mapping keeps the object alive
    if (p == MAP_FAILED) {
        ::shm_unlink(shm_name.c_str());
        return R::fail("frame_shm create: mmap: " + std::string(std::strerror(errno)));
    }

    auto ring = std::unique_ptr<FrameShmRing>(new FrameShmRing());
    ring->map_ = static_cast<uint8_t*>(p);
    ring->map_size_ = total;
    ring->slot_count_ = slots;
    ring->slot_data_size_ = slot_size;
    ring->slot_stride_ = stride;
    ring->name_ = shm_name;
    ring->is_owner_ = true;
    if (have_backing) {
        ring->backing_dev_ = static_cast<uint64_t>(backing.st_dev);
        ring->backing_ino_ = static_cast<uint64_t>(backing.st_ino);
    }

    uint8_t* b = ring->map_;
    std::memset(b, 0, total);  // zeroes write_idx / read_idx / futex_seq / waiting
    store_u32(b, kFrHdrMagic, kFrameRingMagic);
    store_u32(b, kFrHdrVersion, kFrameRingVersion);
    store_u32(b, kFrHdrSlotCount, slots);
    store_u32(b, kFrHdrSlotDataSize, slot_size);
    store_u32(b, kFrHdrTotalSize, static_cast<uint32_t>(total));
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    store_u32(b, kFrHdrEpoch,
              static_cast<uint32_t>(static_cast<uint64_t>(ts.tv_sec) * 1000u +
                                    static_cast<uint64_t>(ts.tv_nsec) / 1000000u));
    // Publish config last: a release store on init_complete makes every prior
    // header write visible to a consumer that acquire-loads it.
    atomic_store_u32(b, kFrHdrInitComplete, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(reinterpret_cast<uint32_t*>(b + kFrHdrFutexSeq), 1,
                       __ATOMIC_SEQ_CST);
    futex_wake(reinterpret_cast<uint32_t*>(b + kFrHdrFutexSeq), INT32_MAX);
    return R::ok(std::move(ring));
}

// ---- attach (consumer) ----------------------------------------------------

R FrameShmRing::attach(const std::string& name) {
    const std::string shm_name = normalize_name(name);
    const int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
    if (fd < 0) {
        return R::fail("frame_shm attach: shm_open('" + shm_name + "'): " +
                       std::strerror(errno));
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const std::string e = std::strerror(errno);
        ::close(fd);
        return R::fail("frame_shm attach: fstat: " + e);
    }
    const size_t map_size = static_cast<size_t>(st.st_size);
    if (map_size < kFrameRingHeaderSize) {
        ::close(fd);
        return R::fail("frame_shm attach: object smaller than ring header");
    }
    void* p = ::mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        return R::fail("frame_shm attach: mmap: " + std::string(std::strerror(errno)));
    }
    uint8_t* b = static_cast<uint8_t*>(p);

    // Acquire on init_complete pairs with the producer's release; every other
    // header field is safe to read once it reads back 1.
    const uint32_t inited = atomic_load_u32(b, kFrHdrInitComplete, __ATOMIC_ACQUIRE);
    const uint32_t magic = load_u32(b, kFrHdrMagic);
    const uint32_t version = load_u32(b, kFrHdrVersion);
    const uint32_t slots = load_u32(b, kFrHdrSlotCount);
    const uint32_t slot_size = load_u32(b, kFrHdrSlotDataSize);
    const uint32_t total_hdr = load_u32(b, kFrHdrTotalSize);

    auto bail = [&](const std::string& msg) -> R {
        ::munmap(p, map_size);
        return R::fail("frame_shm attach: " + msg);
    };
    if (magic != kFrameRingMagic) {
        return bail("bad magic");
    }
    if (version != kFrameRingVersion) {
        return bail("version mismatch");
    }
    if (inited != 1) {
        return bail("producer has not published init_complete");
    }
    if (!is_pow2(slots) || slot_size == 0) {
        return bail("invalid geometry (slot_count not pow2 or slot_size 0)");
    }
    const size_t stride = align8(kFrameSlotLenPrefix + slot_size);
    const size_t total = kFrameRingHeaderSize + static_cast<size_t>(slots) * stride;
    if (total != map_size || total_hdr != map_size) {
        return bail("total_size disagrees with mmap size");
    }

    const int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        return bail("eventfd: " + std::string(std::strerror(errno)));
    }

    auto ring = std::unique_ptr<FrameShmRing>(new FrameShmRing());
    ring->map_ = b;
    ring->map_size_ = map_size;
    ring->slot_count_ = slots;
    ring->slot_data_size_ = slot_size;
    ring->slot_stride_ = stride;
    ring->name_ = shm_name;
    ring->backing_dev_ = static_cast<uint64_t>(st.st_dev);
    ring->backing_ino_ = static_cast<uint64_t>(st.st_ino);
    ring->is_owner_ = false;
    ring->is_consumer_ = true;
    ring->event_fd_ = efd;
    if (atomic_load_u32(b, kFrHdrHealthMagic, __ATOMIC_ACQUIRE) ==
        kFrameHealthMagic) {
        ring->health_full_drops_baseline_ =
            atomic_load_u64(b, kFrHdrFullDrops, __ATOMIC_RELAXED);
        ring->health_baseline_valid_ = true;
    }
    ring->reader_ = std::thread([r = ring.get()] { r->reader_loop(); });
    return R::ok(std::move(ring));
}

bool FrameShmRing::backing_object_current() const {
    if (!is_consumer_ || map_ == nullptr ||
        atomic_load_u32(map_, kFrHdrInitComplete, __ATOMIC_ACQUIRE) != 1) {
        return false;
    }
    const int fd = ::shm_open(name_.c_str(), O_RDWR, 0);
    if (fd < 0) {
        return false;
    }
    struct stat st{};
    const bool current =
        ::fstat(fd, &st) == 0 &&
        static_cast<uint64_t>(st.st_dev) == backing_dev_ &&
        static_cast<uint64_t>(st.st_ino) == backing_ino_;
    ::close(fd);
    return current;
}

// ---- reader thread (consumer) ---------------------------------------------

void FrameShmRing::reader_loop() {
    uint8_t* b = map_;
    uint32_t* seq_addr = reinterpret_cast<uint32_t*>(b + kFrHdrFutexSeq);
    while (!stop_.load(std::memory_order_acquire)) {
        // Readiness is a level, not an edge: whenever the ring holds an
        // undrained frame, nudge the main loop. This is robust to the
        // start-up race where the producer bumps futex_seq before this thread
        // ever parks — we detect the pending frame directly instead of relying
        // on catching that one wake.
        const uint64_t wr = atomic_load_u64(b, kFrHdrWriteIdx, __ATOMIC_ACQUIRE);
        const uint64_t rd = atomic_load_u64(b, kFrHdrReadIdx, __ATOMIC_ACQUIRE);
        if (wr != rd) {
            const uint64_t one = 1;
            const ssize_t n = ::write(event_fd_, &one, sizeof(one));
            (void)n;  // eventfd add of 1 never blocks/short-writes here
        }
        // Park until the producer seq-bumps (low-latency wake) or the 100 ms
        // tick fires (re-check pending / stop_). Publish "waiting" BEFORE
        // loading the seq we sleep on: the producer seq-bumps then reads
        // consumer_waiting, and the seq_cst ordering makes the race decidable —
        // either it sees waiting=1 and wakes us, or FUTEX_WAIT sees the moved
        // seq and returns immediately.
        atomic_store_u32(b, kFrHdrConsumerWaiting, 1, __ATOMIC_SEQ_CST);
        const uint32_t seq = atomic_load_u32(b, kFrHdrFutexSeq, __ATOMIC_SEQ_CST);
        futex_wait(seq_addr, seq, kReaderTimeoutNs);
        atomic_store_u32(b, kFrHdrConsumerWaiting, 0, __ATOMIC_SEQ_CST);
    }
}

// ---- write_frame (producer) -----------------------------------------------

bool FrameShmRing::write_frame(const uint8_t* data, size_t len) {
    if (len > slot_data_size_) {
        ++stats_.oversize_drops;
        return false;
    }
    uint8_t* b = map_;
    // Producer owns write_idx (relaxed self-read); acquire read_idx to see the
    // consumer's latest release.
    const uint64_t w = atomic_load_u64(b, kFrHdrWriteIdx, __ATOMIC_RELAXED);
    const uint64_t r = atomic_load_u64(b, kFrHdrReadIdx, __ATOMIC_ACQUIRE);
    if (w - r >= slot_count_) {  // full: drop, never block (§15.4)
        ++stats_.full_drops;
        return false;
    }
    const size_t slot = static_cast<size_t>(w & (slot_count_ - 1));
    uint8_t* p = b + kFrameRingHeaderSize + slot * slot_stride_;
    const uint32_t l = static_cast<uint32_t>(len);
    std::memcpy(p, &l, sizeof(l));
    if (len > 0) {
        std::memcpy(p + kFrameSlotLenPrefix, data, len);
    }
    // Release-store write_idx publishes the slot bytes to the consumer's
    // acquire-load.
    atomic_store_u64(b, kFrHdrWriteIdx, w + 1, __ATOMIC_RELEASE);

    // Bump futex_seq and wake the consumer if it parked (seq_cst pairs with the
    // reader's waiting-store / seq-load ordering).
    __atomic_add_fetch(reinterpret_cast<uint32_t*>(b + kFrHdrFutexSeq), 1,
                       __ATOMIC_SEQ_CST);
    if (atomic_load_u32(b, kFrHdrConsumerWaiting, __ATOMIC_SEQ_CST) != 0) {
        futex_wake(reinterpret_cast<uint32_t*>(b + kFrHdrFutexSeq), 1);
    }
    ++stats_.writes;
    note_frame(len);
    return true;
}

// ---- read_frame (consumer, owning thread only) ----------------------------

long FrameShmRing::read_frame(uint8_t* buf, size_t cap) {
    uint8_t* b = map_;
    const uint64_t w = atomic_load_u64(b, kFrHdrWriteIdx, __ATOMIC_ACQUIRE);
    const uint64_t r = atomic_load_u64(b, kFrHdrReadIdx, __ATOMIC_RELAXED);
    if (r == w) {
        return 0;  // empty
    }
    // §15.3 Pass 109: ring_full remains independent leading evidence even when
    // producer health is available. Sample it before this read frees the slot;
    // the producer may report no drop if the read wins the race.
    if (w - r >= slot_count_) {
        ++stats_.ring_full;
    }
    const size_t slot = static_cast<size_t>(r & (slot_count_ - 1));
    const uint8_t* p = b + kFrameRingHeaderSize + slot * slot_stride_;
    uint32_t len;
    std::memcpy(&len, p, sizeof(len));
    if (len > slot_data_size_) {
        // Corrupt slot: skip it so a single bad frame can't stall the ring.
        ++stats_.bad_slots;
        atomic_store_u64(b, kFrHdrReadIdx, r + 1, __ATOMIC_RELEASE);
        return -1;
    }
    if (len > cap) {
        // B5: caller buffer too small — don't advance; caller must retry with a
        // buffer sized to slot_data_size(). Counted as a bad slot, and logged
        // once so this ring-wedging misconfiguration is not silent.
        ++stats_.bad_slots;
        if (!warned_undersized_) {
            warned_undersized_ = true;
            std::fprintf(stderr,
                         "frame_shm: read buffer %zu < frame %u — ingress "
                         "stalled; size the buffer to slot_data_size (%u)\n",
                         cap, len, slot_data_size_);
        }
        return -1;
    }
    if (len > 0) {
        std::memcpy(buf, p + kFrameSlotLenPrefix, len);
    }
    atomic_store_u64(b, kFrHdrReadIdx, r + 1, __ATOMIC_RELEASE);
    ++stats_.reads;
    note_frame(len);
    return static_cast<long>(len);
}

void FrameShmRing::note_frame(size_t len) {
    const uint32_t size = static_cast<uint32_t>(len);
    stats_.frame_bytes += len;
    stats_.frame_size_last = size;
    if (stats_.frame_size_min == 0 || size < stats_.frame_size_min) {
        stats_.frame_size_min = size;
    }
    if (size > stats_.frame_size_max) {
        stats_.frame_size_max = size;
    }

    const uint64_t now = monotonic_us();
    if (last_frame_us_ != 0) {
        const uint64_t interval = now - last_frame_us_;
        stats_.frame_interval_us = interval;
        if (previous_interval_us_ != 0) {
            const uint64_t variation = interval > previous_interval_us_
                                           ? interval - previous_interval_us_
                                           : previous_interval_us_ - interval;
            // Fixed-point J*16 form of J += (variation - J) / 16.
            const uint64_t current = (jitter_q4_us_ + 8u) >> 4u;
            if (variation >= current) {
                jitter_q4_us_ += variation - current;
            } else {
                jitter_q4_us_ -= current - variation;
            }
            stats_.frame_jitter_us = (jitter_q4_us_ + 8u) >> 4u;
        }
        previous_interval_us_ = interval;
    }
    last_frame_us_ = now;
}

FrameShmRing::Stats FrameShmRing::stats() {
    Stats out = stats_;
    if (!is_consumer_ || map_ == nullptr) {
        return out;
    }

    // The extension is optional at version 1. Only the exact marker makes the
    // following bytes meaningful; zero or unknown values remain attached and
    // report unavailable forever without turning absence into a false zero.
    if (atomic_load_u32(map_, kFrHdrHealthMagic, __ATOMIC_ACQUIRE) !=
        kFrameHealthMagic) {
        out.full_drops = 0;
        out.health_valid = false;
        out.throttle_permille = 0;
        health_baseline_valid_ = false;
        health_full_drops_baseline_ = 0;
        return out;
    }

    const uint64_t full_drops =
        atomic_load_u64(map_, kFrHdrFullDrops, __ATOMIC_RELAXED);
    out.health_valid = true;
    out.throttle_permille =
        atomic_load_u16(map_, kFrHdrThrottlePermille, __ATOMIC_RELAXED);
    if (!health_baseline_valid_ ||
        full_drops < health_full_drops_baseline_) {
        // A producer restart/counter reset rebases the public delta instead of
        // underflowing into an enormous cumulative count.
        health_full_drops_baseline_ = full_drops;
        health_baseline_valid_ = true;
        out.full_drops = 0;
    } else {
        out.full_drops = full_drops - health_full_drops_baseline_;
    }
    return out;
}

void FrameShmRing::reset_stats() {
    stats_ = {};
    last_frame_us_ = 0;
    previous_interval_us_ = 0;
    jitter_q4_us_ = 0;
    if (is_consumer_ && map_ != nullptr &&
        atomic_load_u32(map_, kFrHdrHealthMagic, __ATOMIC_ACQUIRE) ==
            kFrameHealthMagic) {
        health_full_drops_baseline_ =
            atomic_load_u64(map_, kFrHdrFullDrops, __ATOMIC_RELAXED);
        health_baseline_valid_ = true;
    } else {
        health_full_drops_baseline_ = 0;
        health_baseline_valid_ = false;
    }
}

// ---- eventfd drain --------------------------------------------------------

void FrameShmRing::drain_event() {
    if (event_fd_ < 0) {
        return;
    }
    uint64_t sink;
    for (;;) {
        const ssize_t n = ::read(event_fd_, &sink, sizeof(sink));
        if (n != static_cast<ssize_t>(sizeof(sink))) {
            break;  // EAGAIN (drained) or short read — nothing more to consume
        }
    }
}

// ---- teardown -------------------------------------------------------------

FrameShmRing::~FrameShmRing() {
    if (reader_.joinable()) {
        stop_.store(true, std::memory_order_release);
        // Kick the futex so the reader doesn't sit out its 100 ms timeout; the
        // mapping is still valid until after the join.
        if (map_ != nullptr) {
            futex_wake(reinterpret_cast<uint32_t*>(map_ + kFrHdrFutexSeq),
                       INT32_MAX);
        }
        reader_.join();
    }
    if (event_fd_ >= 0) {
        ::close(event_fd_);
        event_fd_ = -1;
    }
    if (map_ != nullptr) {
        ::munmap(map_, map_size_);
        map_ = nullptr;
    }
    if (is_owner_ && !name_.empty()) {
        // A newer producer may already have unlinked/recreated this name.
        // Never let teardown of our orphaned mapping unlink its replacement.
        const int fd = ::shm_open(name_.c_str(), O_RDWR, 0);
        struct stat st{};
        const bool still_ours =
            fd >= 0 && ::fstat(fd, &st) == 0 &&
            static_cast<uint64_t>(st.st_dev) == backing_dev_ &&
            static_cast<uint64_t>(st.st_ino) == backing_ino_;
        if (fd >= 0) {
            ::close(fd);
        }
        if (still_ours) {
            ::shm_unlink(name_.c_str());
        }
    }
}

}  // namespace wblink
