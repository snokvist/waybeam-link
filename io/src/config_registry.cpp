// SPDX-License-Identifier: GPL-2.0-or-later
// The declared §15.2 key surface. See wblink/config_registry.h for what this
// table is for and how it is kept honest.
//
// Generated once from the loader's accessor sites and thereafter maintained by
// hand: tests/config_registry_test.py fails the build if it drifts from
// io/src/config.cpp in either direction.
#include "wblink/config_registry.h"

#include <algorithm>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace wblink {
namespace {

using json = nlohmann::json;

// --- Liveness predicates (§15.2). ------------------------------------------
//
// Each encodes a gate that exists in the shipped code, cited here. A group
// whose gate cannot be pointed at gets NO predicate: reporting a live key as
// dead is worse than not reporting it at all.

// PROTOCOL.md:4510 — a spectator "generates no ARQ / NACK / LINK_REPORT
// (return and §3.9 recovery paths no-op with no tx adapter)". The gate is the
// TX adapter, not the spectator flag: the Ethernet cache archetype is also
// uplink-free and correctly omits policy.return, which keying on `spectator`
// would have missed. (Its config, deploy/cache-192.168.2.247.json, was deleted
// in Pass 164 -- the node was offline and could not be mirrored -- so the
// example is no longer checkable in-repo; the reasoning stands on the
// archetype, not the file.) Runtime: `iface()->has_tx()`, app/main.cpp:1657 / :3846.
//
// has_tx() is per BACKEND, and only the RF one follows the adapters
// (RadioAir::has_tx returns impl_->has_tx). UdpAir hardcodes
// `return true` — "the dev backend has an uplink by construction"
// (io/include/wblink/air_udp.h:119) — and udp configs carry no adapters at
// all, so an adapters-only predicate called every policy.return key inert on
// a live udp node. That is the false-inert this whole design exists to avoid;
// it was caught in review, and the udp/loopback cases are now claimed live.
bool has_uplink(const Config& c) {
    switch (c.air.kind) {
        case AirCfg::Kind::kUdp:
        case AirCfg::Kind::kUdpBroadcast:
            return true;  // air_udp.h:119
        case AirCfg::Kind::kNone:
            return true;  // loopback: no backend to point at, so claim nothing
        case AirCfg::Kind::kRadio:
            break;
    }
    for (const AdapterCfg& a : c.adapters) {
        if (a.role == Role::kTx) return true;
    }
    return false;
}

// io/src/config.cpp:1135-1145 — air.tx / air.rx / air.pace_mbps are read only
// inside the udp / udp-broadcast branch. Exact paths only: air.tx_retry_limit
// and air.rx_drop_permille merely share a prefix and are radio-backend keys.
bool air_is_udp(const Config& c) {
    return c.air.kind == AirCfg::Kind::kUdp ||
           c.air.kind == AirCfg::Kind::kUdpBroadcast;
}

// app/main.cpp:2143 — venc.enabled gates the bitrate/caps/ladder ACTUATOR,
// and nothing else. TxCore's constructor is where it is read.
bool venc_enabled(const Config& c) { return c.venc.enabled; }

// The §15.5 operating-mode catalog is a separate subsystem with a separate
// gate: `venc.mode_apply_cmd.empty()` at app/main.cpp:5083 (the §11.7 MODE
// command) and :8508 (`/api/v1/modes`). It works with venc.enabled false, so
// predicating the mode keys on venc.enabled reported live keys as dead on
// exactly the §16 craft layout that sets mode_apply_cmd — caught in review.
bool mode_configured(const Config& c) { return !c.venc.mode_apply_cmd.empty(); }

// app/main.cpp:6202 / :6254 — the repair and store halves are independent.
bool cache_repair_enabled(const Config& c) { return c.cache.repair.enabled; }
bool cache_store_enabled(const Config& c) { return c.cache.store.enabled; }

// Pass 164 stranded these: the kernel-monitor backend was their only reader.
// Not "inert on some backend" — inert everywhere, which is why the predicate
// takes no decision. Grep is the gate here: `adapter.ifname` has no consumer
// left in io/ or app/, and calib_store.cpp's calib_identity() no longer reads
// calib_id (§10.6 tiers 2-4 went with the backend; radio keys on the EFUSE
// MAC and has deliberately no fallback tier, Pass 154 D3).
bool never_live(const Config&) { return false; }

// §10.3/§11.7 0x0A (Pass 166): the tier's preset list is chosen by the node's
// actuation space, so exactly one of the two lists governs and the other is
// read by nothing but its own load-time clamp. The selection has ONE site —
// TxCore::set_backend_relative() promotes power_offset_presets_qdb into
// presets_qdb, and run_rx seeds UplinkPower from the offset pair — so the
// predicate is a restatement of `air.kind`, not a second model of it.
bool backend_is_relative(const Config& c) {
    return c.air.kind == AirCfg::Kind::kRadio;
}
bool backend_is_absolute(const Config& c) { return !backend_is_relative(c); }

// DO NOT PREDICATE adapters[].max_power_qdb.
// Pass 164's first cut declared both inert on the radio backend, reasoning
// that every reader sits on the `!backend_relative_` arm. That covers the
// craft TxCore sites and MISSES three that run on radio (line numbers drift —
// grep the expressions, they are unique):
//   `snap.link.tx_power_ceiling_qdb = *power_tier_ceiling()` — §15.3,
//       ungated by backend
//   `h.tx_power_json` / power_tier_json — §15.5 GET /api/v1/tx/power_tier
//       emits presets_qdb and ceiling_qdb, ungated
//   `UplinkPower::hw_qdb()` — the ground clamps a §10.5 override latch by
//       ceiling_qdb, and that REACHES THE ACTUATOR
// plus the load-time clamp of power_presets_qdb to max_power_qdb in this file's
// sibling, io/src/config.cpp. NOT the §10.7 sweep bound: Pass 165 made the
// tier fold unreachable on a relative uplink, and both calibrators take the
// offset-space window first — that reader is gone, and citing it would send a
// future retire-the-key pass down a dead path.
// Both keys are set on both flying configs, so the false predicate told the
// operator to delete a key that clamps TX power. §15.2 says it plainly: the
// §10.5 reference role went with kernel-monitor, the §10.3 ceiling role did
// not. Caught by pre-merge review, not by the seven mutation tests — those
// proved only that the tests agreed with the wrong model.
//
// Pass 166 predicated power_presets_qdb after all — but on `air.kind` alone,
// which the three readers above now follow rather than contradict: each one
// reads the space-selected list, and on a relative node that list is
// power_offset_presets_qdb. max_power_qdb is a DIFFERENT claim. On a relative
// node its one remaining reader is the clamp of a list that is itself inert
// there, which looks like inertness and may be — but that is the shape of the
// error above, so it stays unpredicated until a pass that re-runs the
// enumeration and can afford to be wrong about it.

const char* kWhyNoUplink =
    "this node has no role:\"tx\" adapter on an RF backend, so \u00a715.2 gives "
    "it no uplink and return/ARQ never run";
const char* kWhyNoModeCmd =
    "venc.mode_apply_cmd is unset, so there is no operating-mode catalog";
const char* kWhyNotUdp =
    "air.kind is not udp/udp-broadcast, and only the udp backend reads it";
const char* kWhyVencOff = "venc.enabled is false";
const char* kWhyRepairOff = "cache.repair.enabled is false";
const char* kWhyStoreOff = "cache.store.enabled is false";
const char* kWhyIfnameGone =
    "the kernel-monitor backend that bound to this netdev was deleted in "
    "Pass 164, and nothing else reads it";
const char* kWhyAbsolutePresets =
    "air.kind is radio (a relative backend, \u00a710.5), whose \u00a711.7 "
    "0x0A tier reads adapters[].power_offset_presets_qdb instead";
const char* kWhyOffsetPresets =
    "air.kind is not radio, so this node's TX actuation is absolute and its "
    "\u00a711.7 0x0A tier reads adapters[].power_presets_qdb instead";
const char* kWhyCalibIdGone =
    "\u00a710.6 identity tiers 2-4 went with kernel-monitor (Pass 164); the "
    "radio backend keys the calibration artifact on the EFUSE MAC and has no "
    "fallback tier";


const KeyEntry kKeys[] = {
    {"adapters",                                  KeyType::kArray},
    {"adapters[].bus",                            KeyType::kString},
    {"adapters[].bw",                             KeyType::kNumber},
    {"adapters[].calib_id",                       KeyType::kString, never_live, kWhyCalibIdGone},
    {"adapters[].channel",                        KeyType::kNumber},
    {"adapters[].ifname",                         KeyType::kString, never_live, kWhyIfnameGone},
    {"adapters[].mac",                            KeyType::kString},
    {"adapters[].max_power_qdb",                  KeyType::kNumber},
    {"adapters[].name",                           KeyType::kString},
    {"adapters[].power_map",                      KeyType::kString},
    {"adapters[].power_offset_max_qdb",           KeyType::kNumber},
    {"adapters[].power_offset_presets_qdb",       KeyType::kArray, backend_is_relative, kWhyOffsetPresets},
    {"adapters[].power_offset_qdb",               KeyType::kNumber},
    {"adapters[].power_presets_qdb",              KeyType::kArray, backend_is_absolute, kWhyAbsolutePresets},
    {"adapters[].role",                           KeyType::kString},
    {"air",                                       KeyType::kObject},
    {"air.ack_responder",                         KeyType::kBool},
    {"air.airtime_efficiency_permille",           KeyType::kNumber},
    {"air.disable_cca",                           KeyType::kBool},
    {"air.kind",                                  KeyType::kString},
    {"air.ldpc",                                  KeyType::kBool},
    {"air.mcs_probe",                             KeyType::kBool},
    {"air.pace_mbps",                             KeyType::kNumber, air_is_udp, kWhyNotUdp},
    {"air.rx",                                    KeyType::kArray, air_is_udp, kWhyNotUdp},
    {"air.rx_drop_permille",                      KeyType::kNumber},
    {"air.stbc",                                  KeyType::kBool},
    {"air.tx",                                    KeyType::kArray, air_is_udp, kWhyNotUdp},
    {"air.tx_retry_limit",                        KeyType::kNumber},
    {"air.uplink_rate",                           KeyType::kObject},
    {"air.uplink_rate.bw",                        KeyType::kNumber},
    {"air.uplink_rate.mcs",                       KeyType::kNumber},
    {"air.uplink_rate.sgi",                       KeyType::kBool},
    {"air.wedge_exit_windows",                    KeyType::kNumber},
    {"air.wedge_min_submits",                     KeyType::kNumber},
    {"air.wedge_window_ms",                       KeyType::kNumber},
    {"cache",                                     KeyType::kObject},
    {"cache.repair",                              KeyType::kObject},
    {"cache.repair.absolute_symbol_limit",        KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.assignment_interval_ms",       KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.caches",                       KeyType::kArray, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.caches[].endpoint",            KeyType::kString, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.caches[].originator",          KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.enabled",                      KeyType::kBool},
    {"cache.repair.hard_close_ms",                KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.health_floor_permille",        KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.listen",                       KeyType::kString, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.local_quiet_ms",               KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.max_cache_attempts",           KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.min_collect_ms",               KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.nack_grace_ms",                KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.repair_fraction_permille",     KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.reply_limit",                  KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.request_timeout_ms",           KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.status_timeout_ms",            KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.stream_id",                    KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.repair.tail_grace_ms",                KeyType::kNumber, cache_repair_enabled, kWhyRepairOff},
    {"cache.store",                               KeyType::kObject},
    {"cache.store.blocks",                        KeyType::kNumber, cache_store_enabled, kWhyStoreOff},
    {"cache.store.controller",                    KeyType::kObject, cache_store_enabled, kWhyStoreOff},
    {"cache.store.controller.endpoint",           KeyType::kString, cache_store_enabled, kWhyStoreOff},
    {"cache.store.controller.originator",         KeyType::kNumber, cache_store_enabled, kWhyStoreOff},
    {"cache.store.enabled",                       KeyType::kBool},
    {"cache.store.listen",                        KeyType::kString, cache_store_enabled, kWhyStoreOff},
    {"cache.store.max_requests_per_s",            KeyType::kNumber, cache_store_enabled, kWhyStoreOff},
    {"cache.store.reply_limit",                   KeyType::kNumber, cache_store_enabled, kWhyStoreOff},
    {"cache.store.status_interval_ms",            KeyType::kNumber, cache_store_enabled, kWhyStoreOff},
    {"cache.store.status_to",                     KeyType::kArray, cache_store_enabled, kWhyStoreOff},
    {"cache.store.stream_ids",                    KeyType::kArray, cache_store_enabled, kWhyStoreOff},
    {"control",                                   KeyType::kObject},
    {"control.bind",                              KeyType::kString},
    {"loopback",                                  KeyType::kObject},
    {"loopback.adapters",                         KeyType::kNumber},
    {"loopback.correlation",                      KeyType::kNumber},
    {"loopback.loss",                             KeyType::kObject},
    {"loopback.loss.ge",                          KeyType::kObject},
    {"loopback.loss.ge.loss_b",                   KeyType::kNumber},
    {"loopback.loss.ge.loss_g",                   KeyType::kNumber},
    {"loopback.loss.ge.p_bg",                     KeyType::kNumber},
    {"loopback.loss.ge.p_gb",                     KeyType::kNumber},
    {"loopback.loss.uniform_p",                   KeyType::kNumber},
    {"loopback.return_loss_p",                    KeyType::kNumber},
    {"loopback.rssi_dbm",                         KeyType::kNumber},
    {"loopback.rssi_fade",                        KeyType::kObject},
    {"loopback.rssi_fade.dbm",                    KeyType::kNumber},
    {"loopback.rssi_fade.end_ms",                 KeyType::kNumber},
    {"loopback.rssi_fade.start_ms",               KeyType::kNumber},
    {"loopback.seed",                             KeyType::kNumber},
    {"node",                                      KeyType::kObject},
    {"node.net_id",                               KeyType::kNumber},
    {"node.originator",                           KeyType::kNumber},
    {"node.preferred_originator",                 KeyType::kNumber},
    {"node.recovery_on_latch",                    KeyType::kBool},
    {"node.role",                                 KeyType::kString},
    {"node.spectator",                            KeyType::kBool},
    {"policy",                                    KeyType::kObject},
    {"policy.arq",                                KeyType::kObject},
    {"policy.arq.airtime_frac",                   KeyType::kNumber},
    {"policy.arq.arq_max_fps",                    KeyType::kNumber},
    {"policy.arq.attempt_cap",                    KeyType::kNumber},
    {"policy.arq.budget_floor_bytes",             KeyType::kNumber},
    {"policy.arq.budget_interval_ms",             KeyType::kNumber},
    {"policy.arq.classifier_size_threshold",      KeyType::kNumber},
    {"policy.arq.fwd_clamp_blocks",               KeyType::kNumber},
    {"policy.arq.holddown_ms",                    KeyType::kNumber},
    {"policy.arq.max_block_pkts",                 KeyType::kNumber},
    {"policy.arq.min_recoverable_ms",             KeyType::kNumber},
    {"policy.arq.release_timeout_ms",             KeyType::kNumber},
    {"policy.arq.ring_byte_budget",               KeyType::kNumber},
    {"policy.arq.ring_window_ms",                 KeyType::kNumber},
    {"policy.calibration",                        KeyType::kObject},
    {"policy.calibration.artifact_dir",           KeyType::kString},
    {"policy.calibration.dwell_probe_frames",     KeyType::kNumber},
    {"policy.calibration.dwell_verify_frames",    KeyType::kNumber},
    {"policy.calibration.feed_quiet_ms",          KeyType::kNumber},
    {"policy.calibration.hard_cap_ms",            KeyType::kNumber},
    {"policy.calibration.loss_bad_milli",         KeyType::kNumber},
    {"policy.calibration.loss_ok_milli",          KeyType::kNumber},
    {"policy.calibration.max_qdb",                KeyType::kNumber},
    {"policy.calibration.min_qdb",                KeyType::kNumber},
    {"policy.calibration.offset_seek_step_qdb",   KeyType::kNumber},
    {"policy.calibration.probe_pace_us",          KeyType::kNumber},
    {"policy.calibration.rssi_guard_dbm",         KeyType::kNumber},
    {"policy.calibration.seek_step_qdb",          KeyType::kNumber},
    {"policy.calibration.settle_ms",              KeyType::kNumber},
    {"policy.calibration.tally_retries",          KeyType::kNumber},
    {"policy.calibration.tally_wait_ms",          KeyType::kNumber},
    {"policy.cmd",                                KeyType::kObject},
    {"policy.cmd.ack_timeout_ms",                 KeyType::kNumber},
    {"policy.cmd.copies",                         KeyType::kNumber},
    {"policy.cmd.copy_interval_ms",               KeyType::kNumber},
    {"policy.cmd.echo_copies",                    KeyType::kNumber},
    {"policy.cmd.min_interval_ms",                KeyType::kNumber},
    {"policy.cmd.retry_cap",                      KeyType::kNumber},
    {"policy.csa",                                KeyType::kObject},
    {"policy.csa.ack_timeout_ms",                 KeyType::kNumber},
    {"policy.csa.adjacent_guard_mhz",             KeyType::kNumber},
    {"policy.csa.bind_release_s",                 KeyType::kNumber},
    {"policy.csa.channel_allowlist",              KeyType::kArray},
    {"policy.csa.home_chan",                      KeyType::kNumber},
    {"policy.csa.min_interval_s",                 KeyType::kNumber},
    {"policy.csa.persist_channel",                KeyType::kBool},
    {"policy.csa.psk",                            KeyType::kString},
    {"policy.csa.rx_liveness_ms",                 KeyType::kNumber},
    {"policy.csa.settle_s",                       KeyType::kNumber},
    {"policy.csa.verify_timeout_ms",              KeyType::kNumber},
    {"policy.fec",                                KeyType::kObject},
    {"policy.fec.overhead_frac",                  KeyType::kNumber},
    {"policy.fec.scheme",                         KeyType::kString},
    {"policy.report_hz",                          KeyType::kNumber},
    {"policy.report_timeout_ms",                  KeyType::kNumber},
    {"policy.return",                             KeyType::kObject, has_uplink, kWhyNoUplink},
    {"policy.return.guard_us",                    KeyType::kNumber, has_uplink, kWhyNoUplink},
    {"policy.return.quiet_gap",                   KeyType::kBool, has_uplink, kWhyNoUplink},
    {"policy.return.report_redundancy",           KeyType::kNumber, has_uplink, kWhyNoUplink},
    {"policy.return.return_window_us",            KeyType::kNumber, has_uplink, kWhyNoUplink},
    {"policy.return.unicast",                     KeyType::kBool, has_uplink, kWhyNoUplink},
    {"policy.rx",                                 KeyType::kObject},
    {"policy.rx.admit_n",                         KeyType::kNumber},
    {"policy.rx.admit_window_ms",                 KeyType::kNumber},
    {"policy.rx.clamp_resync_ms",                 KeyType::kNumber},
    {"policy.rx.dwell_ceiling_ms",                KeyType::kNumber},
    {"policy.rx.fwd_clamp_pkts",                  KeyType::kNumber},
    {"policy.rx.idle_teardown_ms",                KeyType::kNumber},
    {"policy.rx.renack_attempts",                 KeyType::kNumber},
    {"policy.rx.renack_backoff_ms",               KeyType::kNumber},
    {"policy.rx.stall_timeout_ms",                KeyType::kNumber},
    {"policy.select",                             KeyType::kObject},
    {"policy.select.bitrate_lead_s",              KeyType::kNumber},
    {"policy.select.demote_milli",                KeyType::kNumber},
    {"policy.select.down_cooldown_s",             KeyType::kNumber},
    {"policy.select.emergency_loss_milli",        KeyType::kNumber},
    {"policy.select.ewma_alpha",                  KeyType::kNumber},
    {"policy.select.failsafe_hold_s",             KeyType::kNumber},
    {"policy.select.failsafe_step_s",             KeyType::kNumber},
    {"policy.select.flap_freeze_count",           KeyType::kNumber},
    {"policy.select.flap_freeze_s",               KeyType::kNumber},
    {"policy.select.flap_freeze_window_s",        KeyType::kNumber},
    {"policy.select.loss_min_uniq",               KeyType::kNumber},
    {"policy.select.loss_persist_score",          KeyType::kNumber},
    {"policy.select.max_profile",                 KeyType::kNumber},
    {"policy.select.mcs_settle_s",                KeyType::kNumber},
    {"policy.select.mcs_up_grace_s",              KeyType::kNumber},
    {"policy.select.min_profile",                 KeyType::kNumber},
    {"policy.select.pressure_escape_s",           KeyType::kNumber},
    {"policy.select.probe_veto_permille",         KeyType::kNumber},
    {"policy.select.probe_veto_ttl_s",            KeyType::kNumber},
    {"policy.select.promote_dwell_s",             KeyType::kNumber},
    {"policy.select.promote_rssi_hyst_db",        KeyType::kNumber},
    {"policy.select.reentry_backoff_s",           KeyType::kNumber},
    {"policy.select.reentry_dwell_s",             KeyType::kNumber},
    {"policy.select.rssi_fade_arm_dbm",           KeyType::kNumber},
    {"policy.select.rssi_fade_db_per_s",          KeyType::kNumber},
    {"policy.select.rssi_floor_dbm",              KeyType::kNumber},
    {"policy.select.rung_lockout_latch_count",    KeyType::kNumber},
    {"policy.select.rung_lockout_s",              KeyType::kNumber},
    {"policy.select.rung_rssi_floor_dbm",         KeyType::kArray},
    {"policy.select.verdict_ttl_s",               KeyType::kNumber},
    {"policy.uplink",                             KeyType::kObject, has_uplink, kWhyNoUplink},
    {"policy.uplink.fallback_ms",                 KeyType::kNumber, has_uplink, kWhyNoUplink},
    {"policy.uplink.pps_budget",                  KeyType::kNumber, has_uplink, kWhyNoUplink},
    {"policy.uplink.telemetry_hold",              KeyType::kNumber, has_uplink, kWhyNoUplink},
    {"profile_table",                             KeyType::kString},
    {"scout",                                     KeyType::kObject},
    {"scout.channels",                            KeyType::kArray},
    {"scout.dwell_ms",                            KeyType::kNumber},
    {"stats",                                     KeyType::kObject},
    {"stats.bind",                                KeyType::kObject},
    {"stats.bind.kind",                           KeyType::kString},
    {"stats.bind.listen",                         KeyType::kString},
    {"stats.bind.name",                           KeyType::kString},
    {"stats.bind.send",                           KeyType::kString},
    {"stats.hz",                                  KeyType::kNumber},
    {"stats.stdout",                              KeyType::kBool},
    {"streams",                                   KeyType::kArray},
    {"streams[].arq_mode",                        KeyType::kString},
    {"streams[].bind",                            KeyType::kObject},
    {"streams[].bind.kind",                       KeyType::kString},
    {"streams[].bind.listen",                     KeyType::kString},
    {"streams[].bind.name",                       KeyType::kString},
    {"streams[].bind.send",                       KeyType::kString},
    {"streams[].classifier",                      KeyType::kString},
    {"streams[].dir",                             KeyType::kString},
    {"streams[].fec",                             KeyType::kObject},
    {"streams[].fec.e_rate_permille",             KeyType::kNumber},
    {"streams[].fec.i_rate_permille",             KeyType::kNumber},
    {"streams[].fec.min_k",                       KeyType::kNumber},
    {"streams[].fec.min_r",                       KeyType::kNumber},
    {"streams[].fec.p_rate_permille",             KeyType::kNumber},
    {"streams[].fec.scheme",                      KeyType::kString},
    {"streams[].jscc_shadow",                     KeyType::kObject},
    {"streams[].jscc_shadow.arq_guard_us",        KeyType::kNumber},
    {"streams[].jscc_shadow.enforce",             KeyType::kBool},
    {"streams[].jscc_shadow.fec_cap_permille",    KeyType::kNumber},
    {"streams[].jscc_shadow.fec_floor_permille",  KeyType::kNumber},
    {"streams[].jscc_shadow.feedback_timeout_ms", KeyType::kNumber},
    {"streams[].jscc_shadow.min_rtt_samples",     KeyType::kNumber},
    {"streams[].originator",                      KeyType::kNumber},
    {"streams[].stream_id",                       KeyType::kNumber},
    {"streams[].stream_type",                     KeyType::kStringOrNumber},
    {"venc",                                      KeyType::kObject},
    {"venc.active_mode",                          KeyType::kString, mode_configured, kWhyNoModeCmd},
    {"venc.cap_ceiling_bytes",                    KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.command_presets",                      KeyType::kObject, venc_enabled, kWhyVencOff},
    {"venc.command_presets.fps",                  KeyType::kArray, venc_enabled, kWhyVencOff},
    {"venc.command_presets.framing",              KeyType::kArray, venc_enabled, kWhyVencOff},
    {"venc.command_presets.resolution",           KeyType::kArray, venc_enabled, kWhyVencOff},
    {"venc.enabled",                              KeyType::kBool},
    {"venc.fps_hint",                             KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder",                           KeyType::kObject, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.enabled",                   KeyType::kBool, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.max",                       KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.min",                       KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.min_p_frame_bytes",         KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.preferred",                 KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.reduce_after_ms",           KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.reduce_dwell_ms",           KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.restore_after_ms",          KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.restore_hysteresis_bytes",  KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.sample_timeout_ms",         KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.fps_ladder.settle_ms",                 KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.frame_caps",                           KeyType::kBool, venc_enabled, kWhyVencOff},
    {"venc.host",                                 KeyType::kString, venc_enabled, kWhyVencOff},
    {"venc.i_headroom_permille",                  KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.max_bitrate_kbps",                     KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.mode_apply_cmd",                       KeyType::kString},
    {"venc.modes_dir",                            KeyType::kString, mode_configured, kWhyNoModeCmd},
    {"venc.p_headroom_permille",                  KeyType::kNumber, venc_enabled, kWhyVencOff},
    {"venc.recovery_enabled",                     KeyType::kBool, venc_enabled, kWhyVencOff},
    {"venc.settle_ms",                            KeyType::kNumber, venc_enabled, kWhyVencOff},
};

const char* type_name(KeyType t) {
    switch (t) {
        case KeyType::kObject:         return "object";
        case KeyType::kArray:          return "array";
        case KeyType::kString:         return "string";
        case KeyType::kNumber:         return "number";
        case KeyType::kBool:           return "bool";
        case KeyType::kStringOrNumber: return "string|number";
    }
    return "?";
}

}  // namespace

const KeyEntry* config_registry(std::size_t* count) {
    if (count != nullptr) {
        *count = sizeof(kKeys) / sizeof(kKeys[0]);
    }
    return kKeys;
}

std::string config_schema_json() {
    std::size_t n = 0;
    const KeyEntry* keys = config_registry(&n);
    // Hand-rolled rather than nlohmann: the output is a golden file, so the
    // exact byte layout is the contract and is easier to pin here than to
    // depend on a serialiser's formatting staying put across a vendor bump.
    std::string out = "{\n  \"version\": 1,\n  \"keys\": [\n";
    for (std::size_t i = 0; i < n; ++i) {
        out += "    {\"path\": \"";
        out += keys[i].path;
        out += "\", \"type\": \"";
        out += type_name(keys[i].type);
        out += "\"}";
        if (i + 1 < n) out += ",";
        out += "\n";
    }
    out += "  ]\n}\n";
    return out;
}


namespace {

// Every path present in the config, dotted, with "[]" for array elements —
// the same spelling the registry uses. Containers are emitted as well as
// leaves, because the registry declares them too.
// The deepest registry path is 4 segments (cache.store.controller.endpoint).
// Anything past this cannot match an entry, so descending further only builds
// longer strings — and the recursion is over attacker/typo-controlled nesting,
// which at ~20k deep overflows the stack on a target with a small one. --check
// itself has no such surface, so --strict must not introduce one.
constexpr int kMaxDepth = 8;

void collect_paths(const json& j, const std::string& prefix, int depth,
                   std::vector<std::string>& out) {
    if (depth > kMaxDepth) return;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string p =
                prefix.empty() ? it.key() : prefix + "." + it.key();
            out.push_back(p);
            collect_paths(it.value(), p, depth + 1, out);
        }
    } else if (j.is_array()) {
        // Scalar arrays have no paths of their own; the array key itself was
        // already emitted by the object branch above.
        for (const json& e : j) {
            if (e.is_object() || e.is_array()) {
                collect_paths(e, prefix + "[]", depth + 1, out);
            }
        }
    }
}

// `_`-prefixed keys are comments by convention (_comment,
// policy.csa._verify_timeout_ms, streams[]._comment) and are the only keys
// allowed to be unknown.
bool is_comment_path(const std::string& p) {
    std::size_t start = 0;
    for (;;) {
        if (start < p.size() && p[start] == '_') return true;
        const std::size_t dot = p.find('.', start);
        if (dot == std::string::npos) return false;
        start = dot + 1;
    }
}

const char* verdict_name(KeyVerdict v) {
    return v == KeyVerdict::kUnknown ? "unknown" : "inert";
}

}  // namespace

std::vector<KeyFinding> check_config_keys(const std::string& config_json,
                                          const Config& cfg) {
    std::vector<KeyFinding> out;
    json j;
    try {
        j = json::parse(config_json);
    } catch (const json::exception&) {
        // The caller has already loaded this text successfully; a parse
        // failure here would mean a different string was passed. Report
        // nothing rather than inventing findings.
        return out;
    }

    std::vector<std::string> paths;
    collect_paths(j, "", 0, paths);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    std::size_t n = 0;
    const KeyEntry* keys = config_registry(&n);
    for (const std::string& p : paths) {
        if (is_comment_path(p)) continue;
        const KeyEntry* hit = nullptr;
        for (std::size_t i = 0; i < n; ++i) {
            if (p == keys[i].path) {
                hit = &keys[i];
                break;
            }
        }
        if (hit == nullptr) {
            out.push_back({p, KeyVerdict::kUnknown, ""});
        } else if (hit->live != nullptr && !hit->live(cfg)) {
            out.push_back({p, KeyVerdict::kInert, hit->inert_reason});
        }
    }
    return out;
}

std::string check_report_json(const std::vector<KeyFinding>& findings) {
    // nlohmann here, unlike config_schema_json(): the reasons are prose and
    // contain quotes, so escaping is a real requirement rather than an
    // assumption the path alphabet makes safe.
    json out;
    out["ok"] = findings.empty();
    out["findings"] = json::array();
    for (const KeyFinding& f : findings) {
        json e;
        e["path"] = f.path;
        e["verdict"] = verdict_name(f.verdict);
        e["reason"] = f.reason == nullptr ? "" : f.reason;
        out["findings"].push_back(std::move(e));
    }
    return out.dump(2) + "\n";
}

}  // namespace wblink
