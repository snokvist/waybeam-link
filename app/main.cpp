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
#include "wblink/config_registry.h"
#include "wblink/control_server.h"
#include "wblink/modes.h"
#include "wblink/crc8.h"
#include "wblink/csa.h"
#include "wblink/hmac_sha256.h"
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
#include "wblink/mcs_probe.h"
#include "wblink/reporter.h"
#include "wblink/ring.h"
#include "wblink/rx.h"
#include "wblink/scheduler.h"
#include "wblink/scout_sense.h"
#include "wblink/scout_store.h"
#include "wblink/selector.h"
#include "wblink/calib_store.h"
#include "wblink/calibrate.h"
#include "wblink/selector_state.h"
#include "wblink/stats.h"
#include "wblink/node/aim.h"
#include "wblink/node/air_backend.h"
#include "wblink/node/clock.h"
#include "wblink/node/tx_core.h"
#include "wblink/node/discovery.h"
#include "wblink/node/rx_core.h"
#include "wblink/table.h"
#include "wblink/airtime.h"
#include "wblink/txwedge.h"
#include "wblink/venc.h"
#include "wblink/video_slot_cadence.h"
#include "wblink/uplink_calib_store.h"
#include "wblink/uplink_calibrate.h"
#include "wblink/calib_dwell.h"
#if WBLINK_RADIO
#include "wblink/air_radio.h"
#endif

namespace {

using namespace wblink;
using wblink::node::AirBackend;
using wblink::node::aim_log_enabled;
using wblink::node::AimHist;
using wblink::node::g_aim_read_tsf;
using wblink::node::g_aim_release;
using wblink::node::mcs_trace_enabled;
using wblink::node::now_ms;
using wblink::node::now_us;
using wblink::node::calib_params_from;
using wblink::node::s_to_ms;
using wblink::node::selector_policy;
using wblink::node::bw_code;
using wblink::node::scheduler_policy;
using wblink::node::TxCore;
using wblink::node::DiscoveryCatalog;
using wblink::node::PacketEventTrace;
using wblink::node::RxCore;
using wblink::node::ScoutEngine;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// §9.10 v2 (Pass 148): distinct from the generic failure 1, so a supervisor
// (or an operator reading the log) can tell "the transmitter wedged and I am
// asking to be re-execed" from "this node cannot start at all".
constexpr int kExitTxWedged = 9;

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

// packet_type_name moved to node/include/wblink/node/air_backend.h (#109 Phase 2a).

// PacketEventTrace moved to node/include/wblink/node/air_backend.h (#109 Phase 2a).

// DiscoveryCatalog moved to node/include/wblink/node/discovery.h
// (#109 Phase 2a).

// ScoutEngine moved to node/include/wblink/node/discovery.h
// (#109 Phase 2a). Its side effects were already injected Hooks.

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
    if (name == "mtu_tier") return vcmd_id::kMtuTier;  // §9.3a Pass 122
    if (name == "tx_power") return vcmd_id::kTxPower;  // §10.3 Pass 135
    return 0;
}


static std::string power_tier_json(int tier, const std::vector<int32_t>& presets,
                                   std::optional<int32_t> ceiling,
                                   bool effective) {
    std::string s = "{\"tier\":" + std::to_string(tier) + ",\"presets_qdb\":[";
    for (size_t i = 0; i < presets.size(); ++i) {
        if (i != 0) s += ",";
        s += std::to_string(presets[i]);
    }
    s += "],\"ceiling_qdb\":";
    s += ceiling ? std::to_string(*ceiling) : "null";
    // §10.3 (Pass 134): a tier on a node with no curve is recorded and moves
    // nothing. Report that rather than implying the setting reached hardware.
    s += ",\"effective\":";
    s += effective ? "true" : "false";
    s += "}";
    return s;
}

// §10.3/§10.5/§10.7/§11.7 0x0A — the ground uplink's power owner (Pass 138).
//
// ONE precedence path: §10.5 latch, then an explicit configured map, then a
// matching artifact, then backend auto. run_rx spelled that ordering out FOUR
// times — the actuation, the §15.3 fill, the startup log, and the §15.5
// `effective` flag — under a comment warning that "a second copy of this
// ordering is how the two drift". Two copies had in fact already drifted, and
// both were Pass 136 bug fixes: `effective` claimed a tier bound hardware when
// it did not, and the tier's own apply path reached no actuator at all in two
// of three configurations. The hazard was documented rather than removed; this
// removes it.
//
// Actuators are injected, exactly as TxCore does with apply_power, so every
// rule below is reachable from a test with no radio, no file and no socket.
struct UplinkPower {
    enum class Owner { kNone, kOverride, kConfigMap, kArtifact };

    std::function<void(int32_t qdb)> apply_qdb;
    std::function<void()> apply_auto;
    // §10.7 applicability is a function of the pairing tuple, which lives in
    // run_rx and changes with selection/CSA — so this stays a callback. It
    // returns an already-ceiling-clamped value, and has the side effect of
    // updating the stale flag; both are properties of its own resolve.
    std::function<std::optional<int32_t>()> artifact_qdb;

    std::vector<int32_t> presets_qdb;
    std::optional<int32_t> ceiling_qdb;   // §10.3, moved by a tier
    std::optional<PowerCurve> curve;      // §10.2 config power_map, if any
    uint8_t mcs = 0;                      // the uplink's fixed operating point
    int tier = -1;                        // §11.7 0x0A, -1 = unset
    std::optional<int32_t> owner_qdb;     // curve resolve under ceiling_qdb
    std::optional<int32_t> override_qdb;  // §10.5 latch (volatile)

    std::optional<int32_t> artifact() const {
        return artifact_qdb ? artifact_qdb() : std::nullopt;
    }

    // Owner + the value that owner REQUESTS, resolved together in the one
    // place the precedence is written. Walking the chain separately per
    // accessor is how run_rx ended up with four copies of it, and the first
    // draft of this struct promptly grew three of its own.
    //
    // Resolving once also matters because artifact() is not free: it re-runs
    // the §10.7 pairing check and updates the stale flag as a side effect.
    struct Resolved {
        Owner owner = Owner::kNone;
        std::optional<int32_t> qdb;
    };
    Resolved resolve() const {
        if (override_qdb) return {Owner::kOverride, override_qdb};
        if (owner_qdb) return {Owner::kConfigMap, owner_qdb};
        if (const std::optional<int32_t> a = artifact()) {
            return {Owner::kArtifact, a};
        }
        return {};
    }

    Owner owner() const { return resolve().owner; }

    const char* owner_name() const {
        switch (owner()) {
            case Owner::kOverride: return "§10.5 override";
            case Owner::kConfigMap: return "config power_map";
            case Owner::kArtifact: return "artifact";
            case Owner::kNone: break;
        }
        return "backend auto";
    }

    // What §15.3 REPORTS. §10.5 is explicit that a latch reports the REQUEST,
    // not the clamped value the hardware got.
    std::optional<int32_t> reported_qdb() const { return resolve().qdb; }

    // What the ACTUATOR receives. Only the latch needs clamping here: the
    // curve resolve takes the ceiling as an argument and the artifact resolve
    // clamps at its source, so clamping them twice would be a no-op that
    // invited someone to "simplify" one of the three away.
    std::optional<int32_t> hw_qdb() const {
        const Resolved r = resolve();
        if (r.owner == Owner::kOverride && ceiling_qdb) {
            return std::min(*r.qdb, *ceiling_qdb);
        }
        return r.qdb;
    }

    // The single convergence point. Every §10.5/§10.7/§11.7 path ends here.
    void apply() {
        if (const std::optional<int32_t> q = hw_qdb()) {
            if (apply_qdb) apply_qdb(*q);
        } else if (apply_auto) {
            apply_auto();
        }
    }


    // §10.3 Pass 134/136: a ceiling binds only where a number of ours reaches
    // the actuator — a curve, an artifact, or a held latch clamped by it.
    bool effective() const { return owner() != Owner::kNone; }

    // Re-resolve the configured map under the CURRENT ceiling. Resolved once
    // at startup and never again, this pinned the boot ceiling for the life of
    // the process, so a tier could not lower a power_map-owned uplink.
    void resolve_owner() {
        if (!curve) return;
        owner_qdb =
            resolve_power_qdb(*curve, mcs, kPowerLevelBaseline, ceiling_qdb);
    }

    // §11.7 0x0A. false = REJECTED (no list, or index past its end). The
    // calibrator's sweep bound is the CALLER's to move — it is not power.
    bool set_tier(int t) {
        if (t < 0 || static_cast<size_t>(t) >= presets_qdb.size()) return false;
        tier = t;
        ceiling_qdb = presets_qdb[static_cast<size_t>(t)];
        resolve_owner();
        apply();
        return true;
    }

    std::string json() const {
        return power_tier_json(tier, presets_qdb, ceiling_qdb, effective());
    }
};

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
        case vcmd_id::kMtuTier: return "mtu_tier";  // §9.3a Pass 122
        case vcmd_id::kTxPower: return "tx_power";  // §10.3 Pass 135
    }
    return "";
}

std::optional<uint8_t> mtu_tier_for_mode(const std::string& mode,
                                         uint16_t supported) {
    if (mode == "default") return mtu_tier::kDefault;
    if (mode == "medium") return mtu_tier::kMedium;
    if (mode == "high") return mtu_tier::kHigh;
    if (mode == "auto") {
        if (supported >= mtu_tier::kHighBudget) return mtu_tier::kHigh;
        if (supported >= mtu_tier::kMediumBudget) return mtu_tier::kMedium;
        return mtu_tier::kDefault;
    }
    return std::nullopt;
}

// bw_code moved to node/include/wblink/node/tx_core.h (#109 Phase 2a).

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

// scheduler_policy moved to node/include/wblink/node/tx_core.h (#109 Phase 2a).

// s_to_ms moved to node/include/wblink/node/tx_core.h (#109 Phase 2a).

// selector_policy moved to node/include/wblink/node/tx_core.h (#109 Phase 2a).

QuietGapPolicy quietgap_policy(const Config& cfg) {
    QuietGapPolicy p;
    p.enabled = cfg.policy.ret.quiet_gap;
    p.guard_us = cfg.policy.ret.guard_us;
    p.window_us = cfg.policy.ret.return_window_us;
    return p;
}

// AimHist + aim_log_enabled moved to node/include/wblink/node/aim.h.

// now_ms/now_us moved to node/include/wblink/node/clock.h (#109 Phase 2a).

// mcs_trace_enabled moved to node/include/wblink/node/air_backend.h (#109 Phase 2a).

// ---- air backend selection (udp dev backend | devourer radio, §3.0) --------

// AirBackend moved to node/include/wblink/node/air_backend.h (#109 Phase 2a).

// ---- TX side: per in-stream framer + ring + scheduler ----------------------

// calib_params_from moved to node/include/wblink/node/tx_core.h (#109 Phase 2a).

// TxCore moved to node/include/wblink/node/tx_core.h (#109 Phase 2a).

// ---- RX side: engine + NACK encode -----------------------------------------

// RxCore moved to node/include/wblink/node/rx_core.h (#109 Phase 2a).
// It referenced no other app-layer structure, which is why it went first.

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
    const char* mtu_mode = nullptr;    // ground local preference
    uint16_t mtu_requested = kDefaultMaxPayload;
    uint16_t mtu_effective = kDefaultMaxPayload;
    uint16_t mtu_supported = kDefaultMaxPayload;
};

// §10.7 (Pass 125) ground-uplink stats. A struct rather than eleven more
// parameters, and a pointer so every other role emits the role-neutral
// idle/zero defaults without threading anything.
struct UplinkStatsFill {
    const char* state = "idle";
    uint8_t rung = 0;
    int32_t power_qdb = 0;
    uint8_t fingerprint = 0;
    bool stale = false;
    // §3.16 (Pass 153) probe-exchange counters for the local node's run.
    uint64_t probes_sent = 0;
    uint64_t tallies_rx = 0;
    uint8_t rx_mcs = kUplinkRxMcsUnknown;
    // §10.3/§10.5/§11.7 0x0A (Pass 135). Only TxCore::fill_stats sets the
    // link.tx_power_* fields, and the ground has no TxCore — so its §15.3
    // reported tier -1 and power 0 no matter what was selected, while the
    // endpoint reported the truth. The hub menu binds to §15.3, so the one
    // row an operator reads was the one that could not move.
    int tx_power_tier = -1;
    int32_t tx_power_ceiling_qdb = 0;
    bool tx_power_tier_effective = false;
    bool tx_power_override = false;
    int32_t tx_power_qdb = 0;
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
                uint16_t channel_mhz = 0,
                const UplinkStatsFill* uplink = nullptr) {
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
    if (uplink != nullptr) {  // §10.7 Pass 125 (ground/rx node only)
        snap.link.uplink_calib_state = uplink->state;
        snap.link.uplink_calib_rung = uplink->rung;
        snap.link.uplink_calib_power_qdb = uplink->power_qdb;
        snap.link.uplink_calib_fingerprint = uplink->fingerprint;
        snap.link.uplink_calib_stale = uplink->stale;
        snap.link.calib_probes_sent = uplink->probes_sent;
        snap.link.calib_tallies_rx = uplink->tallies_rx;
        snap.link.calib_rx_mcs = uplink->rx_mcs;
        snap.link.tx_power_tier = uplink->tx_power_tier;
        snap.link.tx_power_ceiling_qdb = uplink->tx_power_ceiling_qdb;
        snap.link.tx_power_tier_effective = uplink->tx_power_tier_effective;
        snap.link.tx_power_override = uplink->tx_power_override;
        snap.link.tx_power_qdb = uplink->tx_power_qdb;
    }
    if (vcmd != nullptr) {
        snap.link.cmd_last_nonce = vcmd->cmd_last_nonce;
        if (vcmd->vcmd_state != nullptr) {
            snap.link.vcmd_state = vcmd->vcmd_state;
            snap.link.vcmd_nonce = vcmd->vcmd_nonce;
        }
        snap.link.arq_rx_enabled = vcmd->arq_rx_enabled;
        if (vcmd->mtu_mode != nullptr) {
            snap.link.mtu_mode = vcmd->mtu_mode;
            snap.link.mtu_requested = vcmd->mtu_requested;
            snap.link.mtu_effective = vcmd->mtu_effective;
            snap.link.mtu_supported = vcmd->mtu_supported;
        }
    }
    // Air adapters first so RxCore::fill_stats can merge its per-adapter
    // liveness view into them by index (radio backend; no-op on udp).
    if (air != nullptr) {
        air->fill_adapter_stats(snap, tsf_fallbacks, tx_wedged);
    }
    if (rx != nullptr) {
        rx->fill_stats(snap, now);
#if WBLINK_RADIO
        // §15.3 Pass 159 role-dependent view: a radio GROUND shows the
        // cause it computes (what it sends); the craft's accepted-verdict
        // fill lives in its own tx fill and is not touched here.
        if (air != nullptr && air->radio != nullptr) {
            snap.link.verdict = air->radio->link_verdict();
            snap.link.verdict_age_ms = 0;
        }
#endif
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
        s += "\",\"channel\":" + std::to_string(chan);
        // §15.5 (Pass 154): the per-unit EFUSE identity the §10.6 artifacts
        // key on; null where the backend reports none (D3 posture visible).
        const std::string mac =
            air != nullptr ? air->adapter_mac(i) : std::string{};
        s += ",\"mac\":";
        s += mac.empty() ? "null" : "\"" + mac + "\"";
        s += "}";
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
    TxCore tx(l.cfg, session, l.have_table ? &l.table : nullptr, l.tv,
              air.value->mtu_supported());
    const AdapterCfg* calib_tx_adapter = nullptr;
    size_t calib_tx_idx = 0;
    for (size_t i = 0; i < l.cfg.adapters.size(); ++i) {
        if (l.cfg.adapters[i].role == Role::kTx) {
            calib_tx_adapter = &l.cfg.adapters[i];
            calib_tx_idx = i;
            break;
        }
    }
    // §10.6 (Pass 154): the canonical calibration identity, resolved ONCE
    // against the live per-unit EFUSE MAC the backend read at bring-up.
    // Empty = an identity-less unit on the radio backend — the D3 fail-closed
    // answer: no calibrator, no artifact read or write, no absolute curve.
    // The §10.5 safe boot offset still applies, so the node stays flyable.
    const std::string calib_ident =
        calib_tx_adapter
            ? calib_identity(*calib_tx_adapter, l.cfg.air.kind,
                             air.value->adapter_mac(calib_tx_idx))
            : "udp";
    if (calib_tx_adapter != nullptr && calib_ident.empty()) {
        std::fprintf(stderr,
                     "calibrate: adapter \"%s\" reports no EFUSE identity — "
                     "§10.6 calibration and any absolute curve are REFUSED "
                     "(Pass 154 D3); running at the §10.5 safe boot offset\n",
                     calib_tx_adapter->name.c_str());
    }
    // §10.5 (Pass 150): ONLY devourer's lever is relative. Keyed on the
    // backend kind, not on is_rf() — the retired kernel-monitor backend also
    // answered is_rf() true while commanding absolute qdb, and gating on it
    // made calibration unreachable fleet-wide.
    //
    // This must precede init_calibration: §10.6 (Pass 151) derives its sweep
    // window from the backend's space, so a flag set afterwards would build an
    // absolute-space calibrator on a relative backend — the exact +27 dB
    // hazard Pass 150 refused the run to avoid.
    tx.set_backend_relative(l.cfg.air.kind == AirCfg::Kind::kRadio);
    // §10.6 (Pass 120): craft-resident calibration — engine seeds, artifact
    // persistence, and the boot auto-load with the fingerprint gate. The
    // §10.3 ceiling (Pass 134) comes from the same adapter the sweep drives.
    // Not constructed at all without an identity (Pass 154 D3): a run whose
    // artifact could never be keyed must refuse at start, not at persist.
    // The policy seeds (feed-quiet expiry) apply either way — the §3.16
    // tally receiver runs on a D3 craft too.
    tx.seed_calib_policy(l.cfg.policy.calibration);
    if (!calib_ident.empty()) {
        tx.init_calibration(l.cfg.policy.calibration,
                            calib_tx_adapter != nullptr
                                ? calib_tx_adapter->max_power_qdb
                                : std::nullopt);
    }
    // §3.16 (Pass 153): the craft's half of the identity pair, stamped
    // into every TALLY. Same canonical identity §10.6 keys its own
    // artifact on, hashed to the one byte the wire has room for. An empty
    // identity hashes to 0 — exactly the wire's "unknown" sentinel.
    tx.set_calib_identity(
        crc8_dvbs2(reinterpret_cast<const uint8_t*>(calib_ident.data()),
                   calib_ident.size()));
    // The last persisted artifact (boot-loaded or written this session) —
    // GET /api/v1/calibration must never report a fingerprint with no body.
    std::optional<CalibArtifact> last_artifact;
    tx.on_calib_artifact = [&](const CalibArtifact& art) {
        if (calib_ident.empty()) {
            // Unreachable while D3 refuses the calibrator whole; kept as the
            // belt so no future start path can persist an unkeyed artifact.
            std::fprintf(stderr,
                         "calibrate: artifact write refused — no adapter "
                         "identity (Pass 154 D3)\n");
            return;
        }
        const uint8_t fp = calib_store_write(
            l.cfg.policy.calibration.artifact_dir, calib_ident, art);
        if (fp == 0) {
            std::fprintf(stderr, "calibrate: artifact write FAILED (%s)\n",
                         l.cfg.policy.calibration.artifact_dir.c_str());
            return;
        }
        last_artifact = art;
        PowerCurve c;
        for (size_t m = 0; m < 8; ++m) c.qdb[m] = art.curve_qdb[m];
        c.valid = true;
        tx.install_curve(c);
        tx.calib_fingerprint_ = fp;
        tx.calib_stale_ = false;
        std::fprintf(stderr, "calibrate: artifact persisted fp=0x%02x\n", fp);
    };
    if (auto stored = calib_ident.empty()
                          ? Result<CalibStored>::fail("no identity (D3)")
                          : calib_store_load(
                                l.cfg.policy.calibration.artifact_dir);
        stored) {
        const std::string& ident = calib_ident;
        // §10.6 (Pass 151): the backend-scoped identity proves which BACKEND
        // authored the artifact, not which SPACE — and on devourer those came
        // apart exactly once, in the window before Pass 150 refused the run.
        // Such an artifact holds absolute rungs (4..108) that would now be
        // read as offsets, clamp onto the §10.5 bound, and park the node on
        // the uncharacterised efuse default this pass exists to keep it off.
        // A placement outside the live window is that artifact; refuse it the
        // same way a fingerprint mismatch is refused.
        const auto win = tx.offset_window();
        bool space_ok = true;
        if (win) {
            for (const int32_t q : stored.value->artifact.placement_qdb) {
                if (q < win->first || q > win->second) space_ok = false;
            }
        }
        if (stored.value->identity == ident && space_ok) {
            // Explicit config power_map wins; the artifact fills the gap.
            if (!tx.has_power_curve()) {
                tx.install_curve(stored.value->curve);
                std::fprintf(stderr,
                             "calibrate: boot auto-load fp=0x%02x (%s)\n",
                             stored.value->fingerprint, ident.c_str());
            }
            tx.calib_fingerprint_ = stored.value->fingerprint;
            last_artifact = stored.value->artifact;  // §15.5 GET surface
        } else {
            tx.calib_stale_ = true;  // §10.6: surface, never apply
            std::fprintf(stderr,
                         "calibrate: STALE artifact (stored %s, live %s%s)\n",
                         stored.value->identity.c_str(), ident.c_str(),
                         space_ok ? "" : ", placements outside the §10.5 "
                                         "offset window — wrong power space");
        }
    }
    tx.estimate_airtime = [&](size_t bytes, bool include_pending,
                              uint16_t packet_budget) {
        return air.value->estimate_airtime_us(bytes, include_pending,
                                              packet_budget);
    };
    // §10.5 (Pass 150): the relative actuator is backend-agnostic — each
    // backend resolves the offset against its own calibrated reference.
    tx.apply_power_offset = [&](size_t idx, int32_t qdb) {
        return air.value->set_power_offset_qdb(idx, qdb);
    };
    if (air.value->is_radio()) {
        tx.apply_mode = [&](uint8_t mcs, bool sgi) {
            air.value->set_tx_mode(mcs, sgi);
        };
        // §9.4 Pass 163: fail-closed — the hook exists only when the
        // operator armed air.mcs_probe on this stage-0-proven unit.
        if (l.cfg.air.mcs_probe) {
            tx.apply_probe = [&](uint16_t period, uint16_t slot,
                                 uint8_t mcs) {
                air.value->set_mcs_probe(period, slot, mcs);
            };
        }
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
    // §3.15 word while the feed is paused (Pass 153): with no live slots the
    // prepend below never fires, so a plain 2 Hz timer keeps the operator's
    // only calibration progress view alive.
    uint64_t next_paused_word_ms = 0;
    const auto send_raw = [&](const uint8_t* f, size_t n) {
        // Pass 110 operator boundary: the 2 Hz selector summary owns no TX
        // opportunity. When due, prepend it inside an already-active live RTP
        // slot; video still ends the slot and alone arms the §7.2 quiet gap.
        const bool live_video_slot = frame_is_live_rtp_data(f, n);
        if (live_video_slot) {
            const uint64_t slot_ms = now_us_it / 1000;
            if (selector_state_cadence.due(live_video_slot, slot_ms)) {
                const SelectorState state = tx.selector_state(slot_ms);
                // §10.6 Pass 120: the calib word makes this 36 bytes; the
                // encoder returns the actual size for whichever flags are set.
                uint8_t sf[kSelectorStateCalibSize];
                const size_t sn = encode_selector_state(state, sf, sizeof(sf));
                if (sn != 0) {
                    (void)selector_state_cadence.note_submitted(
                        air.value->inject(sf, sn), slot_ms);
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
    // §3.16 (Pass 153): probes and tallies are control-plane — plain inject,
    // never the §7.2 held queue (they must not arm or ride quiet gaps).
    tx.send_calib = [&](const uint8_t* f, size_t n) {
        air.value->inject(f, n);
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
        h.link_mtu_json = [&] {
            return std::string("{\"mode\":\"remote\",\"requested\":") +
                   std::to_string(tx.mtu_requested()) +
                   ",\"effective\":" + std::to_string(tx.mtu_effective()) +
                   ",\"supported\":" + std::to_string(tx.mtu_supported()) +
                   "}";
        };
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
            // §10.5 (Pass 150): bounded and REJECTED, not clamped — the
            // operator learns instead of wondering why nothing moved.
            if (!tx.set_power_override(qdb))
                return "qdb exceeds power_offset_max_qdb on a role:\"tx\" "
                       "adapter (§10.5); raise the bound to opt in";
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
            // The last persisted artifact serves whenever the current run
            // has none to offer (boot-loaded, or a later run failed).
            if (art == nullptr && last_artifact) art = &*last_artifact;
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
            s += l.cfg.air.kind == AirCfg::Kind::kRadio ? "radio" : "udp";
            s += "\"}";
            return s;
        };
        // §10.3/§11.7 0x0A (Pass 135). A craft has no bound craft of its own,
        // so `both` is a 409 rather than a silently local-only apply.
        h.tx_power_tier_json = [&] {
            return power_tier_json(tx.power_tier(), tx.power_presets(),
                                   tx.power_tier_ceiling(),
                                   tx.power_tier_effective());
        };
        h.tx_power_tier_set = [&](int tier, bool both)
            -> std::pair<int, std::string> {
            if (both) {
                return {409, std::string("{\"ok\":false,\"error\":\"both "
                                         "requires a bound craft — this node "
                                         "is the craft\"}")};
            }
            // Checked before the call so the operator gets the reason. Both
            // refusals are a false return, and reporting "no preset at that
            // index" for a running sweep would send someone to edit config
            // over what is a wait-and-retry (§10.3 Pass 136).
            if (tx.calibrating()) {
                return {409,
                        std::string("{\"ok\":false,\"error\":\"calibration "
                                    "running — abort it first (§10.6)\"}")};
            }
            if (!tx.set_power_tier(static_cast<uint8_t>(tier))) {
                return {409,
                        std::string("{\"ok\":false,\"error\":\"no power "
                                    "preset at that index "
                                    "(adapters[].power_presets_qdb, or "
                                    ".power_offset_presets_qdb on a relative "
                                    "backend)\"}")};
            }
            return {200, std::string("{\"ok\":true}")};
        };
        h.fec = [&](int sid, int ip, int pp, int mk, int mr,
                    std::optional<uint16_t> ep) -> std::string {
            if (sid < 0 || sid > 255) return "bad stream_id";
            if (ip < 0 || ip > 4000 || pp < 0 || pp > 4000 || mk < 1 ||
                mr < 0 || mr > 255)
                return "bad fec rates (0..4000 permille, min_k>=1, min_r 0..255)";
            // §14.1a: the control server already range-checked e_permille;
            // nullopt here means "inherit p_permille" (full replacement).
            return tx.set_stream_fec(static_cast<uint8_t>(sid),
                                     static_cast<uint16_t>(ip),
                                     static_cast<uint16_t>(pp),
                                     static_cast<uint16_t>(mk),
                                     static_cast<uint16_t>(mr), ep)
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
            tx.reset_negotiated_mtu();
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
                tx.reset_negotiated_mtu();
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
    // §10.5 (Pass 150): forced safe boot offset on every role:"tx" adapter,
    // applied before the first frame goes out — a node must never transmit at
    // the uncharacterised efuse default even briefly.
    tx.apply_boot_power_offsets();
    std::fprintf(stderr, "tx: session=%u, running%s\n", session,
                 qg.enabled() ? " (quiet-gap pacing)" : "");
    std::optional<uint16_t> prior_bound_issuer;
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
                    // §9.3a: every newly authenticated claim starts at
                    // Default, including a rebooted ground that reuses the
                    // same numeric originator. The new owner may reassert its
                    // local capability only after this claim commits.
                    tx.reset_negotiated_mtu();
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
            // meta.rssi / meta.rx_mcs feed §3.16's cumulative counters at the
            // accepted-LINK_REPORT point inside on_air.
            if (tx.on_air(d, n, service_now, meta.rssi, meta.rx_mcs)) {
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
        // service_air stamps accepted packets with a fresh µs clock. Refresh
        // the loop timestamp before ticking the follower so unsigned age
        // arithmetic cannot observe time moving backwards and expire a fresh
        // binding immediately.
        now_us_it = now_us();
        // Resolve CSA/binding deadlines before framing any newly arrived
        // video. In particular, a binding release must restore Default before
        // another block can be constructed with the previous owner's jumbo
        // budget.
        const CsaAction ca = csa.tick(now_us_it);
        if (ca.kind != CsaAction::Kind::kNone) {
            const bool retuned =
                air.value->retune_all(ca.chan_mhz, ca.bw, ca.fast);
            if (!retuned) {
                std::fprintf(stderr, "csa: retune to %u MHz FAILED\n",
                             ca.chan_mhz);  // Pass 69: never silent
            } else {
                tx.reassert_power();  // §10.5: retune may reset power
                tx.on_rf_environment(ca.chan_mhz, ca.bw, now);
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
        // §9.3a: jumbo authority belongs to the current claim. Losing or
        // changing that claim restores the compatibility tier before ingress.
        const std::optional<uint16_t> bound_now = csa.latched_issuer();
        if (prior_bound_issuer && bound_now != prior_bound_issuer) {
            tx.reset_negotiated_mtu();
            std::fprintf(stderr, "mtu: claim released/changed -> Default\n");
        }
        prior_bound_issuer = bound_now;
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
        // §3.16 (Pass 153) input-starve: while a calibration direction runs,
        // the video input is not read at all — ring event fds stay out of the
        // wait set, UDP ingress datagrams are consumed and dropped, and the
        // frame-shm drain below is skipped. venc keeps encoding; frames drop
        // at the ring (drop-not-block). Resume is the same edge as the
        // rate/power restore (own run) or the probe-quiet expiry (D-C).
        const bool feed_paused = tx.feed_paused();
        std::vector<int> ready_fds = air.value->wait_fds();
        const size_t air_fd_count = ready_fds.size();
        for (const ShmIn& si : shm_ins) {
            if (si.ring && !feed_paused) {
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
        if (!feed_paused) {
            for (const ShmIn& si : shm_ins) shm_pending |= si.pending;
        }
        const int in_timeout =
            held.empty() && !air.value->tx_pending() && !shm_pending ? 2 : 0;
        bindings.value->poll_once(
            in_timeout,
            [&](const IngressEvent& ev) {
                if (feed_paused) return;  // Pass 153: consumed and dropped
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
            if (feed_paused) break;  // Pass 153: leave pending for resume
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
        // §3.15 word while paused (Pass 153): no live slots exist, so the
        // 2 Hz summary gets its own timer instead of the send_raw prepend.
        if (feed_paused) {
            const uint64_t nowp = now_us_it / 1000;
            if (nowp >= next_paused_word_ms) {
                const SelectorState st = tx.selector_state(nowp);
                uint8_t sf[kSelectorStateCalibSize];
                const size_t sn = encode_selector_state(st, sf, sizeof sf);
                if (sn != 0) (void)air.value->inject(sf, sn);
                next_paused_word_ms = nowp + 500;
            }
        }
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
        if (csa_liveness_deadline_ms && now >= *csa_liveness_deadline_ms) {
            if (air.value->rx_frames_total() == csa_liveness_rx_baseline) {
                std::fprintf(stderr,
                             "csa: RX SILENT %u ms after retune to %u MHz — "
                             "half-applied retune, monitor re-init (§11.6 "
                             "Pass 80)\n",
                             l.cfg.policy.csa.rx_liveness_ms,
                             csa_liveness_chan);
                air.value->recover_all(csa_liveness_chan, csa_liveness_bw);
                // §10.5: a recovery can reset hardware power (Pass 48) — put
                // the latch / resolved curve value back regardless.
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
            // §9.10 v2 (Pass 148). This adapter IS the video transmitter, so a
            // sustained wedge means the link is already dead — the restart
            // costs nothing that is not already lost. Nothing weaker recovers
            // it (Pass 147: the USB re-enumeration does not heal the dead
            // libusb handle, recover() never touches the TX path, and a second
            // InitWrite terminates the process), so exit and let the
            // supervisor re-exec. Deliberately absent from the ground loop,
            // where the wedged adapter is the uplink and RX/video still work.
            if (l.cfg.air.wedge_exit_windows != 0 &&
                wedge.consecutive_wedged() >= l.cfg.air.wedge_exit_windows) {
                std::fprintf(stderr,
                             "air: TX WEDGED for %u consecutive windows — "
                             "exiting for supervisor re-exec (§9.10 v2)\n",
                             wedge.consecutive_wedged());
                return kExitTxWedged;
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
    // §10.7 (Pass 125): commit the uplink operating point. Before this an rx
    // node never called set_tx_mode at all and rode the TxRate struct default;
    // the seeds match it, so this changes no bytes on air. What it buys is
    // that the rung is asserted — the §10.7 artifact records it and §3.16's
    // last_rx_mcs cross-checks it, and neither is meaningful against a
    // default nobody chose.
    air.value->latch_uplink_rate(l.cfg.air.uplink_mcs, l.cfg.air.uplink_sgi);

    // §10.7 (Pass 125): the ground's single designated uplink adapter is the
    // only local power actuator. Config load already guarantees a power_map
    // can only sit on a role:"tx" adapter, and the radio backend guarantees
    // there is at most one.
    const AdapterCfg* uplink_adapter = nullptr;
    size_t uplink_idx = 0;
    for (size_t i = 0; i < l.cfg.adapters.size(); ++i) {
        if (l.cfg.adapters[i].role == Role::kTx) {
            uplink_adapter = &l.cfg.adapters[i];
            uplink_idx = i;
            break;
        }
    }
    // §10.3/§11.7 0x0A (Pass 135): the ground's live ceiling. Seeded from the
    // adapter's boot max_power_qdb and moved by a power tier, so the three
    // sites that clamp against it — the §10.7 sweep bound, the power-map
    // resolve, and the artifact apply — all follow one selection.
    // §10.3/§10.5/§10.7/§11.7 0x0A: one owner for the uplink's power, holding
    // the ceiling a tier moves and the single precedence path every apply site
    // runs through. Its actuators are wired below, once the radio exists.
    UplinkPower upwr;
    upwr.mcs = l.cfg.air.uplink_mcs;
    // §10.5/§10.7 (Pass 151): the ground's power space, decided ONCE from the
    // backend kind and read by the §10.7 sweep window, its actuator, and the
    // owner's apply alike. Pass 150's second review found this half-converted
    // — the sweep measured absolute while the applier commanded relative, so
    // an 84 qdb placement became 108+84 = 192 — so measurement and actuation
    // move together here or not at all.
    const bool uplink_relative = l.cfg.air.kind == AirCfg::Kind::kRadio;
    if (uplink_adapter != nullptr) {
        // §10.3/§11.7 0x0A (Pass 166): the ceiling and the preset list come
        // from the uplink's OWN actuation space. Seeding the absolute pair on
        // a relative node was the Pass 165 residual: `hw_qdb()` and the
        // artifact resolve both clamp to `ceiling_qdb`, and a 108 there is a
        // no-op against offsets, so neither clamp bound anything. This is the
        // single place the space is chosen; everything downstream —
        // `set_tier`, §15.3 `tx_power_ceiling_qdb`, §15.5, the sweep-bound
        // fold — reads `upwr` and needs no branch of its own.
        upwr.ceiling_qdb =
            uplink_relative
                ? std::optional<int32_t>(uplink_adapter->power_offset_max_qdb)
                : uplink_adapter->max_power_qdb;
        upwr.presets_qdb = uplink_relative
                               ? uplink_adapter->power_offset_presets_qdb
                               : uplink_adapter->power_presets_qdb;
    }
    // The policy-level sweep bound, kept so a runtime tier can rebuild
    // ucal_params.seek.max_qdb from the same two inputs the startup fold used.
    const int32_t cp_max_qdb =
        static_cast<int32_t>(l.cfg.policy.calibration.max_qdb);
    if (uplink_adapter != nullptr && !uplink_adapter->power_map.empty()) {
        auto curve = load_power_curve(uplink_adapter->power_map,
                                      uplink_adapter->channel_mhz >= 5000);
        if (curve) {
            // Retained rather than scoped to the load: a tier moves the
            // ceiling this resolve is clamped by, so the resolve has to be
            // repeatable (§10.2 — the authored curve IS level 4).
            upwr.curve = *curve.value;
            upwr.resolve_owner();
            if (upwr.owner_qdb) {
                std::fprintf(stderr,
                             "uplink: power_map mcs=%u -> %d qdb (explicit)\n",
                             l.cfg.air.uplink_mcs, *upwr.owner_qdb);
            }
        } else {
            std::fprintf(stderr, "uplink: power_map load failed: %s\n",
                         curve.error.c_str());
        }
    }

    auto bindings = BindingSet::create(l.cfg);
    if (!bindings) {
        std::fprintf(stderr, "binding error: %s\n", bindings.error.c_str());
        return 1;
    }
    const uint32_t session = session_nonce();
    DiscoveryCatalog discovery;
    RxCore rx(l.cfg, session, l.have_table ? &l.table : nullptr,
              l.have_table ? std::optional<uint8_t>(l.tv) : std::nullopt);

    // §3.16 (Pass 153): the craft adapter fingerprint arrives on TALLYs now
    // (D-A ruling) — 0 until the first run's first tally this session.
    uint8_t craft_tally_fp = 0;
    uint32_t selected_craft_session = 0;
    UplinkCalibParams ucal_params;
    {
        const CalibrationPolicy& cp = l.cfg.policy.calibration;
        ucal_params.seek = Calibrator::seek_params(calib_params_from(cp));
        if (uplink_relative && uplink_adapter != nullptr &&
            uplink_adapter->power_offset_max_qdb >
                uplink_adapter->power_offset_qdb) {
            // §10.7 (Pass 151): offset space, derived from the same §10.5 keys
            // that boot this uplink — so the sweep climbs from the safe offset
            // and stops at the bound. The §10.3 absolute ceiling below is not
            // a comparable quantity here and is deliberately not folded in.
            ucal_params.seek.min_qdb = uplink_adapter->power_offset_qdb;
            ucal_params.seek.max_qdb = uplink_adapter->power_offset_max_qdb;
            ucal_params.seek.seek_step_qdb = cp.offset_seek_step_qdb;
            // §10.5 (Pass 153): the window may extend above the efuse
            // reference, but an unbracketed placement never does.
            ucal_params.seek.no_bracket_cap_qdb = 0;
            ucal_params.taper_rung_ceiling = false;
        } else if (upwr.ceiling_qdb) {
            // §10.3 (Pass 134): the adapter's opt-in sanity ceiling bounds the
            // sweep, not only the §10.7 apply below. The ground half carries
            // the stronger case for it — this placement auto-applies at boot
            // with no operator between the measurement and the actuator.
            ucal_params.seek.max_qdb =
                std::min(ucal_params.seek.max_qdb, *upwr.ceiling_qdb);
        }
        ucal_params.settle_ms = static_cast<uint32_t>(cp.settle_ms);
        ucal_params.dwell_probe_frames =
            static_cast<uint16_t>(cp.dwell_probe_frames);
        ucal_params.dwell_verify_frames =
            static_cast<uint16_t>(cp.dwell_verify_frames);
        ucal_params.dwell.probe_pace_us =
            static_cast<uint32_t>(cp.probe_pace_us);
        ucal_params.dwell.tally_wait_ms =
            static_cast<uint32_t>(cp.tally_wait_ms);
        ucal_params.dwell.tally_retries =
            static_cast<uint32_t>(cp.tally_retries);
        ucal_params.hard_cap_ms = static_cast<uint32_t>(cp.hard_cap_ms);
        // §10.7 (Pass 153): THE rung is the configured operating point; its
        // rate identity and §10.3 taper level come from the §9.3 table row
        // that carries it, so a deployment that authored a different table
        // agrees without a code change (core/ keeps no table dependency).
        ucal_params.rate =
            UplinkRate{l.cfg.air.uplink_mcs, l.cfg.air.uplink_sgi};
        if (l.have_table) {
            for (const Profile& pr : l.table.profiles) {
                if (pr.mcs == l.cfg.air.uplink_mcs &&
                    (pr.gi == GuardInterval::kShort) ==
                        l.cfg.air.uplink_sgi) {
                    ucal_params.rung_level = pr.tx_power_level;
                    break;
                }
            }
        }
    }
    UplinkCalibrator uplink_cal(ucal_params);
    // The placement for the rung the uplink actually transmits at — since
    // Pass 153 the only rung measured; the lookup shape survives so an older
    // eight-entry artifact resolves the matching entry.
    const auto uplink_measured_qdb = [&]() -> std::optional<int32_t> {
        for (const UplinkPlacement& pl : uplink_cal.placements()) {
            if (pl.mcs == l.cfg.air.uplink_mcs &&
                pl.short_gi == l.cfg.air.uplink_sgi) {
                return pl.placement_qdb;
            }
        }
        return std::nullopt;
    };
    CalibSequencer calib_seq;
    const uint16_t op_chan =
        l.cfg.adapters.empty() ? 0 : l.cfg.adapters[0].channel_mhz;
    const uint8_t op_bw_mhz =
        l.cfg.adapters.empty() ? 20 : l.cfg.adapters[0].bw;

    struct LinkSelection {
        uint16_t originator = 0;
        uint16_t chan = 0;
        uint8_t bw = 0;  // §11 width code
        // §3.0: nullopt is "unconfigured — accept any net_id", which is a
        // distinct state from 0 and must survive a claim, a rollback and a
        // §15.5a sweep. Collapsing it to 0 here made an unconfigured node deaf
        // to every non-zero net_id the moment a sweep rested.
        std::optional<uint8_t> net_id;
    };
    LinkSelection active_selection{rx.selected_originator().value_or(0),
                                   op_chan, bw_code(op_bw_mhz),
                                   l.cfg.node.net_id};
    std::optional<LinkSelection> pending_selection;
    std::optional<LinkSelection> previous_selection;
    std::string selection_state = "configured";
    std::string previous_selection_state = selection_state;

    // §15.5a (Pass 65): the ground's current operating channel — the config
    // default until a claim commits, then the committed target. The scout returns
    // all ears here, and a failed claim rolls back here.
    uint16_t operating_chan = op_chan;
    // Tier 2: a persisted artifact, applied only when the local adapter, the
    // craft, and the band/bandwidth all match. A mismatch is surfaced as
    // stale and never applied — the hardware stays at the higher-precedence
    // source (§10.7). This block sits after the pairing tuple it needs
    // (`active_selection`, `quality_gate`, `operating_chan`) rather than up
    // with the config tier, because the CRAFT half of the identity does not
    // exist at config-load time.
    // §10.6 (Pass 154): resolved against the live per-unit EFUSE MAC the
    // backend read at bring-up. Empty = identity-less unit on the radio
    // backend — §10.7 runs are refused (D3) and no stored artifact can match.
    const std::string uplink_identity =
        uplink_adapter != nullptr
            ? calib_identity(*uplink_adapter, l.cfg.air.kind,
                             air.value->adapter_mac(uplink_idx))
            : "udp";
    if (uplink_adapter != nullptr && uplink_identity.empty()) {
        std::fprintf(stderr,
                     "uplink: adapter \"%s\" reports no EFUSE identity — "
                     "§10.7 calibration and any absolute curve are REFUSED "
                     "(Pass 154 D3); running at the §10.5 safe boot offset\n",
                     uplink_adapter->name.c_str());
    }
    // §3.16 (Pass 153): the ground's receiver half (craft §10.6 downlink
    // probes → tallies) and the probe-exchange observability counters.
    DwellReceiver dcal_rx;
    const uint8_t ground_ident_fp = crc8_dvbs2(
        reinterpret_cast<const uint8_t*>(uplink_identity.data()),
        uplink_identity.size());
    uint64_t ucal_probes_tx = 0;
    uint64_t ucal_tallies_rx = 0;
    uint64_t ucal_tallies_tx = 0;
    uint8_t ucal_rx_mcs = kUplinkRxMcsUnknown;
    std::optional<UplinkArtifact> uplink_artifact;
    uint8_t uplink_artifact_fp = 0;
    bool uplink_artifact_stale = false;
    if (auto stored = uplink_identity.empty()
                          ? Result<UplinkArtifact>::fail("no identity (D3)")
                          : uplink_calib_store_load(
                                l.cfg.policy.calibration.artifact_dir);
        stored) {
        uplink_artifact_fp = uplink_calib_fingerprint(*stored.value);
        if (stored.value->local_adapter_identity != uplink_identity) {
            uplink_artifact_stale = true;
            std::fprintf(stderr,
                         "uplink: artifact STALE (stored %s, live %s)\n",
                         stored.value->local_adapter_identity.c_str(),
                         uplink_identity.c_str());
        }
        uplink_artifact = std::move(*stored.value);
    }
    // The craft's adapter fingerprint as the ARTIFACT records it: the RX
    // chain the placement was measured against. Since Pass 153 it arrives on
    // the run's §3.16 TALLYs (D-A) — so before any run this session it is 0
    // = UNKNOWN, and the artifact resolve below defers the craft-adapter
    // check to the rest of the tuple; the first tally with a different
    // fingerprint flips the artifact STALE through the pairing key.
    const auto uplink_craft_fp = [&]() -> uint8_t { return craft_tally_fp; };
    const auto uplink_artifact_qdb = [&]() -> std::optional<int32_t> {
        if (!uplink_artifact) return std::nullopt;
        // §10.7: "Apply it only when the same craft is selected and both local
        // and remote adapter identities match; otherwise surface stale and
        // leave hardware at the higher-precedence source." The local half was
        // checked at load; the rest cannot be, so the FULL tuple is checked
        // here, at every resolve — the same tuple the writer stamps in.
        const uint8_t live_fp = uplink_craft_fp();
        const uint8_t fp_for_match =
            live_fp != 0 ? live_fp
                         : uplink_artifact->craft_adapter_fingerprint;
        if (!uplink_calib_matches(*uplink_artifact, uplink_identity,
                                  active_selection.originator, fp_for_match,
                                  operating_chan, op_bw_mhz)) {
            uplink_artifact_stale = true;
            return std::nullopt;
        }
        uplink_artifact_stale = false;
        const UplinkPlacement* p = uplink_calib_placement_for(
            *uplink_artifact, l.cfg.air.uplink_mcs, l.cfg.air.uplink_sgi);
        if (p == nullptr) return std::nullopt;
        int32_t q = p->placement_qdb;
        // §10.3 ceiling still clamps a stored placement — the artifact
        // records what was measured, the ceiling is what the operator allows.
        if (upwr.ceiling_qdb) {
            q = std::min(q, *upwr.ceiling_qdb);
        }
        return q;
    };
    // Wired now that the pairing-dependent resolve above exists. §10.7
    // applicability changes with selection and CSA, so it stays a callback
    // rather than a value the owner caches.
    upwr.artifact_qdb = uplink_artifact_qdb;
    // Applicability is a FUNCTION of that tuple, and every existing restore
    // call site fires before the tuple is knowable (startup), or on an event
    // unrelated to it (§10.5 unlatch, calibration exit). Without a re-resolve
    // on tuple change a valid artifact loaded at boot would never be applied
    // at all. Seeded with the startup tuple so the first pass is not a
    // spurious actuation.
    uint64_t uplink_pairing_key = 0;
    uint32_t uplink_last_dwell_seq = 0;
    // The craft a running §10.7 measurement is bound to. The artifact stamps
    // the craft identity, so a selection change mid-run would persist a
    // placement measured partly against a different craft's RX chain.
    uint16_t uplink_cal_craft = 0;
    const auto uplink_pairing_now = [&]() -> uint64_t {
        return (static_cast<uint64_t>(active_selection.originator) << 32) |
               (static_cast<uint64_t>(uplink_craft_fp()) << 24) |
               static_cast<uint64_t>(operating_chan);
    };
    // §10.5 (Pass 125) override latch, now available on any node with a
    // role:"tx" adapter. It outranks every resolved tier — that is what makes
    // it the manual counterpart to §10.7 and the reference placement for the
    // calibrated-versus-manual comparison, with no config edit and restart.
    // §10.7 restore. ONE convergence path for every borrowed actuator, in R4
    // order — RATE first, then power — because the rung a placement was
    // measured at is what makes the power meaningful, and the same reasoning
    // that put power before the selector pin in §10.6 puts rate before power
    // here. Pass 131 added the rate: the eight-rung sweep commands
    // `set_tx_mode` per rung, so a run that exits at rung 5 would otherwise
    // leave the uplink transmitting at MCS5 with a rung-0 power on it.
    //
    // Power precedence now lives in UplinkPower::apply() — ONE copy, which is
    // what this comment used to warn about having four of.
    const auto uplink_restore_actuators = [&]() {
        if (uplink_adapter == nullptr) return;
        air.value->latch_uplink_rate(l.cfg.air.uplink_mcs,
                                     l.cfg.air.uplink_sgi);
        upwr.apply();
    };
    // The actuators the owner drives. §10.5: "the §10.3 max_power_qdb ceiling
    // — when configured — is the only *clamp*: the hardware receives
    // min(qdb, max_power_qdb) per adapter, while GET/§15.3 report the latched
    // request value." UplinkPower::hw_qdb() applies that clamp;
    // reported_qdb() deliberately does not.
    // §10.5 (Pass 150): the uplink is a role:"tx" adapter like any other, so
    // it runs the same relative contract.
    // §10.5/§10.7 (Pass 150 review, re-based Pass 151): every value the owner
    // precedence can produce — artifact placement, power_map resolve, latch —
    // is in whatever space the §10.7 sweep measured in, so the applier reads
    // the same `uplink_relative` the sweep window did. Converting one without
    // the other turned an 84 qdb placement into 108+84 = 192 qdb.
    upwr.apply_qdb = [&](int32_t q) {
        if (uplink_relative) {
            (void)air.value->set_power_offset_qdb(uplink_idx, q);
            return;
        }
        (void)air.value->set_power_qdb(uplink_idx, q);
    };
    // "auto" must land on the configured safe offset, never the backend
    // default (offset 0 = the uncharacterised efuse point).
    upwr.apply_auto = [&] {
        if (uplink_adapter != nullptr) {
            (void)air.value->set_power_offset_qdb(
                uplink_idx, uplink_adapter->power_offset_qdb);
        }
    };
    // §10.5 forced safe boot offset, before the uplink carries anything.
    if (uplink_adapter != nullptr) {
        const bool ok = air.value->set_power_offset_qdb(
            uplink_idx, uplink_adapter->power_offset_qdb);
        std::fprintf(stderr,
                     "power: %s §10.5 boot offset %+d qdb (bound %+d) -> %s\n",
                     uplink_adapter->name.c_str(),
                     static_cast<int>(uplink_adapter->power_offset_qdb),
                     static_cast<int>(uplink_adapter->power_offset_max_qdb),
                     ok ? "applied" : "NOT APPLIED");
    }
    uplink_restore_actuators();
    uplink_pairing_key = uplink_pairing_now();
    if (uplink_adapter != nullptr) {
        // One line naming which tier actually owns the actuator. The
        // precedence is invisible otherwise, and "why is my power_map not
        // being applied" is exactly the question this answers.
        const std::optional<int32_t> aq = uplink_artifact_qdb();
        // Names the owner from the SAME precedence the actuator ran through,
        // so the log cannot describe a different node than the radio is on.
        std::fprintf(stderr, "uplink: power owner = %s", upwr.owner_name());
        if (upwr.owner_qdb) {
            std::fprintf(stderr, " (%d qdb)", *upwr.owner_qdb);
        } else if (aq) {
            std::fprintf(stderr, " (%d qdb, fp=0x%02x)", *aq,
                         uplink_artifact_fp);
        } else if (uplink_artifact && active_selection.originator == 0) {
            // Not stale in the operator sense — nothing is selected yet, so
            // the pairing simply cannot be evaluated. Saying STALE here would
            // send people looking for a mismatch that does not exist.
            std::fprintf(stderr, " (artifact present, awaiting craft)");
        } else if (uplink_artifact_stale) {
            std::fprintf(stderr, " (artifact present but STALE)");
        }
        std::fprintf(stderr, "\n");
    }

    // One place that turns live §10.7/§3.16 state into the §15.3 fields, so
    // the stats line and GET /api/v1/calibration cannot describe the node
    // differently.
    const auto uplink_fill = [&]() {
        UplinkStatsFill u;
        switch (uplink_cal.state()) {
            case CalibState::kIdle: u.state = "idle"; break;
            case CalibState::kRunning: u.state = "running"; break;
            case CalibState::kDone: u.state = "done"; break;
            case CalibState::kFailed: u.state = "failed"; break;
        }
        // Pass 131: the rung is live progress through the eight-rung sweep,
        // the same quantity §3.15's calibration word carries for §10.6.
        u.rung = uplink_cal.rung();
        u.power_qdb = uplink_cal.state() == CalibState::kRunning
                          ? uplink_cal.qdb()
                          : uplink_measured_qdb().value_or(0);
        u.fingerprint = uplink_artifact_fp;
        u.stale = uplink_artifact_stale;
        // §10.3/§10.5/§11.7 0x0A. This used to warn that it mirrored
        // uplink_restore_actuators() and had to be edited in step with it.
        // There is nothing to keep in step now — both read one object.
        u.tx_power_tier = upwr.tier;
        u.tx_power_ceiling_qdb = upwr.ceiling_qdb.value_or(0);
        u.tx_power_tier_effective = upwr.effective();
        u.tx_power_override = upwr.override_qdb.has_value();
        // §10.5: §15.3 reports the latched REQUEST, not the clamped value the
        // hardware got — reported_qdb() is the half of the split that does
        // not apply the ceiling. The craft half does the same.
        u.tx_power_qdb = upwr.reported_qdb().value_or(0);
        // §3.16 (Pass 153) probe-exchange counters.
        u.probes_sent = ucal_probes_tx;
        u.tallies_rx = ucal_tallies_rx;
        u.rx_mcs = ucal_rx_mcs;
        return u;
    };

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
        // §14.3 wire field is a plain uint8_t — a cache assignment carries no
        // "any net_id" encoding, so an unconfigured link assigns 0.
        a.target_net_id = selected.net_id.value_or(0);
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
        // §3.16 scopes its accept to the exact (originator, session) of the
        // stream we consume, and the session is LEARNED from that craft's
        // DATA. Carrying the previous craft's session across a selection
        // change leaves the gate scoped to a tuple that cannot exist, while
        // the §10.7 "no craft selected" prerequisite reads a non-zero value
        // and lets a start through against feedback that will never arrive.
        if (selected.originator != active_selection.originator) {
            selected_craft_session = 0;
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
    // §3.5/§10.7: the epoch is stamped HERE, at the radio call, and advances
    // only on a successful submit. Reports are built well before they are
    // injected (§7.2 holds a batch for the craft's quiet gap) and can be
    // dropped in between — an epoch spent on a frame the radio never took is
    // phantom loss on the ground's §10.7 seek, because the craft's
    // last_report_epoch delta IS that seek's denominator.
    const auto send_report = [&](uint16_t target, uint8_t* f, size_t n) {
        (void)link_report_stamp_epoch(f, n, rx.next_report_epoch());
        if (air.value->inject_return(target, f, n, false) != 0) {
            rx.commit_report_epoch();
        }
    };
    const RxCore::Inject inject_report = [&](const uint8_t* f, size_t n,
                                             uint16_t target) {
        if (!qg.enabled()) {
            uint8_t tmp[kLinkReportSize];
            if (n > sizeof(tmp)) {
                // Was a bare `return` — a dropped LINK_REPORT with no counter
                // and no log, on the stream §10.6 scores its dwells from.
                std::fprintf(stderr,
                             "return: LINK_REPORT %zu B exceeds %zu B, "
                             "dropped\n",
                             n, sizeof(tmp));
                ++ret_window_misses;
                return;
            }
            std::memcpy(tmp, f, n);
            send_report(target, tmp, n);
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
    // §9.3a: Automatic is local only. A successfully-created RadioAir means
    // every active adapter is a supported Realtek and may advertise High;
    // unknown backends resolve conservatively to Default.
    std::string mtu_mode = "default";
    const uint16_t mtu_supported = air.value->mtu_supported();
    uint16_t mtu_requested = kDefaultMaxPayload;
    uint16_t mtu_effective = kDefaultMaxPayload;
    bool mtu_reissue_pending = false;
    std::function<std::pair<int, std::string>(const std::string&, int)>
        start_vehicle_command;
    // §10.7: "abort, process shutdown, retune conflict, and failure must never
    // leave the last probe power active." ONE convergence path for every
    // cancel — the alternative is a restore call per exit site, and the sites
    // that get forgotten are exactly the ones nobody exercises. Placed after
    // start_vehicle_command's declaration because the downlink phase has to
    // be cancelled over air.
    const auto cancel_calibration = [&](const char* why) {
        if (uplink_cal.state() != CalibState::kRunning && !calib_seq.active()) {
            return;
        }
        const SeqActions sa = calib_seq.abort(now_ms());
        (void)uplink_cal.abort(now_ms());
        // Drain the single-shot restore edge here rather than leaving it to
        // the service loop: shutdown never reaches the loop again.
        const UplinkCalibActions ua =
            uplink_cal.tick(now_ms());
        if (ua.restore) uplink_restore_actuators();
        // Empty on a cache-assignment node, which never gets the issuer
        // handlers — calling it unguarded would throw bad_function_call.
        if (sa.abort_downlink && start_vehicle_command) {
            (void)start_vehicle_command("calibrate", 0);
        }
        std::fprintf(stderr, "uplink-calib: CANCELLED (%s)\n", why);
    };
    // §9.10: the ground's designated uplink TX adapter gets the same
    // CCX-liveness watchdog as the craft's radio.
    TxWedge wedge(TxWedgePolicy{l.cfg.air.wedge_window_ms,
                                l.cfg.air.wedge_min_submits});
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
            // §15.5a (Pass 155): frame-free sense of the scout adapter;
            // nullopt on sensor-less backends.
            [&](size_t a) { return air.value->iface()->rx_sense(a); },
            // §15.5a (Pass 161): calibration-domain key + §3.16 verdict.
            [&, scout_idx]() -> std::string {
                const std::string m =
                    air.value->iface()->adapter_mac(scout_idx);
                return m.empty() ? "idx/" + std::to_string(scout_idx)
                                 : "mac/" + m;
            },
            [&]() -> uint8_t {
#if WBLINK_RADIO
                if (air.value->radio != nullptr) {
                    return air.value->radio->link_verdict();
                }
#endif
                return 0;
            },
        },
        op_bw_mhz, op_chan, l.cfg.node.net_id, scout_idx);
    // §15.5 REST control plane. RX/ground node owns the CSA trigger (replaces
    // the removed stdin trigger); profile/fec are TX-only knobs → null → 409.
    StatsSnapshot last_snap;
    std::unique_ptr<ControlServer> control;
    // §11.7 issuer, held as a NAMED local rather than reached through `h`,
    // because ControlHandlers is std::move()d into the server once every
    // handler is registered, so a lambda that captures `h` by reference and
    // dereferences a sibling member at CALL time reads the moved-from husk.
    //
    // It is declared HERE, beside `control`, and NOT inside the block below
    // (Pass 166). The server outlives that block and dispatches from the main
    // loop, so a handler capturing a block-scoped local by reference reads a
    // DESTROYED std::function — ASan `stack-use-after-scope`, and plain UB in
    // a release build. Device-caught on the .242 ground the first time
    // §11.7 0x0A `both:true` became reachable on an RF node; `main` at
    // 02d6145 aborts identically on a udp-air uplink, so the bug predates
    // Pass 166 and was merely shielded by Pass 165's blanket 409.
    std::function<std::pair<int, std::string>(const std::string&, int)>
        issue_vcmd;
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
        h.scout_results = [&] { return scout.results_json(now_ms()); };
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
                              std::to_string(active_selection.net_id.value_or(0)) +
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
        // Read-only MTU capability/state is available on every RX role,
        // including spectators and controlled caches.
        h.link_mtu_json = [&] {
            return std::string("{\"mode\":\"") + mtu_mode +
                   "\",\"requested\":" + std::to_string(mtu_requested) +
                   ",\"effective\":" + std::to_string(mtu_effective) +
                   ",\"supported\":" + std::to_string(mtu_supported) + "}";
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
            // §15.5a (Pass 144): a claim is what a sweep is *for*, so it ends
            // the sweep instead of racing it. Left running, the sweep finishes
            // seconds later and its rest() restores the resting channel and
            // net_id filter *over* the claim, with the campaign already in
            // flight — the scout clobbering the operator's click. Placed after
            // every cheap rejection, so a claim that never happens leaves a
            // running sweep alone. abandon() rather than stop(): the very next
            // lines retune every ear and re-pin the filter, so stop()'s restore
            // would be a full round trip to the resting channel and back, in
            // front of a campaign the craft is timing.
            if (scout.scanning()) {
                scout.abandon(now_ms());
            }
            // §15.5a: bind the link to the craft's net_id and move all ears onto
            // its current channel so the campaign and CSA_ARMED return are heard.
            air.value->set_stamp_net_id(cand->net_id);
            air.value->set_filter_net_id(cand->net_id);
            if (!air.value->retune_all(cand->chan, op_bw_mhz, false)) {
                air.value->set_stamp_net_id(active_selection.net_id.value_or(0));
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
                air.value->set_stamp_net_id(active_selection.net_id.value_or(0));
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
            // §10.7 (D2): scout_idx IS the role:"tx" uplink adapter, so a
            // sweep roams the calibration actuator. The liveness clock cannot
            // catch it — §3.16 keeps arriving on the diversity RX adapters
            // that stay at rest — so every dwell would score a real blackout
            // the seek blames on power and ramp to max_qdb.
            h.scout_start = [&, do_claim](const std::vector<uint16_t>& chans,
                                          uint32_t dwell, const std::string& mode,
                                          int target) -> std::string {
                if (mode == "quickconnect") {
                    if (target < 0) {
                        return "quickconnect requires target.originator";
                    }
                    // §15.5a (Pass 161) one-classifier rule: a fresh
                    // Weak/Saturated on the ACTIVE link means the impairment
                    // is range or self-jam — a channel move will not help.
#if WBLINK_RADIO
                    if (air.value->radio != nullptr) {
                        const uint8_t v = air.value->radio->link_verdict();
                        if (v == link_verdict::kWeak ||
                            v == link_verdict::kSaturated) {
                            return std::string("refused: active impairment ") +
                                   (v == link_verdict::kWeak ? "WEAK"
                                                             : "SATURATED") +
                                   " is not channel-attributable "
                                   "(§15.5a Pass 161)";
                        }
                    }
#endif
                    return do_claim(target, 0);  // pick the emptiest channel
                }
                cancel_calibration("scout sweep");
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
            // §10.5 override latch on the ground's uplink adapter.
            h.tx_power_set = [&](bool is_auto, int qdb) -> std::string {
                if (uplink_adapter == nullptr) {
                    return "no role:\"tx\" adapter on this node";
                }
                // Latching mid-run would fight the seek for the actuator, so
                // the run yields first and restores through its own single
                // convergence path — before the latch applies (§10.5).
                cancel_calibration("tx/power override latched");
                if (is_auto) {
                    upwr.override_qdb.reset();
                    uplink_restore_actuators();  // immediate, per §10.5
                    return "";
                }
                if (qdb < -511 || qdb > 511) {
                    return "qdb out of range (-511..511)";
                }
                // §10.5 (Pass 151): the bound follows the space, because the
                // latch flows into UplinkPower::apply_qdb — which is the
                // RELATIVE actuator once `uplink_relative`. Unbounded there, a
                // plain `{"qdb":84}` becomes +21 dB of offset from REST, which
                // is the hazard this whole conversion exists to remove. On an
                // absolute ground — only the udp bench since Pass 164 — the
                // value is an absolute qdb and a relative bound would reject
                // every sane one, so it stays unbounded exactly as before.
                if (uplink_relative && uplink_adapter != nullptr &&
                    qdb > uplink_adapter->power_offset_max_qdb) {
                    return "qdb exceeds power_offset_max_qdb on this "
                           "role:\"tx\" adapter (§10.5); raise the bound to "
                           "opt in";
                }
                upwr.override_qdb = qdb;
                uplink_restore_actuators();
                return "";
            };
            h.tx_power_json = [&] {
                std::string s = "{\"override_active\":";
                s += upwr.override_qdb ? "true" : "false";
                if (upwr.override_qdb) {
                    s += ",\"qdb\":" + std::to_string(*upwr.override_qdb);
                }
                s += ",\"backend\":\"";
                s += l.cfg.air.kind == AirCfg::Kind::kRadio ? "radio" : "udp";
                s += "\"}";
                return s;
            };
            // §10.7 GET: the ground's OWN uplink state. The craft response
            // keeps the §10.6 schema; `direction` is what tells a Hub which
            // of the two it is holding, since both live at this path.
            h.calibration_json = [&]() -> std::string {
                const UplinkStatsFill u = uplink_fill();
                std::string s = "{\"direction\":\"uplink\",\"phase\":\"";
                s += CalibSequencer::phase_name(calib_seq.phase());
                s += "\",\"state\":\"";
                s += u.state;
                s += "\",\"rung\":" + std::to_string(u.rung);
                s += ",\"power_qdb\":" + std::to_string(u.power_qdb);
                s += ",\"fingerprint\":" + std::to_string(u.fingerprint);
                s += ",\"stale\":";
                s += u.stale ? "true" : "false";
                s += ",\"probes\":{\"sent\":" +
                     std::to_string(u.probes_sent);
                s += ",\"tallies_rx\":" + std::to_string(u.tallies_rx);
                s += ",\"rx_mcs\":" + std::to_string(u.rx_mcs);
                s += "},\"artifact\":";
                if (uplink_artifact) {
                    s += "{\"local_adapter_identity\":\"" +
                         uplink_artifact->local_adapter_identity + "\"";
                    s += ",\"craft_originator\":" +
                         std::to_string(uplink_artifact->craft_originator);
                    s += ",\"craft_adapter_fingerprint\":" +
                         std::to_string(
                             uplink_artifact->craft_adapter_fingerprint);
                    s += ",\"channel_mhz\":" +
                         std::to_string(uplink_artifact->channel_mhz);
                    s += ",\"bw_mhz\":" +
                         std::to_string(uplink_artifact->bw_mhz);
                    s += ",\"t_unix\":" +
                         std::to_string(uplink_artifact->t_unix);
                    s += ",\"placements\":[";
                    bool first_p = true;
                    for (const UplinkPlacement& p : uplink_artifact->placements) {
                        if (!first_p) s += ",";
                        first_p = false;
                        s += "{\"mcs\":" + std::to_string(p.mcs);
                        s += ",\"short_gi\":";
                        s += p.short_gi ? "true" : "false";
                        s += ",\"placement_qdb\":" +
                             std::to_string(p.placement_qdb);
                        s += ",\"placement_rssi_dbm\":" +
                             std::to_string(p.placement_rssi_dbm);
                        s += ",\"placement_loss_milli\":" +
                             std::to_string(p.placement_loss_milli);
                        s += ",\"last_clean_qdb\":" +
                             std::to_string(p.last_clean_qdb);
                        s += ",\"first_bad_qdb\":";
                        s += p.has_first_bad ? std::to_string(p.first_bad_qdb)
                                             : "null";
                        s += "}";
                    }
                    s += "]}";
                } else {
                    s += "null";
                }
                s += ",\"fail_reason\":";
                // The sequencer's reason wins when set: it names WHICH phase
                // failed, which the uplink calibrator's cannot.
                const char* fr = calib_seq.fail_reason()
                                     ? calib_seq.fail_reason()
                                     : uplink_cal.fail_reason();
                if (fr != nullptr) {
                    s += "\"";
                    s += fr;
                    s += "\"";
                } else {
                    s += "null";
                }
                s += "}";
                return s;
            };
            // §10.7 start prerequisites. Each returns the failed one so the
            // operator sees WHY, not just 409 — the list is long and every
            // entry is a real hazard, not a formality.
            // `issue_vcmd` is declared at run_rx scope beside `control` — see
            // the comment there. It used to live here, which made every
            // handler that captured it by reference read a destroyed object
            // once this block exited (the earlier symptom was `both:true`
            // reporting "no vehicle-command path on this node" on a ground
            // that plainly had one; the sharper one is an ASan abort).
            // §10.3/§11.7 0x0A (Pass 135): the ground's own uplink ceiling,
            // and with `both` the craft's too — one action, both directions,
            // the shape {"action":"start_both"} already has for calibration.
            // `effective` means "this ceiling reaches hardware" — a curve, a
            // PAIRED artifact, or a held §10.5 latch the ceiling clamps. It is
            // UplinkPower::effective() now, so §15.5 and §15.3 cannot disagree.
            h.tx_power_tier_json = [&] { return upwr.json(); };
            h.tx_power_tier_set = [&](int tier, bool both)
                -> std::pair<int, std::string> {
                if (tier < 0 ||
                    static_cast<size_t>(tier) >= upwr.presets_qdb.size()) {
                    return {409,
                            std::string("{\"ok\":false,\"error\":\"no power "
                                        "preset at that index "
                                        "(adapters[].power_presets_qdb, or "
                                        ".power_offset_presets_qdb on a "
                                        "relative backend)\"}")};
                }
                // A running §10.7 sweep owns the uplink actuator and is
                // mid-descent against the OLD bound. Same position §10.5
                // takes on latching mid-run, and the same one the craft half
                // takes in set_power_tier().
                if (uplink_cal.state() == CalibState::kRunning) {
                    return {409,
                            std::string("{\"ok\":false,\"error\":\"uplink "
                                        "calibration running — abort it "
                                        "first (§10.7)\"}")};
                }
                // §11.7 0x0A / Pass 151, ground half (Pass 165). The craft
                // has refused this since Pass 151 (set_power_tier's
                // `if (backend_relative_) return false`); this path never
                // did, so a tier applied here and its absolute ceiling then
                // overwrote the OFFSET-space §10.7 sweep bound below — the
                // window derived correctly at startup from
                // power_offset_max_qdb. On the flying ground that moved the
                // next sweep's ceiling from +24 qdb (+6 dB) to 108 qdb read
                // as an offset (+27 dB), the compressing point Pass 150
                // measured.
                //
                // Pass 166 WITHDRAWS that refusal by re-base, not by
                // relaxation: `upwr.presets_qdb` and `ceiling_qdb` are seeded
                // from the OFFSET-space keys on a relative node, so the
                // ceiling a tier installs and the resolve it clamps are
                // finally the same quantity, and the sweep bound folded below
                // moves inside the \u00a710.5 window instead of past it. The index
                // check above carries the whole refusal now \u2014 a relative node
                // with no `power_offset_presets_qdb` has an empty list and
                // 409s there, before anything is recorded.
                //
                // `both` first: if the craft cannot be commanded, refuse the
                // whole action rather than half-applying it locally and
                // reporting an error the operator reads as "nothing happened".
                if (both) {
                    if (!issue_vcmd) {
                        return {409,
                                std::string("{\"ok\":false,\"error\":\"no "
                                            "vehicle-command path on this "
                                            "node\"}")};
                    }
                    const auto [code, body] = issue_vcmd("tx_power", tier);
                    if (code != 200) return {code, body};
                }
                // Moves the ceiling, re-resolves the configured map under it,
                // and applies — one call, because all three were separate
                // steps here and two of them were missing.
                if (!upwr.set_tier(tier)) {
                    return {409,
                            std::string("{\"ok\":false,\"error\":\"no power "
                                        "preset at that index "
                                        "(adapters[].power_presets_qdb, or "
                                        ".power_offset_presets_qdb on a "
                                        "relative backend)\"}")};
                }
                // The sweep bound is NOT power and stays the caller's: folded
                // once at startup, so without this an ABSOLUTE §10.7 run
                // started after a tier change would still climb to the BOOT
                // ceiling — the tier would bound flight power but not the one
                // operation that deliberately walks a rung into overload.
                //
                // ABSOLUTE ONLY (Pass 167, operator ruling). On a relative
                // uplink this fold is what collapsed the sweep: the tier
                // ceiling is an OFFSET (-48 at fleet tier 1) while the window
                // floor is power_offset_qdb (-24), so max < floor and the run
                // swept a single point, reported SUCCESS, and overwrote an
                // artifact that recorded last_clean_qdb 24. Measured on the
                // .242 ground, 2026-08-09; see docs/findings.md. In offset
                // space the sweep window is the §10.5 band and nothing
                // session-volatile may narrow it — that is how a unit's real
                // maximum is found. The startup fold above is already
                // branched this way; this one never was.
                //
                // It must go through the calibrator: UplinkCalibrator COPIES
                // its params at construction, so assigning ucal_params here
                // updated a struct nothing reads again and the bound never
                // moved. ucal_params is kept in step because §15.3 and the
                // §10.7 start gate still read liveness_ms from it.
                if (!uplink_relative && upwr.ceiling_qdb) {
                    ucal_params.seek.max_qdb =
                        std::min(cp_max_qdb, *upwr.ceiling_qdb);
                    (void)uplink_cal.set_max_qdb(ucal_params.seek.max_qdb);
                }
                // Re-seed the pairing key: set_tier() already actuated, and
                // leaving the key stale would make the next pairing pass fire
                // a second, redundant restore.
                uplink_pairing_key = uplink_pairing_now();
                return {200, std::string("{\"ok\":true}")};
            };
            h.uplink_calibrate = [&](const std::string& action)
                -> std::string {
                if (action == "abort") {
                    cancel_calibration("operator abort");  // idempotent
                    return "";
                }
                const bool both = (action == "start_both");
                if (!both && action != "start") {
                    return "action must be start, start_both or abort";
                }
                if (calib_seq.active()) {
                    return "bi-directional calibration already running";
                }
                if (uplink_cal.state() == CalibState::kRunning) {
                    return "calibration already running";
                }
                if (uplink_adapter == nullptr) {
                    return "no designated role:\"tx\" uplink adapter";
                }
                if (active_selection.originator == 0 ||
                    selected_craft_session == 0) {
                    return "no craft selected as DATA source";
                }
                // Authority (§10.7, Pass 131): we must hold the craft's §3.5
                // report latch. Pass 125 inferred that from a valid MAC on
                // §3.16; with the MAC gone it is READ from §3.15a, which the
                // craft already publishes. The two failure messages are kept
                // apart on purpose — "not published" sends the operator to the
                // craft's build, "held by N" sends them to the other ground,
                // and one message for both would send them to neither.
                if (const int holder = rx.craft_report_latch_holder();
                    holder < 0) {
                    return "craft is not publishing §3.15a report_latch_holder";
                } else if (holder != l.cfg.node.originator) {
                    return holder == 0
                               ? "craft has no report latch holder"
                               : "another ground holds the craft's report latch";
                }
                if (upwr.override_qdb) {
                    return "§10.5 TX-power override is latched";
                }
                if (scout.scanning()) return "scout is running";
                if (issuer.active()) return "CSA campaign active (issuer)";
                if (follower.campaign_active()) return "CSA campaign active";
                // The craft must not be calibrating its own downlink: §10.7
                // drives ground power to min_qdb, which starves the uplink
                // return path every §10.6 tally rides (§3.16).
                if (rx.craft_calibrating()) {
                    return "craft downlink calibration is running";
                }
                // Pass 153: no feedback-freshness or floor precondition —
                // with the craft's feed paused the contention floor is
                // structurally zero (absolute walls), and probe/tally
                // delivery is its own health check.
                if (uplink_identity.empty()) {
                    // Pass 154 D3: an identity-less unit cannot key the
                    // artifact §10.7 exists to persist — refuse at start,
                    // not at persist.
                    return "adapter reports no EFUSE identity (Pass 154 D3)";
                }
                if (!uplink_cal.start(now_ms())) {
                    return "calibration already running";
                }
                uplink_cal_craft = active_selection.originator;
                if (both) calib_seq.start(now_ms());
                std::fprintf(stderr, "uplink-calib: START mcs=%u%s\n",
                             l.cfg.air.uplink_mcs,
                             both ? " (bi-directional)" : "");
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
            start_vehicle_command = [&](const std::string& cmd, int arg)
                -> std::pair<int, std::string> {
                const uint8_t id = vcmd_id_for(cmd);
                if (id == 0) {
                    return {400, "{\"ok\":false,\"error\":\"unknown cmd\"}"};
                }
                // §10.7 (D6): "while §10.7 runs the ground refuses to issue a
                // §11.7 CALIBRATE campaign." Only the reverse direction was
                // implemented. Without this the craft starts its §10.6 run
                // with the ground uplink deliberately at min_qdb, starving
                // the report stream every §10.6 dwell and its abort clock
                // depend on. The sequencer's own downlink issue is exempt: by
                // then the uplink phase is terminal.
                if (id == vcmd_id::kCalibrate && arg != 0 &&
                    uplink_cal.state() == CalibState::kRunning) {
                    return {409,
                            "{\"ok\":false,\"error\":\"ground uplink "
                            "calibration is running\"}"};
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
            issue_vcmd = [&](const std::string& cmd, int arg) {
                const uint8_t id = vcmd_id_for(cmd);
                if (vcmd_id::typed_endpoint_only(id)) {
                    return std::pair<int, std::string>{
                        400,
                        "{\"ok\":false,\"error\":\"command requires typed "
                        "endpoint\"}"};
                }
                return start_vehicle_command(cmd, arg);
            };
            h.vehicle_command = issue_vcmd;
            h.link_mtu = [&](const std::string& mode)
                -> std::pair<int, std::string> {
                if (l.cfg.node.spectator) {
                    return {409,
                            "{\"ok\":false,\"error\":\"passive spectator "
                            "cannot negotiate MTU\"}"};
                }
                const auto tier = mtu_tier_for_mode(mode, mtu_supported);
                if (!tier) {
                    return {400,
                            "{\"ok\":false,\"error\":\"mode must be "
                            "default|medium|high|auto\"}"};
                }
                const uint16_t resolved = mtu_tier::budget(*tier);
                if (resolved > mtu_supported) {
                    return {400,
                            "{\"ok\":false,\"error\":\"requested MTU "
                            "exceeds local adapter support\"}"};
                }
                if (vissuer.active()) {
                    return {409,
                            "{\"ok\":false,\"error\":\"campaign pending\"}"};
                }
                mtu_mode = mode;
                mtu_requested = resolved;
                if (selection_state != "committed" ||
                    active_selection.originator == 0) {
                    mtu_reissue_pending = true;
                    return {200, std::string("{\"ok\":true,\"queued\":true,") +
                                     "\"requested\":" +
                                     std::to_string(mtu_requested) + "}"};
                }
                const auto result = start_vehicle_command("mtu_tier", *tier);
                if (result.first == 200) mtu_reissue_pending = false;
                return result;
            };
            h.csa = [&](uint32_t mhz,
                        uint32_t klass) -> std::pair<int, std::string> {
                const auto err = [](int code, const char* msg) {
                    return std::pair<int, std::string>{
                        code, std::string("{\"ok\":false,\"error\":\"") + msg +
                                  "\"}"};
                };
                if (!air.value->supports_csa()) {
                    return err(400, "CSA unsupported by this air backend");
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
        if (aim_log_enabled()) {
            static uint64_t aim_next_dump_ms = 0;
            if (now >= aim_next_dump_ms) {
                aim_next_dump_ms = now + 30000;
                g_aim_release.dump("release_lateness");
                g_aim_read_tsf.dump("read_tsf_cost");
            }
        }
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
        if (return_deadline_due && aim_log_enabled()) {
            // Issue #99: how late past the computed §7.2 deadline the host
            // loop actually released the window.
            g_aim_release.add(now_us_it - *ret_at_us);
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
            // §10.7 (Pass 132): a probe burst is PACED across return windows,
            // never dumped into one. The craft is RX-deaf while it transmits
            // and §7.2's quiet gap is only `return_window_us` (~2000 µs) wide,
            // so a batch larger than the gap spills into the craft's transmit
            // period and is simply not heard. Measured on the bench: a
            // 100-probe burst flushed at once lost ~40% of every dwell
            // REGARDLESS of commanded power — a flat, power-independent floor
            // that made every rung read `no_clean_point` while RSSI tracked
            // power perfectly. Seed 8 ≈ 2000 µs / ~160 µs for a ~100-byte
            // ONE, which is the shape §7.2 is engineered for and the only one
            // measured good: normal traffic delivers 99.7% at 1 per window, 8 per
            // window lost 4-15%, and 3 per window left verify dwells at 22-30permille
            // — straddling loss_ok_milli, so runs became a coin flip. The speedup
            // does NOT come from packing the gap; it comes from using EVERY gap
            // (~60/s at 60 fps) instead of every sixth one at the 10 Hz cadence.
            // Same shape, 6x the rate. RE-DERIVE (§17) if
            // `return_window_us` or the report size moves. Outside a
            // calibration this never binds — ordinary operation queues one
            // report per window.
            // ONLY while a §10.7 burst is in flight. Outside one this flushes
            // the whole batch and clears, exactly as it always did. Capping
            // ordinary traffic was a REGRESSION: reports are built on a 10 Hz
            // timer but windows open per video EOB, so whenever windows open
            // slower than reports are built the queue grows without bound and
            // every report is delivered later than the last. Measured on the
            // bench as the craft seeing ~4.8 epochs/s instead of 10 and
            // tripping REPORT_TIMEOUT on staleness — a healthy-looking link
            // (RSSI -54, 2.7M packets) with a starved return path.
            size_t window_budget = report_ret_held.size();
            while (!report_ret_held.empty() && window_budget-- > 0) {
                auto& [f, target] = report_ret_held.front();
                // Stamps in place, so the redundancy copy must be taken AFTER
                // the send: a repeat carries the SAME epoch (§3.5 — a
                // redundant copy is not a new emission).
                send_report(target, f.data(), f.size());
                if (ret_at_us && l.cfg.policy.ret.report_redundancy > 1) {
                    report_repeat_held.emplace_back(f, target);
                }
                report_ret_held.pop_front();
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
        // §10.7 calibrator service. `restore` is single-shot and set on EVERY
        // terminal path, so this is the one place probe power is handed back
        // to the §10.7 owner — no exit can strand it.
        // Ticked UNCONDITIONALLY. tick() early-returns when the calibrator is
        // not running, but it drains the single-shot restore/artifact edges
        // first — and a terminal state set from OUTSIDE this loop (the REST
        // abort, the §10.5 latch) has no other drain point. Gating this on
        // kRunning stranded probe power on exactly those paths.
        {
            const UplinkCalibActions ua = uplink_cal.tick(now_ms());
            // Rate before power (§10.7 R4 order): the rung is what the power
            // is being measured FOR, so commanding power first would spend a
            // settle window at the new level on the old rung.
            if (ua.set_rate && uplink_adapter != nullptr) {
                air.value->set_tx_mode(ua.set_rate->mcs, ua.set_rate->short_gi);
            }
            if (ua.set_qdb && uplink_adapter != nullptr) {
                // §10.7 (Pass 151): the probe drives the same actuator the
                // window was derived against — see `uplink_relative`.
                if (uplink_relative) {
                    (void)air.value->set_power_offset_qdb(
                        uplink_idx,
                        std::min(*ua.set_qdb,
                                 uplink_adapter->power_offset_max_qdb));
                } else {
                    (void)air.value->set_power_qdb(uplink_idx, *ua.set_qdb);
                }
            }
            // §3.16 (Pass 153): drain the run's probe emissions — MTU-padded,
            // paced by the engine, unicast to the craft the run is bound to.
            if (uplink_adapter != nullptr &&
                uplink_cal.state() == CalibState::kRunning &&
                uplink_cal_craft != 0) {
                uplink_cal.new_tick();
                for (;;) {
                    const DwellProbeOut po = uplink_cal.next_probe(now_ms());
                    if (!po.send) break;
                    CalibProbe pr;
                    pr.prefix.originator = l.cfg.node.originator;
                    pr.prefix.destination = uplink_cal_craft;
                    pr.prefix.session_id = session;
                    pr.run_id = uplink_cal.probe_run_id();
                    pr.dwell_id = uplink_cal.probe_dwell_id();
                    pr.seq = po.seq;
                    pr.count = uplink_cal.probe_dwell_count();
                    uint8_t buf[mtu_tier::kHighBudget];
                    const size_t n = encode_calib_probe(
                        pr, mtu_effective, buf, sizeof buf);
                    if (n != 0) {
                        (void)air.value->inject_return(uplink_cal_craft, buf,
                                                       n, false);
                        ++ucal_probes_tx;
                    }
                }
            }
            if (ua.restore) uplink_restore_actuators();
            // §10.7 per-dwell trace. The campaign's deliverable is a per-run
            // record (duration, samples/dwell, loss, RSSI, bracket); without
            // it a "verify_failed" carries no numbers and cannot be argued
            // with. Edge-triggered on the dwell counter, so this is one line
            // per completed dwell, not per tick.
            if (const auto& dw = uplink_cal.last_dwell();
                dw.seq != uplink_last_dwell_seq) {
                uplink_last_dwell_seq = dw.seq;
                std::fprintf(stderr,
                             "uplink-calib: dwell#%u rung=%u %s qdb=%d "
                             "sent=%u/%u received=%u loss=%upermille rssi=%d\n",
                             dw.seq, dw.rung, dw.verify ? "VERIFY" : "probe ",
                             dw.qdb, dw.sent, dw.target, dw.received,
                             dw.loss_milli, dw.rssi_mean);
            }
            // A craft change mid-run invalidates the measurement in progress
            // (D4) — the artifact stamps the craft identity, and the §3.16
            // counter domain restarts under a different RX chain.
            if (uplink_cal.state() == CalibState::kRunning &&
                active_selection.originator != uplink_cal_craft) {
                cancel_calibration("craft selection changed");
            }
            // §10.7 tier-2 applicability moves with the pairing: the craft is
            // selected after startup, its §3.16 fingerprint arrives later
            // still, and a CSA moves the channel. Re-resolve on that change —
            // never mid-run, where the calibrator owns the actuator.
            if (uplink_artifact &&
                uplink_cal.state() != CalibState::kRunning) {
                const uint64_t key = uplink_pairing_now();
                if (key != uplink_pairing_key) {
                    uplink_pairing_key = key;
                    uplink_restore_actuators();
                }
            }
            if (ua.artifact_ready && uplink_identity.empty()) {
                // Unreachable while D3 refuses the start; kept as the same
                // belt the craft persist carries — no future start path may
                // persist an artifact keyed on "" (it would match nothing
                // and read permanently stale).
                std::fprintf(stderr,
                             "uplink: artifact write refused — no adapter "
                             "identity (Pass 154 D3)\n");
            } else if (ua.artifact_ready) {
                UplinkArtifact art;
                art.local_adapter_identity = uplink_identity;
                art.craft_originator = active_selection.originator;
                // The craft half of the pairing comes from the feedback that
                // produced this measurement, not from config — it identifies
                // the RX chain the placement was actually measured against.
art.craft_adapter_fingerprint = craft_tally_fp;
                art.channel_mhz = operating_chan;
                art.bw_mhz = op_bw_mhz;
                art.t_unix = static_cast<int64_t>(::time(nullptr));
                art.placements = uplink_cal.placements();
                const uint8_t fp = uplink_calib_store_write(
                    l.cfg.policy.calibration.artifact_dir, art);
                if (fp == 0) {
                    // §10.7 (Pass 129): persistence IS the deliverable, so a
                    // write that never landed fails the run rather than
                    // reporting `done` with fingerprint 0. Drain the re-armed
                    // restore edge here so the actuator goes back to its
                    // pre-run owner instead of holding a placement that dies
                    // at the next boot.
                    std::fprintf(stderr,
                                 "uplink-calib: artifact write FAILED (%s) — "
                                 "run FAILED, placement not persisted\n",
                                 l.cfg.policy.calibration.artifact_dir.c_str());
                    uplink_cal.fail_persist();
                    const UplinkCalibActions fa = uplink_cal.tick(now_ms());
                    if (fa.restore) uplink_restore_actuators();
                } else {
                    uplink_artifact = std::move(art);
                    uplink_artifact_fp = fp;
                    uplink_artifact_stale = false;
                    std::fprintf(stderr,
                                 "uplink-calib: DONE %zu rungs fp=0x%02x\n",
                                 uplink_cal.placements().size(), fp);
                    for (const UplinkPlacement& pl : uplink_cal.placements()) {
                        std::fprintf(
                            stderr,
                            "uplink-calib:   mcs=%u %s -> %d qdb "
                            "rssi=%d loss=%upermille%s\n",
                            pl.mcs, pl.short_gi ? "sgi" : "lgi",
                            pl.placement_qdb, pl.placement_rssi_dbm,
                            pl.placement_loss_milli,
                            (pl.mcs == l.cfg.air.uplink_mcs &&
                             pl.short_gi == l.cfg.air.uplink_sgi)
                                ? "  <- operating"
                                : "");
                    }
                }
                // The seek already applied the placement; re-resolving makes
                // the running value and the persisted owner the same thing.
                uplink_restore_actuators();
            }
        }
        // §10.7 bi-directional sequencer. Ticked unconditionally of the
        // calibrator above, because the downlink phase runs entirely after
        // the local calibrator has gone terminal.
        if (calib_seq.active()) {
            const SeqActions sa = calib_seq.tick(
                uplink_cal.state(), rx.craft_calib_state(), now_ms());
            if (sa.start_downlink) {
                const auto r = start_vehicle_command("calibrate", 1);
                std::fprintf(stderr,
                             "calib-seq: uplink done -> downlink (%d)\n",
                             r.first);
            }
            if (!calib_seq.active()) {
                std::fprintf(stderr, "calib-seq: %s%s%s\n",
                             CalibSequencer::phase_name(calib_seq.phase()),
                             calib_seq.fail_reason() ? " " : "",
                             calib_seq.fail_reason() ? calib_seq.fail_reason()
                                                     : "");
            }
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
            if (mcs_trace_enabled()) {  // #101 stage-0 verifier (Tier-2)
                if (const DataView* dv = std::get_if<DataView>(&dec)) {
                    // sid: seq spaces are per-stream — a gap analysis over
                    // this trace must never conflate streams' counters.
                    std::fprintf(stderr,
                                 "mcstrace seq=%u mcs=%u ad=%u rssi=%d sid=%u\n",
                                 dv->hdr.seq, meta.rx_mcs, meta.adapter_id,
                                 static_cast<int>(rssi),
                                 static_cast<unsigned>(dv->hdr.stream_id));
                }
            }
            discovery.observe(dec, now, meta.net_id);
            scout.on_frame(meta, dec, d, n);  // §15.5a sweep aggregation
            // §15.5a (Pass 144): the sweep widen is node-wide, so foreign
            // net_ids reach this callback. The two consumers above want them —
            // reporting who is out there is what a sweep is for, and neither
            // holds link state. Everything below does. Measured on the bench
            // before this gate existed: sweeping past an unpaired craft on
            // another net_id delivered 5085 packets of its video to the local
            // stream output, and the same path can latch it or follow its CSA.
            // Only on RF (udp-air has no
            // §3.0 identity) and only when a net_id is configured (§3.0: an
            // unconfigured node accepts any).
            if (air.value->is_radio() && scout.scanning() &&
                active_selection.net_id &&
                meta.net_id != *active_selection.net_id) {
                return;
            }
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
                    // §10.7: a retune moves the channel out from under an
                    // in-flight run. Continuing would persist a placement
                    // measured across two channels under one channel_mhz
                    // identity — worse than having no artifact at all.
                    cancel_calibration("CSA retune");
                    std::fprintf(stderr, "csa: following -> %u MHz\n",
                                 c->target_chan);
                }
                return;
            }
            // §3.16 (Pass 153): the calibration family, scoped to the craft
            // we are currently taking DATA from.
            if (const CalibProbe* cp = std::get_if<CalibProbe>(&dec)) {
                // Ground as RECEIVER: the craft's §10.6 downlink probes.
                if (cp->prefix.destination == l.cfg.node.originator &&
                    cp->prefix.originator == active_selection.originator &&
                    (selected_craft_session == 0 ||
                     cp->prefix.session_id == selected_craft_session)) {
                    const DwellTallyOut t = dcal_rx.on_probe(
                        cp->run_id, cp->dwell_id, cp->seq, cp->count,
                        meta.rssi, meta.rx_mcs, now);
                    if (t.send && uplink_adapter != nullptr) {
                        CalibTally out;
                        out.prefix.originator = l.cfg.node.originator;
                        out.prefix.destination = cp->prefix.originator;
                        out.prefix.session_id = session;
                        out.run_id = t.run_id;
                        out.dwell_id = t.dwell_id;
                        out.received = t.received;
                        out.rssi_sum_dbm = t.rssi_sum_dbm;
                        out.rx_mcs = t.rx_mcs;
                        out.adapter_fingerprint = ground_ident_fp;
                        uint8_t tb[kCalibTallySize];
                        const size_t tn =
                            encode_calib_tally(out, tb, sizeof tb);
                        if (tn != 0) {
                            (void)air.value->inject_return(
                                cp->prefix.originator, tb, tn, false);
                            ++ucal_tallies_tx;
                        }
                    }
                }
                return;
            }
            if (const CalibTally* ct = std::get_if<CalibTally>(&dec)) {
                // Ground as SENDER: the craft's per-dwell receipt for our
                // §10.7 uplink run.
                if (ct->prefix.destination == l.cfg.node.originator &&
                    ct->prefix.originator == active_selection.originator) {
                    uplink_cal.on_tally(ct->run_id, ct->dwell_id,
                                        ct->received, ct->rssi_sum_dbm,
                                        ct->rx_mcs, ct->adapter_fingerprint);
                    craft_tally_fp = ct->adapter_fingerprint;
                    ucal_rx_mcs = ct->rx_mcs;
                    ++ucal_tallies_rx;
                }
                return;
            }
            if (std::holds_alternative<ExtUnknown>(dec)) {
                return;  // §3.16: newer peer — feature unavailable
            }
            if (const DataView* v = std::get_if<DataView>(&dec)) {
                // The craft's session comes from the stream we are actually
                // consuming — §3.16 scopes its accept to that exact tuple.
                if (v->hdr.prefix.originator == active_selection.originator) {
                    selected_craft_session = v->hdr.prefix.session_id;
                }
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
                    const bool accepted =
                        vissuer.on_echo(*vc, now_us_it);  // §11.7 craft echo
                    if (accepted && vc->cmd_id == vcmd_id::kMtuTier &&
                        std::strcmp(vissuer.state_str(), "acked") == 0) {
                        mtu_effective = mtu_tier::budget(vc->cmd_arg);
                        std::fprintf(stderr,
                                     "mtu: vehicle ACK tier=%u budget=%u\n",
                                     vc->cmd_arg, mtu_effective);
                    }
                }
                return;  // a ground never acts on a command
            }
            rx.on_air(meta.adapter_id, d, n, now, deliver, rssi,
                      early_deliver, meta.rx_mcs);
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
#if WBLINK_RADIO
        // §3.16 (Pass 159): the node cause from the shared quality drain;
        // Unknown off the radio backend, and the verdict frame rides its own
        // inject (no §3.5 epoch stamp/commit — see RxCore::tick).
        const uint8_t lv = air.value->radio != nullptr
                               ? air.value->radio->link_verdict()
                               : link_verdict::kUnknown;
#else
        const uint8_t lv = link_verdict::kUnknown;
#endif
        const RxCore::Inject inject_verdict =
            [&](const uint8_t* f, size_t n, uint16_t target) {
                uint8_t tmp[kLinkVerdictSize];
                if (n > sizeof(tmp)) return;
                std::memcpy(tmp, f, n);
                (void)air.value->inject_return(target, tmp, n, false);
            };
        // §9.4 Pass 163: scope the probe feed to the accepted sender, then
        // feed CRC-errored descriptor rates as deltas (guard 3 — the
        // backend dropped the bodies pre-parse).
        rx.set_probe_originator(active_selection.originator);
        {
            static uint64_t crc_last[kRxMcsBuckets] = {};
            uint64_t crc_now[kRxMcsBuckets];
            if (air.value->crc_mcs_totals(crc_now)) {
                for (size_t m = 0; m < kRxMcsBuckets; ++m) {
                    if (crc_now[m] < crc_last[m]) {
                        // Counter regressed (backend recreate): resync
                        // without feeding a wrapped delta.
                        crc_last[m] = crc_now[m];
                        continue;
                    }
                    const uint64_t d = crc_now[m] - crc_last[m];
                    if (d != 0) {
                        rx.on_crc_frames(
                            static_cast<uint8_t>(m),
                            static_cast<uint32_t>(
                                std::min<uint64_t>(d, 0xFFFFFFFFull)),
                            now);
                    }
                    crc_last[m] = crc_now[m];
                }
            }
        }
        rx.tick(now, deliver, inject_report, inject_nack,
                arq_rx_enabled && (!qg.enabled() || repair_tail_closed), lv,
                &inject_verdict);
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
        // §10.7 (D1): the follower guard alone was the RARE half — a ground
        // node is normally the CSA ISSUER, and its commit/revert/abort paths
        // all retune. Any non-idle issuer action means the channel is about
        // to move under an in-flight seek, which would persist a placement
        // measured across two channels under one channel_mhz identity.
        if (ia.kind != CsaIssuer::IssuerAction::Kind::kNone) {
            cancel_calibration("CSA issuer retune");
        }
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
                            previous_selection->net_id.value_or(0));
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
                    // Every successful claim reasserts the local preference,
                    // including Default. This matters when a ground reboots
                    // and reclaims before the craft's old binding expires.
                    mtu_reissue_pending = true;
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
                    air.value->set_stamp_net_id(previous_selection->net_id.value_or(0));
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
                air.value->set_stamp_net_id(active_selection.net_id.value_or(0));
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
        // §9.3a: a preference chosen before/during claim is reissued once the
        // CSA claim is actually committed. Keep this non-blocking and share
        // the exact command validation/keying path used by the REST endpoint.
        if (mtu_reissue_pending && selection_state == "committed" &&
            !vissuer.active() && start_vehicle_command) {
            const auto tier = mtu_tier_for_mode(mtu_mode, mtu_supported);
            if (tier) {
                const auto result = start_vehicle_command("mtu_tier", *tier);
                if (result.first == 200) mtu_reissue_pending = false;
            }
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
                                      vissuer.nonce(), arq_rx_enabled,
                                      mtu_mode.c_str(), mtu_requested,
                                      mtu_effective, mtu_supported};
            const UplinkStatsFill ufill = uplink_fill();
            emit_stats(emitter, l, session, t0, nullptr, &rx, &*air.value,
                       tsf_fallbacks,
                       issuer.active() ? issuer.state_str()
                                       : follower.state_str(),
                       ret_window_hits, ret_window_misses, wedge.wedged(),
                       &frame_stats, &shm_stats,
                       cache_ctl ? &crs : nullptr,
                       cache_store ? &css : nullptr, &last_snap, &timing,
                       &vfill, operating_chan, &ufill);
            if (control) {
                control->publish_stats(emitter.last_line());
            }
            next_stats = now + stats_period;
        }
    }
    // §10.7: shutdown is an exit like any other. Without this a SIGTERM
    // mid-run leaves the uplink adapter at the last probe power until some
    // later start re-resolves the owner.
    cancel_calibration("shutdown");
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
    // §3.5: Reporter::build() leaves report_epoch at 0 and the INJECTOR stamps
    // it, so an injector that does not stamp emits a constant 0 on the wire.
    // run_rx has send_report for this; loopback passed inject_nack as its
    // report injector and therefore emitted epoch 0 on every LINK_REPORT,
    // silently degrading anything on the bench that keys on epoch monotonicity
    // — including the §10.7 denominator the whole feature is measured with.
    const RxCore::Inject inject_report = [&](const uint8_t* f, size_t n,
                                             uint16_t) {
        std::vector<uint8_t> tmp(f, f + n);
        (void)link_report_stamp_epoch(tmp.data(), tmp.size(),
                                      rx.next_report_epoch());
        if (return_rng.uniform() >= l.cfg.loopback.return_loss_p) {
            tx.on_air(tmp.data(), tmp.size(), loop_now);
            rx.commit_report_epoch();
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
        h.fec = [&](int sid, int ip, int pp, int mk, int mr,
                    std::optional<uint16_t> ep) -> std::string {
            if (sid < 0 || sid > 255) return "bad stream_id";
            if (ip < 0 || ip > 4000 || pp < 0 || pp > 4000 || mk < 1 ||
                mr < 0 || mr > 255)
                return "bad fec rates (0..4000 permille, min_k>=1, min_r 0..255)";
            // §14.1a: the control server already range-checked e_permille;
            // nullopt here means "inherit p_permille" (full replacement).
            return tx.set_stream_fec(static_cast<uint8_t>(sid),
                                     static_cast<uint16_t>(ip),
                                     static_cast<uint16_t>(pp),
                                     static_cast<uint16_t>(mk),
                                     static_cast<uint16_t>(mr), ep)
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
        rx.tick(loop_now, deliver, inject_nack, inject_report);
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
                 "usage: %s <tx|rx|loopback> -c <config.json> "
                 "[--check [--strict] [--json]]\n"
                 "       %s config-schema [--json]\n"
                 "  --check         validate config + bindings and exit\n"
                 "  --strict        also report unknown and inert keys\n"
                 "                  (warnings; exit stays 0)\n"
                 "  --json          machine-readable --strict report on stdout\n"
                 "  config-schema   print the declared §15.2 key surface to\n"
                 "                  stdout (JSON is the only format; --json is\n"
                 "                  accepted for symmetry with #106)\n",
                 argv0, argv0);
    return 2;
}

}  // namespace

// Everything above is in an anonymous namespace, so it has internal linkage
// and no other translation unit can link against it. tests/app_test.cpp
// therefore #includes THIS FILE and suppresses the entry point, which is the
// only way to reach TxCore and the app-layer helpers from a test without
// first extracting them. See tests/app_test.cpp for why that trade was made.
#ifndef WBLINK_APP_TEST
int main(int argc, char** argv) {
    if (argc < 2) {
        return usage(argv[0]);
    }
    const std::string mode = argv[1];
    // Reporting only: no config is loaded and no binding is opened, so this
    // runs anywhere the binary does — including a craft overlay with no
    // config staged yet. Data goes to stdout; the flight loop's §15.3 stats
    // share that fd but are unreachable from here.
    if (mode == "config-schema") {
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--json") != 0) {
                return usage(argv[0]);
            }
        }
        const std::string schema = config_schema_json();
        // A schema this exits 0 on but did not finish writing is worse than no
        // schema: the caller is usually a pipeline or a generator that will
        // treat truncation as an empty key set. Check the write AND the flush
        // — buffered output can fail at either.
        if (std::fwrite(schema.data(), 1, schema.size(), stdout) != schema.size() ||
            std::fflush(stdout) != 0) {
            std::perror("config-schema: write");
            return 1;
        }
        return 0;
    }
    if (mode != "tx" && mode != "rx" && mode != "loopback") {
        return usage(argv[0]);
    }
    std::string config_path;
    bool check_only = false;
    bool strict = false;
    bool as_json = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else if (std::strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else if (std::strcmp(argv[i], "--json") == 0) {
            as_json = true;
        } else {
            return usage(argv[0]);
        }
    }
    if (config_path.empty()) {
        return usage(argv[0]);
    }
    // Both only mean anything while validating, and silently ignoring them on
    // a flight invocation would be the same class of bug this feature exists
    // to find.
    if ((strict || as_json) && !check_only) {
        std::fprintf(stderr, "--strict and --json apply to --check only\n");
        return usage(argv[0]);
    }
    // --json alone produced no output and exited 0, which a script parsing the
    // report reads as "clean". It is the report's format, not a mode.
    if (as_json && !strict) {
        std::fprintf(stderr, "--json needs --strict (it formats its report)\n");
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
        if (strict) {
            // Re-read rather than thread the text through load_all: --check is
            // a one-shot path, and the alternative is a field on Loaded that
            // the flight path would carry for nothing.
            std::string text;
            std::FILE* cf = std::fopen(config_path.c_str(), "rb");
            if (cf == nullptr) {
                // Silence here would print "0 finding(s)" for a file we never
                // read — a false clean, and under #106 item 8 a false pass.
                std::perror("--strict: reopen config");
                return 1;
            }
            char buf[4096];
            size_t got;
            while ((got = std::fread(buf, 1, sizeof(buf), cf)) > 0) {
                text.append(buf, got);
            }
            const bool read_failed = std::ferror(cf) != 0;
            std::fclose(cf);
            if (read_failed) {
                std::fprintf(stderr, "--strict: error reading %s\n",
                             config_path.c_str());
                return 1;
            }
            const std::vector<KeyFinding> findings =
                check_config_keys(text, l.cfg);
            if (as_json) {
                const std::string report = check_report_json(findings);
                if (std::fwrite(report.data(), 1, report.size(), stdout) !=
                        report.size() ||
                    std::fflush(stdout) != 0) {
                    std::perror("--json: write");
                    return 1;
                }
            } else {
                for (const KeyFinding& f : findings) {
                    if (f.verdict == KeyVerdict::kUnknown) {
                        std::fprintf(stderr,
                                     "strict: unknown key %s — not read by the "
                                     "loader; check `config-schema --json`\n",
                                     f.path.c_str());
                    } else {
                        std::fprintf(stderr, "strict: inert key %s — %s\n",
                                     f.path.c_str(), f.reason);
                    }
                }
                std::fprintf(stderr, "strict: %zu finding(s)\n",
                             findings.size());
            }
            // Warnings, not errors: #106 item 8 promotes --strict to a
            // non-zero exit once the generator is what authors configs.
        }
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
    // (the bounded CLI wait, blocking backend reads), so with SA_RESTART cleared
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
#endif  // WBLINK_APP_TEST
