// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link io: §9.6 venc bitrate actuator — NON-BLOCKING HTTP/1.0 GET
// `/api/v1/live/set?video0.bitrate=<kbps>` over a plain POSIX TCP socket to
// the same-SoC encoder (MUT_LIVE, sub-ms server side).
//
// B1 (pre-flight audit): the setters record the DESIRED value only; poll(),
// called once per event-loop iteration, drives at most one HTTP transaction
// through a non-blocking connect/send/recv state machine, spending only what
// a zero-timeout poll() costs per iteration. A wedged-but-accepting venc
// (process alive, never draining) can no longer stall the flight loop — and
// with it csa.tick(), ARQ service and the §7.2 quiet-gap guard — the way the
// old blocking ~600 ms (200 ms × connect/send/recv) budget could. A hard
// per-transaction wall deadline reclaims a half-open socket so a zombie
// transaction cannot wedge actuation forever. A dead venc stays harmless
// (ECONNREFUSED is instant, blocking or not).
//
// VOLATILE-FIRST (§9.6 Pass 73): every push targets /api/v1/live/set (no
// flash write); a 404 re-sends the push on the persisting /api/v1/set
// (pre-live venc) as a chained transaction. The fallback latches only when
// that /set re-send succeeds — venc 404s everything for seconds while its
// pipeline brings up (httpd binds before routes register), and a permanent
// latch off one transient 404 silently reintroduces the flash wear this
// exists to stop — and a latched fallback re-probes /live/set every 10 min so
// it heals. WRITE-ON-CHANGE stays load-bearing (§9.6): flash wear on the
// fallback path, pointless HTTP churn on the live path. A setter is a no-op
// when the target equals the last SUCCESSFULLY pushed value.
//
// Failures are counted, never fatal: the transport must keep flying on a
// stuck encoder API (the §9.8 fail-safe handles the rest).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "wblink/config.h"

namespace wblink {

class VencActuator {
  public:
    explicit VencActuator(const VencCfg& cfg) : cfg_(cfg) {}
    ~VencActuator() { close_conn(); }

    // Advance the in-flight HTTP transaction (if any) and, when idle, start the
    // next pending one. MUST be called once per event-loop iteration. Never
    // blocks: every socket op is non-blocking and every poll() here uses a
    // zero timeout.
    void poll(uint64_t now_ms);

    // Record the desired kbps (write-on-change vs the last SUCCESSFULLY pushed
    // value). The push itself happens in poll(). No-op when disabled or already
    // at the target. Callers may invoke this every tick.
    void set_bitrate(uint32_t kbps);
    // §9.6 Pass 37 horizon caps: one /set carrying both fields (venc applies
    // them as one live group). Same write-on-change discipline; gated on
    // cfg.enabled AND cfg.frame_caps. {0,0} is a no-op (never command
    // "unlimited" implicitly).
    void set_max_frame_size(uint32_t max_i_bytes, uint32_t max_p_bytes);
    // §9.11 FPS ladder: write-on-change like the others; venc applies fps
    // live (skipping no-op rebinds) and requests an IDR after a real change.
    void set_fps(uint16_t fps);
    // Request a decoder-recovery IDR. Returns true when the request was queued
    // for send (recovery enabled, not inside the 1 s rate gate). The HTTP
    // outcome is asynchronous — poll() sends it and counts idr_failures.
    bool request_idr(uint64_t now_ms);

    // §15.5 Pass 103: a venc restart (the §16 mode applier) discards the
    // encoder's live/volatile bitrate/caps/fps but leaves this actuator's
    // write-on-change cache intact, so the setters would see want_==last_ and
    // never re-push — the encoder is stranded at its persisted config. Called
    // AFTER the restart (POST /api/v1/venc/reassert), this drops the cache so
    // the next poll() re-asserts every commanded value onto the fresh encoder.
    // want_* (the desired targets) are untouched; last_* are cleared so they
    // read as pending. The volatile path is re-probed fresh (a fallback latch
    // from before the restart no longer applies) and the retry holdoff is
    // cleared so re-assertion is not delayed a further window.
    void invalidate();

    uint64_t pushes() const { return pushes_; }
    uint64_t failures() const { return failures_; }
    uint64_t idr_requests() const { return idr_requests_; }
    uint64_t idr_failures() const { return idr_failures_; }
    bool enabled() const { return cfg_.enabled; }
    bool recovery_enabled() const { return cfg_.recovery_enabled; }
    bool frame_caps_enabled() const { return cfg_.enabled && cfg_.frame_caps; }
    // §15.3 actuator state: last commanded values (0 = never pushed) and the
    // doc-model "pending transition" — within settle_ms of the last accepted
    // value-changing push.
    uint32_t commanded_bitrate_kbps() const { return last_ ? *last_ : 0; }
    uint32_t commanded_max_i_bytes() const {
        return last_caps_ ? last_caps_->first : 0;
    }
    uint32_t commanded_max_p_bytes() const {
        return last_caps_ ? last_caps_->second : 0;
    }
    uint16_t commanded_fps() const { return last_fps_ ? *last_fps_ : 0; }
    bool settling(uint64_t now_ms) const {
        return last_change_ms_ != 0 &&
               now_ms < last_change_ms_ + cfg_.settle_ms;
    }
    // §9.6 Pass 73: true once a 404 latched the persisting-/set fallback.
    bool live_fallback() const { return live_fallback_; }
    // B1: an HTTP transaction is in flight (a poll() step is pending).
    bool busy() const { return phase_ != HttpPhase::kIdle; }

  private:
    enum class HttpPhase { kIdle, kConnecting, kSending, kReceiving };
    enum class TxnKind { kBitrate, kCaps, kFps, kIdr };

    void start_next_txn(uint64_t now_ms);   // pick pending work, open socket
    void advance_txn(uint64_t now_ms);      // step the in-flight state machine
    void finish_txn(int status, uint64_t now_ms);  // status: HTTP code, 0=xport
    void close_conn();                      // close fd, back to kIdle
    bool select_pending(uint64_t now_ms);   // sets txn_* from want_ vs last_
    std::string txn_query() const;          // the "video0.X=Y" for the txn kind
    static int parse_status(const char* buf, size_t len);  // 0 = unparseable

    // A latched fallback re-probes /live/set at this cadence: a venc
    // upgrade (or a transient bring-up 404 wrongly read as pre-live) heals
    // without a link restart.
    static constexpr uint64_t kLiveReprobeMs = 600000;  // 10 min
    // Hard wall-clock ceiling on one transaction: a wedged-but-accepting venc
    // that connects then never answers must not pin the connection forever.
    // Spent across non-blocking poll()s, so it never blocks the loop.
    static constexpr uint64_t kTxnDeadlineMs = 500;

    VencCfg cfg_;
    bool live_fallback_ = false;
    uint64_t live_reprobe_ms_ = 0;
    std::optional<uint32_t> last_;
    std::optional<std::pair<uint32_t, uint32_t>> last_caps_;
    std::optional<uint16_t> last_fps_;
    uint64_t last_change_ms_ = 0;
    uint64_t no_retry_until_ms_ = 0;
    uint64_t pushes_ = 0;
    uint64_t failures_ = 0;
    uint64_t next_idr_ms_ = 0;
    uint64_t idr_requests_ = 0;
    uint64_t idr_failures_ = 0;

    // Desired state — recorded by the setters, reconciled by poll(). A value
    // differing from its last_ counterpart is pending work.
    std::optional<uint32_t> want_bitrate_;
    std::optional<std::pair<uint32_t, uint32_t>> want_caps_;
    std::optional<uint16_t> want_fps_;
    bool want_idr_ = false;
    // One-shot: a RECOVERY_REQUEST refused because venc.recovery_enabled is
    // false is otherwise a completely silent failure.
    bool recovery_disabled_warned_ = false;

    // Non-blocking HTTP transaction state.
    HttpPhase phase_ = HttpPhase::kIdle;
    int fd_ = -1;
    std::string req_buf_;
    size_t req_sent_ = 0;
    char resp_buf_[128] = {};
    size_t resp_len_ = 0;
    uint64_t txn_deadline_ms_ = 0;
    TxnKind txn_kind_ = TxnKind::kBitrate;
    bool txn_persist_ = false;      // false = /live/set attempt, true = /set
    bool txn_latch_on_ok_ = false;  // reached /set via a 404 -> latch on success
    bool chain_persist_ = false;    // a 404 wants the same value re-sent on /set
    // The value snapshotted at txn start (also the value re-sent on a 404).
    uint32_t txn_bitrate_ = 0;
    std::pair<uint32_t, uint32_t> txn_caps_{0, 0};
    uint16_t txn_fps_ = 0;
};

}  // namespace wblink
