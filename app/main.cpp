// SPDX-License-Identifier: GPL-2.0-or-later
// waybeam-link — one portable binary, modes tx / rx / loopback (PROTOCOL.md
// §16.1). Steps 1–10 are live: wire codec, I/O/config/stats, framer + ring,
// merged RX engine, resend scheduler, loopback bench, udp-air dev backend,
// NAL classifier, §9 selector + §10 power, the devourer radio backend
// (§3.0) with the §7.2 TSF quiet-gap pacer, and the §11 follow-me CSA
// (craft follower / ground issuer, triggered via POST /api/v1/csa), and the
// §15.5 REST control plane (stats + live knobs; stdin CSA trigger is gone).
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <deque>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "wblink/air_udp.h"
#include "wblink/binding.h"
#include "wblink/cache_controller.h"
#include "wblink/cache_assignment.h"
#include "wblink/cache_store.h"
#include "wblink/cache_udp.h"
#include "wblink/config.h"
#include "wblink/control_server.h"
#include "wblink/modes.h"
#include "wblink/csa.h"
#include "wblink/vehicle_cmd.h"
#include "wblink/endian.h"
#include "wblink/fps_ladder.h"
#include "wblink/frame_caps.h"
#include "wblink/frame_framer.h"
#include "wblink/frame_reassembler.h"
#include "wblink/frame_shm.h"
#include "wblink/frame_shm_format.h"
#include "wblink/framer.h"
#include "wblink/jscc_runtime_shadow.h"
#include "wblink/loss_model.h"
#include "wblink/power.h"
#include "wblink/power_file.h"
#include "wblink/quietgap.h"
#include "wblink/recovery.h"
#include "wblink/report_gate.h"
#include "wblink/reporter.h"
#include "wblink/ring.h"
#include "wblink/rx.h"
#include "wblink/scheduler.h"
#include "wblink/selector.h"
#include "wblink/calib_store.h"
#include "wblink/calibrate.h"
#include "wblink/selector_state.h"
#include "wblink/stats.h"
#include "wblink/table.h"
#include "wblink/airtime.h"
#include "wblink/txwedge.h"
#include "wblink/venc.h"
#include "wblink/video_slot_cadence.h"
#include "wblink/air_mon.h"
#if WBLINK_RADIO
#include "wblink/air_radio.h"
#endif

namespace {

using namespace wblink;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// §15.5 Pass 104: where GET /api/v1/modes enumerates from. Explicit
// venc.modes_dir wins; otherwise derive it from mode_apply_cmd's directory (the
// §16 layout co-locates the applier and the modes/<name>.json files), so a
// deployed craft serves the catalog with no extra config. "" when neither set.
std::string mode_catalog_dir(const wblink::VencCfg& venc) {
    if (!venc.modes_dir.empty()) return venc.modes_dir;
    const auto slash = venc.mode_apply_cmd.find_last_of('/');
    if (slash != std::string::npos) return venc.mode_apply_cmd.substr(0, slash);
    return {};
}

// §15.5 (Pass 96): fork a detached operating-mode applier. Applying a mode
// restarts venc (sensor.mode/video0.size are restart_required), which takes
// seconds, so this must NOT block the flight loop — double-fork so the
// grandchild is reparented to init (no zombie, nothing to wait on) and execl
// with argv (never a shell) so the charset-validated name cannot inject.
// Precedent: RadioAir already forks execvp('iw', …) for channel retunes.
bool spawn_mode_applier(const std::string& cmd, const std::string& name) {
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setsid();  // detach from the flight process's session
        const pid_t pid2 = ::fork();
        if (pid2 == 0) {
            // Close inherited fds (the §15.5 control listen socket, UDP
            // bindings, frame-shm, …) before exec so neither the applier nor
            // the venc it restarts (S95) inherits them. A leaked control-listen
            // fd pins 8091, and the link then cannot rebind it on its next
            // restart — found on hardware while verifying Pass 100.
            const long maxfd = ::sysconf(_SC_OPEN_MAX);
            const int top = (maxfd > 0 && maxfd < 1 << 20)
                                ? static_cast<int>(maxfd)
                                : 4096;
            for (int fd = 3; fd < top; ++fd) {
                ::close(fd);
            }
            ::execl(cmd.c_str(), cmd.c_str(), name.c_str(),
                    static_cast<char*>(nullptr));
            _exit(127);  // applier not executable
        }
        _exit(0);  // intermediate child exits immediately; grandchild orphaned
    }
    int status = 0;
    ::waitpid(pid, &status, 0);  // instant: the intermediate child just exited
    return true;
}

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

class ArqTimingTracker {
  public:
    void note_eob(uint64_t at_us) { last_eob_us_ = at_us; }

    void note_nack_built(const uint8_t* frame, size_t len, uint64_t at_us) {
        const Decoded dec = decode(frame, len);
        const NackView* n = std::get_if<NackView>(&dec);
        if (n == nullptr) return;
        if (last_eob_us_ && at_us >= *last_eob_us_) {
            eob_to_build_.observe(at_us - *last_eob_us_);
        }
        for_each_seq(*n, [&](const Key& k) { built_[k] = at_us; });
        trim(built_);
    }

    void note_nack_injected(const uint8_t* frame, size_t len,
                            uint64_t at_us) {
        const Decoded dec = decode(frame, len);
        const NackView* n = std::get_if<NackView>(&dec);
        if (n == nullptr) return;
        bool sampled = false;
        for_each_seq(*n, [&](const Key& k) {
            const auto it = built_.find(k);
            if (!sampled && it != built_.end() && at_us >= it->second) {
                build_to_inject_.observe(at_us - it->second);
                sampled = true;
            }
            injected_[k] = at_us;
        });
        trim(injected_);
    }

    void note_retransmit_arrived(const DataView& v, uint64_t at_us) {
        if ((v.hdr.data_flags & data_flags::kRetransmit) == 0) return;
        const Key k = key(v.hdr.prefix.originator, v.hdr.prefix.session_id,
                          v.hdr.stream_id, v.hdr.seq);
        if (const auto it = injected_.find(k);
            it != injected_.end() && at_us >= it->second) {
            inject_to_retransmit_.observe(at_us - it->second);
            injected_.erase(it);
        }
        if (const auto it = built_.find(k);
            it != built_.end() && at_us >= it->second) {
            build_to_retransmit_.observe(at_us - it->second);
            built_.erase(it);
        }
    }

    void note_nack_received(const uint8_t* frame, size_t len,
                            uint64_t at_us) {
        const Decoded dec = decode(frame, len);
        const NackView* n = std::get_if<NackView>(&dec);
        if (n == nullptr) return;
        for_each_seq(*n, [&](const Key& k) { received_[k] = at_us; });
        trim(received_);
    }

    void note_resend_submitted(const uint8_t* frame, size_t len,
                               uint64_t at_us) {
        const Decoded dec = decode(frame, len);
        const DataView* v = std::get_if<DataView>(&dec);
        if (v == nullptr ||
            (v->hdr.data_flags & data_flags::kRetransmit) == 0) return;
        const Key k = key(v->hdr.prefix.originator,
                          v->hdr.prefix.session_id, v->hdr.stream_id,
                          v->hdr.seq);
        const auto it = received_.find(k);
        if (it != received_.end() && at_us >= it->second) {
            receive_to_resend_.observe(at_us - it->second);
            received_.erase(it);
        }
    }

    ArqTimingStats snapshot() const {
        ArqTimingStats out;
        out.eob_to_nack_build = eob_to_build_.snapshot();
        out.nack_build_to_inject = build_to_inject_.snapshot();
        out.nack_inject_to_retransmit = inject_to_retransmit_.snapshot();
        out.nack_build_to_retransmit = build_to_retransmit_.snapshot();
        out.nack_receive_to_resend = receive_to_resend_.snapshot();
        return out;
    }

    void reset() {
        eob_to_build_.reset();
        build_to_inject_.reset();
        inject_to_retransmit_.reset();
        build_to_retransmit_.reset();
        receive_to_resend_.reset();
        built_.clear();
        injected_.clear();
        received_.clear();
        last_eob_us_.reset();
    }

  private:
    class Series {
      public:
        void observe(uint64_t delta) {
            const uint32_t us = static_cast<uint32_t>(
                std::min<uint64_t>(delta, UINT32_MAX));
            ++samples_;
            max_ = std::max(max_, us);
            recent_.push_back(us);
            if (recent_.size() > 512) recent_.pop_front();
        }
        TimingMetricStats snapshot() const {
            TimingMetricStats out{samples_, 0, max_};
            if (recent_.empty()) return out;
            std::vector<uint32_t> sorted(recent_.begin(), recent_.end());
            std::sort(sorted.begin(), sorted.end());
            const size_t rank = (sorted.size() * 95 + 99) / 100;
            out.p95_us = sorted[rank - 1];
            return out;
        }
        void reset() {
            samples_ = 0;
            max_ = 0;
            recent_.clear();
        }
      private:
        uint64_t samples_ = 0;
        uint32_t max_ = 0;
        std::deque<uint32_t> recent_;
    };

    struct Key {
        uint16_t originator;
        uint32_t session;
        uint8_t stream_id;
        uint32_t seq;
        bool operator<(const Key& other) const {
            return std::tie(originator, session, stream_id, seq) <
                   std::tie(other.originator, other.session, other.stream_id,
                            other.seq);
        }
    };
    static Key key(uint16_t originator, uint32_t session, uint8_t stream_id,
                   uint32_t seq) {
        return Key{originator, session, stream_id, seq};
    }
    template <class F>
    static void for_each_seq(const NackView& n, const F& fn) {
        for (unsigned i = 0; i < static_cast<unsigned>(n.bitmap_len) * 8;
             ++i) {
            if ((n.bitmap[i / 8] & (1u << (i % 8))) != 0) {
                fn(key(n.hdr.target_originator, n.hdr.target_session,
                       n.hdr.target_stream_id, n.hdr.base_seq + i));
            }
        }
    }
    static void trim(std::map<Key, uint64_t>& m) {
        while (m.size() > 4096) m.erase(m.begin());
    }

    Series eob_to_build_;
    Series build_to_inject_;
    Series inject_to_retransmit_;
    Series build_to_retransmit_;
    Series receive_to_resend_;
    std::map<Key, uint64_t> built_;
    std::map<Key, uint64_t> injected_;
    std::map<Key, uint64_t> received_;
    std::optional<uint64_t> last_eob_us_;
};

// Bench-only, bounded packet-event trace. The wire remains the source of truth:
// this observer decodes existing frames and never feeds decisions back in.
class PacketEventTrace {
  public:
    explicit PacketEventTrace(const char* role) : role_(role) {
        const char* path = std::getenv("WBLINK_PACKET_TRACE");
        if (path == nullptr || *path == '\0') return;
        if (const char* cap = std::getenv("WBLINK_PACKET_TRACE_MAX")) {
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(cap, &end, 10);
            if (end != cap && *end == '\0' && parsed > 0) {
                cap_ = static_cast<uint64_t>(parsed);
            }
        }
        out_ = std::fopen(path, "w");
        if (out_ == nullptr) {
            std::fprintf(stderr, "packet-trace: cannot open %s\n", path);
            return;
        }
        static constexpr size_t kBufferBytes = 1024 * 1024;
        buffer_.resize(kBufferBytes);
        std::setvbuf(out_, buffer_.data(), _IOFBF, buffer_.size());
        std::fprintf(out_,
                     "{\"type\":\"schema\",\"schema\":\"waybeam-packet-events-v1\","
                     "\"role\":\"%s\",\"cap\":%llu}\n",
                     role_, static_cast<unsigned long long>(cap_));
    }

    ~PacketEventTrace() {
        if (out_ == nullptr) return;
        std::fprintf(out_,
                     "{\"type\":\"trace_end\",\"events\":%llu,"
                     "\"events_dropped\":%llu}\n",
                     static_cast<unsigned long long>(events_),
                     static_cast<unsigned long long>(dropped_));
        std::fclose(out_);
    }

    bool enabled() const { return out_ != nullptr; }
    void flush() { if (out_ != nullptr) std::fflush(out_); }

    void packet(const char* direction, const char* outcome, int adapter,
                const uint8_t* frame, size_t len) {
        if (out_ == nullptr) return;
        if (events_ >= cap_) {
            ++dropped_;
            return;
        }
        ++events_;
        const uint64_t t = now_us();
        const Decoded dec = decode(frame, len);
        if (const DataView* data = std::get_if<DataView>(&dec)) {
            const bool repair =
                (data->hdr.data_flags & data_flags::kFecRepair) != 0;
            uint16_t k = 0;
            uint16_t symbol = 0;
            uint32_t frame_len = 0;
            if (repair && data->payload_len >= kFecRepairSubheaderSize) {
                k = be16_read(data->payload + kFecOffWindowLen);
                symbol = data->payload[kFecOffRepairIdx];
                frame_len = be32_read(data->payload + kFecOffFrameLen);
            } else if (!repair && data->payload_len >= kFecSourceSubheaderSize) {
                k = be16_read(data->payload + kFecSrcOffWindowLen);
                symbol = be16_read(data->payload + kFecSrcOffSymIndex);
            }
            std::fprintf(
                out_,
                "{\"type\":\"packet\",\"t_us\":%llu,\"direction\":\"%s\","
                "\"outcome\":\"%s\",\"adapter\":%d,\"packet\":\"data\","
                "\"originator\":%u,\"session\":%u,\"stream\":%u,"
                "\"block\":%u,\"seq\":%u,\"kind\":\"%s\","
                "\"symbol\":%u,\"k\":%u,\"frame_len\":%u,"
                "\"arq\":%s,\"retransmit\":%s,\"eob\":%s,"
                "\"bytes\":%zu}\n",
                static_cast<unsigned long long>(t), direction, outcome, adapter,
                data->hdr.prefix.originator, data->hdr.prefix.session_id,
                data->hdr.stream_id, data->hdr.block_id, data->hdr.seq,
                repair ? "repair" : "source", symbol, k, frame_len,
                (data->hdr.data_flags & data_flags::kArq) ? "true" : "false",
                (data->hdr.data_flags & data_flags::kRetransmit) ? "true" : "false",
                (data->hdr.data_flags & data_flags::kEndOfBlock) ? "true" : "false",
                len);
            return;
        }
        if (const NackView* nack = std::get_if<NackView>(&dec)) {
            std::string bitmap;
            static constexpr char kHex[] = "0123456789abcdef";
            bitmap.reserve(static_cast<size_t>(nack->bitmap_len) * 2);
            for (uint8_t i = 0; i < nack->bitmap_len; ++i) {
                bitmap.push_back(kHex[nack->bitmap[i] >> 4]);
                bitmap.push_back(kHex[nack->bitmap[i] & 0x0f]);
            }
            std::fprintf(
                out_,
                "{\"type\":\"packet\",\"t_us\":%llu,\"direction\":\"%s\","
                "\"outcome\":\"%s\",\"adapter\":%d,\"packet\":\"nack\","
                "\"originator\":%u,\"session\":%u,\"stream\":%u,"
                "\"target_originator\":%u,\"target_session\":%u,"
                "\"base_seq\":%u,\"bitmap\":\"%s\",\"bytes\":%zu}\n",
                static_cast<unsigned long long>(t), direction, outcome, adapter,
                nack->hdr.prefix.originator, nack->hdr.prefix.session_id,
                nack->hdr.target_stream_id, nack->hdr.target_originator,
                nack->hdr.target_session, nack->hdr.base_seq, bitmap.c_str(), len);
            return;
        }
        std::fprintf(out_,
                     "{\"type\":\"packet\",\"t_us\":%llu,"
                     "\"direction\":\"%s\",\"outcome\":\"%s\","
                     "\"adapter\":%d,\"packet\":\"other\",\"bytes\":%zu}\n",
                     static_cast<unsigned long long>(t), direction, outcome,
                     adapter, len);
    }

  private:
    const char* role_;
    std::FILE* out_ = nullptr;
    std::vector<char> buffer_;
    uint64_t cap_ = 75000;
    uint64_t events_ = 0;
    uint64_t dropped_ = 0;
};

// Pass 17: bounded, observational catalog. It never feeds latch decisions.
class DiscoveryCatalog {
  public:
    void observe(const Decoded& dec, uint64_t now, uint8_t net_id = 0) {
        const CommonPrefix* p = nullptr;
        const DataView* data = std::get_if<DataView>(&dec);
        const Announce* an = nullptr;
        if (data != nullptr) {
            p = &data->hdr.prefix;
        } else if (const Heartbeat* hb = std::get_if<Heartbeat>(&dec)) {
            p = &hb->prefix;
        } else if ((an = std::get_if<Announce>(&dec)) != nullptr) {
            p = &an->prefix;  // §15.5 presence source (Pass 62)
        } else {
            return;
        }
        const uint64_t nk = (static_cast<uint64_t>(p->originator) << 32) |
                            p->session_id;
        Node& n = nodes_[nk];  // update-in-place keeps prior ANNOUNCE fields
        n.originator = p->originator;
        n.session = p->session_id;
        n.net_id = net_id;
        n.last_seen_ms = now;
        if (an != nullptr) {
            n.announced = true;
            n.claimed = (an->flags & announce_flags::kClaimed) != 0;
            n.claimed_by = an->claimed_by;
            n.psk_present = (an->flags & announce_flags::kPskPresent) != 0;
            if (n.psk_present) {  // Pass 63: announced token is public — cache it
                std::memcpy(n.token.data(), an->psk, kAnnouncePskSize);
                n.have_token = true;
            }
        }
        if (data != nullptr) {
            const uint64_t sk = (nk << 8) | data->hdr.stream_id;
            Stream& s = streams_[sk];
            if (s.packet_count == 0) {
                s.originator = p->originator;
                s.session = p->session_id;
                s.stream_id = data->hdr.stream_id;
                s.stream_type = data->hdr.stream_type;
                s.first_seen_ms = now;
            }
            ++s.packet_count;
            s.last_seen_ms = now;
        }
        trim();
    }

    std::string json(uint64_t now, const std::vector<StreamKey>& latched) {
        age(now);
        std::string out = "{\"nodes\":[";
        bool comma = false;
        for (const auto& [key, n] : nodes_) {
            (void)key;
            if (comma) out += ',';
            comma = true;
            out += "{\"originator\":" + std::to_string(n.originator) +
                   ",\"session\":" + std::to_string(n.session) +
                   ",\"net_id\":" + std::to_string(n.net_id) +
                   ",\"last_seen_ms\":" + std::to_string(n.last_seen_ms);
            if (n.announced) {  // §15.5 ANNOUNCE claim view (Pass 62; no token)
                out += ",\"claimed\":" + std::string(n.claimed ? "true" : "false") +
                       ",\"claimed_by\":" + std::to_string(n.claimed_by) +
                       ",\"psk_present\":" +
                       std::string(n.psk_present ? "true" : "false");
            }
            out += "}";
        }
        out += "],\"streams\":[";
        comma = false;
        for (const auto& [key, s] : streams_) {
            (void)key;
            bool is_latched = false;
            for (const StreamKey& k : latched) {
                is_latched = is_latched ||
                             (k.originator == s.originator &&
                              k.session_id == s.session &&
                              k.stream_id == s.stream_id);
            }
            if (comma) out += ',';
            comma = true;
            out += "{\"originator\":" + std::to_string(s.originator) +
                   ",\"session\":" + std::to_string(s.session) +
                   ",\"stream_id\":" + std::to_string(s.stream_id) +
                   ",\"stream_type\":" + std::to_string(s.stream_type) +
                   ",\"packet_count\":" + std::to_string(s.packet_count) +
                   ",\"first_seen_ms\":" + std::to_string(s.first_seen_ms) +
                   ",\"last_seen_ms\":" + std::to_string(s.last_seen_ms) +
                   ",\"latched\":" + (is_latched ? "true" : "false") + "}";
        }
        out += "]}";
        return out;
    }

    // Pass 63: the announced token cached from a craft's ANNOUNCE, for keying a
    // CSA claim (§15.5a). Returns the most-recently-seen token for `originator`.
    std::optional<std::array<uint8_t, kAnnouncePskSize>> token_for(
        uint16_t originator) const {
        std::optional<std::array<uint8_t, kAnnouncePskSize>> best;
        uint64_t best_seen = 0;
        for (const auto& [key, n] : nodes_) {
            (void)key;
            if (n.originator == originator && n.have_token &&
                n.last_seen_ms >= best_seen) {
                best = n.token;
                best_seen = n.last_seen_ms;
            }
        }
        return best;
    }

    // §15.5a claim staleness (B9): the session `originator` most recently
    // announced. A craft picks a fresh session_id each boot, so a scout
    // candidate whose session differs from the live one predates a reboot —
    // its cached channel is stale. nullopt if the craft was never seen.
    std::optional<uint32_t> session_for(uint16_t originator) const {
        std::optional<uint32_t> best;
        uint64_t best_seen = 0;
        for (const auto& [key, n] : nodes_) {
            (void)key;
            if (n.originator == originator && n.last_seen_ms >= best_seen) {
                best = n.session;
                best_seen = n.last_seen_ms;
            }
        }
        return best;
    }

  private:
    struct Node {
        uint16_t originator = 0;
        uint32_t session = 0;
        uint8_t net_id = 0;
        uint64_t last_seen_ms = 0;
        bool announced = false;  // §3.12 ANNOUNCE seen (Pass 62)
        bool claimed = false;
        uint16_t claimed_by = 0;
        bool psk_present = false;
        bool have_token = false;  // Pass 63: cached announced token (public)
        std::array<uint8_t, kAnnouncePskSize> token{};
    };
    struct Stream {
        uint16_t originator = 0;
        uint32_t session = 0;
        uint8_t stream_id = 0;
        uint8_t stream_type = 0;
        uint64_t packet_count = 0;
        uint64_t first_seen_ms = 0;
        uint64_t last_seen_ms = 0;
    };
    void age(uint64_t now) {
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            it = now > it->second.last_seen_ms + 5000 ? nodes_.erase(it)
                                                       : std::next(it);
        }
        for (auto it = streams_.begin(); it != streams_.end();) {
            it = now > it->second.last_seen_ms + 5000 ? streams_.erase(it)
                                                       : std::next(it);
        }
    }
    void trim() {
        while (nodes_.size() > 64) nodes_.erase(nodes_.begin());
        while (streams_.size() > 64) streams_.erase(streams_.begin());
    }
    std::map<uint64_t, Node> nodes_;
    std::map<uint64_t, Stream> streams_;
};

// §15.5a ground scout: one sweep engine. Retunes the scout adapter across a
// channel list, dwelling on each to aggregate the delivered-frame stream into
// per-channel candidates + an occupancy record (ACS-superset; v1 fills only the
// packet-derivable fields). During a sweep the RX filter is widened to hear all
// net_ids; on completion it returns to the resting channel/filter. Runs on the
// single-threaded event loop — no locks. The claim path (quickconnect) is layered
// on top and keys a CsaIssuer from the cached announced token.
class ScoutEngine {
  public:
    struct Hooks {
        std::function<bool(uint16_t chan_mhz, uint8_t bw)> retune;  // scout adapter
        std::function<void(uint16_t chan_mhz, uint8_t bw)> retune_all;  // all ears
        std::function<void(std::optional<uint8_t> net_id)> set_filter;
        std::function<bool(uint16_t originator)> psk_known;
    };
    ScoutEngine(Hooks h, uint8_t bw, uint16_t rest_chan,
                std::optional<uint8_t> rest_filter, size_t scout_adapter)
        : h_(std::move(h)),
          bw_(bw),
          rest_chan_(rest_chan),
          rest_filter_(rest_filter),
          scout_adapter_(scout_adapter) {}

    // §15.5a (Pass 65): update the channel a sweep returns all ears to — the
    // ground's current operating channel (config default, or a committed target).
    void set_rest_chan(uint16_t chan) { rest_chan_ = chan; }
    void set_rest_filter(std::optional<uint8_t> net_id) {
        rest_filter_ = net_id;
    }

    bool scanning() const { return phase_ == Phase::kScanning; }

    // Begin a sweep across `channels` (caller substitutes the allowlist when the
    // request omits them). Returns "" on success or a short error.
    std::string start(std::vector<uint16_t> channels, uint32_t dwell_ms,
                      uint64_t now_ms) {
        if (channels.empty()) return "no channels to scan";
        channels_ = std::move(channels);
        dwell_ms_ = dwell_ms ? dwell_ms : 300;
        results_.clear();
        chan_idx_ = 0;
        phase_ = Phase::kScanning;
        h_.set_filter(std::nullopt);  // §15.5a: hear all net_ids during a sweep
        enter_channel(now_ms);
        return "";
    }

    void stop(uint64_t now_ms) {
        if (phase_ == Phase::kIdle) return;
        finalize_current(now_ms);
        rest();
    }

    // Feed one decoded RX frame (raw wire bytes d/n for length + originator).
    void on_frame(const AirRxMeta& meta, const Decoded& dec, const uint8_t* d,
                  size_t n) {
        // §15.5a (Pass 65): survey only the scout adapter's frames — a diversity
        // ear parked on the resting channel would otherwise smear the craft across
        // every swept channel and inflate occupancy.
        if (phase_ != Phase::kScanning || meta.adapter_id != scout_adapter_ ||
            n < kHeartbeatSize) {
            return;
        }
        const uint16_t originator = be16_read(d + 3);  // §3.1 prefix offset
        ++accum_.frames;
        // §15.5a wifi_util: decodable-frame airtime estimate. Conservative MCS0
        // long-GI, raw PHY; +28 B for the 802.11 header/FCS not in the payload.
        if (const auto us = ht20_service_time_us(n + 28, 0, false, 1000)) {
            accum_.airtime_us += *us;
        }
        if (meta.rssi != 0 && meta.rssi < accum_.min_rssi) {
            accum_.min_rssi = meta.rssi;
        }
        accum_.transmitters.insert(originator);
        ++accum_.frames_by_orig[originator];  // §15.5a (Pass 66): evidence weight
        if (const Announce* an = std::get_if<Announce>(&dec)) {
            Candidate& c = accum_.candidates[originator];
            c.originator = originator;
            c.net_id = meta.net_id;
            c.session = an->prefix.session_id;
            c.claimed = (an->flags & announce_flags::kClaimed) != 0;
            c.claimed_by = an->claimed_by;
            c.psk_known = (an->flags & announce_flags::kPskPresent) != 0 &&
                          h_.psk_known(originator);
            c.chan = channels_[chan_idx_];
        }
    }

    // Advance the sweep when the current dwell elapses. §15.5a (Pass 72): a
    // dwell that heard waybeam frames but resolved no ANNOUNCE candidate yet
    // extends once (up to dwell_ms + kExtendMs total) — the announce cadence
    // (§3.12 ≥1 Hz) can exceed a short base dwell. An extension ends at the
    // first resolved candidate.
    void tick(uint64_t now_ms) {
        if (phase_ != Phase::kScanning) return;
        if (now_ms < dwell_deadline_ms_) {
            if (!extended_ || accum_.candidates.empty()) return;
        } else if (!extended_ && accum_.frames > 0 &&
                   accum_.candidates.empty()) {
            extended_ = true;
            dwell_deadline_ms_ = entered_ms_ + dwell_ms_ + kExtendMs;
            return;
        }
        finalize_current(now_ms);
        if (++chan_idx_ >= channels_.size()) {
            rest();  // sweep complete
            return;
        }
        enter_channel(now_ms);
    }

    std::string results_json() const {
        std::string out = "{\"scanning\":";
        out += scanning() ? "true" : "false";
        out += ",\"current_chan\":";
        out += (scanning() && chan_idx_ < channels_.size())
                   ? std::to_string(channels_[chan_idx_])
                   : "null";
        out += ",\"channels\":[";
        bool comma = false;
        for (const auto& r : results_) {
            if (comma) out += ',';
            comma = true;
            const Occupancy& o = r.occ;
            out += "{\"chan\":" + std::to_string(r.chan) +
                   ",\"occupancy\":{\"wifi_util_permille\":" +
                   std::to_string(o.wifi_util_permille) +
                   ",\"util_permille\":" + std::to_string(o.util_permille) +
                   ",\"interference_util_permille\":null,\"noise_dbm\":" +
                   (o.have_noise ? std::to_string(o.noise_dbm) : "null") +
                   ",\"bss_count\":" + std::to_string(o.bss_count) +
                   ",\"quality_permille\":" + std::to_string(o.quality_permille) +
                   ",\"availability_permille\":" +
                   std::to_string(o.availability_permille) + "}}";
        }
        out += "],\"candidates\":[";
        comma = false;
        for (const auto& r : results_) {
            for (const auto& c : r.candidates) {
                if (comma) out += ',';
                comma = true;
                out += "{\"originator\":" + std::to_string(c.originator) +
                       ",\"net_id\":" + std::to_string(c.net_id) +
                       ",\"session\":" + std::to_string(c.session) +
                       ",\"claimed\":" + (c.claimed ? "true" : "false") +
                       ",\"claimed_by\":" + std::to_string(c.claimed_by) +
                       ",\"chan\":" + std::to_string(c.chan) +
                       ",\"psk_known\":" + (c.psk_known ? "true" : "false") + "}";
            }
        }
        out += "]}";
        return out;
    }

    // §15.5a claim support: what a quickconnect needs about a scouted craft.
    struct Claim {
        uint16_t chan = 0;      // the channel it was heard on most (§15.5a Pass 66)
        uint8_t net_id = 0;     // §3.0 L2 tag to stamp/filter after the claim
        uint32_t session = 0;
        bool psk_known = false; // a usable CSA key is held (token or secret)
    };
    // Best candidate for `originator` across the last sweep — the channel it was
    // heard on most (§15.5a Pass 66) — or nullopt if it was never scouted (a claim
    // requires a prior scout to learn its channel).
    std::optional<Claim> candidate_for(uint16_t originator) const {
        // §15.5a (Pass 66): pick the swept channel the craft was heard on with the
        // most frames — robust to a retune-settling leak onto an adjacent channel.
        // >= keeps the last channel on a tie (matches the pre-Pass-66 behaviour).
        std::optional<Claim> found;
        uint64_t best_frames = 0;
        for (const auto& r : results_) {
            for (const auto& c : r.candidates) {
                if (c.originator == originator && c.frames >= best_frames) {
                    best_frames = c.frames;
                    found = Claim{c.chan, c.net_id, c.session, c.psk_known};
                }
            }
        }
        return found;
    }
    // Emptiest allowlisted channel by measured wifi_util (lowest wins), skipping
    // `except` (the craft's current channel). 0 if no occupancy for any allowed
    // channel (caller then falls back to an explicit target).
    uint16_t emptiest(const std::vector<uint16_t>& allowlist,
                      uint16_t except) const {
        uint16_t best = 0;
        uint32_t best_util = 1001;  // > any per-mille
        for (const uint16_t ch : allowlist) {
            if (ch == except) continue;
            for (const auto& r : results_) {
                if (r.chan != ch) continue;
                if (r.occ.wifi_util_permille < best_util) {
                    best_util = r.occ.wifi_util_permille;
                    best = ch;
                }
            }
        }
        return best;
    }

  private:
    enum class Phase { kIdle, kScanning };
    struct Occupancy {
        uint16_t wifi_util_permille = 0;
        uint16_t util_permille = 0;
        bool have_noise = false;
        int noise_dbm = 0;
        uint16_t bss_count = 0;
        uint16_t quality_permille = 0;
        uint16_t availability_permille = 0;
    };
    struct Candidate {
        uint16_t originator = 0;
        uint8_t net_id = 0;
        uint32_t session = 0;
        bool claimed = false;
        uint16_t claimed_by = 0;
        bool psk_known = false;
        uint16_t chan = 0;
        uint64_t frames = 0;  // §15.5a (Pass 66): heard-most channel wins the claim
    };
    struct ChannelResult {
        uint16_t chan = 0;
        Occupancy occ;
        std::vector<Candidate> candidates;
    };
    struct Accum {
        uint64_t frames = 0;
        uint64_t airtime_us = 0;
        int min_rssi = 0;
        std::set<uint16_t> transmitters;
        std::map<uint16_t, Candidate> candidates;
        std::map<uint16_t, uint64_t> frames_by_orig;  // §15.5a (Pass 66)
    };

    void enter_channel(uint64_t now_ms) {
        accum_ = Accum{};
        h_.retune(channels_[chan_idx_], bw_);
        entered_ms_ = now_ms;
        extended_ = false;
        dwell_deadline_ms_ = now_ms + dwell_ms_;
    }
    void finalize_current(uint64_t now_ms) {
        ChannelResult r;
        r.chan = channels_[chan_idx_];
        // §15.5a (Pass 72): the actual elapsed dwell is the airtime
        // denominator — an extended (or early-resolved) dwell must not skew
        // wifi_util_permille.
        const uint64_t elapsed_ms =
            now_ms > entered_ms_ ? now_ms - entered_ms_ : dwell_ms_;
        const uint64_t dwell_us = elapsed_ms * 1000u;
        const uint32_t util =
            dwell_us ? static_cast<uint32_t>(std::min<uint64_t>(
                           1000u, accum_.airtime_us * 1000u / dwell_us))
                     : 0u;
        r.occ.wifi_util_permille = static_cast<uint16_t>(util);
        r.occ.util_permille = static_cast<uint16_t>(util);  // v1: Wi-Fi only
        r.occ.bss_count = static_cast<uint16_t>(accum_.transmitters.size());
        r.occ.availability_permille = static_cast<uint16_t>(1000u - util);
        r.occ.quality_permille = r.occ.availability_permille;  // v1 proxy
        if (accum_.frames > 0) {
            r.occ.have_noise = true;
            r.occ.noise_dbm = accum_.min_rssi;
        }
        for (auto& [k, c] : accum_.candidates) {
            c.frames = accum_.frames_by_orig[k];  // §15.5a (Pass 66) evidence
            r.candidates.push_back(c);
        }
        results_.push_back(std::move(r));
    }
    void rest() {
        phase_ = Phase::kIdle;
        h_.set_filter(rest_filter_);     // restore the narrow net_id filter
        h_.retune_all(rest_chan_, bw_);  // §15.5a (Pass 65): return ALL ears to
                                         // rest — heals any prior claim split.
    }

    Hooks h_;
    uint8_t bw_;
    uint16_t rest_chan_;
    std::optional<uint8_t> rest_filter_;
    size_t scout_adapter_;
    Phase phase_ = Phase::kIdle;
    std::vector<uint16_t> channels_;
    uint32_t dwell_ms_ = 300;
    size_t chan_idx_ = 0;
    uint64_t dwell_deadline_ms_ = 0;
    // §15.5a (Pass 72): extension budget past the base dwell — one full
    // worst-case 1 Hz announce period (§3.12) plus margin.
    static constexpr uint64_t kExtendMs = 1200;
    uint64_t entered_ms_ = 0;
    bool extended_ = false;
    Accum accum_;
    std::vector<ChannelResult> results_;
};

// §15.2 policy.csa → the core engine's parameter block (string PSK to raw
// bytes, seconds to ms).
CsaParams csa_params(const Config& cfg) {
    const CsaPolicy& c = cfg.policy.csa;
    CsaParams p;
    p.psk.assign(c.psk.begin(), c.psk.end());
    p.settle_ms = static_cast<uint32_t>(c.settle_s * 1000.0);
    p.verify_timeout_ms = c.verify_timeout_ms;
    p.min_interval_ms = c.min_interval_s * 1000;
    p.ack_timeout_ms = c.ack_timeout_ms;
    p.bind_release_ms = c.bind_release_s * 1000;
    p.allowlist = c.channel_allowlist;
    // §11.4a (Pass 85): only a passive spectator may follow an unauthenticated
    // CSA. Craft/ground with an empty key are FAULTED, not unauthenticated,
    // and fail closed — the permission rides the role, never an empty buffer.
    p.allow_unauthenticated = cfg.node.spectator;
    return p;
}

// §15.2 policy.cmd → the §11.7 engine parameter block. The key follows the
// §11.4a CSA provenance (secret here; announced token keyed by the caller).
VcmdParams vcmd_params(const Config& cfg) {
    const CmdPolicy& c = cfg.policy.cmd;
    VcmdParams p;
    p.psk.assign(cfg.policy.csa.psk.begin(), cfg.policy.csa.psk.end());
    p.copies = static_cast<uint8_t>(c.copies);
    p.copy_interval_ms = c.copy_interval_ms;
    p.echo_copies = static_cast<uint8_t>(c.echo_copies);
    p.ack_timeout_ms = c.ack_timeout_ms;
    p.retry_cap = static_cast<uint8_t>(c.retry_cap);
    p.min_interval_ms = c.min_interval_ms;
    return p;
}

// §15.5 vehicle/command REST names ↔ §11.7 registry ids.
uint8_t vcmd_id_for(const std::string& name) {
    if (name == "arq") return vcmd_id::kArq;
    if (name == "selector") return vcmd_id::kSelector;
    if (name == "fps_ladder") return vcmd_id::kFpsLadder;
    if (name == "fps_select") return vcmd_id::kFpsSelect;
    if (name == "resolution") return vcmd_id::kResolution;
    if (name == "framing") return vcmd_id::kFraming;
    if (name == "mode") return vcmd_id::kMode;  // §11.7 Pass 105
    if (name == "calibrate") return vcmd_id::kCalibrate;  // §10.6 Pass 120
    return 0;
}

const char* vcmd_name_for(uint8_t id) {
    switch (id) {
        case vcmd_id::kArq: return "arq";
        case vcmd_id::kSelector: return "selector";
        case vcmd_id::kFpsLadder: return "fps_ladder";
        case vcmd_id::kFpsSelect: return "fps_select";
        case vcmd_id::kResolution: return "resolution";
        case vcmd_id::kFraming: return "framing";
        case vcmd_id::kMode: return "mode";  // §11.7 Pass 105
        case vcmd_id::kCalibrate: return "calibrate";  // §10.6 Pass 120
    }
    return "";
}

uint8_t bw_code(uint8_t width_mhz) {
    return width_mhz >= 80 ? 2 : width_mhz >= 40 ? 1 : 0;
}

bool channel_allowed(const std::vector<uint16_t>& allowlist, uint16_t chan) {
    return std::find(allowlist.begin(), allowlist.end(), chan) !=
           allowlist.end();
}

// §7.2: the pacer keys off END_OF_BLOCK frames in both directions.
bool frame_is_eob(const uint8_t* f, size_t n) {
    const Decoded dec = decode(f, n);
    const DataView* v = std::get_if<DataView>(&dec);
    return v != nullptr && (v->hdr.data_flags & data_flags::kEndOfBlock) != 0;
}

// §7.2 Pass 78 paced-stream semantics: only the RTP video stream's EOBs open
// craft listen windows / re-anchor ground returns. A non-video datagram is a
// one-datagram block whose EOB must not re-arm the gap (50 Hz audio EOBs
// re-arming mid-flush is the measured rung-flapping failure).
bool frame_is_paced_eob(const uint8_t* f, size_t n) {
    const Decoded dec = decode(f, n);
    const DataView* v = std::get_if<DataView>(&dec);
    return v != nullptr &&
           (v->hdr.data_flags & data_flags::kEndOfBlock) != 0 &&
           v->hdr.stream_type == stream_type::kRtp;
}

bool frame_is_live_rtp_data(const uint8_t* f, size_t n) {
    const Decoded dec = decode(f, n);
    const DataView* v = std::get_if<DataView>(&dec);
    return v != nullptr && v->hdr.stream_type == stream_type::kRtp &&
           (v->hdr.data_flags & data_flags::kRetransmit) == 0;
}

// §2: random per-boot session nonce.
uint32_t session_nonce() {
    uint32_t nonce = 0;
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        const size_t got = std::fread(&nonce, 1, sizeof(nonce), f);
        std::fclose(f);
        if (got == sizeof(nonce) && nonce != 0) {
            return nonce;
        }
    }
    return static_cast<uint32_t>(now_ms()) | 1u;  // degraded fallback
}

// §11.4a: per-boot 16-byte announced pairing token P (io/app entropy; the pure
// core layer stays RNG-free and only verifies against a supplied key). Used as
// the craft's CSA HMAC key AND advertised in ANNOUNCE (§3.12) when no operator
// csa.psk is configured (announced mode).
std::array<uint8_t, kAnnouncePskSize> announce_token() {
    std::array<uint8_t, kAnnouncePskSize> t{};
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        const size_t got = std::fread(t.data(), 1, t.size(), f);
        std::fclose(f);
        if (got == t.size()) return t;
    }
    // Degraded fallback: never key with all-zero. Mix the boot clock.
    const uint64_t ms = now_ms();
    for (size_t i = 0; i < t.size(); ++i) {
        t[i] = static_cast<uint8_t>((ms >> ((i % 8) * 8)) ^ (i * 0x9du)) | 1u;
    }
    return t;
}

SchedulerPolicy scheduler_policy(const Config& cfg) {
    SchedulerPolicy p;
    p.holddown_ms = cfg.policy.arq.holddown_ms;
    p.attempt_cap = cfg.policy.arq.attempt_cap;
    p.airtime_frac = cfg.policy.arq.airtime_frac;
    p.preferred_originator = cfg.node.preferred_originator;
    p.release_timeout_ms = cfg.policy.arq.release_timeout_ms;
    p.min_recoverable_ms = cfg.policy.arq.min_recoverable_ms;
    p.interval_ms = cfg.policy.arq.budget_interval_ms;
    p.max_block_pkts = cfg.policy.arq.max_block_pkts;
    p.budget_floor_bytes = cfg.policy.arq.budget_floor_bytes;
    return p;
}

RxPolicy rx_policy(const Config& cfg) {
    RxPolicy p;
    p.fwd_clamp_pkts = cfg.policy.rx.fwd_clamp_pkts;
    p.fwd_clamp_blocks = cfg.policy.arq.fwd_clamp_blocks;
    p.stall_timeout_ms = cfg.policy.rx.stall_timeout_ms;
    p.dwell_ceiling_ms = cfg.policy.rx.dwell_ceiling_ms;
    p.admit_n = cfg.policy.rx.admit_n;
    p.admit_window_ms = cfg.policy.rx.admit_window_ms;
    p.renack_attempts = cfg.policy.rx.renack_attempts;
    p.renack_backoff_ms = cfg.policy.rx.renack_backoff_ms;
    p.idle_teardown_ms = cfg.policy.rx.idle_teardown_ms;
    p.clamp_resync_ms = cfg.policy.rx.clamp_resync_ms;
    return p;
}

uint32_t s_to_ms(double s) {
    return s <= 0.0 ? 0u : static_cast<uint32_t>(s * 1000.0 + 0.5);
}

SelectorPolicy selector_policy(const Config& cfg) {
    const SelectPolicy& s = cfg.policy.select;
    SelectorPolicy p;
    p.demote_milli = s.demote_milli;
    p.emergency_loss_milli = s.emergency_loss_milli;
    p.loss_min_uniq = s.loss_min_uniq;
    p.loss_persist_score = s.loss_persist_score;
    p.rung_lockout_ms = s_to_ms(s.rung_lockout_s);
    p.rung_lockout_latch_count = s.rung_lockout_latch_count;
    p.rssi_floor_dbm = s.rssi_floor_dbm;
    p.rssi_fade_db_per_s = s.rssi_fade_db_per_s;
    p.rssi_fade_arm_dbm = s.rssi_fade_arm_dbm;
    p.down_cooldown_ms = s_to_ms(s.down_cooldown_s);
    p.ewma_alpha = s.ewma_alpha;
    p.rung_rssi_floor_dbm = s.rung_rssi_floor_dbm;
    p.promote_rssi_hyst_db = s.promote_rssi_hyst_db;
    p.promote_dwell_ms = s_to_ms(s.promote_dwell_s);
    p.bitrate_lead_ms = s_to_ms(s.bitrate_lead_s);
    p.mcs_up_grace_ms = s_to_ms(s.mcs_up_grace_s);
    p.mcs_settle_ms = s_to_ms(s.mcs_settle_s);
    p.reentry_backoff_ms = s_to_ms(s.reentry_backoff_s);
    p.reentry_dwell_ms = s_to_ms(s.reentry_dwell_s);
    p.flap_freeze_count = s.flap_freeze_count;
    p.flap_freeze_window_ms = s_to_ms(s.flap_freeze_window_s);
    p.flap_freeze_ms = s_to_ms(s.flap_freeze_s);
    p.min_profile = s.min_profile;
    p.max_profile = s.max_profile;
    p.max_bitrate_kbps = cfg.venc.max_bitrate_kbps;  // §9.6 Pass 75 ceiling
    p.report_timeout_ms = cfg.policy.report_timeout_ms;
    p.failsafe_hold_ms = s_to_ms(s.failsafe_hold_s);
    p.failsafe_step_ms = s_to_ms(s.failsafe_step_s);
    p.pressure_escape_ms = s_to_ms(s.pressure_escape_s);
    return p;
}

QuietGapPolicy quietgap_policy(const Config& cfg) {
    QuietGapPolicy p;
    p.enabled = cfg.policy.ret.quiet_gap;
    p.guard_us = cfg.policy.ret.guard_us;
    p.window_us = cfg.policy.ret.return_window_us;
    return p;
}

// ---- air backend selection (udp dev backend | devourer radio, §3.0) --------

struct AirBackend {
    std::optional<UdpAir> udp;
    std::optional<MonAir> mon;
#if WBLINK_RADIO
    std::optional<RadioAir> radio;
#endif
    uint64_t last_tx_ms = 0;
    uint64_t last_announce_ms = 0;  // §3.12 ANNOUNCE cadence (own timer)
    // Live per-adapter channel, indexed like cfg.adapters. §15.5 /api/v1/info
    // reported the CONFIG channel before this, so every adapter still read its
    // boot channel after a CSA or a scout sweep. Per-adapter rather than one
    // backend-wide value because retune_one() moves a single ear (the scout
    // sweeps with the others left in place), so they genuinely diverge.
    //
    // Confidence differs by backend, deliberately not papered over:
    //   kernel-monitor — confirmed: MonAir::retune reports iw_set_freq's result.
    //   radio          — commanded: RadioAir::retune returns true unconditionally
    //                    (devourer FastRetune/SetMonitorChannel are void), so
    //                    this records intent, not confirmation.
    //   udp dev        — logged intent, same as the retune itself.
    // The seed is config intent on kernel-monitor: MonAir::create never applies
    // channel_mhz, so before the first retune the card is on whatever mon-up
    // left it. RadioAir does apply it at create.
    std::vector<uint16_t> chan_by_adapter;

    static Result<AirBackend> create(const Config& cfg) {
        AirBackend b;
        b.chan_by_adapter.reserve(cfg.adapters.size());
        for (const AdapterCfg& a : cfg.adapters) {
            b.chan_by_adapter.push_back(a.channel_mhz);
        }
        if (cfg.air.kind == AirCfg::Kind::kUdp ||
            cfg.air.kind == AirCfg::Kind::kUdpBroadcast) {
            AirUdpCfg uc = cfg.air.udp;
            uc.rx_drop_permille = cfg.air.rx_drop_permille;  // bench synthetic loss
            uc.broadcast = cfg.air.kind == AirCfg::Kind::kUdpBroadcast;
            uc.originator = cfg.node.originator;
            auto a = UdpAir::create(uc);
            if (!a) {
                return Result<AirBackend>::fail(a.error);
            }
            b.udp.emplace(std::move(*a.value));
            return Result<AirBackend>::ok(std::move(b));
        }
        if (cfg.air.kind == AirCfg::Kind::kMonitor) {
            MonAirCfg mc;
            mc.adapters = cfg.adapters;
            // A dedicated cache with no media streams receives DATA over RF
            // and returns CACHE_STATUS/REQUEST/REPLY over Ethernet. It must
            // not manufacture RF traffic merely to satisfy the normal ground
            // uplink invariant. A §2/§13 passive spectator (Pass 74) is the
            // other uplink-free node: a display receiver with no return path.
            mc.allow_rx_only =
                (cfg.cache.store.enabled && cfg.streams.empty()) ||
                cfg.node.spectator;
            mc.stamp_net_id = cfg.node.net_id.value_or(0);
            mc.filter_net_id = cfg.node.net_id;
            mc.originator = cfg.node.originator;
            mc.rx_drop_permille = cfg.air.rx_drop_permille;
            mc.airtime_efficiency_permille =
                cfg.air.airtime_efficiency_permille;
            auto a = MonAir::create(mc);
            if (!a) {
                return Result<AirBackend>::fail(a.error);
            }
            b.mon.emplace(std::move(*a.value));
            return Result<AirBackend>::ok(std::move(b));
        }
        if (cfg.air.kind == AirCfg::Kind::kRadio) {
#if WBLINK_RADIO
            RadioAirCfg rc;
            rc.adapters = cfg.adapters;
            rc.stamp_net_id = cfg.node.net_id.value_or(0);
            rc.filter_net_id = cfg.node.net_id;
            rc.originator = cfg.node.originator;
            rc.rx_drop_permille = cfg.air.rx_drop_permille;
            // §3.0 Pass 12 hardware-ACK hybrid halves.
            rc.ack_responder = cfg.air.ack_responder;
            rc.unicast_returns = cfg.policy.ret.unicast;
            auto a = RadioAir::create(rc);
            if (!a) {
                return Result<AirBackend>::fail(a.error);
            }
            b.radio.emplace(std::move(*a.value));
            return Result<AirBackend>::ok(std::move(b));
#else
            return Result<AirBackend>::fail(
                "air: kind \"radio\" but this binary was built with "
                "WBLINK_RADIO=OFF");
#endif
        }
        return Result<AirBackend>::fail(
            "no air backend configured (add an \"air\" section)");
    }

    size_t inject(const uint8_t* f, size_t n) {
        size_t sent = 0;
        if (mon) {
            sent = mon->inject(f, n);
        } else {
#if WBLINK_RADIO
            if (radio) {
                sent = radio->inject(f, n);
            } else
#endif
            {
                sent = udp->inject(f, n);
            }
        }
        if (sent > 0) {
            last_tx_ms = now_ms();
        }
        return sent;
    }

    size_t inject_resend(const uint8_t* f, size_t n) {
        size_t sent = 0;
        if (mon) {
            sent = mon->inject_resend(f, n);
        } else {
#if WBLINK_RADIO
            if (radio) {
                sent = radio->inject_resend(f, n);
            } else
#endif
            {
                sent = udp->inject_resend(f, n);
            }
        }
        if (sent > 0) last_tx_ms = now_ms();
        return sent;
    }

    std::vector<int> wait_fds() const {
        if (mon) return {mon->wait_fd()};
#if WBLINK_RADIO
        if (radio) return {radio->wait_fd()};
#endif
        return udp->wait_fds();
    }

    // Returns (NACK/LINK_REPORT) carry their target so the radio backend
    // can address them as §3.0 unicast when return.unicast is on; the udp
    // dev backend has no L2 addressing and ignores the target.
    size_t inject_return(uint16_t target, const uint8_t* f, size_t n,
                         bool urgent = false) {
        size_t sent = 0;
        if (mon) {
            sent = mon->inject_return(target, f, n, urgent);
        } else {
#if WBLINK_RADIO
            if (radio) {
                sent = radio->inject_return(target, f, n, urgent);
            } else
#endif
            {
                (void)target;
                sent = udp->inject(f, n);
            }
        }
        if (sent > 0) {
            last_tx_ms = now_ms();
        }
        return sent;
    }

    void heartbeat(uint16_t originator, uint32_t session, uint64_t now) {
        if (mon && !mon->has_tx()) return;
        if (now < last_tx_ms || now - last_tx_ms < 1000) {
            return;
        }
        Heartbeat hb;
        hb.prefix.originator = originator;
        hb.prefix.session_id = session;
        uint8_t frame[kHeartbeatSize];
        if (encode_heartbeat(hb, frame, sizeof(frame)) == sizeof(frame)) {
            inject(frame, sizeof(frame));
        }
    }

    // §3.12 pairing beacon. Unlike heartbeat this is NOT suppressed by active
    // DATA — it is the craft's continuous claim/token advertisement, emitted at
    // ~2 Hz in both claimed and unclaimed states so a rebooted ground can
    // re-learn the token and re-claim in place (§11.5a). psk_present selects
    // announced (token in psk) vs secret (all-zero) mode; claimed/claimed_by
    // come from the follower's §11.5a binding.
    void announce(uint16_t originator, uint32_t session, uint64_t now,
                  const uint8_t* token, bool psk_present, bool claimed,
                  uint16_t claimed_by) {
        static constexpr uint64_t kAnnounceIntervalMs = 500;  // §3.12 1–2 Hz
        if (now < last_announce_ms ||
            now - last_announce_ms < kAnnounceIntervalMs) {
            return;
        }
        last_announce_ms = now;
        Announce a;
        a.prefix.originator = originator;
        a.prefix.session_id = session;  // destination = 0 (§3.12)
        a.flags = 0;
        if (claimed) {
            a.flags |= announce_flags::kClaimed;
            a.claimed_by = claimed_by;
        }
        if (psk_present) {
            a.flags |= announce_flags::kPskPresent;
            std::memcpy(a.psk, token, kAnnouncePskSize);
        }  // else psk stays all-zero (secret mode)
        uint8_t frame[kAnnounceSize];
        if (encode_announce(a, frame, sizeof(frame)) == sizeof(frame)) {
            inject(frame, sizeof(frame));
        }
    }

    int poll_once(int timeout_ms, const UdpAir::RxCb& cb) {
        if (mon) {
            return mon->poll_once(timeout_ms, cb);
        }
#if WBLINK_RADIO
        if (radio) {
            return radio->poll_once(timeout_ms, cb);
        }
#endif
        return udp->poll_once(timeout_ms, cb);
    }

    void set_packet_trace(PacketEventTrace* trace) {
        if (!udp || trace == nullptr || !trace->enabled()) return;
        udp->set_trace([trace](const char* direction, const char* outcome,
                              int adapter, const uint8_t* frame, size_t len) {
            trace->packet(direction, outcome, adapter, frame, len);
        });
    }

    size_t rx_adapters() const {
        if (mon) {
            return mon->rx_adapters();
        }
#if WBLINK_RADIO
        if (radio) {
            return radio->rx_adapters();
        }
#endif
        return udp->rx_adapters();
    }

    // Radio-only surfaces (no-ops / nullopt on the udp dev backend).
    void set_tx_mode(uint8_t mcs, bool sgi) {
        if (mon) {
            mon->set_tx_mode(mcs, sgi);
            return;
        }
#if WBLINK_RADIO
        if (radio) {
            radio->set_tx_mode(mcs, sgi);
        }
#else
        (void)mcs;
        (void)sgi;
#endif
    }
    // §10.5: false = the backend did NOT accept the value (callers must not
    // cache it as applied). radio/udp writes are in-process — always accepted.
    bool set_power_qdb(size_t adapter, int32_t qdb) {
        if (mon) {
            return mon->set_power_qdb(adapter, qdb);
        }
#if WBLINK_RADIO
        if (radio) {
            radio->set_power_qdb(adapter, qdb);
        }
#else
        (void)adapter;
        (void)qdb;
#endif
        return true;
    }
    // §10.5 auto restore: monitor → driver default (`txpower auto`); radio →
    // offset 0 (undoes the latch; the §10.2 curve resolve re-applies on top
    // when a curve is loaded). udp: logged intent, like set_power_qdb.
    void set_power_auto(size_t adapter) {
        if (mon) {
            mon->set_power_auto(adapter);
            return;
        }
#if WBLINK_RADIO
        if (radio) {
            radio->set_power_qdb(adapter, 0);
        }
#else
        (void)adapter;
#endif
    }
    std::optional<uint64_t> read_tsf(uint8_t adapter) {
        if (mon) {
            return mon->read_tsf(adapter);
        }
#if WBLINK_RADIO
        if (radio) {
            return radio->read_tsf(adapter);
        }
#else
        (void)adapter;
#endif
        return std::nullopt;
    }
    // §11.6 intra-process atomic switch: every local adapter retunes at
    // T_switch (a straggler follows because a sibling heard the CSA). On the
    // udp dev backend the retune is a logged intent — the CSA state machines
    // stay exercisable end-to-end without radios.
    bool retune_all(uint16_t chan_mhz, uint8_t bw, bool fast) {
        if (mon) {
            const uint8_t width = bw <= 2 ? (bw == 2 ? 80 : bw == 1 ? 40 : 20)
                                          : bw;
            bool ok = true;
            for (size_t i = 0; i < mon->rx_adapters(); ++i) {
                const bool tuned = mon->retune(i, chan_mhz, width, fast);
                ok = tuned && ok;
                if (tuned) {
                    mon->reapply_tx_power(i);
                    note_chan(i, chan_mhz);  // confirmed by iw_set_freq
                }
            }
            // Pass 69 §11.6 verify hygiene: pre-retune backlog is
            // old-channel residue — it must never satisfy a video-verify.
            mon->flush_rx();
            return ok;
        }
#if WBLINK_RADIO
        if (radio) {
            const uint8_t code = bw > 2 ? bw_code(bw) : bw;
            bool ok = true;
            for (size_t i = 0; i < radio->rx_adapters(); ++i) {
                if (!radio->retune(i, chan_mhz, code, fast)) {
                    ok = false;
                    std::fprintf(stderr, "csa: adapter %zu retune to %u MHz "
                                         "failed\n",
                                 i, chan_mhz);
                } else {
                    note_chan(i, chan_mhz);
                }
                radio->reapply_tx_power(i);  // §11.2 post-retune TXAGC
            }
            // Pass 69 §11.6 verify hygiene: pre-retune backlog is
            // old-channel residue — it must never satisfy a video-verify.
            radio->flush_rx();
            return ok;
        }
#endif
        std::fprintf(stderr, "csa: retune -> %u MHz bw=%u%s (udp backend, "
                             "intent only)\n",
                     chan_mhz, bw, fast ? " fast" : "");
        for (size_t i = 0; i < chan_by_adapter.size(); ++i) {
            note_chan(i, chan_mhz);  // dev backend: follow the logged intent
        }
        return true;
    }
    // Bounds-checked because the retune loops iterate the BACKEND's adapter
    // count. That equals cfg.adapters.size() today (both backends build 1:1
    // from cfg, all-or-fail), so the drop is unreachable — but any future
    // adapter pre-filtering would silently reintroduce the stale-channel bug
    // this exists to fix, so the check stays and this note with it.
    void note_chan(size_t adapter, uint16_t chan_mhz) {
        if (adapter < chan_by_adapter.size()) {
            chan_by_adapter[adapter] = chan_mhz;
        }
    }
    // §11.6 Pass 80: total RX frames across adapters (liveness baseline) and
    // the one-shot full monitor re-init recovery (kernel-monitor only).
    uint64_t rx_frames_total() const {
        uint64_t total = 0;
        if (mon) {
            for (size_t i = 0; i < mon->rx_adapters(); ++i) {
                total += mon->counters(i).rx_frames;
            }
        }
#if WBLINK_RADIO
        if (radio) {
            for (size_t i = 0; i < radio->rx_adapters(); ++i) {
                total += radio->counters(i).rx_frames;
            }
        }
#endif
        return total;
    }
    bool recover_all(uint16_t chan_mhz, uint8_t bw) {
        if (mon) {
            const uint8_t width = bw <= 2 ? (bw == 2 ? 80 : bw == 1 ? 40 : 20)
                                          : bw;
            bool ok = true;
            for (size_t i = 0; i < mon->rx_adapters(); ++i) {
                const bool up = mon->recover(i, chan_mhz, width);
                if (up) note_chan(i, chan_mhz);
                ok = up && ok;
            }
            mon->flush_rx();
            return ok;
        }
        return false;  // §11.6 Pass 80: devourer/udp out of scope
    }
    bool is_radio() const {
        if (mon) {
            return true;
        }
#if WBLINK_RADIO
        return radio.has_value();
#else
        return false;
#endif
    }
    bool tx_pending() const { return udp && udp->tx_pending(); }
    std::optional<uint32_t> estimate_airtime_us(size_t bytes,
                                                bool include_pending) const {
        if (mon) {
            return mon->estimate_airtime_us(bytes, include_pending);
        }
        if (!udp) return std::nullopt;
        return udp->estimate_airtime_us(bytes, include_pending);
    }
    bool supports_csa() const {
        // UDP exercises CSA state without a physical retune; kernel-monitor now
        // retunes via iw (Pass 63); the radio backend via devourer. All real.
        return true;
    }
    // §15.5a scout: widen/narrow the RX net_id filter at runtime (monitor only;
    // no-op elsewhere) and retune a single adapter (the scout adapter).
    void set_filter_net_id(std::optional<uint8_t> net_id) {
        if (mon) mon->set_filter_net_id(net_id);
    }
    void set_stamp_net_id(uint8_t net_id) {
        if (mon) mon->set_stamp_net_id(net_id);
    }
    // §15.5a scout (Pass 64): index of the uplink adapter the sweep roams. Only
    // the kernel-monitor backend has a resolvable tx adapter here; the radio/udp
    // dev backends scout index 0 (their tx is conventionally first, and udp is a
    // logged-intent no-op anyway).
    size_t tx_index() const {
        if (mon) return mon->tx_index();
        return 0;
    }
    bool retune_one(size_t adapter, uint16_t chan_mhz, uint8_t bw, bool fast) {
        if (mon) {
            const uint8_t width = bw <= 2 ? (bw == 2 ? 80 : bw == 1 ? 40 : 20)
                                          : bw;
            const bool tuned = mon->retune(adapter, chan_mhz, width, fast);
            if (tuned) note_chan(adapter, chan_mhz);
            return tuned;
        }
#if WBLINK_RADIO
        if (radio) {
            const bool tuned = radio->retune(adapter, chan_mhz,
                                             bw > 2 ? bw_code(bw) : bw, fast);
            if (tuned) note_chan(adapter, chan_mhz);
            return tuned;
        }
#endif
        note_chan(adapter, chan_mhz);
        return true;  // udp: logged intent
    }
    bool set_udp_rx_drop(int permille) {
        if (!udp || permille < 0 || permille > 1000) {
            return false;
        }
        udp->set_rx_drop_permille(static_cast<uint16_t>(permille));
        return true;
    }
    // §9.10: the watchdog runs in the mode loop (it owns the clock); its
    // verdict is grafted onto the TX adapter's stats entry here.
    void fill_adapter_stats(StatsSnapshot& snap, uint64_t tsf_fallbacks,
                            bool tx_wedged) const {
        if (udp) {
            const size_t n = udp->rx_adapters();
            for (size_t i = 0; i < n; ++i) {
                AdapterStats as;
                as.name = "udp" + std::to_string(i);
                as.rx = udp->rx_frames(i);
                as.filtered = udp->rx_filtered(i);
                as.drop = udp->rx_dropped(i);
                as.kernel_drop = udp->kernel_dropped(i);
                if (i == 0) {
                    as.tx_submitted = udp->tx_submitted();
                    as.tx_failed = udp->tx_failed();
                }
                snap.adapters.push_back(std::move(as));
            }
            // A TX-only UDP node has no RX adapter entry to carry its aggregate.
            if (n == 0 && (udp->tx_submitted() != 0 || udp->tx_failed() != 0)) {
                AdapterStats as;
                as.name = "udp-tx";
                as.tx_submitted = udp->tx_submitted();
                as.tx_failed = udp->tx_failed();
                snap.adapters.push_back(std::move(as));
            }
            (void)tsf_fallbacks;
            (void)tx_wedged;
            return;
        }
        if (mon) {
            for (size_t i = 0; i < mon->rx_adapters(); ++i) {
                const auto c = mon->counters(i);
                AdapterStats as;
                as.name = c.name;
                as.rx = c.rx_frames;
                as.rssi_best = c.rssi_last;
                as.rssi_mean = c.rssi_last;
                as.tx_submitted = c.tx_submitted;
                as.tx_failed = c.tx_failed;
                as.drop = c.rx_dropped;
                as.filtered = c.rx_filtered;
                as.kernel_drop = c.kernel_dropped;
                as.bpf_filtered = c.bpf_filtered;
                as.tsf_fallback = (i == 0) ? tsf_fallbacks : 0;
                // No CCX tx.report on monitor injection. The §9.10 verdict
                // instead uses netdev tx_packets progress internally.
                as.tx_reports = 0;
                as.tx_report_fails = 0;
                for (size_t m = 0; m < kRxMcsBuckets; ++m) {
                    as.rx_mcs[m] = c.rx_mcs[m];  // §15.3 Pass 118
                }
                as.rx_mcs_unknown = c.rx_mcs_unknown;
                as.tx_wedged = c.tx && tx_wedged;
                snap.adapters.push_back(std::move(as));
            }
            return;
        }
#if WBLINK_RADIO
        if (!radio) {
            return;
        }
        for (size_t i = 0; i < radio->rx_adapters(); ++i) {
            const auto c = radio->counters(i);
            AdapterStats as;
            as.name = c.name;
            as.rx = c.rx_frames;
            as.rssi_best = c.rssi_last;
            as.rssi_mean = c.rssi_last;
            as.tx_submitted = c.tx_submitted;
            as.tx_failed = c.tx_failed;
            as.drop = c.rx_dropped;
            as.filtered = c.rx_filtered;  // Pass 114 audit: was computed+dropped
            // Node-wide §7.2 TSF-read fallback count, surfaced once.
            as.tsf_fallback = (i == 0) ? tsf_fallbacks : 0;
            as.tx_reports = c.tx_reports;
            as.tx_report_fails = c.tx_report_fails;
            for (size_t m = 0; m < kRxMcsBuckets; ++m) {
                as.rx_mcs[m] = c.rx_mcs[m];  // §15.3 Pass 118
            }
            as.rx_mcs_unknown = c.rx_mcs_unknown;
            as.tx_wedged = c.tx && tx_wedged;
            as.rx_dead = c.rx_dead;  // §15.3 Pass 101 (RadioAir only)
            snap.adapters.push_back(std::move(as));
        }
#else
        (void)snap;
        (void)tsf_fallbacks;
        (void)tx_wedged;
#endif
    }

    // §15.3 return-block unicast counters (radio backend; no-op on udp).
    void fill_return_stats(ReturnStats& ret) const {
        if (mon) {
            mon->return_counters(ret.unicast_sent, ret.unicast_fallback);
            return;
        }
#if WBLINK_RADIO
        if (radio) {
            radio->return_counters(ret.unicast_sent, ret.unicast_fallback);
        }
#else
        (void)ret;
#endif
    }

    // TX adapter's cumulative (submitted, completion-progress) counters for
    // §9.10: netdev tx_packets on monitor, CCX reports on devourer. nullopt on
    // UDP, which has no independent completion surface.
    std::optional<std::pair<uint64_t, uint64_t>> tx_progress_counters() const {
        if (mon) {
            uint64_t s = 0;
            uint64_t p = 0;
            mon->tx_progress_counters(s, p);
            return std::make_pair(s, p);
        }
#if WBLINK_RADIO
        if (radio) {
            uint64_t s = 0;
            uint64_t r = 0;
            radio->tx_report_counters(s, r);
            return std::make_pair(s, r);
        }
#endif
        return std::nullopt;
    }
};

// ---- TX side: per in-stream framer + ring + scheduler ----------------------

struct TxCore {
    using Inject = std::function<void(const uint8_t*, size_t)>;

    // A stream carries EITHER a UDP-datagram Framer (§5.1) or a whole-frame
    // FrameFramer (§5.1a, frame-shm ingress) — never both. Exactly one of the
    // two optionals is engaged per the ingress binding kind.
    struct Stream {
        uint8_t stream_id;
        uint8_t stream_type;
        std::optional<Framer> framer;             // udp ingress
        std::optional<FrameFramer> frame_framer;  // frame-shm ingress
        std::optional<JsccRuntimeShadow> jscc_shadow;
        ResendRing ring;
        ResendScheduler sched;
        JsccShadowResult jscc_latest;
        uint64_t jscc_decision_frames = 0;
        uint64_t jscc_valid_decisions = 0;
        uint64_t jscc_fallback_decisions = 0;
        // §14.2 enforcement (Pass 38).
        bool jscc_enforce = false;
        uint64_t jscc_enforced_frames = 0;
        uint64_t jscc_discarded_frames = 0;
    };

    TxCore(const Config& cfg, uint32_t session, const ProfileTable* table,
           uint8_t table_version)
        : originator_(cfg.node.originator),
          session_(session),
          table_version_(table_version),
          table_(table),
          selector_(selector_policy(cfg), table),
          venc_(cfg.venc),
          venc_knobs_(cfg.venc),
          arq_max_fps_(cfg.policy.arq.arq_max_fps),
          boot_min_profile_(cfg.policy.select.min_profile),
          boot_max_profile_(cfg.policy.select.max_profile),
          feedback_gate_(ReportGatePolicy{
              cfg.node.preferred_originator,
              cfg.policy.report_timeout_ms * 4}),
          report_gate_(ReportGatePolicy{
              cfg.node.preferred_originator,
              cfg.policy.report_timeout_ms * 4}) {
        // §9.11 FPS ladder (Pass 39; instantiate-vs-run split Pass 99). The
        // object is instantiated on every venc craft — its existence commands
        // nothing. `fps_ladder.enabled` sets only the BOOT run-state
        // (cmd_fps_enabled_), so FPS_LADDER on/off (§11.7 0x03 / the craft-local
        // POST /api/v1/link/fps) toggles the loop both ways at runtime with no
        // link restart — which is what makes the variable-fps mode switchable.
        if (cfg.venc.enabled) {
            const FpsLadderCfg& lc = cfg.venc.fps_ladder;
            FpsLadderPolicy fp;
            fp.min_fps = lc.min;
            fp.preferred_fps = lc.preferred;
            fp.min_p_frame_bytes = lc.min_p_frame_bytes;
            fp.restore_hysteresis_bytes = lc.restore_hysteresis_bytes;
            fp.sample_timeout_ms = lc.sample_timeout_ms;
            fp.reduce_after_ms = lc.reduce_after_ms;
            fp.reduce_dwell_ms = lc.reduce_dwell_ms;
            fp.restore_after_ms = lc.restore_after_ms;
            fp.settle_ms = lc.settle_ms;
            fps_ladder_.emplace(fp);
            cmd_fps_enabled_ = lc.enabled;  // static mode boots the loop off
        }
        // §10: one power curve per TX adapter with an authored map. The
        // resolve happens at profile commit and actuates through the
        // apply_power hook on both RF backends (§10.5); on the udp dev
        // backend it stays a logged intent.
        for (size_t i = 0; i < cfg.adapters.size(); ++i) {
            const AdapterCfg& a = cfg.adapters[i];
            if (a.role != Role::kTx) {
                continue;
            }
            // §10.5 override targets: EVERY tx adapter, curve or not.
            power_targets_.push_back(
                PowerTarget{a.name, i, a.max_power_qdb});
            if (a.power_map.empty()) {
                continue;
            }
            auto curve =
                load_power_curve(a.power_map, a.channel_mhz >= 4000);
            if (!curve) {
                std::fprintf(stderr, "power: %s: %s\n", a.name.c_str(),
                             curve.error.c_str());
                continue;
            }
            power_.push_back(PowerAdapter{a.name, i, *curve.value,
                                          a.max_power_qdb, std::nullopt});
        }
        for (const StreamCfg& s : cfg.streams) {
            if (s.dir != Dir::kIn) {
                continue;
            }
            RingConfig rc;
            rc.window_ms = cfg.policy.arq.ring_window_ms;
            rc.byte_budget = cfg.policy.arq.ring_byte_budget;
            Stream st{s.stream_id, s.stream_type, std::nullopt, std::nullopt,
                      std::nullopt, ResendRing(rc),
                      ResendScheduler(scheduler_policy(cfg), table), {}, 0, 0, 0};
            if (s.bind.kind == BindKind::kFrameShm) {
                // §5.1a: whole-frame ingress from a venc SHM ring. FEC policy
                // comes from the stream's fec block; MTU from the floor rung.
                FrameFramerConfig fc;
                fc.originator = cfg.node.originator;
                fc.session_id = session;
                fc.stream_id = s.stream_id;
                fc.stream_type = s.stream_type;
                fc.arq_mode = s.arq_mode;
                fc.fec.scheme = s.fec.scheme;
                fc.fec.i_rate_permille = s.fec.i_rate_permille;
                fc.fec.p_rate_permille = s.fec.p_rate_permille;
                fc.fec.min_k = s.fec.min_k;
                fc.fec.min_r = s.fec.min_r;
                st.frame_framer.emplace(fc);
                st.frame_framer->set_operating_point(0, table_version,
                                                     max_payload_for(0));
                if (s.jscc_shadow) {
                    const JsccShadowCfg& jc = *s.jscc_shadow;
                    st.jscc_shadow.emplace(JsccRuntimeShadowConfig{
                        jc.fec_floor_permille, jc.fec_cap_permille,
                        jc.arq_guard_us, jc.feedback_timeout_ms,
                        jc.min_rtt_samples});
                    st.jscc_enforce = jc.enforce;  // §14.2 Pass 38
                }
            } else {
                FramerConfig fc;
                fc.originator = cfg.node.originator;
                fc.session_id = session;
                fc.stream_id = s.stream_id;
                fc.stream_type = s.stream_type;
                fc.classifier = s.classifier;
                fc.classifier_size_threshold =
                    cfg.policy.arq.classifier_size_threshold;
                st.framer.emplace(fc);
                st.framer->set_operating_point(0, table_version);
            }
            streams_.push_back(std::move(st));
        }
        for (const AdapterCfg& a : cfg.adapters) {
            if (a.role == Role::kTx) {
                (void)selector_.on_rf_environment(a.channel_mhz,
                                                  bw_code(a.bw), 0);
                break;
            }
        }
    }

    // §5.1a MTU budget: the active rung's max_payload (profile 0 = floor at
    // startup), or the standard-rung default when no table is loaded.
    uint16_t max_payload_for(uint8_t profile_id) const {
        if (table_ != nullptr) {
            for (const Profile& p : table_->profiles) {
                if (p.id == profile_id) {
                    return p.max_payload;
                }
            }
        }
        return kDefaultMaxPayload;
    }

    uint32_t frame_deadline_us(bool is_idr) const {
        if (table_ == nullptr) return 0;
        const uint8_t active = selector_.profile_id();
        for (const Profile& p : table_->profiles) {
            if (p.id != active) continue;
            const uint16_t ms = is_idr ? p.arq_deadline_iframe_ms
                                       : p.arq_deadline_pframe_ms;
            return static_cast<uint32_t>(ms) * 1000u;
        }
        return 0;
    }

    void on_ingress(uint8_t stream_id, const uint8_t* d, size_t n,
                    uint64_t now, const Inject& inject) {
        for (Stream& s : streams_) {
            if (s.stream_id != stream_id || !s.framer) {
                continue;
            }
            s.framer->on_datagram(
                d, n, now,
                [&](const uint8_t* frame, size_t len, const DataHeader& hdr,
                    uint64_t t) {
                    inject(frame, len);
                    s.ring.push(frame, len, hdr, t);
                    s.sched.note_live_bytes(len);
                });
            return;
        }
    }

    // §5.1a frame-shm ingress: one whole [VencFrameMeta][Annex-B] blob is
    // fragmented + FEC'd by the stream's FrameFramer. Same emit contract as
    // on_ingress (inject + ring + scheduler bookkeeping).
    void on_frame(uint8_t stream_id, const uint8_t* blob, size_t len,
                  uint64_t now, const Inject& inject) {
        for (Stream& s : streams_) {
            if (s.stream_id != stream_id || !s.frame_framer) {
                continue;
            }
            // §9.6 cadence: windowed frame count (the ring's last-gap
            // interval collapses under batch drains and cannot be used).
            if (cadence_start_ms_ == 0) {
                cadence_start_ms_ = now;
            }
            ++cadence_frames_;
            VencFrameMeta meta;
            const bool have_meta = read_frame_meta(blob, len, &meta);
            const bool idr = have_meta && (meta.flags & kFrameFlagIdr) != 0;
            if (fps_ladder_ && have_meta && !idr) {
                fps_ladder_->note_p_frame(
                    static_cast<uint32_t>(std::min<size_t>(
                        len - kVencFrameMetaSize, UINT32_MAX)),
                    now);
            }
            if (s.jscc_shadow && have_meta) {
                const uint16_t symbol = s.frame_framer->symbol_size();
                const size_t k_sz = std::max<size_t>(1, (len + symbol - 1) / symbol);
                const uint16_t k = static_cast<uint16_t>(
                    std::min<size_t>(k_sz, UINT16_MAX));
                const size_t source_bytes =
                    len + static_cast<size_t>(k) *
                              (kDataHeaderSize + kFecSourceSubheaderSize);
                const size_t resend_bytes =
                    kDataHeaderSize + kFecSourceSubheaderSize + symbol;
                JsccShadowFrameInput input;
                input.source_k = k;
                input.deadline_us = frame_deadline_us(idr);
                input.arq_capable =
                    (idr ||
                     s.frame_framer->arq_mode() == FrameArqMode::kAllFrames) &&
                    !arq_fps_suppressed_;  // §4.1 Pass 40 cutoff
                input.now_ms = now;
                if (estimate_airtime) {
                    input.source_tx_remaining_us =
                        estimate_airtime(source_bytes, true);
                    input.resend_airtime_us =
                        estimate_airtime(resend_bytes, false);
                }
                s.jscc_latest = s.jscc_shadow->evaluate(input);
                ++s.jscc_decision_frames;
                if (s.jscc_latest.valid) {
                    ++s.jscc_valid_decisions;
                } else {
                    ++s.jscc_fallback_decisions;
                }
                // §14.2 enforcement (Pass 38): a VALID decision actuates for
                // this one frame; any fallback keeps the fixed §14.1 path.
                if (s.jscc_enforce && s.jscc_latest.valid) {
                    if (s.jscc_latest.decision.discard) {
                        ++s.jscc_discarded_frames;  // rule 2: drop, not queue
                        return;
                    }
                    s.frame_framer->set_next_frame_override(
                        s.jscc_latest.decision.parity_symbols,
                        s.jscc_latest.decision.arq_eligible);
                    ++s.jscc_enforced_frames;
                }
            }
            s.frame_framer->on_frame(
                blob, len, now,
                [&](const uint8_t* frame, size_t flen, const DataHeader& hdr,
                    uint64_t t) {
                    inject(frame, flen);
                    s.ring.push(frame, flen, hdr, t);
                    s.sched.note_live_bytes(flen);
                });
            return;
        }
    }

    // Air packets heard back (uplink): NACKs feed the scheduler, LINK_REPORTs
    // feed the §9 selector.
    bool on_air(const uint8_t* d, size_t n, uint64_t now) {
        const Decoded dec = decode(d, n);
        if (const JsccFeedback* f = std::get_if<JsccFeedback>(&dec)) {
            if (f->target_originator != originator_ ||
                f->target_session != session_ ||
                !feedback_gate_.accept(f->prefix.originator,
                                       f->prefix.session_id, now)) {
                return false;
            }
            for (Stream& s : streams_) {
                if (s.stream_id == f->target_stream_id && s.jscc_shadow) {
                    s.jscc_shadow->observe_feedback(*f, now);
                    return false;
                }
            }
            return false;
        }
        if (const NackView* nack = std::get_if<NackView>(&dec)) {
            if (nack->hdr.target_originator != originator_ ||
                nack->hdr.target_session != session_) {
                return false;
            }
            if (!cmd_arq_enabled_) {
                return false;  // §11.7 ARQ off: serve no NACKs
            }
            for (Stream& s : streams_) {
                if (s.stream_id == nack->hdr.target_stream_id) {
                    s.sched.on_nack(*nack, s.ring, now);
                    return true;
                }
            }
            return false;
        }
        if (const LinkReport* r = std::get_if<LinkReport>(&dec)) {
            if (r->target_originator != originator_ ||
                r->target_session != session_) {
                return false;
            }
            // §7.3 Pass 79: selection feedback keys on RTP streams only.
            // Defensive against a pre-79 ground still reporting non-video
            // streams (mixed-version fleet).
            bool video_stream = false;
            for (const Stream& s : streams_) {
                if (s.stream_id == r->target_stream_id &&
                    s.stream_type == stream_type::kRtp) {
                    video_stream = true;
                    break;
                }
            }
            if (!video_stream) {
                return false;
            }
            // §3.5 acceptance filter (Pass 41): preferred/latched reporters
            // only — BEFORE the selector and the §9.11 ladder consume it.
            if (!report_gate_.accept(r->prefix.originator,
                                     r->prefix.session_id, now)) {
                return false;
            }
            // Pass 78: count selector-fresh epochs only — redundant copies
            // (return.report_redundancy) and replays must not inflate the
            // §15.3 heard-ratio.
            if (selector_.on_report(*r, now)) {
                ++reports_received_;
            }
            if (calibrator_) {
                // §10.6: every ACCEPTED report feeds the calibration dwell
                // (the engine discards samples inside its settle window).
                calibrator_->on_report(static_cast<int8_t>(r->rssi_mean),
                                       r->loss_postdiv_prearq, r->uniq, now);
            }
            return false;
        }
        if (const RecoveryRequest* r = std::get_if<RecoveryRequest>(&dec)) {
            if (r->target_originator != originator_ ||
                r->target_session != session_) {
                return false;
            }
            for (const Stream& s : streams_) {
                if (s.stream_id == r->target_stream_id &&
                    s.stream_type == stream_type::kRtp) {
                    const bool queued = venc_.request_idr(now);
                    std::fprintf(stderr,
                                 "venc: decoder recovery stream=%u requester=%u %s\n",
                                 r->target_stream_id, r->prefix.originator,
                                 queued ? "requested" : "suppressed");
                    return false;
                }
            }
        }
        return false;
    }

    void drain_resends(uint64_t now, const Inject& inject_resend) {
        for (Stream& s : streams_) {
            s.ring.evict(now);
            s.sched.drain(s.ring, now, [&](const uint8_t* f, size_t l) {
                inject_resend(f, l);
            });
        }
    }

    void tick(uint64_t now, const Inject& inject,
              const Inject& inject_resend = {}) {
        const SelectorActions act = selector_.tick(now);
        if (act.commit) {
            // §9.5 commit: the operating point stamped on every DATA packet
            // (drives RX deadlines + supersession budgets)...
            const uint16_t mp = max_payload_for(act.commit->profile_id);
            for (Stream& s : streams_) {
                if (s.framer) {
                    s.framer->set_operating_point(act.commit->profile_id,
                                                  table_version_);
                }
                if (s.frame_framer) {
                    s.frame_framer->set_operating_point(act.commit->profile_id,
                                                        table_version_, mp);
                }
            }
            // ...the TX adapter's modulation default (§10.4, radio backend)...
            if (apply_mode) {
                apply_mode(act.commit->mcs,
                           act.commit->gi == GuardInterval::kShort);
            }
            // ...and the §10 per-adapter power resolve, applied inside the
            // same sequenced transition through the apply_power hook (both RF
            // backends, §10.5; a logged intent on the udp dev backend). While
            // the §10.5 override-latch is set, the resolve YIELDS — the
            // latched value already sits on the hardware.
            last_commit_mcs_ = act.commit->mcs;
            last_commit_level_ = act.commit->tx_power_level;
            if (!power_override_ && !calibrating()) {
                resolve_and_apply_power(act.commit->mcs,
                                        act.commit->tx_power_level);
            }
        }
        // B1: advance the non-blocking venc HTTP state machine once per loop
        // iteration (never blocks). The setters below only record the desired
        // value; this drives the actual connect/send/recv.
        venc_.poll(now);
        // §10.6: drive the calibration engine at loop cadence.
        calibrate_service(now);
        // Push the CURRENT target every tick: write-on-change (§9.6) makes
        // this a no-op normally, and a failed push (encoder briefly down)
        // retries next tick instead of waiting for the next rung change.
        if (selector_.bitrate_kbps() > 0) {
            venc_.set_bitrate(selector_.bitrate_kbps());
        }
        // §9.6 cadence estimate: frames over a ~1 s window.
        if (cadence_start_ms_ != 0 && now >= cadence_start_ms_ + 1000) {
            frame_cadence_us_ = cadence_frames_ > 0
                ? (now - cadence_start_ms_) * 1000ull / cadence_frames_
                : 0;
            cadence_frames_ = 0;
            cadence_start_ms_ = now;
        }
        // §9.11 FPS ladder: preserve measured P-frame FEC block size. Bitrate
        // and cap transitions get first claim on the resulting frame evidence.
        // §11.7 FPS_LADDER off: the loop stops issuing commands and the fps
        // holds where it is (current_fps stays the cadence input below).
        if (fps_ladder_ && cmd_fps_enabled_) {
            fps_ladder_->tick(now, venc_.settling(now));
            // Re-offer the current target every tick. VencActuator dedupes a
            // successfully applied value and retries after transient HTTP or
            // shared-holdoff failures, so controller state cannot outrun venc.
            venc_.set_fps(fps_ladder_->current_fps());
        } else if (cmd_fps_select_hz_ != 0) {
            // §11.7 FPS_SELECT (Pass 71): same re-offer discipline while no
            // ladder runs, so a transient HTTP failure cannot lose a one-shot
            // command. Cleared when FPS_LADDER on takes back ownership.
            venc_.set_fps(cmd_fps_select_hz_);
        }
        // §4.1 Pass 40 high-cadence ARQ cutoff, driven by the same cadence
        // input the §9.6 caps use (ladder-commanded, else measured, else
        // hint). Sticky on the framers until the cadence drops back.
        {
            const uint32_t snapped = snap_frame_period_us(cadence_period_us());
            arq_fps_suppressed_ = arq_max_fps_ != 0 && snapped != 0 &&
                                  snapped < 1000000u / arq_max_fps_;
            for (Stream& s : streams_) {
                if (s.frame_framer) {
                    s.frame_framer->set_arq_suppressed(arq_fps_suppressed_);
                }
            }
        }
        // §9.6 Pass 37 horizon caps: recomputed from slow inputs (rung
        // budget, ladder-snapped cadence, I deadline, live §14.1 rates);
        // write-on-change makes the steady state a no-op.
        if (venc_.frame_caps_enabled() && selector_.bitrate_kbps() > 0) {
            for (Stream& s : streams_) {
                if (!s.frame_framer) {
                    continue;  // caps apply to frame-shm ingress only (§9.6)
                }
                FrameCapInputs in;
                in.budget_kbps = selector_.bitrate_kbps();
                // §9.11: while the ladder runs, the COMMANDED fps is the
                // authoritative cadence — measurement lags a change by ~1 s.
                in.frame_period_us = snap_frame_period_us(cadence_period_us());
                in.iframe_deadline_ms = static_cast<uint16_t>(
                    frame_deadline_us(true) / 1000u);
                const FrameFecConfig& fec = s.frame_framer->fec();
                if (fec.scheme != FecScheme::kNone) {
                    in.i_rate_permille = fec.i_rate_permille;
                    in.p_rate_permille = fec.p_rate_permille;
                }
                in.symbol_size = static_cast<uint16_t>(
                    max_payload_for(selector_.profile_id()) -
                    kDataHeaderSize - kFecRepairSubheaderSize);
                in.ceiling_bytes = venc_knobs_.cap_ceiling_bytes;
                in.i_headroom_permille = venc_knobs_.i_headroom_permille;
                in.p_headroom_permille = venc_knobs_.p_headroom_permille;
                const FrameCaps caps = derive_frame_caps(in);
                venc_.set_max_frame_size(caps.max_i_bytes, caps.max_p_bytes);
                break;  // single video stream (§9.6)
            }
        }
        drain_resends(now, inject_resend ? inject_resend : inject);
    }

    void set_pressure(bool on, uint64_t now) {  // §9.9 gauge (step 9+ feeds it)
        selector_.set_pressure(on, now);
    }


    // §11.3: freeze the cascade + pause the watchdog across the CSA blackout.
    void csa_freeze(uint64_t until_ms) { selector_.csa_freeze(until_ms); }

    // §3.5 Pass 115: report authority is ONE authority across both return
    // gates. These wrap the pair deliberately — moving report_gate_ alone
    // would leave §3.10 JSCC feedback flowing from the displaced ground,
    // which presents as a working fix. Keep the two calls together.
    void report_authority_set(uint16_t originator, uint64_t now_ms) {
        report_gate_.force_latch(originator, now_ms);
        feedback_gate_.force_latch(originator, now_ms);
        // §12 Pass 116: repairs follow the claim too, per-stream — the
        // scheduler owns one lock each. Soft: an actively-NACKing node
        // reclaims via contested release, unlike the report latch.
        for (Stream& s : streams_) {
            s.sched.force_lock(originator);
        }
    }
    void report_authority_clear() {
        report_gate_.clear_latch();
        feedback_gate_.clear_latch();
        for (Stream& s : streams_) {
            s.sched.release_lock();
        }
    }
    bool report_authority_overridable() const {
        return report_gate_.overridable();
    }
    void on_rf_environment(uint16_t channel_mhz, uint8_t bw,
                           uint64_t now_ms) {
        (void)selector_.on_rf_environment(channel_mhz, bw, now_ms);
    }

    SelectorState selector_state(uint64_t now_ms) const {
        const SelectorLockout lock = selector_.lockout(now_ms);
        SelectorState s;
        s.prefix = {originator_, 0, session_};
        s.table_version = table_version_;
        s.active_profile = selector_.profile_id();
        s.safe_floor_profile = selector_.safe_floor_profile();
        s.ceiling_profile = lock.ceiling_profile;
        s.lockout_profile = lock.profile;
        if (lock.active) {
            s.state_flags |= selector_state_flags::kActive;
        }
        if (lock.latched) {
            s.state_flags |= selector_state_flags::kLatched;
        }
        if (lock.conflict) {
            s.state_flags |= selector_state_flags::kConflict;
        }
        s.lockout_strikes = lock.active ? lock.strikes : 0;
        s.remaining_ms = static_cast<uint16_t>(
            std::min<uint32_t>(lock.remaining_ms, 0xFFFFu));
        s.transition_reason = static_cast<uint8_t>(selector_.reason());
        s.loss_window_milli = selector_.loss_window_milli();
        s.lockout_active_mask = lock.active_mask;
        s.lockout_latched_mask = lock.latched_mask;
        s.loss_ewma_milli = selector_.loss_ewma_milli();
        s.loss_uniq = selector_.loss_uniq();
        s.loss_score = selector_.loss_score();
        // §3.15a Pass 117: name the §3.5 latch holder on air. This build
        // always carries it, so bit3 is always set and the packet is 34 bytes.
        s.state_flags |= selector_state_flags::kHolderPresent;
        s.report_latch_holder = report_gate_.latched_originator();
        if (calibrator_) {  // §10.6 Pass 120: bit4 word (implies bit3)
            s.state_flags |= selector_state_flags::kCalibPresent;
            s.calib_word = calibrator_->word();
            s.calib_fingerprint = calib_fingerprint_;
        }
        return s;
    }
    // §11.6: CSA_ARMED on every outgoing DATA frame while the campaign holds.
    void set_csa_armed(bool on) {
        const uint8_t f = on ? data_flags::kCsaArmed : 0;
        for (Stream& s : streams_) {
            if (s.framer) {
                s.framer->set_extra_flags(f);
            }
            if (s.frame_framer) {
                s.frame_framer->set_extra_flags(f);
            }
        }
    }
    uint8_t power_level() const { return selector_.tx_power_level(); }

    // §15.5 control-plane knobs -------------------------------------------
    // §9.7 profile pin: clamp the operating-point ladder to [min,max] by id.
    void set_profile_pin(uint8_t min_profile, uint8_t max_profile) {
        selector_.set_profile_pin(min_profile, max_profile);
    }
    // §15.5 Pass 103: forget the venc actuator's write-on-change cache so the
    // next tick re-asserts bitrate/caps/fps — called after an out-of-loop venc
    // restart (the §16 mode applier) which the actuator cannot otherwise see.
    void reassert_venc() { venc_.invalidate(); }
    // §14.1 live FEC-rate retune for a frame-shm stream. Returns false if the
    // stream_id is unknown or is not a frame-shm (FrameFramer) stream.
    bool set_stream_fec(uint8_t stream_id, uint16_t i_permille,
                        uint16_t p_permille, uint16_t min_k, uint16_t min_r) {
        for (Stream& s : streams_) {
            if (s.stream_id != stream_id) {
                continue;
            }
            if (!s.frame_framer) {
                return false;  // udp stream: no per-stream FEC (§15.2)
            }
            s.frame_framer->set_fec_rates(i_permille, p_permille, min_k, min_r);
            return true;
        }
        return false;
    }
    // §9.6/§9.11 cadence input: ladder-commanded, else §11.7 FPS_SELECT
    // (Pass 71 — authoritative immediately, same rule as a ladder command),
    // else measured, else hint.
    uint64_t cadence_period_us() const {
        if (fps_ladder_ && fps_ladder_->current_fps() > 0) {
            return 1000000ull / fps_ladder_->current_fps();
        }
        if (cmd_fps_select_hz_ != 0) {
            return 1000000ull / cmd_fps_select_hz_;
        }
        return frame_cadence_us_ != 0 ? frame_cadence_us_
                                      : 1000000ull / venc_knobs_.fps_hint;
    }
    // §11.7 remote command actuation (VcmdCraft::Apply). false = REJECTED —
    // unknown cmd_id, out-of-range arg, or unconfigured actuator.
    bool apply_command(uint8_t cmd_id, uint8_t arg, uint64_t now) {
        switch (cmd_id) {
            case vcmd_id::kArq:
                if (arg > 1) return false;
                cmd_arq_enabled_ = arg != 0;
                for (Stream& s : streams_) {
                    if (s.framer) {
                        s.framer->set_arq_enabled(cmd_arq_enabled_);
                    }
                    if (s.frame_framer) {
                        s.frame_framer->set_arq_enabled(cmd_arq_enabled_);
                    }
                }
                return true;  // all-off boot config ⇒ acked no-op (§11.7)
            case vcmd_id::kSelector:
                if (arg > 1) return false;
                cmd_selector_frozen_ = arg != 0;
                // §11.7: the pin lands at the next evaluate() — during a
                // §11.3 freeze the rung cannot move, so sampling now equals
                // sampling at freeze-lift.
                if (cmd_selector_frozen_) {
                    const uint8_t p = selector_.profile_id();
                    selector_.set_profile_pin(p, p);
                } else {
                    selector_.set_profile_pin(boot_min_profile_,
                                              boot_max_profile_);
                }
                return true;
            case vcmd_id::kFpsLadder:
                if (arg > 1) return false;
                if (!fps_ladder_) {
                    return false;  // §11.7: the ladder did not run at boot
                }
                if (arg != 0 && !cmd_fps_enabled_) {
                    fps_ladder_->resume(now);  // §9.11 settle semantics
                    // §11.7 (Pass 71): the ladder takes back video0.fps; a
                    // later ladder-off holds fps where the ladder left it.
                    cmd_fps_select_hz_ = 0;
                }
                cmd_fps_enabled_ = arg != 0;
                return true;
            case vcmd_id::kFpsSelect: {
                // §11.7 v2 (Pass 71): preset-indexed; REJECTED while the
                // §9.11 ladder owns video0.fps (issue FPS_LADDER off first).
                if (!venc_.enabled()) return false;
                if (arg >= venc_knobs_.preset_fps.size()) return false;
                if (cmd_fps_ladder()) return false;
                const uint16_t fps = venc_knobs_.preset_fps[arg];
                venc_.set_fps(fps);
                if (fps_ladder_) {
                    // A disabled ladder resumes from the selected rung.
                    fps_ladder_->note_external_fps(fps);
                }
                cmd_fps_select_hz_ = fps;
                cmd_fps_select_ = static_cast<uint8_t>(arg + 1);
                return true;
            }
            case vcmd_id::kResolution:
            case vcmd_id::kFraming:
                // §11.7 v2 staged (Pass 71): specified, but the venc-side
                // knobs do not exist yet — unconfigured-actuator REJECTED.
                return false;
            case vcmd_id::kCalibrate:
                // §10.6 (Pass 120): start needs an actuator and a latched
                // reporter — the loop is blind without §3.5 reports.
                if (arg > 1) return false;
                if (!calibrator_) return false;
                if (arg == 1) {
                    if (!apply_power) return false;  // udp: logged intent only
                    if (report_gate_.latched_originator() == 0) return false;
                    return calibrator_->start(now);
                }
                return calibrator_->abort(now);
            default:
                return false;
        }
    }
    void init_calibration(const CalibrationPolicy& c) {
        CalibrateParams p;
        p.target_rssi_dbm = c.target_rssi_dbm;
        p.rssi_tol_db = c.rssi_tol_db;
        p.loss_ok_milli = static_cast<uint16_t>(c.loss_ok_milli);
        p.loss_bad_milli = static_cast<uint16_t>(c.loss_bad_milli);
        p.ceil_step_qdb = c.ceil_step_qdb;
        p.min_qdb = c.min_qdb;
        p.max_qdb = c.max_qdb;
        p.settle_ms = static_cast<uint32_t>(c.settle_ms);
        p.probe_dwell_ms = static_cast<uint32_t>(c.probe_dwell_ms);
        p.verify_dwell_ms = static_cast<uint32_t>(c.verify_dwell_ms);
        p.report_loss_abort_ms =
            static_cast<uint32_t>(c.report_loss_abort_ms);
        p.hard_cap_ms = static_cast<uint32_t>(c.hard_cap_ms);
        calibrator_.emplace(p);
    }
    bool cmd_arq_enabled() const { return cmd_arq_enabled_; }
    bool cmd_selector_frozen() const { return cmd_selector_frozen_; }
    bool cmd_fps_ladder() const {
        return fps_ladder_.has_value() && cmd_fps_enabled_;
    }

    void reset_stats() {
        for (Stream& s : streams_) {
            if (s.framer) {
                s.framer->reset_stats();
            }
            if (s.frame_framer) {
                s.frame_framer->reset_stats();
            }
            if (s.jscc_shadow) {
                s.jscc_shadow->reset();
                s.jscc_latest = {};
                s.jscc_decision_frames = 0;
                s.jscc_valid_decisions = 0;
                s.jscc_fallback_decisions = 0;
            }
            s.sched.reset_counters();
        }
        reports_received_ = 0;
    }

    void fill_stats(StatsSnapshot& snap, uint64_t now) const {
        for (const Stream& s : streams_) {
            StreamStats st;
            st.stream_id = s.stream_id;
            st.type = "TX";
            if (s.framer) {
                st.seq = s.framer->next_seq();
                st.delivered = s.framer->stats().frames;
                st.decode_errors = s.framer->stats().oversize_ingress;
            } else if (s.frame_framer) {
                st.seq = s.frame_framer->next_seq();
                st.delivered = s.frame_framer->stats().frames;
                st.malformed = s.frame_framer->stats().malformed_frame;
                st.source_symbols_sent =
                    s.frame_framer->stats().source_symbols;
                st.repair_symbols_sent =
                    s.frame_framer->stats().repair_symbols;
                st.fec_oversize_frames =
                    s.frame_framer->stats().fec_oversize_k;
                st.idr_frames = s.frame_framer->stats().idr_frames;
                st.arq_frames = s.frame_framer->stats().arq_frames;
                st.arq_cutoff_frames =
                    s.frame_framer->stats().arq_cutoff_frames;
            }
            if (s.jscc_shadow) {
                const JsccShadowResult& js = s.jscc_latest;
                st.jscc_decision_frames = s.jscc_decision_frames;
                st.jscc_valid_decisions = s.jscc_valid_decisions;
                st.jscc_fallback_decisions = s.jscc_fallback_decisions;
                st.jscc_decision_valid = js.valid;
                st.jscc_fallback = jscc_shadow_fallback_string(js.fallback);
                st.jscc_reason = js.valid
                    ? jscc_reason_string(js.decision.reason) : std::string();
                st.jscc_input_k = js.input.source_k;
                st.jscc_input_predicted_symbols =
                    js.input.predicted_loss_symbols;
                st.jscc_input_floor_symbols = js.input.fec_floor_symbols;
                st.jscc_input_cap_symbols = js.input.fec_cap_symbols;
                st.jscc_input_deadline_us = js.input.deadline_us;
                st.jscc_input_source_tx_us = js.input.source_tx_remaining_us;
                st.jscc_input_rtt_p95_us = js.input.rtt_p95_us;
                st.jscc_input_resend_us = js.input.resend_airtime_us;
                st.jscc_input_guard_us = js.input.arq_guard_us;
                st.jscc_output_parity_symbols = js.decision.parity_symbols;
                st.jscc_output_remaining_us =
                    js.decision.remaining_after_source_us;
                st.jscc_output_arq_eligible = js.decision.arq_eligible;
                st.jscc_output_discard = js.decision.discard;
                st.jscc_feedback_epoch = js.feedback_epoch;
                st.jscc_feedback_age_ms = js.feedback_age_ms;
                st.jscc_enforced_frames = s.jscc_enforced_frames;
                st.jscc_discarded_frames = s.jscc_discarded_frames;
            }
            st.resends_sent = s.sched.counters().resends_sent;
            st.arq_lock_holder = s.sched.counters().lock_holder;
            st.double_send_suppressed =
                s.sched.counters().holddown_suppressed;
            st.active_profile = selector_.profile_id();
            st.table_version = table_version_;
            snap.streams.push_back(std::move(st));
        }
        snap.link.profile = selector_.profile_id();
        snap.link.mcs = selector_.mcs();
        snap.link.report_epoch = selector_.report_epoch();
        snap.link.report_age_ms =
            static_cast<uint32_t>(selector_.report_age_ms(now));
        snap.link.state = selector_.state();
        snap.link.transition_reason =
            selector_reason_name(selector_.reason());
        snap.link.loss_window_milli = selector_.loss_window_milli();
        snap.link.loss_ewma_milli = selector_.loss_ewma_milli();
        snap.link.loss_uniq = selector_.loss_uniq();
        snap.link.loss_score = selector_.loss_score();
        snap.link.safe_floor_profile = selector_.safe_floor_profile();
        snap.link.selector_state_valid = true;  // local selector authority
        const SelectorLockout lock = selector_.lockout(now);
        snap.link.lockout_active = lock.active;
        snap.link.lockout_latched = lock.latched;
        snap.link.lockout_profile = lock.profile;
        snap.link.lockout_ceiling_profile = lock.ceiling_profile;
        snap.link.lockout_remaining_ms = lock.remaining_ms;
        snap.link.lockout_strikes = lock.strikes;
        snap.link.lockout_active_mask = lock.active_mask;
        snap.link.lockout_latched_mask = lock.latched_mask;
        snap.link.lockout_conflict = lock.conflict;
        snap.link.flap_freeze = selector_.flap_frozen(now);
        // §9.6 actuator state (Pass 37).
        snap.link.venc_bitrate_kbps = venc_.commanded_bitrate_kbps();
        snap.link.venc_max_i_bytes = venc_.commanded_max_i_bytes();
        snap.link.venc_max_p_bytes = venc_.commanded_max_p_bytes();
        snap.link.venc_pushes = venc_.pushes();
        snap.link.venc_failures = venc_.failures();
        snap.link.venc_settling = venc_.settling(now);
        snap.link.venc_fps = venc_.commanded_fps();
        snap.link.cmd_arq = cmd_arq_enabled_;
        snap.link.cmd_selector_frozen = cmd_selector_frozen_;
        snap.link.cmd_fps_ladder = cmd_fps_ladder();
        snap.link.cmd_fps_select = cmd_fps_select_;
        if (calibrator_) {  // §10.6 Pass 120
            switch (calibrator_->state()) {
                case CalibState::kIdle: snap.link.calib_state = "idle"; break;
                case CalibState::kRunning:
                    snap.link.calib_state = "running"; break;
                case CalibState::kDone: snap.link.calib_state = "done"; break;
                case CalibState::kFailed:
                    snap.link.calib_state = "failed"; break;
            }
            snap.link.calib_rung = calibrator_->rung();
            snap.link.calib_fingerprint = calib_fingerprint_;
            snap.link.calib_stale = calib_stale_;
        }
        // cmd_resolution_select / cmd_framing_select stay 0 until the venc
        // knobs exist (§11.7 staged, Pass 71).
        if (fps_ladder_) {
            snap.link.venc_p_frame_bytes =
                fps_ladder_->observed_p_frame_bytes();
            snap.link.venc_p_frame_target_bytes =
                fps_ladder_->target_p_frame_bytes();
            snap.link.venc_fps_ladder_state = fps_ladder_->state();
        }
        // §10.5: while the override-latch is set it IS the hardware value.
        snap.link.tx_power_override = power_override_.has_value();
        if (power_override_) {
            snap.link.tx_power_qdb = *power_override_;
        } else {
            for (const PowerAdapter& pa : power_) {
                if (pa.applied_qdb) {
                    snap.link.tx_power_qdb = *pa.applied_qdb;  // first TX adapter
                    break;
                }
            }
        }
        // §7.3 return-path visibility: the RX's epoch counter says how many
        // reports it SENT; we know how many arrived.
        snap.ret.reports_expected = selector_.report_epoch();
        snap.ret.reports_received = reports_received_;
        snap.ret.reports_rejected = report_gate_.rejected();  // §3.5 Pass 41
        snap.ret.feedback_rejected = feedback_gate_.rejected();   // Pass 115
        snap.ret.report_latch_holder = report_gate_.latched_originator();
        snap.link.report_latch_holder = report_gate_.latched_originator();
        snap.link.report_latch_known = true;   // local gate; always known here
    }

    struct PowerAdapter {
        std::string name;
        size_t adapter_idx;  // position in cfg.adapters == radio index
        PowerCurve curve;
        std::optional<int32_t> ceiling;
        std::optional<int32_t> applied_qdb;
    };
    // §10.5 override targets: every role:"tx" adapter, curve or not.
    struct PowerTarget {
        std::string name;
        size_t adapter_idx;
        std::optional<int32_t> ceiling;  // §10.3 — the ONE clamp on overrides
    };

    // §10.4 curve resolve for the committed operating point, through the
    // change-detection cache (apply only when the resolved value moves).
    void resolve_and_apply_power(uint8_t mcs, uint8_t level) {
        for (PowerAdapter& pa : power_) {
            const auto qdb =
                resolve_power_qdb(pa.curve, mcs, level, pa.ceiling);
            if (qdb && (!pa.applied_qdb || *pa.applied_qdb != *qdb)) {
                // §10.5: cache only what the backend accepted — a failed
                // write retries at the next commit/re-assert.
                bool ok = true;
                if (apply_power) {
                    ok = apply_power(pa.adapter_idx, *qdb);
                } else {
                    std::fprintf(stderr, "power: %s mcs=%u level=%u -> %d qdb\n",
                                 pa.name.c_str(), mcs, level, *qdb);
                }
                if (ok) pa.applied_qdb = *qdb;
            }
        }
    }

    // §10.5 (Pass 114) override-latch: latch an absolute qdb on every tx
    // adapter; the §10.4 commit resolve yields until cleared. Ceiling-clamped
    // per adapter (§10.3) — nothing else bounds it.
    void set_power_override(int32_t qdb) {
        power_override_ = qdb;
        for (const PowerTarget& t : power_targets_) {
            const int32_t v =
                t.ceiling ? std::min(qdb, *t.ceiling) : qdb;
            if (apply_power) {
                apply_power(t.adapter_idx, v);
            } else {
                std::fprintf(stderr, "power: %s override -> %d qdb\n",
                             t.name.c_str(), v);
            }
        }
    }

    // §10.5 auto: clear the latch with one forced immediate restore —
    // apply_power_auto (backend default) first, then re-resolve the curve at
    // the last committed operating point so a loaded power_map re-asserts
    // without waiting for the next profile change.
    void clear_power_override() {
        power_override_.reset();
        if (apply_power_auto) {
            for (const PowerTarget& t : power_targets_) {
                apply_power_auto(t.adapter_idx);
            }
        }
        for (PowerAdapter& pa : power_) {
            pa.applied_qdb.reset();  // force re-apply even at the same value
        }
        if (last_commit_mcs_) {
            resolve_and_apply_power(*last_commit_mcs_, last_commit_level_);
        }
    }

    // §10.5 re-assert after any event that may have reset hardware power (a
    // §11 retune's TXAGC reset, a §11.6 monitor recovery's `txpower auto`).
    void reassert_power() {
        if (power_override_) {
            set_power_override(*power_override_);
            return;
        }
        for (PowerAdapter& pa : power_) {
            if (pa.applied_qdb && apply_power) {
                if (!apply_power(pa.adapter_idx, *pa.applied_qdb)) {
                    pa.applied_qdb.reset();  // §10.5: retry at next commit
                }
            }
        }
    }

    std::optional<int32_t> power_override() const { return power_override_; }
    bool has_power_targets() const { return !power_targets_.empty(); }

    // Actuation hooks (§10.4/§10.5); unset = logged intent. apply_mode is
    // radio-only by design (Pass 13: monitor carries MCS per-packet).
    std::function<void(uint8_t mcs, bool sgi)> apply_mode;
    std::function<bool(size_t adapter_idx, int32_t qdb)> apply_power;
    std::function<void(size_t adapter_idx)> apply_power_auto;
    std::function<std::optional<uint32_t>(size_t bytes, bool include_pending)>
        estimate_airtime;

    uint16_t originator_;
    uint32_t session_;
    uint8_t table_version_;
    const ProfileTable* table_;
    Selector selector_;
    VencActuator venc_;
    VencCfg venc_knobs_;            // §9.6 cap knobs (Pass 37)
    std::optional<FpsLadder> fps_ladder_;  // §9.11 (Pass 39)
    uint16_t arq_max_fps_ = 100;           // §4.1 Pass 40 cutoff
    bool arq_fps_suppressed_ = false;
    // §11.7 remote command state (craft-session-volatile).
    bool cmd_arq_enabled_ = true;
    bool cmd_selector_frozen_ = false;
    // §11.7 v2 FPS_SELECT (Pass 71): stats index (1-based, 0 = none this
    // session) and the re-offer target (cleared when the ladder resumes).
    uint8_t cmd_fps_select_ = 0;
    uint16_t cmd_fps_select_hz_ = 0;
    bool cmd_fps_enabled_ = true;
    uint8_t boot_min_profile_ = 0;   // §9.7 boot envelope for SELECTOR run
    uint8_t boot_max_profile_ = 255;
    ReportGate feedback_gate_;             // §3.10 Pass 55
    ReportGate report_gate_;               // §3.5 Pass 41
    uint64_t frame_cadence_us_ = 0; // windowed ingress cadence estimate
    uint64_t cadence_start_ms_ = 0;
    uint32_t cadence_frames_ = 0;
    std::vector<PowerAdapter> power_;
    std::vector<PowerTarget> power_targets_;      // §10.5 all tx adapters
    std::optional<int32_t> power_override_;       // §10.5 latch (volatile)
    std::optional<uint8_t> last_commit_mcs_;      // §10.5 clear-restore point
    uint8_t last_commit_level_ = 4;
    uint32_t reports_received_ = 0;
    std::vector<Stream> streams_;

  public:
    // §10.6 (Pass 120) craft-resident calibration. The engine is core/; the
    // app maps its actions onto the §9.7 pin and §10.5 power seams and
    // persists the artifact through on_calib_artifact.
    std::optional<Calibrator> calibrator_;
    std::function<void(const CalibArtifact&)> on_calib_artifact;
    uint8_t calib_fingerprint_ = 0;   // CRC-8 of persisted artifact (0=none)
    bool calib_stale_ = false;        // persisted artifact pairing mismatch

    void calibrate_service(uint64_t now) {
        if (!calibrator_) return;
        const CalibActions a = calibrator_->tick(now);
        // Artifact BEFORE restore: persisting installs the fresh curve, so
        // the restore resolve below re-places the committed rung from it.
        if (a.artifact_ready && on_calib_artifact) {
            on_calib_artifact(calibrator_->artifact());
        }
        if (a.restore) {
            // R4 order: power first (a probe may sit on a rung's ceiling),
            // then the selector window, then the §10.4 resolve.
            for (PowerAdapter& pa : power_) {
                pa.applied_qdb.reset();  // force a real re-apply
            }
            if (cmd_selector_frozen_) {
                const uint8_t pf = selector_.profile_id();
                selector_.set_profile_pin(pf, pf);
            } else {
                selector_.set_profile_pin(boot_min_profile_,
                                          boot_max_profile_);
            }
            if (last_commit_mcs_ && !power_override_) {
                resolve_and_apply_power(*last_commit_mcs_,
                                        last_commit_level_);
            }
            std::fprintf(stderr, "calibrate: %s%s%s\n",
                         calibrator_->state() == CalibState::kDone
                             ? "done"
                             : "failed",
                         calibrator_->fail_reason() ? " reason=" : "",
                         calibrator_->fail_reason()
                             ? calibrator_->fail_reason()
                             : "");
        }
        if (a.set_qdb) {
            // §10.6: every tx adapter, curve or not (power_targets_) — the
            // run exists precisely because no curve may be loaded yet.
            for (const PowerTarget& t : power_targets_) {
                const int32_t v = t.ceiling
                                      ? std::min(*a.set_qdb, *t.ceiling)
                                      : *a.set_qdb;
                if (apply_power) (void)apply_power(t.adapter_idx, v);
            }
        }
        if (a.pin_rung) {
            selector_.set_profile_pin(*a.pin_rung, *a.pin_rung);
        }
    }
    bool has_power_curve() const { return !power_.empty(); }
    // §10.6: install/replace the tx adapters' §10.2 curve (calibration
    // artifact or boot auto-load) so the commit resolve uses it.
    void install_curve(const PowerCurve& c) {
        power_.clear();
        for (const PowerTarget& t : power_targets_) {
            power_.push_back(
                PowerAdapter{t.name, t.adapter_idx, c, t.ceiling,
                             std::nullopt});
        }
    }
    bool calibrating() const {
        return calibrator_ &&
               calibrator_->state() == CalibState::kRunning;
    }
};

// ---- RX side: engine + NACK encode -----------------------------------------

struct RxCore {
    // (frame, len, target_originator) — the target rides along so returns
    // can be addressed as §3.0 unicast when return.unicast is on.
    using Inject = std::function<void(const uint8_t*, size_t, uint16_t)>;

    RxCore(const Config& cfg, uint32_t session, const ProfileTable* table,
           std::optional<uint8_t> table_version)
        : originator_(cfg.node.originator),
          session_(session),
          table_(table),
          local_table_version_(table_version),
          engine_(rx_policy(cfg), wants(cfg), table, table_version),
          reporter_(ReporterPolicy{cfg.policy.report_hz > 0
                                       ? static_cast<uint32_t>(
                                             1000.0 / cfg.policy.report_hz)
                                       : 0},
                    table_version),
          feedback_period_ms_(cfg.policy.report_hz > 0
                                  ? static_cast<uint32_t>(
                                        1000.0 / cfg.policy.report_hz)
                                  : 0),
          // §3.9 Pass 106. A spectator (§2 Pass 74) has no uplink, so it never
          // emits regardless of the knob — gated here rather than relying on
          // the return inject to no-op, so it also stays quiet in the log.
          recovery_on_latch_(cfg.node.recovery_on_latch &&
                             !cfg.node.spectator) {}

    static std::vector<WantSpec> wants(const Config& cfg) {
        std::vector<WantSpec> out;
        for (const StreamCfg& s : cfg.streams) {
            if (s.dir == Dir::kOut) {
                out.push_back(WantSpec{s.stream_id, s.stream_type,
                                       s.originator});
            }
        }
        return out;
    }

    void on_air(uint8_t adapter, const uint8_t* d, size_t n, uint64_t now,
                const RxEngine::Deliver& deliver, int8_t rssi = 0,
                const RxEngine::EarlyDeliver& early_deliver = {}) {
        const Decoded dec = decode(d, n);
        if (const DataView* v = std::get_if<DataView>(&dec)) {
            engine_.on_data(adapter, *v, now, deliver, rssi, early_deliver);
            return;
        }
        if (const SelectorState* s = std::get_if<SelectorState>(&dec)) {
            for (const RxStreamInfo& info : engine_.streams()) {
                if (info.stream_type == stream_type::kRtp &&
                    selector_state_admissible(
                        *s, local_table_version_, info.key.originator,
                        info.key.session_id)) {
                    remote_selector_state_ = *s;
                    remote_selector_state_ms_ = now;
                    return;
                }
            }
        }
    }

    void complete_frame(uint8_t stream_id, uint32_t block_id, uint64_t now,
                        const RxEngine::Deliver& deliver) {
        engine_.complete_frame(stream_id, block_id, now, deliver);
    }

    bool defer_first_nack(uint8_t stream_id, uint32_t block_id,
                          uint64_t not_before_ms) {
        return engine_.defer_first_nack(stream_id, block_id, not_before_ms);
    }

    bool block_had_nack(uint8_t stream_id, uint32_t block_id) const {
        return engine_.block_had_nack(stream_id, block_id);
    }

    void tick(uint64_t now, const RxEngine::Deliver& deliver,
              const Inject& inject_report, const Inject& inject_nack,
              bool emit_nacks = true) {
        engine_.tick(now, deliver);
        // §7.3: LINK_REPORTs ride the same uplink as NACKs.
        for (LinkReport r : reporter_.build(engine_, now)) {
            r.prefix.originator = originator_;
            r.prefix.destination = r.target_originator;
            r.prefix.session_id = session_;
            uint8_t frame[kLinkReportSize];
            if (encode_link_report(r, frame, sizeof(frame)) > 0) {
                inject_report(frame, sizeof(frame), r.target_originator);
            }
        }
        if (!emit_nacks) {
            return;
        }
        for (const NackRequest& req : engine_.build_nacks(now)) {
            NackHeader hdr;
            hdr.prefix.originator = originator_;
            hdr.prefix.destination = req.target_originator;
            hdr.prefix.session_id = session_;
            hdr.target_originator = req.target_originator;
            hdr.target_session = req.target_session;
            hdr.target_stream_id = req.target_stream_id;
            hdr.base_seq = req.base_seq;
            uint8_t frame[kNackFixedSize + 255];
            const size_t n = encode_nack(
                hdr, req.bitmap.data(),
                static_cast<uint8_t>(req.bitmap.size()), frame, sizeof(frame));
            if (n > 0) {
                inject_nack(frame, n, req.target_originator);
            }
        }
    }

    void emit_jscc_feedback(
        uint64_t now,
        const std::vector<std::pair<uint8_t, JsccRepairFeedbackState>>& states,
        const Inject& inject) {
        if (feedback_period_ms_ == 0 || now < next_feedback_ms_) return;
        next_feedback_ms_ = now + feedback_period_ms_;
        for (const RxStreamInfo& info : engine_.streams()) {
            const JsccRepairFeedbackState* state = nullptr;
            for (const auto& [sid, candidate] : states) {
                if (sid == info.local_stream_id) {
                    state = &candidate;
                    break;
                }
            }
            if (state == nullptr) continue;  // not a frame-SHM egress
            JsccFeedback f;
            f.prefix = {originator_, info.key.originator, session_};
            f.target_originator = info.key.originator;
            f.target_session = info.key.session_id;
            f.target_stream_id = info.key.stream_id;
            f.feedback_epoch = ++feedback_epoch_;
            f.repair_demand_permille = state->repair_demand_permille;
            f.rtt_p95_us = info.counters.nack_rtt_p95_us;
            f.repair_samples = state->repair_samples;
            f.rtt_samples = info.counters.nack_rtt_samples;
            if (state->repair_ready) {
                f.valid_flags |= jscc_feedback_flags::kRepairReady;
            }
            if (f.rtt_samples > 0) {
                f.valid_flags |= jscc_feedback_flags::kRttReady;
            }
            f.observed_block_id = state->observed_block_id;
            uint8_t frame[kJsccFeedbackSize];
            if (encode_jscc_feedback(f, frame, sizeof(frame)) == sizeof(frame)) {
                inject(frame, sizeof(frame), f.target_originator);
            }
        }
    }

    void fill_stats(StatsSnapshot& snap, uint64_t now) const {
        for (const RxStreamInfo& info : engine_.streams()) {
            StreamStats st;
            st.stream_id = info.local_stream_id;
            st.type = info.stream_type == stream_type::kRtp ? "RTP" : "OTHER";
            st.seq = info.counters.highest_seq;
            st.delivered = info.counters.delivered;
            st.uniq = info.counters.uniq;
            st.diversity = info.counters.diversity;
            st.loss_prediversity_milli =
                info.counters.prediv_expected == 0
                    ? 0
                    : static_cast<uint32_t>(
                          info.counters.prediv_lost * 1000 /
                          info.counters.prediv_expected);
            const uint64_t denom =
                info.counters.uniq + info.counters.lost_declared;
            st.loss_postdiv_prearq_milli =
                denom == 0 ? 0
                           : static_cast<uint32_t>(
                                 info.counters.lost_declared * 1000 / denom);
            st.recovered_arq = info.counters.recovered_arq;
            st.dropped_superseded = info.counters.dropped_superseded;
            st.dropped_deadline = info.counters.dropped_deadline;
            st.nacks_sent = info.counters.nacks_sent;
            st.nack_rtt_hist = info.counters.nack_rtt_hist;
            st.nack_rtt_max_ms = info.counters.nack_rtt_max_ms;
            st.nack_rtt_samples = info.counters.nack_rtt_samples;
            st.nack_rtt_p95_us = info.counters.nack_rtt_p95_us;
            st.arq_rec_hist = info.counters.arq_rec_hist;
            st.arq_rec_max_ms = info.counters.arq_rec_max_ms;
            st.active_profile = info.active_profile;
            snap.streams.push_back(std::move(st));
        }
        for (const auto& [id, a] : engine_.adapters()) {
            // Radio backend: the air layer already emitted this adapter's
            // counters (same index order) — graft the §6 RX-liveness stall
            // onto that entry instead of duplicating it as "vadapterN".
            if (id < snap.adapters.size()) {
                snap.adapters[id].adapter_stalled = a.stalled;
                continue;
            }
            AdapterStats as;
            as.name = "vadapter" + std::to_string(id);
            as.rx = a.rx;
            as.adapter_stalled = a.stalled;
            snap.adapters.push_back(std::move(as));
        }
        bool selector_source_current = false;
        if (remote_selector_state_) {
            for (const RxStreamInfo& info : engine_.streams()) {
                if (info.stream_type == stream_type::kRtp &&
                    selector_state_admissible(
                        *remote_selector_state_, local_table_version_,
                        info.key.originator, info.key.session_id)) {
                    selector_source_current = true;
                    break;
                }
            }
        }
        if (selector_source_current &&
            selector_state_fresh(now, remote_selector_state_ms_)) {
            const SelectorState& s = *remote_selector_state_;
            const uint64_t age = now - remote_selector_state_ms_;
            snap.link.profile = s.active_profile;
            if (table_ != nullptr) {
                for (const Profile& p : table_->profiles) {
                    if (p.id == s.active_profile) {
                        snap.link.mcs = p.mcs;
                        break;
                    }
                }
            }
            snap.link.transition_reason = selector_reason_name(
                static_cast<SelectorReason>(s.transition_reason));
            snap.link.loss_window_milli = s.loss_window_milli;
            snap.link.loss_ewma_milli = s.loss_ewma_milli;
            snap.link.loss_uniq = s.loss_uniq;
            snap.link.loss_score = s.loss_score;
            snap.link.safe_floor_profile = s.safe_floor_profile;
            // §3.15a: bit3 clear means the craft did not report it (legacy
            // build) — distinct from "nobody holds the latch", which is bit3
            // set with holder 0.
            if ((s.state_flags & selector_state_flags::kHolderPresent) != 0) {
                snap.link.report_latch_holder = s.report_latch_holder;
                snap.link.report_latch_known = true;
            }
            snap.link.selector_state_valid = true;
            snap.link.selector_state_age_ms = static_cast<uint32_t>(age);
            snap.link.lockout_active =
                (s.state_flags & selector_state_flags::kActive) != 0;
            snap.link.lockout_latched =
                (s.state_flags & selector_state_flags::kLatched) != 0;
            snap.link.lockout_conflict =
                (s.state_flags & selector_state_flags::kConflict) != 0;
            snap.link.lockout_profile = s.lockout_profile;
            snap.link.lockout_ceiling_profile = s.ceiling_profile;
            snap.link.lockout_remaining_ms =
                s.remaining_ms > age
                    ? static_cast<uint32_t>(s.remaining_ms - age)
                    : 0;
            snap.link.lockout_strikes = s.lockout_strikes;
            snap.link.lockout_active_mask = s.lockout_active_mask;
            snap.link.lockout_latched_mask = s.lockout_latched_mask;
        }
    }

    // §15.5 stats/reset. The frame-shm reassemblers live in run_rx (ShmOut),
    // so the caller resets those; here we zero the RX engine's counters.
    // §3.5: the reporter's loss window deltas are taken against the engine's
    // counters, so resetting one without the other underflows the next
    // LINK_REPORT to 0 permille — failing OPTIMISTIC, which §3.5 forbids.
    void reset_stats() {
        engine_.reset_stats();
        reporter_.reset_link();
    }

    void select_originator(uint16_t originator) {
        engine_.select_originator(originator);
        reporter_.reset_link();
        next_feedback_ms_ = 0;
        remote_selector_state_.reset();
        remote_selector_state_ms_ = 0;
    }

    std::optional<uint16_t> selected_originator() const {
        return engine_.selected_originator();
    }

    // §15.5 Pass 108: the originator this node is actually LATCHED to, as
    // opposed to selected_originator()'s configured output-want pin (which is
    // nullopt on the common ground that names no preferred_originator — the
    // reason latch adoption cannot read it). nullopt when nothing is latched,
    // or when latched streams disagree: an ambiguous binding is no binding.
    std::optional<uint16_t> latched_originator() const {
        std::optional<uint16_t> found;
        for (const RxStreamInfo& info : engine_.streams()) {
            if (found && *found != info.key.originator) return std::nullopt;
            found = info.key.originator;
        }
        return found;
    }

    std::string request_recovery(int local_stream_id, const Inject& inject) {
        std::optional<RxStreamInfo> selected;
        for (const RxStreamInfo& info : engine_.streams()) {
            if (info.stream_type != stream_type::kRtp ||
                (local_stream_id >= 0 &&
                 info.local_stream_id != static_cast<uint8_t>(local_stream_id))) {
                continue;
            }
            if (selected && local_stream_id < 0) {
                return "stream_id required when multiple RTP streams are latched";
            }
            selected = info;
        }
        if (!selected) {
            return "no matching latched RTP stream";
        }
        RecoveryRequest req;
        req.prefix = {originator_, selected->key.originator, session_};
        req.target_originator = selected->key.originator;
        req.target_session = selected->key.session_id;
        req.target_stream_id = selected->key.stream_id;
        uint8_t frame[kRecoveryRequestSize];
        if (encode_recovery_request(req, frame, sizeof(frame)) != sizeof(frame)) {
            return "failed to encode recovery request";
        }
        inject(frame, sizeof(frame), req.target_originator);
        return "";
    }

    // §3.9 Pass 106: fresh latch is itself a decoder-bootstrap event. The link
    // cannot see decoder readiness (VFRM v1 carries no consumer generation,
    // §15.4) and a GDR craft emits no IRAP unasked, so the first latch is both
    // the only moment we can detect and the only one at which an IRAP is
    // guaranteed absent from everything the consumer will ever see. Bounded at
    // kLatchRecoveryAttempts: the stop condition is unobservable on RTP egress,
    // and a craft with venc.recovery_enabled false would otherwise draw a
    // return every second for the whole flight.
    void emit_latch_recovery(uint64_t now, const Inject& inject) {
        if (!recovery_on_latch_) return;
        // Called once per rx-loop iteration: reuse the scratch buffer so the
        // steady state costs a scan and no allocation. due() likewise returns
        // an empty vector (no allocation) on the overwhelmingly common tick
        // where nothing is due.
        latch_scratch_.clear();
        for (const RxStreamInfo& info : engine_.streams()) {
            if (info.stream_type != stream_type::kRtp) continue;
            latch_scratch_.push_back(LatchStream{info.key, info.local_stream_id});
        }
        for (const StreamKey& key : latch_recovery_.due(latch_scratch_, now)) {
            RecoveryRequest req;
            req.prefix = {originator_, key.originator, session_};
            req.target_originator = key.originator;
            req.target_session = key.session_id;
            req.target_stream_id = key.stream_id;
            uint8_t frame[kRecoveryRequestSize];
            if (encode_recovery_request(req, frame, sizeof(frame)) !=
                sizeof(frame)) {
                continue;
            }
            inject(frame, sizeof(frame), req.target_originator);
            std::fprintf(stderr,
                         "rx: latch recovery stream=%u origin=%u attempt %u/%u\n",
                         key.stream_id, key.originator,
                         unsigned(latch_recovery_.attempts(key)),
                         unsigned(LatchRecovery::kAttempts));
        }
    }

    // §3.9 Pass 106 early exit: an IRAP-flagged frame reaching a frame-SHM
    // egress ring is the link's own observable proxy for "the consumer now has
    // something to start from". RTP egress has no equivalent — there the
    // attempt bound is the only stop.
    void note_egress_irap(uint8_t local_stream_id) {
        latch_recovery_.note_irap(local_stream_id);
    }

    std::vector<StreamKey> stream_keys() const {
        std::vector<StreamKey> out;
        for (const RxStreamInfo& info : engine_.streams()) {
            out.push_back(info.key);
        }
        return out;
    }

    uint16_t originator_;
    uint32_t session_;
    const ProfileTable* table_;
    std::optional<uint8_t> local_table_version_;
    RxEngine engine_;
    Reporter reporter_;
    uint32_t feedback_period_ms_ = 0;
    uint64_t next_feedback_ms_ = 0;
    uint32_t feedback_epoch_ = 0;
    bool recovery_on_latch_ = false;
    LatchRecovery latch_recovery_;
    std::vector<LatchStream> latch_scratch_;  // reused; see emit_latch_recovery
    std::optional<SelectorState> remote_selector_state_;
    uint64_t remote_selector_state_ms_ = 0;
};

// ---- shared setup -----------------------------------------------------------

struct Loaded {
    Config cfg;
    ProfileTable table;
    bool have_table = false;
    uint8_t tv = 0;
};

int load_all(const std::string& config_path, Loaded& out) {
    auto cfg = load_config(config_path);
    if (!cfg) {
        std::fprintf(stderr, "config error: %s\n", cfg.error.c_str());
        return 1;
    }
    out.cfg = std::move(*cfg.value);
    std::fputs(dump_config_summary(out.cfg).c_str(), stderr);
    if (!out.cfg.profile_table_path.empty()) {
        auto table = load_profile_table(out.cfg.profile_table_path);
        if (!table) {
            std::fprintf(stderr, "profile table error: %s\n",
                         table.error.c_str());
            return 1;
        }
        out.table = std::move(*table.value);
        out.have_table = true;
        out.tv = table_version(out.table);
        std::fprintf(stderr,
                     "profile table: %zu profiles, table_version=0x%02X\n",
                     out.table.profiles.size(), out.tv);
        // §9.7 (Pass 83): min/max_profile are profile IDs. An id absent from
        // the table is a config error, not a silent clamp onto a neighbouring
        // rung — the operator asked for an operating envelope that this table
        // cannot express. 255 is the documented "unpinned top" sentinel.
        const SelectPolicy& sel = out.cfg.policy.select;
        const auto has_id = [&out](uint8_t id) {
            for (const Profile& p : out.table.profiles) {
                if (p.id == id) return true;
            }
            return false;
        };
        const std::pair<const char*, uint8_t> pins[] = {
            {"min_profile", sel.min_profile},
            {"max_profile", sel.max_profile}};
        for (const auto& [name, id] : pins) {
            if (id != 255 && !has_id(id)) {
                std::fprintf(stderr,
                             "config error: policy.select.%s = %u is not a "
                             "profile id in %s (§9.7 ids, not indices)\n",
                             name, static_cast<unsigned>(id),
                             out.cfg.profile_table_path.c_str());
                return 1;
            }
        }
    }
    return 0;
}

// §11.7/§15.3 command-surface fields not owned by Tx/RxCore (the engines
// live in the mode loops): craft nonce, issuer campaign state, rx ARQ gate.
struct VcmdStatsFill {
    uint32_t cmd_last_nonce = 0;       // craft
    const char* vcmd_state = nullptr;  // issuer (null = not an issuer)
    uint32_t vcmd_nonce = 0;
    bool arq_rx_enabled = true;        // rx gate
};

void emit_stats(StatsEmitter& emitter, const Loaded& l, uint32_t session,
                uint64_t t0, const TxCore* tx, const RxCore* rx,
                const AirBackend* air = nullptr,
                uint64_t tsf_fallbacks = 0,
                const char* csa_state = nullptr,
                uint32_t ret_window_hits = 0,
                uint32_t ret_window_misses = 0,
                bool tx_wedged = false,
                const std::vector<std::pair<uint8_t, FrameReassemblerStats>>*
                    frame_stats = nullptr,
                const std::vector<std::pair<uint8_t, FrameShmRing::Stats>>*
                    shm_stats = nullptr,
                const CacheRepairStatsOut* cache_repair = nullptr,
                const CacheStoreStatsOut* cache_store = nullptr,
                StatsSnapshot* out_snap = nullptr,
                const ArqTimingStats* arq_timing = nullptr,
                const VcmdStatsFill* vcmd = nullptr,
                uint16_t channel_mhz = 0) {
    const uint64_t now = now_ms();
    StatsSnapshot snap;
    snap.t_ms = now - t0;
    snap.node = l.cfg.node.originator;
    snap.session = session;
    // §7.2 observability (ground): paced vs blind coalesced return batches.
    snap.ret.return_window_hits = ret_window_hits;
    snap.ret.return_window_misses = ret_window_misses;
    // §3.0 Pass 12: unicast-return counters (radio backend only).
    if (air != nullptr) {
        air->fill_return_stats(snap.ret);
    }
    if (csa_state != nullptr) {
        snap.link.csa_state = csa_state;
    }
    snap.link.channel_mhz = channel_mhz;  // §11 current operating channel (0 = not tracked)
    if (tx != nullptr) {
        tx->fill_stats(snap, now);
    }
    if (vcmd != nullptr) {
        snap.link.cmd_last_nonce = vcmd->cmd_last_nonce;
        if (vcmd->vcmd_state != nullptr) {
            snap.link.vcmd_state = vcmd->vcmd_state;
            snap.link.vcmd_nonce = vcmd->vcmd_nonce;
        }
        snap.link.arq_rx_enabled = vcmd->arq_rx_enabled;
    }
    // Air adapters first so RxCore::fill_stats can merge its per-adapter
    // liveness view into them by index (radio backend; no-op on udp).
    if (air != nullptr) {
        air->fill_adapter_stats(snap, tsf_fallbacks, tx_wedged);
    }
    if (rx != nullptr) {
        rx->fill_stats(snap, now);
    }
    // §6.3a frame-shm egress: fold each reassembler's frame-level outcomes into
    // the matching stream (by stream_id). recovered_arq / delivered / loss stay
    // the packet-layer view from RxEngine; these are the frame-layer view.
    if (frame_stats != nullptr) {
        for (const auto& [sid, fr] : *frame_stats) {
            StreamStats* st = nullptr;
            for (StreamStats& s : snap.streams) {
                if (s.stream_id == sid) {
                    st = &s;
                    break;
                }
            }
            if (st == nullptr) {  // not latched yet — surface it anyway
                snap.streams.push_back(StreamStats{});
                st = &snap.streams.back();
                st->stream_id = sid;
                st->type = "RTP";
            }
            st->recovered_fec = fr.frames_fec;
            st->fec_recovered_source_symbols =
                fr.fec_recovered_source_symbols;
            st->arq_recovered_source_symbols =
                fr.arq_recovered_source_symbols;
            st->arq_recovered_repair_symbols =
                fr.arq_recovered_repair_symbols;
            st->frames_with_arq = fr.frames_with_arq;
            st->frames_fec_only = fr.frames_fec_only;
            st->frames_fec_after_arq = fr.frames_fec_after_arq;
            st->frames_fast = fr.frames_fast;
            st->frames_unrecoverable = fr.frames_unrecoverable;
            st->malformed = fr.malformed;
            st->jscc_shadow_blocks = fr.jscc_shadow_blocks;
            st->jscc_predicted_loss_symbols = fr.jscc_predicted_loss_symbols;
            st->jscc_observed_loss_symbols = fr.jscc_observed_loss_symbols;
            st->jscc_underpredicted_blocks = fr.jscc_underpredicted_blocks;
            st->jscc_predicted_parity_symbols =
                fr.jscc_predicted_parity_symbols;
            st->jscc_predicted_repair_symbols =
                fr.jscc_predicted_repair_symbols;
            st->jscc_observed_repair_symbols =
                fr.jscc_observed_repair_symbols;
            st->jscc_repair_underpredicted_blocks =
                fr.jscc_repair_underpredicted_blocks;
            st->jscc_repair_demand_censored_blocks =
                fr.jscc_repair_demand_censored_blocks;
            st->jscc_repair_predicted_parity_symbols =
                fr.jscc_repair_predicted_parity_symbols;
            st->decode_errors = fr.decode_failures;
            st->dropped_superseded = fr.frames_superseded;
            st->dropped_deadline = fr.frames_deadline;
        }
    }
    if (shm_stats != nullptr) {
        for (const auto& [sid, ss] : *shm_stats) {
            for (StreamStats& st : snap.streams) {
                if (st.stream_id == sid) {
                    st.frame_count = ss.reads + ss.writes;
                    st.frame_bytes = ss.frame_bytes;
                    st.frame_size_last = ss.frame_size_last;
                    st.frame_size_min = ss.frame_size_min;
                    st.frame_size_max = ss.frame_size_max;
                    st.frame_interval_us = ss.frame_interval_us;
                    st.frame_jitter_us = ss.frame_jitter_us;
                    // §15.3 Pass 109: ingress full_drops/throttle are published
                    // only by a producer carrying the exact health marker.
                    // Egress retains its local producer counters; ring_full
                    // remains independent consumer-side evidence.
                    st.shm_health_valid = ss.health_valid;
                    st.shm_full_drops = ss.full_drops;
                    st.shm_throttle_permille = ss.throttle_permille;
                    st.shm_oversize_drops = ss.oversize_drops;
                    st.shm_bad_slots = ss.bad_slots;
                    st.shm_ring_full = ss.ring_full;
                    break;
                }
            }
        }
    }
    // §15.3: cache blocks present only when the §14.3 role is enabled.
    if (cache_repair != nullptr) {
        snap.cache_repair = *cache_repair;
    }
    if (cache_store != nullptr) {
        snap.cache_store = *cache_store;
    }
    if (arq_timing != nullptr) {
        snap.arq_timing = *arq_timing;
    }
    if (out_snap != nullptr) {
        *out_snap = snap;  // §15.5: GET /health reads the freshest snapshot
    }
    emitter.emit(snap);
}

// §15.5 Pass 113: live TX self state appended to GET /info (channel, pairing
// gate, §11.5a claim). Null on non-craft nodes.
struct InfoSelfState {
    uint16_t channel_mhz = 0;
    bool psk_announced = false;
    std::optional<uint16_t> claimed_by;
};

// §15.5 GET /info — static identity. Hand-built (no json dep in app/); the
// field values are numeric or house-controlled strings (no escaping needed).
std::string build_info_json(const Loaded& l, uint32_t session,
                            const char* role,
                            const InfoSelfState* self = nullptr,
                            const AirBackend* air = nullptr) {
    std::string s = "{\"role\":\"";
    s += role;
    s += "\",\"node\":" + std::to_string(l.cfg.node.originator);
    s += ",\"session\":" + std::to_string(session);
    s += ",\"table_version\":" + std::to_string(l.tv);
    s += ",\"streams\":[";
    bool first = true;
    for (const StreamCfg& st : l.cfg.streams) {
        if (!first) s += ',';
        first = false;
        s += "{\"stream_id\":" + std::to_string(st.stream_id);
        s += ",\"dir\":\"";
        s += (st.dir == Dir::kIn ? "in" : "out");
        s += "\",\"bind\":\"";
        s += (st.bind.kind == BindKind::kFrameShm ? "frame-shm" : "udp");
        s += "\"}";
    }
    s += "],\"adapters\":[";
    first = true;
    for (size_t i = 0; i < l.cfg.adapters.size(); ++i) {
        const AdapterCfg& a = l.cfg.adapters[i];
        if (!first) s += ',';
        first = false;
        // Live channel, not the boot config: a CSA switch, a craft-local
        // retune (§15.5 Pass 113) or a scout sweep all move adapters without
        // touching cfg. On the ground this is the ONLY channel in the object —
        // there is no top-level `channel` outside the TX self-state — so a
        // stale value here is the whole answer, not a detail.
        const uint16_t chan =
            (air != nullptr && i < air->chan_by_adapter.size())
                ? air->chan_by_adapter[i]
                : a.channel_mhz;
        s += "{\"name\":\"" + a.name + "\",\"role\":\"";
        s += (a.role == Role::kTx ? "tx" : "rx");
        s += "\",\"channel\":" + std::to_string(chan) + "}";
    }
    s += "]";
    if (self != nullptr) {  // Pass 113 TX self state
        s += ",\"channel\":" + std::to_string(self->channel_mhz);
        s += ",\"psk_announced\":";
        s += self->psk_announced ? "true" : "false";
        s += ",\"claimed\":";
        s += self->claimed_by ? "true" : "false";
        s += ",\"claimed_by\":" + std::to_string(self->claimed_by.value_or(0));
    }
    s += ",\"control\":\"" + l.cfg.control.bind + "\"}";
    return s;
}

// §15.5 GET /health — terse link summary from the freshest snapshot.
std::string build_health_json(const StatsSnapshot& snap) {
    int32_t rssi_best = 0;
    bool have = false;
    for (const AdapterStats& a : snap.adapters) {
        if (!have || a.rssi_best > rssi_best) {
            rssi_best = a.rssi_best;
            have = true;
        }
    }
    uint32_t loss_milli = 0;
    uint64_t delivered = 0;
    if (!snap.streams.empty()) {
        loss_milli = snap.streams.front().loss_prediversity_milli;
        delivered = snap.streams.front().delivered;
    }
    std::string s = "{\"state\":\"" + snap.link.state + "\"";
    s += ",\"profile\":" + std::to_string(snap.link.profile);
    s += ",\"mcs\":" + std::to_string(snap.link.mcs);
    s += ",\"rssi_best\":" + std::to_string(have ? rssi_best : 0);
    s += ",\"loss_milli\":" + std::to_string(loss_milli);
    s += ",\"delivered\":" + std::to_string(delivered);
    s += ",\"csa_state\":\"" + snap.link.csa_state + "\"}";
    return s;
}

// ---- modes -------------------------------------------------------------------

int run_tx(const Loaded& l) {
    auto air = AirBackend::create(l.cfg);
    if (!air) {
        std::fprintf(stderr, "air error: %s\n", air.error.c_str());
        return 1;
    }
    PacketEventTrace packet_trace("tx");
    air.value->set_packet_trace(&packet_trace);
    auto bindings = BindingSet::create(l.cfg);
    if (!bindings) {
        std::fprintf(stderr, "binding error: %s\n", bindings.error.c_str());
        return 1;
    }
    const uint32_t session = session_nonce();
    // §11.4a key provenance: csa.psk configured ⇒ secret (token off the air);
    // absent ⇒ announced (the per-boot token is both the CSA key and the
    // advertised ANNOUNCE psk). Pass 61: presence is the sole selector.
    // Pass 113: runtime-mutable — the §11.4a pairing gate re-keys the token
    // and flips announcement at runtime; the announce site reads per tick.
    std::array<uint8_t, kAnnouncePskSize> token = announce_token();
    bool psk_announced = l.cfg.policy.csa.psk.empty();
    DiscoveryCatalog discovery;
    TxCore tx(l.cfg, session, l.have_table ? &l.table : nullptr, l.tv);
    // §10.6 (Pass 120): craft-resident calibration — engine seeds, artifact
    // persistence, and the boot auto-load with the fingerprint gate.
    tx.init_calibration(l.cfg.policy.calibration);
    const AdapterCfg* calib_tx_adapter = nullptr;
    for (const AdapterCfg& a : l.cfg.adapters) {
        if (a.role == Role::kTx) {
            calib_tx_adapter = &a;
            break;
        }
    }
    tx.on_calib_artifact = [&](const CalibArtifact& art) {
        const std::string ident =
            calib_tx_adapter ? calib_identity(*calib_tx_adapter) : "udp";
        const uint8_t fp = calib_store_write(
            l.cfg.policy.calibration.artifact_dir, ident, art);
        if (fp == 0) {
            std::fprintf(stderr, "calibrate: artifact write FAILED (%s)\n",
                         l.cfg.policy.calibration.artifact_dir.c_str());
            return;
        }
        PowerCurve c;
        for (size_t m = 0; m < 8; ++m) c.qdb[m] = art.curve_qdb[m];
        c.valid = true;
        tx.install_curve(c);
        tx.calib_fingerprint_ = fp;
        tx.calib_stale_ = false;
        std::fprintf(stderr, "calibrate: artifact persisted fp=0x%02x\n", fp);
    };
    if (auto stored =
            calib_store_load(l.cfg.policy.calibration.artifact_dir);
        stored) {
        const std::string ident =
            calib_tx_adapter ? calib_identity(*calib_tx_adapter) : "udp";
        if (stored.value->identity == ident) {
            // Explicit config power_map wins; the artifact fills the gap.
            if (!tx.has_power_curve()) {
                tx.install_curve(stored.value->curve);
                std::fprintf(stderr,
                             "calibrate: boot auto-load fp=0x%02x (%s)\n",
                             stored.value->fingerprint, ident.c_str());
            }
            tx.calib_fingerprint_ = stored.value->fingerprint;
        } else {
            tx.calib_stale_ = true;  // §10.6: surface, never apply
            std::fprintf(stderr,
                         "calibrate: STALE artifact (stored %s, live %s)\n",
                         stored.value->identity.c_str(), ident.c_str());
        }
    }
    tx.estimate_airtime = [&](size_t bytes, bool include_pending) {
        return air.value->estimate_airtime_us(bytes, include_pending);
    };
    if (air.value->is_radio()) {
        tx.apply_mode = [&](uint8_t mcs, bool sgi) {
            air.value->set_tx_mode(mcs, sgi);
        };
        tx.apply_power = [&](size_t idx, int32_t qdb) {
            return air.value->set_power_qdb(idx, qdb);
        };
        // §10.5 auto restore (Pass 114): backend default power.
        tx.apply_power_auto = [&](size_t idx) {
            air.value->set_power_auto(idx);
        };
    }
    // §15.4 frame-shm ingress: one consumer ring per frame-shm in-stream. The
    // venc producer may not be up yet, so a failed attach is not fatal — it
    // retries lazily in the loop. have_udp_ins tells us whether BindingSet has
    // any pollable ingress fd of its own (frame-shm-only nodes have none).
    struct ShmIn {
        uint8_t stream_id;
        std::string name;
        std::unique_ptr<FrameShmRing> ring;  // null until attached
        bool pending = false;
    };
    std::vector<ShmIn> shm_ins;
    bool have_udp_ins = false;
    for (const StreamCfg& s : l.cfg.streams) {
        if (s.dir != Dir::kIn) {
            continue;
        }
        if (s.bind.kind != BindKind::kFrameShm) {
            have_udp_ins = true;
            continue;
        }
        ShmIn si{s.stream_id, s.bind.name, nullptr, false};
        auto r = FrameShmRing::attach(s.bind.name);
        if (r) {
            si.ring = std::move(*r.value);
            std::fprintf(stderr, "tx: frame-shm '%s' attached\n",
                         s.bind.name.c_str());
        } else {
            std::fprintf(stderr,
                         "tx: frame-shm '%s' not up yet (%s); retrying\n",
                         s.bind.name.c_str(), r.error.c_str());
        }
        shm_ins.push_back(std::move(si));
    }
    std::vector<uint8_t> frame_buf(kFrameRingDefaultSlotSize);
    uint64_t next_shm_attach_ms = 0;
    uint64_t next_shm_identity_check_ms = 0;

    StatsEmitter emitter(l.cfg.stats.to_stdout, bindings.value->stats_egress());
    const uint64_t t0 = now_ms();
    uint64_t next_stats = t0;
    const uint64_t stats_period =
        l.cfg.stats.hz > 0 ? static_cast<uint64_t>(1000.0 / l.cfg.stats.hz)
                           : 0;

    // §7.2 craft side: after an END_OF_BLOCK the pacer holds video for the
    // quiet window so the single radio can hear returns. Frames arriving
    // inside the window queue behind it (order preserved); the backlog
    // override degrades to §7.1 under load.
    QuietGap qg(quietgap_policy(l.cfg));
    std::deque<std::vector<uint8_t>> held;
    ArqTimingTracker arq_timing;
    uint64_t now_us_it = now_us();
    VideoSlotCadence selector_state_cadence(500);
    const auto send_raw = [&](const uint8_t* f, size_t n) {
        // Pass 110 operator boundary: the 2 Hz selector summary owns no TX
        // opportunity. When due, prepend it inside an already-active live RTP
        // slot; video still ends the slot and alone arms the §7.2 quiet gap.
        const bool live_video_slot = frame_is_live_rtp_data(f, n);
        if (live_video_slot) {
            const uint64_t slot_ms = now_us_it / 1000;
            if (selector_state_cadence.due(live_video_slot, slot_ms)) {
                const SelectorState state = tx.selector_state(slot_ms);
                uint8_t sf[kSelectorStateSize];
                if (encode_selector_state(state, sf, sizeof(sf)) ==
                    sizeof(sf)) {
                    (void)selector_state_cadence.note_submitted(
                        air.value->inject(sf, sizeof(sf)), slot_ms);
                }
            }
        }
        air.value->inject(f, n);
        // Pass 78: only video EOBs open a listen window; a held audio
        // datagram flushing here must not re-arm the gap on the rest.
        if (qg.enabled() && frame_is_paced_eob(f, n)) {
            qg.note_eob_sent(now_us_it);
        }
    };
    const TxCore::Inject inject = [&](const uint8_t* f, size_t n) {
        if (!held.empty() ||
            !qg.can_send_video(now_us_it,
                               static_cast<uint32_t>(held.size()))) {
            held.emplace_back(f, f + n);
            return;
        }
        send_raw(f, n);
    };
    const TxCore::Inject inject_resend = [&](const uint8_t* f, size_t n) {
        air.value->inject_resend(f, n);
        arq_timing.note_resend_submitted(f, n, now_us());
    };
    // §11 craft follower: validates campaigns, arms the CSA_ARMED flag, and
    // retunes the (single) radio at the TSF-anchored T_switch. In announced
    // mode (§11.4a) it verifies CSA against the per-boot token; in secret mode
    // csa_params already keyed it from csa.psk.
    CsaParams follower_params = csa_params(l.cfg);
    if (psk_announced) {
        follower_params.psk.assign(token.begin(), token.end());
    }
    CsaFollower csa(follower_params);
    // §15.5 Pass 113: the craft's live operating channel/width — boot values
    // from the first adapter, updated by CSA commits and local channel-set.
    uint16_t cur_chan =
        l.cfg.adapters.empty() ? 0 : l.cfg.adapters[0].channel_mhz;
    /* §11 width CODE (0/1/2), matching ca.bw from CSA commits. */
    uint8_t cur_bw =
        bw_code(l.cfg.adapters.empty() ? 20 : l.cfg.adapters[0].bw);
    // §11.6 Pass 80 post-retune RX-liveness guard state (one-shot).
    std::optional<uint64_t> csa_liveness_deadline_ms;
    bool csa_armed_flag = false;  // §11.6 Pass 89: mirrors csa.campaign_active()
    uint64_t csa_liveness_rx_baseline = 0;
    uint16_t csa_liveness_chan = 0;
    uint8_t csa_liveness_bw = 0;
    // §15.5 operating-mode label (Pass 96): restored at boot, set on accept.
    std::string active_mode = l.cfg.venc.active_mode;
    // §11.7 craft command engine: same key provenance as the CSA follower.
    VcmdParams craft_cmd_params = vcmd_params(l.cfg);
    if (psk_announced) {
        craft_cmd_params.psk.assign(token.begin(), token.end());
    }
    VcmdCraft craft_cmd(craft_cmd_params,
                        CommonPrefix{l.cfg.node.originator, 0, session});
    const VcmdCraft::Apply apply_cmd = [&](uint8_t id, uint8_t arg) {
        if (id == vcmd_id::kMode) {
            // §11.7 0x07 MODE (Pass 105): arg indexes the name-sorted §15.5
            // catalog. Resolve against the SAME enumeration GET /api/v1/modes
            // is built from, then fork the §16 applier — the identical path
            // POST /api/v1/mode takes (Pass 103 self-reassert heals the venc
            // restart). REJECTED (return false) on a non-actuating node or an
            // index past the catalog end.
            if (l.cfg.venc.mode_apply_cmd.empty()) return false;
            const std::string name =
                mode_name_at(mode_catalog_dir(l.cfg.venc), arg);
            if (name.empty()) return false;  // arg ≥ catalog length
            if (!spawn_mode_applier(l.cfg.venc.mode_apply_cmd, name)) {
                return false;
            }
            active_mode = name;  // optimistic; the applier is authoritative
            std::fprintf(stderr, "vcmd: MODE[%u] -> %s (applier %s)\n", arg,
                         name.c_str(), l.cfg.venc.mode_apply_cmd.c_str());
            return true;
        }
        return tx.apply_command(id, arg, now_ms());
    };
    // §9.10 TX-wedge watchdog over the TX adapter's CCX-report counters.
    TxWedge wedge(TxWedgePolicy{l.cfg.air.wedge_window_ms,
                                l.cfg.air.wedge_min_submits});
    // §15.5 REST control plane. TX node owns the profile-pin + FEC-retune
    // knobs; CSA is issuer-only (rx), so h.csa stays null → 409.
    StatsSnapshot last_snap;
    std::unique_ptr<ControlServer> control;
    if (!l.cfg.control.bind.empty()) {
        auto cs = ControlServer::create(l.cfg.control.bind);
        if (!cs) {
            std::fprintf(stderr, "control: %s\n", cs.error.c_str());
            return 1;
        }
        control = std::move(*cs.value);
        ControlHandlers h;
        h.stats_line = [&]() -> std::string {
            std::string s = emitter.last_line();
            if (!s.empty() && s.back() == '\n') s.pop_back();
            return s;
        };
        h.info_json = [&] {
            InfoSelfState self;
            self.channel_mhz = cur_chan;
            self.psk_announced = psk_announced;
            self.claimed_by = csa.latched_issuer();
            return build_info_json(l, session, "tx", &self,
                                   air.value ? &*air.value : nullptr);
        };
        h.health_json = [&] { return build_health_json(last_snap); };
        h.discovery_json = [&] {
            return discovery.json(now_ms(), {});
        };
        h.profile = [&](int mn, int mx) -> std::string {
            if (mn < 0 || mn > 255 || mx < 0 || mx > 255)
                return "min/max must be 0..255";
            if (mx != 255 && mn > mx) return "min > max";
            tx.set_profile_pin(static_cast<uint8_t>(mn),
                               static_cast<uint8_t>(mx));
            return "";
        };
        // §10.5 (Pass 114) TX-power override-latch: one endpoint, both RF
        // backends (monitor iw-fixed / radio SetTxPowerOffsetQdb).
        h.tx_power_set = [&](bool is_auto, int qdb) -> std::string {
            if (!tx.has_power_targets())
                return "no role:\"tx\" adapter on this node";
            if (is_auto) {
                tx.clear_power_override();
                std::fprintf(stderr, "power: §10.5 override cleared (auto)\n");
                return "";
            }
            if (qdb < -511 || qdb > 511)
                return "qdb out of range (-511..511)";
            tx.set_power_override(qdb);
            return "";
        };
        h.calibration_json = [&] {  // §10.6 Pass 120
            std::string st = "idle";
            uint8_t rung = 0;
            const char* reason = nullptr;
            const CalibArtifact* art = nullptr;
            if (tx.calibrator_) {
                switch (tx.calibrator_->state()) {
                    case CalibState::kIdle: st = "idle"; break;
                    case CalibState::kRunning: st = "running"; break;
                    case CalibState::kDone: st = "done"; break;
                    case CalibState::kFailed: st = "failed"; break;
                }
                rung = tx.calibrator_->rung();
                reason = tx.calibrator_->fail_reason();
                if (tx.calibrator_->state() == CalibState::kDone) {
                    art = &tx.calibrator_->artifact();
                }
            }
            return calib_store_json(st, rung, tx.calib_fingerprint_,
                                    tx.calib_stale_, reason, art);
        };
        h.tx_power_json = [&] {
            const auto ov = tx.power_override();
            std::string s = "{\"override_active\":";
            s += ov ? "true" : "false";
            if (ov) {
                s += ",\"qdb\":";
                s += std::to_string(*ov);
            }
            s += ",\"backend\":\"";
            s += l.cfg.air.kind == AirCfg::Kind::kMonitor ? "kernel-monitor"
                 : l.cfg.air.kind == AirCfg::Kind::kRadio ? "radio"
                                                          : "udp";
            s += "\"}";
            return s;
        };
        h.fec = [&](int sid, int ip, int pp, int mk, int mr) -> std::string {
            if (sid < 0 || sid > 255) return "bad stream_id";
            if (ip < 0 || ip > 4000 || pp < 0 || pp > 4000 || mk < 1 ||
                mr < 0 || mr > 255)
                return "bad fec rates (0..4000 permille, min_k>=1, min_r 0..255)";
            return tx.set_stream_fec(static_cast<uint8_t>(sid),
                                     static_cast<uint16_t>(ip),
                                     static_cast<uint16_t>(pp),
                                     static_cast<uint16_t>(mk),
                                     static_cast<uint16_t>(mr))
                       ? ""
                       : "no frame-shm stream with that id";
        };
        h.reset_stats = [&] {
            tx.reset_stats();
            arq_timing.reset();
            for (ShmIn& si : shm_ins) {
                if (si.ring) si.ring->reset_stats();
            }
        };
        // §15.5 Pass 115: §3.5 report-authority override. `clear` frees a
        // stuck latch (a bench ground that never falls silent never ages out),
        // so the next reporter takes it within relatch_ms. A configured
        // preferred_originator outranks both forms — refused, not silently
        // ignored, so the operator learns why nothing happened.
        h.reports_latch = [&](bool clear, int originator) -> std::string {
            if (!tx.report_authority_overridable()) {
                return "preferred_originator is configured; override refused";
            }
            if (clear) {
                tx.report_authority_clear();
                return "";
            }
            if (originator <= 0 || originator > 0xFFFF) {
                return "originator out of range";
            }
            tx.report_authority_set(static_cast<uint16_t>(originator),
                                    now_ms());
            return "";
        };
        // §15.5 Pass 103: the §16 mode applier POSTs this after restarting venc
        // so the link re-asserts bitrate/caps/fps onto the fresh encoder (a
        // restart discards the encoder's live state; write-on-change would
        // otherwise never re-push). Side-effect only.
        h.venc_reassert = [&] {
            tx.reassert_venc();
            std::fprintf(stderr, "venc: reassert — actuator cache dropped, "
                                 "re-asserting on next tick\n");
        };
        // §15.5 craft-local FPS-ladder toggle (Pass 99). Routes through the
        // exact §11.7 FPS_LADDER transition the over-air path uses, so local
        // and remote toggles are identical. false → static (loop off), true →
        // variable (loop on). REJECTED (→ 400) on a craft with no venc actuator.
        h.link_fps = [&](bool ladder_on) -> std::string {
            return tx.apply_command(vcmd_id::kFpsLadder, ladder_on ? 1 : 0,
                                    now_ms())
                       ? ""
                       : "fps ladder unavailable (no venc actuator on this node)";
        };
        // §15.5 operating-mode selection (Pass 96). The link is the control
        // authority: the hub POSTs a mode name here, the link records it and
        // forks the on-craft applier, which persists the venc + link config
        // (docs/venc-mode-matrix.md §16) and restarts venc. The range pin is
        // applied live by the applier through /api/v1/link/profile, so the
        // link and CSA never restart. active_mode is the label, restored from
        // config at boot and set optimistically on accept.
        h.mode_get = [&]() -> std::string {
            std::string s = "{\"active\":\"" + active_mode + "\"";
            s += ",\"apply_configured\":";
            s += l.cfg.venc.mode_apply_cmd.empty() ? "false" : "true";
            s += "}";
            return s;
        };
        h.mode_set = [&](const std::string& name) -> std::string {
            if (l.cfg.venc.mode_apply_cmd.empty()) {
                return "no venc.mode_apply_cmd configured on this node";
            }
            for (char c : name) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) ||
                      c == '-' || c == '_' || c == '.')) {
                    return "mode name: only [A-Za-z0-9._-] allowed";
                }
            }
            if (!spawn_mode_applier(l.cfg.venc.mode_apply_cmd, name)) {
                return "failed to launch mode applier";
            }
            active_mode = name;  // optimistic; the applier is authoritative
            std::fprintf(stderr, "mode: applying \"%s\" via %s\n", name.c_str(),
                         l.cfg.venc.mode_apply_cmd.c_str());
            return "";
        };
        // §15.5 Pass 104: GET /api/v1/modes — the catalog. modes_dir defaults to
        // the directory holding mode_apply_cmd (§16 co-locates them).
        h.modes_list = [&, modes_dir = mode_catalog_dir(l.cfg.venc)]() {
            return modes_catalog_json(modes_dir, active_mode,
                                      !l.cfg.venc.mode_apply_cmd.empty());
        };
        // §15.5 Pass 113: craft-local channel set within the CSA allowlist.
        // Same commit sequence as a CSA switch (retune → selector → §11.6
        // liveness guard), then clears any in-flight campaign and drops the
        // §11.5a binding — the ground must re-scout. Volatile.
        h.channel_set = [&](int mhz) -> std::string {
            if (mhz <= 0 ||
                !channel_allowed(l.cfg.policy.csa.channel_allowlist,
                                 static_cast<uint16_t>(mhz))) {
                return "mhz not in channel_allowlist";
            }
            const uint16_t chan = static_cast<uint16_t>(mhz);
            if (!air.value->retune_all(chan, cur_bw, false)) {
                return "retune failed";
            }
            tx.reassert_power();  // §10.5: retune may reset TXAGC/nl80211
            const uint64_t now = now_ms();
            tx.on_rf_environment(chan, cur_bw, now);
            if (l.cfg.policy.csa.rx_liveness_ms > 0) {
                csa_liveness_deadline_ms = now + l.cfg.policy.csa.rx_liveness_ms;
                csa_liveness_rx_baseline = air.value->rx_frames_total();
                csa_liveness_chan = chan;
                csa_liveness_bw = cur_bw;
            }
            csa.clear_campaign();
            csa.release_binding();
            // §3.5 Pass 115: authority is released as one thing too. Dropping
            // the binding while the report latch stays pinned to the departed
            // issuer is precisely the split state the transfer exists to
            // prevent — and it would NOT self-heal if that issuer keeps
            // reporting, since its own traffic refreshes the silence timer.
            tx.report_authority_clear();
            cur_chan = chan;
            std::fprintf(stderr, "channel: local retune -> %u MHz (Pass 113)\n",
                         chan);
            return "";
        };
        // §11.4a Pass 113 runtime pairing gate. false = fresh token + new
        // pairing epoch (announce); true = keep the key, stop announcing.
        h.psk_enable = [&](bool enabled) -> std::string {
            if (!enabled) {
                token = announce_token();
                csa.set_psk({token.begin(), token.end()});
                craft_cmd.set_psk({token.begin(), token.end()});
                psk_announced = true;
                // §3.5 Pass 115: set_psk drops the §11.5a binding, so release
                // report authority with it — a new pairing epoch must not
                // leave the previous issuer still driving the selector.
                tx.report_authority_clear();
            } else {
                psk_announced = false;
            }
            std::fprintf(stderr, "psk: pairing %s (Pass 113)\n",
                         enabled ? "locked" : "open");
            return "";
        };
        control->set_handlers(std::move(h));
        std::fprintf(stderr, "control: REST on %s (tx)\n",
                     l.cfg.control.bind.c_str());
    }
    std::fprintf(stderr, "tx: session=%u, running%s\n", session,
                 qg.enabled() ? " (quiet-gap pacing)" : "");
    const auto service_air = [&](uint64_t service_now) {
        const uint64_t service_us = now_us();
        air.value->poll_once(0, [&](const AirRxMeta& meta, const uint8_t* d,
                                    size_t n) {
            const Decoded dec = decode(d, n);
            discovery.observe(dec, service_now, meta.net_id);
            if (const CsaPacket* c = std::get_if<CsaPacket>(&dec)) {
                if (!air.value->supports_csa()) return;
                if (csa.on_csa(*c, service_us,
                               air.value->read_tsf(meta.adapter_id),
                               static_cast<uint32_t>(meta.tsf_us),
                               std::nullopt)) {
                    tx.csa_freeze(service_now + static_cast<uint64_t>(
                                                   l.cfg.policy.csa.settle_s *
                                                   1000));
                    // §3.5 Pass 115: the claim takes report authority too, so
                    // command and reports can never name different grounds.
                    // Driven by the acceptance EVENT — the §11.5a binding is
                    // not consulted here and never forms on a passive ground.
                    tx.report_authority_set(c->prefix.originator, service_now);
                    // Pass 89: the flag is owned by the campaign_active()
                    // edge check on the loop body — one writer, so the
                    // clearing edge cannot be missed.
                    std::fprintf(stderr,
                                 "csa: armed -> %u MHz (nonce %u, dt %u ms)\n",
                                 c->target_chan, c->csa_nonce,
                                 c->dt_to_switch_ms);
                }
                return;
            }
            if (!std::holds_alternative<DecodeError>(dec)) {
                // §11.5a: pass the sender (common-prefix originator @3) so the
                // follower can refresh the binding on the bound issuer's traffic.
                csa.note_valid_rx(service_us, be16_read(d + 3));
            }
            if (const VehicleCmd* vc = std::get_if<VehicleCmd>(&dec)) {
                // §11.7: bound-issuer-only; the engine handles MAC / nonce /
                // duplicate re-echo / REJECTED, tx.apply_command actuates.
                if (craft_cmd.on_cmd(*vc, service_us, csa.latched_issuer(),
                                     apply_cmd)) {
                    std::fprintf(stderr,
                                 "vcmd: applied %s=%u (nonce %u, from %u)\n",
                                 vcmd_name_for(vc->cmd_id), vc->cmd_arg,
                                 vc->cmd_nonce, vc->prefix.originator);
                }
                return;
            }
            if (tx.on_air(d, n, service_now)) {
                arq_timing.note_nack_received(d, n, service_us);
                // A valid NACK bypasses the normal tick and live-video path.
                tx.drain_resends(service_now, inject_resend);
            }
        });
    };
    while (g_stop == 0) {
        // One timestamp per iteration: every callback and the tick share it,
        // so the time injected into the core never steps backward between
        // calls (a fresh now_ms() inside a callback can land 1 ms AFTER the
        // tick's captured now, and u64 "now - stamp" arithmetic underflows).
        const uint64_t now = now_ms();
        now_us_it = now_us();
        // Return-radio readiness is always serviced before video ingress or a
        // held-live flush. This is the vehicle's ARQ priority boundary.
        service_air(now);
        if (now >= next_shm_identity_check_ms) {
            for (ShmIn& si : shm_ins) {
                if (si.ring && !si.ring->backing_object_current()) {
                    std::fprintf(stderr,
                                 "tx: frame-shm '%s' producer replaced; "
                                 "reattaching\n",
                                 si.name.c_str());
                    si.ring.reset();
                    si.pending = false;
                }
            }
            next_shm_identity_check_ms = now + 250;
        }
        // Flush held video the moment the gap allows it; an EOB inside the
        // flush re-arms the gap and holds the rest.
        while (!held.empty() &&
               qg.can_send_video(now_us_it,
                                 static_cast<uint32_t>(held.size()))) {
            const std::vector<uint8_t> f = std::move(held.front());
            held.pop_front();
            send_raw(f.data(), f.size());
        }
        // Lazy (re)attach of any frame-shm producer that wasn't up at startup.
        bool any_pending = false;
        for (const ShmIn& si : shm_ins) {
            if (!si.ring) {
                any_pending = true;
            }
        }
        if (any_pending && now >= next_shm_attach_ms) {
            for (ShmIn& si : shm_ins) {
                if (si.ring) {
                    continue;
                }
                if (auto r = FrameShmRing::attach(si.name)) {
                    si.ring = std::move(*r.value);
                    si.pending = false;
                    std::fprintf(stderr, "tx: frame-shm '%s' attached\n",
                                 si.name.c_str());
                }
            }
            next_shm_attach_ms = now + 500;
        }
        // Air readiness comes first in the unified wait. SHM readiness only
        // marks a ring pending; a small bounded burst per ring is consumed
        // below without allowing a producer to monopolise the vehicle loop.
        std::vector<int> ready_fds = air.value->wait_fds();
        const size_t air_fd_count = ready_fds.size();
        for (const ShmIn& si : shm_ins) {
            if (si.ring) {
                ready_fds.push_back(si.ring->event_fd());
            }
        }
        const auto on_ready = [&](size_t j) {
            if (j < air_fd_count) {
                service_air(now_ms());
                return;
            }
            const size_t want = j - air_fd_count;
            size_t k = 0;
            for (ShmIn& si : shm_ins) {
                if (!si.ring) continue;
                if (k++ != want) continue;
                si.ring->drain_event();
                si.pending = true;
                return;
            }
        };
        // Held frames need µs-scale reactivity — don't sleep in poll then.
        bool shm_pending = false;
        for (const ShmIn& si : shm_ins) shm_pending |= si.pending;
        const int in_timeout =
            held.empty() && !air.value->tx_pending() && !shm_pending ? 2 : 0;
        bindings.value->poll_once(
            in_timeout,
            [&](const IngressEvent& ev) {
                tx.on_ingress(ev.stream_id, ev.data, ev.len, now, inject);
            },
            ready_fds, on_ready);
        // Nothing to wait on yet (no UDP ingress and every frame-shm producer
        // still down): poll_once returned instantly — pace to avoid a busy spin
        // while waiting for the producer to come up.
        if (!have_udp_ins && ready_fds.empty() && in_timeout > 0) {
            ::poll(nullptr, 0, in_timeout);
        }
        const uint64_t service_now = now_ms();
        service_air(service_now);
        for (ShmIn& si : shm_ins) {
            if (!si.ring || !si.pending) continue;
            // B5: match the buffer to the producer's declared slot size (grows
            // at most once per larger geometry, including a producer swap). An
            // undersized buffer makes read_frame reject-without-advancing and
            // stalls video ingress for the rest of the flight.
            if (frame_buf.size() < si.ring->slot_data_size()) {
                frame_buf.resize(si.ring->slot_data_size());
            }
            size_t drained = 0;
            while (drained < kFrameShmIngressDrainBudget) {
                const long got =
                    si.ring->read_frame(frame_buf.data(), frame_buf.size());
                if (got <= 0) {
                    si.pending = false;
                    break;
                }
                tx.on_frame(si.stream_id, frame_buf.data(),
                            static_cast<size_t>(got), service_now, inject);
                ++drained;
            }
            // If the budget was exhausted, leave pending set so the next main
            // loop iteration continues draining without waiting for an edge.
        }
        now_us_it = now_us();
        // §3.2 bit 4 / §11.6 (Pass 89): CSA_ARMED tracks the whole campaign —
        // set on accept, cleared on COMMITTED (or on the §11.5 revert back to
        // IDLE). Driven from follower state rather than pinned at the accept
        // and switch sites, so the bit is cleared by whichever edge resolves
        // the campaign. Edge-triggered: set_csa_armed walks every stream's
        // framer, so it must not run per iteration.
        if (const bool want_armed = csa.campaign_active();
            want_armed != csa_armed_flag) {
            csa_armed_flag = want_armed;
            tx.set_csa_armed(want_armed);
        }
        const CsaAction ca = csa.tick(now_us_it);
        if (ca.kind != CsaAction::Kind::kNone) {
            const bool retuned =
                air.value->retune_all(ca.chan_mhz, ca.bw, ca.fast);
            if (!retuned) {
                std::fprintf(stderr, "csa: retune to %u MHz FAILED\n",
                             ca.chan_mhz);  // Pass 69: never silent
            } else {
                tx.reassert_power();  // §10.5: retune may reset power
                tx.on_rf_environment(ca.chan_mhz, ca.bw, service_now);
                cur_chan = ca.chan_mhz;
                cur_bw = ca.bw;
            }
            std::fprintf(stderr, "csa: %s -> %u MHz\n", csa.state_str(),
                         ca.chan_mhz);
            // §11.6 Pass 80: arm the post-retune RX-liveness guard. The
            // issuer's beacons blanket the verify window, so total silence
            // for the deadline means the in-place retune half-applied.
            if (l.cfg.policy.csa.rx_liveness_ms > 0) {
                csa_liveness_deadline_ms =
                    now + l.cfg.policy.csa.rx_liveness_ms;
                csa_liveness_rx_baseline = air.value->rx_frames_total();
                csa_liveness_chan = ca.chan_mhz;
                csa_liveness_bw = ca.bw;
            }
        }
        if (csa_liveness_deadline_ms && now >= *csa_liveness_deadline_ms) {
            if (air.value->rx_frames_total() == csa_liveness_rx_baseline) {
                std::fprintf(stderr,
                             "csa: RX SILENT %u ms after retune to %u MHz — "
                             "half-applied retune, monitor re-init (§11.6 "
                             "Pass 80)\n",
                             l.cfg.policy.csa.rx_liveness_ms,
                             csa_liveness_chan);
                air.value->recover_all(csa_liveness_chan, csa_liveness_bw);
                // §10.5: recovery ends in `txpower auto` (Pass 48) — put the
                // latch / resolved curve value back.
                tx.reassert_power();
            }
            csa_liveness_deadline_ms.reset();  // one-shot per retune
        }
        // §11.7 echo burst — the craft's diversity-carried command ACK.
        while (const auto echo = craft_cmd.tick(now_us_it)) {
            uint8_t ef[kVehicleCmdSize];
            if (encode_vehicle_cmd(*echo, ef, sizeof(ef)) == sizeof(ef)) {
                air.value->inject(ef, sizeof(ef));
            }
        }
        tx.tick(service_now, inject, inject_resend);
        air.value->heartbeat(l.cfg.node.originator, session, service_now);
        {
            const std::optional<uint16_t> latched = csa.latched_issuer();
            air.value->announce(l.cfg.node.originator, session, service_now,
                                token.data(), psk_announced,
                                latched.has_value(), latched.value_or(0));
        }
        if (const auto trc = air.value->tx_progress_counters()) {
            if (wedge.poll(now, trc->first, trc->second)) {
                std::fprintf(stderr, "%s", wedge.wedged()
                        ? "air: TX WEDGE — submissions advancing, zero "
                          "backend TX progress over the window (§9.10)\n"
                        : "air: tx wedge cleared — backend TX progress "
                          "resumed\n");
            }
        }
        if (control) {
            control->service(now);
        }
        if (stats_period != 0 && now >= next_stats) {
            std::vector<std::pair<uint8_t, FrameShmRing::Stats>> shm_stats;
            for (const ShmIn& si : shm_ins) {
                if (si.ring) {
                    shm_stats.emplace_back(si.stream_id, si.ring->stats());
                }
            }
            const ArqTimingStats timing = arq_timing.snapshot();
            const VcmdStatsFill vfill{craft_cmd.last_nonce(), nullptr, 0,
                                      true};
            emit_stats(emitter, l, session, t0, &tx, nullptr, &*air.value, 0,
                       csa.state_str(), 0, 0, wedge.wedged(), nullptr,
                       &shm_stats, nullptr, nullptr, &last_snap, &timing,
                       &vfill, cur_chan);
            if (control) {
                control->publish_stats(emitter.last_line());
            }
            next_stats = now + stats_period;
        }
    }
    return 0;
}

int run_rx(const Loaded& l) {
    auto air = AirBackend::create(l.cfg);
    if (!air) {
        std::fprintf(stderr, "air error: %s\n", air.error.c_str());
        return 1;
    }
    PacketEventTrace packet_trace("rx");
    air.value->set_packet_trace(&packet_trace);
    auto bindings = BindingSet::create(l.cfg);
    if (!bindings) {
        std::fprintf(stderr, "binding error: %s\n", bindings.error.c_str());
        return 1;
    }
    const uint32_t session = session_nonce();
    DiscoveryCatalog discovery;
    RxCore rx(l.cfg, session, l.have_table ? &l.table : nullptr,
              l.have_table ? std::optional<uint8_t>(l.tv) : std::nullopt);
    const uint16_t op_chan =
        l.cfg.adapters.empty() ? 0 : l.cfg.adapters[0].channel_mhz;
    const uint8_t op_bw_mhz =
        l.cfg.adapters.empty() ? 20 : l.cfg.adapters[0].bw;

    struct LinkSelection {
        uint16_t originator = 0;
        uint16_t chan = 0;
        uint8_t bw = 0;  // §11 width code
        uint8_t net_id = 0;
    };
    LinkSelection active_selection{rx.selected_originator().value_or(0),
                                   op_chan, bw_code(op_bw_mhz),
                                   l.cfg.node.net_id.value_or(0)};
    std::optional<LinkSelection> pending_selection;
    std::optional<LinkSelection> previous_selection;
    std::string selection_state = "configured";
    std::string previous_selection_state = selection_state;

    // §15.4 frame-shm egress: one producer ring + a §6.3a reassembler per
    // frame-shm out-stream. deliver_now carries the loop's per-iteration clock
    // into the deliver lambda (the reassembler needs now_ms for its deadlines).
    struct ShmOut {
        uint8_t stream_id;
        std::unique_ptr<FrameShmRing> ring;
        std::unique_ptr<FrameReassembler> reasm;
        std::optional<StreamKey> source;
    };
    std::vector<ShmOut> shm_outs;
    for (const StreamCfg& s : l.cfg.streams) {
        if (s.dir != Dir::kOut || s.bind.kind != BindKind::kFrameShm) {
            continue;
        }
        auto r = FrameShmRing::create(s.bind.name);
        if (!r) {
            std::fprintf(stderr, "frame-shm egress '%s': %s\n",
                         s.bind.name.c_str(), r.error.c_str());
            return 1;
        }
        FrameReassemblerConfig frc;
        // Map the drop deadline from the floor rung's I-frame budget when a
        // table is loaded; otherwise keep the §6.3a default.
        if (l.have_table) {
            for (const Profile& p : l.table.profiles) {
                if (p.id == l.table.floor_profile &&
                    p.arq_deadline_iframe_ms > 0) {
                    frc.deadline_ms = p.arq_deadline_iframe_ms;
                }
            }
        }
        std::fprintf(stderr, "rx: frame-shm egress '%s' created\n",
                     s.bind.name.c_str());
        shm_outs.push_back(ShmOut{s.stream_id, std::move(*r.value),
                                  std::make_unique<FrameReassembler>(frc),
                                  std::nullopt});
    }
    uint64_t deliver_now = now_ms();

    // §14.3 cache roles (v1 IP transport; both optional, either or both).
    std::unique_ptr<CacheController> cache_ctl;
    std::unique_ptr<CacheUdp> cache_repair_sock;
    std::map<uint16_t, CacheEndpoint> cache_endpoints;  // originator -> ep
    FrameReassembler* cache_reasm = nullptr;
    FrameShmRing* cache_ring = nullptr;
    if (l.cfg.cache.repair.enabled) {
        const CacheRepairCfg& cr = l.cfg.cache.repair;
        for (const CacheEndpointCfg& e : cr.caches) {
            auto ep = CacheUdp::resolve(e.endpoint);
            if (!ep) {
                std::fprintf(stderr, "cache.repair '%s': %s\n",
                             e.endpoint.c_str(), ep.error.c_str());
                return 1;
            }
            cache_endpoints.emplace(e.originator, *ep.value);
        }
        auto sock = CacheUdp::open(cr.listen);
        if (!sock) {
            std::fprintf(stderr, "cache.repair listen: %s\n",
                         sock.error.c_str());
            return 1;
        }
        cache_repair_sock =
            std::make_unique<CacheUdp>(std::move(*sock.value));
        CacheControllerConfig cc;
        cc.self_originator = l.cfg.node.originator;
        cc.self_session = session;
        for (const CacheEndpointCfg& e : cr.caches) {
            cc.caches.push_back(e.originator);
        }
        cc.tail_grace_ms = cr.tail_grace_ms;
        cc.local_quiet_ms = cr.local_quiet_ms;
        cc.min_collect_ms = cr.min_collect_ms;
        cc.hard_close_ms = cr.hard_close_ms;
        cc.request_timeout_ms = cr.request_timeout_ms;
        cc.repair_fraction_permille = cr.repair_fraction_permille;
        cc.absolute_symbol_limit = cr.absolute_symbol_limit;
        cc.max_cache_attempts = cr.max_cache_attempts;
        cc.reply_limit = cr.reply_limit;
        cc.health_floor_permille = cr.health_floor_permille;
        cc.status_timeout_ms = cr.status_timeout_ms;
        cache_ctl = std::make_unique<CacheController>(cc);
        for (ShmOut& so : shm_outs) {  // config validated the stream exists
            if (so.stream_id == cr.stream_id) {
                cache_reasm = so.reasm.get();
                cache_ring = so.ring.get();
            }
        }
        std::fprintf(stderr, "rx: cache repair on stream %u (%zu caches)\n",
                     cr.stream_id, cr.caches.size());
    }
    std::unique_ptr<CacheStore> cache_store;
    std::unique_ptr<CacheUdp> cache_store_sock;
    std::vector<CacheEndpoint> cache_status_to;
    std::optional<CacheEndpoint> cache_controller_endpoint;
    std::unique_ptr<CacheAssignmentGate> cache_assignment_gate;
    uint64_t next_cache_status_ms = 0;
    if (l.cfg.cache.store.enabled) {
        const CacheStoreCfg& cs = l.cfg.cache.store;
        auto sock = CacheUdp::open(cs.listen);
        if (!sock) {
            std::fprintf(stderr, "cache.store listen: %s\n",
                         sock.error.c_str());
            return 1;
        }
        cache_store_sock = std::make_unique<CacheUdp>(std::move(*sock.value));
        for (const std::string& t : cs.status_to) {
            auto ep = CacheUdp::resolve(t);
            if (!ep) {
                std::fprintf(stderr, "cache.store status_to '%s': %s\n",
                             t.c_str(), ep.error.c_str());
                return 1;
            }
            cache_status_to.push_back(*ep.value);
        }
        CacheStoreConfig sc;
        sc.self_originator = l.cfg.node.originator;
        sc.target_originator = l.cfg.node.preferred_originator;
        sc.stream_ids = cs.stream_ids;
        sc.blocks = cs.blocks;
        sc.reply_limit = cs.reply_limit;
        sc.max_requests_per_s = cs.max_requests_per_s;
        cache_store = std::make_unique<CacheStore>(sc);
        if (cs.controller) {
            auto ep = CacheUdp::resolve(cs.controller->endpoint);
            if (!ep) {
                std::fprintf(stderr, "cache.store controller '%s': %s\n",
                             cs.controller->endpoint.c_str(), ep.error.c_str());
                return 1;
            }
            cache_controller_endpoint = *ep.value;
            cache_assignment_gate = std::make_unique<CacheAssignmentGate>(
                l.cfg.node.originator, cs.controller->originator);
        }
        std::fprintf(stderr, "rx: cache store on '%s' (%zu streams)\n",
                     cs.listen.c_str(), cs.stream_ids.size());
    }

    uint32_t cache_assignment_epoch = 0;
    uint64_t next_cache_assignment_ms = 0;
    std::optional<CacheAssign> desired_cache_assignment;
    const auto assign_caches = [&](const LinkSelection& selected) {
        if (!cache_ctl || selected.originator == 0) return;
        cache_ctl->reset_link();
        desired_cache_assignment.emplace();
        CacheAssign& a = *desired_cache_assignment;
        a.prefix = {l.cfg.node.originator, 0, session};
        a.target_originator = selected.originator;
        a.assignment_epoch = ++cache_assignment_epoch;
        a.target_chan = selected.chan;
        a.target_bw = selected.bw;
        a.target_net_id = selected.net_id;
        next_cache_assignment_ms = 0;
    };
    assign_caches(active_selection);  // startup/restart healing (§14.3 Pass 67)

    // §15.4 egress write + the §3.9 Pass 106 early exit: an IRAP reaching the
    // ring means the consumer has a start point, so the latch-recovery schedule
    // for that stream can stand down.
    const auto write_egress = [&](FrameShmRing* ring, uint8_t stream_id,
                                  const uint8_t* f, size_t len) {
        ring->write_frame(f, len);
        VencFrameMeta meta;
        if (read_frame_meta(f, len, &meta) &&
            (meta.flags & kFrameFlagIdr) != 0) {
            rx.note_egress_irap(stream_id);
        }
    };
    const RxEngine::Deliver deliver = [&](uint8_t sid, uint32_t block_id,
                                          uint8_t flags, const uint8_t* d,
                                          size_t n) {
        for (ShmOut& so : shm_outs) {
            if (so.stream_id == sid) {
                so.reasm->push(block_id, flags, d, n, deliver_now,
                               [&](const uint8_t* f, size_t len) {
                                   write_egress(so.ring.get(), so.stream_id, f, len);
                               });
                return;
            }
        }
        if (UdpEgress* out = bindings.value->egress_for(sid)) {
            out->send(d, n);
        }
    };
    const RxEngine::EarlyDeliver early_deliver =
        [&](const StreamKey& source, uint8_t sid, uint32_t block_id,
            uint8_t flags, const uint8_t* d,
            size_t n) -> RxEngine::EarlyDeliverResult {
        for (ShmOut& so : shm_outs) {
            if (so.stream_id != sid) {
                continue;
            }
            if (!so.source || !(*so.source == source)) {
                so.reasm->reset_stream();
                so.source = source;
            }
            const bool complete = so.reasm->push(
                block_id, flags, d, n, deliver_now,
                [&](const uint8_t* f, size_t len) {
                    write_egress(so.ring.get(), so.stream_id, f, len);
                });
            return RxEngine::EarlyDeliverResult{/*handled=*/true, complete};
        }
        return {};
    };
    const auto apply_selection = [&](const LinkSelection& selected) {
        rx.select_originator(selected.originator);
        for (ShmOut& so : shm_outs) {
            so.reasm->reset_stream();
            so.source.reset();
        }
        active_selection = selected;
        assign_caches(selected);
    };

    // §7.2 ground side: returns (NACK/LINK_REPORT) coalesce and fire at the
    // middle of the craft's quiet gap, anchored on the EOB's receive-TSF.
    // Disabled (default) they inject immediately — §7.1 baseline.
    QuietGap qg(quietgap_policy(l.cfg));
    std::deque<std::pair<std::vector<uint8_t>, uint16_t>> urgent_ret_held;
    std::deque<std::pair<std::vector<uint8_t>, uint16_t>> report_ret_held;
    // §7.2 Pass 78: anchored report batches re-fire once at the NEXT return
    // window (spread across two listen gaps; a blind fallback batch is not
    // repeated). Byte-identical copies — the TX epoch filter dedups.
    std::deque<std::pair<std::vector<uint8_t>, uint16_t>> report_repeat_held;
    std::optional<uint64_t> ret_at_us;
    // §11.2 (Pass 90): the campaign copy awaiting the craft's quiet gap, held
    // decoded so it can be re-stamped at the instant it goes on air. At most
    // one is outstanding — copies are paced at kCopySpacingUs and the gap
    // recurs far faster, so a newer copy simply supersedes an unsent one.
    std::optional<CsaPacket> csa_copy_held;
    std::optional<uint64_t> csa_copy_fallback_us;
    bool ret_tsf_anchored = false;
    std::optional<uint64_t> report_fallback_us;
    // If the repair-tail EOB itself is lost, silence after the last received
    // DATA symbol is the only close signal available. Keep a rolling host-
    // time fallback at the return-window midpoint so ARQ cannot remain
    // suppressed forever waiting for an EOB that will never arrive.
    std::optional<uint64_t> repair_tail_fallback_us;
    uint32_t ret_window_hits = 0;
    uint32_t ret_window_misses = 0;
    uint64_t tsf_fallbacks = 0;
    uint64_t now_us_it = now_us();
    ArqTimingTracker arq_timing;
    const auto send_return = [&](uint16_t target, const uint8_t* f, size_t n,
                                 bool urgent) {
        if (urgent) arq_timing.note_nack_injected(f, n, now_us());
        air.value->inject_return(target, f, n, urgent);
    };
    const RxCore::Inject inject_report = [&](const uint8_t* f, size_t n,
                                             uint16_t target) {
        if (!qg.enabled()) {
            send_return(target, f, n, false);
            return;
        }
        report_ret_held.emplace_back(std::vector<uint8_t>(f, f + n), target);
        if (!report_fallback_us) {
            // Prefer the next EOB midpoint. If video/EOB disappears entirely,
            // degrade to opportunistic return after one report period.
            report_fallback_us = now_us() + 100000;
        }
    };
    const RxCore::Inject inject_nack = [&](const uint8_t* f, size_t n,
                                           uint16_t target) {
        arq_timing.note_nack_built(f, n, now_us());
        if (!qg.enabled() || !ret_tsf_anchored) {
            // Monitor mode cannot read live TSF. By the time the repair-tail
            // close is observed, host arrival already includes USB delay;
            // adding the quiet-gap midpoint again only makes ARQ later.
            send_return(target, f, n, true);
            return;
        }
        urgent_ret_held.emplace_back(std::vector<uint8_t>(f, f + n), target);
    };
    // Service cache replies and issue fresh-cache requests before RxCore
    // builds NACKs in this iteration. This ordering lets an accepted reply
    // complete the block first, while a successfully sent request can arm the
    // exact block's bounded first-NACK grace (§14.3 rule 8).
    const auto service_cache_repair = [&](uint64_t service_ms) {
        if (!cache_ctl) return;
        if (desired_cache_assignment &&
            service_ms >= next_cache_assignment_ms) {
            next_cache_assignment_ms =
                service_ms + l.cfg.cache.repair.assignment_interval_ms;
            for (const auto& [cache_originator, endpoint] : cache_endpoints) {
                if (cache_ctl->has_fresh_target(
                        cache_originator,
                        desired_cache_assignment->target_originator,
                        service_ms)) {
                    continue;
                }
                CacheAssign a = *desired_cache_assignment;
                a.prefix.destination = cache_originator;
                a.target_cache = cache_originator;
                uint8_t abuf[kCacheAssignSize];
                if (encode_cache_assign(a, abuf, sizeof(abuf)) ==
                    sizeof(abuf)) {
                    cache_repair_sock->send_to(endpoint, abuf, sizeof(abuf));
                }
            }
        }
        uint8_t cbuf[kCacheReplyFixedSize + kDataHeaderSize +
                     kMaxDataPayload];
        CacheEndpoint from;
        long rn;
        // B6: bounded drain — an unbounded cache-reply socket lets any reachable
        // host hold the flight loop here; ready data re-fires the next pass.
        for (int cdrained = 0;
             cdrained < 64 &&
             (rn = cache_repair_sock->recv_one(cbuf, sizeof(cbuf), &from)) > 0;
             ++cdrained) {
            const Decoded cdec = decode(cbuf, static_cast<size_t>(rn));
            if (const CacheStatus* st = std::get_if<CacheStatus>(&cdec)) {
                const auto it = cache_endpoints.find(st->prefix.originator);
                if (it != cache_endpoints.end() && it->second == from) {
                    cache_ctl->on_status(*st, service_ms);
                }
                continue;
            }
            const CacheReplyView* rv = std::get_if<CacheReplyView>(&cdec);
            if (rv == nullptr) continue;
            const auto it = cache_endpoints.find(rv->prefix.originator);
            if (it == cache_endpoints.end() || !(it->second == from)) {
                continue;
            }
            const Decoded wdec = decode(rv->wrapped, rv->wrapped_len);
            const DataView* wv = std::get_if<DataView>(&wdec);
            if (wv == nullptr ||
                wv->hdr.stream_id != l.cfg.cache.repair.stream_id) {
                continue;
            }
            bool latched = false;
            for (const StreamKey& k : rx.stream_keys()) {
                latched |= k.stream_id == wv->hdr.stream_id &&
                           k.originator == wv->hdr.prefix.originator &&
                           k.session_id == wv->hdr.prefix.session_id;
            }
            if (!latched) continue;
            const uint64_t reply_us = now_us();
            if (cache_ctl->on_reply(rv->prefix.originator, rv->request_id,
                                    *wv, reply_us) !=
                CacheController::ReplyVerdict::kAccept) {
                continue;
            }
            const bool emitted = cache_reasm->push(
                wv->hdr.block_id, wv->hdr.data_flags, wv->payload,
                wv->payload_len, service_ms,
                [&](const uint8_t* f, size_t len) {
                    // §14.3 repair lands in the same egress ring, so it can
                    // also satisfy the §3.9 Pass 106 early exit.
                    write_egress(cache_ring, l.cfg.cache.repair.stream_id, f,
                                 len);
                },
                /*air_path=*/false);
            if (emitted) {
                const bool before_nack = !rx.block_had_nack(
                    l.cfg.cache.repair.stream_id, wv->hdr.block_id);
                cache_ctl->note_completed(wv->hdr.block_id, reply_us,
                                          before_nack);
                rx.complete_frame(l.cfg.cache.repair.stream_id,
                                  wv->hdr.block_id, service_ms, deliver);
            }
        }
        for (const StreamKey& k : rx.stream_keys()) {
            if (k.stream_id != l.cfg.cache.repair.stream_id) continue;
            RepairCandidate cands[16];
            const size_t cn = cache_reasm->repair_candidates(cands, 16);
            for (CacheRequestOut& r :
                 cache_ctl->tick(service_ms, k, cands, cn)) {
                const auto it = cache_endpoints.find(r.cache_originator);
                if (it == cache_endpoints.end() ||
                    !cache_repair_sock->send_to(
                        it->second, r.frame.data(), r.frame.size())) {
                    continue;
                }
                cache_ctl->note_request_sent(r.request_id, now_us());
                const uint32_t grace_ms =
                    l.cfg.cache.repair.nack_grace_ms;
                if (grace_ms != 0 && rx.defer_first_nack(
                        l.cfg.cache.repair.stream_id, r.block_id,
                        service_ms + grace_ms)) {
                    cache_ctl->note_nack_grace_armed();
                }
            }
            break;
        }
    };
    StatsEmitter emitter(l.cfg.stats.to_stdout, bindings.value->stats_egress());
    const uint64_t t0 = now_ms();
    uint64_t next_stats = t0;
    const uint64_t stats_period =
        l.cfg.stats.hz > 0 ? static_cast<uint64_t>(1000.0 / l.cfg.stats.hz)
                           : 0;
    // §11 ground: the issuer runs campaigns (stdin trigger "csa <mhz>
    // [class]", PSK required); the follower makes a PSK-less RX node a
    // spectator that follows others' campaigns. The issuer's own copies never
    // reach the local follower (RadioAir drops own-originator frames).
    const CsaParams cparams = csa_params(l.cfg);
    CsaIssuer issuer(cparams);
    CsaFollower follower(cparams);
    // §11.7 command issuer. Keyed like the CSA issuer (configured secret now;
    // a claim re-keys both with the craft's announced token). The nonce
    // domain starts random per session (§3.14 cross-session echo replay).
    VcmdIssuer vissuer(vcmd_params(l.cfg));
    vissuer.seed_nonce(session_nonce());
    bool arq_rx_enabled = true;  // §6.4 emission gate (POST /api/v1/arq)
    // §9.10: the ground's designated uplink TX adapter gets the same
    // CCX-liveness watchdog as the craft's radio.
    TxWedge wedge(TxWedgePolicy{l.cfg.air.wedge_window_ms,
                                l.cfg.air.wedge_min_submits});
    // §15.5a (Pass 65): the ground's current operating channel — the config
    // default until a claim commits, then the committed target. The scout returns
    // all ears here, and a failed claim rolls back here.
    uint16_t operating_chan = op_chan;
    // §15.5a scout (Pass 64). Roams the uplink (role:"tx") adapter only — the
    // diversity RX adapters hold the resting channel — and widens the net_id
    // filter during a sweep; psk_known reports a usable CSA key (configured
    // secret, or a cached announced token, Pass 63).
    const size_t scout_idx = air.value->tx_index();
    ScoutEngine scout(
        ScoutEngine::Hooks{
            [&, scout_idx](uint16_t ch, uint8_t bw) {
                return air.value->retune_one(scout_idx, ch, bw, false);
            },
            [&](uint16_t ch, uint8_t bw) {
                air.value->retune_all(ch, bw, false);
            },
            [&](std::optional<uint8_t> nid) {
                air.value->set_filter_net_id(nid);
            },
            [&](uint16_t orig) {
                return !l.cfg.policy.csa.psk.empty() ||
                       discovery.token_for(orig).has_value();
            },
        },
        op_bw_mhz, op_chan, l.cfg.node.net_id, scout_idx);
    // §15.5 REST control plane. RX/ground node owns the CSA trigger (replaces
    // the removed stdin trigger); profile/fec are TX-only knobs → null → 409.
    StatsSnapshot last_snap;
    std::unique_ptr<ControlServer> control;
    if (!l.cfg.control.bind.empty()) {
        auto cs = ControlServer::create(l.cfg.control.bind);
        if (!cs) {
            std::fprintf(stderr, "control: %s\n", cs.error.c_str());
            return 1;
        }
        control = std::move(*cs.value);
        ControlHandlers h;
        h.stats_line = [&]() -> std::string {
            std::string s = emitter.last_line();
            if (!s.empty() && s.back() == '\n') s.pop_back();
            return s;
        };
        h.info_json = [&] {
            return build_info_json(l, session, "rx", nullptr,
                                   air.value ? &*air.value : nullptr);
        };
        h.health_json = [&] { return build_health_json(last_snap); };
        h.discovery_json = [&] {
            return discovery.json(now_ms(), rx.stream_keys());
        };
        h.scout_results = [&] { return scout.results_json(); };
        h.selection_json = [&] {
            const uint64_t at = now_ms();
            size_t following = 0;
            if (cache_ctl) {
                for (const auto& [orig, ep] : cache_endpoints) {
                    (void)ep;
                    following += cache_ctl->has_fresh_target(
                                     orig, active_selection.originator, at)
                                     ? 1u
                                     : 0u;
                }
            }
            std::string out = "{\"state\":\"" + selection_state +
                              "\",\"originator\":" +
                              std::to_string(active_selection.originator) +
                              ",\"channel\":" +
                              std::to_string(active_selection.chan) +
                              ",\"bw\":" +
                              std::to_string(active_selection.bw) +
                              ",\"net_id\":" +
                              std::to_string(active_selection.net_id) +
                              ",\"caches_configured\":" +
                              std::to_string(cache_endpoints.size()) +
                              ",\"caches_following\":" +
                              std::to_string(following);
            if (pending_selection) {
                out += ",\"pending_originator\":" +
                       std::to_string(pending_selection->originator) +
                       ",\"pending_channel\":" +
                       std::to_string(pending_selection->chan);
            }
            return out + "}";
        };
        if (cache_store) {
            h.cache_assignment_json = [&] {
                std::string out = "{\"controller\":";
                if (l.cfg.cache.store.controller) {
                    out += std::to_string(
                        l.cfg.cache.store.controller->originator);
                } else {
                    out += "null";
                }
                out += ",\"target_originator\":" +
                       std::to_string(cache_store->target_originator());
                if (cache_assignment_gate &&
                    cache_assignment_gate->current()) {
                    const CacheAssign& a =
                        *cache_assignment_gate->current();
                    out += ",\"controller_session\":" +
                           std::to_string(a.prefix.session_id) +
                           ",\"assignment_epoch\":" +
                           std::to_string(a.assignment_epoch) +
                           ",\"channel\":" +
                           std::to_string(a.target_chan) +
                           ",\"bw\":" + std::to_string(a.target_bw) +
                           ",\"net_id\":" +
                           std::to_string(a.target_net_id);
                }
                return out + "}";
            };
        }
        // §15.5a claim: CSA-grab a scouted craft. Re-keys the issuer with the
        // craft's key (configured secret, or the cached announced token §11.4a),
        // binds the link to its net_id, moves onto its channel to be heard, then
        // issues a §11 campaign to target_chan (0 → the emptiest allowlisted
        // channel). The loop's issuer.tick drives the copies/commit; post-claim
        // the net_id stamp/filter and channel hold until an explicit re-scout.
        auto do_claim = [&](int originator_i, int target_chan_i) -> std::string {
            if (originator_i <= 0 || originator_i > 0xFFFF) {
                return "invalid originator";
            }
            const uint16_t orig = static_cast<uint16_t>(originator_i);
            const auto cand = scout.candidate_for(orig);
            if (!cand) return "unknown craft (run a scout first)";
            // §15.5a / B9: a scout candidate that predates a craft reboot points
            // at the craft's OLD channel — the claim would retune there and then
            // abort (no CSA_ARMED) with no hint to the operator. If the craft has
            // announced a different session more recently than the scout saw it,
            // the candidate is stale; demand a re-scout instead of a silent abort.
            const auto live_sess = discovery.session_for(orig);
            if (live_sess && *live_sess != cand->session) {
                return "stale candidate (craft rebooted since scout) — re-scout";
            }
            // §2/§13 passive spectator (Pass 74): no uplink for a §11 issuer
            // campaign, so "select" is a passive tune. Retune all ears onto the
            // scouted feed's channel + net_id; §2 first-latch /
            // preferred_originator picks up the stream. csa_psk stays
            // craft+ground; a CSA move is recovered by re-scout, not followed.
            if (l.cfg.node.spectator) {
                air.value->set_stamp_net_id(cand->net_id);
                air.value->set_filter_net_id(cand->net_id);
                if (!air.value->retune_all(cand->chan, op_bw_mhz, false)) {
                    return "failed to tune onto feed channel";
                }
                active_selection =
                    LinkSelection{orig, cand->chan, 0, cand->net_id};
                selection_state = "tuned";
                std::fprintf(
                    stderr, "spectator: tuned originator=%u net_id=%u %u MHz\n",
                    orig, cand->net_id, cand->chan);
                return "";
            }
            if (!air.value->supports_csa()) {
                return "CSA unsupported by this backend";
            }
            // Configured secret wins; else the cached announced token (§11.4a).
            std::vector<uint8_t> key = cparams.psk;
            if (key.empty()) {
                const auto tok = discovery.token_for(orig);
                if (!tok) return "no CSA key for craft (no cached token)";
                key.assign(tok->begin(), tok->end());
            }
            if (!issuer.set_psk(key)) return "claim busy (campaign active)";
            if (!vissuer.set_psk(key)) {
                return "claim busy (command campaign active)";
            }
            uint16_t target = static_cast<uint16_t>(target_chan_i);
            if (target == 0) {
                target =
                    scout.emptiest(l.cfg.policy.csa.channel_allowlist, cand->chan);
            }
            if (target == 0) return "no target channel (specify target_chan)";
            // §15.5a: bind the link to the craft's net_id and move all ears onto
            // its current channel so the campaign and CSA_ARMED return are heard.
            air.value->set_stamp_net_id(cand->net_id);
            air.value->set_filter_net_id(cand->net_id);
            if (!air.value->retune_all(cand->chan, op_bw_mhz, false)) {
                air.value->set_stamp_net_id(active_selection.net_id);
                air.value->set_filter_net_id(active_selection.net_id);
                air.value->retune_all(active_selection.chan, op_bw_mhz, false);
                return "failed to retune onto craft channel";
            }
            // retune_class 1 (500 ms dt budget) gives the craft slack for the iw
            // shell-out retune before its §11.5 verify timeout. bw code 0 = 20 MHz
            // (v1 single-width, matching the /csa trigger).
            const CommonPrefix pre{l.cfg.node.originator, 0, session};
            if (!issuer.start(pre, target, 0, 1, cand->chan, 0, 4, now_us_it)) {
                air.value->retune_all(active_selection.chan, op_bw_mhz, false);
                air.value->set_stamp_net_id(active_selection.net_id);
                air.value->set_filter_net_id(active_selection.net_id);
                return "rejected (active campaign or rate-limit)";
            }
            previous_selection = active_selection;
            previous_selection_state = selection_state;
            pending_selection = LinkSelection{orig, target, 0, cand->net_id};
            selection_state = "claiming";
            std::fprintf(stderr, "csa: claim originator=%u net_id=%u %u->%u MHz\n",
                         orig, cand->net_id, cand->chan, target);
            return "";
        };
        // A controlled cache is a follower of its receiver.  It must not expose
        // an independent scout/claim/CSA control surface that could split it
        // from the receiver's committed selection.
        if (!cache_assignment_gate) {
            // Capture do_claim BY VALUE: it is a block-local lambda, but the
            // handlers outlive this block (moved into `control`, invoked from
            // the event loop below), so a by-reference capture would dangle —
            // a stack-use-after-scope on the first quickconnect. scout_quickconnect
            // already copies it (assignment below); scout_start must too.
            h.scout_start = [&, do_claim](const std::vector<uint16_t>& chans,
                                          uint32_t dwell, const std::string& mode,
                                          int target) -> std::string {
                if (mode == "quickconnect") {
                    if (target < 0) {
                        return "quickconnect requires target.originator";
                    }
                    return do_claim(target, 0);  // pick the emptiest channel
                }
                std::vector<uint16_t> ch = chans;
                if (ch.empty()) ch = l.cfg.scout.channels;
                if (ch.empty()) ch = l.cfg.policy.csa.channel_allowlist;
                scout.set_rest_chan(operating_chan);  // return here after dwell
                scout.set_rest_filter(active_selection.net_id);
                return scout.start(ch, dwell ? dwell : l.cfg.scout.dwell_ms,
                                   now_ms());
            };
            h.scout_stop = [&]() -> std::string {
                scout.stop(now_ms());
                return "";
            };
            h.scout_quickconnect = do_claim;
            // §11.7 command campaign toward the bound craft (§15.5).
            h.vehicle_command_json = [&] {
                return std::string("{\"nonce\":") +
                       std::to_string(vissuer.nonce()) + ",\"cmd\":\"" +
                       vcmd_name_for(vissuer.cmd_id()) +
                       "\",\"arg\":" + std::to_string(vissuer.cmd_arg()) +
                       ",\"state\":\"" + vissuer.state_str() + "\"}";
            };
            h.vehicle_command = [&](const std::string& cmd, int arg)
                -> std::pair<int, std::string> {
                const uint8_t id = vcmd_id_for(cmd);
                if (id == 0) {
                    return {400, "{\"ok\":false,\"error\":\"unknown cmd\"}"};
                }
                // §11.7/§3.14: MODE (Pass 105) rides the full u8 — the catalog
                // lives on the craft, so an over-range index is the craft's
                // REJECTED to give, not a local 400. Every other command is
                // capped at 0..4 (Pass 68).
                const int arg_max = (id == vcmd_id::kMode) ? 255 : kVcmdMaxArg;
                if (arg < 0 || arg > arg_max) {
                    return {400,
                            "{\"ok\":false,\"error\":\"arg out of range\"}"};
                }
                // §11.7/§15.5: refused up front, not timed out — a campaign
                // needs a bound craft. Pass 108 deliberately does NOT admit a
                // `latched` selection here, unlike /csa: §11.7 "no bootstrap"
                // makes the craft silently drop commands from an issuer it has
                // not accepted a CSA from, so sending on a latch alone returns
                // 200 and then always times out (bench-confirmed). Name the
                // remedy instead of pretending the command went anywhere.
                if (selection_state != "committed" ||
                    active_selection.originator == 0) {
                    return {409,
                            "{\"ok\":false,\"error\":\"craft not claimed — "
                            "§11.7 commands need a CSA claim first "
                            "(/api/v1/csa or scout/quickconnect)\"}"};
                }
                if (vissuer.active()) {
                    return {409,
                            "{\"ok\":false,\"error\":\"campaign pending\"}"};
                }
                // §15.5a / B9, Pass 108: re-key from the craft's LIVE announced
                // token before every campaign, exactly as /csa does. Previously
                // only do_claim ever keyed this issuer, so a craft reached by
                // latch alone had no key at all and every command died as a bare
                // "rate-limit or no key". The configured-secret path is stable;
                // skip it there.
                if (cparams.psk.empty()) {
                    const auto tok =
                        discovery.token_for(active_selection.originator);
                    if (!tok) {
                        return {400,
                                "{\"ok\":false,\"error\":\"no live command key "
                                "for craft (secret-mode, or not heard for "
                                ">5 s)\"}"};
                    }
                    const std::vector<uint8_t> key(tok->begin(), tok->end());
                    if (!vissuer.set_psk(key)) {
                        return {409,
                                "{\"ok\":false,\"error\":\"command issuer busy "
                                "(campaign active)\"}"};
                    }
                }
                if (!vissuer.start(
                        CommonPrefix{l.cfg.node.originator, 0, session},
                        active_selection.originator, id,
                        static_cast<uint8_t>(arg), now_us_it)) {
                    return {409,
                            "{\"ok\":false,\"error\":\"rejected (rate-limit "
                            "or no key)\"}"};
                }
                std::fprintf(stderr, "vcmd: campaign %s=%d nonce=%u -> %u\n",
                             cmd.c_str(), arg, vissuer.nonce(),
                             active_selection.originator);
                return {200, "{\"ok\":true,\"nonce\":" +
                                 std::to_string(vissuer.nonce()) + "}"};
            };
            h.csa = [&](uint32_t mhz,
                        uint32_t klass) -> std::pair<int, std::string> {
                const auto err = [](int code, const char* msg) {
                    return std::pair<int, std::string>{
                        code, std::string("{\"ok\":false,\"error\":\"") + msg +
                                  "\"}"};
                };
                if (!air.value->supports_csa()) {
                    return err(400, "CSA unsupported by kernel-monitor backend");
                }
                // §15.5a / B9: re-key from the craft's LIVE announced token
                // before every campaign. do_claim keyed the issuer once, but the
                // craft regenerates its token each boot — a reboot since the
                // claim leaves that key stale and the craft silently drops the
                // §11.4 MAC, so the switch dies with a bare {"ok":true}. The
                // configured-secret path is stable; skip it there.
                if (cparams.psk.empty()) {
                    // Pass 108: two distinct refusals, previously conflated into
                    // one "rebooted? re-scout" string that sent operators to
                    // re-scout a healthy link. Nothing selected is a 409 like
                    // every other unbound-craft refusal; selected-but-keyless
                    // stays a 400.
                    if (active_selection.originator == 0) {
                        return err(409,
                                   "no craft selected (nothing latched, no "
                                   "claim)");
                    }
                    const auto tok =
                        discovery.token_for(active_selection.originator);
                    if (!tok) {
                        return err(400,
                                   "no live CSA key for craft (secret-mode, or "
                                   "not heard for >5 s)");
                    }
                    std::vector<uint8_t> key(tok->begin(), tok->end());
                    if (!issuer.set_psk(key)) {
                        return err(409, "claim busy (campaign active)");
                    }
                }
                const CommonPrefix pre{l.cfg.node.originator, 0, session};
                if (issuer.start(pre, static_cast<uint16_t>(mhz), 0,
                                 static_cast<uint8_t>(klass != 0),
                                 operating_chan, active_selection.bw, 4,
                                 now_us_it)) {
                    previous_selection = active_selection;
                    previous_selection_state = selection_state;
                    pending_selection = active_selection;
                    pending_selection->chan = static_cast<uint16_t>(mhz);
                    pending_selection->bw = 0;
                    selection_state = "claiming";
                    return std::pair<int, std::string>{200, "{\"ok\":true}"};
                }
                return err(409,
                           "rejected (active campaign, PSK, allowlist, or "
                           "rate-limit)");
            };
        }
        h.video_recover = [&](int stream_id) {
            return rx.request_recovery(stream_id, inject_nack);
        };
        // §6.4 RX-local NACK-emission gate — this node only (§15.5).
        h.arq_enable = [&](bool enabled) -> std::string {
            if (arq_rx_enabled != enabled) {
                std::fprintf(stderr, "arq: rx NACK emission %s\n",
                             enabled ? "enabled" : "disabled");
            }
            arq_rx_enabled = enabled;
            return "";
        };
        if (air.value->udp) {
            h.bench_rx_drop = [&](int permille) -> std::string {
                return air.value->set_udp_rx_drop(permille)
                    ? std::string() : "permille must be 0..1000";
            };
        }
        h.reset_stats = [&] {
            rx.reset_stats();
            for (ShmOut& so : shm_outs) {
                so.reasm->reset_stats();
                so.ring->reset_stats();
            }
            if (cache_ctl) {
                cache_ctl->reset_stats();
            }
            if (cache_store) {
                cache_store->reset_stats();
            }
            ret_window_hits = 0;
            ret_window_misses = 0;
            tsf_fallbacks = 0;
            arq_timing.reset();
        };
        control->set_handlers(std::move(h));
        std::fprintf(stderr, "control: REST on %s (rx)\n",
                     l.cfg.control.bind.c_str());
    }
    std::fprintf(stderr, "rx: session=%u, %zu adapters, running%s\n",
                 session, air.value->rx_adapters(),
                 qg.enabled() ? " (quiet-gap returns)" : "");
    while (g_stop == 0) {
        // One timestamp per iteration (see run_tx): callbacks and tick share
        // it so core-injected time never steps backward.
        const uint64_t now = now_ms();
        now_us_it = now_us();
        deliver_now = now;  // the deliver lambda's clock for reassembler pushes
        // Fire the coalesced return window. Reports prefer an EOB midpoint and
        // degrade to opportunistic return only after the bounded fallback.
        const bool have_returns =
            !urgent_ret_held.empty() || !report_ret_held.empty();
        const bool return_deadline_due =
            ret_at_us && now_us_it >= *ret_at_us;
        const bool report_fallback_due =
            !ret_at_us && report_fallback_us &&
            now_us_it >= *report_fallback_us;
        // §11.2 (Pass 90): release the held campaign copy on the same window
        // the returns use, or on its own blind fallback if EOB has stopped.
        // Re-stamped at this instant; dropped outright once T_switch has
        // passed rather than transmitted with a stale dt.
        if (csa_copy_held &&
            (return_deadline_due ||
             (csa_copy_fallback_us && now_us_it >= *csa_copy_fallback_us))) {
            if (issuer.restamp_copy(*csa_copy_held, now_us_it)) {
                uint8_t frame[32];
                if (encode_csa(*csa_copy_held, frame, sizeof(frame)) == 32) {
                    air.value->inject(frame, 32);
                }
            }
            csa_copy_held.reset();
            csa_copy_fallback_us.reset();
        }
        if (return_deadline_due || report_fallback_due) {
            for (const auto& [f, target] : urgent_ret_held) {
                send_return(target, f.data(), f.size(), true);
            }
            // Pass 78: last window's anchored reports repeat here, before
            // the fresh batch so epochs stay monotonic at the receiver.
            for (const auto& [f, target] : report_repeat_held) {
                send_return(target, f.data(), f.size(), false);
            }
            report_repeat_held.clear();
            for (const auto& [f, target] : report_ret_held) {
                send_return(target, f.data(), f.size(), false);
                if (ret_at_us && l.cfg.policy.ret.report_redundancy > 1) {
                    report_repeat_held.emplace_back(f, target);
                }
            }
            // §7.2 observability: a batch fired on a TSF-anchored window
            // deadline is a hit; one sent blind (no EOB heard) is a miss.
            if (ret_at_us && have_returns) {
                ++ret_window_hits;
            } else if (qg.enabled()) {
                ++ret_window_misses;
            }
            urgent_ret_held.clear();
            report_ret_held.clear();
            ret_at_us.reset();
            report_fallback_us.reset();
            ret_tsf_anchored = false;
        }
        const int air_timeout =
            urgent_ret_held.empty() && report_ret_held.empty() ? 2 : 0;
        air.value->poll_once(air_timeout, [&](const AirRxMeta& meta,
                                              const uint8_t* d, size_t n) {
            // udp-air carries no real RSSI; fall back to the loopback
            // section's synthetic value so the §9 selector can be exercised
            // over the dev backend (the radio backend supplies real RSSI).
            const int8_t rssi =
                meta.rssi != 0 ? meta.rssi : l.cfg.loopback.rssi_dbm;
            // §11 taps (cheap header decode; DATA still flows to the engine).
            const Decoded dec = decode(d, n);
            discovery.observe(dec, now, meta.net_id);
            scout.on_frame(meta, dec, d, n);  // §15.5a sweep aggregation
            if (const CsaPacket* c = std::get_if<CsaPacket>(&dec)) {
                // Pass 67: a receiver-owned cache follows only its controller's
                // Ethernet assignment, never an RF spectator campaign.
                if (cache_assignment_gate) return;
                if (!air.value->supports_csa()) {
                    return;
                }
                if (follower.on_csa(*c, now_us_it,
                                    air.value->read_tsf(meta.adapter_id),
                                    static_cast<uint32_t>(meta.tsf_us),
                                    std::nullopt)) {
                    std::fprintf(stderr, "csa: following -> %u MHz\n",
                                 c->target_chan);
                }
                return;
            }
            if (const DataView* v = std::get_if<DataView>(&dec)) {
                arq_timing.note_retransmit_arrived(*v, now_us_it);
                if (frame_is_eob(d, n)) arq_timing.note_eob(now_us_it);
                if (qg.enabled()) {
                    repair_tail_fallback_us =
                        qg.return_deadline(now_us_it, 0, std::nullopt);
                }
                const bool craft_armed =
                    (v->hdr.data_flags & data_flags::kCsaArmed) != 0;
                if (craft_armed) {
                    issuer.note_craft_armed(now_us_it);  // §11.6 implicit ACK
                }
                // §11.6 beacon tail: success is latched here; the campaign
                // (and the selection_state flip) closes at the deadline via
                // the kSuccess action below. Pass 89: only a CSA_ARMED-CLEAR
                // frame counts — the craft clears the bit on COMMITTED, so
                // its absence is the commit proof.
                issuer.note_craft_video(now_us_it, craft_armed);
                if (cache_store) {  // §14.3: retain the verbatim wire packet
                    cache_store->note_data(*v, d, n);
                }
            }
            if (!std::holds_alternative<DecodeError>(dec)) {
                follower.note_valid_rx(now_us_it, be16_read(d + 3));
            }
            if (const VehicleCmd* vc = std::get_if<VehicleCmd>(&dec)) {
                if ((vc->cmd_flags & vcmd_flags::kAck) != 0) {
                    vissuer.on_echo(*vc, now_us_it);  // §11.7 craft echo
                }
                return;  // a ground never acts on a command
            }
            rx.on_air(meta.adapter_id, d, n, now, deliver, rssi,
                      early_deliver);
            if (qg.enabled() && frame_is_paced_eob(d, n)) {
                // Anchor on the SAME adapter's TSF (clocks never cross
                // adapters); a failed read falls back to host arrival.
                // Pass 78: audio EOBs don't re-anchor — the craft's gap
                // keys on the same video EOB this side heard.
                const auto tsf_now = air.value->read_tsf(meta.adapter_id);
                ret_tsf_anchored = tsf_now.has_value();
                if (!tsf_now) {
                    ++tsf_fallbacks;
                }
                ret_at_us = qg.return_deadline(
                    now_us_it, static_cast<uint32_t>(meta.tsf_us), tsf_now);
                repair_tail_fallback_us.reset();
            }
        });
        // With quiet-gap pacing, construct NACKs only after the repair-tail
        // EOB has closed local FEC collection. LINK_REPORT generation remains
        // periodic and may queue before that close.
        const bool repair_tail_closed =
            ret_at_us.has_value() ||
            (repair_tail_fallback_us &&
             now_us_it >= *repair_tail_fallback_us);
        service_cache_repair(now);
        // §6.4 RX-local emission gate (§15.5 POST /api/v1/arq) composes with
        // the quiet-gap repair-tail hold.
        rx.tick(now, deliver, inject_report, inject_nack,
                arq_rx_enabled && (!qg.enabled() || repair_tail_closed));
        air.value->heartbeat(l.cfg.node.originator, session, now);
        // §6.3a: drop reassembler blocks past their deadline (unrecoverable),
        // so a stalled block never wedges frame-shm egress.
        for (ShmOut& so : shm_outs) {
            so.reasm->tick(now, [&](const uint8_t* f, size_t len) {
                write_egress(so.ring.get(), so.stream_id, f, len);
            });
        }
        // §3.9 Pass 106: bootstrap a decoder behind a freshly latched stream.
        rx.emit_latch_recovery(now, inject_nack);
        // §15.5 Pass 108: a §2 latch binds. Until this ruling `active_selection`
        // was written only by CSA campaign outcomes, so a ground that boots,
        // hears its craft and latches sat at originator 0 for its whole uptime —
        // and with it /csa (token_for(0)), every §11.7 command, and §14.3 cache
        // assignment were dead. Adoption is one-way and one-shot (`configured`
        // only), so it can never overwrite a deliberate claim. The tuple carries
        // THIS node's channel/bw/net_id: a latch observes a craft, it does not
        // discover a channel — only /csa and quickconnect move the link.
        //
        // Deliberately NOT apply_selection(): that re-pins the engine's output
        // wants and resets every frame reassembler, which at latch would discard
        // the §3.9 Pass 106 bootstrap IDR just requested. Adoption records the
        // tuple and drives §14.3 assignment, nothing more.
        if (selection_state == "configured") {
            if (const auto latched = rx.latched_originator()) {
                if (*latched != 0 && *latched != active_selection.originator) {
                    active_selection.originator = *latched;
                    active_selection.chan = static_cast<uint16_t>(operating_chan);
                    assign_caches(active_selection);
                    selection_state = "latched";
                    std::fprintf(stderr,
                                 "link: latched originator=%u (%u MHz) — "
                                 "selection bound\n",
                                 *latched, operating_chan);
                }
            }
        }
        // §14.3 cache store: answer requests + push periodic status.
        if (cache_store) {
            uint8_t cbuf[512];
            CacheEndpoint from;
            long rn;
            // B6: bounded drain (see the cache-repair loop above).
            for (int cdrained = 0;
                 cdrained < 64 &&
                 (rn = cache_store_sock->recv_one(cbuf, sizeof(cbuf), &from)) >
                     0;
                 ++cdrained) {
                const Decoded cdec = decode(cbuf, static_cast<size_t>(rn));
                if (const CacheAssign* ca =
                        std::get_if<CacheAssign>(&cdec)) {
                    if (!cache_assignment_gate || !cache_controller_endpoint ||
                        !(from == *cache_controller_endpoint)) {
                        continue;
                    }
                    const CacheAssignmentGate::Verdict verdict =
                        cache_assignment_gate->evaluate(*ca);
                    if (verdict == CacheAssignmentGate::Verdict::kDuplicate) {
                        continue;
                    }
                    if (verdict != CacheAssignmentGate::Verdict::kApply ||
                        !channel_allowed(l.cfg.policy.csa.channel_allowlist,
                                         ca->target_chan)) {
                        continue;
                    }
                    if (!air.value->retune_all(ca->target_chan, ca->target_bw,
                                               false)) {
                        std::fprintf(stderr,
                                     "cache: assignment retune to %u failed\n",
                                     ca->target_chan);
                        continue;
                    }
                    air.value->set_filter_net_id(ca->target_net_id);
                    cache_store->assign_target(ca->target_originator);
                    cache_assignment_gate->commit(*ca);
                    std::fprintf(stderr,
                                 "cache: receiver %u assigned vehicle %u "
                                 "channel %u net_id %u\n",
                                 ca->prefix.originator, ca->target_originator,
                                 ca->target_chan, ca->target_net_id);
                    continue;
                }
                const CacheRequestView* rq =
                    std::get_if<CacheRequestView>(&cdec);
                if (rq == nullptr) {
                    continue;
                }
                std::vector<const std::vector<uint8_t>*> pkts;
                if (cache_store->answer(*rq, now, pkts) !=
                    CacheStore::Verdict::kAnswered) {
                    continue;
                }
                for (const std::vector<uint8_t>* p : pkts) {
                    uint8_t rbuf[kCacheReplyFixedSize + kDataHeaderSize +
                                 kMaxDataPayload];
                    const size_t sz = encode_cache_reply(
                        CommonPrefix{l.cfg.node.originator,
                                     rq->hdr.prefix.originator, session},
                        rq->hdr.request_id, p->data(),
                        static_cast<uint16_t>(p->size()), rbuf, sizeof(rbuf));
                    if (sz > 0) {
                        cache_store_sock->send_to(from, rbuf, sz);
                    }
                }
            }
            if (now >= next_cache_status_ms && !cache_status_to.empty()) {
                next_cache_status_ms =
                    now + l.cfg.cache.store.status_interval_ms;
                for (const CacheStore::StatusEntry& se :
                     cache_store->status()) {
                    CacheStatus cs;
                    cs.prefix = CommonPrefix{l.cfg.node.originator, 0,
                                             session};
                    cs.target_originator = se.key.originator;
                    cs.target_session = se.key.session_id;
                    cs.target_stream_id = se.stream_id;
                    cs.oldest_block = se.oldest_block;
                    cs.newest_block = se.newest_block;
                    cs.rx_health_permille = se.rx_health_permille;
                    cs.capability_flags = cache_capability::kIpTransport;
                    uint8_t sbuf[kCacheStatusSize];
                    if (encode_cache_status(cs, sbuf, sizeof(sbuf)) == 0) {
                        continue;
                    }
                    for (const CacheEndpoint& to : cache_status_to) {
                        if (cache_store_sock->send_to(to, sbuf,
                                                      kCacheStatusSize)) {
                            ++cache_store->stats().status_sent;
                        }
                    }
                }
            }
        }
        std::vector<std::pair<uint8_t, JsccRepairFeedbackState>> feedback;
        feedback.reserve(shm_outs.size());
        for (const ShmOut& so : shm_outs) {
            feedback.emplace_back(so.stream_id, so.reasm->jscc_feedback());
        }
        rx.emit_jscc_feedback(now, feedback, inject_nack);
        // §11 campaign engine. The trigger is now POST /api/v1/csa (§15.5);
        // the stdin trigger was removed with the control-plane migration.
        const CsaIssuer::IssuerAction ia = issuer.tick(now_us_it);
        switch (ia.kind) {
            case CsaIssuer::IssuerAction::Kind::kSendCopy: {
                // §11.2 (Pass 90): campaign copies ride the craft's §7.2 quiet
                // gap like every other ground->craft message. Injecting them
                // blind exempted the one message the campaign depends on from
                // the mechanism that makes delivery to a single-radio,
                // RX-deaf-while-transmitting craft work (~73% per-copy vs
                // gap-scheduled reports arriving at the rate they are sent).
                // Held as a decoded packet, NOT encoded bytes: it is
                // re-stamped and re-MAC'd at release, since dt is relative to
                // the copy's own transmission.
                if (qg.enabled()) {
                    csa_copy_held = ia.pkt;
                    if (!csa_copy_fallback_us) {
                        // If video/EOB stops entirely there is no gap to aim
                        // at; degrade to a prompt send rather than lose the
                        // campaign. One copy spacing, matching the report
                        // path's own blind-fallback posture.
                        csa_copy_fallback_us = now_us_it + 20000;
                    }
                    break;
                }
                uint8_t frame[32];
                if (encode_csa(ia.pkt, frame, sizeof(frame)) == 32) {
                    air.value->inject(frame, 32);
                }
                break;
            }
            case CsaIssuer::IssuerAction::Kind::kCommit:
                if (!air.value->retune_all(ia.chan_mhz, ia.bw, ia.fast)) {
                    // §11.6 review pass 2: an issuer that cannot trust the
                    // position of its ears must not verify with them —
                    // abandon the campaign; the armed craft reverts on its
                    // own verify timeout and reconverges on prev_chan.
                    std::fprintf(stderr, "csa: commit retune to %u MHz "
                                         "FAILED — campaign abandoned\n",
                                 ia.chan_mhz);
                    issuer.note_commit_failed();
                    if (previous_selection) {
                        air.value->retune_all(previous_selection->chan,
                                              previous_selection->bw,
                                              ia.fast);
                        air.value->set_stamp_net_id(
                            previous_selection->net_id);
                        air.value->set_filter_net_id(
                            previous_selection->net_id);
                        apply_selection(*previous_selection);
                        operating_chan = previous_selection->chan;
                        selection_state = previous_selection_state;
                        scout.set_rest_chan(active_selection.chan);
                        scout.set_rest_filter(active_selection.net_id);
                    }
                    pending_selection.reset();
                    previous_selection.reset();
                    break;
                }
                operating_chan = ia.chan_mhz;
                if (pending_selection) {
                    apply_selection(*pending_selection);
                    scout.set_rest_chan(active_selection.chan);
                    scout.set_rest_filter(active_selection.net_id);
                    selection_state = "verifying";
                }
                std::fprintf(stderr, "csa: commit -> %u MHz\n", ia.chan_mhz);
                break;
            case CsaIssuer::IssuerAction::Kind::kSendBeacon: {
                // §11.6 rendezvous beacon (Pass 69) — campaign timing, never
                // quiet-gap-held, same as the copies.
                uint8_t frame[32];
                if (encode_csa(ia.pkt, frame, sizeof(frame)) == 32) {
                    air.value->inject(frame, 32);
                }
                break;
            }
            case CsaIssuer::IssuerAction::Kind::kSuccess:
                // §11.6 beacon tail complete: craft video was seen inside the
                // window and the campaign closed at the deadline.
                if (selection_state == "verifying") {
                    selection_state = "committed";
                }
                pending_selection.reset();
                previous_selection.reset();
                std::fprintf(stderr, "csa: campaign confirmed -> %u MHz\n",
                             operating_chan);
                break;
            case CsaIssuer::IssuerAction::Kind::kRevert:
                // Pass 67: the craft reverts to the campaign's prev_chan; this
                // receiver restores its prior vehicle tuple, which may be on a
                // different channel when switching between vehicles.
                if (previous_selection) {
                    if (!air.value->retune_all(previous_selection->chan,
                                               previous_selection->bw,
                                               ia.fast)) {
                        std::fprintf(stderr, "csa: revert retune to %u MHz "
                                             "FAILED\n",
                                     previous_selection->chan);
                    }
                    air.value->set_stamp_net_id(previous_selection->net_id);
                    air.value->set_filter_net_id(previous_selection->net_id);
                    apply_selection(*previous_selection);
                    operating_chan = previous_selection->chan;
                    selection_state = previous_selection_state;
                    scout.set_rest_chan(active_selection.chan);
                    scout.set_rest_filter(active_selection.net_id);
                }
                pending_selection.reset();
                previous_selection.reset();
                std::fprintf(stderr, "csa: selection reverted -> %u MHz\n",
                             operating_chan);
                break;
            case CsaIssuer::IssuerAction::Kind::kAbort:
                // §15.5a (Pass 65): a failed claim (no CSA_ARMED) rolls every ear
                // and the net_id stamp/filter back to the resting state, so it's a
                // clean no-op rather than stranding a diversity ear on the craft.
                if (!air.value->retune_all(active_selection.chan, op_bw_mhz,
                                           false)) {
                    std::fprintf(stderr, "csa: abort retune to %u MHz "
                                         "FAILED\n",
                                 active_selection.chan);  // Pass 69
                }
                air.value->set_stamp_net_id(active_selection.net_id);
                air.value->set_filter_net_id(active_selection.net_id);
                operating_chan = active_selection.chan;
                selection_state = previous_selection_state;
                pending_selection.reset();
                previous_selection.reset();
                std::fprintf(stderr,
                             "csa: aborted (no CSA_ARMED) -> resting %u MHz\n",
                             operating_chan);
                break;
            case CsaIssuer::IssuerAction::Kind::kNone:
                break;
        }
        // §11.7 command campaign copies ride the same uplink as CSA copies.
        {
            const VcmdIssuer::Action va = vissuer.tick(now_us_it);
            if (va.kind == VcmdIssuer::Action::Kind::kSendCopy) {
                uint8_t frame[kVehicleCmdSize];
                if (encode_vehicle_cmd(va.pkt, frame, sizeof(frame)) ==
                    sizeof(frame)) {
                    air.value->inject(frame, sizeof(frame));
                }
            }
        }
        // Spectator follower actions (PSK-less RX nodes; a ground issuer's
        // follower stays IDLE for its own campaigns — own frames are dropped).
        const CsaAction fa = follower.tick(now_us_it);
        if (fa.kind != CsaAction::Kind::kNone) {
            air.value->retune_all(fa.chan_mhz, fa.bw, fa.fast);
            operating_chan = fa.chan_mhz;  // §15.3 link.channel tracks a
                                           // followed retune (not boot chan)
        }
        scout.tick(now);  // §15.5a advance the sweep when a dwell elapses
        if (const auto trc = air.value->tx_progress_counters()) {
            if (wedge.poll(now, trc->first, trc->second)) {
                std::fprintf(stderr, "%s", wedge.wedged()
                        ? "air: TX WEDGE — submissions advancing, zero "
                          "backend TX progress over the window (§9.10)\n"
                        : "air: tx wedge cleared — backend TX progress "
                          "resumed\n");
            }
        }
        if (control) {
            control->service(now);
        }
        if (stats_period != 0 && now >= next_stats) {
            // §6.3a: fold the per-out-stream reassembler counters into stats.
            std::vector<std::pair<uint8_t, FrameReassemblerStats>> frame_stats;
            std::vector<std::pair<uint8_t, FrameShmRing::Stats>> shm_stats;
            frame_stats.reserve(shm_outs.size());
            shm_stats.reserve(shm_outs.size());
            for (const ShmOut& so : shm_outs) {
                frame_stats.emplace_back(so.stream_id, so.reasm->stats());
                shm_stats.emplace_back(so.stream_id, so.ring->stats());
            }
            // §15.3: cache blocks are emitted only when the role is enabled.
            CacheRepairStatsOut crs;
            if (cache_ctl) {
                const CacheRepairStats s = cache_ctl->stats();
                crs.requests = s.requests;
                crs.replies = s.replies;
                crs.symbols_accepted = s.symbols_accepted;
                crs.symbols_rejected = s.symbols_rejected;
                crs.blocks_closed_deficit = s.blocks_closed_deficit;
                crs.blocks_repaired = s.blocks_repaired;
                crs.blocks_futile = s.blocks_futile;
                crs.requests_suppressed = s.requests_suppressed;
                crs.caches_fresh = s.caches_fresh;
                crs.nack_graces_armed = s.nack_graces_armed;
                crs.blocks_repaired_before_nack =
                    s.blocks_repaired_before_nack;
                crs.request_to_first_reply = {
                    s.request_to_first_reply.samples,
                    s.request_to_first_reply.p95_us,
                    s.request_to_first_reply.max_us};
                crs.request_to_completion = {
                    s.request_to_completion.samples,
                    s.request_to_completion.p95_us,
                    s.request_to_completion.max_us};
            }
            CacheStoreStatsOut css;
            if (cache_store) {
                const CacheStoreStats& s = cache_store->stats();
                css.requests_received = s.requests_received;
                css.requests_answered = s.requests_answered;
                css.requests_rejected = s.requests_rejected;
                css.symbols_sent = s.symbols_sent;
                css.status_sent = s.status_sent;
                css.blocks_held = s.blocks_held;
                css.health_permille = s.health_permille;
            }
            const ArqTimingStats timing = arq_timing.snapshot();
            const VcmdStatsFill vfill{0, vissuer.state_str(),
                                      vissuer.nonce(), arq_rx_enabled};
            emit_stats(emitter, l, session, t0, nullptr, &rx, &*air.value,
                       tsf_fallbacks,
                       issuer.active() ? issuer.state_str()
                                       : follower.state_str(),
                       ret_window_hits, ret_window_misses, wedge.wedged(),
                       &frame_stats, &shm_stats,
                       cache_ctl ? &crs : nullptr,
                       cache_store ? &css : nullptr, &last_snap, &timing,
                       &vfill, operating_chan);
            if (control) {
                control->publish_stats(emitter.last_line());
            }
            next_stats = now + stats_period;
        }
    }
    return 0;
}

int run_loopback(const Loaded& l) {
    auto bindings = BindingSet::create(l.cfg);
    if (!bindings) {
        std::fprintf(stderr, "binding error: %s\n", bindings.error.c_str());
        return 1;
    }
    const uint32_t session = session_nonce();
    TxCore tx(l.cfg, session, l.have_table ? &l.table : nullptr, l.tv);
    // The RX wants: loopback delivers every in-stream back out; if no
    // out-streams are configured, latch the in-streams' types and drop the
    // payloads (counter-only bench).
    Config rx_cfg = l.cfg;
    if (RxCore::wants(rx_cfg).empty()) {
        for (const StreamCfg& s : l.cfg.streams) {
            if (s.dir == Dir::kIn) {
                StreamCfg out = s;
                out.dir = Dir::kOut;
                out.bind = BindCfg{};
                rx_cfg.streams.push_back(out);
            }
        }
    }
    RxCore rx(rx_cfg, session ^ 0x5A5A5A5Au,
              l.have_table ? &l.table : nullptr,
              l.have_table ? std::optional<uint8_t>(l.tv) : std::nullopt);

    std::optional<GeParams> ge;
    if (l.cfg.loopback.ge) {
        ge = GeParams{(*l.cfg.loopback.ge)[0], (*l.cfg.loopback.ge)[1],
                      (*l.cfg.loopback.ge)[2], (*l.cfg.loopback.ge)[3]};
    }
    AdapterLossField field(l.cfg.loopback.adapters, l.cfg.loopback.seed,
                           l.cfg.loopback.correlation, l.cfg.loopback.uniform_p,
                           ge);
    LossRng return_rng;
    return_rng.s = l.cfg.loopback.seed ^ 0xFEEDFACEull;

    // One timestamp per loop iteration, shared by every callback and tick
    // (see run_tx): a fresh now_ms() inside inject can land 1 ms after the
    // tick's captured now and u64 "now - stamp" arithmetic underflows.
    uint64_t loop_now = now_ms();

    const RxEngine::Deliver deliver = [&](uint8_t sid, uint32_t, uint8_t,
                                          const uint8_t* d, size_t n) {
        if (UdpEgress* out = bindings.value->egress_for(sid)) {
            out->send(d, n);
        }
    };
    // Synthetic RSSI for the §9 loop: static value, with an optional
    // scripted fade window (relative to process start).
    const uint64_t rssi_t0 = now_ms();
    const auto synthetic_rssi = [&]() -> int8_t {
        if (const auto& f = l.cfg.loopback.rssi_fade) {
            const uint64_t rel = loop_now - rssi_t0;
            if (rel >= f->start_ms && rel < f->end_ms) {
                return f->dbm;
            }
        }
        return l.cfg.loopback.rssi_dbm;
    };
    // TX -> synthetic air -> RX: one loss verdict per (packet, adapter).
    const TxCore::Inject inject = [&](const uint8_t* f, size_t n) {
        field.begin_packet();
        for (uint8_t a = 0; a < field.adapters(); ++a) {
            if (!field.drop(a)) {
                rx.on_air(a, f, n, loop_now, deliver, synthetic_rssi());
            }
        }
    };
    // RX -> return direction -> TX (its own loss). No L2 addressing in
    // loopback — the unicast target is meaningless here.
    const RxCore::Inject inject_nack = [&](const uint8_t* f, size_t n,
                                           uint16_t) {
        if (return_rng.uniform() >= l.cfg.loopback.return_loss_p) {
            tx.on_air(f, n, loop_now);
        }
    };

    StatsEmitter emitter(l.cfg.stats.to_stdout, bindings.value->stats_egress());
    const uint64_t t0 = now_ms();
    uint64_t next_stats = t0;
    const uint64_t stats_period =
        l.cfg.stats.hz > 0 ? static_cast<uint64_t>(1000.0 / l.cfg.stats.hz)
                           : 0;
    // §15.5 REST control plane on the bench: tx knobs (profile/fec) + reset
    // (both sides). CSA is a no-op with synthetic air → left null → 409.
    StatsSnapshot last_snap;
    std::string loop_active_mode = l.cfg.venc.active_mode;  // §15.5 Pass 96
    std::unique_ptr<ControlServer> control;
    if (!l.cfg.control.bind.empty()) {
        auto cs = ControlServer::create(l.cfg.control.bind);
        if (!cs) {
            std::fprintf(stderr, "control: %s\n", cs.error.c_str());
            return 1;
        }
        control = std::move(*cs.value);
        ControlHandlers h;
        h.stats_line = [&]() -> std::string {
            std::string s = emitter.last_line();
            if (!s.empty() && s.back() == '\n') s.pop_back();
            return s;
        };
        h.info_json = [&] {
            // loopback has no AirBackend — nothing retunes, so the config
            // channel is the live channel.
            return build_info_json(l, session, "loopback");
        };
        h.health_json = [&] { return build_health_json(last_snap); };
        h.profile = [&](int mn, int mx) -> std::string {
            if (mn < 0 || mn > 255 || mx < 0 || mx > 255)
                return "min/max must be 0..255";
            if (mx != 255 && mn > mx) return "min > max";
            tx.set_profile_pin(static_cast<uint8_t>(mn),
                               static_cast<uint8_t>(mx));
            return "";
        };
        h.fec = [&](int sid, int ip, int pp, int mk, int mr) -> std::string {
            if (sid < 0 || sid > 255) return "bad stream_id";
            if (ip < 0 || ip > 4000 || pp < 0 || pp > 4000 || mk < 1 ||
                mr < 0 || mr > 255)
                return "bad fec rates (0..4000 permille, min_k>=1, min_r 0..255)";
            return tx.set_stream_fec(static_cast<uint8_t>(sid),
                                     static_cast<uint16_t>(ip),
                                     static_cast<uint16_t>(pp),
                                     static_cast<uint16_t>(mk),
                                     static_cast<uint16_t>(mr))
                       ? ""
                       : "no frame-shm stream with that id";
        };
        h.reset_stats = [&] {
            tx.reset_stats();
            rx.reset_stats();
        };
        // §15.5 operating-mode selection (Pass 96) — same as tx, so the bench
        // can exercise it. See the tx block for the full contract.
        h.mode_get = [&]() -> std::string {
            std::string s = "{\"active\":\"" + loop_active_mode + "\"";
            s += ",\"apply_configured\":";
            s += l.cfg.venc.mode_apply_cmd.empty() ? "false" : "true";
            s += "}";
            return s;
        };
        h.mode_set = [&](const std::string& name) -> std::string {
            if (l.cfg.venc.mode_apply_cmd.empty()) {
                return "no venc.mode_apply_cmd configured on this node";
            }
            for (char c : name) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) ||
                      c == '-' || c == '_' || c == '.')) {
                    return "mode name: only [A-Za-z0-9._-] allowed";
                }
            }
            if (!spawn_mode_applier(l.cfg.venc.mode_apply_cmd, name)) {
                return "failed to launch mode applier";
            }
            loop_active_mode = name;
            return "";
        };
        h.modes_list = [&, modes_dir = mode_catalog_dir(l.cfg.venc)]() {
            return modes_catalog_json(modes_dir, loop_active_mode,
                                      !l.cfg.venc.mode_apply_cmd.empty());
        };
        control->set_handlers(std::move(h));
        std::fprintf(stderr, "control: REST on %s (loopback)\n",
                     l.cfg.control.bind.c_str());
    }
    std::fprintf(stderr, "loopback: %u adapters, seed=%llu, running\n",
                 l.cfg.loopback.adapters,
                 static_cast<unsigned long long>(l.cfg.loopback.seed));
    while (g_stop == 0) {
        loop_now = now_ms();
        bindings.value->poll_once(2, [&](const IngressEvent& ev) {
            tx.on_ingress(ev.stream_id, ev.data, ev.len, loop_now, inject);
        });
        tx.tick(loop_now, inject);
        rx.tick(loop_now, deliver, inject_nack, inject_nack);
        if (control) {
            control->service(loop_now);
        }
        if (stats_period != 0 && loop_now >= next_stats) {
            emit_stats(emitter, l, session, t0, &tx, &rx, nullptr, 0, nullptr,
                       0, 0, false, nullptr, nullptr, nullptr, nullptr,
                       &last_snap);
            if (control) {
                control->publish_stats(emitter.last_line());
            }
            next_stats = loop_now + stats_period;
        }
    }
    return 0;
}

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <tx|rx|loopback> -c <config.json> [--check]\n"
                 "  --check  validate config + bindings and exit\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage(argv[0]);
    }
    const std::string mode = argv[1];
    if (mode != "tx" && mode != "rx" && mode != "loopback") {
        return usage(argv[0]);
    }
    std::string config_path;
    bool check_only = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else {
            return usage(argv[0]);
        }
    }
    if (config_path.empty()) {
        return usage(argv[0]);
    }

    Loaded l;
    if (const int rc = load_all(config_path, l); rc != 0) {
        return rc;
    }
    if (check_only) {
        auto bindings = BindingSet::create(l.cfg);
        if (!bindings) {
            std::fprintf(stderr, "binding error: %s\n",
                         bindings.error.c_str());
            return 1;
        }
        std::fprintf(stderr, "bindings: OK\n");
        return 0;
    }

    // A venc restart across our HTTP send, or a log reader exiting on the
    // §15.3 stdout NDJSON, must not take the flight process down with it.
    // Every write path checks its own return value.
    std::signal(SIGPIPE, SIG_IGN);
    // B2 (pre-flight audit): install SIGINT/SIGTERM WITHOUT SA_RESTART. glibc's
    // std::signal() defaults to BSD semantics (SA_RESTART set), which restarts a
    // blocking flight-loop syscall instead of interrupting it — so a shutdown
    // signal could be swallowed. Every blocking path already handles EINTR
    // (air_mon recv/recvmsg, the bounded CLI wait), so with SA_RESTART cleared
    // the loop observes g_stop promptly on teardown.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    if (mode == "tx") {
        return run_tx(l);
    }
    if (mode == "rx") {
        return run_rx(l);
    }
    return run_loopback(l);
}
