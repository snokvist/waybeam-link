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
#include <atomic>
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
#include "wblink/node/uplink_data.h"
#include "wblink/node/clock.h"
#include "wblink/node/tx_core.h"
#include "wblink/node/stats_fill.h"
#include "wblink/node/discovery.h"
#include "wblink/node/load.h"
#include "wblink/node/entropy.h"
#include "wblink/node/frame_kind.h"
#include "wblink/node/policy.h"
#include "wblink/node/rx_core.h"
#include "wblink/node/rx_node.h"
#include "wblink/node/tx_node.h"
#include "wblink/node/uplink_power.h"
#include "wblink/node/vcmd.h"
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
using wblink::node::ArqTimingTracker;
using wblink::node::emit_stats;
using wblink::node::InfoSelfState;
using wblink::node::load_all;
using wblink::node::Loaded;
using wblink::node::UplinkStatsFill;
using wblink::node::VcmdStatsFill;
using wblink::node::DiscoveryCatalog;
using wblink::node::PacketEventTrace;
using wblink::node::mode_catalog_dir;
using wblink::node::run_rx;
using wblink::node::run_tx;
using wblink::node::RxCore;
using wblink::node::ScoutEngine;
using wblink::node::announce_token;
using wblink::node::build_health_json;
using wblink::node::build_info_json;
using wblink::node::channel_allowed;
using wblink::node::csa_params;
using wblink::node::frame_is_eob;
using wblink::node::frame_is_live_rtp_data;
using wblink::node::frame_is_paced_eob;
using wblink::node::mtu_tier_for_mode;
using wblink::node::power_tier_json;
using wblink::node::quietgap_policy;
using wblink::node::session_nonce;
using wblink::node::UplinkPower;
using wblink::node::vcmd_id_for;
using wblink::node::vcmd_name_for;
using wblink::node::vcmd_params;

// Lock-free on every target here, which is what keeps it legal to write
// from a signal handler; `volatile sig_atomic_t` would be legal there too
// but says nothing across threads, and node/ now takes a reference to this
// that a consumer may set from one (#109 Phase 2c step 2).
std::atomic<int> g_stop{0};
static_assert(std::atomic<int>::is_always_lock_free,
              "g_stop is written from a signal handler");
void on_signal(int) { g_stop = 1; }

// §9.10 v2 (Pass 148): distinct from the generic failure 1, so a supervisor
// (or an operator reading the log) can tell "the transmitter wedged and I am
// asking to be re-execed" from "this node cannot start at all".
constexpr int kExitTxWedged = 9;

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





// ArqTimingTracker moved to node/include/wblink/node/stats_fill.h (#109 Phase 2a).

// packet_type_name moved to node/include/wblink/node/air_backend.h (#109 Phase 2a).

// PacketEventTrace moved to node/include/wblink/node/air_backend.h (#109 Phase 2a).

// DiscoveryCatalog moved to node/include/wblink/node/discovery.h
// (#109 Phase 2a).

// ScoutEngine moved to node/include/wblink/node/discovery.h
// (#109 Phase 2a). Its side effects were already injected Hooks.

// csa_params + vcmd_params moved to node/include/wblink/node/policy.h
// (#109 Phase 2c).

// vcmd_id_for moved to node/include/wblink/node/vcmd.h
// (#109 Phase 2c).

// power_tier_json + UplinkPower moved to
// node/include/wblink/node/uplink_power.h (#109 Phase 2c).

// vcmd_name_for + mtu_tier_for_mode moved to
// node/include/wblink/node/vcmd.h (#109 Phase 2c).

// bw_code moved to node/include/wblink/node/tx_core.h (#109 Phase 2a).

// channel_allowed moved to node/include/wblink/node/policy.h
// (#109 Phase 2c).

// frame_is_eob / frame_is_paced_eob / frame_is_live_rtp_data moved to
// node/include/wblink/node/frame_kind.h (#109 Phase 2c).

// session_nonce + announce_token moved to
// node/include/wblink/node/entropy.h (#109 Phase 2c).

// scheduler_policy moved to node/include/wblink/node/tx_core.h (#109 Phase 2a).

// s_to_ms moved to node/include/wblink/node/tx_core.h (#109 Phase 2a).

// selector_policy moved to node/include/wblink/node/tx_core.h (#109 Phase 2a).

// quietgap_policy moved to node/include/wblink/node/policy.h
// (#109 Phase 2c).

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

// Loaded moved to node/include/wblink/node/stats_fill.h (#109 Phase 2a).

// load_all moved to node/include/wblink/node/load.h (#109 Phase 3 prep),
// beside the Loaded it fills.

// VcmdStatsFill moved to node/include/wblink/node/stats_fill.h (#109 Phase 2a).

// UplinkStatsFill moved to node/include/wblink/node/stats_fill.h (#109 Phase 2a).

// emit_stats moved to node/include/wblink/node/stats_fill.h (#109 Phase 2a).

// InfoSelfState moved to node/include/wblink/node/stats_fill.h (#109 Phase 2a).

// build_info_json moved to node/include/wblink/node/stats_fill.h
// (#109 Phase 2c).

// build_health_json moved to node/include/wblink/node/stats_fill.h
// (#109 Phase 2c).

// ---- modes -------------------------------------------------------------------


// run_rx moved to node/src/rx_node.cpp (#109 Phase 2c step 2). It reached
// exactly one app-scope name on the way out — the stop flag — which is now
// a parameter. app/main.cpp is the driver; node/ owns the loop.

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
            // Shared builder (2026-08-14 review): this hand copy had already
            // drifted from the tx predicate once. The loopback bench always
            // owns an applier (spawn_mode_applier), so configured == cmd set.
            return wblink::node::build_mode_json(
                loop_active_mode, !l.cfg.venc.mode_apply_cmd.empty());
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

// §15.2 (Pass 195): a JSON string for `adapters --emit`. The tree escapes
// this field everywhere else it is serialized (stats_fill.h's json_escape);
// an adapter named `wing"1` would otherwise emit a block that does not parse,
// and this mode accepts the array form, where the name is operator-supplied.
std::string json_quote(const std::string& in) {
    std::string out = "\"";
    for (const char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) out += c;
                break;
        }
    }
    return out + "\"";
}

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <tx|rx|loopback> -c <config.json> "
                 "[--check [--strict] [--json]]\n"
                 "       %s config-schema [--json]\n"
                 "       %s adapters -c <config.json> [--emit]\n"
                 "  --check         validate config + bindings and exit\n"
                 "  --strict        also report unknown and inert keys\n"
                 "                  (warnings; exit stays 0)\n"
                 "  --json          machine-readable --strict report on stdout\n"
                 "  config-schema   print the declared §15.2 key surface to\n"
                 "                  stdout (JSON is the only format; --json is\n"
                 "                  accepted for symmetry with #106)\n"
                 "  adapters        bring up the radios this config resolves\n"
                 "                  to, print the election, and exit. --check\n"
                 "                  cannot answer this: under §15.2 auto the\n"
                 "                  adapter set is hardware, not config.\n"
                 "  --emit          with `adapters`, print the result as a\n"
                 "                  paste-ready array-form \"adapters\" block —\n"
                 "                  discover once, then freeze the assignment\n",
                 argv0, argv0, argv0);
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
    if (mode != "tx" && mode != "rx" && mode != "loopback" &&
        mode != "adapters") {
        return usage(argv[0]);
    }
    std::string config_path;
    bool check_only = false;
    bool strict = false;
    bool as_json = false;
    bool emit = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else if (std::strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else if (std::strcmp(argv[i], "--json") == 0) {
            as_json = true;
        } else if (std::strcmp(argv[i], "--emit") == 0) {
            emit = true;
        } else {
            return usage(argv[0]);
        }
    }
    // --emit shapes the `adapters` report; on a flight invocation it would be
    // silently ignored, which is the class of bug --strict/--json already
    // refuse above.
    if (emit && mode != "adapters") {
        std::fprintf(stderr, "--emit applies to the `adapters` mode only\n");
        return usage(argv[0]);
    }
    if (mode == "adapters" && (check_only || strict || as_json)) {
        std::fprintf(stderr,
                     "`adapters` brings up hardware; it is not a --check "
                     "mode\n");
        return usage(argv[0]);
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
    // §15.2 (Pass 195): the election preview. `--check` structurally cannot
    // answer "which adapter will transmit" under the auto form — the answer is
    // the hardware, not the config — so this brings the radios up, prints the
    // candidate table the backend logs at bring-up, and exits. The array form
    // is accepted too: it reports the same table, which is how an operator
    // confirms a bus or mac pin landed on the unit they meant.
    if (mode == "adapters") {
        auto air = AirBackend::create(l.cfg);
        if (!air) {
            std::fprintf(stderr, "air error: %s\n", air.error.c_str());
            return 1;
        }
        // Drain briefly before tearing down. TWO reasons, and the second is
        // not optional:
        //
        // 1. It makes the report answer something create() cannot — whether
        //    each ear is actually hearing frames on the configured channel.
        // 2. A RadioAir created and destroyed with NO poll in between hangs in
        //    ~Impl's join. That is a pre-existing condition, not one this mode
        //    introduced: `hwtrial_bringup --bus <p> --seconds 0` on main hangs
        //    identically, and ~Impl says why in its own comment — "a join can
        //    block while a bring-up is still in flight (bring-up does not poll
        //    the stop flag)". So StopRxLoop can be missed by an RX thread that
        //    has not yet reached devourer's loop. Fixing that race belongs to
        //    the backend's threading contract and its own device pass; this
        //    mode simply does what every real consumer does. See
        //    docs/findings.md 2026-08-30.
        // A WALL-CLOCK deadline, not a poll count: poll_once only waits when
        // its queue is EMPTY, so on an ear that is actually receiving, twenty
        // 50 ms calls return in well under a millisecond — and the frame
        // counts below are advanced by the RX threads over elapsed time, not
        // by the poll. A fixed count would therefore give the shortest window
        // exactly where traffic is heaviest, which is backwards.
        const auto drain_until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (std::chrono::steady_clock::now() < drain_until) {
            air.value->iface()->poll_once(50, [](const AirRxMeta&,
                                                 const uint8_t*, size_t) {});
        }
        std::vector<uint64_t> heard(l.cfg.adapters.size(), 0);
        for (size_t i = 0; i < heard.size(); ++i) {
            heard[i] = air.value->iface()->rx_frames(i);
        }

        // After create(), l.cfg.adapters is the RESOLVED array either way, so
        // one loop describes both forms.
        for (size_t i = 0; i < l.cfg.adapters.size(); ++i) {
            const AdapterCfg& a = l.cfg.adapters[i];
            const AirIface::AdapterCapsView caps = air.value->adapter_caps(i);
            // The LIVE EFUSE identity, not the stanza's. Under auto they are
            // the same; under the array form `resolved_adapters()` returns the
            // AUTHORED stanzas untouched, so `a.mac` is whatever the operator
            // wrote — normally nothing. Printing that would make this mode
            // useless for the very thing it advertises on the array form:
            // confirming which physical unit a pin landed on.
            const std::string live_mac = air.value->adapter_mac(i);
            std::fprintf(stderr,
                         "adapter %zu: %-20s role=%-2s chan=%u bw=%u "
                         "part=%s (%s) chip=%s mac=%s power_actuator=%s "
                         "rx_frames=%llu\n",
                         i, a.name.c_str(), a.role == Role::kTx ? "tx" : "rx",
                         a.channel_mhz, unsigned(a.bw),
                         caps.part.empty() ? "unknown" : caps.part.c_str(),
                         caps.aliases.empty() ? "-" : caps.aliases.c_str(),
                         caps.chip.c_str(),
                         live_mac.empty() ? "none" : live_mac.c_str(),
                         caps.power_actuator ? "yes" : "no",
                         static_cast<unsigned long long>(heard[i]));
        }
        if (emit) {
            // stdout, while the table above went to stderr: this is the half a
            // caller redirects into a config, and mixing the two would make
            // `> adapters.json` produce something that does not parse.
            // A COMPLETE stanza, or the "freeze" silently changes the node.
            // The emitted block replaces the adapters section outright — the
            // two §15.2 forms are exclusive, so pasting it means deleting the
            // auto block — and every key that only lived there has to come
            // with it. Omitting the power keys would drop the operator's
            // §10.5 boot offset back to the -24 seed and remove the §10.3
            // ceiling, which is a power change disguised as a refactor.
            const auto qdb_list = [](const char* key,
                                     const std::vector<int32_t>& v) {
                if (v.empty()) return std::string{};
                std::string s2 = std::string(", \"") + key + "\": [";
                for (size_t k = 0; k < v.size(); ++k) {
                    if (k) s2 += ", ";
                    s2 += std::to_string(v[k]);
                }
                return s2 + "]";
            };
            std::string out = "  \"adapters\": [\n";
            for (size_t i = 0; i < l.cfg.adapters.size(); ++i) {
                const AdapterCfg& a = l.cfg.adapters[i];
                // Escaped like every other serializer in the tree: under auto
                // the names are synthesized and safe, but this mode also
                // accepts the array form, where the name is operator-supplied
                // and validated only for non-emptiness and uniqueness.
                out += "    { \"name\": " + json_quote(a.name);
                // The identity is the point of emitting: it is what makes the
                // frozen array bind to the same physical units after a re-plug
                // (§15.2 precedence mac > bus > first-free). Taken LIVE from
                // the backend, because on the array form the stanza usually
                // carries no mac at all and the emitted block would then bind
                // first-free — moving role:"tx" to another dongle, which is
                // precisely what the mac is here to prevent.
                const std::string live_mac = air.value->adapter_mac(i);
                if (!live_mac.empty()) {
                    out += ", \"mac\": " + json_quote(live_mac);
                }
                // A bus pin the operator authored survives the round trip; it
                // is the §15.2 port pin and dropping it would silently widen
                // the binding.
                if (!a.bus.empty()) out += ", \"bus\": " + json_quote(a.bus);
                out += ", \"role\": \"";
                out += a.role == Role::kTx ? "tx" : "rx";
                out += "\", \"channel\": " + std::to_string(a.channel_mhz);
                out += ", \"bw\": " + std::to_string(unsigned(a.bw));
                if (!a.power_map.empty()) {
                    out += ", \"power_map\": " + json_quote(a.power_map);
                }
                if (a.max_power_qdb) {
                    out += ", \"max_power_qdb\": " +
                           std::to_string(*a.max_power_qdb);
                }
                out += ", \"power_offset_qdb\": " +
                       std::to_string(a.power_offset_qdb);
                out += ", \"power_offset_max_qdb\": " +
                       std::to_string(a.power_offset_max_qdb);
                out += qdb_list("power_presets_qdb", a.power_presets_qdb);
                out += qdb_list("power_offset_presets_qdb",
                                a.power_offset_presets_qdb);
                out += " }";
                if (i + 1 < l.cfg.adapters.size()) out += ',';
                out += '\n';
            }
            out += "  ]\n";
            if (std::fwrite(out.data(), 1, out.size(), stdout) != out.size() ||
                std::fflush(stdout) != 0) {
                std::perror("adapters --emit: write");
                return 1;
            }
        }
        return 0;
    }
    if (check_only) {
        auto bindings = BindingSet::create(l.cfg);
        if (!bindings) {
            std::fprintf(stderr, "binding error: %s\n",
                         bindings.error.c_str());
            return 1;
        }
        // §7.5: the uplink stream shape is role-dependent, so the loader
        // cannot rule on it — but --check knows the mode, and the deploy
        // gate runs --check on every config, so refusing here keeps the
        // spec's "refused at --check" promise honest (a bad shape must not
        // pass validation and then fail at flight start).
        if (mode == "tx" || mode == "rx") {
            for (const StreamCfg& s : l.cfg.streams) {
                if (const char* err = uplink_shape_error(s, mode == "tx")) {
                    std::fprintf(stderr, "stream %u: %s\n", s.stream_id, err);
                    return 1;
                }
            }
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
        // B9 lives here, not in node/: the app supplies the fork and owns the
        // exit code. kExitTxWedged stays 9 on the wire (Pass 148) — only the
        // decision to exit moved out of the loop.
        const int rc = run_tx(l, g_stop, spawn_mode_applier);
        return rc == wblink::node::kTxWedged ? kExitTxWedged : rc;
    }
    if (mode == "rx") {
        return run_rx(l, g_stop);
    }
    return run_loopback(l);
}
#endif  // WBLINK_APP_TEST
