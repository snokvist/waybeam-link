// SPDX-License-Identifier: GPL-2.0-or-later
#include "wblink/venc.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wblink/binding.h"
#include "wblink/log.h"

namespace wblink {

// ---- setters: record the desired value; poll() does the I/O (B1) -----------

void VencActuator::set_bitrate(uint32_t kbps) {
    if (!cfg_.enabled) {
        return;
    }
    want_bitrate_ = kbps;  // §9.6 write-on-change resolved against last_ in poll
}

void VencActuator::set_max_frame_size(uint32_t max_i_bytes,
                                      uint32_t max_p_bytes) {
    if (!cfg_.enabled || !cfg_.frame_caps) {
        return;
    }
    if (max_i_bytes == 0 && max_p_bytes == 0) {
        return;  // §9.6: insufficient cap inputs — leave venc alone
    }
    want_caps_ = {max_i_bytes, max_p_bytes};
}

void VencActuator::set_fps(uint16_t fps) {
    if (!cfg_.enabled || fps == 0) {
        return;
    }
    want_fps_ = fps;
}

bool VencActuator::request_idr(uint64_t now_ms) {
    if (!cfg_.recovery_enabled) {
        return false;
    }
    if (now_ms < next_idr_ms_) {
        return true;  // inside the 1 s rate gate — treated as recently requested
    }
    if (now_ms < no_retry_until_ms_) {
        // B1: honour the shared post-failure hold-off; the RecoveryRequest path
        // re-offers, so a wedged venc is not re-poked here.
        return false;
    }
    want_idr_ = true;
    return true;
}

void VencActuator::invalidate() {
    // §15.5 Pass 103: forget everything we believe the encoder holds. A venc
    // restart (the §16 mode applier) wiped its live/volatile state, so drop any
    // transaction in flight to the now-dead process and clear the
    // write-on-change cache. want_* (the desired targets) stay; with last_*
    // empty the next poll() re-asserts bitrate + caps + fps onto the fresh
    // encoder. Re-probe the volatile path (a pre-restart fallback latch no
    // longer applies) and clear the retry hold-off so re-assertion is prompt.
    close_conn();
    last_.reset();
    last_caps_.reset();
    last_fps_.reset();
    last_change_ms_ = 0;
    live_fallback_ = false;
    live_reprobe_ms_ = 0;
    no_retry_until_ms_ = 0;
}

// ---- non-blocking transaction engine ---------------------------------------

int VencActuator::parse_status(const char* buf, size_t len) {
    // "HTTP/1.x <code> ..." — return the code (0 on a garbled/short line).
    const void* sp = std::memchr(buf, ' ', len);
    if (sp == nullptr) {
        return 0;
    }
    const char* p = static_cast<const char*>(sp) + 1;
    if (p >= buf + len || *p < '1' || *p > '9') {
        return 0;
    }
    return std::atoi(p);
}

void VencActuator::close_conn() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    phase_ = HttpPhase::kIdle;
    req_buf_.clear();
    req_sent_ = 0;
    resp_len_ = 0;
}

std::string VencActuator::txn_query() const {
    switch (txn_kind_) {
        case TxnKind::kBitrate:
            return "video0.bitrate=" + std::to_string(txn_bitrate_);
        case TxnKind::kCaps:
            return "video0.maxIBytes=" + std::to_string(txn_caps_.first) +
                   "&video0.maxPBytes=" + std::to_string(txn_caps_.second);
        case TxnKind::kFps:
            return "video0.fps=" + std::to_string(txn_fps_);
        case TxnKind::kIdr:
            return std::string();  // idr uses a fixed path, no query
    }
    return std::string();
}

// Choose the next pending transaction from want_ vs last_. Returns false when
// there is nothing to do. Priority: finish a 404 fallback chain, then caps,
// bitrate, fps, idr. §9.6 Pass 112: SigmaStar cap application perturbs RC, so
// bitrate must be the final write when both are pending.
bool VencActuator::select_pending(uint64_t now_ms) {
    if (chain_persist_) {
        return true;  // txn_* already holds the value that drew the 404
    }
    if (want_caps_ && (!last_caps_ || *want_caps_ != *last_caps_)) {
        txn_kind_ = TxnKind::kCaps;
        txn_caps_ = *want_caps_;
        return true;
    }
    if (want_bitrate_ && (!last_ || *want_bitrate_ != *last_)) {
        txn_kind_ = TxnKind::kBitrate;
        txn_bitrate_ = *want_bitrate_;
        return true;
    }
    if (want_fps_ && (!last_fps_ || *want_fps_ != *last_fps_)) {
        txn_kind_ = TxnKind::kFps;
        txn_fps_ = *want_fps_;
        return true;
    }
    if (want_idr_ && now_ms >= next_idr_ms_) {
        txn_kind_ = TxnKind::kIdr;
        return true;
    }
    return false;
}

void VencActuator::start_next_txn(uint64_t now_ms) {
    if (phase_ != HttpPhase::kIdle || now_ms < no_retry_until_ms_) {
        return;
    }
    if (!select_pending(now_ms)) {
        return;
    }

    // Path selection (§9.6 volatile-first). A 404 chain and the idr request go
    // to fixed paths; a fresh push probes /live/set unless the fallback is
    // latched and the 10-min re-probe window has not opened.
    std::string path;
    if (txn_kind_ == TxnKind::kIdr) {
        want_idr_ = false;
        next_idr_ms_ = now_ms + 1000;  // §9.6 rate gate anchored to the send
        ++idr_requests_;
        txn_persist_ = false;
        txn_latch_on_ok_ = false;
        path = "/request/idr";
    } else if (chain_persist_) {
        chain_persist_ = false;
        txn_persist_ = true;
        txn_latch_on_ok_ = true;  // this /set was reached via a 404
        path = "/api/v1/set?" + txn_query();
    } else {
        const bool probe = !live_fallback_ || now_ms >= live_reprobe_ms_;
        txn_persist_ = !probe;
        txn_latch_on_ok_ = false;
        path = (probe ? "/api/v1/live/set?" : "/api/v1/set?") + txn_query();
        ++pushes_;
    }

    const auto hp = split_host_port(cfg_.host);
    if (!hp) {
        finish_txn(0, now_ms);
        return;
    }
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        finish_txn(0, now_ms);
        return;
    }
    const int fl = ::fcntl(fd_, F_GETFL, 0);
    if (fl < 0 || ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK) < 0) {
        finish_txn(0, now_ms);
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(hp.value->second);
    if (::inet_pton(AF_INET, hp.value->first.c_str(), &addr.sin_addr) != 1) {
        finish_txn(0, now_ms);  // venc lives at a literal IP; no DNS here
        return;
    }
    req_buf_ = "GET " + path + " HTTP/1.0\r\nHost: " + cfg_.host + "\r\n\r\n";
    req_sent_ = 0;
    resp_len_ = 0;
    txn_deadline_ms_ = now_ms + kTxnDeadlineMs;

    const int rc =
        ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        phase_ = HttpPhase::kSending;  // loopback: often connects immediately
    } else if (errno == EINPROGRESS) {
        phase_ = HttpPhase::kConnecting;
    } else {
        finish_txn(0, now_ms);  // ECONNREFUSED (dead venc) etc. — instant
    }
}

void VencActuator::advance_txn(uint64_t now_ms) {
    if (phase_ == HttpPhase::kIdle) {
        return;
    }
    if (now_ms >= txn_deadline_ms_) {
        finish_txn(0, now_ms);  // wedged-but-accepting venc: reclaim the socket
        return;
    }
    pollfd pfd{fd_, 0, 0};
    switch (phase_) {
        case HttpPhase::kConnecting:
            pfd.events = POLLOUT;
            break;
        case HttpPhase::kSending:
            pfd.events = POLLOUT;
            break;
        case HttpPhase::kReceiving:
            pfd.events = POLLIN;
            break;
        case HttpPhase::kIdle:
            return;
    }
    const int pr = ::poll(&pfd, 1, 0);  // zero timeout: never blocks the loop
    if (pr <= 0) {
        return;  // not ready yet (or EINTR); retry next poll()
    }

    if (phase_ == HttpPhase::kConnecting) {
        int err = 0;
        socklen_t elen = sizeof(err);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 ||
            err != 0) {
            finish_txn(0, now_ms);  // connect failed (refused/unreachable)
            return;
        }
        phase_ = HttpPhase::kSending;
        // fall through opportunistically: the socket is writable now
    }

    if (phase_ == HttpPhase::kSending) {
        const size_t remaining = req_buf_.size() - req_sent_;
        // MSG_NOSIGNAL: a venc that died between connect and send RSTs the
        // socket; without this the SIGPIPE default would kill the link process.
        const ssize_t n = ::send(fd_, req_buf_.data() + req_sent_, remaining,
                                 MSG_NOSIGNAL);
        if (n > 0) {
            req_sent_ += static_cast<size_t>(n);
            if (req_sent_ >= req_buf_.size()) {
                phase_ = HttpPhase::kReceiving;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
            finish_txn(0, now_ms);
        }
        return;
    }

    if (phase_ == HttpPhase::kReceiving) {
        const size_t cap = sizeof(resp_buf_) - 1 - resp_len_;
        const ssize_t n =
            cap > 0 ? ::recv(fd_, resp_buf_ + resp_len_, cap, 0) : 0;
        if (n > 0) {
            resp_len_ += static_cast<size_t>(n);
            // The status line is all we need; parse as soon as it is complete.
            const int st = parse_status(resp_buf_, resp_len_);
            if (st != 0 || resp_len_ >= sizeof(resp_buf_) - 1) {
                finish_txn(st, now_ms);
            }
        } else if (n == 0) {
            finish_txn(parse_status(resp_buf_, resp_len_), now_ms);  // peer EOF
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            finish_txn(0, now_ms);
        }
    }
}

void VencActuator::finish_txn(int status, uint64_t now_ms) {
    close_conn();
    const bool ok = status >= 200 && status < 300;

    if (txn_kind_ == TxnKind::kIdr) {
        if (!ok) {
            ++idr_failures_;
            no_retry_until_ms_ = now_ms + 500;  // B1: shared holdoff
        }
        return;
    }

    // A commit lands last_* and the §9.6 settling anchor, from either path.
    auto commit = [&]() {
        switch (txn_kind_) {
            case TxnKind::kBitrate:
                last_ = txn_bitrate_;
                break;
            case TxnKind::kCaps:
                last_caps_ = txn_caps_;
                break;
            case TxnKind::kFps:
                last_fps_ = txn_fps_;
                break;
            case TxnKind::kIdr:
                break;
        }
        last_change_ms_ = now_ms;  // §9.6 settling window anchor
    };

    if (!txn_persist_) {  // this was the /live/set attempt
        if (ok) {
            commit();
            if (live_fallback_) {
                live_fallback_ = false;
                wb_logf("venc: /api/v1/live/set available — "
                                "volatile path restored\n");
            }
        } else if (status == 404) {
            // §9.6 Pass 73: re-send the SAME push on the persisting /set; the
            // fallback latches only if that succeeds (both failing = venc
            // restarting, retry later). txn_* still holds the value.
            chain_persist_ = true;
        } else {
            ++failures_;  // transport/5xx: no latch, holdoff retries
            no_retry_until_ms_ = now_ms + 500;
        }
        return;
    }

    // this was the persisting /set (either a 404 chain or a latched skip)
    if (ok) {
        commit();
        if (txn_latch_on_ok_) {
            if (!live_fallback_) {
                live_fallback_ = true;
                wb_logf("venc: /api/v1/live/set unsupported (pre-live "
                        "venc) — falling back to persisting /api/v1/set\n");
            }
            live_reprobe_ms_ = now_ms + kLiveReprobeMs;
        }
    } else {
        ++failures_;
        no_retry_until_ms_ = now_ms + 500;
    }
}

void VencActuator::poll(uint64_t now_ms) {
    advance_txn(now_ms);
    start_next_txn(now_ms);
}

}  // namespace wblink
