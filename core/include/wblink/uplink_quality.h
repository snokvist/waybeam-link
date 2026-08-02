// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link core: §3.16 (Pass 125) UPLINK_QUALITY endpoints — the craft's
// cumulative accumulator and the ground's authenticated accept gate. Pure and
// time-injected like the §11 engines: no sockets, no clocks, no radio.
//
// The two halves are here together because they are one contract read from
// opposite ends, and every field's meaning (reset domain, wrap arithmetic,
// what counts as a sample) has to agree byte-for-byte or §10.7 divides by a
// number that does not mean what it says.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "wblink/hmac_sha256.h"
#include "wblink/types.h"
#include "wblink/wire.h"

namespace wblink {

// ---- craft side ------------------------------------------------------------

// Cumulative counters for the craft's currently accepted §3.5 reporter. The
// caller folds in ONLY reports that passed both the report-authority gate and
// the selector freshness check: a redundant copy carries no new epoch, and
// counting it would tell the ground its uplink delivered more than it did.
class UplinkQualityCounters {
  public:
    // A new reporter tuple starts a fresh domain — a ground must never read
    // the previous one's history as its own delivery.
    void note_accepted(uint16_t originator, uint32_t session, uint32_t epoch,
                       int8_t rssi, uint8_t rx_mcs) {
        if (originator != originator_ || session != session_) {
            originator_ = originator;
            session_ = session;
            last_epoch_ = 0;
            reports_ = 0;
            rssi_sum_ = 0;
            last_rx_mcs_ = kUplinkRxMcsUnknown;
        }
        last_epoch_ = epoch;
        ++reports_;
        // Unsigned: §3.16 accumulation is modulo 2^32 and the field is a
        // two's-complement wire image. Signed overflow here would be UB.
        rssi_sum_ += static_cast<uint32_t>(static_cast<int32_t>(rssi));
        last_rx_mcs_ = rx_mcs;
    }

    void reset() { *this = UplinkQualityCounters{}; }

    uint16_t target_originator() const { return originator_; }
    uint32_t target_session() const { return session_; }
    uint32_t reports_received() const { return reports_; }
    uint32_t last_report_epoch() const { return last_epoch_; }
    uint32_t rssi_sum() const { return rssi_sum_; }
    uint8_t last_rx_mcs() const { return last_rx_mcs_; }

    // Build the authenticated packet. nullopt when there is nothing honest to
    // say: no PSK (this moves a power actuator — unauthenticated is not an
    // option), no latched reporter, or no accepted report yet in this domain.
    std::optional<UplinkQuality> build(uint16_t self_originator,
                                       uint32_t self_session,
                                       uint8_t fingerprint,
                                       const std::string& psk) const {
        if (psk.empty() || originator_ == 0 || reports_ == 0) {
            return std::nullopt;
        }
        UplinkQuality q;
        q.prefix = {self_originator, originator_, self_session};
        q.target_originator = originator_;
        q.target_session = session_;
        q.last_report_epoch = last_epoch_;
        q.reports_received = reports_;
        q.rssi_sum_dbm = rssi_sum_;
        q.craft_adapter_fingerprint = fingerprint;
        q.last_rx_mcs = last_rx_mcs_;
        uint8_t buf[kUplinkQualitySize];
        if (encode_uplink_quality(q, buf, sizeof(buf)) != kUplinkQualitySize) {
            return std::nullopt;
        }
        q.quality_mac =
            quality_mac(reinterpret_cast<const uint8_t*>(psk.data()),
                        psk.size(), buf);
        return q;
    }

  private:
    uint16_t originator_ = 0;
    uint32_t session_ = 0;
    uint32_t last_epoch_ = 0;
    uint32_t reports_ = 0;
    uint32_t rssi_sum_ = 0;
    uint8_t last_rx_mcs_ = kUplinkRxMcsUnknown;
};

// ---- ground side -----------------------------------------------------------

// §3.16 two clocks. `liveness` advances on ANY accepted packet — it is
// evidence the craft is alive, still targeting us, and still airing DATA.
// `progress` advances only when reports_received moves. §10.7 reads them
// separately: liveness loss aborts the run, stalled counters under live
// feedback are a 1000permille loss observation. Collapsing them would make a
// dead uplink indistinguishable from a dead craft — and since the seek starts
// at min_qdb, that is where every run begins.
struct QualitySample {
    bool accepted = false;   // passed every §3.16 gate
    bool progressed = false; // reports_received advanced (a real sample)
    bool resynced = false;   // the counter domain restarted under us
    uint32_t reports_delta = 0;
    uint32_t epoch_delta = 0;
    int32_t rssi_sum_delta = 0;
};

class UplinkQualityGate {
  public:
    // psk and the local identity are fixed for the process; the selected
    // craft can change, so it is passed per call.
    UplinkQualityGate(std::string psk, uint16_t self_originator,
                      uint32_t self_session)
        : psk_(std::move(psk)),
          self_originator_(self_originator),
          self_session_(self_session) {}

    // selected_craft/session: the craft this ground currently takes DATA from.
    // A zero originator means "nothing selected" and rejects everything.
    QualitySample accept(const UplinkQuality& q, uint16_t selected_craft,
                         uint32_t selected_session, uint64_t now_ms) {
        QualitySample s;
        if (psk_.empty() || selected_craft == 0) return s;
        if (q.prefix.originator != selected_craft ||
            q.prefix.session_id != selected_session) {
            ++rejected_;
            return s;
        }
        if (q.target_originator != self_originator_ ||
            q.target_session != self_session_) {
            ++rejected_;
            return s;
        }
        uint8_t buf[kUplinkQualitySize];
        if (encode_uplink_quality(q, buf, sizeof(buf)) != kUplinkQualitySize) {
            ++rejected_;
            return s;
        }
        if (quality_mac(reinterpret_cast<const uint8_t*>(psk_.data()),
                        psk_.size(), buf) != q.quality_mac) {
            ++rejected_;
            return s;
        }
        // A source/target change starts a fresh receive domain: the previous
        // craft's cumulative state is not a baseline for this one.
        if (!have_ || q.prefix.originator != last_.prefix.originator ||
            q.prefix.session_id != last_.prefix.session_id) {
            return rebaseline_(s, q, now_ms);
        }
        // Wrap-aware ordering: a backward counter is a replay, not a wrap.
        // Deltas are bounded by a calibrator run, so anything in the top half
        // of the u32 space is backward, not a very large forward step.
        const uint32_t d_reports = q.reports_received - last_.reports_received;
        const uint32_t d_epoch = q.last_report_epoch - last_.last_report_epoch;
        if (d_reports > kBackwardThreshold || d_epoch > kBackwardThreshold) {
            // The craft resets its counters whenever ITS accepted reporter
            // tuple changes (§3.16) — which happens without the craft's own
            // originator/session moving, so the domain check above cannot see
            // it. Treating that permanently as a replay wedged the gate:
            // reproduced at 519 consecutive rejects, i.e. every packet for the
            // rest of the process. A SUSTAINED backward run is a reset; an
            // isolated one is still refused, so a replayed packet interleaved
            // with the live 2 Hz stream never reaches the threshold.
            if (++backward_ >= kResyncAfter) {
                ++resyncs_;
                QualitySample r = rebaseline_(s, q, now_ms);
                r.resynced = true;
                return r;
            }
            ++rejected_;
            return s;
        }
        backward_ = 0;
        s.accepted = true;
        liveness_ms_ = now_ms;  // any accepted packet is liveness
        if (d_reports == 0) {
            // Duplicate, or a live craft whose uplink is delivering nothing.
            // Both refresh liveness and neither is a sample; §10.7 tells them
            // apart by whether a dwell ever completes.
            last_ = q;
            return s;
        }
        s.progressed = true;
        s.reports_delta = d_reports;
        s.epoch_delta = d_epoch;
        s.rssi_sum_delta =
            static_cast<int32_t>(q.rssi_sum_dbm - last_.rssi_sum_dbm);
        last_ = q;
        return s;
    }

    bool live(uint64_t now_ms, uint32_t timeout_ms) const {
        return have_ && now_ms - liveness_ms_ <= timeout_ms;
    }
    uint64_t liveness_ms() const { return liveness_ms_; }
    bool have() const { return have_; }
    const UplinkQuality& last() const { return last_; }
    uint32_t rejected() const { return rejected_; }
    uint32_t resyncs() const { return resyncs_; }

  private:
    // Half the u32 space: forward deltas in a run are thousands at most.
    static constexpr uint32_t kBackwardThreshold = 0x8000'0000u;
    // Consecutive backward packets that mean "reset", not "replay". At the
    // 2 Hz §3.16 cadence this is 1.5 s — long enough that a live stream
    // interleaves and short enough that a run is not lost to it.
    static constexpr uint32_t kResyncAfter = 3;

    QualitySample rebaseline_(QualitySample s, const UplinkQuality& q,
                              uint64_t now_ms) {
        last_ = q;
        have_ = true;
        liveness_ms_ = now_ms;
        backward_ = 0;
        s.accepted = true;
        return s;  // baseline only — no delta to report yet
    }

    std::string psk_;
    uint16_t self_originator_ = 0;
    uint32_t self_session_ = 0;
    UplinkQuality last_{};
    bool have_ = false;
    uint64_t liveness_ms_ = 0;
    uint32_t rejected_ = 0;
    uint32_t backward_ = 0;
    uint32_t resyncs_ = 0;
};

}  // namespace wblink
