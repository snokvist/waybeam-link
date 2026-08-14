// SPDX-License-Identifier: GPL-2.0-or-later
// The RX node's run loop, moved verbatim out of app/main.cpp
// (#109 Phase 2c step 2). See node/include/wblink/node/rx_node.h for what the
// move cost and what it deliberately does not yet fix.
//
// The include list is app/main.cpp's, minus the headers only the TX and
// loopback loops need. Every one of them was reaching this code transitively
// through that 8.8k-line translation unit; declaring them is the same one-time
// tax Phase 2a paid on every move (eleven undeclared includes in TxCore alone).
#include "wblink/node/rx_node.h"

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

#include "wblink/node/aim.h"
#include "wblink/node/air_backend.h"
#include "wblink/node/clock.h"
#include "wblink/node/discovery.h"
#include "wblink/node/entropy.h"
#include "wblink/node/frame_kind.h"
#include "wblink/node/policy.h"
#include "wblink/node/rx_core.h"
#include "wblink/log.h"
#include "wblink/node/rx_runtime_control.h"
#include "wblink/node/tx_core.h"
#include "wblink/node/uplink_power.h"
#include "wblink/node/vcmd.h"

#if WBLINK_RADIO
#include "wblink/air_radio.h"
#endif

namespace wblink {
namespace node {

int run_rx(const Loaded& l, const std::atomic<int>& stop,
           const FrameSink& frame_out) {
    return run_rx(l, stop, frame_out, nullptr);
}

int run_rx(const Loaded& l, const std::atomic<int>& stop,
           const FrameSink& frame_out, RxRuntimeControl* runtime_control) {
    auto air = AirBackend::create(l.cfg);
    if (!air) {
        std::fprintf(stderr, "air error: %s\n", air.error.c_str());
        return 1;
    }
    PacketEventTrace packet_trace("rx");
    air.value->set_packet_trace(&packet_trace);
    // §15.5 (Pass 172): publish the per-die capability answers the moment the
    // backend can state them. Static for the life of the run, so this one
    // publish serves every later wblink_rx_adapters copy — including on an
    // embedder built WBLINK_CONTROL_SERVER=OFF, where it is the ONLY surface.
    if (runtime_control != nullptr) {
        runtime_control->publish_adapters(
            "{\"adapters\":" + build_adapters_array(l, &*air.value) + "}");
    }
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
    // Read only by the §15.5 tier handler, which is compiled out with the
    // control server; the value is still the right one to keep here.
    [[maybe_unused]] const int32_t cp_max_qdb =
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
    // §15.2 `bind.kind: "frame-shm"` is the WHOLE-FRAME egress kind: blocks
    // are reassembled into a frame and the frame is handed on. Where it is
    // handed is what B10 (#109) made a choice rather than a constant — a
    // caller-supplied sink takes those streams instead of a ring, which is
    // what lets a node run on a build with no frame-SHM at all. UDP-bound
    // out-streams are untouched by the sink: they are datagram egress, a
    // different thing, and hijacking them would surprise a consumer that
    // wanted only its video.
    struct FrameOut {
        uint8_t stream_id;
        // Already bound to this stream, so the two destinations look the same
        // at the call site — the public FrameSink carries a stream_id the
        // caller needs and this one does not.
        std::function<void(const uint8_t*, size_t)> sink;
        std::unique_ptr<FrameReassembler> reasm;
        std::optional<StreamKey> source;
        // §15.3 frame counters for a sink-backed stream. The ring keeps these
        // for itself (FrameShmRing::note_frame), and stats.cpp emits the
        // fields UNCONDITIONALLY — so without a local copy a live callback
        // egress reports frame_count 0 and frame_interval_us 0, which reads as
        // the stall it is not. Same shape and same units as the ring's, so a
        // consumer cannot tell which egress produced them.
        bool count_locally = false;
        FrameShmRing::Stats counters;
        uint64_t last_frame_us = 0;
        uint64_t previous_interval_us = 0;
        uint64_t jitter_q4_us = 0;
#if WBLINK_FRAME_SHM
        // Owned only so the ring outlives the sink that writes it, and read
        // for the §15.3 counters, which have no meaning for a callback.
        std::unique_ptr<FrameShmRing> ring;
#endif
    };
    std::vector<FrameOut> frame_outs;
    for (const StreamCfg& s : l.cfg.streams) {
        if (s.dir != Dir::kOut || s.bind.kind != BindKind::kFrameShm) {
            continue;
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
        FrameOut fo;
        fo.stream_id = s.stream_id;
        fo.reasm = std::make_unique<FrameReassembler>(frc);
        if (frame_out) {
            const uint8_t sid = s.stream_id;
            fo.sink = [&frame_out, sid](const uint8_t* f, size_t len) {
                frame_out(sid, f, len);
            };
            fo.count_locally = true;
            std::fprintf(stderr, "rx: frame egress stream %u -> caller sink\n",
                         s.stream_id);
        } else {
#if WBLINK_FRAME_SHM
            auto r = FrameShmRing::create(s.bind.name);
            if (!r) {
                std::fprintf(stderr, "frame-shm egress '%s': %s\n",
                             s.bind.name.c_str(), r.error.c_str());
                return 1;
            }
            fo.ring = std::move(*r.value);
            // The pointee address survives the vector reallocating under
            // push_back; the unique_ptr moves, the ring does not.
            FrameShmRing* ring = fo.ring.get();
            fo.sink = [ring](const uint8_t* f, size_t len) {
                ring->write_frame(f, len);
            };
            std::fprintf(stderr, "rx: frame-shm egress '%s' created\n",
                         s.bind.name.c_str());
#else
            // Fail closed. A node that silently dropped its video because the
            // build lacked a subsystem its config asks for would look like a
            // link fault, and be debugged as one.
            std::fprintf(stderr,
                         "frame-shm egress '%s' configured, but this build has "
                         "WBLINK_FRAME_SHM=OFF and no frame sink was supplied\n",
                         s.bind.name.c_str());
            return 1;
#endif
        }
        frame_outs.push_back(std::move(fo));
    }
    uint64_t deliver_now = now_ms();

    // §14.3 cache roles (v1 IP transport; both optional, either or both).
    std::unique_ptr<CacheController> cache_ctl;
    std::unique_ptr<CacheUdp> cache_repair_sock;
    std::map<uint16_t, CacheEndpoint> cache_endpoints;  // originator -> ep
    FrameReassembler* cache_reasm = nullptr;
    FrameOut* cache_out = nullptr;
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
        for (FrameOut& fo : frame_outs) {  // config validated the stream exists
            if (fo.stream_id == cr.stream_id) {
                cache_reasm = fo.reasm.get();
                cache_out = &fo;
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
    const auto write_egress = [&](FrameOut& fo, const uint8_t* f, size_t len) {
        fo.sink(f, len);
        if (fo.count_locally) {
            // Mirrors FrameShmRing::note_frame, including the fixed-point
            // J += (variation - J)/16 jitter, so the §15.3 numbers mean the
            // same thing on both egress paths.
            const uint32_t size = static_cast<uint32_t>(len);
            FrameShmRing::Stats& c = fo.counters;
            ++c.writes;
            c.frame_bytes += len;
            c.frame_size_last = size;
            if (c.frame_size_min == 0 || size < c.frame_size_min) {
                c.frame_size_min = size;
            }
            if (size > c.frame_size_max) c.frame_size_max = size;
            const uint64_t t = now_us();
            if (fo.last_frame_us != 0) {
                const uint64_t interval = t - fo.last_frame_us;
                c.frame_interval_us = interval;
                if (fo.previous_interval_us != 0) {
                    const uint64_t variation =
                        interval > fo.previous_interval_us
                            ? interval - fo.previous_interval_us
                            : fo.previous_interval_us - interval;
                    const uint64_t current = (fo.jitter_q4_us + 8u) >> 4u;
                    if (variation >= current) {
                        fo.jitter_q4_us += variation - current;
                    } else {
                        fo.jitter_q4_us -= current - variation;
                    }
                    c.frame_jitter_us = (fo.jitter_q4_us + 8u) >> 4u;
                }
                fo.previous_interval_us = interval;
            }
            fo.last_frame_us = t;
        }
        VencFrameMeta meta;
        if (read_frame_meta(f, len, &meta) &&
            (meta.flags & kFrameFlagIdr) != 0) {
            rx.note_egress_irap(fo.stream_id);
        }
    };
    const RxEngine::Deliver deliver = [&](uint8_t sid, uint32_t block_id,
                                          uint8_t flags, const uint8_t* d,
                                          size_t n) {
        for (FrameOut& fo : frame_outs) {
            if (fo.stream_id == sid) {
                fo.reasm->push(block_id, flags, d, n, deliver_now,
                               [&](const uint8_t* f, size_t len) {
                                   write_egress(fo, f, len);
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
        for (FrameOut& fo : frame_outs) {
            if (fo.stream_id != sid) {
                continue;
            }
            if (!fo.source || !(*fo.source == source)) {
                fo.reasm->reset_stream();
                fo.source = source;
            }
            const bool complete = fo.reasm->push(
                block_id, flags, d, n, deliver_now,
                [&](const uint8_t* f, size_t len) {
                    write_egress(fo, f, len);
                });
            return RxEngine::EarlyDeliverResult{/*handled=*/true, complete};
        }
        return {};
    };
    const auto apply_selection = [&](const LinkSelection& selected) {
        rx.select_originator(selected.originator);
        for (FrameOut& fo : frame_outs) {
            fo.reasm->reset_stream();
            fo.source.reset();
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
                    write_egress(*cache_out, f, len);
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
    // Pass 172 (2026-08-14 review fix): the adapters snapshot carries the
    // LIVE channel, which CSA, craft-local retunes and scout dwells all move
    // — a one-shot publish would freeze it for the life of the run while
    // /info kept reporting the truth. Republished at 1 Hz; the caps fields
    // are static, so only `channel` ever changes between publishes.
    uint64_t next_adapters_pub = t0 + 1000;
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
    // §11.7 issuers, held as NAMED locals rather than reached through `h`,
    // because ControlHandlers is std::move()d into the server once every
    // handler is registered, so a lambda that captures `h` by reference and
    // dereferences a sibling member at CALL time reads the moved-from husk.
    //
    // They are declared HERE, at run_rx scope, and NOT inside the control
    // block below (Pass 166). The server outlives that block and dispatches
    // from the main loop, so a handler capturing a block-scoped local by
    // reference reads a DESTROYED std::function — ASan `stack-use-after-scope`,
    // and plain UB in a release build. Device-caught on the .242 ground the
    // first time §11.7 0x0A `both:true` became reachable on an RF node; `main`
    // at 02d6145 aborts identically on a udp-air uplink, so the bug predates
    // Pass 166 and was merely shielded by Pass 165's blanket 409.
    //
    // `issue_vcmd` joined it here when the assignments were hoisted for the
    // C ABI: it is the UNTYPED entry point (it refuses the §15.5 commands that
    // require a typed REST endpoint), which is what both the generic
    // /vehicle/command handler and a queued C caller need.
    std::function<std::pair<int, std::string>(const std::string&, int)>
        start_vehicle_command;
    std::function<std::pair<int, std::string>(const std::string&, int)>
        issue_vcmd;
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

    // One implementation for both the REST handler and the queued C ABI. The
    // engine and every hook it owns are RX-loop state; callers outside this
    // function may only enqueue an intent through RxRuntimeControl.
    const auto start_scout_sweep = [&](const std::vector<uint16_t>& requested,
                                       uint32_t dwell_ms) -> std::string {
        if (cache_assignment_gate) {
            return "controlled cache cannot scout independently";
        }
        cancel_calibration("scout sweep");
        std::vector<uint16_t> channels = requested;
        if (channels.empty()) channels = l.cfg.scout.channels;
        if (channels.empty()) channels = l.cfg.policy.csa.channel_allowlist;
        scout.set_rest_chan(operating_chan);
        scout.set_rest_filter(active_selection.net_id);
        const uint16_t selected_originator = active_selection.originator;
        // preferred_originator is an operator pin even when the stream wants
        // are wildcarded (Android's passive sink is), so RxCore's selected
        // originator can still be zero here. Preserve that explicit trust.
        const uint16_t trusted_originator =
            l.cfg.node.preferred_originator != 0
                ? l.cfg.node.preferred_originator
                : ((selection_state == "tuned" ||
                    selection_state == "committed")
                       ? selected_originator
                       : 0);
        scout.set_trusted_rest_originator(
            trusted_originator != 0
                ? std::optional<uint16_t>(trusted_originator)
                : std::nullopt);
        return scout.start(channels,
                           dwell_ms ? dwell_ms : l.cfg.scout.dwell_ms,
                           now_ms());
    };
    const auto stop_scout_sweep = [&]() { scout.stop(now_ms()); };

    // §2/§13 passive spectator selection. This intentionally resolves through
    // ScoutEngine::candidate_for rather than trusting one public JSON row: the
    // engine retains per-channel frame evidence and chooses the heard-most
    // channel, which rejects retune-settling leakage onto an adjacent channel.
    const auto select_scout_candidate = [&](uint16_t originator) -> std::string {
        if (!l.cfg.node.spectator) {
            return "passive scout selection requires node.spectator";
        }
        if (cache_assignment_gate) {
            return "controlled cache cannot select independently";
        }
        if (originator == 0) return "invalid originator";
        const auto candidate = scout.candidate_for(originator);
        if (!candidate) return "unknown craft (run a scout first)";
        const auto live_session = discovery.session_for(originator);
        if (live_session && *live_session != candidate->session) {
            return "stale candidate (craft rebooted since scout) — re-scout";
        }
        // A selection is what the sweep is for. Do not call stop(), whose
        // restore would make an unnecessary round trip immediately before this
        // retune; abandon still folds the current dwell.
        if (scout.scanning()) scout.abandon(now_ms());
        air.value->set_stamp_net_id(candidate->net_id);
        air.value->set_filter_net_id(candidate->net_id);
        if (!air.value->retune_all(candidate->chan, op_bw_mhz, false)) {
            air.value->set_stamp_net_id(active_selection.net_id.value_or(0));
            air.value->set_filter_net_id(active_selection.net_id);
            return "failed to tune onto feed channel";
        }
        active_selection =
            LinkSelection{originator, candidate->chan, 0, candidate->net_id};
        operating_chan = candidate->chan;
        selection_state = "tuned";
        scout.set_rest_chan(operating_chan);
        scout.set_rest_filter(active_selection.net_id);
        std::fprintf(stderr,
                     "spectator: tuned originator=%u net_id=%u %u MHz\n",
                     originator, candidate->net_id, candidate->chan);
        return "";
    };
    const auto build_selection_json = [&]() {
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
    // §11.4 claim and the §11.7 command campaign, hoisted to run_rx scope
    // for the reason start_scout_sweep states above: ONE implementation for
    // both the REST handler and the queued C ABI. They were block-local to
    // the control-server branch until the Android consumer needed them, and
    // a second copy behind WBLINK_CONTROL_SERVER=OFF is exactly the drift
    // this layer exists to prevent.
    //
    // Each one now carries its OWN cache-assignment refusal. It used to be
    // the `if (!cache_assignment_gate)` that wrapped handler REGISTRATION
    // below, so hoisting them out from under it would have silently handed a
    // controlled cache the independent claim/CSA surface that guard exists to
    // deny — see its comment there. start_scout_sweep already self-guards the
    // same way; this matches it rather than inventing a second pattern.
    // §15.5a claim: CSA-grab a scouted craft. Re-keys the issuer with the
    // craft's key (configured secret, or the cached announced token §11.4a),
    // binds the link to its net_id, moves onto its channel to be heard, then
    // issues a §11 campaign to target_chan (0 → the emptiest allowlisted
    // channel). The loop's issuer.tick drives the copies/commit; post-claim
    // the net_id stamp/filter and channel hold until an explicit re-scout.
    const auto do_claim = [&](int originator_i, int target_chan_i) -> std::string {
        // Was the enclosing `if (!cache_assignment_gate)` around handler
        // registration until this lambda was hoisted; see the header comment.
        if (cache_assignment_gate) {
            return "controlled cache cannot claim independently";
        }
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
            return select_scout_candidate(orig);
        }
        if (!air.value->supports_csa()) {
            return "CSA unsupported by this backend";
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
        // KEYING HAPPENS BEFORE THE RETUNE, and the history matters because it
        // moved the other way first. §11.4a's announced token is cached by
        // `DiscoveryCatalog` from ANY dwell the scout spends on the craft's
        // channel, so by the time a claim is possible at all — it needs a scout
        // candidate, refused above — the key is already in hand. The 2026-08-13
        // ruling to key AFTER the retune was made on a premise that turned out
        // to be wrong: the token was believed to reach the catalog only from
        // resting-channel discovery. It never did; the token was being cached
        // from the sweep and then AGED OUT with the presence view, by the
        // ground polling its own discovery snapshot (`discovery.h`, and
        // `node_discovery_test.cpp` holds the reproduction). With the cache
        // fixed, retuning first buys nothing and costs a key-less claim a
        // retune out and back — `token_for` is synchronous and cannot wait for
        // a 2 Hz announce, which is exactly why the retune-first build still
        // refused on hardware.
        //
        // So every refusal that needs no radio stays above the retune and stays
        // cheap; `claim_rollback` covers the one failure that can still happen
        // after the ears have moved.
        //
        // Configured secret wins; else the cached announced token (§11.4a).
        std::vector<uint8_t> key = cparams.psk;
        if (key.empty()) {
            const auto tok = discovery.token_for(orig);
            if (!tok) return "no CSA key for craft (never announced one)";
            key.assign(tok->begin(), tok->end());
        }
        if (!issuer.set_psk(key)) return "claim busy (campaign active)";
        if (!vissuer.set_psk(key)) {
            return "claim busy (command campaign active)";
        }
        // §15.5a (Pass 144): a claim is what a sweep is *for*, so it ends the
        // sweep instead of racing it. Left running, the sweep finishes seconds
        // later and its rest() restores the resting channel and net_id filter
        // *over* the claim, with the campaign already in flight — the scout
        // clobbering the operator's click.
        //
        // THIS MUST STAY DIRECTLY ABOVE THE RETUNE. abandon() deliberately
        // skips rest(), on the contract that the caller is about to retune and
        // re-pin the filter itself — so any refusal BETWEEN the abandon and the
        // retune ends the sweep and leaves every ear parked on the sweep's last
        // dwell with the filter still widened, i.e. no video until something
        // else retunes. Keying moved above the retune and took this with it for
        // one commit; the three refusals just above (no key, and either issuer
        // busy) were reachable in exactly that stranded state.
        if (scout.scanning()) {
            scout.abandon(now_ms());
        }
        // §15.5a: bind the link to the craft's net_id and move all ears onto
        // its current channel so the campaign and CSA_ARMED return are heard.
        const auto claim_rollback = [&] {
            air.value->set_stamp_net_id(active_selection.net_id.value_or(0));
            air.value->set_filter_net_id(active_selection.net_id);
            air.value->retune_all(active_selection.chan, op_bw_mhz, false);
        };
        air.value->set_stamp_net_id(cand->net_id);
        air.value->set_filter_net_id(cand->net_id);
        if (!air.value->retune_all(cand->chan, op_bw_mhz, false)) {
            claim_rollback();
            return "failed to retune onto craft channel";
        }
        // retune_class 1 (500 ms dt budget) gives the craft slack for the iw
        // shell-out retune before its §11.5 verify timeout. bw code 0 = 20 MHz
        // (v1 single-width, matching the /csa trigger).
        const CommonPrefix pre{l.cfg.node.originator, 0, session};
        if (!issuer.start(pre, target, 0, 1, cand->chan, 0, 4, now_us_it)) {
            claim_rollback();
            // Named precisely: this is also what a target outside
            // policy.csa.channel_allowlist returns, and an empty allowlist
            // denies every channel — which cost a bench round to work out.
            return "rejected (target not allowlisted, campaign active, or "
                   "rate-limit)";
        }
        previous_selection = active_selection;
        previous_selection_state = selection_state;
        pending_selection = LinkSelection{orig, target, 0, cand->net_id};
        selection_state = "claiming";
        std::fprintf(stderr, "csa: claim originator=%u net_id=%u %u->%u MHz\n",
                     orig, cand->net_id, cand->chan, target);
        return "";
    };

    start_vehicle_command = [&](const std::string& cmd, int arg)
        -> std::pair<int, std::string> {
        // Same refusal as do_claim's, and for the same reason: a controlled
        // cache must not command the craft its receiver owns.
        if (cache_assignment_gate) {
            return {409,
                    "{\"ok\":false,\"error\":\"controlled cache cannot issue "
                    "vehicle commands\"}"};
        }
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

    // §11.7 campaign state (§15.5 /vehicle/command GET). Hoisted with the two
    // above because the queued C caller needs the same readout the REST client
    // polls — it is the only place a campaign's outcome becomes visible, since
    // a command that was accepted still has to be ACKed by the craft.
    const auto command_campaign_json = [&] {
        return std::string("{\"nonce\":") + std::to_string(vissuer.nonce()) +
               ",\"cmd\":\"" + vcmd_name_for(vissuer.cmd_id()) +
               "\",\"arg\":" + std::to_string(vissuer.cmd_arg()) +
               ",\"state\":\"" + vissuer.state_str() + "\"}";
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
    // §15.5 REST control plane. RX/ground node owns the CSA trigger (replaces
    // the removed stdin trigger); profile/fec are TX-only knobs → null → 409.
    StatsSnapshot last_snap;
#if !WBLINK_CONTROL_SERVER
    // Fail closed, for the same reason the frame-shm branch does: a node whose
    // control plane silently was not there is debugged as a network fault.
    if (!l.cfg.control.bind.empty()) {
        std::fprintf(stderr,
                     "control: '%s' configured, but this build has "
                     "WBLINK_CONTROL_SERVER=OFF\n", l.cfg.control.bind.c_str());
        return 1;
    }
#else
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
        h.scout_results = [&] { return scout.results_json(now_ms()); };
        h.selection_json = build_selection_json;
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
        // A controlled cache is a follower of its receiver.  It must not expose
        // an independent scout/claim/CSA control surface that could split it
        // from the receiver's committed selection.
        if (!cache_assignment_gate) {
            // do_claim is captured BY VALUE. It lives at run_rx scope now, so
            // this no longer guards against a dangling block-local — but the
            // handlers are moved into `control` and invoked from the event
            // loop, so copying stays the honest expression of that lifetime.
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
                return start_scout_sweep(chans, dwell);
            };
            h.scout_stop = [&]() -> std::string {
                stop_scout_sweep();
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
                // §10.5 (Pass 171): refuse rather than return 200 for a move
                // the chip cannot make. After the `auto` branch on purpose —
                // clearing a latch is meaningful whatever the hardware does.
                const auto ap = air.value->tx_power_applied(uplink_idx);
                if (ap && !ap->actuator) {
                    return "this node's uplink adapter has no TX-power "
                           "actuator (§10.5); the offset would report applied "
                           "and move nothing";
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
                return build_tx_power_json(
                    upwr.override_qdb, air.value->tx_power_applied(uplink_idx),
                    l.cfg.air.kind == AirCfg::Kind::kRadio);
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
            // `issue_vcmd` is declared and assigned at run_rx scope — see the
            // comment at its declaration. It used to live here, which made
            // every handler that captured it by reference read a destroyed
            // object once this block exited (the earlier symptom was
            // `both:true` reporting "no vehicle-command path on this node" on
            // a ground that plainly had one; the sharper one is an ASan abort).
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
                // §10.5/§10.7 (Pass 171): a run walks rungs in offset space
                // and attributes what the air did to the knob. On a chip with
                // no knob every rung commands the same power, so the seek
                // reads flat and PERSISTS noise as an artifact — which every
                // later resolve, pairing pass and boot then believes. Refuse.
                if (const auto ap = air.value->tx_power_applied(uplink_idx);
                    ap && !ap->actuator) {
                    return "uplink adapter has no TX-power actuator (§10.5) — "
                           "a calibration would persist noise as an artifact";
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
            h.vehicle_command_json = command_campaign_json;
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
            for (FrameOut& fo : frame_outs) {
                fo.reasm->reset_stats();
                fo.counters = FrameShmRing::Stats{};
                fo.last_frame_us = 0;
                fo.previous_interval_us = 0;
                fo.jitter_q4_us = 0;
#if WBLINK_FRAME_SHM
                if (fo.ring) fo.ring->reset_stats();
#endif
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
#endif  // WBLINK_CONTROL_SERVER
    std::fprintf(stderr, "rx: session=%u, %zu adapters, running%s\n",
                 session, air.value->rx_adapters(),
                 qg.enabled() ? " (quiet-gap returns)" : "");
    while (stop == 0) {
        // One timestamp per iteration (see run_tx): callbacks and tick share
        // it so core-injected time never steps backward.
        const uint64_t now = now_ms();
        now_us_it = now_us();
        deliver_now = now;  // the deliver lambda's clock for reassembler pushes
        if (runtime_control != nullptr) {
            if (auto command = runtime_control->take_command()) {
                std::string error;
                // Empty unless this command produced a synchronous verdict a
                // queued caller would otherwise never see (§11.7 only — the
                // others report through the scout/selection snapshots).
                std::string command_verdict;
                switch (command->kind) {
                    case RxRuntimeControl::CommandKind::kScoutStart:
                        error = start_scout_sweep(command->channels,
                                                  command->dwell_ms);
                        break;
                    case RxRuntimeControl::CommandKind::kScoutStop:
                        stop_scout_sweep();
                        break;
                    case RxRuntimeControl::CommandKind::kScoutSelect:
                        // A queued caller cannot receive the helper's error
                        // synchronously. Publish an outcome coupled to this
                        // generation instead; otherwise a failed repeat-select
                        // of the same originator can masquerade as the prior
                        // "tuned" snapshot under a newer generation.
                        selection_state = "selecting";
                        error = select_scout_candidate(command->originator);
                        if (!error.empty()) selection_state = "select_failed";
                        break;
                    case RxRuntimeControl::CommandKind::kClaim:
                        // do_claim leaves selection_state at "claiming" on
                        // success and untouched on refusal, so unlike select
                        // there is nothing to stage here. The claim's real
                        // outcome lands later, when the campaign commits or
                        // rolls back — the selection snapshot is where a
                        // consumer watches for it.
                        error = do_claim(command->originator,
                                         command->target_chan);
                        break;
                    case RxRuntimeControl::CommandKind::kVehicleCommand: {
                        // The untyped entry point, matching what a REST client
                        // reaches through /vehicle/command: commands that
                        // §15.5 restricts to a typed endpoint are refused
                        // rather than silently issued with a bare integer.
                        const auto [code, body] =
                            issue_vcmd(command->cmd, command->arg);
                        command_verdict =
                            "{\"code\":" + std::to_string(code) +
                            ",\"result\":" + body + "}";
                        if (code != 200) error = body;
                        break;
                    }
                }
                runtime_control->note_applied(command->generation);
                if (!error.empty()) {
                    // wb_logf, not fprintf(stderr): this is the ONLY place a
                    // queued command's refusal reason exists, and an
                    // in-process consumer cannot see stderr. Measured on an
                    // S22 — a claim that the mailbox accepted and the loop
                    // then refused looked identical to one that was
                    // transmitted and ignored by the craft. The rest of this
                    // file still uses fprintf and is equally invisible there;
                    // this is the line the C ABI made load-bearing.
                    wb_logf("rx runtime control generation %llu: %s\n",
                            static_cast<unsigned long long>(
                                command->generation),
                            error.c_str());
                }
                // Publish the state represented by this applied generation
                // immediately. A consumer can distinguish a queued command
                // from a stale pre-command snapshot without touching loop
                // state from its own thread.
                runtime_control->publish_scout(
                    scout.results_json(now), command->generation);
                runtime_control->publish_selection(
                    build_selection_json(), command->generation);
                // The campaign readout always carries the live §11.7 state;
                // `verdict` is present only when this generation was the one
                // that tried to start a campaign. A consumer that polls after
                // a kClaim therefore sees the campaign it did not issue, with
                // no verdict claiming it did.
                runtime_control->publish_command(
                    "{\"campaign\":" + command_campaign_json() +
                        (command_verdict.empty()
                             ? std::string()
                             : ",\"verdict\":" + command_verdict) +
                        "}",
                    command->generation);
            }
        }
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
        for (FrameOut& fo : frame_outs) {
            fo.reasm->tick(now, [&](const uint8_t* f, size_t len) {
                write_egress(fo, f, len);
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
        feedback.reserve(frame_outs.size());
        for (const FrameOut& fo : frame_outs) {
            feedback.emplace_back(fo.stream_id, fo.reasm->jscc_feedback());
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
        if (runtime_control != nullptr) {
            const uint64_t generation =
                runtime_control->applied_generation();
            if (runtime_control->take_scout_snapshot_request()) {
                runtime_control->publish_scout(scout.results_json(now),
                                               generation);
            }
            if (runtime_control->take_discovery_snapshot_request()) {
                runtime_control->publish_discovery(
                    discovery.json(now, rx.stream_keys()));
            }
            if (runtime_control->take_selection_snapshot_request()) {
                runtime_control->publish_selection(build_selection_json(),
                                                   generation);
            }
            if (runtime_control->take_command_snapshot_request()) {
                // No verdict here: this refresh belongs to no command. A
                // campaign's state advances on its own as the craft ACKs, and
                // that is what a poller between commands is watching.
                runtime_control->publish_command(
                    "{\"campaign\":" + command_campaign_json() + "}",
                    generation);
            }
        }
        if (runtime_control != nullptr && now >= next_adapters_pub) {
            runtime_control->publish_adapters(
                "{\"adapters\":" + build_adapters_array(l, &*air.value) + "}");
            next_adapters_pub = now + 1000;
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
#if WBLINK_CONTROL_SERVER
        if (control) {
            control->service(now);
        }
#endif
        if (stats_period != 0 && now >= next_stats) {
            // §6.3a: fold the per-out-stream reassembler counters into stats.
            std::vector<std::pair<uint8_t, FrameReassemblerStats>> frame_stats;
            std::vector<std::pair<uint8_t, FrameShmRing::Stats>> shm_stats;
            frame_stats.reserve(frame_outs.size());
            shm_stats.reserve(frame_outs.size());
            for (const FrameOut& fo : frame_outs) {
                frame_stats.emplace_back(fo.stream_id, fo.reasm->stats());
#if WBLINK_FRAME_SHM
                if (fo.ring) {
                    shm_stats.emplace_back(fo.stream_id, fo.ring->stats());
                    continue;
                }
#endif
                // A sink-backed stream reports the same fields from the
                // counters write_egress keeps; the ring-only ones (reads,
                // ring_full, the producer-health block) stay zero because
                // there is no ring to observe, not because nothing happened.
                if (fo.count_locally) {
                    shm_stats.emplace_back(fo.stream_id, fo.counters);
                }
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
#if WBLINK_CONTROL_SERVER
            if (control) {
                control->publish_stats(emitter.last_line());
            }
#endif
            next_stats = now + stats_period;
        }
    }
    if (runtime_control != nullptr) {
        const uint64_t generation = runtime_control->applied_generation();
        runtime_control->publish_scout(scout.results_json(now_ms()), generation);
        runtime_control->publish_discovery(
            discovery.json(now_ms(), rx.stream_keys()));
        runtime_control->publish_selection(build_selection_json(), generation);
        runtime_control->publish_command(
            "{\"campaign\":" + command_campaign_json() + "}", generation);
    }
    // §10.7: shutdown is an exit like any other. Without this a SIGTERM
    // mid-run leaves the uplink adapter at the last probe power until some
    // later start re-resolves the owner.
    cancel_calibration("shutdown");
    return 0;
}

}  // namespace node
}  // namespace wblink
