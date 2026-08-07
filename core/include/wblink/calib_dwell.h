// §3.16 (Pass 153) calibration dwell primitive — the shared evidence engine
// under §10.6 and §10.7. One dwell: the sender emits `count` numbered,
// MTU-padded PROBE frames at a fixed (rung, qdb); the receiver counts them
// and returns ONE TALLY; loss is self-denominating from the sender's own
// count. No cumulative counters, no drains, no dual clocks — the dwell_id is
// the synchronisation.
//
// Library-shaped (design doc §3, feeds the #109 extraction): pure state
// machines, time injected as now_ms, wire ownership stays with the caller —
// the sender surfaces "emit this probe" actions and consumes decoded tallies;
// the receiver consumes decoded probes and surfaces "send this tally"
// actions. No sockets, threads, or std::function.
#pragma once

#include <array>
#include <cstdint>

#include "wblink/types.h"

namespace wblink {

// ---------------------------------------------------------------------------
// Sender half — drives one dwell's probe burst and tally wait.

enum class DwellState : uint8_t {
    kIdle = 0,
    kEmitting,    // burst in flight, paced by probe_pace_us
    kAwaitTally,  // burst done; waiting, re-eliciting by re-emitting the tail
    kDone,        // tally accepted — result() is valid
    kNoEvidence,  // re-elicitation exhausted — inconclusive, NEVER clean
};

struct DwellSendParams {
    // §15.2 Tier-2 seeds (Pass 153). Pacing keeps a burst from flooding the
    // adapter queue; the wait/retry pair bounds tally re-elicitation.
    uint32_t probe_pace_us = 2000;
    uint32_t tally_wait_ms = 500;
    uint32_t tally_retries = 3;
    // Catch-up bound per tick: a stalled caller must not dump the whole
    // remaining burst into one injection window.
    uint16_t max_probes_per_tick = 8;
};

// One "emit a probe" instruction. seq repeats only for tail re-elicitation.
struct DwellProbeOut {
    bool send = false;
    uint16_t seq = 0;
};

struct DwellResult {
    uint16_t sent = 0;
    uint16_t received = 0;
    uint32_t rssi_sum_dbm = 0;  // i32 wire image, from the tally
    uint8_t rx_mcs = kUplinkRxMcsUnknown;
    uint8_t adapter_fingerprint = 0;
    // loss‰ = 1000 * (sent - received) / sent — computed here so every
    // consumer shares one rounding rule.
    uint16_t loss_milli() const {
        if (sent == 0) return 1000;
        const uint32_t r = received > sent ? sent : received;
        return static_cast<uint16_t>(1000u * (sent - r) / sent);
    }
};

class DwellSender {
  public:
    explicit DwellSender(const DwellSendParams& p) : p_(p) {}

    DwellState state() const { return state_; }
    uint32_t run_id() const { return run_id_; }
    uint16_t dwell_id() const { return dwell_id_; }
    // Valid once state() == kDone.
    const DwellResult& result() const { return result_; }

    // Arms a dwell of `count` probes. dwell_id must be non-zero and, within
    // a run, strictly increasing (the receiver closes-by-succession on it).
    bool begin(uint32_t run_id, uint16_t dwell_id, uint16_t count,
               uint64_t now_ms) {
        if (dwell_id == 0 || count == 0) return false;
        run_id_ = run_id;
        dwell_id_ = dwell_id;
        count_ = count;
        next_seq_ = 1;
        retries_left_ = p_.tally_retries;
        state_ = DwellState::kEmitting;
        last_emit_ms_ = now_ms;
        emit_credit_us_ = p_.probe_pace_us;  // first probe goes immediately
        wait_since_ms_ = 0;
        result_ = DwellResult{};
        return true;
    }

    // Poll for the next probe to emit. Call repeatedly per tick until
    // .send == false; the caller encodes (run_id, dwell_id, seq, count_) and
    // injects. Pacing is credit-based on the injected clock.
    DwellProbeOut next_probe(uint64_t now_ms) {
        DwellProbeOut out;
        if (state_ == DwellState::kEmitting) {
            accrue_(now_ms);
            if (emitted_this_tick_ >= p_.max_probes_per_tick) return out;
            if (emit_credit_us_ < p_.probe_pace_us) return out;
            emit_credit_us_ -= p_.probe_pace_us;
            ++emitted_this_tick_;
            out.send = true;
            out.seq = next_seq_;
            if (next_seq_ == count_) {
                state_ = DwellState::kAwaitTally;
                wait_since_ms_ = now_ms;
            } else {
                ++next_seq_;
            }
            return out;
        }
        if (state_ == DwellState::kAwaitTally) {
            accrue_(now_ms);
            if (now_ms - wait_since_ms_ < p_.tally_wait_ms) return out;
            if (retries_left_ == 0) {
                // §3.16: inconclusive, never clean — the caller re-dwells
                // (bounded) and then fails the run `evidence_lost`.
                state_ = DwellState::kNoEvidence;
                return out;
            }
            --retries_left_;
            wait_since_ms_ = now_ms;
            out.send = true;  // re-elicit: the tail probe, same seq == count
            out.seq = count_;
            return out;
        }
        return out;
    }

    // Feed every decoded TALLY addressed to this node; wrong run/dwell ids
    // are ignored (a stale tally is not evidence about this dwell).
    bool on_tally(uint32_t run_id, uint16_t dwell_id, uint16_t received,
                  uint32_t rssi_sum_dbm, uint8_t rx_mcs,
                  uint8_t adapter_fingerprint) {
        if (state_ != DwellState::kEmitting &&
            state_ != DwellState::kAwaitTally) {
            return false;
        }
        if (run_id != run_id_ || dwell_id != dwell_id_) return false;
        result_.sent = count_;
        result_.received = received;
        result_.rssi_sum_dbm = rssi_sum_dbm;
        result_.rx_mcs = rx_mcs;
        result_.adapter_fingerprint = adapter_fingerprint;
        state_ = DwellState::kDone;
        return true;
    }

    void reset() { state_ = DwellState::kIdle; }
    // Call once per caller tick, before the next_probe() drain loop.
    void new_tick() { emitted_this_tick_ = 0; }

  private:
    void accrue_(uint64_t now_ms) {
        const uint64_t d = now_ms - last_emit_ms_;
        last_emit_ms_ = now_ms;
        // Saturate: an idle stretch must not bank an unbounded burst.
        const uint64_t cap =
            static_cast<uint64_t>(p_.probe_pace_us) * p_.max_probes_per_tick;
        emit_credit_us_ += d * 1000;
        if (emit_credit_us_ > cap) emit_credit_us_ = cap;
    }

    DwellSendParams p_;
    DwellState state_ = DwellState::kIdle;
    uint32_t run_id_ = 0;
    uint16_t dwell_id_ = 0;
    uint16_t count_ = 0;
    uint16_t next_seq_ = 1;
    uint32_t retries_left_ = 0;
    uint64_t last_emit_ms_ = 0;
    uint64_t emit_credit_us_ = 0;
    uint16_t emitted_this_tick_ = 0;
    uint64_t wait_since_ms_ = 0;
    DwellResult result_;
};

// ---------------------------------------------------------------------------
// Receiver half — counts probes per (run_id, dwell_id) and answers tallies.

struct DwellTallyOut {
    bool send = false;
    uint32_t run_id = 0;
    uint16_t dwell_id = 0;
    uint16_t received = 0;
    uint32_t rssi_sum_dbm = 0;  // i32 wire image (modulo-2^32 accumulation)
    uint8_t rx_mcs = kUplinkRxMcsUnknown;
    // True exactly when this probe's run_id opened a NEW run — the caller's
    // hook for the §3.16 feed pause (craft side, D-C ruling).
    bool new_run = false;
};

// Largest dwell burst the receiver can dedup exactly — bounds the §15.2
// dwell_probe_frames/dwell_verify_frames knobs (config enforces it). 1024
// bits = 128 bytes of state; well above the 500/1000 seeds.
inline constexpr uint16_t kMaxDwellFrames = 1024;

class DwellReceiver {
  public:
    // Feed every ACCEPTED probe (the caller has already applied the §3.16
    // source/destination gate). rssi is the accepted copy's AirRxMeta.rssi;
    // rx_mcs its delivered MCS. `received` counts DISTINCT seq via a bitmap
    // — exact under cross-adapter diversity duplication and reorder, which
    // a high-water scheme would undercount into overstated loss.
    DwellTallyOut on_probe(uint32_t run_id, uint16_t dwell_id, uint16_t seq,
                           uint16_t count, int8_t rssi, uint8_t rx_mcs,
                           uint64_t now_ms) {
        DwellTallyOut out;
        last_probe_ms_ = now_ms;
        if (run_id != run_id_) {
            // New run: forget everything, including the closed-tally memory.
            run_id_ = run_id;
            open_dwell_ = 0;
            closed_dwell_ = 0;
            out.new_run = true;
        }
        if (dwell_id == closed_dwell_ && closed_dwell_ != 0) {
            // Late/duplicate probe of a closed dwell: idempotent re-send of
            // the same tally (the sender's re-elicitation path).
            out.send = true;
            fill_(out, closed_tally_);
            return out;
        }
        if (dwell_id < open_dwell_ || dwell_id < closed_dwell_) {
            return out;  // stale dwell — nothing useful to say
        }
        if (dwell_id != open_dwell_) {
            // First probe of a later dwell. Close-by-succession: the open
            // dwell's tail (and our tally chance) may be gone — emit its
            // tally now, then open the new dwell.
            if (open_dwell_ != 0) {
                close_();
                out.send = true;
                fill_(out, closed_tally_);
            }
            open_dwell_ = dwell_id;
            acc_ = Acc{};
        }
        // Count distinct seq exactly; the tail re-elicit (seq == count,
        // already seen) and diversity duplicates must not re-count. seq past
        // the bitmap cannot be booked distinct — ignored (config bounds the
        // burst to kMaxDwellFrames, so this only fires on a hostile frame).
        if (seq <= kMaxDwellFrames && !acc_.test_set(seq)) {
            ++acc_.received;
            acc_.rssi_sum += static_cast<uint32_t>(static_cast<int32_t>(rssi));
            acc_.rx_mcs = rx_mcs;
        }
        if (seq == count) {
            // Tail observed: close and answer immediately.
            close_();
            // Overwrite any close-by-succession payload with this dwell's.
            out.send = true;
            fill_(out, closed_tally_);
        }
        return out;
    }

    // Run expiry — the caller polls this with its feed-pause timeout
    // (`feed_quiet_ms`) to resume a paused feed; expiring also forgets the
    // run so a stale re-elicit cannot resurrect it.
    bool quiet_for(uint64_t now_ms, uint32_t timeout_ms) const {
        return run_id_ != 0 && now_ms - last_probe_ms_ >= timeout_ms;
    }
    void expire_run() {
        run_id_ = 0;
        open_dwell_ = 0;
        closed_dwell_ = 0;
    }
    uint32_t run_id() const { return run_id_; }
    uint64_t last_probe_ms() const { return last_probe_ms_; }

  private:
    struct Acc {
        uint16_t received = 0;
        uint32_t rssi_sum = 0;
        uint8_t rx_mcs = kUplinkRxMcsUnknown;
        std::array<uint64_t, kMaxDwellFrames / 64> seen{};
        // Returns the prior bit for 1-based seq, setting it.
        bool test_set(uint16_t seq) {
            const uint16_t i = seq - 1;
            uint64_t& w = seen[i >> 6];
            const uint64_t bit = uint64_t{1} << (i & 63);
            const bool had = (w & bit) != 0;
            w |= bit;
            return had;
        }
    };
    struct Closed {
        uint16_t dwell_id = 0;
        uint16_t received = 0;
        uint32_t rssi_sum = 0;
        uint8_t rx_mcs = kUplinkRxMcsUnknown;
    };
    void close_() {
        closed_dwell_ = open_dwell_;
        closed_tally_.dwell_id = open_dwell_;
        closed_tally_.received = acc_.received;
        closed_tally_.rssi_sum = acc_.rssi_sum;
        closed_tally_.rx_mcs = acc_.rx_mcs;
        open_dwell_ = 0;
        acc_ = Acc{};
    }
    void fill_(DwellTallyOut& out, const Closed& c) const {
        out.run_id = run_id_;
        out.dwell_id = c.dwell_id;
        out.received = c.received;
        out.rssi_sum_dbm = c.rssi_sum;
        out.rx_mcs = c.rx_mcs;
    }

    uint32_t run_id_ = 0;
    uint16_t open_dwell_ = 0;
    uint16_t closed_dwell_ = 0;
    Acc acc_;
    Closed closed_tally_;
    uint64_t last_probe_ms_ = 0;
};

}  // namespace wblink
