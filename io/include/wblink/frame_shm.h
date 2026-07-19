// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: venc_frame_ring POSIX shared-memory transport (PROTOCOL.md
// §15.4). This is the live ring — mmap + memory-ordered index access + a
// Linux futex/eventfd bridge — over the pure layout constants in
// core/include/wblink/frame_shm_format.h. Byte-for-byte mirror of the
// waybeam_venc producer: same-host SPSC, native-endian.
//
// Producer (create): owns line 1 (write_idx/futex_seq), publishes frames,
// drops when full — never blocks. Consumer (attach): owns line 2
// (read_idx/consumer_waiting); a single reader thread futex-waits on the
// producer's futex_seq and signals an eventfd so the ring drops into a
// poll()-based main loop. All read_frame() calls stay on the owning (main)
// thread — the ring is strictly single-consumer.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "wblink/config.h"            // Result<T>
#include "wblink/frame_shm_format.h"  // layout constants + offsets

namespace wblink {

class FrameShmRing {
  public:
    ~FrameShmRing();
    FrameShmRing(const FrameShmRing&) = delete;
    FrameShmRing& operator=(const FrameShmRing&) = delete;

    // Producer: shm_unlink any stale name, shm_open O_CREAT|O_EXCL, publish as
    // mode 0666 (the consumer writes its SPSC indices), ftruncate, mmap, zero,
    // fill the immutable config header, then publish init_complete with a
    // release store. `name` without a leading '/' gets one.
    static Result<std::unique_ptr<FrameShmRing>> create(
        const std::string& name,
        uint32_t slots = kFrameRingDefaultSlots,
        uint32_t slot_size = kFrameRingDefaultSlotSize);

    // Consumer: shm_open O_RDWR, mmap, validate the header (magic / version /
    // power-of-two slot_count / init_complete, and recomputed total_size ==
    // the mmap size via fstat). Spawns one reader thread that futex-waits on
    // futex_seq and signals event_fd() on each wake.
    static Result<std::unique_ptr<FrameShmRing>> attach(const std::string& name);

    // Producer: write one whole frame blob. false if oversize
    // (> slot_data_size) or the ring is full (dropped — never blocks). Both
    // outcomes are recorded in stats().
    bool write_frame(const uint8_t* data, size_t len);

    // Consumer: copy the next frame into buf (call ONLY from the owning
    // thread — single-consumer). >0 = bytes copied, 0 = ring empty,
    // -1 = a bad/oversized slot (skipped) or buf too small (cap < frame).
    long read_frame(uint8_t* buf, size_t cap);

    int event_fd() const { return event_fd_; }  // consumer readiness; -1 producer
    void drain_event();                          // read() the eventfd after servicing
    bool is_consumer() const { return is_consumer_; }

    // False when the producer cleared init_complete or unlinked/recreated the
    // name behind this consumer's existing mapping.
    bool backing_object_current() const;

    struct Stats {
        uint64_t writes = 0;
        uint64_t reads = 0;
        uint64_t frame_bytes = 0;
        uint32_t frame_size_last = 0;
        uint32_t frame_size_min = 0;
        uint32_t frame_size_max = 0;
        uint64_t frame_interval_us = 0;
        uint64_t frame_jitter_us = 0;
        uint64_t full_drops = 0;
        uint64_t oversize_drops = 0;
        uint64_t bad_slots = 0;
    };
    const Stats& stats() const { return stats_; }
    void reset_stats();

  private:
    FrameShmRing() = default;

    // Typed views onto the mapped header (offsets from frame_shm_format.h).
    uint8_t* base() const { return map_; }
    void reader_loop();  // consumer thread body
    void note_frame(size_t len);

    uint8_t* map_ = nullptr;    // mmap base (page-aligned)
    size_t map_size_ = 0;       // total_size == header + slots*stride
    uint32_t slot_count_ = 0;   // power of two
    uint32_t slot_data_size_ = 0;
    size_t slot_stride_ = 0;    // align8(4 + slot_data_size)
    std::string name_;          // leading-'/' shm name
    uint64_t backing_dev_ = 0;  // fstat identity captured at attach
    uint64_t backing_ino_ = 0;
    bool is_owner_ = false;     // producer: shm_unlink on destroy
    bool is_consumer_ = false;  // consumer: has reader thread + eventfd
    int event_fd_ = -1;         // consumer eventfd (readiness)
    std::thread reader_;
    std::atomic<bool> stop_{false};
    Stats stats_;
    uint64_t last_frame_us_ = 0;
    uint64_t previous_interval_us_ = 0;
    uint64_t jitter_q4_us_ = 0;
};

}  // namespace wblink
