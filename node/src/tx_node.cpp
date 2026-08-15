// SPDX-License-Identifier: GPL-2.0-or-later
// The TX node's run loop, moved out of app/main.cpp (#109 Phase 3).
//
// NOT a verbatim move, and the header says why: run_rx reached one app-scope
// name, run_tx reaches four, and three of them are the process-owning
// behaviours B9 forbids here. Four edits to 977 lines are the whole
// difference — see node/include/wblink/node/tx_node.h.
//
// The include list is rx_node.cpp's, for the same reason it has one: every
// header here was previously reached transitively through app/main.cpp's
// 8.8k-line translation unit, and a library TU must declare what it uses.
#include "wblink/node/tx_node.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "wblink/air_udp.h"
#include "wblink/airtime.h"
#include "wblink/binding.h"
#include "wblink/cache_assignment.h"
#include "wblink/cache_controller.h"
#include "wblink/cache_store.h"
#include "wblink/cache_udp.h"
#include "wblink/calib_dwell.h"
#include "wblink/calib_store.h"
#include "wblink/calibrate.h"
#include "wblink/config.h"
#include "wblink/control_server.h"
#include "wblink/crc8.h"
#include "wblink/csa.h"
#include "wblink/endian.h"
#include "wblink/fps_ladder.h"
#include "wblink/frame_caps.h"
#include "wblink/frame_framer.h"
#include "wblink/frame_reassembler.h"
#include "wblink/frame_shm.h"
#include "wblink/frame_shm_format.h"
#include "wblink/framer.h"
#include "wblink/hmac_sha256.h"
#include "wblink/jscc_runtime_shadow.h"
#include "wblink/log.h"
#include "wblink/loss_model.h"
#include "wblink/mcs_probe.h"
#include "wblink/power.h"
#include "wblink/power_file.h"
#include "wblink/quietgap.h"
#include "wblink/recovery.h"
#include "wblink/report_gate.h"
#include "wblink/reporter.h"
#include "wblink/ring.h"
#include "wblink/rx.h"
#include "wblink/scheduler.h"
#include "wblink/scout_sense.h"
#include "wblink/scout_store.h"
#include "wblink/selector.h"
#include "wblink/selector_state.h"
#include "wblink/stats.h"
#include "wblink/table.h"
#include "wblink/txwedge.h"
#include "wblink/uplink_calib_store.h"
#include "wblink/uplink_calibrate.h"
#include "wblink/vehicle_cmd.h"
#include "wblink/video_slot_cadence.h"

#include "wblink/modes.h"

#include "wblink/node/aim.h"
#include "wblink/node/air_backend.h"
#include "wblink/node/clock.h"
#include "wblink/node/discovery.h"
#include "wblink/node/entropy.h"
#include "wblink/node/frame_kind.h"
#include "wblink/node/policy.h"
#include "wblink/node/load.h"
#include "wblink/node/rx_core.h"
#include "wblink/node/stats_fill.h"
#include "wblink/node/tx_core.h"
#include "wblink/node/tx_runtime_info.h"
#include "wblink/node/uplink_power.h"
#include "wblink/node/vcmd.h"

#if WBLINK_RADIO
#include "wblink/air_radio.h"
#endif

namespace wblink {
namespace node {

int run_tx(const Loaded& l, const std::atomic<int>& stop,
           const ModeApplyFn& mode_apply) {
    return run_tx(l, stop, mode_apply, nullptr);
}

int run_tx(const Loaded& l, const std::atomic<int>& stop,
           const ModeApplyFn& mode_apply, TxRuntimeInfo* runtime_info) {
    auto air = AirBackend::create(l.cfg);
    if (!air) {
        wb_logf("air error: %s\n", air.error.c_str());
        return 1;
    }
    // §15.5 (Pass 172/174): publish the per-die capability answers the moment
    // the backend can state them — the same one-builder object /info serves.
    if (runtime_info != nullptr) {
        runtime_info->publish_adapters(
            "{\"adapters\":" + build_adapters_array(l, &*air.value) + "}");
    }
    PacketEventTrace packet_trace("tx");
    air.value->set_packet_trace(&packet_trace);
    auto bindings = BindingSet::create(l.cfg);
    if (!bindings) {
        wb_logf("binding error: %s\n", bindings.error.c_str());
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
        wb_logf("calibrate: adapter \"%s\" reports no EFUSE identity — "
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
    // §10.5/§10.6 (Pass 171): and whether the lever exists on the silicon, not
    // just in the wiring. Read here for the same reason as the line above —
    // before anything can start a run against it.
    if (const auto ap = air.value->tx_power_applied(calib_tx_idx)) {
        tx.set_power_actuator(ap->actuator);
    }
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
            wb_logf("calibrate: artifact write refused — no adapter "
                    "identity (Pass 154 D3)\n");
            return;
        }
        const uint8_t fp = calib_store_write(
            l.cfg.policy.calibration.artifact_dir, calib_ident, art);
        if (fp == 0) {
            wb_logf("calibrate: artifact write FAILED (%s)\n",
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
        wb_logf("calibrate: artifact persisted fp=0x%02x\n", fp);
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
                wb_logf("calibrate: boot auto-load fp=0x%02x (%s)\n",
                        stored.value->fingerprint, ident.c_str());
            }
            tx.calib_fingerprint_ = stored.value->fingerprint;
            last_artifact = stored.value->artifact;  // §15.5 GET surface
        } else {
            tx.calib_stale_ = true;  // §10.6: surface, never apply
            wb_logf("calibrate: STALE artifact (stored %s, live %s%s)\n",
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
            wb_logf("tx: frame-shm '%s' attached\n",
                    s.bind.name.c_str());
        } else {
            wb_logf("tx: frame-shm '%s' not up yet (%s); retrying\n",
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
    // Pass 174: the status snapshot's own cadence, deliberately independent
    // of stats.hz so a node with §15.3 output disabled still informs its
    // embedder.
    uint64_t next_status = t0;
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
            if (!(mode_apply && mode_apply(l.cfg.venc.mode_apply_cmd, name))) {
                return false;
            }
            active_mode = name;  // optimistic; the applier is authoritative
            wb_logf("vcmd: MODE[%u] -> %s (applier %s)\n", arg,
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
            wb_logf("control: %s\n", cs.error.c_str());
            return 1;
        }
        control = std::move(*cs.value);
        // Pass 178: the socket's answer, published only after a successful
        // bind. See the RX twin.
        if (runtime_info != nullptr && !control->bound_endpoint().empty()) {
            runtime_info->publish_control_endpoint(control->bound_endpoint());
        }
        ControlHandlers h;
        h.stats_line = [&] { return stats_line_string(emitter); };
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
                wb_logf("power: §10.5 override cleared (auto)\n");
                return "";
            }
            if (qdb < -511 || qdb > 511)
                return "qdb out of range (-511..511)";
            // §10.5 (Pass 171): refuse rather than return 200 for a move the
            // chip cannot make. Checked after the `auto` branch on purpose —
            // clearing a latch is meaningful whatever the hardware does.
            const auto ap = air.value->tx_power_applied(calib_tx_idx);
            if (ap && !ap->actuator)
                return "this node's role:\"tx\" adapter has no TX-power "
                       "actuator (§10.5); the offset would report applied and "
                       "move nothing";
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
            // §10.5 (Pass 169): the craft is the node whose rail was measured
            // (docs/findings.md 2026-08-14), so this half is the one a ground
            // stepping power over §11.7 depends on.
            return build_tx_power_json(
                tx.power_override(), air.value->tx_power_applied(calib_tx_idx),
                l.cfg.air.kind == AirCfg::Kind::kRadio);
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
            wb_logf("venc: reassert — actuator cache dropped, "
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
            // Shared with the Pass 174 status snapshot — one builder, one
            // predicate, so the two surfaces cannot report opposite booleans
            // for the same node (2026-08-14 review; app/main.cpp's hand copy
            // had already drifted once).
            return build_mode_json(
                active_mode,
                mode_apply_configured(l.cfg.venc.mode_apply_cmd,
                                      bool(mode_apply)));
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
            if (!mode_apply) {
                // Distinct from a failed launch: nothing was attempted, and
                // nothing could be. apply_configured already says so.
                return "this node cannot apply modes";
            }
            if (!mode_apply(l.cfg.venc.mode_apply_cmd, name)) {
                return "failed to launch mode applier";
            }
            active_mode = name;  // optimistic; the applier is authoritative
            wb_logf("mode: applying \"%s\" via %s\n", name.c_str(),
                    l.cfg.venc.mode_apply_cmd.c_str());
            return "";
        };
        // §15.5 Pass 104: GET /api/v1/modes — the catalog. modes_dir defaults to
        // the directory holding mode_apply_cmd (§16 co-locates them).
        h.modes_list = [&, modes_dir = mode_catalog_dir(l.cfg.venc)]() {
            return modes_catalog_json(
                modes_dir, active_mode,
                !l.cfg.venc.mode_apply_cmd.empty() && bool(mode_apply));
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
            wb_logf("channel: local retune -> %u MHz (Pass 113)\n",
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
            wb_logf("psk: pairing %s (Pass 113)\n",
                    enabled ? "locked" : "open");
            return "";
        };
        control->set_handlers(std::move(h));
        wb_logf("control: REST on %s (tx)\n",
                l.cfg.control.bind.c_str());
    }
    // §10.5 (Pass 150): forced safe boot offset on every role:"tx" adapter,
    // applied before the first frame goes out — a node must never transmit at
    // the uncharacterised efuse default even briefly.
    tx.apply_boot_power_offsets();
    wb_logf("tx: session=%u, running%s\n", session,
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
                    wb_logf("csa: armed -> %u MHz (nonce %u, dt %u ms)\n",
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
                    wb_logf("vcmd: applied %s=%u (nonce %u, from %u)\n",
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
    // Pass 174 (2026-08-14 review fix): ONE lambda builds the status object
    // so the 1 Hz publish, the §9.10 wedge exit and the clean stop all emit
    // the same shape — the wedge exit used to return ABOVE the publish site,
    // so the final readable status never carried the wedge that ended the
    // run. Field semantics: mode mirrors §15.5 GET /mode (shared builder),
    // wedge mirrors §9.10 (progress_proven = Pass 170's visible inert
    // watchdog), claimed/claimed_by mirror §11.4.
    const auto publish_status_now = [&](uint64_t at_ms) {
        if (runtime_info == nullptr) return;
        const std::optional<uint16_t> latched = csa.latched_issuer();
        std::string s = "{\"session\":" + std::to_string(session);
        s += ",\"channel\":" + std::to_string(cur_chan);
        s += ",\"csa\":\"" + std::string(csa.state_str()) + "\"";
        s += ",\"claimed\":";
        s += latched.has_value() ? "true" : "false";
        s += ",\"claimed_by\":" + std::to_string(latched.value_or(0));
        s += ",\"mode\":" +
             build_mode_json(active_mode,
                             mode_apply_configured(l.cfg.venc.mode_apply_cmd,
                                                   bool(mode_apply)));
        s += ",\"wedge\":{\"enabled\":";
        s += wedge.enabled() ? "true" : "false";
        s += ",\"progress_proven\":";
        s += wedge.progress_proven() ? "true" : "false";
        s += ",\"wedged\":";
        s += wedge.wedged() ? "true" : "false";
        s += ",\"consecutive\":" + std::to_string(wedge.consecutive_wedged());
        s += ",\"windows\":" + std::to_string(wedge.wedge_windows());
        s += "}}";
        runtime_info->publish_status(std::move(s));
        next_status = at_ms + 1000;
    };
    while (stop == 0) {
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
                wb_logf("csa: retune to %u MHz FAILED\n",
                        ca.chan_mhz);  // Pass 69: never silent
            } else {
                tx.reassert_power();  // §10.5: retune may reset power
                tx.on_rf_environment(ca.chan_mhz, ca.bw, now);
                cur_chan = ca.chan_mhz;
                cur_bw = ca.bw;
            }
            wb_logf("csa: %s -> %u MHz\n", csa.state_str(),
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
            wb_logf("mtu: claim released/changed -> Default\n");
        }
        prior_bound_issuer = bound_now;
        if (now >= next_shm_identity_check_ms) {
            for (ShmIn& si : shm_ins) {
                if (si.ring && !si.ring->backing_object_current()) {
                    wb_logf("tx: frame-shm '%s' producer replaced; "
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
                    wb_logf("tx: frame-shm '%s' attached\n",
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
                wb_logf("csa: RX SILENT %u ms after retune to %u MHz — "
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
                wb_logf("%s", wedge.wedged()
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
                wb_logf("air: TX WEDGED for %u consecutive windows — "
                        "exiting for supervisor re-exec (§9.10 v2)\n",
                        wedge.consecutive_wedged());
                // Pass 174 review fix: the terminal snapshot must carry the
                // wedge that ended the run — this return used to sit above
                // the publish site, so a supervisor reading the final status
                // after WBLINK_TX_WEDGED recorded a healthy node.
                publish_status_now(now);
                return kTxWedged;
            }
        }
        if (runtime_info != nullptr && now >= next_status) {
            publish_status_now(now);
            // Pass 172 review fix (2026-08-14): the adapters object carries
            // the LIVE channel, so it republishes on the same cadence — the
            // caps fields are static, only `channel` moves between publishes.
            runtime_info->publish_adapters(
                "{\"adapters\":" + build_adapters_array(l, &*air.value) + "}");
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
            // Pass 176: the same two strings the control server serves,
            // readable through the C ABI — one fill path, three transports.
            if (runtime_info != nullptr) {
                runtime_info->publish_stats(stats_line_string(emitter));
                runtime_info->publish_health(build_health_json(last_snap));
            }
            next_stats = now + stats_period;
        }
    }
    // Pass 174 review fix: a final snapshot on the clean-stop path too, so
    // the last readable status is the state the run actually ended in.
    publish_status_now(now_ms());
    return 0;
}

}  // namespace node
}  // namespace wblink
